#pragma once
#include "dialogs/DraggablePanel.h"
#include <QVector2D>

class QSlider;
class QLabel;
class QComboBox;
class QWidget;

// ---------------------------------------------------------------------------
// ChromaticAberrationPanel (Pro限定)
// ---------------------------------------------------------------------------
// フィルターメニュー「色収差」アクション用のポップアップパネル。MosaicPanelと
// 同様、CanvasWidgetの子として「キャンバス上に」浮かせて表示する非モーダルのQWidget。
// 平行/円形の切り替え、平行モード時のみ有効になる角度スライダー、ずらす距離スライダー
// を持つ。円形モード時の中心は数値入力を持たず、キャンバス上に表示されるハンドルを
// 直接ドラッグして指定する(ChromaticAberrationTool::onMousePress/onMouseMove側で処理)。
// ---------------------------------------------------------------------------
class ChromaticAberrationPanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit ChromaticAberrationPanel(QWidget *parent = nullptr);

    void setMode(int mode); // 0=平行, 1=円形。外部からセット(シグナルは出さない)
    void setAngleDeg(float angleDeg);
    void setDistancePx(float distancePx);

signals:
    void modeChanged(int mode);
    void angleChanged(float angleDeg);
    void distanceChanged(float distancePx);
    void confirmed();
    void cancelled();

private:
    QComboBox *modeCombo_ = nullptr;
    QWidget   *angleRow_ = nullptr;
    QSlider   *angleSlider_ = nullptr;
    QLabel    *angleValueLabel_ = nullptr;
    QLabel    *centerHintLabel_ = nullptr;
    QSlider   *distanceSlider_ = nullptr;
    QLabel    *distanceValueLabel_ = nullptr;
};
