#pragma once
#include "dialogs/DraggablePanel.h"

class QSpinBox;
class QCheckBox;

// ---------------------------------------------------------------------------
// ImageResolutionPanel
// ---------------------------------------------------------------------------
// 編集メニュー「画像解像度変更」アクション用のポップアップパネル。
// 幅/高さのQSpinBoxと「縦横比を固定」チェックボックスを持つ。CanvasWidgetの子として
// 「キャンバス上に」浮かせて表示する非モーダルのQWidget。
// 縦横比が固定されている間は、幅を変えると高さが(パネルを開いた時点の比率で)
// 自動追従し、その逆も同様。
// ---------------------------------------------------------------------------
class ImageResolutionPanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit ImageResolutionPanel(QWidget *parent = nullptr);

    // アクション開始時: 現在の解像度にリセットする(縦横比の基準もここで更新される)
    void resetTo(int w, int h);

signals:
    void sizeChanged(int w, int h);
    void confirmed();
    void cancelled();

protected:

private:
    QSpinBox  *widthSpin_  = nullptr;
    QSpinBox  *heightSpin_ = nullptr;
    QCheckBox *lockAspectCheck_ = nullptr;
    double     aspectRatio_ = 1.0; // width / height (resetTo()時点の比率)

    void onWidthChanged(int w);
    void onHeightChanged(int h);
};
