#pragma once

#include "tools/core/ToolConfig.h"

#include <QWidget>
#include <QImage>
#include <QColor>
#include <QSlider>
#include <QSpacerItem>
#include <QVBoxLayout>
#include <QVector>

// ===========================================================================
// ColorWheelWidget
//
// 色相リング(OKLCH Hue) + LCスクエア + アルファスライダー
//
// ColorCircleDock(ドック側)とSolidColorPickerPanel(単色レイヤーのポップアップ)
// の両方から使われる、OKLCHベースのカラーサークルの実体。見た目・挙動は完全に
// 共通で、埋め込み先(ドック/ポップアップ)側の事情はこのクラスには持ち込まない。
//
// 座標系:
//   m_h   :  0.0 〜 1.0  リング上の正規化位置
//             0.0 = 真上 = Yellow(H=109°)
//             0.5 = 真下 = Blue  (H=264°)
//             時計回りで増加
//             右半分(0〜0.5): Path2 Green側, 左半分(0.5〜1): Path1 Red側
//   m_c   :  0.0 〜 1.0  C_norm (正規化彩度, 左=0=無彩, 右=1=そのHueの最大彩度)
//   m_l   :  0.0 〜 1.0  L (輝度, 下=0=暗, 上=1=明)
//   m_alpha: 0.0 〜 1.0
//   色相ツイスト(s_hueTwist): Hueシフト量(°/ΔL)。上余白(→Yellow)・下余白(→Blue)共通。
//     以前はウィジェット下部の「D」スライダーで instance ごとに持っていたが、
//     色そのものではなく「カラーサークルの振る舞い」の設定なので、環境設定
//     (SettingsDlg「色相ツイスト」)へ移動してアプリ全体で1つの値を共有する。
// ===========================================================================
class ColorWheelWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ColorWheelWidget(QWidget *parent = nullptr);
    ~ColorWheelWidget() override;

    void   setColor(const QColor &color);
    QColor color() const;
    void pickColor(const QColor &color);

    // 「透明色」(塗るのではなく消す色)を選べるようにするか。真にすると、
    // 左下のプレビュースウォッチがクリックで通常色/透明色を切り替えるボタンになる。
    // 既定はoff(単色レイヤーの色ポップアップなど、消す概念が無い用途では切り替えない)。
    void setTransparentSelectable(bool on);
    bool isTransparent() const { return m_transparent; }
    // 外から状態を合わせる(スポイトで透明色を拾ったとき等)。シグナルは出さない。
    void setTransparent(bool on);

    // 色相ツイスト(全インスタンス共通)。環境設定の値をMainWindowから流し込む。
    // 起動時とSettingsDlgのOK直後に呼ばれ、生きている全カラーサークルの
    // スクエア画像を作り直して表示中のものは新しい色を通知する。
    static float hueTwist() { return s_hueTwist; }
    static void  setHueTwist(float degPerL);
    static constexpr float HUE_TWIST_DEFAULT = 20.0f;
    static constexpr int   HUE_TWIST_MIN     = -60;
    static constexpr int   HUE_TWIST_MAX     = 60;

    // 表示上の見た目だけを変えるカラーモード(RGB/CMYK擬似/グレースケール)。
    // リング・スクエア・プレビュースウォッチの描画色にだけ適用し、pickColor()/
    // color()が返す実際の色(実際にキャンバスへ塗られる色)は変更しない。
    void setColorMode(ColorMode m);

    // モニターキャリブレーション(明るさ・コントラスト・CMY)。カラーモード変換の
    // 後、パイプラインの一番最後に適用する。値域は-100〜100(CalibrationConfigと同じ)。
    void setCalibration(int brightness, int contrast, int cyan, int magenta, int yellow);

signals:
    void colorChanged(const QColor &color);
    // 「透明色」のオン/オフが変わった。色そのものは colorChanged 側で通知され続ける
    // (透明色のときもアルファ=消す強さとして使うため)。
    void transparentChanged(bool on);

protected:
    void paintEvent(QPaintEvent *event)     override;
    void mousePressEvent(QMouseEvent *e)    override;
    void mouseMoveEvent(QMouseEvent *e)     override;
    void mouseReleaseEvent(QMouseEvent *e)  override;
    void resizeEvent(QResizeEvent *e)       override;

private:
    // --- 色状態 ---
    float m_h     = 0.0f;   // 0.0 〜 1.0 (リング位置)
    float m_c     = 0.0f;   // 0.0 〜 1.0 (C_norm)
    float m_l     = 0.5f;   // 0.0 〜 1.0 (L)
    float m_alpha = 1.0f;
    // 「透明色」を選んでいるか。色そのもの(m_h/m_c/m_l)は保持したままなので、
    // 解除すれば元の色に戻る。m_alphaは透明色のときは「消す強さ」として使われる。
    bool  m_transparent = false;
    bool  m_transparentSelectable = false;
    // 色相ツイスト(°/ΔL)。環境設定由来でアプリ全体共通のため static。
    // 生きているインスタンスはsetHueTwist()から一斉に更新するので、その一覧も持つ。
    static float s_hueTwist;
    static QVector<ColorWheelWidget *> s_instances;
    ColorMode m_colorMode = ColorMode::RGB;
    // モニターキャリブレーション値(-100〜100)。CalibrationConfigと同じ範囲。
    int m_calBrightness = 0;
    int m_calContrast   = 0;
    int m_calCyan       = 0;
    int m_calMagenta    = 0;
    int m_calYellow     = 0;

    // --- OKLCH定数 ---
    static constexpr float H_YELLOW = 109.0f;
    static constexpr float H_BLUE   = 264.0f;
    // Path1(左半分, Red側):  Blue→(+方向)→Yellow, 距離 (109-264+360)%360 = 205°
    static constexpr float PATH1_SPAN = 205.0f;
    // Path2(右半分, Green側): Yellow→(+方向)→Blue, 距離 (264-109) = 155°
    static constexpr float PATH2_SPAN = 155.0f;

    // --- OKLCHテーブル(Hue軸: 1°刻み360エントリ) ---
    float m_maxChromaTable[360] = {};
    // [h][x*2+0]=max_L, [h][x*2+1]=min_L, xはテーブル用固定幅 TABLE_W
    static constexpr int TABLE_W = 300;
    float m_lBoundsTable[360 * TABLE_W * 2] = {};
    bool  m_tableReady = false;

    // --- レイアウト用 ---
    static constexpr int MIN_W          = 150;
    static constexpr int BASE_W         = 220;
    static constexpr int SLIDERS_H      = 30; // 下部スライダー1行ぶん(アルファのみ)
    static constexpr int SLIDER_LABEL_W = 40;
    static constexpr int MARKER_R       = 5;

    int bbox_size = 0;
    int r_out = 0;
    int r_in  = 0;
    int cx    = 0;
    int cy    = 0;
    int sq_size = 0;

    // --- キャッシュ ---
    QImage m_ringImage;
    QImage m_squareImage;

    // --- ドラッグ状態 ---
    enum class Drag { None, Ring, Square };
    Drag m_drag = Drag::None;

    // --- UI部品 ---
    QSlider     *m_alphaSlider = nullptr;
    // アルファスライダー右の「NN%」表示。スライダーを外から動かした場合
    // (設定の復元・スポイト)にも更新する必要があるのでメンバに持つ。
    QLabel      *m_alphaValueLabel = nullptr;
    QSpacerItem *spaser        = nullptr;
    QVBoxLayout *vl            = nullptr;

    QLabel *rgbLabel = nullptr;
    QLabel *hsvLabel = nullptr;

    // --- ヘルパー ---
    QRect squareRect() const;
    // 左下のプレビュースウォッチ。現在の色を市松模様の上に出す表示であり、
    // 透明色を選べる場合は「通常色 / 透明色」を切り替えるボタンも兼ねる。
    QRect swatchRect() const;
    // 市松模様(透過を表す背景)を矩形いっぱいに敷く
    static void fillChecker(QPainter &p, const QRect &r);
    void  updateLayoutVars();
    QSize SizeHint() const;

    void buildOklchTables();
    void buildRingImage();
    void buildSquareImage();


    // リング位置 m_h ↔ OKLCH Hue(°) の変換
    float mhToOklchHue(float mh) const;
    float oklchHueToMh(float hue_deg) const;
    void srgbToOklch(float r, float g, float b, float &L, float &C, float &hue_deg);
    // m_h から dir (+1 or -1) を取得
    int   dirFromMh(float mh) const;

    // OKLCH → linear sRGB
    static bool  inGamutFast(float L, float C, float cos_h, float sin_h);
    static void  oklchToLinearSrgb(float L, float C, float cos_h, float sin_h,
                                   float &r, float &g, float &b);
    static float linearToGamma(float c);
    // カラーモード(CMYK擬似/グレースケール)+モニターキャリブレーションによる
    // 見た目だけの変換。render.fragの最終合成結果に対する変換と同じ式・同じ順序
    // (カラーモード→明るさ・コントラスト→CMY)を使う(CanvasWidget.cpp/render.frag参照)。
    QRgb applyDisplayPreview(QRgb rgb) const;
    // OKLCH → QRgb (クランプ込み)
    static QRgb  oklchToQRgb(float L, float C, float hue_deg);

    // テーブル補間
    float getMaxChroma(float hue_deg) const;
    void  getLBounds(float hue_deg, int x_table, float &b_max, float &b_min) const;

    QColor currentColor() const;

    void applyHFromPoint(const QPoint &p);
    void applyCLFromPoint(const QPoint &p);

    void updateControls();
    void emitColor();
};
