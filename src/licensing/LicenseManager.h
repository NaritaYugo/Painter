#pragma once

#include <QString>
#include <QDate>

// ---------------------------------------------------------------------------
// LicenseManager
// ---------------------------------------------------------------------------
// 有料版(Pro)ビルドでのみコンパイルされる。ライセンスファイルの署名検証を行い、
// Pro機能を実際に有効化してよいかどうかを判定する。
//
// 署名方式: ECDSA P-256 + SHA-256 (Windows CNG/BCryptを使用。追加の外部ライブラリは
// 不要)。秘密鍵はこちら(開発者)だけが持ち、tools/license/ 以下のPowerShellスクリプトで
// 鍵ペア生成・ライセンスファイル発行を行う。アプリ本体には公開鍵だけを埋め込む
// (LicensePublicKey.h、鍵ペア生成後に差し替える)。
//
// ライセンスファイルの形式(JSON):
//   { "payload": "<base64>", "signature": "<base64>" }
// payloadはさらにbase64decode+UTF-8デコードすると以下のJSONになる:
//   { "product": "TiepoloPro", "licensee": "...", "issuedAt": "YYYY-MM-DD",
//     "expiresAt": "YYYY-MM-DD" または null, "features": ["pro"] }
// 署名はpayloadフィールドの中身(base64decode前の生JSON文字列のUTF-8バイト列)に対する
// ものであり、JSON再シリアライズを介さないため、署名側(PowerShell/.NET)と検証側
// (Qt/C++)でJSON表現の差異(キー順序・空白等)による不一致が起こらない。
// ---------------------------------------------------------------------------
struct LicenseInfo {
    QString product;
    QString licensee;
    QDate   issuedAt;
    QDate   expiresAt;      // 無効(null)なら無期限
    QStringList features;
};

class LicenseManager
{
public:
    static LicenseManager &instance();

    // アプリ起動時に一度呼ぶ。既定のライセンスファイルパス
    // (%APPDATA%/pwxwx/Tiepolo/license.tiepololicense)を探して検証する。
    // 見つからない/検証失敗でもcrashはせず、isUnlocked()がfalseのままになるだけ。
    void loadDefault();

    // 指定パスのライセンスファイルを読み込んで検証する(ファイル選択ダイアログ経由の
    // 手動インポート等に使う)。戻り値: 検証成功かどうか。
    bool loadFromFile(const QString &path, QString *errorOut = nullptr);

    bool isUnlocked() const { return unlocked_; }
    const LicenseInfo &info() const { return info_; }

    static QString defaultLicensePath();

private:
    LicenseManager() = default;

    bool unlocked_ = false;
    LicenseInfo info_;
};
