#include "docks/ToolPropertyDock.h"
#include "components/CollapsibleSection.h"
#include "components/ThemeColors.h"
#include "document/BlendModeList.h"
#include "components/ImagePresetPicker.h"
#include "dialogs/ToneCurveEditor.h"
#include "tools/core/BrushTipPresets.h"
#include "tools/core/PaperTexPresets.h"
#include "tools/core/PressureCurve.h"
#include "tools/core/ToolRegistry.h"

#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QRadioButton>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QtMath>
#include <QScrollArea>
#include <QFileDialog>
#include <QMessageBox>
#include <QPixmap>
#include <QIcon>
#include <QPainter>
#include <QSettings>

using ToolProperty::SettingId;
using ToolProperty::SectionDef;
using ToolProperty::ToolPageDef;

// ===========================================================================
// 画像(先端画像・紙質テクスチャ)のプレビュー用ボタン
// ---------------------------------------------------------------------------
// QIcon任せの表示だと透過部分が背景色に埋もれて見えづらい上、正方形でない
// 画像の収まり方も分かりにくいため、自前で背景に市松模様を敷いた上で、
// 縦横比を保ったまま「長いほうの辺がプレビュー領域にちょうど収まる」大きさに
// 縮小して中央に描画する。
//
// 紙質テクスチャは「目の細かさ」そのものが違いなので、縮小すると全部同じ灰色に
// 潰れてしまう。setCropToFill(true) にすると縮小せず中央を等倍で切り出す
// (ImagePresetPicker::ThumbMode と同じ考え方)。
// ===========================================================================
class ImagePreviewButton : public QPushButton
{
public:
    explicit ImagePreviewButton(QWidget *parent = nullptr) : QPushButton(parent)
    {
        setAttribute(Qt::WA_Hover); // hover状態が変わった際にpaintEventを再度呼んでもらうため
    }

    void setPreviewPixmap(const QPixmap &pm)
    {
        m_pixmap = pm;
        update();
    }

    void setCropToFill(bool on) { m_crop = on; }

    // style.qssのQPushButton共通ルール(padding: 6px 16px;等)が乗ると、
    // QPushButton::sizeHint()がその分大きく(横長に)計算されてしまい、
    // setFixedSize()を呼んでいても再polish時に上書きされることがある。
    // 正方形のプレビューにしたいので、sizeHint自体を固定してしまう。
    QSize sizeHint() const override { return QSize(48, 48); }
    QSize minimumSizeHint() const override { return QSize(48, 48); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);

        const QRect outer = rect().adjusted(0, 0, -1, -1);

        // 背景(市松模様)
        p.fillRect(rect(), QColor(0x33, 0x33, 0x33));
        const QRect inner = outer.adjusted(1, 1, -1, -1);
        const int checker = 6;
        for (int y = inner.top(); y < inner.bottom(); y += checker) {
            for (int x = inner.left(); x < inner.right(); x += checker) {
                bool dark = ((x - inner.left()) / checker + (y - inner.top()) / checker) % 2 == 0;
                p.fillRect(QRect(x, y, checker, checker).intersected(inner),
                           dark ? Theme::checkerDark : Theme::checkerLight);
            }
        }

        // 画像(長いほうの辺を基準に、全体が収まるサイズへ縮小して中央配置)。
        // 切り出しモードでは縮小せず中央のinnerぶんだけを等倍で描く。
        if (!m_pixmap.isNull()) {
            if (m_crop && m_pixmap.width() >= inner.width() && m_pixmap.height() >= inner.height()) {
                p.drawPixmap(inner, m_pixmap,
                             QRect((m_pixmap.width()  - inner.width())  / 2,
                                   (m_pixmap.height() - inner.height()) / 2,
                                   inner.width(), inner.height()));
            } else {
                QSize target = m_pixmap.size().scaled(inner.size(), Qt::KeepAspectRatio);
                QPixmap scaled = m_pixmap.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                QPoint topLeft = inner.center() - QPoint(scaled.width() / 2, scaled.height() / 2);
                p.drawPixmap(topLeft, scaled);
            }
        }

        // 枠線
        p.setPen(underMouse() ? QColor(0x99, 0x99, 0x99) : QColor(0x66, 0x66, 0x66));
        p.drawRect(outer);
    }

private:
    QPixmap m_pixmap;
    bool    m_crop = false;
};

// ===========================================================================
// 対数スケール変換ヘルパー
// ---------------------------------------------------------------------------
// スライダーの内部値は常に 0〜LOG_SLIDER_MAX の整数で、実際の値(px や %)へは
// 対数で対応させる。ブラシサイズやスタンプ間隔のように「小さい側の1目盛りの
// 意味が大きい」値は、リニアだと小さい側がスライダーの左端に潰れてしまい
// 細かい調整ができないため。
// ===========================================================================
static constexpr int LOG_SLIDER_MAX = 1000;

// ブラシサイズの範囲(syncSize() でも使うのでここに置く)
static constexpr int MIN_SIZE = 1;
static constexpr int MAX_SIZE = 1000;

static int logSliderToValue(int sliderVal, int minV, int maxV)
{
    const double t    = double(sliderVal) / LOG_SLIDER_MAX;
    const double log1 = qLn(double(minV));
    const double log2 = qLn(double(maxV));
    return qBound(minV, qRound(qExp(log1 + t * (log2 - log1))), maxV);
}

static int valueToLogSlider(int v, int minV, int maxV)
{
    v = qBound(minV, v, maxV);
    const double log1 = qLn(double(minV));
    const double log2 = qLn(double(maxV));
    const double t    = (qLn(double(v)) - log1) / (log2 - log1);
    return qBound(0, qRound(t * LOG_SLIDER_MAX), LOG_SLIDER_MAX);
}

static int sliderToPx(int sliderVal) { return logSliderToValue(sliderVal, MIN_SIZE, MAX_SIZE); }
static int pxToSlider(int px)        { return valueToLogSlider(px, MIN_SIZE, MAX_SIZE); }

// スタンプ間隔(ブラシ直径に対する%)の範囲
static constexpr int MIN_SPACING_PCT = 5;
static constexpr int MAX_SPACING_PCT = 300;

// 紙質テクスチャの拡大率(%)の範囲。PenToolConfig::setPaperScaleのクランプと合わせること。
static constexpr int MIN_PAPER_SCALE_PCT = 25;
static constexpr int MAX_PAPER_SCALE_PCT = 400;

// ===========================================================================
// 複数ツールが同名で持つプロパティへのアクセス
// ---------------------------------------------------------------------------
// サイズ・硬さのように「ペンも消しゴムもぼかしも持っている」値は、ToolType から
// 対応する Config を解決するヘルパーをここに1つだけ置く。以前は設定項目ごとに
// if (isPen || isEraser || isBlur ...) と switch を繰り返していたため、ツールを
// 増やすたびに全項目へ手を入れる必要があった。
//
// 持っていないツールに対しては呼ばれない(kToolPages にその項目を並べていない)。
// 万一呼ばれてもクラッシュしないよう、既定はペンの値を返す。
// ===========================================================================
static int brushSize(ToolConfig &c, ToolType t)
{
    switch (t) {
    case ToolType::Eraser:    return c.eraser().size();
    case ToolType::Blur:      return c.blur().size();
    case ToolType::Warp:      return c.warp().size();
    case ToolType::Selection: return c.selection().size();
    case ToolType::Airbrush:  return c.airbrush().size();
    default:                  return c.pen().size();
    }
}
static void setBrushSize(ToolConfig &c, ToolType t, int px)
{
    switch (t) {
    case ToolType::Eraser:    c.eraser().setSize(px);    break;
    case ToolType::Blur:      c.blur().setSize(px);      break;
    case ToolType::Warp:      c.warp().setSize(px);      break;
    case ToolType::Selection: c.selection().setSize(px); break;
    case ToolType::Airbrush:  c.airbrush().setSize(px);  break;
    default:                  c.pen().setSize(px);       break;
    }
}

static float brushHardness(ToolConfig &c, ToolType t)
{
    switch (t) {
    case ToolType::Eraser:    return c.eraser().hardness();
    case ToolType::Blur:      return c.blur().hardness();
    case ToolType::Warp:      return c.warp().hardness();
    case ToolType::Selection: return c.selection().hardness();
    case ToolType::Airbrush:  return c.airbrush().hardness();
    default:                  return c.pen().hardness();
    }
}
static void setBrushHardness(ToolConfig &c, ToolType t, float h)
{
    switch (t) {
    case ToolType::Eraser:    c.eraser().setHardness(h);    break;
    case ToolType::Blur:      c.blur().setHardness(h);      break;
    case ToolType::Warp:      c.warp().setHardness(h);      break;
    case ToolType::Selection: c.selection().setHardness(h); break;
    case ToolType::Airbrush:  c.airbrush().setHardness(h);  break;
    default:                  c.pen().setHardness(h);       break;
    }
}

static float brushOpacity(ToolConfig &c, ToolType t)
{
    return t == ToolType::Airbrush ? c.airbrush().opacity() : c.pen().opacity();
}
static void setBrushOpacity(ToolConfig &c, ToolType t, float o)
{
    if (t == ToolType::Airbrush) c.airbrush().setOpacity(o);
    else                         c.pen().setOpacity(o);
}

static float brushSpacing(ToolConfig &c, ToolType t)
{
    return t == ToolType::Airbrush ? c.airbrush().spacing() : c.pen().spacing();
}
static void setBrushSpacing(ToolConfig &c, ToolType t, float s)
{
    if (t == ToolType::Airbrush) c.airbrush().setSpacing(s);
    else                         c.pen().setSpacing(s);
}

// 筆圧カーブを持つのは、実際に筆圧で半径が変わるツールだけ(CanvasWidget::mapPressure)。
static const PressureCurve &toolPressureCurve(ToolConfig &c, ToolType t)
{
    switch (t) {
    case ToolType::Eraser:   return c.eraser().pressureCurve();
    case ToolType::Airbrush: return c.airbrush().pressureCurve();
    case ToolType::Blur:     return c.blur().pressureCurve();
    case ToolType::Warp:     return c.warp().pressureCurve();
    default:                 return c.pen().pressureCurve();
    }
}
static void setToolPressureCurve(ToolConfig &c, ToolType t, const PressureCurve &pc)
{
    switch (t) {
    case ToolType::Eraser:   c.eraser().setPressureCurve(pc);   break;
    case ToolType::Airbrush: c.airbrush().setPressureCurve(pc); break;
    case ToolType::Blur:     c.blur().setPressureCurve(pc);     break;
    case ToolType::Warp:     c.warp().setPressureCurve(pc);     break;
    default:                 c.pen().setPressureCurve(pc);      break;
    }
}

// 最小サイズ(筆圧0のときに残す太さの比率)。筆圧カーブと同じツールが持つ。
static float minSizeRatio(ToolConfig &c, ToolType t)
{
    switch (t) {
    case ToolType::Eraser:   return c.eraser().minSizeRatio();
    case ToolType::Airbrush: return c.airbrush().minSizeRatio();
    case ToolType::Blur:     return c.blur().minSizeRatio();
    case ToolType::Warp:     return c.warp().minSizeRatio();
    default:                 return c.pen().minSizeRatio();
    }
}
static void setMinSizeRatio(ToolConfig &c, ToolType t, float r)
{
    switch (t) {
    case ToolType::Eraser:   c.eraser().setMinSizeRatio(r);   break;
    case ToolType::Airbrush: c.airbrush().setMinSizeRatio(r); break;
    case ToolType::Blur:     c.blur().setMinSizeRatio(r);     break;
    case ToolType::Warp:     c.warp().setMinSizeRatio(r);     break;
    default:                 c.pen().setMinSizeRatio(r);      break;
    }
}

// 筆圧→不透明度は、不透明度の設定を持つツール(ペン/エアブラシ)だけが持つ。
static bool  pressureOpacity(ToolConfig &c, ToolType t)
{
    return t == ToolType::Airbrush ? c.airbrush().pressureOpacity() : c.pen().pressureOpacity();
}
static void  setPressureOpacity(ToolConfig &c, ToolType t, bool on)
{
    if (t == ToolType::Airbrush) c.airbrush().setPressureOpacity(on);
    else                         c.pen().setPressureOpacity(on);
}
static float minOpacityRatio(ToolConfig &c, ToolType t)
{
    return t == ToolType::Airbrush ? c.airbrush().minOpacityRatio() : c.pen().minOpacityRatio();
}
static void  setMinOpacityRatio(ToolConfig &c, ToolType t, float r)
{
    if (t == ToolType::Airbrush) c.airbrush().setMinOpacityRatio(r);
    else                         c.pen().setMinOpacityRatio(r);
}

// ===========================================================================
// ツールごとの設定項目一覧
// ---------------------------------------------------------------------------
// 「どのツールに、どのセクションで、どの項目を出すか」はここだけを見れば分かる。
// 項目の実体(ラベル・スライダーの範囲・値の読み書き)は buildSetting() 側にある。
//
// セクションの id は開閉状態の保存キーになるので、表示名を変えても変えないこと
// (変えるとユーザーの開閉状態がリセットされる)。
// ===========================================================================
static const QVector<ToolPageDef> &toolPages()
{
    static const QVector<ToolPageDef> pages = {
        { ToolType::Pen, {
            { "tip",    "ブラシ先端", { SettingId::TipImage, SettingId::TipAngle,
                                            SettingId::TipRoundness,
                                            SettingId::TipFollowDirection } },
            { "basic",  "基本",       { SettingId::Size, SettingId::Opacity, SettingId::Flow,
                                            SettingId::Hardness, SettingId::BrushBlendMode } },
            { "stroke", "ストローク", { SettingId::Spacing, SettingId::Smoothing,
                                            SettingId::PostCorrection } },
            { "pressure", "筆圧",     { SettingId::PressureMinSize, SettingId::PressureOpacity,
                                        SettingId::PressureCurveEdit } },
            { "tilt",     "傾き・ペン回転", { SettingId::TiltSize, SettingId::TiltOpacity,
                                                SettingId::TiltFlatten,
                                                SettingId::TiltAngleFollow,
                                                SettingId::PenRotationFollow } },
            { "paper",    "紙質",     { SettingId::PaperTexture, SettingId::PaperStrength,
                                          SettingId::PaperScale } },
            { "mix",      "混色",     { SettingId::MixRate, SettingId::PaintAmount,
                                          SettingId::PaintExtend, SettingId::PickupOnly } },
            { "taper",    "入り抜き", { SettingId::TaperIn, SettingId::TaperOut,
                                          SettingId::TaperTarget } },
            { "scatter",  "散布・ランダム", { SettingId::Scatter, SettingId::ParticleCount,
                                              SettingId::SizeJitter, SettingId::OpacityJitter,
                                              SettingId::AngleJitter, SettingId::SpacingJitter,
                                              SettingId::HueJitter, SettingId::ValueJitter } },
        }},
        { ToolType::Eraser, {
            { "basic",  "基本",       { SettingId::Size, SettingId::Hardness } },
            { "stroke", "ストローク", { SettingId::Smoothing } },
            { "pressure", "筆圧",     { SettingId::PressureMinSize, SettingId::PressureCurveEdit } },
        }},
        { ToolType::Fill, {
            { "fill", "塗りつぶし", { SettingId::FillGapSize, SettingId::FillProtectRay,
                                      SettingId::FillExtension } },
            { "ref",  "参照先",     { SettingId::FillReference } },
        }},
        { ToolType::Dropper, {
            { "ref", "参照先", { SettingId::DropperReference } },
        }},
        { ToolType::Move,   {} },
        { ToolType::Rotate, {} },
        { ToolType::Blur, {
            { "basic", "基本",   { SettingId::Size, SettingId::Hardness } },
            { "blur",  "ぼかし", { SettingId::BlurStrength, SettingId::BlurRadius } },
            { "pressure", "筆圧",  { SettingId::PressureMinSize, SettingId::PressureCurveEdit } },
        }},
        { ToolType::Warp, {
            { "basic", "基本",   { SettingId::Size, SettingId::Hardness } },
            { "warp",  "ゆがみ", { SettingId::WarpStrength } },
            { "pressure", "筆圧",  { SettingId::PressureMinSize, SettingId::PressureCurveEdit } },
        }},
        { ToolType::Selection, {
            { "selection", "選択範囲", { SettingId::SelectionClear, SettingId::SelectionMode } },
            // 投げ縄モードではサイズ/硬さは使わないが、モード切替のたびにページを
            // 作り直すわけではないので常時出しておく(以前からの挙動)。
            { "basic",     "ブラシ",   { SettingId::Size, SettingId::Hardness } },
        }},
        // テキストは専用の編集パネルを別途持つため、ここには設定項目なし
        { ToolType::Text,   {} },
        { ToolType::Airbrush, {
            { "basic",  "基本",       { SettingId::Size, SettingId::Opacity, SettingId::Hardness } },
            { "stroke", "ストローク", { SettingId::Spacing, SettingId::Smoothing } },
            { "pressure", "筆圧",     { SettingId::PressureMinSize, SettingId::PressureOpacity,
                                        SettingId::PressureCurveEdit } },
        }},
    };
    return pages;
}

static const QVector<SectionDef> &sectionsForTool(ToolType tool)
{
    static const QVector<SectionDef> empty;
    for (const ToolPageDef &p : toolPages()) {
        if (p.tool == tool) return p.sections;
    }
    return empty;
}

// セクションの開閉状態を保存するキー。ツールごとに独立させる。
static QString sectionSettingsKey(ToolType tool, const QString &sectionId)
{
    // 既存ユーザーの開閉状態を引き継ぐため、QSettings上の旧キー名は変更しない。
    return QStringLiteral("toolProp/sections/%1/%2")
        .arg(toolTypeSettingsKey(tool), sectionId);
}

// ===========================================================================
// コンストラクタ
// ===========================================================================
ToolPropertyDock::ToolPropertyDock(CanvasWidget *gl, ToolConfig *toolCfg, QWidget *parent)
    : QWidget(parent), glWidget(gl), toolCfg_(toolCfg)
{
    auto *vLayout = new QVBoxLayout(this);
    vLayout->setContentsMargins(0, 0, 0, 0);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    vLayout->addWidget(scrollArea);

    stack = new QStackedWidget;
    scrollArea->setWidget(stack);

    // QStackedWidget のインデックスが (int)ToolType と一致している必要がある
    // (setCurrentTool がそれを前提にしている)ため、enum の順に1枚ずつ足す。
    // toolTypeRegistry() は ToolType 1つにつき1行なので、その要素数＝ツール数。
    for (int i = 0; i < toolTypeRegistry().size(); i++)
        stack->addWidget(makeToolPage(static_cast<ToolType>(i)));

    setCurrentTool(ToolType::Pen);
}

// ===========================================================================
void ToolPropertyDock::setCurrentTool(ToolType tool)
{
    const int index = (int)tool;
    stack->setCurrentIndex(index);

    // QStackedWidget は「全ページのうち最大の高さ」を自分の必要サイズとして返すため、
    // そのままだと項目の少ないページ(移動・スポイト等)や、セクションを畳んだ状態でも
    // 一番背の高いページぶんの余白が残り、中身が無いところまでスクロールできてしまう。
    // 表示中以外のページの縦方向サイズポリシーを Ignored にして、
    // 「今表示しているページの高さ」だけがスクロール範囲に効くようにする。
    for (int i = 0; i < stack->count(); i++) {
        QWidget *w = stack->widget(i);
        w->setSizePolicy(QSizePolicy::Preferred,
                         i == index ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    }
    if (QWidget *w = stack->widget(index))
        w->adjustSize();
}

void ToolPropertyDock::refreshFromSettings()
{
    for (auto &fn : refreshFns)
        fn();
}

void ToolPropertyDock::setCanvasWidget(CanvasWidget *gl)
{
    QObject::disconnect(selectionChangedConn_);
    glWidget = gl;
    if (selectionClearBtn_) {
        selectionClearBtn_->setEnabled(glWidget->hasSelection());
        selectionChangedConn_ = connect(glWidget, &CanvasWidget::selectionChanged, this, [this](bool has) {
            selectionClearBtn_->setEnabled(has);
        });
    }
    setCurrentTool(glWidget->getActiveTool());
    refreshFromSettings();
}

void ToolPropertyDock::syncSize(int px)
{
    const int index = stack->currentIndex();
    if (auto *s = m_sizeSliders.value(index, nullptr)) {
        QSignalBlocker blocker(s);
        s->setValue(pxToSlider(px));
        if (auto *l = m_sizeLabels.value(index, nullptr))
            l->setText(QString::number(px));
    }
}

// ===========================================================================
// 汎用の行ヘルパー
// ===========================================================================
QSlider *ToolPropertyDock::makeSlider(QWidget *parent, int min, int max, int val)
{
    auto *s = new QSlider(Qt::Horizontal, parent);
    s->setRange(min, max);
    s->setValue(val);
    return s;
}

ToolPropertyDock::SliderRow ToolPropertyDock::addSliderRow(
    QWidget *page, QVBoxLayout *layout, const QString &label,
    int sliderMin, int sliderMax,
    std::function<int()> sliderFromConfig,
    std::function<void(int)> configFromSlider,
    std::function<QString(int)> textFromSlider)
{
    layout->addWidget(new QLabel(label));

    const int cur = sliderFromConfig();
    auto *row    = new QHBoxLayout();
    auto *slider = makeSlider(page, sliderMin, sliderMax, cur);
    auto *value  = new QLabel(textFromSlider(cur));
    value->setFixedWidth(40);
    row->addWidget(slider);
    row->addWidget(value);
    layout->addLayout(row);

    connect(slider, &QSlider::valueChanged, page,
            [configFromSlider, textFromSlider, value](int v) {
        configFromSlider(v);
        value->setText(textFromSlider(v));
    });

    refreshFns.append([sliderFromConfig, textFromSlider, slider, value]() {
        const int v = sliderFromConfig();
        QSignalBlocker blocker(slider);
        slider->setValue(v);
        value->setText(textFromSlider(v));
    });

    return { slider, value };
}

ToolPropertyDock::SliderRow ToolPropertyDock::addPercentRow(
    QWidget *page, QVBoxLayout *layout, const QString &label,
    std::function<float()> get, std::function<void(float)> set)
{
    return addSliderRow(page, layout, label, 0, 100,
                        [get] { return int(get() * 100.0f + 0.5f); },
                        [set](int v) { set(v / 100.0f); },
                        [](int v) { return QStringLiteral("%1%").arg(v); });
}

ToolPropertyDock::SliderRow ToolPropertyDock::addIntRow(
    QWidget *page, QVBoxLayout *layout, const QString &label,
    int minV, int maxV, const QString &suffix,
    std::function<int()> get, std::function<void(int)> set)
{
    return addSliderRow(page, layout, label, minV, maxV,
                        get, set,
                        [suffix](int v) { return QStringLiteral("%1%2").arg(v).arg(suffix); });
}

ToolPropertyDock::SliderRow ToolPropertyDock::addLogIntRow(
    QWidget *page, QVBoxLayout *layout, const QString &label,
    int minV, int maxV, const QString &suffix,
    std::function<int()> get, std::function<void(int)> set)
{
    return addSliderRow(page, layout, label, 0, LOG_SLIDER_MAX,
                        [get, minV, maxV] { return valueToLogSlider(get(), minV, maxV); },
                        [set, minV, maxV](int s) { set(logSliderToValue(s, minV, maxV)); },
                        [minV, maxV, suffix](int s) {
                            return QStringLiteral("%1%2")
                                .arg(logSliderToValue(s, minV, maxV)).arg(suffix);
                        });
}

QCheckBox *ToolPropertyDock::addCheckRow(
    QWidget *page, QVBoxLayout *layout, const QString &label,
    std::function<bool()> get, std::function<void(bool)> set)
{
    auto *check = new QCheckBox(label, page);
    check->setChecked(get());
    layout->addWidget(check);

    connect(check, &QCheckBox::toggled, page, [set](bool on) { set(on); });
    refreshFns.append([get, check]() {
        QSignalBlocker b(check);
        check->setChecked(get());
    });
    return check;
}

void ToolPropertyDock::addComboRow(
    QWidget *page, QVBoxLayout *layout, const QString &label,
    const QVector<QPair<QString, int>> &items,
    std::function<int()> get, std::function<void(int)> set)
{
    layout->addWidget(new QLabel(label));

    auto *combo = new QComboBox(page);
    for (const auto &it : items) {
        if (it.first.isEmpty()) combo->insertSeparator(combo->count());
        else                    combo->addItem(it.first, it.second);
    }
    auto selectValue = [combo](int v) {
        const int idx = combo->findData(v);
        combo->setCurrentIndex(idx >= 0 ? idx : 0);
    };
    selectValue(get());
    layout->addWidget(combo);

    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), page,
            [combo, set](int idx) {
        const QVariant v = combo->itemData(idx);
        if (v.isValid()) set(v.toInt()); // セパレーターは選べないので通常ここは常に有効
    });

    refreshFns.append([get, selectValue, combo]() {
        QSignalBlocker b(combo);
        selectValue(get());
    });
}

void ToolPropertyDock::addRadio2Row(
    QWidget *page, QVBoxLayout *layout, const QString &label,
    const QString &labelFalse, const QString &labelTrue,
    std::function<bool()> get, std::function<void(bool)> set)
{
    layout->addWidget(new QLabel(label));

    auto *row      = new QHBoxLayout();
    auto *falseBtn = new QRadioButton(labelFalse, page);
    auto *trueBtn  = new QRadioButton(labelTrue, page);
    row->addWidget(falseBtn);
    row->addWidget(trueBtn);
    layout->addLayout(row);

    falseBtn->setChecked(!get());
    trueBtn->setChecked(get());

    connect(falseBtn, &QRadioButton::toggled, page, [set](bool checked) { if (checked) set(false); });
    connect(trueBtn,  &QRadioButton::toggled, page, [set](bool checked) { if (checked) set(true); });

    refreshFns.append([get, falseBtn, trueBtn]() {
        const bool on = get();
        QSignalBlocker bf(falseBtn);
        QSignalBlocker bt(trueBtn);
        falseBtn->setChecked(!on);
        trueBtn->setChecked(on);
    });
}

// ===========================================================================
// 設定項目1つぶんの生成
// ---------------------------------------------------------------------------
// ここに1ケース書けば、あとは kToolPages の表に並べるだけでどのツールにも出せる。
// ===========================================================================
void ToolPropertyDock::buildSetting(SettingId id, ToolType tool, QWidget *page, QVBoxLayout *layout)
{
    ToolConfig &cfg = *toolCfg_;

    switch (id) {

    // ---- 先端画像(スタンプ) ----
    // プレビューをクリックするとファイル選択ダイアログが開き、選んだ画像がそのまま
    // ブラシの形状(アルファチャンネル)として使われる(硬さは別途、中心からの
    // 距離に応じた不透明度フォールオフとして掛かる)。
    case SettingId::TipImage: {
        auto *tipBtn = new ImagePreviewButton(page);
        tipBtn->setFixedSize(48, 48);
        tipBtn->setFlat(true);
        tipBtn->setCursor(Qt::PointingHandCursor);
        tipBtn->setToolTip("クリックして先端画像(スタンプ)を選択");
        auto *tipName = new QLabel(page);
        tipName->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        // style.qssのQPushButton共通ルール(padding: 6px 16px; min-height: 18px;)は、
        // このボタンが再polishされるたび(ドックのリサイズ等で発生しうる)にC++側の
        // setFixedSize()/sizeHint()の指定を上書きしてしまい、縦幅だけ18pxまで縮む
        // ことがある(横幅はQSS側に幅の指定が無いため48pxのまま→結果的に横長に
        // つぶれる)。ウィジェット単位のスタイルシートは共通ルールより優先されるため、
        // ここだけpadding/min・maxサイズを明示的に上書きして固定する。
        tipBtn->setStyleSheet(
            "QPushButton { padding: 0px; min-width: 48px; max-width: 48px; "
            "min-height: 48px; max-height: 48px; }");

        // ツールプリセット切り替え時、実際にGPU側で使われるテクスチャがそのツールプリセットの
        // 保存済みパスと食い違わないよう、プレビュー更新のたびに現在ロード済みの
        // パスと比較して必要なら読み直す(refreshFns経由でツールプリセット切替のたびに呼ばれる)。
        auto updateTipPreview = [this, tipBtn, tipName]() {
            QString path = toolCfg_->pen().tipImagePath();
            if (glWidget->currentPenTipImagePath() != path)
                glWidget->setPenTipImage(path);
            tipBtn->setPreviewPixmap(QPixmap(path));
            tipName->setText(BrushTipPresets::labelFor(path));
        };
        updateTipPreview();

        // クリックで同梱プリセットのグリッド(ImagePresetPicker)を開く。任意の画像を
        // 使いたい場合はそのダイアログの「ファイルから選択...」から。
        connect(tipBtn, &QPushButton::clicked, page, [this, updateTipPreview]() {
            ImagePresetPicker picker("先端画像を選択", BrushTipPresets::all(),
                                     toolCfg_->pen().tipImagePath(),
                                     ImagePresetPicker::ThumbMode::Fit, QString(), this);
            if (picker.exec() != QDialog::Accepted) return;
            const QString path = picker.selectedPath();
            if (path.isEmpty()) return;

            if (!glWidget->setPenTipImage(path)) {
                QMessageBox::warning(this, "読み込み失敗", "画像を読み込めませんでした。");
                return;
            }
            toolCfg_->pen().setTipImagePath(path);
            updateTipPreview();
        });

        refreshFns.append(updateTipPreview);

        auto *tipRow = new QHBoxLayout();
        tipRow->addWidget(tipBtn);
        tipRow->addWidget(tipName, 1);
        layout->addLayout(tipRow);
        break;
    }

    // ---- 紙質テクスチャ(ペン専用) ----
    // 紙の目をキャンバス座標へ貼り付け、その明暗をブラシの濃度に掛ける。
    // ストロークではなくキャンバスに貼り付くので、重ね描きしても目の位置が揃う
    // (stroke.comp の paperFactor 参照)。
    case SettingId::PaperTexture: {
        auto *paperBtn = new ImagePreviewButton(page);
        // 紙の目は縮小すると潰れて全部同じ灰色に見えるので、等倍で切り出して見せる
        paperBtn->setCropToFill(true);
        paperBtn->setFixedSize(48, 48);
        paperBtn->setFlat(true);
        paperBtn->setCursor(Qt::PointingHandCursor);
        paperBtn->setToolTip("クリックして紙質テクスチャを選択");
        auto *paperName = new QLabel(page);
        paperName->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        paperBtn->setStyleSheet( // 理由は先端画像のプレビューボタンと同じ
            "QPushButton { padding: 0px; min-width: 48px; max-width: 48px; "
            "min-height: 48px; max-height: 48px; }");

        auto updatePaperPreview = [this, paperBtn, paperName]() {
            const QString path = toolCfg_->pen().paperTexPath();
            if (glWidget->currentPaperTexturePath() != path)
                glWidget->setPaperTexture(path);
            paperBtn->setPreviewPixmap(QPixmap(path)); // 空パスならnullで市松模様だけ出る
            paperName->setText(PaperTexPresets::labelFor(path));
        };
        updatePaperPreview();

        connect(paperBtn, &QPushButton::clicked, page, [this, updatePaperPreview]() {
            ImagePresetPicker picker("紙質テクスチャを選択", PaperTexPresets::all(),
                                     toolCfg_->pen().paperTexPath(),
                                     ImagePresetPicker::ThumbMode::Crop,
                                     PaperTexPresets::noneLabel(), this);
            if (picker.exec() != QDialog::Accepted) return;
            const QString path = picker.selectedPath(); // 「なし」を選ぶと空になる

            if (!glWidget->setPaperTexture(path)) {
                QMessageBox::warning(this, "読み込み失敗", "画像を読み込めませんでした。");
                return;
            }
            toolCfg_->pen().setPaperTexPath(path);
            updatePaperPreview();
        });

        refreshFns.append(updatePaperPreview);

        auto *paperRow = new QHBoxLayout();
        paperRow->addWidget(paperBtn);
        paperRow->addWidget(paperName, 1);
        layout->addLayout(paperRow);
        break;
    }

    case SettingId::PaperStrength:
        addPercentRow(page, layout, "適用量",
                      [&cfg] { return cfg.pen().paperStrength(); },
                      [&cfg](float v) { cfg.pen().setPaperStrength(v); });
        break;

    // 拡大率。1倍で画像1pxがキャンバス1pxなので、細かい目を大きく使いたいときに上げる。
    // 太いブラシほど大きめが合うため実用域が広く、対数スケールにしてある。
    case SettingId::PaperScale: {
        addLogIntRow(page, layout, "拡大率", MIN_PAPER_SCALE_PCT, MAX_PAPER_SCALE_PCT, "%",
                     [&cfg] { return int(cfg.pen().paperScale() * 100.0f + 0.5f); },
                     [&cfg](int pct) { cfg.pen().setPaperScale(pct / 100.0f); });
        auto *note = new QLabel("※紙の目はキャンバスに貼り付くので、重ね描きしても位置が揃います");
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        layout->addWidget(note);
        break;
    }

    // ---- ブラシサイズ(対数スライダー) ----
    case SettingId::Size: {
        SliderRow row = addLogIntRow(page, layout, "サイズ", MIN_SIZE, MAX_SIZE, QString(),
                                     [&cfg, tool] { return brushSize(cfg, tool); },
                                     [&cfg, tool](int px) { setBrushSize(cfg, tool, px); });
        // サイズだけはブラシサイズドックとの相互同期(syncSize)があるので覚えておく
        connect(row.slider, &QSlider::valueChanged, this, [this] { emit sizeChanged(); });
        m_sizeSliders[(int)tool] = row.slider;
        m_sizeLabels[(int)tool]  = row.value;
        break;
    }

    case SettingId::Opacity:
        addPercentRow(page, layout, "不透明度",
                      [&cfg, tool] { return brushOpacity(cfg, tool); },
                      [&cfg, tool](float o) { setBrushOpacity(cfg, tool, o); });
        break;

    // ---- フロー ----
    // 不透明度が「1ストロークで到達できる濃さの上限」なのに対し、フローは
    // 「1スタンプでそこへどれだけ近づくか」。下げると、同じ場所を重ねてなぞるほど
    // 濃くなる(鉛筆で少しずつ濃くしていく感じ)。
    case SettingId::Flow: {
        addSliderRow(page, layout, "フロー", 1, 100,
                     [&cfg] { return int(cfg.pen().flow() * 100.0f + 0.5f); },
                     [&cfg](int v) { cfg.pen().setFlow(v / 100.0f); },
                     [](int v) { return QStringLiteral("%1%").arg(v); });
        auto *note = new QLabel("※下げるほど、重ねてなぞった回数だけ不透明度まで濃くなります");
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        layout->addWidget(note);
        break;
    }

    case SettingId::Hardness:
        addPercentRow(page, layout, "硬さ",
                      [&cfg, tool] { return brushHardness(cfg, tool); },
                      [&cfg, tool](float h) { setBrushHardness(cfg, tool, h); });
        break;

    // ---- ブラシの合成モード ----
    // ストロークをレイヤーへ焼き込むときの合成方法。一覧はレイヤーの合成モードと
    // 共通(BlendModeList.h)。
    case SettingId::BrushBlendMode: {
        QVector<QPair<QString, int>> items;
        items.reserve(blendModeItems().size());
        for (const BlendModeItem &it : blendModeItems())
            items.append({ it.label, (int)it.mode });
        addComboRow(page, layout, "合成モード", items,
                    [&cfg] { return cfg.pen().brushBlendMode(); },
                    [&cfg](int m) { cfg.pen().setBrushBlendMode(m); });
        break;
    }

    // ---- スタンプ間隔 ----
    // ブラシ直径に対する比率(%)。距離ベースのスタンプ方式(PenEraserTool)で、
    // ペン先が実際に移動した距離がこの間隔に達するたびに1スタンプ描く。
    // 実用域(5〜30%程度)がリニアだと左端に固まってしまうので対数スケールにする。
    case SettingId::Spacing:
        addLogIntRow(page, layout, "スタンプ間隔", MIN_SPACING_PCT, MAX_SPACING_PCT, "%",
                     [&cfg, tool] { return int(brushSpacing(cfg, tool) * 100.0f + 0.5f); },
                     [&cfg, tool](int pct) { setBrushSpacing(cfg, tool, pct / 100.0f); });
        break;

    // ---- 手振れ補正 ----
    // 内部値は「1.0で補正なし、小さいほど強く補正」だが、UIは直感に合わせて
    // 「0%で補正なし、大きいほど強く補正」と反転させて出す。
    // エアブラシだけはCanvasWidget共有の値ではなく、ツールプリセットごとに独立した
    // 値(AirbrushToolConfig::smoothing)を持つ。
    case SettingId::Smoothing: {
        const bool perPreset = (tool == ToolType::Airbrush);
        auto get = [this, perPreset]() -> float {
            return perPreset ? toolCfg_->airbrush().smoothing()
                             : glWidget->getSmoothingStrength();
        };
        auto set = [this, perPreset](float v) {
            if (perPreset) toolCfg_->airbrush().setSmoothing(v);
            else           glWidget->setSmoothingStrength(v);
        };
        // 補正なし=内部1.0=UI 0%。上限は99%(内部0.01。0にはできない)。
        addSliderRow(page, layout, "手振れ補正", 0, 99,
                     [get] { return qBound(0, int(100.0f - get() * 100.0f + 0.5f), 99); },
                     [set](int v) { set((100 - v) / 100.0f); },
                     [](int v) { return QStringLiteral("%1%").arg(v); });
        break;
    }

    // ---- 後補正 ----
    // 手振れ補正が「描いている間ペン先へ遅れて追従する」ことで揺れを抑えるのに対し、
    // こちらはペンを離してから軌道そのものを引き直す。描いている間は入力どおりに
    // 追従するので、遅れずに線を整えられる。
    case SettingId::PostCorrection: {
        addPercentRow(page, layout, "後補正",
                      [&cfg] { return cfg.pen().postCorrection(); },
                      [&cfg](float v) { cfg.pen().setPostCorrection(v); });
        auto *note = new QLabel("※ペンを離した時点で軌道をなめらかに引き直します"
                                "(描いている間は遅れません)");
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        layout->addWidget(note);
        break;
    }

    // ---- 筆圧カーブ(このツール専用) ----
    // 横軸=入ってきた筆圧、縦軸=実際に使う筆圧。環境設定の「全体の筆圧カーブ」を
    // 通した後にこれが掛かる(CanvasWidget::mapPressure)。
    // 編集ウィジェットはトーンカーブと同じ ToneCurveEditor を流用している。
    case SettingId::PressureCurveEdit: {
        auto *editor = new ToneCurveEditor(page);
        editor->setEditorSize(170); // ドックの幅に収まる大きさ
        editor->setPoints(toolPressureCurve(cfg, tool).points());
        layout->addWidget(editor, 0, Qt::AlignLeft);

        auto *btnRow    = new QHBoxLayout();
        auto *deleteBtn = new QPushButton("点を削除", page);
        auto *resetBtn  = new QPushButton("リセット", page);
        deleteBtn->setEnabled(false);
        btnRow->addWidget(deleteBtn);
        btnRow->addWidget(resetBtn);
        layout->addLayout(btnRow);

        auto *note = new QLabel("※環境設定の筆圧カーブの後に掛かります");
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        layout->addWidget(note);

        connect(editor, &ToneCurveEditor::selectionChanged, deleteBtn, &QPushButton::setEnabled);
        connect(deleteBtn, &QPushButton::clicked, editor, &ToneCurveEditor::removeSelected);
        connect(resetBtn,  &QPushButton::clicked, editor, &ToneCurveEditor::reset);
        connect(editor, &ToneCurveEditor::pointsChanged, page,
                [this, tool](const QVector<QPointF> &pts) {
            PressureCurve pc;
            pc.setPoints(pts);
            setToolPressureCurve(*toolCfg_, tool, pc);
        });

        // ツールプリセットを切り替えたら、そのプリセットのカーブを読み直す
        // (setPoints はシグナルを出さないので、書き戻しループにはならない)。
        refreshFns.append([this, tool, editor]() {
            editor->setPoints(toolPressureCurve(*toolCfg_, tool).points());
        });
        break;
    }

    // ---- 最小サイズ ----
    // 筆圧0のときに残す太さ(最大サイズに対する比率)。0%なら従来どおり筆圧0で
    // 消える。入り抜きが効きすぎて線が途切れる場合に上げる。
    case SettingId::PressureMinSize:
        addPercentRow(page, layout, "最小サイズ",
                      [&cfg, tool] { return minSizeRatio(cfg, tool); },
                      [&cfg, tool](float r) { setMinSizeRatio(cfg, tool, r); });
        break;

    // ---- 筆圧→不透明度 ----
    // チェックと「最小不透明度」をひとまとめに作る(片方だけ出しても意味が無く、
    // オフのときはスライダーを無効化して関係が分かるようにしたいため)。
    case SettingId::PressureOpacity: {
        auto *check = addCheckRow(page, layout, "筆圧で不透明度を変える",
                                  [&cfg, tool] { return pressureOpacity(cfg, tool); },
                                  [&cfg, tool](bool on) { setPressureOpacity(cfg, tool, on); });

        SliderRow row = addPercentRow(page, layout, "最小不透明度",
                                      [&cfg, tool] { return minOpacityRatio(cfg, tool); },
                                      [&cfg, tool](float r) { setMinOpacityRatio(cfg, tool, r); });
        // 「筆圧で不透明度を変える」がオフの間は最小不透明度に意味が無いので無効化する。
        auto syncEnabled = [check, row]() {
            row.slider->setEnabled(check->isChecked());
            row.value->setEnabled(check->isChecked());
        };
        syncEnabled();
        connect(check, &QCheckBox::toggled, page, [syncEnabled](bool) { syncEnabled(); });
        refreshFns.append(syncEnabled); // ツールプリセット切替でチェック状態が変わったとき用
        break;
    }

    // ---- 先端の形(ペン専用) ----
    // 角度と真円率は stroke.comp 側で「ブラシのローカル座標へ回して短軸を伸ばす」
    // 形で効く(uRoundness のコメント参照)。真円のまま角度だけ変えても
    // 手続き的な円は見た目が変わらないので、平筆にするか先端画像と併用する。
    case SettingId::TipAngle:
        addIntRow(page, layout, "角度", 0, 359, "°",
                  [&cfg] { return cfg.pen().angleDeg(); },
                  [&cfg](int v) { cfg.pen().setAngleDeg(v); });
        break;

    case SettingId::TipRoundness: {
        // 5%未満まで潰すと線として成立しなくなるので下限を設けてある。
        addSliderRow(page, layout, "真円率", 5, 100,
                     [&cfg] { return int(cfg.pen().roundness() * 100.0f + 0.5f); },
                     [&cfg](int v) { cfg.pen().setRoundness(v / 100.0f); },
                     [](int v) { return QStringLiteral("%1%").arg(v); });
        auto *note = new QLabel("※下げるほど平たい楕円(平筆)になります");
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        layout->addWidget(note);
        break;
    }

    case SettingId::TipFollowDirection:
        addCheckRow(page, layout, "進行方向に追従",
                    [&cfg] { return cfg.pen().followDirection(); },
                    [&cfg](bool on) { cfg.pen().setFollowDirection(on); });
        break;

    // ---- 傾き・ペン回転(ペン専用) ----
    // いずれもタブレットのペンを寝かせた量/軸回転で効く。マウス操作中は
    // 傾き0・回転0が渡るので、設定しても見た目は変わらない。
    case SettingId::TiltSize:
        // 100%で「最大まで寝かせたときに2倍の太さ」。鉛筆を寝かせて広く塗る感じ。
        addSliderRow(page, layout, "傾きで太くする", 0, 200,
                     [&cfg] { return int(cfg.pen().tiltSize() * 100.0f + 0.5f); },
                     [&cfg](int v) { cfg.pen().setTiltSize(v / 100.0f); },
                     [](int v) { return QStringLiteral("%1%").arg(v); });
        break;

    case SettingId::TiltOpacity:
        addPercentRow(page, layout, "傾きで薄くする",
                      [&cfg] { return cfg.pen().tiltOpacity(); },
                      [&cfg](float v) { cfg.pen().setTiltOpacity(v); });
        break;

    case SettingId::TiltFlatten:
        addPercentRow(page, layout, "傾きで平筆化",
                      [&cfg] { return cfg.pen().tiltFlatten(); },
                      [&cfg](float v) { cfg.pen().setTiltFlatten(v); });
        break;

    case SettingId::TiltAngleFollow:
        addCheckRow(page, layout, "傾けた方向へ向ける",
                    [&cfg] { return cfg.pen().tiltAngleFollow(); },
                    [&cfg](bool on) { cfg.pen().setTiltAngleFollow(on); });
        break;

    case SettingId::PenRotationFollow: {
        addCheckRow(page, layout, "ペンの回転で回す",
                    [&cfg] { return cfg.pen().penRotationFollow(); },
                    [&cfg](bool on) { cfg.pen().setPenRotationFollow(on); });
        auto *note = new QLabel("※タブレットの傾き・回転に対応したペンが必要です"
                                "(回転はアートペン等のみ)");
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        layout->addWidget(note);
        break;
    }

    // ---- 入り抜き(ペン専用) ----
    // 書き始め/書き終わりを指定距離かけて細く/薄くする。「抜き」はペンを離すまで
    // 終点が分からないため、離した瞬間にストロークを引き直して適用される
    // (PenEraserTool::rebuildStrokeWithTaper参照)。
    case SettingId::TaperIn:
        addIntRow(page, layout, "入りの長さ", 0, 500, "px",
                  [&cfg] { return cfg.pen().taperInPx(); },
                  [&cfg](int v) { cfg.pen().setTaperInPx(v); });
        break;

    case SettingId::TaperOut: {
        addIntRow(page, layout, "抜きの長さ", 0, 500, "px",
                  [&cfg] { return cfg.pen().taperOutPx(); },
                  [&cfg](int v) { cfg.pen().setTaperOutPx(v); });
        auto *note = new QLabel("※抜きはペンを離した瞬間に反映されます");
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        layout->addWidget(note);
        break;
    }

    case SettingId::TaperTarget:
        addCheckRow(page, layout, "サイズに適用",
                    [&cfg] { return cfg.pen().taperSize(); },
                    [&cfg](bool on) { cfg.pen().setTaperSize(on); });
        addCheckRow(page, layout, "不透明度に適用",
                    [&cfg] { return cfg.pen().taperOpacity(); },
                    [&cfg](bool on) { cfg.pen().setTaperOpacity(on); });
        break;

    // ---- 散布・ランダム(ペン専用) ----
    // どれもスタンプを打つ位置/大きさ/濃さ/向き/間隔をばらけさせる設定で、
    // 実際の適用は PenEraserTool::appendStamps / nextSpacingPx が行う。
    case SettingId::Scatter:
        // ブラシ直径に対する比率。100%ならブラシ1個ぶん外まで散る。
        addSliderRow(page, layout, "散布量", 0, 400,
                     [&cfg] { return int(cfg.pen().scatter() * 100.0f + 0.5f); },
                     [&cfg](int v) { cfg.pen().setScatter(v / 100.0f); },
                     [](int v) { return QStringLiteral("%1%").arg(v); });
        break;

    case SettingId::ParticleCount:
        addIntRow(page, layout, "粒子数", 1, 16, QString(),
                  [&cfg] { return cfg.pen().particleCount(); },
                  [&cfg](int v) { cfg.pen().setParticleCount(v); });
        break;

    case SettingId::SizeJitter:
        addPercentRow(page, layout, "サイズのランダム",
                      [&cfg] { return cfg.pen().sizeJitter(); },
                      [&cfg](float v) { cfg.pen().setSizeJitter(v); });
        break;

    case SettingId::OpacityJitter:
        addPercentRow(page, layout, "不透明度のランダム",
                      [&cfg] { return cfg.pen().opacityJitter(); },
                      [&cfg](float v) { cfg.pen().setOpacityJitter(v); });
        break;

    // ---- 下地混色 ----
    // 筆が進みながら、描いている先のキャンバスの色を拾って混ぜる。1スタンプごとに
    // 「筆に乗っている絵の具」を下地へこの割合だけ寄せる(brushState.comp)。
    case SettingId::MixRate: {
        addPercentRow(page, layout, "下地混色",
                      [&cfg] { return cfg.pen().mixRate(); },
                      [&cfg](float v) { cfg.pen().setMixRate(v); });
        auto *note = new QLabel("※描いている先(アクティブレイヤー)の色を拾いながら塗ります。"
                                "何も無い所からは拾いません");
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        layout->addWidget(note);
        break;
    }

    // ---- 絵の具量・絵の具の伸び ----
    // 筆に乗る絵の具を有限にして、進むほど減らす。尽きると新しい絵の具を置かなくなり、
    // 線がかすれて消える(拾う動作は続くので、乾いた筆で下地をなぞる感じになる)。
    // 消費量はストローク開始からの経路長だけで決まる(brushState.comp)。
    case SettingId::PaintAmount: {
        addPercentRow(page, layout, "絵の具量",
                      [&cfg] { return cfg.pen().paintAmount(); },
                      [&cfg](float v) { cfg.pen().setPaintAmount(v); });
        auto *note = new QLabel("※100%で尽きません。下げるほど早くかすれます");
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        layout->addWidget(note);
        break;
    }

    case SettingId::PaintExtend:
        addPercentRow(page, layout, "絵の具の伸び",
                      [&cfg] { return cfg.pen().paintExtend(); },
                      [&cfg](float v) { cfg.pen().setPaintExtend(v); });
        break;

    // ---- 拾った色だけで塗る(指先・色伸ばし) ----
    // 筆が自分の絵の具を持たなくなり、下地から拾ったぶんしか置かなくなる。
    // 拾う動作そのものは下地混色なので、単体では意味を持たない。
    case SettingId::PickupOnly: {
        addCheckRow(page, layout, "拾った色だけで塗る",
                    [&cfg] { return cfg.pen().pickupOnly(); },
                    [&cfg](bool on) { cfg.pen().setPickupOnly(on); });
        auto *note = new QLabel("※自分の絵の具を持たなくなります(指先・色伸ばし)。"
                                "下地混色と併せて使ってください");
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        layout->addWidget(note);
        break;
    }

    // ---- 色のランダム ----
    // 他のランダムと違い、スタンプごとに「色」が変わるので、ストローク中の色を
    // 画素ごとに覚えておくバッファが要る(stroke.comp の uUseStrokeColor)。
    // 0%のままなら従来どおり1色で塗る経路を通るので、バッファも確保されない。
    case SettingId::HueJitter:
        addPercentRow(page, layout, "色相のランダム",
                      [&cfg] { return cfg.pen().hueJitter(); },
                      [&cfg](float v) { cfg.pen().setHueJitter(v); });
        break;

    case SettingId::ValueJitter: {
        addPercentRow(page, layout, "明度のランダム",
                      [&cfg] { return cfg.pen().valueJitter(); },
                      [&cfg](float v) { cfg.pen().setValueJitter(v); });
        auto *note = new QLabel("※色相は前後に、明度は暗い側にずれます");
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        layout->addWidget(note);
        break;
    }

    case SettingId::AngleJitter: {
        addPercentRow(page, layout, "角度のランダム",
                      [&cfg] { return cfg.pen().angleJitter(); },
                      [&cfg](float v) { cfg.pen().setAngleJitter(v); });
        // 手続き的な円は回しても見た目が変わらないので、誤解しないよう書いておく。
        auto *note = new QLabel("※先端画像を回します(円のままでは変化しません)");
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        layout->addWidget(note);
        break;
    }

    case SettingId::SpacingJitter:
        addPercentRow(page, layout, "間隔のランダム",
                      [&cfg] { return cfg.pen().spacingJitter(); },
                      [&cfg](float v) { cfg.pen().setSpacingJitter(v); });
        break;

    // ---- 塗りつぶし ----
    case SettingId::FillGapSize:
        addIntRow(page, layout, "隙間認識", -100, 100, "px",
                  [&cfg] { return cfg.fill().fillGapSize(); },
                  [&cfg](int v) { cfg.fill().setFillGapSize(v); });
        break;

    case SettingId::FillProtectRay:
        addIntRow(page, layout, "細線保護", -100, 100, "px",
                  [&cfg] { return cfg.fill().protectRayLength(); },
                  [&cfg](int v) { cfg.fill().setProtectRayLength(v); });
        break;

    // 0.1px刻みなので、スライダー内部値は10倍した整数で扱う
    case SettingId::FillExtension:
        addSliderRow(page, layout, "領域拡大", -50, 50,
                     [&cfg] { return int(cfg.fill().fillExtension() * 10.0f); },
                     [&cfg](int v) { cfg.fill().setFillExtension(v / 10.0f); },
                     [](int v) { return QStringLiteral("%1px").arg(v / 10.0f); });
        break;

    case SettingId::FillReference:
        addRadio2Row(page, layout, "参照先", "レイヤー", "キャンバス",
                     [&cfg] { return cfg.fill().referenceCanvas(); },
                     [&cfg](bool v) { cfg.fill().setReferenceCanvas(v); });
        break;

    // ---- スポイト ----
    case SettingId::DropperReference:
        addRadio2Row(page, layout, "参照先", "レイヤー", "キャンバス",
                     [&cfg] { return cfg.dropper().referenceCanvas(); },
                     [&cfg](bool v) { cfg.dropper().setReferenceCanvas(v); });
        break;

    // ---- ぼかし ----
    case SettingId::BlurStrength:
        addPercentRow(page, layout, "強さ",
                      [&cfg] { return cfg.blur().strength(); },
                      [&cfg](float v) { cfg.blur().setStrength(v); });
        break;

    case SettingId::BlurRadius:
        addIntRow(page, layout, "ぼかし範囲", 1, 32, "px",
                  [&cfg] { return cfg.blur().blurRadius(); },
                  [&cfg](int v) { cfg.blur().setBlurRadius(v); });
        break;

    // ---- ゆがみ ----
    case SettingId::WarpStrength:
        addPercentRow(page, layout, "強さ",
                      [&cfg] { return cfg.warp().strength(); },
                      [&cfg](float v) { cfg.warp().setStrength(v); });
        break;

    // ---- 選択 ----
    case SettingId::SelectionMode:
        addRadio2Row(page, layout, "モード", "投げ縄", "ペン選択",
                     [&cfg] { return cfg.selection().mode() == SelectionMode::PenSelect; },
                     [&cfg](bool pen) {
                         cfg.selection().setMode(pen ? SelectionMode::PenSelect
                                                     : SelectionMode::Lasso);
                     });
        break;

    case SettingId::SelectionClear: {
        selectionClearBtn_ = new QPushButton("選択を解除", page);
        selectionClearBtn_->setEnabled(glWidget->hasSelection());
        layout->addWidget(selectionClearBtn_);

        connect(selectionClearBtn_, &QPushButton::clicked, this, [this]() {
            glWidget->clearSelection();
        });
        selectionChangedConn_ = connect(glWidget, &CanvasWidget::selectionChanged, this, [this](bool has) {
            selectionClearBtn_->setEnabled(has);
        });
        break;
    }
    }
}

// ===========================================================================
// ツール1つぶんのページ
// ===========================================================================
QWidget *ToolPropertyDock::makeToolPage(ToolType tool)
{
    auto *page    = new QWidget();
    auto *vLayout = new QVBoxLayout(page);
    vLayout->setContentsMargins(8, 8, 8, 8);
    vLayout->setSpacing(2);

    // タイトル(ToolRegistryの表示名をそのまま使う)
    auto *titleLabel = new QLabel(toolTypeLabel(tool));
    titleLabel->setStyleSheet("font-weight: bold; font-size: 12px;");
    vLayout->addWidget(titleLabel);

    const QVector<SectionDef> &sections = sectionsForTool(tool);
    if (sections.isEmpty()) {
        auto *none = new QLabel("設定項目はありません");
        none->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDisabled.name()));
        vLayout->addWidget(none);
    }

    for (const SectionDef &sec : sections) {
        auto *box = new CollapsibleSection(sec.title, page);
        for (SettingId id : sec.settings)
            buildSetting(id, tool, page, box->contentLayout());
        vLayout->addWidget(box);

        // 開閉状態は前回の状態を覚えておく(ツール×セクションごと)
        const QString key = sectionSettingsKey(tool, sec.id);
        box->setExpanded(QSettings().value(key, true).toBool());
        connect(box, &CollapsibleSection::expandedChanged, this, [this, key, page](bool on) {
            QSettings().setValue(key, on);
            // 開閉でページの必要な高さが変わるので、スクロール範囲を追従させる
            // (setCurrentTool のサイズポリシーのコメント参照)。
            page->adjustSize();
            if (stack) stack->updateGeometry();
        });
    }

    vLayout->addStretch();
    return page;
}
