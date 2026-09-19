#pragma once

#include "actions/CanvasAction.h"
#include "tools/actions/TransformTool.h"
#include "tools/actions/FreeTransformTool.h"

// ---------------------------------------------------------------------------
// 拡大・縮小・回転 / 自由変形 アクション。
// パネルは持たず、キャンバス上のハンドルドラッグ(handleMouseXxx)と
// paintOverlay(枠線・ハンドル・確定/キャンセルボタン)だけで完結する。
// activeTool に関係なくマウスを奪うが、ペン入力そのものはブロックしない
// (blocksToolInput()=false。移動/回転ツールへの切り替えは元コードと同様に許可)。
// render.frag へは engaged() の間だけ uIsXxxTool 等を渡す。
// ---------------------------------------------------------------------------

class TransformAction : public CanvasAction
{
public:
    TransformAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    void initialize(QOpenGLContext *ctx) override { tool_.initialize(ctx); }
    bool blocksToolInput() const override { return false; }
    void applyRenderState(QOpenGLShaderProgram *renderProgram) override;
    bool paintOverlay(QPainter &painter, const ToolContext &ctx) override { tool_.paintOverlay(painter, ctx); return true; }
    bool handleMousePress(QMouseEvent *e, ToolContext &ctx) override {
        tool_.onMousePress(e, ctx);
        if (!tool_.engaged()) active_ = false; // 確定/キャンセルボタンのクリック等でツール側が先に終了した場合に追従する
        return true;
    }
    bool handleMouseMove(QMouseEvent *e, ToolContext &ctx) override { tool_.onMouseMove(e, ctx); return true; }
    bool handleMouseRelease(QMouseEvent *e, ToolContext &ctx) override { tool_.onMouseRelease(e, ctx); return true; }
    bool mapSelectionOutlinePoint(QPointF &canvasPx) const override;
    bool nudge(const QVector2D &delta) override {
        if (!isActive()) return false;
        tool_.nudge(delta);
        return true;
    }
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    TransformTool tool_;
};

class FreeTransformAction : public CanvasAction
{
public:
    FreeTransformAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    void initialize(QOpenGLContext *ctx) override { tool_.initialize(ctx); }
    bool blocksToolInput() const override { return false; }
    void applyRenderState(QOpenGLShaderProgram *renderProgram) override;
    bool paintOverlay(QPainter &painter, const ToolContext &ctx) override { tool_.paintOverlay(painter, ctx); return true; }
    bool handleMousePress(QMouseEvent *e, ToolContext &ctx) override {
        tool_.onMousePress(e, ctx);
        if (!tool_.engaged()) active_ = false;
        return true;
    }
    bool handleMouseMove(QMouseEvent *e, ToolContext &ctx) override { tool_.onMouseMove(e, ctx); return true; }
    bool handleMouseRelease(QMouseEvent *e, ToolContext &ctx) override { tool_.onMouseRelease(e, ctx); return true; }
    bool mapSelectionOutlinePoint(QPointF &canvasPx) const override;
    bool nudge(const QVector2D &delta) override {
        if (!isActive()) return false;
        tool_.nudge(delta);
        return true;
    }
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    FreeTransformTool tool_;
};
