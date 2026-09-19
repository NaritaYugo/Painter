#pragma once

#include "actions/CanvasAction.h"
#include "tools/actions/MotionBlurTool.h"

// ---------------------------------------------------------------------------
// 移動ぼかしアクション。
// フィルター系(GaussianBlur/Mosaic等)と同じ「Tool がプレビューテクスチャを更新し
// render.frag がサンプルする」構造に加え、円形モードでは回転中心のハンドルを
// キャンバス上でドラッグして指定するため、ChromaticAberrationAction と同様に
// マウス横取り(handleMousePress/Move/Release)と paintOverlay を持つ。
// ---------------------------------------------------------------------------

class MotionBlurPanel;

class MotionBlurAction : public CanvasAction
{
public:
    MotionBlurAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    void initialize(QOpenGLContext *ctx) override { tool_.initialize(ctx); }
    void positionPanel() override;
    void applyRenderState(QOpenGLShaderProgram *p) override;
    bool paintOverlay(QPainter &painter, const ToolContext &ctx) override;
    bool handleMousePress  (QMouseEvent *e, ToolContext &ctx) override;
    bool handleMouseMove   (QMouseEvent *e, ToolContext &ctx) override;
    bool handleMouseRelease(QMouseEvent *e, ToolContext &ctx) override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    MotionBlurTool   tool_;
    MotionBlurPanel *panel_ = nullptr;
};
