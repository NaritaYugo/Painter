#pragma once

#include "actions/CanvasAction.h"
#include "tools/actions/LensBlurTool.h"

// ---------------------------------------------------------------------------
// レンズぼかしアクション(Pro限定)。
// GaussianBlurAction/MosaicAction と同じ「Tool がプレビューテクスチャを更新し
// render.frag がサンプルする」構造。キャンバス上のハンドルは持たないので、
// マウス横取りや paintOverlay は不要(パネルのみで完結する)。
//
// ライセンスゲート(未認証時に ProFeatureDialog を出す)と、無料版ビルドでの
// フォールバックは GLWidget::startLensBlurAction() 側に残す
// (無料版ビルドではこのアクション自体が登録されない)。
// ---------------------------------------------------------------------------

class LensBlurPanel;

class LensBlurAction : public CanvasAction
{
public:
    LensBlurAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    void initialize(QOpenGLContext *ctx) override { tool_.initialize(ctx); }
    void positionPanel() override;
    void applyRenderState(QOpenGLShaderProgram *p) override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    LensBlurTool   tool_;
    LensBlurPanel *panel_ = nullptr;
};
