#include "docks/LayerDock.h"
#include "widgets/GLWidget.h"
#include "components/ThemeColors.h"
#include "backend/BlendModeList.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QFrame>
#include <QPainter>
#include <QPolygonF>
#include <QLineF>
#include <QTimer>
#include <QPixmap>
#include <QImage>
#include <QGuiApplication>
#include <QScreen>
#include <QCursor>
#include <QMenu>
#include <QAction>
#include <QSplitter>
#include <QRegularExpression>
#include <QHash>
#include <QRandomGenerator>
#include <cmath>

// ===========================================================================
// MaskOpacityPreview ― 不透明度スライダーを置き換える統合コントロール
//
// レイヤーマスクは要するに「場所ごとに違う不透明度」なので、一律の不透明度と
// レイヤーマスクを1つのプレビュー枠にまとめる。
//   ・マスクが無いとき: 不透明度に対応した一様なグレー(100%=白, 0%=黒)を表示。
//   ・マスクがあるとき: マスクの濃淡プレビューを表示。
//   ・左右にドラッグ: 一律の不透明度(scalar)を変更する。
//   ・(ドラッグせず)クリック: そのレイヤーのマスク編集モードをオン/オフする
//     (GLWidget側で、まだマスクが無ければ白マスクを自動生成する)。
// 実際のマスク濃淡プレビュー画像・不透明度・編集中フラグは LayerDock が
// setState() で流し込む(このウィジェット自身はドキュメントを知らない)。
// ===========================================================================
class MaskOpacityPreview : public QWidget
{
    Q_OBJECT
public:
    explicit MaskOpacityPreview(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedWidth(60);
        setFixedHeight(45); // レイヤープレビューのスウォッチ(40x30)と縦幅を揃える
        setCursor(Qt::PointingHandCursor);
        setToolTip("左右ドラッグで不透明度、クリックでマスク編集の切り替え");
    }

    void setState(bool enabled, float opacity, bool hasMask, bool editing, const QImage &maskThumb)
    {
        enabled_   = enabled;
        opacity_   = opacity;
        hasMask_   = hasMask;
        editing_   = editing;
        maskThumb_ = maskThumb;
        update();
    }

signals:
    void opacityChanged(float value); // 左右ドラッグ中(0..1)
    void clicked();                   // ドラッグを伴わないクリック

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);

        if (!enabled_) {
            QRect r = rect().adjusted(0, 0, -1, -1);
            p.fillRect(rect(), Theme::bgInput);
            p.setPen(QPen(Theme::bgButton, 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(r);
            return;
        }

        // 本体(濃淡プレビュー)の上下を三角形マーカーぶんだけ狭める。三角形の
        // x位置で現在の不透明度(スライダー値)を示す(スライダーのつまみ相当)。
        constexpr int kTriH = 5;
        QRect body = rect().adjusted(0, kTriH, 0, -kTriH);
        QRect r    = body.adjusted(0, 0, -1, -1);

        if (hasMask_ && !maskThumb_.isNull()) {
            // マスク濃淡プレビュー(枠いっぱいに引き伸ばす)。下地に市松模様は敷かず、
            // グレースケールをそのまま見せる。
            p.drawImage(body, maskThumb_);
        } else {
            // 一律の不透明度に対応したグレー(100%=白, 0%=黒)。
            int g = qBound(0, int(opacity_ * 255.0f + 0.5f), 255);
            p.fillRect(body, QColor(g, g, g));
        }

        // 数値(不透明度%)。読める色を背景の明るさから選ぶ。
        int pct = qBound(0, int(opacity_ * 100.0f + 0.5f), 100);
        bool darkBg = hasMask_ ? true : (opacity_ < 0.5f);
        p.setPen(darkBg ? QColor(255, 255, 255) : QColor(0, 0, 0));
        QFont f = p.font();
        f.setPointSizeF(8.5);
        p.setFont(f);
        p.drawText(body.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   QString::number(pct) + "%");

        // 枠。マスク編集中は強調表示する。
        p.setPen(QPen(editing_ ? Theme::textBright : Theme::bgButton, editing_ ? 2 : 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r);

        // 上下の三角形マーカー(▼▲)。中心x = opacity_ * width() が現在値を示す。
        p.setRenderHint(QPainter::Antialiasing);
        const qreal tw = 4.0; // 三角形の半幅
        const qreal cx = qBound(tw, opacity_ * width(), qreal(width()) - tw);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::accent);
        QPolygonF top;
        top << QPointF(cx - tw, 0.0) << QPointF(cx + tw, 0.0) << QPointF(cx, qreal(kTriH));
        p.drawPolygon(top);
        QPolygonF bottom;
        bottom << QPointF(cx - tw, qreal(height())) << QPointF(cx + tw, qreal(height()))
               << QPointF(cx, qreal(height() - kTriH));
        p.drawPolygon(bottom);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !enabled_) return;
        pressed_  = true;
        dragging_ = false;
        pressPos_ = event->pos();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!pressed_) return;
        if (!dragging_ && qAbs(event->pos().x() - pressPos_.x()) > 3)
            dragging_ = true;
        if (dragging_) {
            const float v = (float)qBound(0.0, event->pos().x() / qreal(qMax(1, width())), 1.0);
            // ドック側への反映(LayerDock::scheduleRefresh)はサムネイル再生成を伴う
            // 重い処理を避けるため80msデバウンスされている(refresh()参照)。それに
            // 頼ると三角形/数値のマーカーがドラッグに追従して見えなくなるため、
            // 見た目(opacity_)はここで即座に自前反映し、実際のドキュメントへの
            // 反映(キャンバス合成)は従来通りシグナル経由で行う。
            opacity_ = v;
            update();
            emit opacityChanged(v);
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !pressed_) return;
        pressed_ = false;
        if (!dragging_) emit clicked(); // 動かさずに離した=クリック
    }

private:
    bool   enabled_ = false;
    float  opacity_ = 1.0f;
    bool   hasMask_ = false;
    bool   editing_ = false;
    QImage maskThumb_;
    bool   pressed_ = false;
    bool   dragging_ = false;
    QPoint pressPos_;
};

// ===========================================================================
// レイヤーを「クリッピングの行」にグルーピングする
//
// scope: -1ならルート階層(トップレベル)全体、フォルダーのレイヤーindexを渡すと
// そのフォルダーの直接の中身(childCount枚)だけを対象にする(=LayerDock/
// LayerIndicatorZoneが「今開いている階層」を表示するための共通ロジック。
// ルート/フォルダーの中のどちらでもまったく同じ関数・同じ描画/ドラッグ処理を
// 使い回せるようにするための一般化)。
//
// clipping=false のレイヤーが行のroot(列0)になり、その直後に連続する
// clipping=true のレイヤーが同じ行の列1,2,...として並ぶ
// (scope範囲を下から順に辿り、非クリッピングに出会うたびに新しい行を開始する)。
// ネストしたフォルダーは、その中身(childCount枚)を展開せず1個のノードとして
// 扱う(中に入るまでは子孫を一切このリストに含めない)。
// ===========================================================================
static QVector<QVector<int>> computeRows(const CanvasDocument &doc, int scope = -1)
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

// idxが指すレイヤーの直後、かつ(フォルダーなら)その中身をすべて含んだ直後の
// flatインデックスを返す(「同じ階層で兄弟として続ける」ときの挿入/移動先境界)。
static int afterLayerBlock(const CanvasDocument &doc, int idx)
{
    if (idx < 0 || idx >= doc.layerCount()) return doc.layerCount();
    const Layer &l = doc.layers[idx];
    return idx + 1 + (l.layerType == LayerType::Folder ? l.childCount : 0);
}

// scope(フォルダーのレイヤーindex、-1ならルート)の中身の終端flatインデックス
// (該当行が見つからない/空スコープのときのデフォルト挿入位置に使う)。
static int scopeEndIndex(const CanvasDocument &doc, int scope)
{
    return (scope < 0) ? doc.layerCount() : (scope + 1 + doc.layers[scope].childCount);
}

// prefix + 数字 の形式を持つ既存レイヤー名の中から最大の数字を探し、+1(未使用な
// ら1)を付けて返す。種類ごとに独立して連番を振るため、対象を絞り込むpredを渡す
// (例: 調整レイヤーはモードにかかわらず同じ連番にしたいのでkindを見ない)。
template <typename Pred>
static QString nextNumberedName(const CanvasDocument &doc, const QString &prefix, Pred pred)
{
    const QRegularExpression re(QRegularExpression::anchoredPattern(
        QRegularExpression::escape(prefix) + "(\\d+)"));
    int maxN = 0;
    for (const Layer &l : doc.layers) {
        if (!pred(l)) continue;
        QRegularExpressionMatch m = re.match(l.name);
        if (m.hasMatch()) maxN = qMax(maxN, m.captured(1).toInt());
    }
    return prefix + QString::number(maxN + 1);
}

static QColor blendModeColor(BlendMode mode)
{
    switch (mode) {
    // 暗くする系 (赤)
    case BlendMode::Darken:
    case BlendMode::Multiply:
    case BlendMode::ColorBurn:
    case BlendMode::LinearBurn:
    case BlendMode::DarkerColor:
        return QColor("#e4043c");
    // 明るくする系 (緑)
    case BlendMode::Lighten:
    case BlendMode::Screen:
    case BlendMode::ColorDodge:
    case BlendMode::LinearDodge:
    case BlendMode::LighterColor:
        return QColor("#32e3a8");
    // コントラスト系 (オレンジ)
    case BlendMode::Overlay:
    case BlendMode::SoftLight:
    case BlendMode::HardLight:
    case BlendMode::VividLight:
    case BlendMode::LinearLight:
    case BlendMode::PinLight:
    case BlendMode::HardMix:
        return QColor("#ff9822");
    // 比較系 (紫)
    case BlendMode::Difference:
    case BlendMode::Exclusion:
    case BlendMode::Subtract:
    case BlendMode::Divide:
        return QColor("#a855f7");
    // 色相/彩度/カラー/輝度 (水色)
    case BlendMode::Hue:
    case BlendMode::Saturation:
    case BlendMode::Color:
    case BlendMode::Luminosity:
        return QColor("#22d3ee");
    // ディザ合成
    case BlendMode::Dissolve:
        return QColor("#9ca3af");
    default:
        return QColor("#2f8dff");
    }
}

// 5稜の星形ポリゴンをrに内接するよう描く(フォルダー用)。
static void drawStarShape(QPainter &p, const QRectF &r)
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

// インジケーターのドットを、レイヤーの種類に応じた図形で描く
// (通常=丸のまま、単色=四角、調整=ダイヤ、テキスト=逆三角、フォルダー=星形)。
// pにはすでにブラシ/ペンが設定済みであること。
static void drawLayerIndicatorShape(QPainter &p, const QRectF &r, LayerType type)
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
        // 調整レイヤー(ダイヤ)と紛れないよう六角形にする
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

static QString blendModeNameJa(BlendMode mode)
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

static QString adjustmentKindNameJa(AdjustmentKind kind)
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

static QString filterKindNameJa(FilterKind kind)
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

// 単色/テキスト/調整レイヤーは、実際の合成結果(単色レイヤーは常に単色、調整
// レイヤーは自分自身にはピクセルが無い)よりも、種類が一目で分かる固定のプレ
// ビューを出したほうが分かりやすいため、種類ごとに決め打ちの画像を返す。
// Normalレイヤーはこの関数を使わず、従来通りgetLayerPreview()の実際の合成結果を使う。
static QPixmap layerTypeThumbnail(const Layer &layer, const QSize &size)
{
    const LayerType type = layer.layerType;
    QPixmap pm(size);
    switch (type) {
    case LayerType::SolidColor:
        pm.fill(layer.solidColor);
        return pm;
    case LayerType::Text: {
        QImage src(":/icons/tool/text_preview.png");
        // プレビューの形に合わせて中央を切り抜く(余白を作らず全面を埋める
        // cover フィット)
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
        // フィルターレイヤーには対応するアイコンが無いので、種類ごとに
        // それらしいサムネイルを手続き的に描く。
        pm.fill(QColor(30, 30, 34));
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF c(size.width() * 0.5, size.height() * 0.5);
        switch (layer.filter.kind) {
        case FilterKind::GaussianBlur: {
            // ぼかし: 中心が濃く外へ滑らかに薄れる同心円(ガウスの断面のイメージ)
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
            // 移動ぼかし: 進行方向へ尾を引く筋(先端が濃く、後方ほど薄れる)
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
            // レンズぼかし: 大きさの異なる玉ボケ(ソフトエッジの円)を三角形に配置
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
            // モザイク: 濃淡の異なる正方形ブロックを格子状に並べる(ピクセレートの表現)
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
            // ノイズ: 中間グレーの地に白黒の粒をランダムに散らす(フィルムグレインの表現)
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
            // 色収差: R/G/Bをずらした3つの円
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

// 透過部分が分かるよう市松模様の背景に画像を重ねたサムネイルを作る
static QPixmap checkeredThumbnail(const QImage &img, const QSize &size)
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

// ===========================================================================
// LayerPickerPopup ― 行を長押し/ドラッグしたときに出す、横並びサムネイル選択UI
//
// サムネイル自体の位置は固定(行のレイヤー数が多いほど右に伸びていく。
// ドックの外(キャンバス側)にはみ出して構わない)。ドラッグすると
// サムネイルではなく、選択中を示す「枠」のほうがカーソルに追従して動く。
// フレームレスのトップレベルウィンドウとして表示し、表示・座標更新は
// すべて呼び出し側(LayerRowWidget)がドラッグ量に応じて行う、
// 純粋な描画専用ウィジェット。
// ===========================================================================
class LayerPickerPopup : public QWidget
{
public:
    static constexpr int ItemW    = 64;
    static constexpr int ThumbH   = 64;
    static constexpr int TextH    = 32;
    static constexpr int ItemH    = ThumbH + 4 + TextH;
    static constexpr int Spacing  = 16;
    static constexpr int Step     = ItemW + Spacing;
    static constexpr int Padding  = 14;

    struct ItemInfo { QPixmap thumb; QString name; QString meta; };

    explicit LayerPickerPopup(QWidget *parent = nullptr)
        : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint)
    {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
    }

    void setItems(const QVector<ItemInfo> &items) { items_ = items; }

    // ドラッグ開始時点で表示していた(=元居た)列。選択枠(ドラッグに追従して動く)とは
    // 別に、常にこの位置がどこかを分かるよう点線の枠で示す(-1なら非表示)。
    void setOriginIndex(int index)
    {
        origin_ = index;
        update();
    }

    static qreal itemCenterX(int index) { return Padding + index * Step + ItemW / 2.0; }

    // frameCenterX: 選択枠の中心のローカルX座標(ドラッグに連続して追従する、
    // スクロールオフセットを含まない論理座標)。
    // activeIndex: 現在枠が指しているレイヤーの列。
    void setFrame(qreal frameCenterX, int activeIndex)
    {
        frameCenterX_ = frameCenterX;
        active_ = activeIndex;
        update();
    }

    // ウィンドウ幅がコンテンツ全幅より狭い(=画面に収まりきらない)ときに、
    // 表示中の項目群をこの分だけ横にずらす(常に <= 0)。
    void setScrollOffset(qreal offset)
    {
        scrollOffset_ = offset;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(Theme::overlayPanelBg);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(rect(), 8, 8);

        const qreal rowTop = 0;

        for (int i = 0; i < items_.size(); i++) {
            bool active = (i == active_);
            qreal cx = itemCenterX(i) + scrollOffset_;
            QRectF thumbRect(cx - ItemW / 2.0, rowTop + Padding, ItemW, ThumbH);

            p.setPen(QPen(Theme::bgButton, 1));
            p.setBrush(Theme::bgInput);
            p.drawRoundedRect(thumbRect, 5, 5);
            p.setOpacity(active ? 1.0 : 0.55);
            p.drawPixmap(thumbRect.adjusted(2, 2, -2, -2).toRect(), items_[i].thumb);
            p.setOpacity(1.0);

            QFont f = p.font();
            f.setPointSizeF(8.0);
            p.setFont(f);
            QRectF nameRect(cx - ItemW / 2.0 - 4, rowTop + Padding + ThumbH + 3, ItemW + 8, 14);
            p.setPen(active ? Theme::textBright : Theme::textDisabled);
            p.drawText(nameRect, Qt::AlignHCenter | Qt::AlignVCenter,
                       p.fontMetrics().elidedText(items_[i].name, Qt::ElideRight, ItemW + 8));

            QRectF metaRect(cx - ItemW / 2.0 - 4, rowTop + Padding + ThumbH + 17, ItemW + 8, 14);
            p.setPen(active ? Theme::panelText : Theme::textDisabled);
            p.drawText(metaRect, Qt::AlignHCenter | Qt::AlignVCenter, items_[i].meta);
        }

        // 元居た列(ドラッグ開始時点でアクティブだった列)を、サムネイルだけを囲む
        // 点線の枠で常に示す(選択枠と重なっていても線種・大きさが違うので見分けが付く)。
        if (origin_ >= 0 && origin_ < items_.size()) {
            qreal ocx = itemCenterX(origin_) + scrollOffset_;
            QRectF originRect(ocx - ItemW / 2.0 - 3, rowTop + Padding - 3, ItemW + 6, ThumbH + 6);
            p.setPen(QPen(Theme::textBright, 2, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(originRect, 6, 6);
        }

        // サムネイルは固定。動くのはこの選択枠だけ。
        qreal frameW = ItemW + 10;
        qreal frameH = ItemH + 6;
        QRectF frame(frameCenterX_ + scrollOffset_ - frameW / 2.0, rowTop + Padding - 5, frameW, frameH);
        p.setPen(QPen(Theme::bgButton, 2.5));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(frame, 7, 7);
    }

private:
    QVector<ItemInfo> items_;
    qreal frameCenterX_ = 0;
    int   active_ = 0;
    qreal scrollOffset_ = 0;
    int   origin_ = -1;
};

// ===========================================================================
// LayerRowWidget ― クリッピングの1行ぶんの表示
//
// 行に複数レイヤー(root + クリップ)がある場合、横幅が伸びすぎないように
// サムネイルは1枚だけを重ねて表示する。行のどこをクリックしてもそのまま
// レイヤーが選択され、ダブルクリックで名前編集、長押し/ドラッグで
// LayerPickerPopup を開いて行内のレイヤーを選び直せる。
// ===========================================================================
class LayerRowWidget : public QFrame
{
    Q_OBJECT
public:
    LayerRowWidget(GLWidget *gl, QVector<int> rowLayers, QWidget *parent = nullptr)
        : QFrame(parent), glWidget(gl), rowLayers_(std::move(rowLayers))
    {
        setFrameShape(QFrame::NoFrame);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(50);

        longPressTimer = new QTimer(this);
        longPressTimer->setSingleShot(true);
        connect(longPressTimer, &QTimer::timeout, this, [this]() {
            if (rowLayers_.size() > 1) openPicker();
        });

        auto *outer = new QHBoxLayout(this);
        outer->setContentsMargins(8, 4, 8, 4);
        outer->setSpacing(6);

        // クリッピングレイヤー(root以外の列)を選択中であることを分かりやすく
        // するための矢印。root選択時は隠す(隠すとレイアウト上も幅が詰まり、
        // 縦線の位置がクリッピング選択時だけ左にずれる)。
        clipArrowLabel = new QLabel("←", this);
        clipArrowLabel->setFixedWidth(10);
        clipArrowLabel->setAlignment(Qt::AlignCenter);
        {
            QFont f = clipArrowLabel->font();
            f.setBold(true);
            clipArrowLabel->setFont(f);
        }
        clipArrowLabel->setVisible(false);
        outer->addWidget(clipArrowLabel);

        // ブレンドモードを一目で分かるようにする左端の色付き縦線
        blendColorBar = new QWidget(this);
        blendColorBar->setFixedWidth(4);
        outer->addWidget(blendColorBar);

        visibleCheck = new QCheckBox(this);
        visibleCheck->setStyleSheet(R"(
            QCheckBox {
                background: transparent;
                border: none;
            }

            QCheckBox::indicator {
                width: 18px;
                height: 18px;
                background: transparent;
                border: none;
            }

            QCheckBox::indicator:unchecked {
                image: url(:/icons/common/invisible.png);
            }
            QCheckBox::indicator:checked {
                image: url(:/icons/common/visible.png);
            }
        )");
        visibleCheck->setToolTip("表示/非表示");
        connect(visibleCheck, &QCheckBox::toggled, this, [this](bool v) {
            glWidget->document().setLayerVisible(currentLayerIndex(), v);
        });
        outer->addWidget(visibleCheck);

        thumbLabel = new QLabel(this);
        thumbLabel->setFixedSize(40, 30);
        thumbLabel->setAlignment(Qt::AlignCenter);
        outer->addWidget(thumbLabel);

        auto *info = new QVBoxLayout();
        info->setSpacing(1);

        nameEdit = new QLineEdit(this);
        nameEdit->setStyleSheet(QString("background: transparent; border: none; font-size: 10px; font-weight: bold; color: %1;")
            .arg(Theme::panelText.name()));
        nameEdit->setReadOnly(true);
        nameEdit->setFocusPolicy(Qt::NoFocus);
        nameEdit->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        connect(nameEdit, &QLineEdit::editingFinished, this, &LayerRowWidget::finishRename);

        metaLabel = new QLabel(this);
        metaLabel->setStyleSheet(QString("color: %1; font-size: 10px; background: transparent;")
            .arg(Theme::hintText.name()));

        info->addWidget(nameEdit);
        info->addWidget(metaLabel);
        outer->addLayout(info, 1);

        refreshFromDocument();
    }

    ~LayerRowWidget() override { delete picker_; }

    void setSelected(bool selected)
    {
        selected_ = selected;
        applyStyle();
    }

    // 行内で現在表示している列(行がアクティブレイヤーを含むならその列、
    // そうでなければ列0=root)
    int displayColumn() const
    {
        int active = glWidget->document().activeLayerIndex();
        int idx = rowLayers_.indexOf(active);
        return idx >= 0 ? idx : 0;
    }
    int currentLayerIndex() const { return rowLayers_[displayColumn()]; }

    void refreshFromDocument()
    {
        const CanvasDocument &doc = glWidget->document();
        int li = currentLayerIndex();
        if (li < 0 || li >= doc.layerCount()) return;
        const Layer &layer = doc.layers[li];

        QSignalBlocker b1(visibleCheck);
        visibleCheck->setChecked(layer.visible);
        if (!nameEdit->hasFocus())
            nameEdit->setText(layer.name);
        const QString modeLabel = (layer.layerType == LayerType::Adjustment)
            ? adjustmentKindNameJa(layer.adjustment.kind)
            : (layer.layerType == LayerType::Filter)
                ? filterKindNameJa(layer.filter.kind)
                : blendModeNameJa(layer.blendMode);
        metaLabel->setText(QString("%1: %2%")
            .arg(modeLabel)
            .arg((int)(layer.opacity * 100)));
        const QString blendColorName = blendModeColor(layer.blendMode).name();
        setStyleSheetIfChanged(blendColorBar, lastBlendBarQss_,
            QString("background-color: %1; border-radius: 2px;").arg(blendColorName));

        // rootではなくクリッピング列を表示中のときだけ、縦線の左に矢印を出して
        // 「ずれている」ことを分かりやすくする
        bool showClipArrow = rowLayers_.size() > 1 && displayColumn() > 0;
        clipArrowLabel->setVisible(showClipArrow);
        if (showClipArrow)
            setStyleSheetIfChanged(clipArrowLabel, lastClipArrowQss_,
                QString("color: %1; background: transparent;").arg(blendColorName));

        // アクティブレイヤーの行は(ペン等のツールがそのレイヤーのピクセルを
        // opacity等を変えずに書き換えうるため)常に再生成、それ以外の行は
        // 表示中レイヤーの番号・opacity・visible・blendMode・layerType・nameの
        // いずれも前回描画時から変わっていなければキャッシュ済みサムネイルを
        // そのまま使い回す。
        // 行ウィジェットはrebuildRows()で「rowLayers(=表示中のindex)が前回と一致する
        // 既存ウィジェット」を使い回す仕組みのため、並べ替えでちょうど同じ枚数の
        // フォルダー同士が入れ替わったようなケースでは、indexは前回と同じままその場所の
        // 中身(=別のフォルダー)だけが変わることがある。opacity/visible/blendMode/
        // layerTypeがどれも初期値のまま一致してしまうと従来のキー(li/opacity/visible/
        // blend/type)だけでは変化なしと誤判定してしまうため、ほぼ確実に異なる値を持つ
        // nameもキーに加えて誤判定を防ぐ。
        bool isActiveRow = rowLayers_.contains(doc.activeLayerIndex());
        bool sigChanged = !thumbCacheValid_
            || cachedLi_ != li
            || cachedOpacity_ != layer.opacity
            || cachedVisible_ != layer.visible
            || cachedBlend_ != layer.blendMode
            || cachedType_ != layer.layerType
            || cachedName_ != layer.name;

        if (isActiveRow || sigChanged) {
            if (layer.layerType == LayerType::SolidColor || layer.layerType == LayerType::Text
                || layer.layerType == LayerType::Adjustment || layer.layerType == LayerType::Filter
                || layer.layerType == LayerType::Folder) {
                thumbLabel->setPixmap(layerTypeThumbnail(layer, thumbLabel->size()));
            } else {
                QImage preview = glWidget->getLayerPreview(li, li, 40);
                thumbLabel->setPixmap(checkeredThumbnail(preview, thumbLabel->size()));
            }
            thumbCacheValid_ = true;
            cachedLi_ = li;
            cachedOpacity_ = layer.opacity;
            cachedVisible_ = layer.visible;
            cachedBlend_ = layer.blendMode;
            cachedType_ = layer.layerType;
            cachedName_ = layer.name;
        }

        applyStyle();
    }

    const QVector<int> &rowLayers() const { return rowLayers_; }

signals:
    void clicked(int layerIndex);
    void adjustmentLayerDoubleClicked(int layerIndex);
    void solidColorLayerDoubleClicked(int layerIndex);
    void filterLayerDoubleClicked(int layerIndex);
    void folderDoubleClicked(int layerIndex);

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            pressPos_ = event->pos();
            pickerOpen_ = false;
            if (rowLayers_.size() > 1)
                longPressTimer->start(280);
        }
        QFrame::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (rowLayers_.size() <= 1) { QFrame::mouseMoveEvent(event); return; }

        if (!pickerOpen_) {
            int dx = event->pos().x() - pressPos_.x();
            if (std::abs(dx) > 8) {
                longPressTimer->stop();
                openPicker();
            } else {
                QFrame::mouseMoveEvent(event);
                return;
            }
        } else {
            updatePickerVisual();
        }
        QFrame::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        longPressTimer->stop();
        if (pickerOpen_) {
            int col = pickerActiveCol_;
            closePicker();
            emit clicked(rowLayers_[col]);
        } else if (event->button() == Qt::LeftButton) {
            emit clicked(currentLayerIndex());
        }
        QFrame::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        longPressTimer->stop();
        // 名前編集はnameEdit欄をダブルクリックしたときだけ開始する(名前欄以外の
        // ダブルクリックは、調整レイヤーならスライダーパネルを開く、フォルダーなら
        // 中へ入る操作に使うため)。
        if (nameEdit->geometry().contains(event->pos())) {
            startRename();
        } else {
            const CanvasDocument &doc = glWidget->document();
            int li = currentLayerIndex();
            if (li >= 0 && li < doc.layerCount()) {
                LayerType t = doc.layers[li].layerType;
                if (t == LayerType::Adjustment)
                    emit adjustmentLayerDoubleClicked(li);
                else if (t == LayerType::SolidColor)
                    emit solidColorLayerDoubleClicked(li);
                else if (t == LayerType::Filter)
                    emit filterLayerDoubleClicked(li);
                else if (t == LayerType::Folder)
                    emit folderDoubleClicked(li); // 右のインジケーターのダブルクリックと同じ
            }
        }
        QFrame::mouseDoubleClickEvent(event);
    }

private:
    void openPicker()
    {
        pickerOpen_    = true;
        pickerBaseCol_ = displayColumn();
        pickerActiveCol_ = pickerBaseCol_;
        lastScrollOffset_ = 0;

        if (!picker_) picker_ = new LayerPickerPopup();

        const CanvasDocument &doc = glWidget->document();
        QVector<LayerPickerPopup::ItemInfo> items;
        items.reserve(rowLayers_.size());
        const QSize itemThumbSize(LayerPickerPopup::ItemW, LayerPickerPopup::ThumbH);
        for (int li : rowLayers_) {
            const Layer &layer = doc.layers[li];
            QPixmap thumb;
            if (layer.layerType == LayerType::SolidColor || layer.layerType == LayerType::Text
                || layer.layerType == LayerType::Adjustment || layer.layerType == LayerType::Filter
                || layer.layerType == LayerType::Folder) {
                thumb = layerTypeThumbnail(layer, itemThumbSize);
            } else {
                QImage img = glWidget->getLayerPreview(li, li, LayerPickerPopup::ItemW);
                thumb = checkeredThumbnail(img, itemThumbSize);
            }
            const QString modeLabel = (layer.layerType == LayerType::Adjustment)
                ? adjustmentKindNameJa(layer.adjustment.kind)
                : (layer.layerType == LayerType::Filter)
                    ? filterKindNameJa(layer.filter.kind)
                    : blendModeNameJa(layer.blendMode);
            LayerPickerPopup::ItemInfo item;
            item.thumb = thumb;
            item.name  = layer.name;
            item.meta  = QString("%1 %2%").arg(modeLabel).arg((int)(layer.opacity * 100));
            items.append(item);
        }
        picker_->setItems(items);
        picker_->setOriginIndex(pickerBaseCol_);

        pickerContentW_ = LayerPickerPopup::Padding * 2 + rowLayers_.size() * LayerPickerPopup::Step;
        const int h = LayerPickerPopup::Padding * 2 + LayerPickerPopup::ItemH;

        QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen = QGuiApplication::primaryScreen();
        QRect screenGeom = screen->geometry();

        // コンテンツが画面幅より広いときはウィンドウ幅を画面幅に収め、はみ出た分は
        // updatePickerVisual()側のスクロールオフセットで表示する。
        int w = qMin(pickerContentW_, screenGeom.width());
        picker_->resize(w, h);

        // ドラッグを始めた場所に依存しない固定の位置(=行の中心)に出す。ウィンドウが
        // 画面に収まりきらない場合だけ、収まる方向へずらす(qBoundによる画面内クランプ)。
        QPoint rowCenter = mapToGlobal(rect().center());
        int x = qRound(rowCenter.x() - w / 2.0);
        x = qBound(screenGeom.left(), x, screenGeom.right() - w);
        int y = rowCenter.y() - h / 2;
        y = qBound(screenGeom.top(), y, screenGeom.bottom() - h);

        picker_->move(x, y);
        picker_->show();
        updatePickerVisual();
    }

    void updatePickerVisual()
    {
        if (!picker_) return;
        qreal x0    = LayerPickerPopup::itemCenterX(0);
        qreal xLast = LayerPickerPopup::itemCenterX(rowLayers_.size() - 1);

        // 選択枠は「ドラッグ開始位置からの移動量」ではなく、マウスの現在の実際の
        // 画面座標をそのままポップアップ内の位置に変換して表示する(区切りに
        // スナップしない、項目の間の中途半端な位置になってもよい)。
        // lastScrollOffset_は直前フレームで求めたスクロール量(このフレームでは
        // まだ確定していないため近似値として使う)。mouseMoveEventのたびに
        // 呼ばれるので、実用上のズレはすぐ収束して気にならない。
        qreal mouseLocalX = QCursor::pos().x() - picker_->x();
        qreal frameCenterX = qBound(x0, mouseLocalX - lastScrollOffset_, xLast);

        int col = qRound((frameCenterX - x0) / qreal(LayerPickerPopup::Step));
        col = qBound(0, col, rowLayers_.size() - 1);
        pickerActiveCol_ = col;

        // 選択枠が画面上のマウス位置と一致するよう、コンテンツ全体をスクロール
        // させる。コンテンツがウィンドウ幅に収まっている場合はオフセット0のまま。
        // 画面端でウィンドウ位置がこれ以上動かせない場合はスクロールが頭打ちに
        // なり、選択枠がマウスより手前で止まる(項目範囲の外までは動かないため)。
        qreal desiredScroll = mouseLocalX - frameCenterX;
        qreal minScroll = qMin(0.0, qreal(picker_->width()) - pickerContentW_);
        desiredScroll = qBound(minScroll, desiredScroll, 0.0);
        lastScrollOffset_ = desiredScroll;

        picker_->setFrame(frameCenterX, col);
        picker_->setScrollOffset(desiredScroll);
    }

    void closePicker()
    {
        pickerOpen_ = false;
        if (picker_) picker_->hide();
    }

    void startRename()
    {
        nameEdit->setReadOnly(false);
        nameEdit->setFocusPolicy(Qt::StrongFocus);
        nameEdit->setAttribute(Qt::WA_TransparentForMouseEvents, false);
        nameEdit->setFocus();
        nameEdit->selectAll();
    }

    void finishRename()
    {
        if (nameEdit->isReadOnly()) return; // 普通時のeditingFinishedは無視
        glWidget->document().setLayerName(currentLayerIndex(), nameEdit->text());
        nameEdit->setReadOnly(true);
        nameEdit->setFocusPolicy(Qt::NoFocus);
        nameEdit->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        nameEdit->clearFocus();
    }

    // QWidget::setStyleSheet() は、同じ文字列を渡した場合でも QStyleSheetStyle の
    // キャッシュをグローバルに捨てて全ウィジェットのスタイルを解決し直す。
    // LayerDockは行ごとに3回(行本体・blendColorBar・clipArrowLabel)呼んでいたため、
    // 更新1回あたり「3×行数」回の全体再解決が走り、コストが行数の2乗で効いていた
    // (実測: 20行で1回8.8ms×60回=530ms、LayerDock更新の98%)。
    // 実際には色やブレンドモードが変わることは稀なので、前回適用した文字列と
    // 同じなら何もしない。見た目は既にその状態なので副作用は無い。
    static void setStyleSheetIfChanged(QWidget *w, QString &cache, const QString &qss)
    {
        if (cache == qss) return;
        cache = qss;
        w->setStyleSheet(qss);
    }

    void applyStyle()
    {
        bool multi = rowLayers_.size() > 1;
        const CanvasDocument &doc = glWidget->document();
        int li = currentLayerIndex();
        bool isFolder = (li >= 0 && li < doc.layerCount()
                          && doc.layers[li].layerType == LayerType::Folder);

        // 左端に二重枠を出すと縦線(blendColorBar/clipArrowLabel)の位置がずれて
        // しまうため、重なりを示す二重枠は右端だけに出す。
        const QString multiBorder = multi
            ? QString("border-right: 5px double %1;").arg(Theme::textDisabled.name())
            : QString();
        // フォルダー行は、他のレイヤー行と一目で見分けられるよう背景をアンバー系
        // で薄く塗る(選択中はいつも通りの選択ハイライトを優先する)。
        QString bg = selected_ ? Theme::scrollHandleHover.name()
                   : isFolder  ? QStringLiteral("rgba(245, 166, 35, 60)")
                               : QStringLiteral("transparent");
        setStyleSheetIfChanged(this, lastRowQss_, QString(
            "LayerRowWidget { background: %1; border-radius: 6px; border: 2px solid transparent; %2 }")
            .arg(bg)
            .arg(multiBorder));
    }

    GLWidget *glWidget;
    QVector<int> rowLayers_;
    bool selected_ = false;

    QLabel    *clipArrowLabel = nullptr;
    QWidget   *blendColorBar = nullptr;
    QCheckBox *visibleCheck = nullptr;
    QLabel    *thumbLabel   = nullptr;
    QLineEdit *nameEdit     = nullptr;
    QLabel    *metaLabel    = nullptr;

    // 前回 setStyleSheet() に渡した文字列(setStyleSheetIfChanged参照)
    QString lastRowQss_;
    QString lastBlendBarQss_;
    QString lastClipArrowQss_;

    QTimer *longPressTimer = nullptr;
    LayerPickerPopup *picker_ = nullptr;
    bool   pickerOpen_    = false;
    QPoint pressPos_;
    qreal  lastScrollOffset_ = 0; // updatePickerVisual()参照
    int    pickerBaseCol_ = 0;
    int    pickerActiveCol_ = 0;
    int    pickerContentW_ = 0; // ポップアップの全項目ぶんの論理幅(ウィンドウ幅より広い場合スクロールする)

    // サムネイルキャッシュ: getLayerPreview()はフルキャンバス合成を伴い重いので、
    // 前回生成時からこの行の見た目に関わる値が変わっていない場合は再生成をスキップする。
    // ただしピクセル編集ツールはopacity等を変えずにアクティブレイヤーの中身だけを
    // 書き換えるため、アクティブレイヤーを表示中の行は毎回再生成が必要。
    bool      thumbCacheValid_ = false;
    int       cachedLi_        = -1;
    float     cachedOpacity_   = -1.0f;
    bool      cachedVisible_   = true;
    BlendMode cachedBlend_     = BlendMode::Normal;
    LayerType cachedType_      = LayerType::Normal;
    QString   cachedName_;
};

// ===========================================================================
// LayerIndicatorZone ― 常時表示のクリッピング構造インジケーター
//
// リストがスクロールしても、行×列のドット配置と接続線で全体の構成を
// 常に見渡せるようにする(幅・高さいっぱいに全行を均等割りして表示するため
// スクロールしない)。ドットをクリックすると該当レイヤーを選択できる。
//
// ドットをドラッグすると、そのレイヤーを好きな行・列へ移動できる。
// ドラッグ中は、挿入先に応じてほかの行/列がよける(隙間があく)ように描画する。
// ===========================================================================
class LayerIndicatorZone : public QWidget
{
    Q_OBJECT
public:
    explicit LayerIndicatorZone(GLWidget *gl, QWidget *parent = nullptr)
        : QWidget(parent), glWidget(gl)
    {
        setMinimumWidth(90);
        setMouseTracking(false);
    }

    // タブ切替時に、表示対象のGLWidget(=キャンバス)を差し替える
    void setGLWidget(GLWidget *gl) { glWidget = gl; update(); }

    // 現在表示すべき階層(-1=ルート、それ以外はフォルダーのレイヤーindex)。
    // LayerDock::currentScope()と同じ値を渡す(computeRows()参照)。
    void setScope(int scope)
    {
        if (scope_ == scope) return;
        scope_ = scope;
        dots_.clear();
        update();
    }

signals:
    void layerClicked(int layerIndex);
    // 星形(フォルダー)のドットをダブルクリックしたときに発火する
    void folderDoubleClicked(int layerIndex);

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // レイヤーリスト側(splitter左)と背景色を揃える。QSplitterはQFrame派生の
        // ため、何もしないとQFrame用の@bgPanel@がここに透けて見えてしまう。
        p.fillRect(rect(), Theme::bgBase);

        const CanvasDocument &doc = glWidget->document();
        QVector<QVector<int>> rows = computeRows(doc, scope_);
        dots_.clear();
        if (rows.isEmpty()) return;

        const int rowCount = rows.size();
        const int active = doc.activeLayerIndex();

        // ドラッグ中に隠す(=浮かせて描く)べきレイヤー群。root列をドラッグ中なら
        // その行のクリップも含めた全体、クリップ列をドラッグ中ならそれ単体。
        QVector<int> draggedLayers;
        if (dragging_) {
            if (dragRowMode_) {
                for (const auto &r : rows) if (r.contains(dragLayer_)) { draggedLayers = r; break; }
            } else {
                draggedLayers = { dragLayer_ };
            }
        }

        HoverTarget hover;
        bool showGap = dragging_ && computeHoverTarget(dragPos_, rows, hover, dragRowMode_);

        // 行の可視位置(0=一番上)ごとに、実際に描画するY座標を計算する。
        // ドラッグ中で行挿入(新しい行としての挿入)なら、該当箇所に1行分の
        // 隙間を追加してほかの行をよけさせる。
        int slotCount = rowCount + ((showGap && hover.isRowInsert) ? 1 : 0);
        qreal rowH = qreal(height()) / qMax(1, slotCount);

        // 可視行rv(0=最前面)→ 隙間を考慮した描画スロット位置
        auto rowSlot = [&](int rv) -> int {
            if (!(showGap && hover.isRowInsert)) return rv;
            int dataRow = rowCount - 1 - rv;
            // 挿入境界hover.boundaryBより下(dataRowが小さい側)は変化なし、
            // 境界以上のdataRowを持つ行は1スロット分下にずれる。
            int gapSlotFromTop = rowCount - hover.boundaryB; // 隙間の可視スロット位置
            return (rv >= gapSlotFromTop) ? rv + 1 : rv;
        };

        QVector<QPointF> col0Centers;

        for (int rv = 0; rv < rowCount; rv++) {
            int dataRow = rowCount - 1 - rv;
            const QVector<int> &cols = rows[dataRow];
            int slot = rowSlot(rv);
            qreal cy = rowH * slot + rowH / 2.0;

            bool colGapHere = showGap && !hover.isRowInsert && hover.dataRow == dataRow;
            int maxCols = 1;
            for (const auto &r : rows) maxCols = qMax(maxCols, r.size());
            if (colGapHere) maxCols = qMax(maxCols, cols.size() + 1);
            qreal colW = qreal(width()) / maxCols;

            QPointF firstCenter, lastCenter;
            bool any = false;
            for (int c = 0; c < cols.size(); c++) {
                int slotC = (colGapHere && c >= hover.col) ? c + 1 : c;
                qreal cx = colW * slotC + colW / 2.0;
                QPointF center(cx, cy);
                if (!any) { firstCenter = center; any = true; }
                if (c == 0) col0Centers.append(center);
                lastCenter = center;
                if (dragging_ && draggedLayers.contains(cols[c])) continue; // ドラッグ中の本体はグリップ位置に描かない
                dots_.append({ center, cols[c] });
            }

            if (cols.size() > 1) {
                p.setPen(QPen(Theme::hoverBg, 2));
                p.drawLine(firstCenter, lastCenter);
            }
        }

        if (col0Centers.size() > 1) {
            p.setPen(QPen(Theme::hoverBg, 2));
            p.drawLine(col0Centers.first(), col0Centers.last());
        }

        // 挿入先の隙間を枠線で示す
        if (showGap) {
            if (hover.isRowInsert) {
                int gapSlot = rowCount - hover.boundaryB;
                QRectF gapRect(4, rowH * gapSlot + 2, width() - 8, rowH - 4);
                p.setPen(QPen(Theme::scrollHandleHover, 2, Qt::DashLine));
                p.setBrush(Theme::bgButton);
                p.drawRoundedRect(gapRect, 5, 5);
            } else {
                int rv = rowCount - 1 - hover.dataRow;
                int slot = rowSlot(rv);
                qreal cy = rowH * slot + rowH / 2.0;
                int maxCols = 1;
                for (const auto &r : rows) maxCols = qMax(maxCols, r.size());
                maxCols = qMax(maxCols, int(rows[hover.dataRow].size()) + 1);
                qreal colW = qreal(width()) / maxCols;
                qreal cx = colW * hover.col + colW / 2.0;
                qreal dotSize = qMin(qMin(rowH, colW) * 0.55, 18.0);
                QRectF gapRect(cx - dotSize / 2 - 4, cy - dotSize / 2 - 4, dotSize + 8, dotSize + 8);
                p.setPen(QPen(Theme::scrollHandleHover, 2, Qt::DashLine));
                p.setBrush(Theme::bgButton);
                p.drawRoundedRect(gapRect, 5, 5);
            }
        }

        qreal dotSizeBase = qMin(qMin(qreal(height()) / qMax(1, slotCount), qreal(width())) * 0.55, 18.0);
        for (const DotInfo &info : dots_) {
            const Layer &layer = doc.layers[info.layerIndex];
            bool selected = (info.layerIndex == active);
            QRectF r(info.center.x() - dotSizeBase / 2, info.center.y() - dotSizeBase / 2, dotSizeBase, dotSizeBase);
            p.setBrush(blendModeColor(layer.blendMode));
            p.setPen(selected ? QPen(Theme::textBright, 2) : QPen(Qt::NoPen));
            drawLayerIndicatorShape(p, r, layer.layerType);
        }

        // ドラッグ中の本体はカーソル位置に浮かせて描く。root列(行)をドラッグ中は、
        // 行に含まれるレイヤー全部を横に並べたゴーストにして「行ごと動く」ことを示す。
        if (dragging_ && dragLayer_ >= 0 && dragLayer_ < doc.layerCount()) {
            qreal ghostSize = dotSizeBase * 1.25;
            if (dragRowMode_ && draggedLayers.size() > 1) {
                qreal spacing = ghostSize * 0.7;
                qreal startX  = dragPos_.x() - spacing * (draggedLayers.size() - 1) / 2.0;
                for (int i = 0; i < draggedLayers.size(); i++) {
                    int li = draggedLayers[i];
                    if (li < 0 || li >= doc.layerCount()) continue;
                    const Layer &layer = doc.layers[li];
                    QPointF c(startX + spacing * i, dragPos_.y());
                    QRectF r(c.x() - ghostSize / 2, c.y() - ghostSize / 2, ghostSize, ghostSize);
                    p.setBrush(blendModeColor(layer.blendMode));
                    p.setPen(QPen(Theme::textBright, 2.5));
                    drawLayerIndicatorShape(p, r, layer.layerType);
                }
            } else {
                const Layer &layer = doc.layers[dragLayer_];
                QRectF r(dragPos_.x() - ghostSize / 2, dragPos_.y() - ghostSize / 2, ghostSize, ghostSize);
                p.setBrush(blendModeColor(layer.blendMode));
                p.setPen(QPen(Theme::textBright, 2.5));
                drawLayerIndicatorShape(p, r, layer.layerType);
            }
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        QPointF pos = event->position();
        qreal best = 1e9;
        int bestLayer = -1;
        for (const DotInfo &info : dots_) {
            qreal d = QLineF(pos, info.center).length();
            if (d < best) { best = d; bestLayer = info.layerIndex; }
        }
        if (bestLayer >= 0 && best < 30.0) {
            pressLayer_ = bestLayer;
            pressPos_   = pos;
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (pressLayer_ < 0) return;
        QPointF pos = event->position();
        if (!dragging_) {
            if (QLineF(pos, pressPos_).length() < 6.0) return;
            dragging_  = true;
            dragLayer_ = pressLayer_;
            // root列(一番左)をつかんでいたら、その行のクリッピングレイヤーごと
            // 移動する「行ドラッグ」モードにする(この場合、上下の並べ替えのみ許可)。
            dragRowMode_ = false;
            const CanvasDocument &doc = glWidget->document();
            QVector<QVector<int>> rows = computeRows(doc, scope_);
            for (const auto &r : rows) {
                if (r.contains(dragLayer_)) { dragRowMode_ = (r.first() == dragLayer_); break; }
            }
        }
        dragPos_ = pos;
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        if (dragging_) {
            commitDrag();
            dragging_    = false;
            dragLayer_   = -1;
            dragRowMode_ = false;
        } else if (pressLayer_ >= 0) {
            emit layerClicked(pressLayer_);
        }
        pressLayer_ = -1;
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        QPointF pos = event->position();
        qreal best = 1e9;
        int bestLayer = -1;
        for (const DotInfo &info : dots_) {
            qreal d = QLineF(pos, info.center).length();
            if (d < best) { best = d; bestLayer = info.layerIndex; }
        }
        if (bestLayer < 0 || best >= 30.0) return;
        const CanvasDocument &doc = glWidget->document();
        if (bestLayer >= doc.layerCount()) return;
        if (doc.layers[bestLayer].layerType == LayerType::Folder) {
            pressLayer_ = -1;
            emit folderDoubleClicked(bestLayer);
        }
    }

private:
    struct DotInfo { QPointF center; int layerIndex; };

    // ドラッグ中カーソルが指している挿入先。
    // isRowInsert==true: boundaryB (0..rowCount) の位置に新しい行として挿入。
    // isRowInsert==false: dataRow行の、列col(1始まり、cols.size()なら末尾)の
    //                      直前にクリップ列として挿入。
    struct HoverTarget {
        bool isRowInsert = true;
        int  boundaryB   = 0;
        int  dataRow     = 0;
        int  col         = 1;
    };

    // rowOnly==true(root列=行ドラッグ中)のときは、常に行の挿入位置(上下の並べ替え)
    // だけを返し、クリップ列への挿入は一切候補にしない。
    bool computeHoverTarget(const QPointF &pos, const QVector<QVector<int>> &rows, HoverTarget &out,
                             bool rowOnly = false) const
    {
        int rowCount = rows.size();
        if (rowCount == 0) return false;

        qreal rowH = qreal(height()) / rowCount;
        int rv = qBound(0, int(pos.y() / rowH), rowCount - 1);
        qreal rowLocalY = pos.y() - rv * rowH;
        int dataRow = rowCount - 1 - rv;

        if (rowOnly) {
            out.isRowInsert = true;
            out.boundaryB   = (rowLocalY < rowH / 2.0) ? (rowCount - rv) : (rowCount - rv - 1);
            return true;
        }

        qreal edgeMargin = rowH * 0.28;
        if (rowLocalY < edgeMargin) {
            out.isRowInsert = true;
            out.boundaryB   = rowCount - rv;
            return true;
        }
        if (rowLocalY > rowH - edgeMargin) {
            out.isRowInsert = true;
            out.boundaryB   = rowCount - rv - 1;
            return true;
        }

        int maxCols = 1;
        for (const auto &r : rows) maxCols = qMax(maxCols, r.size());
        qreal colW = qreal(width()) / maxCols;
        const QVector<int> &cols = rows[dataRow];

        int col = qRound(pos.x() / colW);
        col = qBound(1, col, cols.size());

        out.isRowInsert = false;
        out.dataRow = dataRow;
        out.col     = col;
        return true;
    }

    void commitDrag()
    {
        CanvasDocument &doc = glWidget->document();
        QVector<QVector<int>> rows = computeRows(doc, scope_);
        if (rows.isEmpty()) return;

        HoverTarget t;
        if (!computeHoverTarget(dragPos_, rows, t, dragRowMode_)) return;

        if (dragRowMode_) {
            // root列をドラッグした場合: その行(クリッピングレイヤーごと)を、
            // 上下の並べ替えとしてのみ移動する(列=クリップ関係は変えない)。
            // rootがフォルダーなら、その中身(childCount枚)も含めた1ブロックとして
            // 一緒に移動する(でないとフォルダーの中身が取り残されてしまう)。
            int fromStart = -1, count = 1;
            for (const auto &r : rows) {
                if (r.contains(dragLayer_)) {
                    fromStart = r.first();
                    count = (doc.layers[r.first()].layerType == LayerType::Folder)
                        ? 1 + doc.layers[r.first()].childCount
                        : r.size();
                    break;
                }
            }
            if (fromStart < 0) return;

            int b = t.boundaryB; // rowOnly指定によりisRowInsertは常にtrue
            int flatInsertBefore = (b <= 0) ? (scope_ < 0 ? 0 : scope_ + 1) : afterLayerBlock(doc, rows[b - 1].last());
            int toIndex = (flatInsertBefore > fromStart) ? flatInsertBefore - count : flatInsertBefore;
            if (doc.moveLayerBlock(fromStart, count, toIndex))
                doc.setActiveLayer(toIndex);
            return;
        }

        // クリップ列としての挿入(列ドラッグ)は、フォルダーへは対応しない
        // (フォルダーに対するクリッピングはまだ実装しない)。
        if (!t.isRowInsert && doc.layers[rows[t.dataRow][0]].layerType == LayerType::Folder)
            return;

        int fromIndex = dragLayer_;
        int flatInsertBefore;
        bool newClipping;

        if (t.isRowInsert) {
            int b = t.boundaryB;
            flatInsertBefore = (b <= 0) ? (scope_ < 0 ? 0 : scope_ + 1) : afterLayerBlock(doc, rows[b - 1].last());
            newClipping = false;
        } else {
            const QVector<int> &cols = rows[t.dataRow];
            int c = t.col;
            flatInsertBefore = (c < cols.size()) ? cols[c] : afterLayerBlock(doc, cols.last());
            newClipping = true;
        }

        int toIndex = (flatInsertBefore > fromIndex) ? flatInsertBefore - 1 : flatInsertBefore;
        doc.moveLayer(fromIndex, toIndex, newClipping);
        doc.setActiveLayer(toIndex);
    }

    GLWidget *glWidget;
    QVector<DotInfo> dots_;
    int scope_ = -1;

    int    pressLayer_ = -1;
    QPointF pressPos_;

    bool    dragging_    = false;
    int     dragLayer_   = -1;
    bool    dragRowMode_ = false;
    QPointF dragPos_;
};

#include "LayerDock.moc"

// ===========================================================================
// LayerDock
// ===========================================================================
LayerDock::LayerDock(GLWidget *gl, QWidget *parent)
    : QWidget(parent), glWidget(gl)
{
    setMinimumSize(MIN_WIDTH, MIN_HEIGHT);

    refreshDebounceTimer_ = new QTimer(this);
    refreshDebounceTimer_->setSingleShot(true);
    connect(refreshDebounceTimer_, &QTimer::timeout, this, &LayerDock::refresh);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(6);

    // ---- 上部コントロール(選択中レイヤーのブレンドモード/不透明度、追加/削除) ----
    auto *ctrlRow = new QHBoxLayout();
    auto *ctrlLeft = new QVBoxLayout();
    blendCombo = new QComboBox(this);
    populateBlendCombo(BlendComboMode::Blend); // 初期状態はブレンドモード一覧
    connect(blendCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        QVariant data = blendCombo->itemData(idx);
        if (!data.isValid()) return; // セパレーター行
        CanvasDocument &doc = glWidget->document();
        int active = doc.activeLayerIndex();
        if (active < 0 || active >= doc.layerCount()) return;
        if (blendComboMode_ == BlendComboMode::Adjustment) {
            // 調整レイヤー選択中は、このコンボは種類(色相・彩度・明度/明るさ・
            // コントラスト)の切り替えとして働く
            doc.layers[active].adjustment.kind = static_cast<AdjustmentKind>(data.toInt());
            refresh(); // メタ表示("色相・彩度・明度: 100%"等)を更新する
        } else if (blendComboMode_ == BlendComboMode::Filter) {
            // フィルターレイヤー選択中は、このコンボは種類(ぼかし/色収差)の
            // 切り替えとして働く。doc.onChangedを経由しない直接書き換えなので、
            // オフスクリーンの連鎖キャッシュを明示的に無効化する(そうしないと
            // 種類を変えても画面がストローク等の次の再合成まで古いままになる)。
            doc.layers[active].filter.kind = static_cast<FilterKind>(data.toInt());
            glWidget->invalidateFilterChainCache();
            glWidget->update();
            refresh();
        } else {
            BlendMode mode = static_cast<BlendMode>(data.toInt());
            doc.setLayerBlendMode(active, mode);
        }
    });
    ctrlLeft->addWidget(blendCombo, 1);
    
    auto *buttonBox = new QHBoxLayout();
    buttonBox->setSpacing(2);

    addRowBtn = new QPushButton(this);
    addRowBtn->setIcon(QIcon(":/icons/common/add_above"));
    addRowBtn->setToolTip("新規レイヤー");
    connect(addRowBtn, &QPushButton::clicked, this, &LayerDock::addRow);

    addColBtn = new QPushButton(this);
    addColBtn->setIcon(QIcon(":/icons/common/add_right"));
    addColBtn->setToolTip("クリップ追加");
    connect(addColBtn, &QPushButton::clicked, this, &LayerDock::addColumn);

    addSpecialLayerBtn = new QPushButton();
    addSpecialLayerBtn->setIcon(QIcon(":/icons/common/new"));
    addSpecialLayerBtn->setToolTip("追加");
    addSpecialLayerBtn->setStyleSheet(R"(
        QPushButton::menu-indicator {
            subcontrol-position: right;
            subcontrol-origin: padding;
            width: 0;
        }
        )");

    // 親をthisにしておく(setMenu()はメニューの所有権を取らないため、
    // 親無しで作るとLayerDock破棄後もQMenuが残る)。
    QMenu *specialLayerMenu = new QMenu(this);
    addSpecialLayerBtn->setMenu(specialLayerMenu);
    QAction *addFolderAction = specialLayerMenu->addAction("新規フォルダー");
    QAction *addSolidColorAction = specialLayerMenu->addAction("新規単色レイヤー");
    QAction *addAdjustmentAction = specialLayerMenu->addAction("新規調整レイヤー");
    QAction *addTextLayerAction = specialLayerMenu->addAction("新規テキストレイヤー");
    QAction *addFilterLayerAction = specialLayerMenu->addAction("新規フィルターレイヤー");
    connect(addFolderAction, &QAction::triggered, this, &LayerDock::addFolder);
    connect(addSolidColorAction, &QAction::triggered, this, &LayerDock::insertSolidColorLayer);
    connect(addAdjustmentAction, &QAction::triggered, this, &LayerDock::insertAdjustmentLayer);
    connect(addTextLayerAction, &QAction::triggered, this, &LayerDock::insertTextLayer);
    connect(addFilterLayerAction, &QAction::triggered, this, &LayerDock::insertFilterLayer);

    duplicateBtn = new QPushButton(this);
    duplicateBtn->setIcon(QIcon(":/icons/common/copy"));
    duplicateBtn->setToolTip("レイヤーを複製");
    connect(duplicateBtn, &QPushButton::clicked, this, &LayerDock::duplicateSelected);

    mergeBtn = new QPushButton(this);
    mergeBtn->setIcon(QIcon(":/icons/common/merge"));
    mergeBtn->setToolTip("レイヤーを結合");
    connect(mergeBtn, &QPushButton::clicked, this, &LayerDock::mergeSelected);

    deleteBtn = new QPushButton(this);
    deleteBtn->setIcon(QIcon(":/icons/common/trash"));
    deleteBtn->setToolTip("レイヤーを削除");
    connect(deleteBtn, &QPushButton::clicked, this, &LayerDock::deleteActiveLayer);

    buttonBox->addWidget(addRowBtn);
    buttonBox->addWidget(addColBtn);
    buttonBox->addWidget(addSpecialLayerBtn);
    buttonBox->addWidget(duplicateBtn);
    buttonBox->addWidget(mergeBtn);
    buttonBox->addWidget(deleteBtn);
    ctrlLeft->addLayout(buttonBox);

    ctrlRow->addLayout(ctrlLeft);

    opacityPreview = new MaskOpacityPreview(this);
    // 左右ドラッグ: 一律の不透明度(scalar)を変更する。
    connect(opacityPreview, &MaskOpacityPreview::opacityChanged, this, [this](float v) {
        CanvasDocument &doc = glWidget->document();
        int a = doc.activeLayerIndex();
        if (a < 0 || a >= doc.layerCount()) return;
        doc.setLayerOpacity(a, v);
    });
    // クリック: そのレイヤーのマスク編集モードをオン/オフ(GLWidget側で、まだ
    // マスクが無ければ白マスクを自動生成する)。
    connect(opacityPreview, &MaskOpacityPreview::clicked, this, [this]() {
        CanvasDocument &doc = glWidget->document();
        int a = doc.activeLayerIndex();
        if (a < 0 || a >= doc.layerCount()) return;
        glWidget->setEditingMaskLayer(a);
        updateControlsFromActiveLayer();
    });
    ctrlRow->addWidget(opacityPreview, 1);

    mainLayout->addLayout(ctrlRow);

    // ---- パンくずリスト(フォルダーの階層を降りているときだけ表示) ----
    breadcrumbBar_ = new QWidget(this);
    breadcrumbLayout_ = new QHBoxLayout(breadcrumbBar_);
    breadcrumbLayout_->setContentsMargins(2, 0, 2, 0);
    breadcrumbLayout_->setSpacing(2);
    breadcrumbBar_->setVisible(false);
    mainLayout->addWidget(breadcrumbBar_);

    // ---- レイヤーリスト + 常時表示インジケーター ----
    auto *splitter = new QSplitter();

    scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    listContainer = new QWidget();
    listLayout = new QVBoxLayout(listContainer);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(4);

    emptyFolderHintLabel_ = new QLabel("(空のフォルダーです)", listContainer);
    emptyFolderHintLabel_->setAlignment(Qt::AlignCenter);
    emptyFolderHintLabel_->setStyleSheet(QString("color: %1; padding: 24px 0;").arg(Theme::hintText.name()));
    emptyFolderHintLabel_->setVisible(false);
    listLayout->addWidget(emptyFolderHintLabel_);

    listLayout->addStretch();
    scrollArea->setWidget(listContainer);
    splitter->addWidget(scrollArea);

    indicatorZone = new LayerIndicatorZone(glWidget, this);
    connect(indicatorZone, &LayerIndicatorZone::layerClicked, this, &LayerDock::selectLayer);
    connect(indicatorZone, &LayerIndicatorZone::folderDoubleClicked, this, &LayerDock::enterFolder);
    splitter->addWidget(indicatorZone);

    splitter->setSizes({600, 400});

    mainLayout->addWidget(splitter, 1);

    rebuildRows();
}

void LayerDock::setGLWidget(GLWidget *gl)
{
    glWidget = gl;
    indicatorZone->setGLWidget(gl);
    // LayerRowWidgetは(サムネイルキャッシュを保持するため)自分がどのGLWidget向けに
    // 作られたかを覚えず、rebuildRows()の使い回しロジックもレイヤー構成の「形状」だけで
    // 一致判定している。そのためタブ切替でglWidget(=対象ドキュメント)が変わった際、
    // refresh()の「形状が同じなら中身だけ更新」高速パスに乗ってしまうと、古いタブに
    // 紐づいたままの行ウィジェットが新しいタブのドキュメントに対して操作され続け、
    // レイヤー数不一致によるインデックス範囲外クラッシュを招く。タブ切替時は必ず
    // 全行を作り直すことでこれを防ぐ。
    qDeleteAll(rows_);
    rows_.clear();
    folderPath_.clear();
    rebuildBreadcrumb();
    refresh();
}

void LayerDock::rebuildRows()
{
    CanvasDocument &doc = glWidget->document();
    const int scope = currentScope();
    indicatorZone->setScope(scope);

    QVector<QVector<int>> rows = computeRows(doc, scope);
    int activeIndex = doc.activeLayerIndex();
    emptyFolderHintLabel_->setVisible(insideFolder() && rows.isEmpty());

    // レイヤー構成(行数/各行の枚数)が変わるたびに全行を作り直すと、変化していない
    // 既存レイヤーぶんまでサムネイルを再生成することになり重い。rowLayers の内容が
    // 完全一致する既存ウィジェットがあればそのまま使い回す(サムネイルキャッシュごと
    // 保持される)ことで、末尾へのレイヤー追加のような典型的な操作を新規1行ぶんの
    // コストだけで済ませる。
    QVector<LayerRowWidget*> oldRows = rows_;
    QVector<bool> reused(oldRows.size(), false);
    rows_.clear();

    // rows は下から上の順で保持している。UI上は上(最前面)を一番上に表示する。
    for (int r = rows.size() - 1; r >= 0; --r) {
        LayerRowWidget *row = nullptr;
        for (int i = 0; i < oldRows.size(); i++) {
            if (!reused[i] && oldRows[i]->rowLayers() == rows[r]) {
                row = oldRows[i];
                reused[i] = true;
                break;
            }
        }
        if (row) {
            listLayout->removeWidget(row);
        } else {
            row = new LayerRowWidget(glWidget, rows[r], listContainer);
            connect(row, &LayerRowWidget::clicked, this, &LayerDock::selectLayer);
            connect(row, &LayerRowWidget::adjustmentLayerDoubleClicked, this, &LayerDock::openAdjustmentLayerEditor);
            connect(row, &LayerRowWidget::solidColorLayerDoubleClicked, this, &LayerDock::openSolidColorLayerEditor);
            connect(row, &LayerRowWidget::filterLayerDoubleClicked,     this, &LayerDock::openFilterLayerEditor);
            connect(row, &LayerRowWidget::folderDoubleClicked,          this, &LayerDock::enterFolder);
        }
        row->refreshFromDocument();
        row->setSelected(rows[r].contains(activeIndex));
        listLayout->insertWidget(listLayout->count() - 1, row);
        rows_.append(row);
    }

    for (int i = 0; i < oldRows.size(); i++)
        if (!reused[i]) oldRows[i]->deleteLater();

    updateControlsFromActiveLayer();
    indicatorZone->update();
}

void LayerDock::selectLayer(int layerIndex)
{
    glWidget->document().setActiveLayer(layerIndex);
    for (LayerRowWidget *row : rows_) {
        row->refreshFromDocument();
        row->setSelected(row->rowLayers().contains(layerIndex));
    }
    updateControlsFromActiveLayer();
    indicatorZone->update();
}

void LayerDock::updateControlsFromActiveLayer()
{
    const CanvasDocument &doc = glWidget->document();
    // キャンバスタブが1つも開いていないとき(glWidgetがMainWindowのダミーGLWidgetを
    // 指している)はlayerCount()が常に0になるため、これをそのまま「レイヤー操作不可」の
    // 判定に使う。押すとglWidget側でクラッシュしうる操作(レイヤー追加/複製/削除等)は
    // すべてここでグレーアウトする。フォルダーの中に居るかどうかに関わらず、
    // 現在のスコープに対して同じ操作が行える(computeRows()等がscope引数で
    // 階層を吸収するため、ここではhasLayerの判定だけで両階層に共通して使える)。
    bool hasLayer = doc.layerCount() > 0;
    blendCombo->setEnabled(hasLayer);
    addRowBtn->setEnabled(hasLayer);
    addColBtn->setEnabled(hasLayer);
    duplicateBtn->setEnabled(hasLayer);
    addSpecialLayerBtn->setEnabled(hasLayer);
    deleteBtn->setEnabled(hasLayer);
    if (!hasLayer) {
        opacityPreview->setState(false, 1.0f, false, false, QImage());
        mergeBtn->setEnabled(false);
        return;
    }

    const Layer &layer = doc.layers[doc.activeLayerIndex()];
    QSignalBlocker b1(blendCombo);
    if (layer.layerType == LayerType::Adjustment) {
        // 調整レイヤー選択中は、ブレンドモードの代わりに調整の種類を選ぶコンボとして使う
        populateBlendCombo(BlendComboMode::Adjustment);
        blendCombo->setCurrentIndex(blendCombo->findData((int)layer.adjustment.kind));
        blendCombo->setEnabled(true);
    } else if (layer.layerType == LayerType::Filter) {
        // フィルターレイヤー選択中は、ブレンドモードの代わりに種類(ぼかし/色収差)
        // を選ぶコンボとして使う(調整レイヤーと同じ考え方)。
        populateBlendCombo(BlendComboMode::Filter);
        blendCombo->setCurrentIndex(blendCombo->findData((int)layer.filter.kind));
        blendCombo->setEnabled(true);
    } else {
        populateBlendCombo(BlendComboMode::Blend);
        blendCombo->setCurrentIndex(blendCombo->findData((int)layer.blendMode));
        // 単色レイヤーはブレンドモードを変更できない
        blendCombo->setEnabled(layer.layerType == LayerType::Normal || layer.layerType == LayerType::Text);
    }
    // 不透明度プレビュー(=旧不透明度スライダー)の状態を反映する。マスクを持つ
    // レイヤーはGPUから濃淡プレビューを読み戻して表示、持たなければ一様グレー。
    {
        int activeIndex = doc.activeLayerIndex();
        bool hasMask  = layer.hasMask;
        bool editing  = (glWidget->editingMaskLayerIndex() == activeIndex);
        QImage maskThumb = hasMask ? glWidget->getMaskPreview(activeIndex, 96) : QImage();
        opacityPreview->setState(true, layer.opacity, hasMask, editing, maskThumb);
    }

    // フォルダーへのクリップ追加はまだ対応しない(フォルダーに対するクリッピングは
    // 未実装のためスコープ外)。
    addColBtn->setEnabled(layer.layerType != LayerType::Folder);

    // 「結合」: クリップ列(col>=1)なら常に有効(左隣に結合できる)。root列(col==0)
    // なら、自分の行にクリップがあるか、1つ下の行が存在するときだけ有効
    // (結合先が何もない、一番下の単独行のときだけ無効になる)。
    // フォルダーの場合はオーバーライドされ、「中身をすべて結合する」の意味になるため、
    // 判定基準も周囲の行の有無ではなく「中身(childCount)が1以上あるか」になる。
    QVector<QVector<int>> rows = computeRows(doc, currentScope());
    int activeIndex = doc.activeLayerIndex();
    bool canMerge = false;
    if (layer.layerType == LayerType::Folder) {
        canMerge = layer.childCount > 0;
    } else {
        for (int r = 0; r < rows.size(); r++) {
            int col = rows[r].indexOf(activeIndex);
            if (col < 0) continue;
            canMerge = (col >= 1) || (rows[r].size() > 1) || (r > 0);
            break;
        }
    }
    mergeBtn->setEnabled(canMerge);
}

// blendComboの中身を、通常のブレンドモード一覧/調整レイヤーの種類一覧/
// フィルターレイヤーの種類一覧のいずれかに入れ替える。切り替えのたびに全項目を
// 作り直すのは無駄なので、既に目的のモードになっていれば何もしない。
void LayerDock::populateBlendCombo(BlendComboMode mode)
{
    if (blendComboMode_ == mode) return;
    blendComboMode_ = mode;

    QSignalBlocker guard(blendCombo);
    blendCombo->clear();

    if (mode == BlendComboMode::Adjustment) {
        // 無料版でも常に使えるものを先頭に。トーンカーブ・グラデーションマップは
        // Pro限定だが一覧には常に出す(選んだ後にダブルクリックした時点で
        // ProFeatureDialogに案内される。フィルターコンボと同じ考え方)。
        blendCombo->addItem("色相・彩度・明度",     (int)AdjustmentKind::HueSaturation);
        blendCombo->addItem("明るさ・コントラスト", (int)AdjustmentKind::BrightnessContrast);
        blendCombo->addItem("カラーバランス",       (int)AdjustmentKind::ColorBalance);
        blendCombo->addItem("トーンカーブ",         (int)AdjustmentKind::ToneCurve);
        blendCombo->addItem("グラデーションマップ", (int)AdjustmentKind::GradientMap);
        return;
    }
    if (mode == BlendComboMode::Filter) {
        // 無料版でも常に使えるものを先頭に。色収差・レンズぼかしはPro限定だが
        // 一覧には常に出す(選んだ後にダブルクリックした時点でProFeatureDialogに
        // 案内される。フィルターメニューのPro限定項目自体が無料版にも常に
        // 見えているのと同じ)。
        blendCombo->addItem("ぼかし",     (int)FilterKind::GaussianBlur);
        blendCombo->addItem("移動ぼかし", (int)FilterKind::MotionBlur);
        blendCombo->addItem("モザイク",   (int)FilterKind::Mosaic);
        blendCombo->addItem("ノイズ",     (int)FilterKind::Noise);
        blendCombo->addItem("色収差",     (int)FilterKind::ChromaticAberration);
        blendCombo->addItem("レンズぼかし", (int)FilterKind::LensBlur);
        return;
    }

    // 一覧はブラシの合成モードコンボ(ToolPropDock)と共有する(BlendModeList.h)。
    for (const BlendModeItem &it : blendModeItems()) {
        if (it.isSeparator()) blendCombo->insertSeparator(blendCombo->count());
        else                  blendCombo->addItem(it.label, (int)it.mode);
    }
}

// レイヤーリストで調整レイヤーのプレビューをダブルクリックしたときに呼ばれる。
void LayerDock::openAdjustmentLayerEditor(int layerIndex)
{
    glWidget->editAdjustmentLayer(layerIndex);
}

// レイヤーリストで単色レイヤーのプレビューをダブルクリックしたときに呼ばれる。
void LayerDock::openSolidColorLayerEditor(int layerIndex)
{
    glWidget->editSolidColorLayer(layerIndex);
}

// レイヤーリストでフィルターレイヤーのプレビューをダブルクリックしたときに呼ばれる。
void LayerDock::openFilterLayerEditor(int layerIndex)
{
    glWidget->editFilterLayer(layerIndex);
}

// 現在のスコープ(currentScope())内で、選択中レイヤーが属する行の直後に新規
// レイヤーを挿入する場合のflat挿入位置を返す(該当行が無ければスコープの末尾)。
// 対象がフォルダーなら、その中身もすべて含んだ直後(afterLayerBlock)に挿入する
// ことで、誤ってフォルダーの中に紛れ込むことなく常に「兄弟」として積まれる。
int LayerDock::computeInsertIndexInScope() const
{
    const CanvasDocument &doc = glWidget->document();
    const int scope = currentScope();
    QVector<QVector<int>> rows = computeRows(doc, scope);
    const int activeIndex = doc.activeLayerIndex();
    for (const auto &r : rows)
        if (r.contains(activeIndex))
            return afterLayerBlock(doc, r.last());
    return scopeEndIndex(doc, scope);
}

void LayerDock::addRow()
{
    CanvasDocument &doc = glWidget->document();
    const int insertIndex = computeInsertIndexInScope();

    // キャンバス作成時に "レイヤー1" が自動追加されるため、ここでは2から始まる
    // ように既存の "レイヤーN" の最大値+1を採番する。
    const QString name = nextNumberedName(doc, "レイヤー", [](const Layer &l) {
        return l.layerType == LayerType::Normal;
    });
    glWidget->beginLayerAddUndo();
    if (!glWidget->addLayer(name, insertIndex, /*clipping=*/false, 0, 0, -1, -1, LayerType::Normal, folderPath_)) {
        glWidget->abortLayerAddUndo();
        return;
    }
    doc.setActiveLayer(insertIndex);
    glWidget->commitLayerAddUndo(insertIndex, folderPath_);
    rebuildRows();
}

void LayerDock::addColumn()
{
    CanvasDocument &doc = glWidget->document();
    if (doc.layerCount() == 0) { addRow(); return; }

    const int scope = currentScope();
    QVector<QVector<int>> rows = computeRows(doc, scope);
    int activeIndex = doc.activeLayerIndex();
    int insertIndex = activeIndex + 1;
    QString rootName;
    bool foundRow = false;
    for (const auto &r : rows) {
        int col = r.indexOf(activeIndex);
        if (col >= 0) {
            // フォルダーへのクリップ追加はまだ対応しない(addColBtn側でも無効化済み、
            // ここは誤ってショートカット等から呼ばれた場合の防御)。
            if (doc.layers[r[0]].layerType == LayerType::Folder) return;
            insertIndex = r[col] + 1;
            rootName = doc.layers[r[0]].name;
            foundRow = true;
            break;
        }
    }
    if (!foundRow) return; // アクティブレイヤーが現在のスコープに存在しない

    // クリッピング先のレイヤー名を接頭辞にした連番(例: "レイヤー1" へのクリップ
    // なら "レイヤー1_1", "レイヤー1_2", ...)にする。
    const QString name = rootName.isEmpty()
        ? QStringLiteral("クリップレイヤー")
        : nextNumberedName(doc, rootName + "_", [](const Layer &) { return true; });
    glWidget->beginLayerAddUndo();
    if (!glWidget->addLayer(name, insertIndex, /*clipping=*/true, 0, 0, -1, -1, LayerType::Normal, folderPath_)) {
        glWidget->abortLayerAddUndo();
        return;
    }
    doc.setActiveLayer(insertIndex);
    glWidget->commitLayerAddUndo(insertIndex, folderPath_);
    rebuildRows();
}

// 現在のスコープ(ルート、またはフォルダーの中)の、選択中の行の直後にフォルダーを
// 1つ挿入する。ネストしたフォルダーもこの経路で作れる(挿入先スコープの
// ancestorFolders=folderPath_をそのままCanvasDocument::addLayerへ渡すだけで、
// 新しいフォルダー自身のchildCountは0から始まる)。
void LayerDock::addFolder()
{
    CanvasDocument &doc = glWidget->document();
    const int insertIndex = computeInsertIndexInScope();

    const QString name = nextNumberedName(doc, "フォルダー", [](const Layer &l) {
        return l.layerType == LayerType::Folder;
    });
    glWidget->beginLayerAddUndo();
    if (!glWidget->addLayer(name, insertIndex, /*clipping=*/false, 0, 0, 0, 0, LayerType::Folder, folderPath_)) {
        glWidget->abortLayerAddUndo();
        return;
    }
    doc.setActiveLayer(insertIndex);
    glWidget->commitLayerAddUndo(insertIndex, folderPath_);
    rebuildRows();
}

// インジケーター上の星形(フォルダー)をダブルクリックしたときに呼ばれる。
// レイヤーリスト・インジケーターの双方を「そのフォルダーの中身だけを表示」する
// 状態に切り替える(現時点ではフォルダーは実際に中身を持たないため、常に空になる)。
void LayerDock::enterFolder(int folderLayerIndex)
{
    const CanvasDocument &doc = glWidget->document();
    if (folderLayerIndex < 0 || folderLayerIndex >= doc.layerCount()) return;
    if (doc.layers[folderLayerIndex].layerType != LayerType::Folder) return;

    folderPath_.append(folderLayerIndex);
    rebuildBreadcrumb();
    rebuildRows();
}

// パンくずリストのセグメントをクリックしたときに呼ばれる。
// keepCount段まで階層を戻る(0ならルートへ戻る)。
void LayerDock::goToBreadcrumbLevel(int keepCount)
{
    if (keepCount >= folderPath_.size()) return;
    folderPath_.resize(qMax(0, keepCount));
    rebuildBreadcrumb();
    rebuildRows();
}

// folderPath_の内容から、パンくずリストのボタン列を作り直す。
void LayerDock::rebuildBreadcrumb()
{
    QLayoutItem *item;
    while ((item = breadcrumbLayout_->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    breadcrumbBar_->setVisible(!folderPath_.isEmpty());
    if (folderPath_.isEmpty()) return;

    const CanvasDocument &doc = glWidget->document();

    auto addSegment = [&](const QString &text, int keepCount, bool isCurrent) {
        auto *btn = new QPushButton(text, breadcrumbBar_);
        btn->setFlat(true);
        btn->setCursor(isCurrent ? Qt::ArrowCursor : Qt::PointingHandCursor);
        btn->setEnabled(!isCurrent);
        btn->setStyleSheet(QString("QPushButton { color: %1; border: none; padding: 2px 4px; text-align: left; }")
            .arg((isCurrent ? Theme::textBright : Theme::panelText).name()));
        if (!isCurrent)
            connect(btn, &QPushButton::clicked, this, [this, keepCount]() { goToBreadcrumbLevel(keepCount); });
        breadcrumbLayout_->addWidget(btn);
    };

    addSegment("ルート", 0, false);
    for (int i = 0; i < folderPath_.size(); i++) {
        auto *sep = new QLabel(">", breadcrumbBar_);
        sep->setStyleSheet(QString("color: %1;").arg(Theme::hintText.name()));
        breadcrumbLayout_->addWidget(sep);

        int li = folderPath_[i];
        QString name = (li >= 0 && li < doc.layerCount()) ? doc.layers[li].name : QStringLiteral("?");
        bool isCurrent = (i == folderPath_.size() - 1);
        addSegment(name, i + 1, isCurrent);
    }
    breadcrumbLayout_->addStretch();
}

// 現在のスコープ内、選択中レイヤーのすぐ後ろに、単色(白)レイヤーを挿入する
void LayerDock::insertSolidColorLayer()
{
    CanvasDocument &doc = glWidget->document();
    const int insertIndex = computeInsertIndexInScope();

    const QString name = nextNumberedName(doc, "単色レイヤー", [](const Layer &l) {
        return l.layerType == LayerType::SolidColor;
    });
    glWidget->beginLayerAddUndo();
    if (!glWidget->addSolidColorLayer(name, insertIndex, QColor(255, 255, 255, 255), folderPath_)) {
        glWidget->abortLayerAddUndo();
        return;
    }
    doc.setActiveLayer(insertIndex);
    glWidget->commitLayerAddUndo(insertIndex, folderPath_);
    rebuildRows();
}

// 空のテキストレイヤーを作成するだけ(文字の入力はテキストツールでこのレイヤーを
// クリックしてから行う。GLWidget::startOrEditTextLayerAt参照)
void LayerDock::insertTextLayer()
{
    CanvasDocument &doc = glWidget->document();
    const int insertIndex = computeInsertIndexInScope();

    const QString name = nextNumberedName(doc, "テキストレイヤー", [](const Layer &l) {
        return l.layerType == LayerType::Text;
    });
    glWidget->beginLayerAddUndo();
    if (!glWidget->addLayer(name, insertIndex, /*clipping=*/false, 0, 0, 0, 0, LayerType::Text, folderPath_)) {
        glWidget->abortLayerAddUndo();
        return;
    }
    doc.setActiveLayer(insertIndex);
    glWidget->commitLayerAddUndo(insertIndex, folderPath_);
    rebuildRows();
}

// 調整レイヤーを作成する(デフォルトは色相・彩度・明度、全パラメータ0)。
// パネルはこの時点では開かず、レイヤーリストでプレビューをダブルクリックした
// ときに開く(GLWidget::editAdjustmentLayer参照)。種類は上部のブレンドモード
// コンボ(調整レイヤー選択中は種類選択として働く)から後で切り替えられる。
void LayerDock::insertAdjustmentLayer()
{
    CanvasDocument &doc = glWidget->document();
    const int insertIndex = computeInsertIndexInScope();

    const QString name = nextNumberedName(doc, "調整レイヤー", [](const Layer &l) {
        return l.layerType == LayerType::Adjustment;
    });
    glWidget->beginLayerAddUndo();
    if (!glWidget->addLayer(name, insertIndex, /*clipping=*/false, 0, 0, 0, 0, LayerType::Adjustment, folderPath_)) {
        glWidget->abortLayerAddUndo();
        return;
    }
    doc.layers[insertIndex].adjustment.kind = AdjustmentKind::HueSaturation;
    doc.setActiveLayer(insertIndex);
    // adjustment.kindを入れてからcommitする(Redoで種類を復元するため)
    glWidget->commitLayerAddUndo(insertIndex, folderPath_);
    rebuildRows();
}

// フィルターレイヤーを作成する(現時点では種類は色収差のみ)。調整レイヤーと同じく
// パネルはこの時点では開かず、レイヤーリストでプレビューをダブルクリックしたときに
// 開く(GLWidget::editFilterLayer参照)。
void LayerDock::insertFilterLayer()
{
    CanvasDocument &doc = glWidget->document();
    const int insertIndex = computeInsertIndexInScope();

    // 種類名を名前に含めない("フィルター1"等)。後から種類を切り替えても
    // (上部のコンボ、populateBlendCombo(Filter)参照)名前が古いままにならないため。
    const QString name = nextNumberedName(doc, "フィルター", [](const Layer &l) {
        return l.layerType == LayerType::Filter;
    });
    glWidget->beginLayerAddUndo();
    if (!glWidget->addLayer(name, insertIndex, /*clipping=*/false, 0, 0, 0, 0, LayerType::Filter, folderPath_)) {
        glWidget->abortLayerAddUndo();
        return;
    }
    // 既定はガウスぼかし(無料版でも常に使える種類)。色収差はPro限定なので、
    // 作成直後にいきなりProFeatureDialogが出るのを避ける。
    FilterParams fp;
    fp.kind = FilterKind::GaussianBlur;
    fp.blurRadiusPx = 8.0f;
    // ノイズの乱数の種はここで決めておく(後から種類をノイズへ切り替えたときのため。
    // FilterLayerEditAction::onActivateはこの値をそのまま使い続けるだけで振り直さない)。
    fp.nsSeed = QRandomGenerator::global()->generate();
    doc.layers[insertIndex].filter = fp;
    doc.setActiveLayer(insertIndex);
    glWidget->commitLayerAddUndo(insertIndex, folderPath_);
    rebuildRows();
}

void LayerDock::deleteActiveLayer()
{
    CanvasDocument &doc = glWidget->document();
    if (doc.layerCount() <= 1) return; // 最後の1枚は消せない
    glWidget->removeLayer(doc.activeLayerIndex(), folderPath_);
    rebuildRows();
}

// 選択中レイヤーの列(root/クリップ)に応じて複製処理を振り分ける。
// ・root列(col==0): 選択中レイヤーが属する行を、クリッピング構成ごと丸ごと
//   1つ上に複製する(クリップが無ければ選択中レイヤーのみの複製になる)
// ・クリップ列(col>=1): 選択中レイヤーを、同じ行の右隣にクリップ列として複製する
void LayerDock::duplicateSelected()
{
    CanvasDocument &doc = glWidget->document();
    QVector<QVector<int>> rows = computeRows(doc, currentScope());
    int activeIndex = doc.activeLayerIndex();
    if (activeIndex < 0) return;

    for (const auto &r : rows) {
        int col = r.indexOf(activeIndex);
        if (col < 0) continue;

        if (col == 0 && doc.layers[r[0]].layerType == LayerType::Folder) {
            // フォルダーの「複製」は中身(ネストも含む)ごと丸ごと複製する
            // オーバーライド。挿入位置は通常のroot列複製と同じく直後(=1つ上の行)。
            int insertAt = afterLayerBlock(doc, r.last());
            int newFolderIndex = duplicateFolderDeep(r[0], insertAt);
            if (newFolderIndex < 0) return;
            doc.setActiveLayer(newFolderIndex);
        } else if (col == 0) {
            // 元の行の直後(上)へ、列の並び順を保ったまま複製していく。
            // 複製の挿入先は常に既存レイヤーより後ろなので、rの各要素の
            // インデックスは複製が進んでも変わらない。
            int insertAt = afterLayerBlock(doc, r.last());
            int newActiveIndex = insertAt;
            for (int i = 0; i < r.size(); i++) {
                glWidget->beginLayerAddUndo();
                int newIndex = glWidget->duplicateLayer(r[i], insertAt + i, folderPath_);
                if (newIndex < 0) { glWidget->abortLayerAddUndo(); return; }
                glWidget->commitLayerAddUndo(newIndex, folderPath_, /*duplicateSourceIndex=*/r[i]);
                if (i == col) newActiveIndex = newIndex;
            }
            doc.setActiveLayer(newActiveIndex);
        } else {
            glWidget->beginLayerAddUndo();
            int newIndex = glWidget->duplicateLayer(activeIndex, activeIndex + 1, folderPath_);
            if (newIndex < 0) { glWidget->abortLayerAddUndo(); return; }
            doc.setLayerClipping(newIndex, true); // 右隣は必ずクリップ列になる
            doc.setActiveLayer(newIndex);
            // clippingを立ててからcommitする(Redoで復元されるのはcommit時点の状態)
            glWidget->commitLayerAddUndo(newIndex, folderPath_, /*duplicateSourceIndex=*/activeIndex);
        }
        rebuildRows();
        return;
    }
}

// sourceFolderIndexの中身(ネストしたフォルダーも含む)を丸ごと複製し、insertAtへ
// 挿入する。元の階層構造をそのまま再現するため、複製元の各要素を先頭(フォルダー
// 自身)から順になぞりながら、複製先の新しいフォルダーindexへの対応表を作り、
// 各要素が実際に属していた祖先フォルダー(複製後のindexに読み替えたもの)を
// ancestorFoldersとしてduplicateLayerへ渡す。
int LayerDock::duplicateFolderDeep(int sourceFolderIndex, int insertAt)
{
    CanvasDocument &doc = glWidget->document();
    const int count = 1 + doc.layers[sourceFolderIndex].childCount;

    QHash<int, int> oldToNew;
    // 元配列上での祖先チェーン(範囲判定にのみ使う。複製処理中はsourceFolderIndex
    // より前の実データなので不変)。
    QVector<int> oldStack{ sourceFolderIndex };

    for (int k = 0; k < count; k++) {
        const int srcIdx = sourceFolderIndex + k;

        // oldStackを、srcIdxが実際に含まれる範囲まで閉じる
        while (oldStack.size() > 1) {
            int top = oldStack.last();
            int topEnd = top + 1 + doc.layers[top].childCount;
            if (srcIdx < topEnd) break;
            oldStack.removeLast();
        }

        // oldStack(祖先の元index列、srcIdx自身は除く)を複製後のindexへ読み替える
        QVector<int> ancestors = folderPath_;
        for (int old : oldStack) {
            if (old == srcIdx) continue; // k==0(フォルダー自身)のときはoldStack==[srcIdx]
            ancestors.append(oldToNew.value(old));
        }

        glWidget->beginLayerAddUndo();
        int newIdx = glWidget->duplicateLayer(srcIdx, insertAt + k, ancestors);
        if (newIdx < 0) { glWidget->abortLayerAddUndo(); return -1; }
        glWidget->commitLayerAddUndo(newIdx, ancestors, /*duplicateSourceIndex=*/srcIdx);
        oldToNew.insert(srcIdx, newIdx);

        if (doc.layers[srcIdx].layerType == LayerType::Folder && doc.layers[srcIdx].childCount > 0)
            oldStack.append(srcIdx);
    }
    return oldToNew.value(sourceFolderIndex, -1);
}

// 選択中レイヤーの列(root/クリップ)に応じて結合処理を振り分ける。
// ・クリップ列(col>=1): 選択中レイヤーを、同じ行で1つ左(直前)の列に結合する
// ・root列(col==0): 選択中レイヤーが属する行を、クリッピング構成ごと丸ごと
//   1つ下の行へ結合する(mergeRootRow参照)
void LayerDock::mergeSelected()
{
    CanvasDocument &doc = glWidget->document();
    QVector<QVector<int>> rows = computeRows(doc, currentScope());
    int activeIndex = doc.activeLayerIndex();

    for (const auto &r : rows) {
        int col = r.indexOf(activeIndex);
        if (col < 0) continue;

        if (col >= 1) {
            int survivor = r[col - 1];
            int victim   = r[col];
            int newIndex = glWidget->mergeLayers(survivor, victim, folderPath_);
            if (newIndex >= 0) {
                doc.setActiveLayer(newIndex);
                rebuildRows();
            }
        } else if (doc.layers[activeIndex].layerType == LayerType::Folder) {
            // フォルダーの「結合」は行そのものではなく、フォルダーの中身をすべて
            // 1枚のレイヤーへまとめる操作としてオーバーライドする。
            mergeFolderContents(activeIndex);
        } else {
            mergeRootRow(activeIndex);
        }
        return;
    }
}

// rootIndexが属する行(一番左の列)を結合する。
// ・自分の行にクリッピングがかかっていれば、それをすべてrootへ結合する
//   (1つ下の行には触れない)
// ・クリッピングが無ければ、1つ下の行のrootとだけ結合する。1つ下の行に
//   クリッピングがかかっていても無視し、rootどうしだけを結合する
//   (結果、その分クリッピングがずれてキャンバスの見た目は変わりうるが、それでよい)
void LayerDock::mergeRootRow(int rootIndex)
{
    CanvasDocument &doc = glWidget->document();
    const int scope = currentScope();
    QVector<QVector<int>> rows = computeRows(doc, scope);
    int r = -1;
    for (int i = 0; i < rows.size(); i++) if (!rows[i].isEmpty() && rows[i][0] == rootIndex) { r = i; break; }
    if (r < 0) return;

    if (rows[r].size() > 1) {
        // 同じ行のクリップ列は常にrootIndexより大きいインデックスなので、
        // 結合してもrootIndex自体はずれない。
        int activeIndex = rootIndex;
        for (;;) {
            QVector<QVector<int>> rows2 = computeRows(doc, scope);
            int r2 = -1;
            for (int i = 0; i < rows2.size(); i++) if (!rows2[i].isEmpty() && rows2[i][0] == activeIndex) { r2 = i; break; }
            if (r2 < 0 || rows2[r2].size() <= 1) break;
            int victim = rows2[r2][1];
            if (glWidget->mergeLayers(activeIndex, victim, folderPath_) < 0) return;
        }
        doc.setActiveLayer(activeIndex);
        rebuildRows();
        return;
    }

    if (r == 0) return; // 一番下の行(結合先が無い)
    int below = rows[r - 1][0];
    int newIndex = glWidget->mergeLayers(below, rootIndex, folderPath_);
    if (newIndex < 0) return;
    doc.setActiveLayer(newIndex);
    rebuildRows();
}

// folderIndexの直接の中身をすべて1枚のNormalレイヤーへ結合し、フォルダーの
// マーカー自体は(結合後のレイヤーを残したまま)取り除く。ネストしたフォルダーが
// 混ざっている場合や、Normal以外の種類(単色/調整/テキスト)が混ざっている場合は
// mergeLayers()自体がそれらを結合できないため、何もしない(現状では未対応)。
void LayerDock::mergeFolderContents(int folderIndex)
{
    CanvasDocument &doc = glWidget->document();
    const int childCount = doc.layers[folderIndex].childCount;
    if (childCount <= 0) return; // 空フォルダーは結合対象が無い

    QVector<int> children;
    for (int i = folderIndex + 1; i < folderIndex + 1 + childCount; i++) {
        if (doc.layers[i].layerType != LayerType::Normal) return;
        children.append(i);
    }
    if (children.isEmpty()) return;

    // 下から順に1つ上へ結合していく。結合のたびにvictim(survivor+1)が消えて
    // それより上のレイヤーが1つ繰り上がるため、次の相手は常にsurvivor+1のまま。
    int survivor = children[0];
    for (int k = 1; k < children.size(); k++) {
        int newIndex = glWidget->mergeLayers(survivor, survivor + 1, folderPath_ + QVector<int>{ folderIndex });
        if (newIndex < 0) return;
        survivor = newIndex;
    }

    // 実レイヤーが1枚だけ残ったfolderIndexのマーカーを、中身(survivor)を巻き込まず
    // 取り除く(includeContents=false)。マーカーはsurvivorの直前(folderIndex ==
    // children[0]-1 == survivor-1)にあるので、除去後survivorは1つ繰り上がる。
    if (!glWidget->removeLayer(folderIndex, folderPath_, /*includeContents=*/false)) return;

    doc.setActiveLayer(folderIndex); // マーカー除去でsurvivorがちょうどfolderIndexの位置に来る
    rebuildRows();
}

void LayerDock::scheduleRefresh()
{
    // ストローク確定の連打や不透明度スライダーのドラッグ等でlayersChanged()が
    // 短時間に連続発火しても、都度refresh()(フルキャンバス合成を伴いうる重い
    // サムネイル再生成)を実行せず、一定時間発火が止まってから最後の1回だけ
    // まとめて実行する。
    refreshDebounceTimer_->start(80);
}

void LayerDock::refresh()
{
    // ストローク中/ペンのホバー中は、アクティブレイヤーのサムネイル再生成
    // (タイルのGPU→CPU読み戻しを伴う)を先送りする(NavigatorDock::refreshと
    // 同じ理由: 文字書きの連打中に次のペンダウンを塞がない)。
    if (glWidget && glWidget->isGLReady() && glWidget->isInkingBusy()) {
        refreshDebounceTimer_->start(150);
        return;
    }

    CanvasDocument &doc = glWidget->document();
    QVector<QVector<int>> rows = computeRows(doc, currentScope());

    // 行の枚数、または行ごとの列数(クリップの追加/削除)が変わっていたら
    // 行ウィジェットを作り直す。それ以外はスライダー操作中のフォーカスが
    // 飛ばないよう、各行の中身だけを更新する。
    bool shapeChanged = (rows.size() != rows_.size());
    if (!shapeChanged) {
        for (int i = 0; i < rows_.size(); i++) {
            // rows_ は上(最前面)から並んでいるので、対応する rows のインデックスは逆順
            int dataRow = rows_.size() - 1 - i;
            if (rows_[i]->rowLayers().size() != rows[dataRow].size()) {
                shapeChanged = true;
                break;
            }
        }
    }

    if (shapeChanged) {
        rebuildRows();
        return;
    }

    int activeIndex = doc.activeLayerIndex();
    for (LayerRowWidget *row : rows_) {
        row->refreshFromDocument();
        row->setSelected(row->rowLayers().contains(activeIndex));
    }
    updateControlsFromActiveLayer();
    indicatorZone->update();
}
