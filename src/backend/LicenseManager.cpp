#include "backend/LicenseManager.h"
#include "backend/LicensePublicKey.h"

#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

#include <windows.h>
#include <bcrypt.h>

static constexpr char kExpectedProduct[] = "TiepoloPro";

namespace {

// payloadBytesに対するsignatureBytes(ECDSA P-256 raw r||s, 64バイト)の署名を、
// LicensePublicKey.h に埋め込まれた公開鍵で検証する。追加ライブラリ不要のWindows
// 標準API(CNG/bcrypt.dll)のみを使う。
bool verifySignature(const QByteArray &payloadBytes, const QByteArray &signatureBytes)
{
    if (signatureBytes.size() != 64) return false; // P-256のr||sは32+32バイト固定

    bool ok = false;
    BCRYPT_ALG_HANDLE hSignAlg = nullptr;
    BCRYPT_ALG_HANDLE hHashAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey     = nullptr;
    BCRYPT_HASH_HANDLE hHash   = nullptr;

    // 公開鍵ブロブ(BCRYPT_ECCPUBLIC_BLOB構造体 + X + Y)を実行時に組み立てる。
    // マジックナンバー等はハードコードせずSDKのbcrypt.hが定義する構造体/定数を
    // そのまま使うことで、手打ちのバイト列で構造を誤る事故を避ける。
    BCRYPT_ECCKEY_BLOB header;
    header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header.cbKey   = sizeof(kTiepoloLicensePublicKeyX);
    QByteArray keyBlob;
    keyBlob.append(reinterpret_cast<const char*>(&header), sizeof(header));
    keyBlob.append(reinterpret_cast<const char*>(kTiepoloLicensePublicKeyX), sizeof(kTiepoloLicensePublicKeyX));
    keyBlob.append(reinterpret_cast<const char*>(kTiepoloLicensePublicKeyY), sizeof(kTiepoloLicensePublicKeyY));

    do {
        if (BCryptOpenAlgorithmProvider(&hSignAlg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) break;
        if (BCryptImportKeyPair(hSignAlg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &hKey,
                                 (PUCHAR)keyBlob.data(), keyBlob.size(), 0) != 0) break;

        if (BCryptOpenAlgorithmProvider(&hHashAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) break;

        DWORD hashObjLen = 0, cbResult = 0;
        if (BCryptGetProperty(hHashAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&hashObjLen,
                               sizeof(hashObjLen), &cbResult, 0) != 0) break;
        QByteArray hashObj(hashObjLen, 0);

        DWORD hashLen = 0;
        if (BCryptGetProperty(hHashAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen,
                               sizeof(hashLen), &cbResult, 0) != 0) break;
        QByteArray hash(hashLen, 0);

        if (BCryptCreateHash(hHashAlg, &hHash, (PUCHAR)hashObj.data(), hashObj.size(),
                              nullptr, 0, 0) != 0) break;
        if (BCryptHashData(hHash, (PUCHAR)payloadBytes.constData(), payloadBytes.size(), 0) != 0) break;
        if (BCryptFinishHash(hHash, (PUCHAR)hash.data(), hash.size(), 0) != 0) break;

        NTSTATUS status = BCryptVerifySignature(hKey, nullptr,
                                                 (PUCHAR)hash.data(), hash.size(),
                                                 (PUCHAR)signatureBytes.data(), signatureBytes.size(), 0);
        ok = (status == 0); // STATUS_SUCCESS
    } while (false);

    if (hHash)     BCryptDestroyHash(hHash);
    if (hKey)      BCryptDestroyKey(hKey);
    if (hHashAlg)  BCryptCloseAlgorithmProvider(hHashAlg, 0);
    if (hSignAlg)  BCryptCloseAlgorithmProvider(hSignAlg, 0);
    return ok;
}

} // namespace

QString LicenseManager::defaultLicensePath()
{
    // Claude確認用
    /*
    const QString override_ = qEnvironmentVariable("TIEPOLO_LICENSE_PATH");
    if (!override_.isEmpty()) return override_;
    */

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + "/license.tiepololicense";
}

LicenseManager &LicenseManager::instance()
{
    static LicenseManager mgr;
    return mgr;
}

void LicenseManager::loadDefault()
{
    const QString path = defaultLicensePath();
    if (!QFile::exists(path)) { unlocked_ = false; return; }
    loadFromFile(path, nullptr);
}

bool LicenseManager::loadFromFile(const QString &path, QString *errorOut)
{
    unlocked_ = false;
    info_ = LicenseInfo{};

    auto fail = [&](const QString &msg) {
        if (errorOut) *errorOut = msg;
        return false;
    };

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail("ライセンスファイルを開けませんでした: " + path);

    const QByteArray outerJson = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument outerDoc = QJsonDocument::fromJson(outerJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !outerDoc.isObject())
        return fail("ライセンスファイルの形式が不正です");

    const QJsonObject outerObj = outerDoc.object();
    const QByteArray payloadBytes   = QByteArray::fromBase64(outerObj["payload"].toString().toLatin1());
    const QByteArray signatureBytes = QByteArray::fromBase64(outerObj["signature"].toString().toLatin1());
    if (payloadBytes.isEmpty() || signatureBytes.isEmpty())
        return fail("ライセンスファイルの形式が不正です");

    // 署名はbase64decode前ではなく、payloadフィールドをbase64decodeした「生JSON文字列の
    // バイト列」に対して行われている(JSON再シリアライズによる表現差異を避けるため)。
    if (!verifySignature(payloadBytes, signatureBytes))
        return fail("ライセンスの署名検証に失敗しました(改ざん、または不正なライセンスファイルです)");

    QJsonParseError payloadParseError;
    const QJsonDocument payloadDoc = QJsonDocument::fromJson(payloadBytes, &payloadParseError);
    if (payloadParseError.error != QJsonParseError::NoError || !payloadDoc.isObject())
        return fail("ライセンス内容の形式が不正です");

    const QJsonObject payloadObj = payloadDoc.object();
    LicenseInfo li;
    li.product   = payloadObj["product"].toString();
    li.licensee  = payloadObj["licensee"].toString();
    li.issuedAt  = QDate::fromString(payloadObj["issuedAt"].toString(), Qt::ISODate);
    if (payloadObj["expiresAt"].isString())
        li.expiresAt = QDate::fromString(payloadObj["expiresAt"].toString(), Qt::ISODate);
    for (const QJsonValue &v : payloadObj["features"].toArray())
        li.features.append(v.toString());

    if (li.product != QLatin1String(kExpectedProduct))
        return fail("このライセンスファイルは別の製品向けです");

    if (li.expiresAt.isValid() && li.expiresAt < QDate::currentDate())
        return fail("ライセンスの有効期限が切れています(期限: " + li.expiresAt.toString(Qt::ISODate) + ")");

    info_ = li;
    unlocked_ = true;
    return true;
}
