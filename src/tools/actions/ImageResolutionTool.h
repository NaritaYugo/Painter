#pragma once
#include "tools/core/ToolContext.h"

// ---------------------------------------------------------------------------
// ImageResolutionTool
// ---------------------------------------------------------------------------
// 編集メニューの「画像解像度変更」アクション本体。CanvasSizeTool(キャンバスサイズ変更)
// と違い、見た目(縦横比・表示上のサイズ)は変えずに、一辺に詰まっているピクセル数
// (=解像度)だけを変える。つまり全レイヤーの内容を新しい解像度へ拡大縮小(リサンプル)
// する操作で、キャンバス上のドラッグ操作は無く、値はImageResolutionPanel(実際の
// QWidget: 幅/高さのQSpinBox + 縦横比ロック)から setSize() 経由で渡される。
// 確定時はGLWidget::resampleCanvasResolutionへ委譲するだけなので、直接GPU関数は
// 呼ばない(HueSatLightToolと違いQOpenGLFunctionsの継承も不要)。
// ---------------------------------------------------------------------------
class ImageResolutionTool
{
public:
    void activate(ToolContext &ctx);   // アクション開始(現在の解像度にリセット)
    void deactivate();                 // キャンセル(GPUには一切書き込まない)
    void confirm(ToolContext &ctx);    // リサンプルして終了する

    bool engaged() const { return engaged_; }

    void setSize(int w, int h) { w_ = w; h_ = h; }
    int  width()  const { return w_; }
    int  height() const { return h_; }
    int  origWidth()  const { return origW_; }
    int  origHeight() const { return origH_; }

private:
    bool engaged_ = false;
    int  origW_ = 0, origH_ = 0; // アクション開始時点の解像度(縦横比計算の基準)
    int  w_ = 0, h_ = 0;         // 変更後の解像度
};
