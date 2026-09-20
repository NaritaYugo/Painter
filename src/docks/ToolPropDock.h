#pragma once

#include "canvas/CanvasWidget.h"
#include "tools/core/ToolConfig.h"

#include <QWidget>
#include <functional>
#include <QMap>
#include <QMetaObject>
#include <QPair>
#include <QVector>

class QPushButton;
class QStackedWidget;
class QSlider;
class QLabel;
class QVBoxLayout;
class QCheckBox;

// ===========================================================================
// ツール設定の項目定義
// ---------------------------------------------------------------------------
// 設定項目は「まず SettingId として1つ定義し、ToolPropDock::buildSetting() に
// 作り方を1ケース書く」→「どのツールのどのセクションに出すかを kToolPages
// (ToolPropDock.cpp)の表へ並べるだけ」で追加できるようにしてある。
//
// 以前はページ生成関数の中で if (isPen || isEraser || isBlur ...) を項目ごとに
// 書き並べていたため、ツールを増やすたびに全条件式へ手を入れる必要があり、
// どのツールに何が出るのかも読み取りづらかった。
//
// 新しい設定項目を足す手順:
//   1. SettingId に値を足す
//   2. ToolPropDock::buildSetting() の switch に1ケース足す
//      (サイズ等、複数ツールが同名で持つ値なら ToolPropDock.cpp 冒頭の
//       アクセスヘルパーにも1つ足す)
//   3. kToolPages の該当ツールのセクションへ SettingId を並べる
// ===========================================================================
namespace ToolProp {

enum class SettingId {
    // ブラシ系(複数ツールが共通で持つ)
    TipImage,       // 先端画像(スタンプ)
    Size,           // ブラシサイズ
    Opacity,        // 不透明度
    Flow,           // フロー(1スタンプが乗せる量。ペンのみ)
    Hardness,       // 硬さ
    BrushBlendMode, // ブラシの合成モード(ペンのみ)
    Spacing,        // スタンプ間隔
    Smoothing,      // 手振れ補正
    PostCorrection, // 後補正(離した後に軌道を整える。ペンのみ)
    PressureCurveEdit, // 筆圧カーブ(このツール専用。環境設定の全体カーブの後に掛かる)
    PressureMinSize,   // 最小サイズ(筆圧0のときに残す太さの比率)
    PressureOpacity,   // 筆圧で不透明度を変える(on/off + 最小不透明度)

    // 先端の形(ペンのみ)
    TipAngle,          // 角度
    TipRoundness,      // 真円率(平筆)
    TipFollowDirection,// 進行方向に追従

    // 傾き・ペン回転(ペンのみ)
    TiltSize,          // 傾きで太くする
    TiltOpacity,       // 傾きで薄くする
    TiltFlatten,       // 傾きで平筆化する
    TiltAngleFollow,   // 傾けた方向へ先端を向ける
    PenRotationFollow, // ペンの回転で先端を回す

    // 紙質(ペンのみ)
    PaperTexture,      // 紙質テクスチャの画像
    PaperStrength,     // 適用量
    PaperScale,        // 拡大率

    // 入り抜き(ペンのみ)
    TaperIn,           // 入りの長さ
    TaperOut,          // 抜きの長さ
    TaperTarget,       // 入り抜きを何に効かせるか(サイズ/不透明度)

    // 散布・ランダム(ペンのみ。BrushScatter系)
    Scatter,           // 散布量
    ParticleCount,     // 粒子数
    SizeJitter,        // サイズのランダム
    OpacityJitter,     // 不透明度のランダム
    HueJitter,         // 色相のランダム
    ValueJitter,       // 明度のランダム

    // 混色(ペンのみ)
    MixRate,           // 下地混色
    PaintAmount,       // 絵の具量
    PaintExtend,       // 絵の具の伸び
    PickupOnly,        // 拾った色だけで塗る(指先・色伸ばし)
    AngleJitter,       // 角度のランダム
    SpacingJitter,     // 間隔のランダム

    // 塗りつぶし
    FillGapSize,
    FillProtectRay,
    FillExtension,
    FillReference,

    // スポイト
    DropperReference,

    // ぼかし
    BlurStrength,
    BlurRadius,

    // ゆがみ
    WarpStrength,

    // 選択
    SelectionMode,
    SelectionClear,
};

// 折り畳みセクション1つぶんの定義。
struct SectionDef {
    QString id;                  // 開閉状態の保存キー(英数字。表示名とは別に固定する)
    QString title;               // 見出しに出す名前
    QVector<SettingId> settings; // 並べる設定項目(この順に上から並ぶ)
};

// ツール1つぶんのページ定義。
struct ToolPageDef {
    ToolType tool;
    QVector<SectionDef> sections;
};

} // namespace ToolProp

// ===========================================================================
// ToolPropDock  ―  選択中ツールに応じて設定項目を切り替えるパネル
//
// ToolDockWidget::toolChanged シグナルを受けて setCurrentTool() を呼ぶ。
// ページは ToolType ごとに1枚で、QStackedWidget のインデックス＝(int)ToolType。
// ===========================================================================
class ToolPropDock : public QWidget
{
    Q_OBJECT
public:
    explicit ToolPropDock(CanvasWidget *gl, ToolConfig *toolCfg, QWidget *parent = nullptr);

    void setCurrentTool(ToolType tool);
    void refreshFromSettings();
    void syncSize(int px);

    // タブ切替時に、表示対象のCanvasWidget(=キャンバス)を差し替える
    void setCanvasWidget(CanvasWidget *gl);

signals:
    void sizeChanged();

private:
    // ---- ページ/項目の生成 ------------------------------------------------
    QWidget *makeToolPage(ToolType tool);
    // 設定項目1つぶんのウィジェットを layout へ足す。
    void buildSetting(ToolProp::SettingId id, ToolType tool, QWidget *page, QVBoxLayout *layout);

    // ---- 汎用の行ヘルパー --------------------------------------------------
    // どれも「ラベル + スライダー + 現在値ラベル」の1行を作り、値の反映と
    // refreshFromSettings() 用の再読み込み関数の登録までまとめて行う。
    struct SliderRow {
        QSlider *slider = nullptr;
        QLabel  *value  = nullptr;
    };
    // スライダーの内部値をそのまま扱う最も低レベルな版。
    // 対数スケールの行は「内部値=対数スライダー目盛り」として使う。
    SliderRow addSliderRow(QWidget *page, QVBoxLayout *layout, const QString &label,
                           int sliderMin, int sliderMax,
                           std::function<int()> sliderFromConfig,
                           std::function<void(int)> configFromSlider,
                           std::function<QString(int)> textFromSlider);
    // 0.0〜1.0 の値を 0〜100% として扱う行
    SliderRow addPercentRow(QWidget *page, QVBoxLayout *layout, const QString &label,
                            std::function<float()> get, std::function<void(float)> set);
    // 整数値(px等)をリニアなスライダーで扱う行
    SliderRow addIntRow(QWidget *page, QVBoxLayout *layout, const QString &label,
                        int minV, int maxV, const QString &suffix,
                        std::function<int()> get, std::function<void(int)> set);
    // 整数値を対数スケールのスライダーで扱う行(サイズ・スタンプ間隔)
    SliderRow addLogIntRow(QWidget *page, QVBoxLayout *layout, const QString &label,
                           int minV, int maxV, const QString &suffix,
                           std::function<int()> get, std::function<void(int)> set);
    // ドロップダウン(コンボボックス)の行。items は (表示名, 値) の並びで、
    // 表示名が空のものはセパレーターとして挿入する。
    void addComboRow(QWidget *page, QVBoxLayout *layout, const QString &label,
                     const QVector<QPair<QString, int>> &items,
                     std::function<int()> get, std::function<void(int)> set);
    // 2択のラジオボタン行
    void addRadio2Row(QWidget *page, QVBoxLayout *layout, const QString &label,
                      const QString &labelFalse, const QString &labelTrue,
                      std::function<bool()> get, std::function<void(bool)> set);
    // on/offのチェックボックス行。作ったチェックボックスを返すので、
    // 他のウィジェットの有効/無効をこれに連動させたい場合に使える。
    QCheckBox *addCheckRow(QWidget *page, QVBoxLayout *layout, const QString &label,
                           std::function<bool()> get, std::function<void(bool)> set);

    static QSlider *makeSlider(QWidget *parent, int min, int max, int val);

    // ---- 状態 --------------------------------------------------------------
    CanvasWidget       *glWidget = nullptr;
    ToolConfig     *toolCfg_ = nullptr;
    QStackedWidget *stack    = nullptr;
    QPushButton    *selectionClearBtn_ = nullptr; // 選択ツールページの「選択を解除」ボタン
    QMetaObject::Connection selectionChangedConn_;

    QVector<std::function<void()>> refreshFns;

    // syncSize() 用。キーは (int)ToolType。
    QMap<int, QSlider *> m_sizeSliders;
    QMap<int, QLabel  *> m_sizeLabels;
};
