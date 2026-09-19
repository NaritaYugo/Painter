#include "tools/core/CursorUtils.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QImage>
#include <QBitmap>
#include <QHash>
#include <QSet>
#include <QtMath>
#include <cmath>

namespace CursorUtils {

// OSカーソルとして安定して表示できるサイズの上限/下限
static constexpr int kMinDiameter = 3;
static constexpr int kMaxDiameter = 300;

// QPainter::drawEllipse()(AAなし)は角度によって線の太さが不均一になったり
// 隙間ができたりする(ストローク描画がラスタライズの都合で1周きれいに
// つながらない)。そこで、中心からの半径がradiusとなるピクセルを直接1個ずつ
// 求めて塗るアプローチにする。
// 全てのx列・全てのy行それぞれについて円周上の点を1つ求めて集合に入れる
// ことで、どの角度でも隙間なく1px幅でつながった円になる(片方のスイープ
// だけだと45度付近で隙間ができるため、x基準・y基準の両方で走査する)。
static QVector<QPoint> circlePixels(int radius)
{
    QSet<QPoint> pixels;
    if (radius <= 0) {
        pixels.insert(QPoint(0, 0));
    } else {
        const double r = radius;
        for (int x = -radius; x <= radius; ++x) {
            const int y = qRound(std::sqrt(qMax(0.0, r * r - double(x) * x)));
            pixels.insert(QPoint(x, y));
            pixels.insert(QPoint(x, -y));
        }
        for (int y = -radius; y <= radius; ++y) {
            const int x = qRound(std::sqrt(qMax(0.0, r * r - double(y) * y)));
            pixels.insert(QPoint(x, y));
            pixels.insert(QPoint(-x, y));
        }
    }
    return QVector<QPoint>(pixels.constBegin(), pixels.constEnd());
}

static void plotCircle(QImage &img, const QPoint &center, int radius, const QColor &color)
{
    for (const QPoint &offset : circlePixels(radius)) {
        const QPoint p = center + offset;
        if (img.rect().contains(p))
            img.setPixelColor(p, color);
    }
}

QCursor makeCircleCursor(float diameterPx)
{
    int d = qBound(kMinDiameter, qRound(diameterPx), kMaxDiameter);

    // 直径が同じなら結果は毎回完全に同じなので、直径をキーにキャッシュする。
    // この関数はGLWidget::updateCursor()経由でマウス移動イベントのたびに呼ばれるが、
    // カーソル画像の生成(数千ピクセルのプロット+QImage確保+ネイティブカーソル
    // オブジェクト生成)はブラシサイズに比例して重い。特にペンタブは、マウスと違い
    // OSが移動イベントを間引かないため生のサンプル頻度(200Hz以上)でここへ到達し、
    // 「大きいブラシ×ペンタブだけ極端に重い」という体感差の主因になっていた。
    static QHash<int, QCursor> cache;
    const auto it = cache.constFind(d);
    if (it != cache.constEnd()) return *it;

    const int margin = 2; // 線が端で切れないための余白
    const int side = d + margin * 2;
    const int radius = d / 2;
    const QPoint center(side / 2, side / 2);

    QImage img(side, side, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    // 明背景/暗背景どちらでも視認できるよう、外側を黒、1px内側を白のリングにする
    // (XOR反転カーソルはWindowsのカーソル拡大縮小でジャギーが出たため不採用)
    plotCircle(img, center, radius, Qt::black);
    if (radius >= 2)
        plotCircle(img, center, radius - 1, Qt::white);

    QCursor cur(QPixmap::fromImage(img), side / 2, side / 2);
    // ズームやサイズ変更で様々な直径が要求されうるので、際限なく貯めないよう
    // 一定数で丸ごと捨てる(直径の種類は高々kMax-kMin=298通りなので実害はないが、
    // ネイティブカーソルはOS資源なので念のため)。
    if (cache.size() > 128) cache.clear();
    cache.insert(d, cur);
    return cur;
}

QCursor makeIconCursor(const QString &iconPath, int size, bool hotspotAtBottomLeft)
{
    // makeCircleCursor()と同じ理由でキャッシュする。この関数もGLWidget::updateCursor()
    // 経由で移動イベントのたびに呼ばれるが、中身は画像ファイルのデコード+平滑化
    // スケーリング+ネイティブカーソル生成で、1回あたりのコストは無視できない。
    static QHash<QString, QCursor> cache;
    const QString key = iconPath + QLatin1Char('#') + QString::number(size)
                       + (hotspotAtBottomLeft ? QStringLiteral("#bl") : QString());
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) return *it;

    QPixmap src(iconPath);
    if (src.isNull())
        return QCursor(Qt::ArrowCursor); // 失敗はキャッシュしない(パスが後から有効になる場合に備える)
    QPixmap scaled = src.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const int hotX = hotspotAtBottomLeft ? 0 : scaled.width() / 2;
    const int hotY = hotspotAtBottomLeft ? scaled.height() - 1 : scaled.height() / 2;
    QCursor cur(scaled, hotX, hotY);
    if (cache.size() > 64) cache.clear(); // ネイティブカーソルはOS資源なので上限を設ける
    cache.insert(key, cur);
    return cur;
}

QCursor makeIconWithSwatchCursor(const QString &iconPath, QColor color, int iconSize)
{
    // 【重要】色ごとにキャッシュする。GLWidget::applyCursor()は「前回と同じ見た目か」を
    // QPixmap::cacheKeyで判定して、変わったときだけsetCursor()+カーソル再描画
    // (内部でQCursor::setPos()を伴う。GLWidget::applyCursorの長いコメント参照)を行う。
    // 毎回新しいQPixmapを作って返すとcacheKeyが毎回変わり、同じ色の上をなぞっている
    // 間じゅう不要なsetPos()を撃ち続けることになる。
    static QHash<QString, QCursor> cache;
    const QString key = iconPath + QLatin1Char('#') + QString::number(iconSize)
                       + QLatin1Char('#') + QString::number(color.rgba(), 16);
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) return *it;

    QPixmap src(iconPath);
    if (src.isNull())
        return QCursor(Qt::ArrowCursor); // 失敗はキャッシュしない(makeIconCursorと同じ理由)
    const QPixmap icon = src.scaled(iconSize, iconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    const int gap = 4;
    const int swW = 26, swH = 18;
    const int w = icon.width() + gap + swW;
    const int h = qMax(icon.height(), swH);

    QPixmap pix(w, h);
    pix.fill(Qt::transparent);
    {
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing);
        p.drawPixmap(0, 0, icon);

        // スウォッチはアイコンの右隣・縦中央。
        const QRect sw(icon.width() + gap, (h - swH) / 2, swW, swH);

        QPainterPath path;
        path.addRoundedRect(sw, 3, 3);
        p.setClipPath(path);
        // 透明/半透明な色を拾ったときに「黒」に見えてしまわないよう、下地に市松模様を
        // 敷いてから元のアルファ値のまま重ねて描く(完全不透明なら単色に、
        // 完全透明なら市松がそのまま見える)。
        const int checker = 5;
        for (int y = sw.top(); y <= sw.bottom(); y += checker) {
            for (int x = sw.left(); x <= sw.right(); x += checker) {
                const bool dark = (((x - sw.left()) / checker) + ((y - sw.top()) / checker)) % 2 == 0;
                p.fillRect(QRect(x, y, checker, checker), dark ? QColor(200, 200, 200) : QColor(255, 255, 255));
            }
        }
        p.fillRect(sw, color);
        p.setClipping(false);

        // 明背景・暗背景のどちらの上でもスウォッチの輪郭が見えるよう、
        // 外側を白・内側を黒の二重枠にする(makeCircleCursorの黒白リングと同じ考え方)。
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255), 1));
        p.drawRoundedRect(QRectF(sw).adjusted(-0.5, -0.5, 0.5, 0.5), 3.5, 3.5);
        p.setPen(QPen(QColor(0, 0, 0), 1));
        p.drawRoundedRect(QRectF(sw).adjusted(0.5, 0.5, -0.5, -0.5), 2.5, 2.5);
    }

    // ホットスポットはアイコンの左下(=スポイトの先端)。makeIconCursor(…, true)と揃える。
    QCursor cur(pix, 0, icon.height() - 1);
    if (cache.size() > 64) cache.clear();
    cache.insert(key, cur);
    return cur;
}

} // namespace CursorUtils
