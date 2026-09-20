#pragma once
#include "dialogs/DraggablePanel.h"

#include <QColor>

class ColorWheelWidget;

// ---------------------------------------------------------------------------
// SolidColorPickerPanel
// ---------------------------------------------------------------------------
// 単色レイヤーの色を編集するポップアップパネル。中身はColorCircleDock(ドック側の
// カラーサークル)と同じColorWheelWidgetをそのまま使う(OKLCHベースのカラー
// サークルの実装を二重に持たないため)。BrightnessContrastPanel等と同様、
// CanvasWidgetの子として「キャンバス上に」浮かせて表示する非モーダルのQWidget。
// ---------------------------------------------------------------------------
class SolidColorPickerPanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit SolidColorPickerPanel(QWidget *parent = nullptr);

    void setColor(const QColor &color); // 既存の値でカラーサークルを初期化する(再編集用)

signals:
    void colorChanged(const QColor &color);
    void confirmed();
    void cancelled();

private:
    ColorWheelWidget *wheel_ = nullptr;
};
