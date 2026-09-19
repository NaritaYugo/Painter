#pragma once
#include "tools/core/ToolContext.h"
#include <QOpenGLFunctions_4_3_Core>

// ---------------------------------------------------------------------------
// ColorBalanceTool
// ---------------------------------------------------------------------------
// 編集メニューの「カラーバランス」アクション本体。HueSatLightToolと同様、
// キャンバス上でのドラッグ操作は無く、値はColorBalancePanel(実際のQWidget
// スライダー C/M/Y)から setCyan/setMagenta/setYellow() 経由で渡される。
// ---------------------------------------------------------------------------
class ColorBalanceTool : protected QOpenGLFunctions_4_3_Core
{
public:
    void initialize(QOpenGLContext *ctx);

    void activate(ToolContext &ctx);   // アクション開始(値を0にリセット)
    void deactivate();                 // キャンセル(GPUには一切書き込まない)
    void confirm(ToolContext &ctx);    // 焼き込んで終了する

    bool engaged() const { return engaged_; }

    void setCyan(int v)    { cyan_    = v; }
    void setMagenta(int v) { magenta_ = v; }
    void setYellow(int v)  { yellow_  = v; }
    int  cyan()    const { return cyan_; }
    int  magenta() const { return magenta_; }
    int  yellow()  const { return yellow_; }

private:
    bool engaged_ = false;
    int  cyan_    = 0; // -100..100
    int  magenta_ = 0; // -100..100
    int  yellow_  = 0; // -100..100

    void applyAdjustment(ToolContext &ctx);
};
