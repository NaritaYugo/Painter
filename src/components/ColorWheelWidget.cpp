#include "components/ColorWheelWidget.h"
#include "components/ThemeColors.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QtMath>
#include <QSignalBlocker>
#include <cmath>

// ===========================================================================
// OKLCH 色変換
// ===========================================================================

bool ColorWheelWidget::inGamutFast(float L, float C, float cos_h, float sin_h)
{
    float a = C * cos_h, b = C * sin_h;
    float l = L + 0.3963377774f*a + 0.2158037573f*b;
    float m = L - 0.1055613458f*a - 0.0638541728f*b;
    float s = L - 0.0894841775f*a - 1.2914855480f*b;
    float l_ = l*l*l, m_ = m*m*m, s_ = s*s*s;
    float r =  4.0767416621f*l_ - 3.3077115913f*m_ + 0.2309699292f*s_;
    if (r < -0.0001f || r > 1.0001f) return false;
    float g = -1.2684380046f*l_ + 2.6097574011f*m_ - 0.3413193965f*s_;
    if (g < -0.0001f || g > 1.0001f) return false;
    float bv= -0.0041960863f*l_ - 0.7034186147f*m_ + 1.7076147010f*s_;
    return (bv >= -0.0001f && bv <= 1.0001f);
}

void ColorWheelWidget::oklchToLinearSrgb(float L, float C,
                                          float cos_h, float sin_h,
                                          float &r, float &g, float &b)
{
    float a  = C * cos_h, bv = C * sin_h;
    float l  = L + 0.3963377774f*a + 0.2158037573f*bv;
    float m  = L - 0.1055613458f*a - 0.0638541728f*bv;
    float s  = L - 0.0894841775f*a - 1.2914855480f*bv;
    float l_ = l*l*l, m_ = m*m*m, s_ = s*s*s;
    r =  4.0767416621f*l_ - 3.3077115913f*m_ + 0.2309699292f*s_;
    g = -1.2684380046f*l_ + 2.6097574011f*m_ - 0.3413193965f*s_;
    b = -0.0041960863f*l_ - 0.7034186147f*m_ + 1.7076147010f*s_;
}

float ColorWheelWidget::linearToGamma(float c)
{
    c = qBound(0.0f, c, 1.0f);
    return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f/2.4f) - 0.055f;
}

QRgb ColorWheelWidget::oklchToQRgb(float L, float C, float hue_deg)
{
    float h_rad = hue_deg * (float)M_PI / 180.0f;
    float r, g, b;
    oklchToLinearSrgb(L, C, std::cos(h_rad), std::sin(h_rad), r, g, b);
    return qRgb((int)(linearToGamma(r)*255.0f),
                (int)(linearToGamma(g)*255.0f),
                (int)(linearToGamma(b)*255.0f));
}

// カラーモード+モニターキャリブレーションによる見た目だけの変換。
// render.fragの最終合成結果に対する変換(CanvasWidget.cpp paintGL/render.frag)と
// 同じ式・同じ適用順序(カラーモード→明るさ・コントラスト→CMY)を使い、
// キャンバスの見え方とカラーサークルの見え方を一致させる。実際に選ばれる色
// (pickColor/color()の戻り値)はこの関数を通さないため、塗る色そのものには
// 影響しない。
QRgb ColorWheelWidget::applyDisplayPreview(QRgb rgb) const
{
    float r = qRed(rgb)   / 255.0f;
    float g = qGreen(rgb) / 255.0f;
    float b = qBlue(rgb)  / 255.0f;

    if (m_colorMode == ColorMode::GrayscaleLuminance) {
        float y = 0.299f*r + 0.587f*g + 0.114f*b;
        r = g = b = y;
    } else if (m_colorMode == ColorMode::GrayscaleLightness) {
        float maxc = qMax(r, qMax(g, b));
        float minc = qMin(r, qMin(g, b));
        r = g = b = (maxc + minc) * 0.5f;
    } else if (m_colorMode == ColorMode::CMYK) {
        float c = 1.0f - r, m = 1.0f - g, y = 1.0f - b;
        float absorbedR = 0.87f*c + 0.10f*m + 0.03f*y;
        float absorbedG = 0.09f*c + 0.86f*m + 0.05f*y;
        float absorbedB = 0.03f*c + 0.18f*m + 0.79f*y;
        r = qBound(0.0f, 1.0f - absorbedR, 1.0f);
        g = qBound(0.0f, 1.0f - absorbedG, 1.0f);
        b = qBound(0.0f, 1.0f - absorbedB, 1.0f);
    }

    // モニターキャリブレーション(CanvasWidget.cpp paintGLと同じ換算式、
    // common.glslのadjustBrightnessContrast/adjustColorBalanceと同じ計算)。
    const float calBrightness = m_calBrightness / 200.0f;
    const float calContrast   = 1.0f + m_calContrast / 100.0f;
    r = (r - 0.5f) * calContrast + 0.5f + calBrightness;
    g = (g - 0.5f) * calContrast + 0.5f + calBrightness;
    b = (b - 0.5f) * calContrast + 0.5f + calBrightness;
    r = qBound(0.0f, r, 1.0f);
    g = qBound(0.0f, g, 1.0f);
    b = qBound(0.0f, b, 1.0f);

    r = qBound(0.0f, r - m_calCyan    / 100.0f, 1.0f);
    g = qBound(0.0f, g - m_calMagenta / 100.0f, 1.0f);
    b = qBound(0.0f, b - m_calYellow  / 100.0f, 1.0f);

    return qRgb((int)qBound(0.0f, r*255.0f, 255.0f),
                (int)qBound(0.0f, g*255.0f, 255.0f),
                (int)qBound(0.0f, b*255.0f, 255.0f));
}

float ColorWheelWidget::mhToOklchHue(float mh) const
{
    float hue;
    if (mh < 0.5f) {
        // Path2: Yellow → Blue (OKLCH Hue +方向)
        hue = H_YELLOW + (mh / 0.5f) * PATH2_SPAN;
    } else {
        // Path1: Blue → Yellow (OKLCH Hue +方向でラップ)
        hue = H_BLUE + ((mh - 0.5f) / 0.5f) * PATH1_SPAN;
    }
    hue = std::fmod(hue, 360.0f);
    if (hue < 0.0f) hue += 360.0f;
    return hue;
}

float ColorWheelWidget::oklchHueToMh(float hue_deg) const
{
    // Path2: hue ∈ [H_YELLOW, H_YELLOW+PATH2_SPAN] = [109, 264]
    float p2_start = H_YELLOW;
    float p2_end   = H_YELLOW + PATH2_SPAN; // = 264 = H_BLUE
    if (hue_deg >= p2_start && hue_deg <= p2_end) {
        return ((hue_deg - p2_start) / PATH2_SPAN) * 0.5f;
    }
    // Path1: hue ∈ [H_BLUE, H_BLUE+PATH1_SPAN] mod 360
    //   H_BLUE=264, 264+205=469 → ラップで 469-360=109 = H_YELLOW
    //   [264, 360) and [0, 109]
    float p1_hue; // H_BLUEからの距離
    if (hue_deg >= H_BLUE) {
        p1_hue = hue_deg - H_BLUE;
    } else {
        p1_hue = (360.0f - H_BLUE) + hue_deg;
    }
    return 0.5f + (p1_hue / PATH1_SPAN) * 0.5f;
}

void ColorWheelWidget::srgbToOklch(float r, float g, float b,
                                    float &L, float &C, float &hue_deg)
{
    // ガンマ除去 → linear sRGB
    auto toLinear = [](float c) -> float {
        c = qBound(0.0f, c, 1.0f);
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    float rl = toLinear(r), gl = toLinear(g), bl = toLinear(b);

    // linear sRGB → OKLab (Björn Ottosson)
    float lc = 0.4122214708f*rl + 0.5363325363f*gl + 0.0514459929f*bl;
    float mc = 0.2119034982f*rl + 0.6806995451f*gl + 0.1073969566f*bl;
    float sc = 0.0883024619f*rl + 0.2817188376f*gl + 0.6299787005f*bl;

    float l_ = std::cbrt(lc);
    float m_ = std::cbrt(mc);
    float s_ = std::cbrt(sc);

    float lab_L =  0.2104542553f*l_ + 0.7936177850f*m_ - 0.0040720468f*s_;
    float lab_a =  1.9779984951f*l_ - 2.4285922050f*m_ + 0.4505937099f*s_;
    float lab_b =  0.0259040371f*l_ + 0.7827717662f*m_ - 0.8086757660f*s_;

    // OKLab → OKLCH
    L       = lab_L;
    C       = std::sqrt(lab_a*lab_a + lab_b*lab_b);
    hue_deg = std::atan2(lab_b, lab_a) * 180.0f / (float)M_PI;
    if (hue_deg < 0.0f) hue_deg += 360.0f;
}


int ColorWheelWidget::dirFromMh(float mh) const
{
    // 右半分(Path2, Green): dir=-1
    // 左半分(Path1, Red):   dir=+1
    return (mh < 0.5f) ? -1 : 1;
}

// ===========================================================================
// OKLCHテーブル構築
// maxChromaTable[h] と lBoundsTable[h][x] を事前計算
// ===========================================================================

void ColorWheelWidget::buildOklchTables()
{
    for (int h = 0; h < 360; h++) {
        float h_rad = h * (float)M_PI / 180.0f;
        float cos_h = std::cos(h_rad), sin_h = std::sin(h_rad);

        // 各Hueの最大Chroma
        float maxC = 0.0f;
        for (float L = 0.0f; L <= 1.0f; L += 0.02f) {
            float C = 0.0f, step = 0.1f;
            while (step > 0.002f) {
                if (inGamutFast(L, C+step, cos_h, sin_h)) C += step;
                else step /= 2.0f;
            }
            if (C > maxC) maxC = C;
        }
        m_maxChromaTable[h] = maxC;

        // 各X(TABLE_W分)のL境界
        int offset = h * TABLE_W * 2;
        for (int x = 0; x < TABLE_W; x++) {
            float C_norm = (float)x / (TABLE_W - 1);
            float C = C_norm * maxC;
            float max_L = -1.0f, min_L = -1.0f;
            for (float L = 1.0f; L >= 0.0f; L -= 0.005f) {
                if (inGamutFast(L, C, cos_h, sin_h)) {
                    if (max_L < 0.0f) max_L = L;
                    min_L = L;
                }
            }
            m_lBoundsTable[offset + x*2]   = (max_L < 0.0f) ? 0.0f : max_L;
            m_lBoundsTable[offset + x*2+1] = (min_L < 0.0f) ? 0.0f : min_L;
        }
    }
    m_tableReady = true;
}

float ColorWheelWidget::getMaxChroma(float hue_deg) const
{
    hue_deg = std::fmod(hue_deg, 360.0f);
    if (hue_deg < 0.0f) hue_deg += 360.0f;
    int h = (int)std::round(hue_deg) % 360;
    return m_maxChromaTable[h];
}

void ColorWheelWidget::getLBounds(float hue_deg, int x_table,
                                   float &b_max, float &b_min) const
{
    hue_deg = std::fmod(hue_deg, 360.0f);
    if (hue_deg < 0.0f) hue_deg += 360.0f;
    int h = (int)std::round(hue_deg) % 360;
    int offset = h * TABLE_W * 2;
    b_max = m_lBoundsTable[offset + x_table*2];
    b_min = m_lBoundsTable[offset + x_table*2+1];
}

// ===========================================================================
// コンストラクタ / レイアウト
// ===========================================================================

float ColorWheelWidget::s_hueTwist = ColorWheelWidget::HUE_TWIST_DEFAULT;
QVector<ColorWheelWidget *> ColorWheelWidget::s_instances;

// 環境設定「色相ツイスト」の変更を、生きている全カラーサークルへ反映する。
// スクエアの描画色はツイスト量に依存する(buildSquareImage参照)ので作り直しが要る。
// 表示中のものだけ色を通知し直すのは、同じ位置でもツイスト量が変わると実際の色が
// 変わるため(通知しないと画面の見た目と描画色がずれる)。非表示のポップアップから
// 通知すると、開いてもいないピッカーが現在色を上書きしてしまうので対象外にする。
void ColorWheelWidget::setHueTwist(float degPerL)
{
    const float v = qBound((float)HUE_TWIST_MIN, degPerL, (float)HUE_TWIST_MAX);
    if (s_hueTwist == v) return;
    s_hueTwist = v;

    for (ColorWheelWidget *w : s_instances) {
        w->buildSquareImage();
        w->updateControls();
        w->update();
        if (w->isVisible()) w->emitColor();
    }
}

ColorWheelWidget::ColorWheelWidget(QWidget *parent) : QWidget(parent)
{
    s_instances.append(this);
    setMinimumSize(MIN_W, MIN_W + SLIDERS_H);
    updateLayoutVars();

    // OKLCHテーブルを構築（起動時1回）
    buildOklchTables();

    // --- 下部レイアウト ---
    vl = new QVBoxLayout(this); {
        spaser = new QSpacerItem(1, height() - SLIDERS_H - 10);
        vl->addItem(spaser);

        QHBoxLayout *infoBox = new QHBoxLayout(); {
            infoBox->addSpacing(width());
            QVBoxLayout *info = new QVBoxLayout(); {
                rgbLabel = new QLabel("#000000");
                rgbLabel->setStyleSheet("color: #888;");
                rgbLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                info->addWidget(rgbLabel);

                hsvLabel = new QLabel("H:100 S:100 V:100");
                hsvLabel->setStyleSheet("color: #888;");
                hsvLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                info->addWidget(hsvLabel);
            }
            infoBox->addLayout(info);
        }
        vl->addLayout(infoBox);

        // 色相ツイスト(旧「D」スライダー)は環境設定へ移動したのでここには置かない。
        // ColorWheelWidget::setHueTwist() 参照。

        QHBoxLayout *alphaRow = new QHBoxLayout(); {
            QLabel *alphaLabel = new QLabel("A"); {
                alphaLabel->setFixedWidth(SLIDER_LABEL_W);
            }
            alphaRow->addWidget(alphaLabel);

            m_alphaValueLabel = new QLabel("100%"); {
                m_alphaValueLabel->setFixedWidth(SLIDER_LABEL_W);
                m_alphaValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            }

            m_alphaSlider = new QSlider(Qt::Horizontal, this); {
                m_alphaSlider->setRange(0, 100);
                m_alphaSlider->setValue(100);
                connect(m_alphaSlider, &QSlider::valueChanged, this,
                    [this](int v) {
                        m_alpha = v / 100.0f;
                        // 透明色のときアルファは「消す強さ」。表示の文言も変わるので
                        // ラベル類はまとめて updateControls() で作り直す
                        // (透明色は解除しない)。
                        updateControls();
                        update();
                        emitColor();
                    }
                );
            }
            alphaRow->addWidget(m_alphaSlider);
            alphaRow->addWidget(m_alphaValueLabel);
        }
        vl->addLayout(alphaRow);

        vl->addStretch();
    }

    buildRingImage();
    buildSquareImage();
    updateControls();
}

ColorWheelWidget::~ColorWheelWidget()
{
    s_instances.removeOne(this);
}

QSize ColorWheelWidget::SizeHint() const
{
    return QSize(BASE_W, BASE_W + SLIDERS_H);
}

QRect ColorWheelWidget::squareRect() const
{
    int half = sq_size / 2;
    return QRect(cx - half, cy - half, sq_size, sq_size);
}

// リングの外接円の45°方向、左下の隅。現在の色のプレビュー(兼、透明色の切り替えボタン)。
QRect ColorWheelWidget::swatchRect() const
{
    const float inv_sqrt2 = 0.7071f;
    // 小さすぎるとクリック/視認が厳しいので下限を設ける。
    int   w     = qMax(16, qRound(r_out * (1.0f - inv_sqrt2)));
    float edgeX = cx - (r_out + 4) * inv_sqrt2;
    float edgeY = cy + (r_out + 4) * inv_sqrt2;
    return QRect(qRound(edgeX) - w, qRound(edgeY), w, w);
}

// 透過を表す市松模様。色は先端画像プレビュー・レイヤープレビュー・キャンバス背景と
// 共通のもの(ThemeColors)を使う。
void ColorWheelWidget::fillChecker(QPainter &p, const QRect &r)
{
    p.save();
    p.setClipRect(r);
    const int cs = 5;
    for (int bx = r.left(); bx < r.right(); bx += cs)
        for (int by = r.top(); by < r.bottom(); by += cs) {
            const int ww = qMin(cs, r.right()  - bx);
            const int hh = qMin(cs, r.bottom() - by);
            p.fillRect(bx, by, ww, hh,
                       ((bx/cs + by/cs) % 2 == 0) ? Theme::checkerDark : Theme::checkerLight);
        }
    p.restore();
}

// ===========================================================================
// リング画像構築
// OKLCH Hue をリング位置に伸縮してマッピング、deltaシフトなし(L=1固定)
// ===========================================================================

void ColorWheelWidget::buildRingImage()
{
    updateLayoutVars();
    if (!m_tableReady) return;

    m_ringImage = QImage(r_out * 2, r_out * 2, QImage::Format_ARGB32_Premultiplied);
    m_ringImage.fill(Qt::transparent);

    QPainter p(&m_ringImage);
    p.setRenderHint(QPainter::Antialiasing, true);

    // リング形状クリップ
    QPainterPath clip;
    clip.addEllipse(QPointF(r_out, r_out), (float)r_out, (float)r_out);
    QPainterPath inner;
    inner.addEllipse(QPointF(r_out, r_out), (float)r_in, (float)r_in);
    clip -= inner;

    // スリット (m_h=0.0/1.0 = Yellow 真上、不連続点を明示)
    float slitW = 2.0f;
    QPainterPath slit;
    slit.addRect((float)r_out - slitW/2.0f, 0, slitW, (float)r_out * 2.0f);
    clip -= slit;

    p.setClipPath(clip);

    // コニカルグラデーション: Qt は真右=0°、反時計回り正
    // 真上=90°から開始、時計回りにm_hが増加
    // grad の stop値 t ∈ [0,1] → 角度 = 90° - t*360° (Qt座標)
    // → m_h = t で t=0:真上(Yellow), t=0.5:真下(Blue)
    QConicalGradient grad(QPointF(r_out, r_out), 270.0f);
    const int steps = 720;
    for (int i = 0; i <= steps; ++i) {
        float mh  = (float)i / steps; // 0〜1
        float hue = -mhToOklchHue(mh);
        // リング表示用: L=1.0で最大輝度の色(三角形外でシフトなし、L固定)
        // ただし高L域はほぼ白なので、maxChromaの60%程度でリング色を表現
        float maxC = getMaxChroma(hue);
        float C    = maxC * 0.85f;
        float L    = 0.75f; // 視認しやすい輝度
        QRgb rgb = applyDisplayPreview(oklchToQRgb(L, C, hue));
        // Qt ConicalGradient: stop=0 が開始角(90°=真上), 時計回り増加
        grad.setColorAt((double)mh, QColor(rgb));
    }

    p.fillRect(0, 0, r_out*2, r_out*2, grad);
}

// ===========================================================================
// スクエア画像構築 (HTMLのdrawLCPlane相当)
//
//  X軸: C_norm (0=左=無彩, 1=右=maxChroma)
//  Y軸: L      (0=下=暗,   1=上=明)
//
//  三角形内部: そのHue, L, C_norm*maxChroma をそのまま描画
//  上余白(L > b_max): Hueを黄色方向にシフト (dist = L_plot - b_max, 絶対ΔL)
//  下余白(L < b_min): Hueを青方向にシフト   (dist = b_min - L_plot, 絶対ΔL)
//  シフト量: dist * s_hueTwist (°/ΔL)
// ===========================================================================

void ColorWheelWidget::buildSquareImage()
{
    if (!m_tableReady) return;

    m_squareImage = QImage(sq_size, sq_size, QImage::Format_RGB32);

    float hue_base = mhToOklchHue(m_h);
    int   dir      = dirFromMh(m_h);

    for (int px = 0; px < sq_size; px++) {
        // C_normをTABLE_Wにマップしてテーブル参照
        float C_norm   = (float)px / (sq_size - 1);
        int   x_table  = qBound(0, (int)std::round(C_norm * (TABLE_W - 1)), TABLE_W - 1);

        float b_max, b_min;
        getLBounds(hue_base, x_table, b_max, b_min);

        QRgb *col = reinterpret_cast<QRgb*>(m_squareImage.bits()) + px; // 列ポインタは後でy分加算

        for (int py = 0; py < sq_size; py++) {
            float L_plot = 1.0f - (float)py / (sq_size - 1); // 上=1.0, 下=0.0

            float draw_H, draw_L, draw_C;

            if (L_plot > b_max) {
                // 上余白: 絶対ΔLに線形比例して黄色方向にシフト
                float dist = L_plot - b_max;
                draw_H = hue_base + (float)dir * dist * s_hueTwist;
                draw_L = L_plot;
            } else if (L_plot < b_min) {
                // 下余白: 絶対ΔLに線形比例して青方向にシフト
                float dist = b_min - L_plot;
                draw_H = hue_base - (float)dir * dist * s_hueTwist;
                draw_L = L_plot;
            } else {
                // 三角形内部
                draw_H = hue_base;
                draw_L = L_plot;
            }

            draw_H = std::fmod(draw_H, 360.0f);
            if (draw_H < 0.0f) draw_H += 360.0f;

            draw_C = C_norm * getMaxChroma(draw_H);

            QRgb rgb = applyDisplayPreview(oklchToQRgb(draw_L, draw_C, draw_H));
            // scanLine経由で書き込み
            reinterpret_cast<QRgb*>(m_squareImage.scanLine(py))[px] = rgb;
        }
        (void)col;
    }
}

// ===========================================================================
// paintEvent
// ===========================================================================

void ColorWheelWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // ---- LCスクエア ----
    QRect sqR = squareRect();
    p.drawImage(sqR, m_squareImage);

    // スクエアマーカー (C_norm=m_c, L=m_l)
    {
        float mx = sqR.left() + m_c * sqR.width();
        float my = sqR.top()  + (1.0f - m_l) * sqR.height();
        QPointF mp(mx, my);
        p.setPen(QPen(Qt::white, 2.0));
        p.drawEllipse(mp, (float)MARKER_R, (float)MARKER_R);
        p.setPen(QPen(QColor(0, 0, 0, 90), 1.0));
        p.drawEllipse(mp, (float)MARKER_R, (float)MARKER_R);
    }

    // ---- 色相リング ----
    p.drawImage(cx - r_out, cy - r_out, m_ringImage);

    // リングマーカー (m_h → 角度)
    {
        // m_h=0 → 真上(90°), 時計回り増加
        float deg = 90.0f - m_h * 360.0f; // Qt角度系(反時計回り正)から変換
        float rad = deg * (float)M_PI / 180.0f;
        float rm  = (r_in + r_out) * 0.5f;
        float mx  = cx + rm * std::cos(rad);
        float my  = cy - rm * std::sin(rad); // Qtはy軸下向き
        QPointF mp(mx, my);

        p.setPen(QPen(Qt::white, 2.5));
        p.drawEllipse(mp, (float)MARKER_R, (float)MARKER_R);
        p.setPen(QPen(QColor(0, 0, 0, 100), 1.0));
        p.drawEllipse(mp, (float)MARKER_R, (float)MARKER_R);
    }

    // ---- プレビュースウォッチ (左下) ----
    // 現在の色の確認と、「通常色 / 透明色」の切り替えボタンを兼ねる
    // (透明色を選べるインスタンスのみ。setTransparentSelectable参照)。
    {
        const QRect sw = swatchRect();
        fillChecker(p, sw);

        // 透明色のときは市松模様だけを見せる(何も塗られないことがそのまま伝わる)
        if (!m_transparent) {
            QColor swColor = currentColor();
            swColor = QColor(applyDisplayPreview(swColor.rgb()));
            swColor.setAlphaF(m_alpha);
            p.fillRect(sw, swColor);
        }
        // 透明色のときは、市松模様だけの表示が「アルファ0の色」と紛らわしいので
        // 枠を強調して選択中であることを示す。
        if (m_transparent) {
            p.setPen(QPen(QColor(255, 255, 255, 230), 2));
            p.drawRect(sw.adjusted(1, 1, -1, -1));
        } else {
            p.setPen(QPen(QColor(0, 0, 0, 80), 1));
            p.drawRect(sw);
        }
    }
}

// ===========================================================================
// マウスイベント
// ===========================================================================

void ColorWheelWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    QPoint pt = e->pos();

    // 左下のスウォッチは「通常色 / 透明色」の切り替えボタンを兼ねる。
    // もう一度押すと元の色へ戻る(色そのものは保持している)。
    if (m_transparentSelectable && swatchRect().contains(pt)) {
        setTransparent(!m_transparent);
        emit transparentChanged(m_transparent);
        return;
    }

    float dx = pt.x() - cx, dy = pt.y() - cy;
    float r2 = dx*dx + dy*dy;

    const bool onRing   = (r2 >= (float)r_in*r_in && r2 <= (float)r_out*r_out);
    const bool onSquare = squareRect().contains(pt);
    // 実際の色を選んだのなら透明色は解除する(アルファスライダーは対象外。
    // 透明色のときアルファは「消す強さ」なので、そのまま調整できる必要がある)。
    if ((onRing || onSquare) && m_transparent) {
        setTransparent(false);
        emit transparentChanged(false);
    }

    if (onRing) {
        m_drag = Drag::Ring;
        applyHFromPoint(pt);
    } else if (onSquare) {
        m_drag = Drag::Square;
        applyCLFromPoint(pt);
    }
}

void ColorWheelWidget::mouseMoveEvent(QMouseEvent *e)
{
    if      (m_drag == Drag::Ring)   applyHFromPoint(e->pos());
    else if (m_drag == Drag::Square) applyCLFromPoint(e->pos());
}

void ColorWheelWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) m_drag = Drag::None;
}

void ColorWheelWidget::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    updateLayoutVars();
    buildRingImage();
    buildSquareImage();
    spaser->changeSize(1, height() - SLIDERS_H + 4);
    vl->invalidate();
    update();
}

// ===========================================================================
// ハンドリングヘルパー
// ===========================================================================

void ColorWheelWidget::applyHFromPoint(const QPoint &pt)
{
    float dx  = pt.x() - cx;
    float dy  = pt.y() - cy; // 下向き正

    // atan2で角度取得(右=0, 反時計回り正)、真上基準に変換
    float angle_rad = std::atan2(-dy, dx); // 真上基準に-dy
    float deg_from_top = 90.0f - (angle_rad * 180.0f / (float)M_PI);
    // deg_from_top: 真上=0, 時計回り正
    // → m_h: 時計回りで0〜1
    float mh = std::fmod(deg_from_top / 360.0f, 1.0f);
    if (mh < 0.0f) mh += 1.0f;

    // スリット付近(m_h ≈ 0 or 1)は端にクランプして0.0に統一
    m_h = qBound(0.0f, mh, 0.9999f);

    buildSquareImage();
    updateControls();
    update();
    emitColor();
}

void ColorWheelWidget::applyCLFromPoint(const QPoint &pt)
{
    QRect r = squareRect();
    m_c = qBound(0.0f, (float)(pt.x() - r.left()) / r.width(),  1.0f);
    m_l = qBound(0.0f, 1.0f - (float)(pt.y() - r.top()) / r.height(), 1.0f);

    updateControls();
    update();
    emitColor();
}

QColor ColorWheelWidget::currentColor() const
{
    if (!m_tableReady) return Qt::black;

    float hue_base = mhToOklchHue(m_h);
    int   dir      = dirFromMh(m_h);

    // スクエアの現在位置のbounds
    int x_table = qBound(0, (int)std::round(m_c * (TABLE_W - 1)), TABLE_W - 1);
    float b_max, b_min;
    getLBounds(hue_base, x_table, b_max, b_min);

    float draw_H, draw_L, draw_C;
    if (m_l > b_max) {
        float dist = m_l - b_max;
        draw_H = hue_base + (float)dir * dist * s_hueTwist;
        draw_L = m_l;
    } else if (m_l < b_min) {
        float dist = b_min - m_l;
        draw_H = hue_base - (float)dir * dist * s_hueTwist;
        draw_L = m_l;
    } else {
        draw_H = hue_base;
        draw_L = m_l;
    }
    draw_H = std::fmod(draw_H, 360.0f);
    if (draw_H < 0.0f) draw_H += 360.0f;
    draw_C = m_c * getMaxChroma(draw_H);

    QRgb rgb = oklchToQRgb(draw_L, draw_C, draw_H);
    QColor col(rgb);
    col.setAlphaF(m_alpha);
    return col;
}

void ColorWheelWidget::updateControls()
{
    const int alphaPct = qRound(m_alpha * 100);
    {
        // スライダーを外から動かした場合(設定の復元・スポイト)にも
        // 右の「NN%」表示を合わせる。
        QSignalBlocker blAlpha(m_alphaSlider);
        m_alphaSlider->setValue(alphaPct);
    }
    m_alphaValueLabel->setText(QStringLiteral("%1%").arg(alphaPct));

    QColor color = currentColor();
    if (m_transparent) {
        // 透明色のときはRGBに意味が無い。アルファは「消す強さ」として効き続けるので、
        // スライダーは触れるままにしておく。
        rgbLabel->setText(QStringLiteral("透明色"));
        hsvLabel->setText(QStringLiteral("消す強さ %1%").arg(qRound(m_alpha * 100)));
        return;
    }
    rgbLabel->setText(QString("%1").arg(color.name()));
    hsvLabel->setText(QString("H:%1 S:%2 V:%3")
        .arg(color.hue()).arg(color.saturation()).arg(color.value())
    );
}

void ColorWheelWidget::setTransparentSelectable(bool on)
{
    if (m_transparentSelectable == on) return;
    m_transparentSelectable = on;
    if (!on && m_transparent) m_transparent = false;
    update();
}

void ColorWheelWidget::setTransparent(bool on)
{
    if (m_transparent == on) return;
    m_transparent = on;
    updateControls();
    update();
}

void ColorWheelWidget::emitColor()
{
    emit colorChanged(currentColor());
}


// ===========================================================================
// 公開API
// ===========================================================================

QColor ColorWheelWidget::color() const
{
    return currentColor();
}

void ColorWheelWidget::setColor(const QColor &color)
{
    // QColorのHSV→OKLCH近似変換
    // OKLCH変換は重いので、HSVのHue→OKLCH Hue近似でリング位置を決める
    float h, s, v, a;
    color.getHsvF(&h, &s, &v, &a);

    // HSV Hue(0〜1, 0=赤=0°) → OKLCH Hue(°) の近似
    // 正確な変換はsRGB→OKLCH変換が必要だが、ここでは線形近似
    float hue_deg_approx = h * 360.0f;
    m_h    = oklchHueToMh(hue_deg_approx);
    m_c    = s;
    m_l    = v;
    m_alpha = a;

    buildSquareImage();
    updateControls();
    update();
}

void ColorWheelWidget::updateLayoutVars()
{
    bbox_size = qMin(width(), height() - SLIDERS_H);
    r_out     = (int)(bbox_size * 0.45f);
    r_in      = (int)(bbox_size * 0.35f);
    cx        = width() / 2;
    cy        = (height() - SLIDERS_H) / 2;
    sq_size   = (int)(r_in * 1.414f);
}

void ColorWheelWidget::setColorMode(ColorMode m)
{
    if (m_colorMode == m) return;
    m_colorMode = m;
    buildRingImage();
    buildSquareImage();
    update();
}

void ColorWheelWidget::setCalibration(int brightness, int contrast, int cyan, int magenta, int yellow)
{
    if (m_calBrightness == brightness && m_calContrast == contrast && m_calCyan == cyan
        && m_calMagenta == magenta && m_calYellow == yellow) return;
    m_calBrightness = brightness;
    m_calContrast   = contrast;
    m_calCyan       = cyan;
    m_calMagenta    = magenta;
    m_calYellow     = yellow;
    buildRingImage();
    buildSquareImage();
    update();
}

void ColorWheelWidget::pickColor(const QColor &color)
{
    // --- 1. RGBA → OKLCH ---
    float sr = (float)color.redF();
    float sg = (float)color.greenF();
    float sb = (float)color.blueF();
    float sa = (float)color.alphaF();

    float L, C, hue_deg;
    srgbToOklch(sr, sg, sb, L, C, hue_deg);

    // --- 2. Hue → m_h (不均等なPath配置を加味) ---
    m_h = oklchHueToMh(hue_deg);

    // --- 3. C → C_norm (そのHueのmaxChromaで正規化) ---
    float maxC  = getMaxChroma(hue_deg);
    float c_norm = (maxC > 0.0001f) ? C / maxC : 0.0f;

    // --- 4. 三角形外クランプ ---
    // sRGB gamut内の色であれば三角形内に収まるはずだが、
    // 浮動小数点誤差や境界付近のために念のためクランプする
    c_norm = qBound(0.0f, c_norm, 1.0f);
    int   x_table = qBound(0, (int)std::round(c_norm * (TABLE_W - 1)), TABLE_W - 1);
    float b_max, b_min;
    getLBounds(hue_deg, x_table, b_max, b_min);
    L = qBound(b_min, L, b_max);

    m_l = L;
    m_c = c_norm;
    m_alpha = sa;

    // --- 5. UI更新 (Dスライダーは変更しない) ---
    buildSquareImage();
    updateControls(); // アルファスライダーと%表示、色/透明色のラベル
    if (!m_transparent) {
        // ラベルだけは「拾った色そのもの」を出す(currentColor()は
        // OKLCHテーブルを往復するぶん1程度ずれることがあるため)。
        rgbLabel->setText(QString("%1").arg(color.name()));
        hsvLabel->setText(QString("H:%1 S:%2 V:%3")
            .arg(color.hue()).arg(color.saturation()).arg(color.value())
        );
    }
    update();
    emitColor();
}
