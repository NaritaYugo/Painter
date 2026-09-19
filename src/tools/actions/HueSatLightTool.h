#pragma once
#include "tools/core/ToolContext.h"
#include <QOpenGLFunctions_4_3_Core>

// ---------------------------------------------------------------------------
// HueSatLightTool
// ---------------------------------------------------------------------------
// 編集メニューの「色相・彩度・明度」アクション本体。TransformTool/FreeTransformTool
// と違い、キャンバス上でのドラッグ操作は無く、値は HueSatLightPanel (実際のQWidget
// スライダー)から setHue/setSaturation/setLightness() 経由で渡される。
// ピクセル位置は動かさないため、変形ツールのようなスナップショットテクスチャは不要で、
// 確定時は選択範囲マスクに応じて対象ピクセルへその場で(RWで)焼き込むだけでよい。
// ---------------------------------------------------------------------------
class HueSatLightTool : protected QOpenGLFunctions_4_3_Core
{
public:
    void initialize(QOpenGLContext *ctx);

    void activate(ToolContext &ctx);   // アクション開始(値を0にリセット)
    void deactivate();                 // キャンセル(GPUには一切書き込まない)
    void confirm(ToolContext &ctx);    // 焼き込んで終了する

    bool engaged() const { return engaged_; }

    void setHue(int v)        { hue_   = v; }
    void setSaturation(int v) { sat_   = v; }
    void setLightness(int v)  { light_ = v; }
    int  hue()        const { return hue_; }
    int  saturation() const { return sat_; }
    int  lightness()  const { return light_; }

private:
    bool engaged_ = false;
    int  hue_   = 0; // -180..180
    int  sat_   = 0; // -100..100
    int  light_ = 0; // -100..100

    void applyAdjustment(ToolContext &ctx);
};
