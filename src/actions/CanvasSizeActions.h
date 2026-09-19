#pragma once

#include "actions/CanvasAction.h"
#include "tools/actions/CanvasSizeTool.h"
#include "tools/actions/ImageResolutionTool.h"

// ---------------------------------------------------------------------------
// キャンバスサイズ変更 / 画像解像度変更 アクション。
// CanvasSizeActionはCanvasSizePanel(アンカー+数値)に加えキャンバス上のハンドル
// ドラッグでも操作できる(blocksToolInput()=false)。ImageResolutionActionは
// パネルのみで完結する純粋な数値入力アクション(既定のblocksToolInput()=trueのまま)。
// ---------------------------------------------------------------------------

class CanvasSizePanel;
class ImageResolutionPanel;

class CanvasSizeAction : public CanvasAction
{
public:
    CanvasSizeAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    bool blocksToolInput() const override { return false; }
    void positionPanel() override;
    bool paintOverlay(QPainter &painter, const ToolContext &ctx) override { tool_.paintOverlay(painter, ctx); return true; }
    bool handleMousePress(QMouseEvent *e, ToolContext &ctx) override;
    bool handleMouseMove(QMouseEvent *e, ToolContext &ctx) override;
    bool handleMouseRelease(QMouseEvent *e, ToolContext &ctx) override { tool_.onMouseRelease(e, ctx); return true; }
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    CanvasSizeTool  tool_;
    CanvasSizePanel *panel_ = nullptr;
};

class ImageResolutionAction : public CanvasAction
{
public:
    ImageResolutionAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    bool blocksAllToolInput() const override { return true; }
    void positionPanel() override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    ImageResolutionTool   tool_;
    ImageResolutionPanel *panel_ = nullptr;
};
