#pragma once
#include "dialogs/DraggablePanel.h"

class QSlider;
class QLabel;
class QComboBox;
class QWidget;

// ---------------------------------------------------------------------------
// MotionBlurPanel
// ---------------------------------------------------------------------------
// フィルターメニュー「移動ぼかし」アクション用のポップアップパネル。
// ChromaticAberrationPanel と同様、CanvasWidgetの子として「キャンバス上に」浮かせて
// 表示する非モーダルのQWidget。平行/円形の切り替えに応じて、平行なら
// 「角度+距離」、円形なら「振れ角(中心はキャンバス上のハンドルで指定)」を出す。
// ---------------------------------------------------------------------------
class MotionBlurPanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit MotionBlurPanel(QWidget *parent = nullptr);

    void setMode(int mode); // 0=平行, 1=円形。外部からセット(シグナルは出さない)
    void setAngleDeg(float angleDeg);
    void setDistancePx(float distancePx);
    void setAngleSpanDeg(float spanDeg);

signals:
    void modeChanged(int mode);
    void angleChanged(float angleDeg);
    void distanceChanged(float distancePx);
    void angleSpanChanged(float spanDeg);
    void confirmed();
    void cancelled();

private:
    void applyModeVisibility(int mode);

    QComboBox *modeCombo_ = nullptr;
    // 平行モード用
    QWidget   *angleRow_ = nullptr;
    QSlider   *angleSlider_ = nullptr;
    QLabel    *angleValueLabel_ = nullptr;
    QWidget   *distanceRow_ = nullptr;
    QSlider   *distanceSlider_ = nullptr;
    QLabel    *distanceValueLabel_ = nullptr;
    // 円形モード用
    QLabel    *centerHintLabel_ = nullptr;
    QWidget   *spanRow_ = nullptr;
    QSlider   *spanSlider_ = nullptr;
    QLabel    *spanValueLabel_ = nullptr;
};
