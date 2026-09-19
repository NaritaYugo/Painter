#pragma once
#include "tools/core/ToolContext.h"
#include <QOpenGLFunctions_4_3_Core>

// ---------------------------------------------------------------------------
// BrightnessContrastTool
// ---------------------------------------------------------------------------
// 編集メニューの「明るさ・コントラスト」アクション本体。HueSatLightToolと同様、
// キャンバス上でのドラッグ操作は無く、値はBrightnessContrastPanel(実際のQWidget
// スライダー)から setBrightness/setContrast() 経由で渡される。
// ---------------------------------------------------------------------------
class BrightnessContrastTool : protected QOpenGLFunctions_4_3_Core
{
public:
    void initialize(QOpenGLContext *ctx);

    void activate(ToolContext &ctx);   // アクション開始(値を0にリセット)
    void deactivate();                 // キャンセル(GPUには一切書き込まない)
    void confirm(ToolContext &ctx);    // 焼き込んで終了する

    bool engaged() const { return engaged_; }

    void setBrightness(int v) { brightness_ = v; }
    void setContrast(int v)   { contrast_   = v; }
    int  brightness() const { return brightness_; }
    int  contrast()   const { return contrast_; }

private:
    bool engaged_    = false;
    int  brightness_ = 0; // -100..100
    int  contrast_   = 0; // -100..100

    void applyAdjustment(ToolContext &ctx);
};
