#pragma once
#include "dialogs/DraggablePanel.h"

class QSlider;
class QLabel;
class QComboBox;
class QWidget;

// ---------------------------------------------------------------------------
// LensBlurPanel (Pro限定)
// ---------------------------------------------------------------------------
// フィルターメニュー「レンズぼかし」アクション用のポップアップパネル。
// GaussianBlurPanel と同様、CanvasWidgetの子として「キャンバス上に」浮かせて表示する
// 非モーダルのQWidget。深度マップは扱わないので、絞り(形状・回転)とハイライトの
// 強調具合だけを持つ。各パラメータの意味は lensBlurFilter.comp のコメント参照。
// ---------------------------------------------------------------------------
class LensBlurPanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit LensBlurPanel(QWidget *parent = nullptr);

    void setRadiusPx(float radiusPx);
    void setBlades(int blades);            // 0=円, 3以上=正多角形の辺数
    void setBladeRotDeg(float rotDeg);
    void setHighlightBoost(float boost);   // 0..1
    void setThreshold(float threshold);    // 0..1

signals:
    void radiusChanged(float radiusPx);
    void bladesChanged(int blades);
    void bladeRotChanged(float rotDeg);
    void highlightBoostChanged(float boost);   // 0..1
    void thresholdChanged(float threshold);    // 0..1
    void confirmed();
    void cancelled();

private:
    QSlider   *radiusSlider_ = nullptr;
    QLabel    *radiusValueLabel_ = nullptr;
    QComboBox *bladesCombo_ = nullptr;
    QWidget   *rotRow_ = nullptr;
    QSlider   *rotSlider_ = nullptr;
    QLabel    *rotValueLabel_ = nullptr;
    QSlider   *boostSlider_ = nullptr;
    QLabel    *boostValueLabel_ = nullptr;
    QSlider   *thresholdSlider_ = nullptr;
    QLabel    *thresholdValueLabel_ = nullptr;
};
