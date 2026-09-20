#include "docks/layers/LayerDockLayout.h"

QVector<QVector<int>> computeRows(const CanvasDocument &doc, int scope)
{
    QVector<QVector<int>> rows;
    const int lo = (scope < 0) ? 0 : scope + 1;
    const int hi = (scope < 0) ? doc.layerCount() : lo + doc.layers[scope].childCount;
    int i = lo;
    while (i < hi) {
        const Layer &l = doc.layers[i];
        if (rows.isEmpty() || !l.clipping)
            rows.append(QVector<int>{ i });
        else
            rows.last().append(i);
        i += (l.layerType == LayerType::Folder) ? (1 + l.childCount) : 1;
    }
    return rows;
}

int afterLayerBlock(const CanvasDocument &doc, int idx)
{
    if (idx < 0 || idx >= doc.layerCount()) return doc.layerCount();
    const Layer &l = doc.layers[idx];
    return idx + 1 + (l.layerType == LayerType::Folder ? l.childCount : 0);
}

int scopeEndIndex(const CanvasDocument &doc, int scope)
{
    return (scope < 0) ? doc.layerCount() : (scope + 1 + doc.layers[scope].childCount);
}

QColor blendModeColor(BlendMode mode)
{
    switch (mode) {
    // 暗くする系 (赤)。
    case BlendMode::Darken:
    case BlendMode::Multiply:
    case BlendMode::ColorBurn:
    case BlendMode::LinearBurn:
    case BlendMode::DarkerColor:
        return QColor("#e4043c");
    // 明るくする系 (緑)。
    case BlendMode::Lighten:
    case BlendMode::Screen:
    case BlendMode::ColorDodge:
    case BlendMode::LinearDodge:
    case BlendMode::LighterColor:
        return QColor("#32e3a8");
    // コントラスト系 (オレンジ)。
    case BlendMode::Overlay:
    case BlendMode::SoftLight:
    case BlendMode::HardLight:
    case BlendMode::VividLight:
    case BlendMode::LinearLight:
    case BlendMode::PinLight:
    case BlendMode::HardMix:
        return QColor("#ff9822");
    // 比較系 (紫)。
    case BlendMode::Difference:
    case BlendMode::Exclusion:
    case BlendMode::Subtract:
    case BlendMode::Divide:
        return QColor("#a855f7");
    // 色相/彩度/カラー/輝度 (水色)。
    case BlendMode::Hue:
    case BlendMode::Saturation:
    case BlendMode::Color:
    case BlendMode::Luminosity:
        return QColor("#22d3ee");
    // ディザ合成。
    case BlendMode::Dissolve:
        return QColor("#9ca3af");
    default:
        return QColor("#2f8dff");
    }
}

void drawStarShape(QPainter &p, const QRectF &r)
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr int points = 5;
    const QPointF c = r.center();
    const qreal outerR = qMin(r.width(), r.height()) / 2.0;
    const qreal innerR = outerR * 0.45;
    const qreal startAngle = -kPi / 2.0; // 真上から開始

    QPolygonF star;
    for (int i = 0; i < points * 2; i++) {
        qreal ang = startAngle + i * kPi / points;
        qreal rad = (i % 2 == 0) ? outerR : innerR;
        star << QPointF(c.x() + rad * std::cos(ang), c.y() + rad * std::sin(ang));
    }
    p.drawPolygon(star);
}

void drawLayerIndicatorShape(QPainter &p, const QRectF &r, LayerType type)
{
    switch (type) {
    case LayerType::SolidColor:
        p.drawRect(r);
        break;
    case LayerType::Adjustment: {
        QPolygonF diamond;
        diamond << QPointF(r.center().x(), r.top())
                << QPointF(r.right(), r.center().y())
                << QPointF(r.center().x(), r.bottom())
                << QPointF(r.left(), r.center().y());
        p.drawPolygon(diamond);
        break;
    }
    case LayerType::Filter: {
        // 調整レイヤー(ダイヤ)と紛れないよう六角形にする。
        QPolygonF hex;
        const qreal cx = r.center().x(), cy = r.center().y();
        const qreal rx = r.width() * 0.5, ry = r.height() * 0.5;
        for (int i = 0; i < 6; i++) {
            const qreal a = M_PI / 6.0 + i * M_PI / 3.0;
            hex << QPointF(cx + rx * qCos(a), cy + ry * qSin(a));
        }
        p.drawPolygon(hex);
        break;
    }
    case LayerType::Text: {
        QPolygonF triangle;
        triangle << QPointF(r.left(), r.top())
                 << QPointF(r.right(), r.top())
                 << QPointF(r.center().x(), r.bottom());
        p.drawPolygon(triangle);
        break;
    }
    case LayerType::Folder:
        drawStarShape(p, r);
        break;
    default:
        p.drawEllipse(r);
        break;
    }
}

QString blendModeNameJa(BlendMode mode)
{
    switch (mode) {
    case BlendMode::Dissolve:     return "ディザ合成";
    case BlendMode::Darken:       return "比較(暗)";
    case BlendMode::Multiply:     return "乗算";
    case BlendMode::ColorBurn:    return "焼き込みカラー";
    case BlendMode::LinearBurn:   return "焼き込み(リニア)";
    case BlendMode::DarkerColor:  return "暗さの比較";
    case BlendMode::Lighten:      return "比較(明)";
    case BlendMode::Screen:       return "スクリーン";
    case BlendMode::ColorDodge:   return "覆い焼きカラー";
    case BlendMode::LinearDodge:  return "覆い焼き(リニア)-加算";
    case BlendMode::LighterColor: return "明るさの比較";
    case BlendMode::Overlay:      return "オーバーレイ";
    case BlendMode::SoftLight:    return "ソフトライト";
    case BlendMode::HardLight:    return "ハードライト";
    case BlendMode::VividLight:   return "ビビッドライト";
    case BlendMode::LinearLight:  return "リニアライト";
    case BlendMode::PinLight:     return "ピンライト";
    case BlendMode::HardMix:      return "ハードミックス";
    case BlendMode::Difference:   return "差の絶対値";
    case BlendMode::Exclusion:    return "除外";
    case BlendMode::Subtract:     return "減算";
    case BlendMode::Divide:       return "除算";
    case BlendMode::Hue:          return "色相";
    case BlendMode::Saturation:   return "彩度";
    case BlendMode::Color:        return "カラー";
    case BlendMode::Luminosity:   return "輝度";
    default:                      return "普通";
    }
}

QString adjustmentKindNameJa(AdjustmentKind kind)
{
    switch (kind) {
    case AdjustmentKind::BrightnessContrast: return "明るさ・コントラスト";
    case AdjustmentKind::HueSaturation:      return "色相・彩度・明度";
    case AdjustmentKind::ColorBalance:       return "カラーバランス";
    case AdjustmentKind::ToneCurve:          return "トーンカーブ";
    case AdjustmentKind::GradientMap:        return "グラデーションマップ";
    }
    return QString();
}

QString filterKindNameJa(FilterKind kind)
{
    switch (kind) {
    case FilterKind::GaussianBlur:        return "ぼかし";
    case FilterKind::MotionBlur:          return "移動ぼかし";
    case FilterKind::LensBlur:            return "レンズぼかし";
    case FilterKind::Mosaic:              return "モザイク";
    case FilterKind::Noise:               return "ノイズ";
    case FilterKind::ChromaticAberration: return "色収差";
    }
    return QString();
}

QPixmap layerTypeThumbnail(const Layer &layer, const QSize &size)
{
    const LayerType type = layer.layerType;
    QPixmap pm(size);
    switch (type) {
    case LayerType::SolidColor:
        pm.fill(layer.solidColor);
        return pm;
    case LayerType::Text: {
        QImage src(":/icons/tool/text_preview.png");
        // 中央を切り抜いてプレビュー全体を埋める。
        QImage scaled = src.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        QRect cropRect(
            (scaled.width()  - size.width())  / 2,
            (scaled.height() - size.height()) / 2,
            size.width(), size.height());
        QPainter p(&pm);
        p.drawImage(pm.rect(), scaled, cropRect);
        return pm;
    }
    case LayerType::Adjustment: {
        QImage src(":/icons/tool/colorAdjust.png");
        QImage scaled = src.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        QRect cropRect(
            (scaled.width()  - size.width())  / 2,
            (scaled.height() - size.height()) / 2,
            size.width(), size.height());
        QPainter p(&pm);
        p.drawImage(pm.rect(), scaled, cropRect);
        return pm;
    }
    case LayerType::Filter: {
        // フィルターの種類ごとにサムネイルを描く。
        pm.fill(QColor(30, 30, 34));
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF c(size.width() * 0.5, size.height() * 0.5);
        switch (layer.filter.kind) {
        case FilterKind::GaussianBlur: {
            // ぼかし: 中心が濃く外へ滑らかに薄れる同心円(ガウスの断面のイメージ)。
            const qreal maxR = qMin(size.width(), size.height()) * 0.42;
            const int steps = 10;
            p.setPen(Qt::NoPen);
            for (int i = steps; i >= 1; i--) {
                const qreal t = (qreal)i / steps;
                p.setBrush(QColor(235, 235, 240, (int)(220 * (1.0 - t * t))));
                p.drawEllipse(c, maxR * t, maxR * t);
            }
            break;
        }
        case FilterKind::MotionBlur: {
            // 移動ぼかし: 進行方向へ尾を引く筋(先端が濃く、後方ほど薄れる)。
            const qreal half = size.width() * 0.32;
            const int steps = 7;
            for (int i = 0; i < steps; i++) {
                const qreal t = (qreal)i / (steps - 1); // 0(後方)..1(先端)
                QPen pen(QColor(235, 235, 240, (int)(200 * t + 30)));
                pen.setWidthF(size.height() * 0.06);
                pen.setCapStyle(Qt::RoundCap);
                p.setPen(pen);
                const qreal x = c.x() - half + half * 2.0 * t;
                p.drawLine(QPointF(c.x() - half, c.y()), QPointF(x, c.y()));
            }
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 230));
            p.drawEllipse(QPointF(c.x() + half, c.y()), size.height() * 0.09, size.height() * 0.09);
            break;
        }
        case FilterKind::LensBlur: {
            // レンズぼかし: 大きさの異なる玉ボケ(ソフトエッジの円)を三角形に配置。
            p.setCompositionMode(QPainter::CompositionMode_Plus);
            p.setPen(Qt::NoPen);
            const qreal baseR = qMin(size.width(), size.height()) * 0.22;
            struct Bokeh { QPointF off; qreal rScale; int alpha; };
            const Bokeh spots[3] = {
                { QPointF(-size.width() * 0.16, -size.height() * 0.12), 1.0, 130 },
                { QPointF( size.width() * 0.14, -size.height() * 0.06), 0.75, 150 },
                { QPointF( size.width() * 0.02,  size.height() * 0.18), 0.55, 170 },
            };
            for (const Bokeh &b : spots) {
                p.setBrush(QColor(255, 244, 214, b.alpha));
                p.drawEllipse(c + b.off, baseR * b.rScale, baseR * b.rScale);
            }
            break;
        }
        case FilterKind::Mosaic: {
            // モザイク: 濃淡の異なる正方形ブロックを格子状に並べる(ピクセレートの表現)。
            p.setPen(Qt::NoPen);
            const int cols = 4, rows = 3;
            const qreal bw = (qreal)size.width() / cols;
            const qreal bh = (qreal)size.height() / rows;
            for (int ry = 0; ry < rows; ry++) {
                for (int rx = 0; rx < cols; rx++) {
                    const int shade = 70 + ((rx * 37 + ry * 61) % 140);
                    p.setBrush(QColor(shade, shade, shade));
                    p.drawRect(QRectF(rx * bw, ry * bh, bw + 0.5, bh + 0.5));
                }
            }
            break;
        }
        case FilterKind::Noise: {
            // ノイズ: 中間グレーの地に白黒の粒をランダムに散らす(フィルムグレインの表現)。
            pm.fill(QColor(110, 110, 110));
            p.setPen(Qt::NoPen);
            quint32 rngState = 12345u;
            auto nextRand = [&]() {
                rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
                return rngState;
            };
            for (int i = 0; i < 90; i++) {
                const qreal x = (qreal)(nextRand() % (uint)size.width());
                const qreal y = (qreal)(nextRand() % (uint)size.height());
                const bool bright = (nextRand() & 1) != 0;
                p.setBrush(bright ? QColor(255, 255, 255, 180) : QColor(0, 0, 0, 180));
                p.drawRect(QRectF(x, y, 1.5, 1.5));
            }
            break;
        }
        case FilterKind::ChromaticAberration: {
            // 色収差: R/G/Bをずらした3つの円。
            p.setCompositionMode(QPainter::CompositionMode_Plus);
            p.setPen(Qt::NoPen);
            const qreal r = qMin(size.width(), size.height()) * 0.30;
            const qreal d = r * 0.45;
            p.setBrush(QColor(255, 0, 0)); p.drawEllipse(c + QPointF(-d, 0), r, r);
            p.setBrush(QColor(0, 255, 0)); p.drawEllipse(c,                 r, r);
            p.setBrush(QColor(0, 0, 255)); p.drawEllipse(c + QPointF( d, 0), r, r);
            break;
        }
        }
        return pm;
    }
    case LayerType::Folder: {
        pm.fill(QColor(245, 166, 35, 60));
        QImage src(":/icons/common/add_folder.png");
        QImage scaled = src.scaled(size * 0.7, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPoint off((size.width() - scaled.width()) / 2, (size.height() - scaled.height()) / 2);
        QPainter p(&pm);
        p.drawImage(off, scaled);
        return pm;
    }
    default:
        return pm;
    }
}

QPixmap checkeredThumbnail(const QImage &img, const QSize &size)
{
    QPixmap pm(size);
    QPainter p(&pm);
    const int cell = 6;
    for (int y = 0; y < size.height(); y += cell) {
        for (int x = 0; x < size.width(); x += cell) {
            bool dark = ((x / cell) + (y / cell)) % 2 == 0;
            p.fillRect(x, y, cell, cell, dark ? Theme::checkerDark : Theme::checkerLight);
        }
    }
    QImage scaled = img.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPoint off((size.width() - scaled.width()) / 2, (size.height() - scaled.height()) / 2);
    p.drawImage(off, scaled);
    return pm;
}
