#include "io/AbrPenMapping.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QStandardPaths>
#include <QtMath>

namespace {

const char *kBuiltinCircle = ":/textures/penTip/circle.png";
const int   kMaxTipPx      = 1024;   // 取り込む先端画像の一辺の上限

// ---------------------------------------------------------------------------
// 先端画像を「円形の型」に収まる正方形へ貼り直す
// ---------------------------------------------------------------------------
// このアプリのスタンプは中心からの距離でフォールオフを掛けるので(stroke.comp の
// sampleBrushAlpha)、先端画像のうち中心から radius より外は必ず消える。つまり
// 使われるのは正方形に内接する円の内側だけ。
// Photoshopの先端画像は縦横比が自由で四隅まで絵があるものも多いため、
//   ・中心から最も遠い「不透明な画素」までの距離を測り
//   ・その距離を半径とする円がちょうど内接する正方形へ、中央寄せで貼り直す
// ことで、絵を欠けさせずに縦横比も保つ。余白は必要な分しか作らないので、
// Photoshop側の大きさ(長辺=サイズ)ともほぼ一致する。
QImage fitToStampSquare(const QImage &srcAny)
{
    const QImage src = srcAny.convertToFormat(QImage::Format_RGBA8888);
    if (src.isNull()) return {};

    const double cx = src.width() * 0.5, cy = src.height() * 0.5;
    double r2max = 0.0;
    for (int y = 0; y < src.height(); y++) {
        const uchar *line = src.constScanLine(y);
        for (int x = 0; x < src.width(); x++) {
            if (line[x*4+3] == 0) continue;
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            r2max = qMax(r2max, dx*dx + dy*dy);
        }
    }
    if (r2max <= 0.0) return {};                      // 全部透明 = 先端として使えない

    int side = (int)qCeil(2.0 * qSqrt(r2max));
    side = qMax(side, qMax(src.width(), src.height()) / 4);   // 極端に小さくしない
    side = qMin(side, kMaxTipPx * 2);

    QImage out(side, side, QImage::Format_RGBA8888);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.drawImage(QPoint((side - src.width()) / 2, (side - src.height()) / 2), src);
    p.end();

    if (side > kMaxTipPx)
        out = out.scaled(kMaxTipPx, kMaxTipPx, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return out;
}

// 取り込んだ先端画像をアプリのデータフォルダへPNGで保存し、そのパスを返す。
// ツールプリセットにはパスだけが保存されるので、実体はアプリ側で持ち続ける必要がある。
QString saveImportedTip(const QImage &tip, const QString &brushName)
{
    const QImage img = fitToStampSquare(tip);
    if (img.isNull()) return {};

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                      + QStringLiteral("/brushTips");
    if (!QDir().mkpath(dir)) return {};

    // 名前が同じ別のブラシで上書きしないよう、中身のハッシュをファイル名に混ぜる。
    // 同じブラシを何度取り込んでも同じファイルになる。
    const QByteArray raw((const char *)img.constBits(), (int)img.sizeInBytes());
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(raw, QCryptographicHash::Md5).toHex().left(8));

    QString base;
    for (QChar ch : brushName) {
        if (ch.isLetterOrNumber() || ch == '-' || ch == '_') base += ch;
        else if (base.isEmpty() || base.back() != '_')       base += '_';
    }
    base = base.left(40);
    if (base.isEmpty()) base = QStringLiteral("tip");

    const QString path = dir + "/" + base + "_" + hash + ".png";
    if (QFileInfo::exists(path)) return path;
    return img.save(path, "PNG") ? path : QString();
}

} // namespace

namespace AbrPenMapping {

QStringList applyToPen(const AbrBrush &b, PenToolConfig &pen)
{
    QStringList notes;

    if (b.diameterPx.has) pen.setSize(qBound(1, (int)qRound(b.diameterPx.v), 1000));
    if (b.hardness.has)   pen.setHardness((float)b.hardness.v);
    // スタンプ間隔はこのアプリでは5%〜300%。Photoshopは1%〜1000%なので丸める。
    if (b.spacing.has) {
        const double s = b.spacing.v;
        if (s > 3.0)       notes << QStringLiteral("スタンプ間隔 %1% は上限の300%へ丸めました")
                                    .arg(qRound(s * 100));
        else if (s < 0.05) notes << QStringLiteral("スタンプ間隔 %1% は下限の5%へ丸めました")
                                    .arg(qRound(s * 100));
        pen.setSpacing((float)qBound(0.05, s, 3.0));
    }
    if (b.angleDeg.has)   pen.setAngleDeg((int)qRound(b.angleDeg.v));
    if (b.roundness.has)  pen.setRoundness((float)qMax(0.05, b.roundness.v));

    if (b.minDiameter.has)   pen.setMinSizeRatio((float)b.minDiameter.v);
    if (b.sizeJitter.has)    pen.setSizeJitter((float)b.sizeJitter.v);
    if (b.angleJitter.has)   pen.setAngleJitter((float)b.angleJitter.v);
    if (b.opacityJitter.has) {
        pen.setOpacityJitter((float)b.opacityJitter.v);
        if (b.opacityJitter.v > 0.0) pen.setPressureOpacity(true);
    }
    if (b.scatter.has) {
        const double s = b.scatter.v;
        if (s > 4.0) notes << QStringLiteral("散布量 %1% は上限の400%へ丸めました").arg(qRound(s * 100));
        pen.setScatter((float)qBound(0.0, s, 4.0));
    }
    if (b.count.has) pen.setParticleCount(b.count.v);
    if (b.followDirection.has) pen.setFollowDirection(b.followDirection.v);

    if (b.tip.isNull()) {
        // 硬さが入っているのは手続き的な丸ブラシ(computedBrush)だけ。
        // 先端画像は使わないブラシなので、前に取り込んだ画像が残らないよう既定の丸へ戻す。
        if (b.hardness.has && pen.tipImagePath() != QLatin1String(kBuiltinCircle)) {
            pen.setTipImagePath(QLatin1String(kBuiltinCircle));
            notes << QStringLiteral("丸ブラシなので先端画像を既定の円に戻しました");
        }
    } else {
        const QString path = saveImportedTip(b.tip, b.name);
        if (path.isEmpty()) {
            notes << QStringLiteral("先端画像を取り込めませんでした");
        } else {
            pen.setTipImagePath(path);
            // Photoshopの先端画像は輪郭まで含めて画像そのもの(硬さの項目が無い)。
            // このアプリの硬さは画像の上からさらにフォールオフを掛けるので、
            // 元の見た目を保つために硬さは最大にする。
            pen.setHardness(1.0f);
            notes << QStringLiteral("先端画像(%1×%2)を取り込み、硬さを100%にしました")
                     .arg(b.tip.width()).arg(b.tip.height());
        }
    }

    return notes;
}

AbrBrush fromPen(const PenToolConfig &pen, const QString &name)
{
    AbrBrush b;
    b.name = name;
    b.diameterPx.set(pen.size());
    b.hardness.set(pen.hardness());
    b.spacing.set(pen.spacing());
    b.angleDeg.set(pen.angleDeg());
    b.roundness.set(pen.roundness());
    b.minDiameter.set(pen.minSizeRatio());
    b.sizeJitter.set(pen.sizeJitter());
    b.angleJitter.set(pen.angleJitter());
    b.opacityJitter.set(pen.opacityJitter());
    b.scatter.set(pen.scatter());
    b.count.set(pen.particleCount());
    b.followDirection.set(pen.followDirection());

    // 既定の丸は手続き的なブラシ(硬さを持てる)として出す方が素直なので、
    // それ以外のときだけ画像を持たせる。
    if (pen.tipImagePath() != QLatin1String(kBuiltinCircle)) {
        const QImage tip(pen.tipImagePath());
        if (!tip.isNull()) b.tip = tip.convertToFormat(QImage::Format_RGBA8888);
    }
    return b;
}

QStringList unmappedFromPen(const PenToolConfig &pen)
{
    QStringList lost;

    // 先端画像を使うブラシはPhotoshop側では sampledBrush になり、硬さの項目が無い。
    if (pen.tipImagePath() != QLatin1String(kBuiltinCircle))
        lost << QStringLiteral("硬さ(先端画像を使うブラシにはPhotoshop側に硬さの項目がありません)");
    if (pen.flow() < 1.0f)              lost << QStringLiteral("フロー");
    if (pen.brushBlendMode() != 0)      lost << QStringLiteral("合成モード");
    if (!pen.pressureCurve().isIdentity()) lost << QStringLiteral("筆圧カーブ");
    if (pen.minOpacityRatio() > 0.0f)   lost << QStringLiteral("最小不透明度");
    if (pen.tiltSize() > 0.0f || pen.tiltOpacity() > 0.0f || pen.tiltFlatten() > 0.0f
        || pen.tiltAngleFollow() || pen.penRotationFollow())
        lost << QStringLiteral("傾き・ペン回転");
    if (pen.paperEnabled())             lost << QStringLiteral("紙質");
    if (pen.taperInPx() > 0 || pen.taperOutPx() > 0) lost << QStringLiteral("入り抜き");
    if (pen.postCorrection() > 0.0f)    lost << QStringLiteral("後補正");
    if (pen.mixRate() > 0.0f || pen.usesPaintDepletion() || pen.pickupOnly())
        lost << QStringLiteral("混色");
    if (pen.hueJitter() > 0.0f || pen.valueJitter() > 0.0f)
        lost << QStringLiteral("色のランダム");
    if (pen.spacingJitter() > 0.0f)     lost << QStringLiteral("間隔のランダム");

    return lost;
}

} // namespace AbrPenMapping
