#pragma once

#include "actions/CanvasAction.h"
#include "tools/actions/HueSatLightTool.h"
#include "tools/actions/BrightnessContrastTool.h"
#include "tools/actions/ColorBalanceTool.h"
#include "tools/actions/ToneCurveTool.h"

// ---------------------------------------------------------------------------
// 色調整系アクション(色相・彩度・明度 / 明るさ・コントラスト / カラーバランス /
// トーンカーブ)。いずれも「専用パネルの値を受け取り、対応する Tool の uniform を
// 更新して render.frag でライブプレビュー、確定でタイルへ焼き込む」構造。
// 以前は CanvasWidget に startXxx/confirmXxx/cancelXxx/positionXxxPanel として直書き
// されていた(CanvasWidget.cpp の各アクション)。
// ---------------------------------------------------------------------------

class HueSatLightPanel;
class BrightnessContrastPanel;
class ColorBalancePanel;
class ToneCurvePanel;

class HueSatLightAction : public CanvasAction
{
public:
    HueSatLightAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    void initialize(QOpenGLContext *ctx) override { tool_.initialize(ctx); }
    void positionPanel() override;
    void applyRenderState(QOpenGLShaderProgram *p) override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    HueSatLightTool  tool_;
    HueSatLightPanel *panel_ = nullptr;
};

class BrightnessContrastAction : public CanvasAction
{
public:
    BrightnessContrastAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    void initialize(QOpenGLContext *ctx) override { tool_.initialize(ctx); }
    void positionPanel() override;
    void applyRenderState(QOpenGLShaderProgram *p) override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    BrightnessContrastTool  tool_;
    BrightnessContrastPanel *panel_ = nullptr;
};

class ColorBalanceAction : public CanvasAction
{
public:
    ColorBalanceAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    void initialize(QOpenGLContext *ctx) override { tool_.initialize(ctx); }
    void positionPanel() override;
    void applyRenderState(QOpenGLShaderProgram *p) override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    ColorBalanceTool  tool_;
    ColorBalancePanel *panel_ = nullptr;
};

class ToneCurveAction : public CanvasAction
{
public:
    ToneCurveAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    void initialize(QOpenGLContext *ctx) override { tool_.initialize(ctx); }
    void positionPanel() override;
    void applyRenderState(QOpenGLShaderProgram *p) override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    ToneCurveTool  tool_;
    ToneCurvePanel *panel_ = nullptr;
};
