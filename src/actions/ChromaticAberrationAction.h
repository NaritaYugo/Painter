#pragma once

#include "actions/CanvasAction.h"
#include "tools/actions/ChromaticAberrationTool.h"

// ---------------------------------------------------------------------------
// 色収差アクション(Pro限定)。
// フィルター系(GaussianBlur/Mosaic等)と同じ「Tool がプレビューテクスチャを更新し
// render.frag がサンプルする」構造に加え、円形モードでは中心ハンドルをキャンバス上で
// ドラッグして指定するため、マウス横取り(handleMousePress/Move/Release)と
// paintOverlay を持つ。
//
// ライセンスゲート(未認証時に ProFeatureDialog を出す)と、無料版ビルドでの
// フォールバックは CanvasWidget::startChromaticAberrationAction() 側に残す
// (無料版ビルドではこのアクション自体が登録されない)。
// ---------------------------------------------------------------------------

class ChromaticAberrationPanel;

class ChromaticAberrationAction : public CanvasAction
{
public:
    ChromaticAberrationAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
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
    ChromaticAberrationTool  tool_;
    ChromaticAberrationPanel *panel_ = nullptr;
};
