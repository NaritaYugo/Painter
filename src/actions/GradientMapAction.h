#pragma once

#include "actions/CanvasAction.h"
#include "tools/actions/GradientMapTool.h"

// ---------------------------------------------------------------------------
// グラデーションマップアクション(Pro限定)。
// ToneCurveAction と同じ「Tool が CPU で組んだ LUT を render.frag が毎フレーム
// サンプルしてプレビューし、確定時にコンピュートシェーダーで焼き込む」構造。
// キャンバス上のハンドルは持たないので、マウス横取りや paintOverlay は不要。
//
// ライセンスゲート(未認証時に ProFeatureDialog を出す)と、無料版ビルドでの
// フォールバックは CanvasWidget::startGradientMapAction() 側に残す
// (無料版ビルドではこのアクション自体が登録されない)。
// ---------------------------------------------------------------------------

class GradientMapPanel;

class GradientMapAction : public CanvasAction
{
public:
    GradientMapAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    void initialize(QOpenGLContext *ctx) override { tool_.initialize(ctx); }
    void positionPanel() override;
    void applyRenderState(QOpenGLShaderProgram *p) override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    GradientMapTool   tool_;
    GradientMapPanel *panel_ = nullptr;
};
