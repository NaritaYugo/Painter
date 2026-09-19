#include "components/ToolPreview.h"
#include "components/ThemeColors.h"
#include "tools/core/ToolConfig.h"
#include "tools/core/PressureResponse.h"

#include <QHash>
#include <QPainter>
#include <QVector2D>
#include <QtMath>

#include <cmath>

namespace {

// ---------------------------------------------------------------------------
// stroke.comp の sampleBrushAlpha() と同じ定数。値の意味はあちらのコメント参照。
// ここを変えるときは向こうも一緒に変えること(見た目がずれる)。
// ---------------------------------------------------------------------------
constexpr float kEdgeAA            = 1.0f;
constexpr float kSoftRefPx         = 8.0f;
constexpr float kSoftOutwardRatio  = 0.15f;
constexpr float kMaxFalloffVsRadius = 3.0f;

// 打つスタンプ数の上限。間隔を最小・粒子数を最大にすると数千個になりうるので、
// プレビュー(数十px四方)には過剰なぶんで頭打ちにする。
constexpr int kMaxStamps = 4000;

float smoothstep(float e0, float e1, float x)
{
    if (e1 <= e0) return x < e0 ? 0.0f : 1.0f;
    const float t = qBound(0.0f, (x - e0) / (e1 - e0), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 先端画像はパスからの読み込みが毎回だと重いので使い回す。
// アルファだけ見る(GPU側も .a しか読まない)ので8bitに落として持つ。
const QImage *tipAlphaImage(const QString &path)
{
    static QHash<QString, QImage> cache;
    if (path.isEmpty()) return nullptr;
    auto it = cache.constFind(path);
    if (it == cache.constEnd()) {
        QImage src(path);
        QImage alpha;
        if (!src.isNull()) {
            src = src.convertToFormat(QImage::Format_RGBA8888);
            alpha = QImage(src.width(), src.height(), QImage::Format_Grayscale8);
            for (int y = 0; y < src.height(); y++) {
                const uchar *s = src.constScanLine(y);
                uchar *d = alpha.scanLine(y);
                for (int x = 0; x < src.width(); x++) d[x] = s[x * 4 + 3];
            }
        }
        it = cache.insert(path, alpha);
    }
    return it->isNull() ? nullptr : &(*it);
}

// uv(0〜1)で先端画像のアルファを線形補間で拾う。GPU側のGL_LINEAR相当。
float sampleTip(const QImage &tip, float u, float v)
{
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return 0.0f;
    const float fx = u * (tip.width()  - 1);
    const float fy = v * (tip.height() - 1);
    const int x0 = qBound(0, (int)fx, tip.width()  - 1);
    const int y0 = qBound(0, (int)fy, tip.height() - 1);
    const int x1 = qMin(x0 + 1, tip.width()  - 1);
    const int y1 = qMin(y0 + 1, tip.height() - 1);
    const float tx = fx - x0, ty = fy - y0;
    const uchar *r0 = tip.constScanLine(y0);
    const uchar *r1 = tip.constScanLine(y1);
    const float a = r0[x0] * (1 - tx) + r0[x1] * tx;
    const float b = r1[x0] * (1 - tx) + r1[x1] * tx;
    return (a * (1 - ty) + b * ty) / 255.0f;
}

// 1つのスタンプがある点へ与えるカバレッジ(0〜1)。
// stroke.comp の sampleBrushAlpha() をそのまま写したもの(点スタンプなので a==b)。
float coverageAt(const QVector2D &pos, const QVector2D &center, float radius,
                 float cosT, float sinT, float roundness, float hardness,
                 const QImage *tip)
{
    const QVector2D d = pos - center;
    QVector2D local(d.x() * cosT + d.y() * sinT,
                    -d.x() * sinT + d.y() * cosT);
    local.setY(local.y() / qMax(roundness, 0.01f));
    const float dist = local.length();

    float falloffPx = qMax((1.0f - hardness) * (radius + kSoftRefPx), kEdgeAA);
    falloffPx = qMin(falloffPx, qMax(radius * kMaxFalloffVsRadius, kEdgeAA));

    const float outer = radius + qMax(0.0f, (falloffPx - radius) * kSoftOutwardRatio);
    if (dist > outer) return 0.0f;

    const float inner   = outer - falloffPx;
    const float falloff = 1.0f - smoothstep(inner, outer, dist);
    if (tip) {
        const float u = local.x() / (2.0f * outer) + 0.5f;
        const float v = local.y() / (2.0f * outer) + 0.5f;
        return sampleTip(*tip, u, v) * falloff;
    }
    return falloff;
}

// PenEraserTool と同じ xorshift32。プレビューは毎回同じ絵になってほしいので
// 種は固定にする(選び直すたびに粒の位置が変わると落ち着かない)。
struct Rng {
    quint32 s = 0x9e3779b9u;
    float next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (s & 0xffffffu) / float(0x1000000u);
    }
    float signedNext() { return next() * 2.0f - 1.0f; }
};

// 1本のストロークを表すマスク(0〜1)。画像の画素数ぶんの配列で返す。
struct StrokeMask {
    QVector<float> a;
    int w = 0, h = 0;
    float at(int x, int y) const { return a[y * w + x]; }
};

// ブラシの見た目を決めるパラメータ。ペン以外のツールは持っていない項目を
// 既定値のままにして同じ経路で描く。
struct BrushShape {
    int   size        = 25;
    float opacity     = 1.0f;
    float hardness    = 0.5f;
    float spacing     = 0.1f;
    float flow        = 1.0f;
    float roundness   = 1.0f;
    int   angleDeg    = 0;
    bool  followDir   = false;
    float minSizeRatio = 0.0f;
    bool  pressureOpacity = false;
    float minOpacityRatio = 0.0f;
    int   taperInPx   = 0;
    int   taperOutPx  = 0;
    bool  taperSize   = true;
    bool  taperOpacity = false;
    float scatter     = 0.0f;
    int   particleCount = 1;
    float sizeJitter  = 0.0f;
    float opacityJitter = 0.0f;
    float angleJitter = 0.0f;
    float spacingJitter = 0.0f;
    QString tipPath;   // 空なら手続き的な円
};

// ストロークの筆圧プロファイル(0〜1)。実際にペンで1本引いたときのように
// 立ち上がって、抜けるところで落ちる。これがあることで「最小サイズ」や
// 「筆圧で不透明度」の設定が絵として見える。
float pressureAt(float t)
{
    return 0.15f + 0.85f * std::pow(std::sin(float(M_PI) * qBound(0.0f, t, 1.0f)), 0.6f);
}

StrokeMask buildStrokeMask(const BrushShape &b, const QSize &sizePx)
{
    StrokeMask mask;
    mask.w = sizePx.width();
    mask.h = sizePx.height();
    mask.a.fill(0.0f, mask.w * mask.h);
    if (mask.w <= 0 || mask.h <= 0) return mask;

    const QImage *tip = tipAlphaImage(b.tipPath);

    // プレビューに収まるように縮める倍率。
    //
    // 「収まらないときだけ縮める」(= min(1, 上限/サイズ))にすると、上限を超える
    // サイズは全部同じ太さで頭打ちになってしまう。ブラシは20〜100pxあたりが
    // 一番よく使われるので、それでは大半のプリセットが同じ絵になる。
    // そこで頭打ちではなく緩やかに飽和させ、どのサイズでも大小関係が見えるようにする:
    //     見た目の太さ = size * 上限 / (size + 上限)
    // (size→∞ で上限に漸近。上限=19pxなら 5px→3.9 / 25px→10.7 / 60px→14.3 /
    //  1000px→18.4 と、太いほど太いまま収まる)
    const float maxThickness = mask.h * 0.5f;
    const float scale = maxThickness / (qMax(1.0f, (float)b.size) + maxThickness);

    // 以降の計算はすべてキャンバスpxで行う(硬さの式が実寸を前提にしているため)。
    // プレビュー画素 → キャンバス座標 は 1/scale 倍。
    const float wC = mask.w / scale;
    const float hC = mask.h / scale;
    const float rMax = b.size * 0.5f;

    // 軌道: ゆるいS字。太くて振れ幅が取れないときは自動的に直線になる。
    const float marginX = qMin(wC * 0.22f, rMax + 2.0f);
    const float x0 = marginX, x1 = wC - marginX;
    if (x1 <= x0) return mask;
    const float amp = qMax(0.0f, hC * 0.5f - rMax - 1.0f) * 0.85f;
    // S字にする(上→中→下)。太くて振れ幅が取れないときは amp が0になり直線になる。
    const auto pathAt = [&](float t) {
        return QVector2D(x0 + (x1 - x0) * t,
                         hC * 0.5f - amp * std::sin(float(M_PI) * 2.0f * t));
    };

    // 軌道長(等間隔にスタンプを置くために先に測る)
    const int kSamples = 256;
    QVector<QVector2D> pts(kSamples + 1);
    QVector<float> arc(kSamples + 1, 0.0f);
    for (int i = 0; i <= kSamples; i++) pts[i] = pathAt(float(i) / kSamples);
    for (int i = 1; i <= kSamples; i++) arc[i] = arc[i-1] + (pts[i] - pts[i-1]).length();
    const float totalLen = arc[kSamples];
    if (totalLen <= 0.0f) return mask;

    Rng rng;
    const float baseStep = qMax(1.0f, b.size * b.spacing);
    const float invScale = 1.0f / scale;

    int stampCount = 0;
    for (float dist = 0.0f; dist <= totalLen && stampCount < kMaxStamps; ) {
        // 軌道上の位置と進行方向
        const float u = dist / totalLen;
        int seg = qBound(1, (int)(u * kSamples), kSamples);
        while (seg < kSamples && arc[seg] < dist) seg++;
        const float segT = (arc[seg] - arc[seg-1] > 0.0f)
                             ? (dist - arc[seg-1]) / (arc[seg] - arc[seg-1]) : 0.0f;
        const QVector2D center = pts[seg-1] + (pts[seg] - pts[seg-1]) * segT;
        const QVector2D dir    = (pts[seg] - pts[seg-1]).normalized();
        const float dirAngle   = std::atan2(dir.y(), dir.x());

        // 筆圧 → 半径と濃さ(PenEraserTool::pressureRadius / pressureStampAlpha と同じ式)
        const float pressure = pressureAt(u);
        float radius = b.size * 0.5f * PressureResponse::scale(pressure, b.minSizeRatio);
        float alpha  = b.pressureOpacity
                         ? PressureResponse::scale(pressure, b.minOpacityRatio) : 1.0f;

        // 入り抜き(PenEraserTool::taperFactor と同じ)
        if (b.taperInPx > 0 || b.taperOutPx > 0) {
            float inPx = (float)b.taperInPx, outPx = (float)b.taperOutPx;
            const float sum = inPx + outPx;
            if (sum > totalLen && sum > 0.0f) { const float k = totalLen / sum; inPx *= k; outPx *= k; }
            float f = 1.0f;
            if (inPx  > 0.0f) f = qMin(f, dist / inPx);
            if (outPx > 0.0f) f = qMin(f, (totalLen - dist) / outPx);
            f = qBound(0.0f, f, 1.0f);
            if (b.taperSize)    radius = qMax(0.1f, radius * f);
            if (b.taperOpacity) alpha *= f;
        }

        // 散布・粒子数・各種ランダム(PenEraserTool::appendStamps と同じ考え方)
        const float scatterPx = b.scatter * (float)b.size;
        for (int i = 0; i < qMax(1, b.particleCount) && stampCount < kMaxStamps; i++) {
            QVector2D p = center;
            if (scatterPx > 0.0f) {
                const float ang = rng.next() * float(M_PI) * 2.0f;
                const float r   = scatterPx * std::sqrt(rng.next());
                p += QVector2D(std::cos(ang) * r, std::sin(ang) * r);
            }
            const float rr = (b.sizeJitter    > 0.0f) ? radius * (1.0f - rng.next() * b.sizeJitter)    : radius;
            const float aa = (b.opacityJitter > 0.0f) ? alpha  * (1.0f - rng.next() * b.opacityJitter) : alpha;
            float th = qDegreesToRadians((float)b.angleDeg);
            if (b.followDir)          th += dirAngle;
            if (b.angleJitter > 0.0f) th += rng.next() * float(M_PI) * 2.0f * b.angleJitter;
            const float cosT = std::cos(th), sinT = std::sin(th);

            const float rad = qMax(0.1f, rr);
            // このスタンプが触れうる範囲だけ回す。フォールオフで外へ広がるぶんも見込む。
            const float reach = rad + qMax(rad * kMaxFalloffVsRadius * kSoftOutwardRatio, kEdgeAA)
                              + rad * (1.0f / qMax(b.roundness, 0.01f) - 1.0f);
            const int px0 = qMax(0,          (int)std::floor((p.x() - reach) * scale));
            const int px1 = qMin(mask.w - 1, (int)std::ceil ((p.x() + reach) * scale));
            const int py0 = qMax(0,          (int)std::floor((p.y() - reach) * scale));
            const int py1 = qMin(mask.h - 1, (int)std::ceil ((p.y() + reach) * scale));

            for (int y = py0; y <= py1; y++) {
                for (int x = px0; x <= px1; x++) {
                    const QVector2D canvasPos((x + 0.5f) * invScale, (y + 0.5f) * invScale);
                    const float cov = coverageAt(canvasPos, p, rad, cosT, sinT,
                                                 b.roundness, b.hardness, tip);
                    if (cov <= 0.0f) continue;
                    // stroke.comp のマスク蓄積そのまま: m += target*dep - m*dep
                    const float dep = cov * b.flow;
                    float &m = mask.a[y * mask.w + x];
                    m = m + aa * dep - m * dep;
                }
            }
            stampCount++;
        }

        float step = baseStep;
        if (b.spacingJitter > 0.0f) step *= 1.0f + rng.signedNext() * b.spacingJitter;
        dist += qMax(1.0f, step);
    }
    return mask;
}

// プレビューの下敷き(=キャンバスに見立てた紙)。
//
// 行の背景の上に直接ストロークを描くと、黒いペンが暗いテーマの行に沈んで
// 何も見えない。実際に描くときと同じ「紙の上」に置くことで、ペンの色が
// そのままの色で見えるようにする。
//
// 紙は基本的に白(普段の描き方に近い)だが、白や淡い色を選んでいるときは
// 白い紙の上では見えなくなってしまう。プレビューの役目は「そのブラシの形が
// 分かること」なので、明るい色のときだけ紙を暗くして必ず見えるようにする。
QColor paperColorFor(const QColor &ink)
{
    return (ink.lightnessF() > 0.72) ? QColor(0x4a, 0x4a, 0x4a) : QColor(0xf4, 0xf4, 0xf4);
}

QImage paperImage(int w, int h, const QColor &paper = QColor(0xf4, 0xf4, 0xf4))
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(paper);
    p.drawRoundedRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0), 3, 3);
    return img;
}

// マスクを1色で塗って紙の上に乗せる(ペン・エアブラシ用)。
QImage inkImage(const StrokeMask &mask, const QColor &ink, float opacity)
{
    QImage img = paperImage(mask.w, mask.h, paperColorFor(ink));
    const int r = ink.red(), g = ink.green(), bl = ink.blue();
    const float ia = (float)ink.alphaF() * opacity;
    for (int y = 0; y < mask.h; y++) {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < mask.w; x++) {
            const float sa = qBound(0.0f, mask.at(x, y) * ia, 1.0f);
            if (sa <= 0.0f) continue;
            const QRgb d = line[x];
            const float da = qAlpha(d) / 255.0f;
            const float oa = sa + da * (1.0f - sa);
            if (oa <= 0.0f) continue;
            const auto mix = [&](int s, int dv) {
                return (int)qRound((s * sa + dv * da * (1.0f - sa)) / oa);
            };
            line[x] = qRgba(mix(r, qRed(d)), mix(g, qGreen(d)), mix(bl, qBlue(d)),
                            (int)qRound(oa * 255.0f));
        }
    }
    return img;
}

// 「消す」系(消しゴム・透明色)の見せ方。
// 紙の上にインクの帯を敷いて、そこからストロークで削り取る。削れたところは
// 紙が出るので、消しゴムの太さと硬さ(縁のぼけ具合)がそのまま見える。
QImage eraseImage(const StrokeMask &mask, float strength)
{
    QImage img = paperImage(mask.w, mask.h);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x4a, 0x6f, 0xa5));   // 紙の上に置いたインク(消される側)
        p.drawRoundedRect(QRectF(3.5, 3.5, mask.w - 7.0, mask.h - 7.0), 2, 2);
    }
    const QImage paper = paperImage(mask.w, mask.h);
    for (int y = 0; y < mask.h; y++) {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        const auto *pl = reinterpret_cast<const QRgb *>(paper.constScanLine(y));
        for (int x = 0; x < mask.w; x++) {
            const float e = qBound(0.0f, mask.at(x, y) * strength, 1.0f);
            if (e <= 0.0f) continue;
            // 削れたぶんだけ紙の色へ戻す
            const QRgb c = line[x], pc = pl[x];
            const auto mix = [&](int a, int b) { return (int)qRound(a * (1.0f - e) + b * e); };
            line[x] = qRgba(mix(qRed(c), qRed(pc)), mix(qGreen(c), qGreen(pc)),
                            mix(qBlue(c), qBlue(pc)), mix(qAlpha(c), qAlpha(pc)));
        }
    }
    return img;
}

// ぼかしの見せ方。紙の上に縦縞を描いて、ストロークが通ったところだけ横方向へ均す。
QImage blurImage(const StrokeMask &mask, float strength, int radiusPx)
{
    QImage img = paperImage(mask.w, mask.h);
    {
        QPainter p(&img);
        p.setClipRect(QRect(2, 2, mask.w - 4, mask.h - 4));
        for (int x = 2; x < mask.w - 2; x += 8)
            p.fillRect(QRect(x, 2, 4, mask.h - 4), QColor(0x4a, 0x6f, 0xa5));
    }
    // 横方向の移動平均(縞が均されて「ぼけた」ように見える)。
    // 半径は設定のぼかし半径をそのまま使うが、プレビューの幅に対して効きすぎない
    // ように上限を付ける。
    const int r = qBound(1, radiusPx, 10);
    QImage blurred = img;
    for (int y = 0; y < mask.h; y++) {
        const auto *src = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        auto *dst = reinterpret_cast<QRgb *>(blurred.scanLine(y));
        for (int x = 0; x < mask.w; x++) {
            int sr = 0, sg = 0, sb = 0, n = 0;
            for (int k = -r; k <= r; k++) {
                const QRgb c = src[qBound(0, x + k, mask.w - 1)];
                sr += qRed(c); sg += qGreen(c); sb += qBlue(c); n++;
            }
            dst[x] = qRgba(sr / n, sg / n, sb / n, qAlpha(src[x]));
        }
    }
    for (int y = 0; y < mask.h; y++) {
        auto *out = reinterpret_cast<QRgb *>(img.scanLine(y));
        const auto *bl = reinterpret_cast<const QRgb *>(blurred.constScanLine(y));
        for (int x = 0; x < mask.w; x++) {
            const float t = qBound(0.0f, mask.at(x, y) * strength, 1.0f);
            if (t <= 0.0f) continue;
            const auto mix = [&](int a, int b) { return (int)qRound(a * (1.0f - t) + b * t); };
            out[x] = qRgba(mix(qRed(out[x]),   qRed(bl[x])),
                           mix(qGreen(out[x]), qGreen(bl[x])),
                           mix(qBlue(out[x]),  qBlue(bl[x])), qAlpha(out[x]));
        }
    }
    return img;
}

} // namespace

// ===========================================================================
namespace ToolPreview {

bool isSupported(ToolType tool)
{
    switch (tool) {
    case ToolType::Pen:
    case ToolType::Eraser:
    case ToolType::Airbrush:
    case ToolType::Blur:
        return true;
    default:
        return false;   // 移動・回転・塗りつぶし・スポイト・選択・テキスト・ゆがみ
    }
}

QImage render(ToolType tool, const QVariantMap &values, const QSize &size,
              const QColor &ink, bool inkIsTransparent)
{
    if (!isSupported(tool) || size.isEmpty()) return {};

    switch (tool) {
    case ToolType::Pen: {
        PenToolConfig c;
        c.fromMap(values);
        BrushShape b;
        b.size = c.size();             b.opacity   = c.opacity();
        b.hardness = c.hardness();     b.spacing   = c.spacing();
        b.flow = c.flow();             b.roundness = c.roundness();
        b.angleDeg = c.angleDeg();     b.followDir = c.followDirection();
        b.minSizeRatio = c.minSizeRatio();
        b.pressureOpacity = c.pressureOpacity();
        b.minOpacityRatio = c.minOpacityRatio();
        b.taperInPx = c.taperInPx();   b.taperOutPx = c.taperOutPx();
        b.taperSize = c.taperSize();   b.taperOpacity = c.taperOpacity();
        b.scatter = c.scatter();       b.particleCount = c.particleCount();
        b.sizeJitter = c.sizeJitter(); b.opacityJitter = c.opacityJitter();
        b.angleJitter = c.angleJitter(); b.spacingJitter = c.spacingJitter();
        b.tipPath = c.tipImagePath();
        const StrokeMask m = buildStrokeMask(b, size);
        // 透明色を選んでいるときのペンは「消す」動作になる(eraseBrushFor参照)ので、
        // プレビューもそちらに合わせる。
        if (inkIsTransparent)
            return eraseImage(m, c.opacity() * (float)ink.alphaF());
        return inkImage(m, ink, c.opacity());
    }
    case ToolType::Airbrush: {
        AirbrushToolConfig c;
        c.fromMap(values);
        BrushShape b;                  // 先端画像は持たない(常に円)
        b.size = c.size();             b.opacity  = c.opacity();
        b.hardness = c.hardness();     b.spacing  = c.spacing();
        b.minSizeRatio = c.minSizeRatio();
        b.pressureOpacity = c.pressureOpacity();
        b.minOpacityRatio = c.minOpacityRatio();
        return inkImage(buildStrokeMask(b, size), ink, c.opacity());
    }
    case ToolType::Eraser: {
        EraserToolConfig c;
        c.fromMap(values);
        BrushShape b;
        b.size = c.size();  b.hardness = c.hardness();  b.spacing = c.spacing();
        b.minSizeRatio = c.minSizeRatio();
        return eraseImage(buildStrokeMask(b, size), 1.0f);
    }
    case ToolType::Blur: {
        BlurToolConfig c;
        c.fromMap(values);
        BrushShape b;
        b.size = c.size();  b.hardness = c.hardness();
        b.minSizeRatio = c.minSizeRatio();
        return blurImage(buildStrokeMask(b, size), c.strength(), c.blurRadius());
    }
    default:
        return {};
    }
}

} // namespace ToolPreview
