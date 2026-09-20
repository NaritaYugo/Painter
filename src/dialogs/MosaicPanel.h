#pragma once
#include "dialogs/DraggablePanel.h"

class QSlider;
class QLabel;

// ---------------------------------------------------------------------------
// MosaicPanel
// ---------------------------------------------------------------------------
// フィルターメニュー「モザイク」アクション用のポップアップパネル(ブロックサイズ
// スライダー1本)。GaussianBlurPanelと同様、CanvasWidgetの子として「キャンバス上に」
// 浮かせて表示する非モーダルのQWidget。
// ---------------------------------------------------------------------------
class MosaicPanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit MosaicPanel(QWidget *parent = nullptr);

    void setBlockSize(int blockSize); // スライダーの値を外部からセット(シグナルは出さない)

signals:
    void blockSizeChanged(int blockSize);
    void confirmed();
    void cancelled();

protected:

private:
    QSlider *blockSizeSlider_ = nullptr;
    QLabel  *blockSizeValueLabel_ = nullptr;
};
