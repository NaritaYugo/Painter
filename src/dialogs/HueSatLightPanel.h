#pragma once
#include "dialogs/DraggablePanel.h"

class QSlider;
class QLabel;

// ---------------------------------------------------------------------------
// HueSatLightPanel
// ---------------------------------------------------------------------------
// 編集メニュー「色相・彩度・明度」アクション用のポップアップパネル。
// モーダルQDialogではなく、CanvasWidgetの子として「キャンバス上に」浮かせて表示する
// 非モーダルのQWidget(スライダーを動かすたびにライブプレビューへ反映するため)。
// 所有・配置(位置決め/表示/非表示)はCanvasWidget側が行う。
// ---------------------------------------------------------------------------
class HueSatLightPanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit HueSatLightPanel(QWidget *parent = nullptr);

    void resetValues(); // 3本のスライダーを0(恒等)に戻す
    void setValues(int hue, int saturation, int lightness); // 既存の値でスライダーを初期化する(再編集用)

signals:
    void valuesChanged(int hue, int saturation, int lightness);
    void confirmed();
    void cancelled();

protected:

private:
    QSlider *hueSlider_        = nullptr;
    QSlider *satSlider_        = nullptr;
    QSlider *lightSlider_      = nullptr;
    QLabel  *hueValueLabel_    = nullptr;
    QLabel  *satValueLabel_    = nullptr;
    QLabel  *lightValueLabel_  = nullptr;

    QWidget *makeRow(const QString &labelText, QSlider *&sliderOut, QLabel *&valueLabelOut,
                      int minV, int maxV);
    void emitValuesChanged();
};
