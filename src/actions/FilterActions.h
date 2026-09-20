#pragma once

#include "actions/CanvasAction.h"
#include "tools/actions/GaussianBlurTool.h"
#include "tools/actions/MosaicTool.h"
#include "tools/actions/NoiseTool.h"
#include "tools/actions/CustomShaderTool.h"

// ---------------------------------------------------------------------------
// フィルター系アクション(ガウスぼかし / モザイク / カスタムシェーダー)。
// いずれも Tool がパラメータ変更のたびにコンピュートシェーダーを1回ディスパッチして
// プレビュー用テクスチャ(toolCtx.transformSrcTex)を更新し、render.frag が
// uIsXxxTool + origin/size でそれをサンプルする(詳細は各 Tool.h 参照)。
// 対象は実ピクセルを持つレイヤー(Normal/Text)のみ。
// ---------------------------------------------------------------------------

class GaussianBlurPanel;
class MosaicPanel;
class NoisePanel;
class CustomShaderPanel;

class GaussianBlurAction : public CanvasAction
{
public:
    GaussianBlurAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    void initialize(QOpenGLContext *ctx) override { tool_.initialize(ctx); }
    void positionPanel() override;
    void applyRenderState(QOpenGLShaderProgram *p) override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    GaussianBlurTool  tool_;
    GaussianBlurPanel *panel_ = nullptr;
};

class MosaicAction : public CanvasAction
{
public:
    MosaicAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    void initialize(QOpenGLContext *ctx) override { tool_.initialize(ctx); }
    void positionPanel() override;
    void applyRenderState(QOpenGLShaderProgram *p) override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    MosaicTool  tool_;
    MosaicPanel *panel_ = nullptr;
};

class NoiseAction : public CanvasAction
{
public:
    NoiseAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    void initialize(QOpenGLContext *ctx) override { tool_.initialize(ctx); }
    void positionPanel() override;
    void applyRenderState(QOpenGLShaderProgram *p) override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    NoiseTool  tool_;
    NoisePanel *panel_ = nullptr;
};

class CustomShaderAction : public CanvasAction
{
public:
    CustomShaderAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    void initialize(QOpenGLContext *ctx) override { tool_.initialize(ctx); }
    // 動的コンパイル済みプログラムはGLコンテキストがあるうちに解放する必要がある
    // (CanvasWidgetデストラクタから CanvasActionController::releaseAllGL() 経由で呼ばれる)。
    void releaseGL() override { tool_.releaseGL(); }
    void positionPanel() override;
    void applyRenderState(QOpenGLShaderProgram *p) override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    CustomShaderTool  tool_;
    CustomShaderPanel *panel_ = nullptr;
};
