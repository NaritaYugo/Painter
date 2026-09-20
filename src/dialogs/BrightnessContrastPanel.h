#pragma once
#include "dialogs/DraggablePanel.h"

class QSlider;
class QLabel;

// ---------------------------------------------------------------------------
// BrightnessContrastPanel
// ---------------------------------------------------------------------------
// 編集メニュー「明るさ・コントラスト」アクション用のポップアップパネル。
// HueSatLightPanelと同様、CanvasWidgetの子として「キャンバス上に」浮かせて表示する
// 非モーダルのQWidget。
// ---------------------------------------------------------------------------
class BrightnessContrastPanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit BrightnessContrastPanel(QWidget *parent = nullptr);

    void resetValues(); // スライダーを0(恒等)に戻す
    void setValues(int brightness, int contrast); // 既存の値でスライダーを初期化する(再編集用)

signals:
    void valuesChanged(int brightness, int contrast);
    void confirmed();
    void cancelled();

protected:

private:
    QSlider *brightnessSlider_     = nullptr;
    QSlider *contrastSlider_       = nullptr;
    QLabel  *brightnessValueLabel_ = nullptr;
    QLabel  *contrastValueLabel_   = nullptr;

    QWidget *makeRow(const QString &labelText, QSlider *&sliderOut, QLabel *&valueLabelOut,
                      int minV, int maxV);
    void emitValuesChanged();
};
