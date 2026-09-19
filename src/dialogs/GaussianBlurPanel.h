#pragma once
#include "dialogs/DraggablePanel.h"

class QSlider;
class QLabel;

// ---------------------------------------------------------------------------
// GaussianBlurPanel
// ---------------------------------------------------------------------------
// フィルターメニュー「ガウスぼかし」アクション用のポップアップパネル(半径スライダー1本)。
// ColorBalancePanel/HueSatLightPanelと同様、GLWidgetの子として「キャンバス上に」
// 浮かせて表示する非モーダルのQWidget。
// ---------------------------------------------------------------------------
class GaussianBlurPanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit GaussianBlurPanel(QWidget *parent = nullptr);

    void setRadius(int radius); // スライダーの値を外部からセット(シグナルは出さない)

signals:
    void radiusChanged(int radius);
    void confirmed();
    void cancelled();

protected:

private:
    QSlider *radiusSlider_ = nullptr;
    QLabel  *radiusValueLabel_ = nullptr;
};
