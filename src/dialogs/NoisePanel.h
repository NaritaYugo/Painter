#pragma once
#include "dialogs/DraggablePanel.h"

class QSlider;
class QLabel;
class QComboBox;
class QWidget;

// ---------------------------------------------------------------------------
// NoisePanel
// ---------------------------------------------------------------------------
// フィルターメニュー「ノイズ」アクション用のポップアップパネル。
// GaussianBlurPanel と同様、GLWidgetの子として「キャンバス上に」浮かせて表示する
// 非モーダルのQWidget。強さ・種類(モノクロ/カラー)・粒の大きさを持つ。
// ---------------------------------------------------------------------------
class NoisePanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit NoisePanel(QWidget *parent = nullptr);

    void setStrength(float strength01); // 0..1
    void setMonochrome(bool mono);
    void setGrainPx(float grainPx);

signals:
    void strengthChanged(float strength01); // 0..1
    void monochromeChanged(bool mono);
    void grainChanged(float grainPx);
    void confirmed();
    void cancelled();

private:
    QSlider   *strengthSlider_ = nullptr;
    QLabel    *strengthValueLabel_ = nullptr;
    QComboBox *typeCombo_ = nullptr;
    QSlider   *grainSlider_ = nullptr;
    QLabel    *grainValueLabel_ = nullptr;
};
