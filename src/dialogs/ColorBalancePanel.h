#pragma once
#include "dialogs/DraggablePanel.h"

class QSlider;
class QLabel;

// ---------------------------------------------------------------------------
// ColorBalancePanel
// ---------------------------------------------------------------------------
// 編集メニュー「カラーバランス」アクション用のポップアップパネル(C/M/Yの3本)。
// HueSatLightPanelと同様、GLWidgetの子として「キャンバス上に」浮かせて表示する
// 非モーダルのQWidget。
// ---------------------------------------------------------------------------
class ColorBalancePanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit ColorBalancePanel(QWidget *parent = nullptr);

    void resetValues(); // 3本のスライダーを0(恒等)に戻す
    void setValues(int cyan, int magenta, int yellow); // シグナルを発火させずに現在値を反映する(調整レイヤーの再編集用)

signals:
    void valuesChanged(int cyan, int magenta, int yellow);
    void confirmed();
    void cancelled();

protected:

private:
    QSlider *cyanSlider_        = nullptr;
    QSlider *magentaSlider_     = nullptr;
    QSlider *yellowSlider_      = nullptr;
    QLabel  *cyanValueLabel_    = nullptr;
    QLabel  *magentaValueLabel_ = nullptr;
    QLabel  *yellowValueLabel_  = nullptr;

    QWidget *makeRow(const QString &labelText, QSlider *&sliderOut, QLabel *&valueLabelOut,
                      int minV, int maxV);
    void emitValuesChanged();
};
