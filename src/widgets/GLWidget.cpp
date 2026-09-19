#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>

#include "widgets/GLWidget.h"
#include "widgets/NativeWindowLog.h" // 診断ログ(TIEPOLO_WINLOG)。初期化時間の計測に使う
#include "backend/ShaderCache.h"
#include "tools/core/ToolRegistry.h"
#include "tools/core/CursorUtils.h"
#include "tools/core/MaskBrush.h"
#include "dialogs/ProFeatureDialog.h"
#include "components/ThemeColors.h"
#include "actions/AdjustmentActions.h"
#include "actions/FilterActions.h"
#include "actions/MotionBlurAction.h"
#include "actions/TransformActions.h"
#include "actions/CanvasSizeActions.h"
#include "actions/LayerEditActions.h"
#include "actions/FilterLayerEditAction.h"
#ifdef TIEPOLO_PRO_BUILD
#include "backend/LicenseManager.h"
#include "actions/ChromaticAberrationAction.h"
#include "actions/LensBlurAction.h"
#include "actions/GradientMapAction.h"
#endif
#include <QTimer>
#include <QMouseEvent>
#include <QDebug>
#include <QFile>
#include <QVector3D>
#include <QQueue>
#include <QStack>
#include <QPainter>
#include <QPainterPath>
#include <QBitmap>
#include <QGuiApplication>
#include <QSurfaceFormat>
#include <QWindow>
#include <QClipboard>
#include <QMimeData>
#include <cmath>

/*
 * image unit 0  : maskTex        (固定)
 * image unit 1  : アクティブレイヤースライス (固定)
 * image unit 2  : compositeTex   (固定)
 * texture unit 1: layerTexArray  (composite 用 sampler2DArray)
 */

// ===========================================================================
// レイヤー統合(mergeLayers)用のCPU側ブレンド計算
//
// resources/shaders/header/common.glsl のブレンドモード計算と同じ式を
// CPU(pre-multiplied alpha, float)で再現したもの。タイル1枚ぶんの
// RGBA8ピクセル列に対してCPUでまとめて計算し、layerTexArrayへ書き戻す
// (頻度が低い操作なのでコンピュートシェーダ化はせず、既存の
//  readSlicePixels/writeSlicePixels を使って完結させる)。
// ===========================================================================
namespace {

// ストローク色バッファ(render.frag の strokeColorTex)が使うテクスチャユニット。
// 0〜7は個別のテクスチャ、8〜15はレイヤーバンク(LAYER_BANK_TEXUNIT_BASE)で
// 埋まっているため、その次。render.fragの layout(binding = 16) と一致させること。
constexpr int kStrokeColorTexUnit = 16;

struct PremulColor { float r, g, b, a; };

PremulColor toPremul(const uint8_t *p) {
    return { p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f };
}
void fromPremul(const PremulColor &c, uint8_t *out) {
    out[0] = (uint8_t)qBound(0.0f, c.r * 255.0f + 0.5f, 255.0f);
    out[1] = (uint8_t)qBound(0.0f, c.g * 255.0f + 0.5f, 255.0f);
    out[2] = (uint8_t)qBound(0.0f, c.b * 255.0f + 0.5f, 255.0f);
    out[3] = (uint8_t)qBound(0.0f, c.a * 255.0f + 0.5f, 255.0f);
}

PremulColor normalBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    return { fg.r + bg.r * (1.0f - fg.a), fg.g + bg.g * (1.0f - fg.a),
             fg.b + bg.b * (1.0f - fg.a), fg.a + bg.a * (1.0f - fg.a) };
}

float safeUnpremulCpu(float c, float a) { return a > 0.0001f ? c / a : 0.0f; }

PremulColor compositeBlendCpu(const PremulColor &bg, const PremulColor &fg,
                               float Br, float Bg, float Bb) {
    float ao = fg.a + bg.a * (1.0f - fg.a);
    float w  = bg.a * fg.a;
    return {
        fg.r * (1.0f - bg.a) + bg.r * (1.0f - fg.a) + Br * w,
        fg.g * (1.0f - bg.a) + bg.g * (1.0f - fg.a) + Bg * w,
        fg.b * (1.0f - bg.a) + bg.b * (1.0f - fg.a) + Bb * w,
        ao
    };
}

PremulColor multiplyBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg, bgr * fgr, bgg * fgg, bgb * fgb);
}

PremulColor screenBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        1.0f - (1.0f - bgr) * (1.0f - fgr),
        1.0f - (1.0f - bgg) * (1.0f - fgg),
        1.0f - (1.0f - bgb) * (1.0f - fgb));
}

float overlayChannelCpu(float bg, float fg) {
    return bg < 0.5f ? 2.0f * bg * fg : 1.0f - 2.0f * (1.0f - bg) * (1.0f - fg);
}
PremulColor overlayBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        overlayChannelCpu(bgr, fgr), overlayChannelCpu(bgg, fgg), overlayChannelCpu(bgb, fgb));
}

// resources/shaders/header/common.glsl のPhotoshop全ブレンドモード追加分と同じ式を
// CPU(pre-multiplied alpha, float)で再現したもの。GLSL側のchannel関数と1対1対応。

float darkenChannelCpu(float bg, float fg) { return qMin(bg, fg); }
PremulColor darkenBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg, darkenChannelCpu(bgr, fgr), darkenChannelCpu(bgg, fgg), darkenChannelCpu(bgb, fgb));
}

float colorBurnChannelCpu(float bg, float fg) {
    if (fg <= 0.0f) return 0.0f;
    if (bg >= 1.0f) return 1.0f;
    return 1.0f - qMin(1.0f, (1.0f - bg) / fg);
}
PremulColor colorBurnBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        colorBurnChannelCpu(bgr, fgr), colorBurnChannelCpu(bgg, fgg), colorBurnChannelCpu(bgb, fgb));
}

float linearBurnChannelCpu(float bg, float fg) { return qBound(0.0f, bg + fg - 1.0f, 1.0f); }
PremulColor linearBurnBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        linearBurnChannelCpu(bgr, fgr), linearBurnChannelCpu(bgg, fgg), linearBurnChannelCpu(bgb, fgb));
}

float blendLumaCpu(float r, float g, float b) { return 0.299f * r + 0.587f * g + 0.114f * b; }
PremulColor darkerColorBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    bool useBg = blendLumaCpu(bgr, bgg, bgb) <= blendLumaCpu(fgr, fgg, fgb);
    return compositeBlendCpu(bg, fg, useBg ? bgr : fgr, useBg ? bgg : fgg, useBg ? bgb : fgb);
}

PremulColor lightenBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg, qMax(bgr, fgr), qMax(bgg, fgg), qMax(bgb, fgb));
}

float colorDodgeChannelCpu(float bg, float fg) {
    if (fg >= 1.0f) return 1.0f;
    if (bg <= 0.0f) return 0.0f;
    return qMin(1.0f, bg / (1.0f - fg));
}
PremulColor colorDodgeBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        colorDodgeChannelCpu(bgr, fgr), colorDodgeChannelCpu(bgg, fgg), colorDodgeChannelCpu(bgb, fgb));
}

float linearDodgeChannelCpu(float bg, float fg) { return qBound(0.0f, bg + fg, 1.0f); }
PremulColor linearDodgeBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        linearDodgeChannelCpu(bgr, fgr), linearDodgeChannelCpu(bgg, fgg), linearDodgeChannelCpu(bgb, fgb));
}

PremulColor lighterColorBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    bool useBg = blendLumaCpu(bgr, bgg, bgb) >= blendLumaCpu(fgr, fgg, fgb);
    return compositeBlendCpu(bg, fg, useBg ? bgr : fgr, useBg ? bgg : fgg, useBg ? bgb : fgb);
}

float softLightChannelCpu(float bg, float fg) {
    if (fg <= 0.5f) return bg - (1.0f - 2.0f * fg) * bg * (1.0f - bg);
    float d = (bg <= 0.25f) ? ((16.0f * bg - 12.0f) * bg + 4.0f) * bg : std::sqrt(bg);
    return bg + (2.0f * fg - 1.0f) * (d - bg);
}
PremulColor softLightBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        softLightChannelCpu(bgr, fgr), softLightChannelCpu(bgg, fgg), softLightChannelCpu(bgb, fgb));
}

float hardLightChannelCpu(float bg, float fg) {
    return fg < 0.5f ? 2.0f * bg * fg : 1.0f - 2.0f * (1.0f - bg) * (1.0f - fg);
}
PremulColor hardLightBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        hardLightChannelCpu(bgr, fgr), hardLightChannelCpu(bgg, fgg), hardLightChannelCpu(bgb, fgb));
}

float vividLightChannelCpu(float bg, float fg) {
    return fg < 0.5f ? colorBurnChannelCpu(bg, 2.0f * fg) : colorDodgeChannelCpu(bg, 2.0f * (fg - 0.5f));
}
PremulColor vividLightBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        vividLightChannelCpu(bgr, fgr), vividLightChannelCpu(bgg, fgg), vividLightChannelCpu(bgb, fgb));
}

float linearLightChannelCpu(float bg, float fg) { return qBound(0.0f, bg + 2.0f * fg - 1.0f, 1.0f); }
PremulColor linearLightBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        linearLightChannelCpu(bgr, fgr), linearLightChannelCpu(bgg, fgg), linearLightChannelCpu(bgb, fgb));
}

float pinLightChannelCpu(float bg, float fg) {
    return fg < 0.5f ? qMin(bg, 2.0f * fg) : qMax(bg, 2.0f * (fg - 0.5f));
}
PremulColor pinLightBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        pinLightChannelCpu(bgr, fgr), pinLightChannelCpu(bgg, fgg), pinLightChannelCpu(bgb, fgb));
}

float hardMixChannelCpu(float bg, float fg) { return vividLightChannelCpu(bg, fg) < 0.5f ? 0.0f : 1.0f; }
PremulColor hardMixBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        hardMixChannelCpu(bgr, fgr), hardMixChannelCpu(bgg, fgg), hardMixChannelCpu(bgb, fgb));
}

PremulColor differenceBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg, std::abs(bgr - fgr), std::abs(bgg - fgg), std::abs(bgb - fgb));
}

PremulColor exclusionBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        bgr + fgr - 2.0f * bgr * fgr, bgg + fgg - 2.0f * bgg * fgg, bgb + fgb - 2.0f * bgb * fgb);
}

PremulColor subtractBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        qBound(0.0f, bgr - fgr, 1.0f), qBound(0.0f, bgg - fgg, 1.0f), qBound(0.0f, bgb - fgb, 1.0f));
}

float divideChannelCpu(float bg, float fg) { return fg <= 0.0001f ? 1.0f : qBound(0.0f, bg / fg, 1.0f); }
PremulColor divideBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return compositeBlendCpu(bg, fg,
        divideChannelCpu(bgr, fgr), divideChannelCpu(bgg, fgg), divideChannelCpu(bgb, fgb));
}

// Photoshop仕様の非線形Lum/Sat(resources/shaders/header/common.glslのhslSetLum等と同一)
float hslLumCpu(float r, float g, float b) { return 0.3f * r + 0.59f * g + 0.11f * b; }
float hslSatCpu(float r, float g, float b) { return qMax(qMax(r, g), b) - qMin(qMin(r, g), b); }

void hslClipColorCpu(float &r, float &g, float &b) {
    float l = hslLumCpu(r, g, b);
    float n = qMin(qMin(r, g), b);
    float x = qMax(qMax(r, g), b);
    if (n < 0.0f) {
        r = l + (r - l) * (l / qMax(l - n, 1e-6f));
        g = l + (g - l) * (l / qMax(l - n, 1e-6f));
        b = l + (b - l) * (l / qMax(l - n, 1e-6f));
    }
    if (x > 1.0f) {
        l = hslLumCpu(r, g, b);
        n = qMin(qMin(r, g), b);
        x = qMax(qMax(r, g), b);
        r = l + (r - l) * ((1.0f - l) / qMax(x - l, 1e-6f));
        g = l + (g - l) * ((1.0f - l) / qMax(x - l, 1e-6f));
        b = l + (b - l) * ((1.0f - l) / qMax(x - l, 1e-6f));
    }
}

void hslSetLumCpu(float &r, float &g, float &b, float l) {
    float d = l - hslLumCpu(r, g, b);
    r += d; g += d; b += d;
    hslClipColorCpu(r, g, b);
}

void hslSetSatCpu(float &r, float &g, float &b, float s) {
    float c[3] = { r, g, b };
    int maxI = (c[0] >= c[1]) ? ((c[0] >= c[2]) ? 0 : 2) : ((c[1] >= c[2]) ? 1 : 2);
    int minI = (c[0] <= c[1]) ? ((c[0] <= c[2]) ? 0 : 2) : ((c[1] <= c[2]) ? 1 : 2);
    int midI = 3 - maxI - minI;

    if (c[maxI] > c[minI]) {
        c[midI] = (c[midI] - c[minI]) * s / (c[maxI] - c[minI]);
        c[maxI] = s;
    } else {
        c[midI] = 0.0f;
        c[maxI] = 0.0f;
    }
    c[minI] = 0.0f;
    r = c[0]; g = c[1]; b = c[2];
}

PremulColor hueBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    float br = fgr, bgc = fgg, bb = fgb;
    hslSetSatCpu(br, bgc, bb, hslSatCpu(bgr, bgg, bgb));
    hslSetLumCpu(br, bgc, bb, hslLumCpu(bgr, bgg, bgb));
    return compositeBlendCpu(bg, fg, br, bgc, bb);
}

PremulColor saturationBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    float br = bgr, bgc = bgg, bb = bgb;
    hslSetSatCpu(br, bgc, bb, hslSatCpu(fgr, fgg, fgb));
    hslSetLumCpu(br, bgc, bb, hslLumCpu(bgr, bgg, bgb));
    return compositeBlendCpu(bg, fg, br, bgc, bb);
}

PremulColor colorBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    float br = fgr, bgc = fgg, bb = fgb;
    hslSetLumCpu(br, bgc, bb, hslLumCpu(bgr, bgg, bgb));
    return compositeBlendCpu(bg, fg, br, bgc, bb);
}

PremulColor luminosityBlendCpu(const PremulColor &bg, const PremulColor &fg) {
    float bgr = safeUnpremulCpu(bg.r, bg.a), bgg = safeUnpremulCpu(bg.g, bg.a), bgb = safeUnpremulCpu(bg.b, bg.a);
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    float br = bgr, bgc = bgg, bb = bgb;
    hslSetLumCpu(br, bgc, bb, hslLumCpu(fgr, fgg, fgb));
    return compositeBlendCpu(bg, fg, br, bgc, bb);
}

// resources/shaders/header/common.glsl の blendPseudoRand と同じハッシュ関数
float blendPseudoRandCpu(float x, float y) {
    float s = std::sin(x * 12.9898f + y * 78.233f) * 43758.5453123f;
    return s - std::floor(s);
}
PremulColor dissolveBlendCpu(const PremulColor &bg, const PremulColor &fg, float px, float py) {
    if (blendPseudoRandCpu(px, py) >= fg.a) return bg;
    float fgr = safeUnpremulCpu(fg.r, fg.a), fgg = safeUnpremulCpu(fg.g, fg.a), fgb = safeUnpremulCpu(fg.b, fg.a);
    return normalBlendCpu(bg, { fgr, fgg, fgb, 1.0f });
}

PremulColor applyBlendCpu(const PremulColor &bg, const PremulColor &fg, BlendMode mode, float px, float py) {
    switch (mode) {
    case BlendMode::Multiply:     return multiplyBlendCpu(bg, fg);
    case BlendMode::Screen:       return screenBlendCpu(bg, fg);
    case BlendMode::Overlay:      return overlayBlendCpu(bg, fg);
    case BlendMode::Dissolve:     return dissolveBlendCpu(bg, fg, px, py);
    case BlendMode::Darken:       return darkenBlendCpu(bg, fg);
    case BlendMode::ColorBurn:    return colorBurnBlendCpu(bg, fg);
    case BlendMode::LinearBurn:   return linearBurnBlendCpu(bg, fg);
    case BlendMode::DarkerColor:  return darkerColorBlendCpu(bg, fg);
    case BlendMode::Lighten:      return lightenBlendCpu(bg, fg);
    case BlendMode::ColorDodge:   return colorDodgeBlendCpu(bg, fg);
    case BlendMode::LinearDodge:  return linearDodgeBlendCpu(bg, fg);
    case BlendMode::LighterColor: return lighterColorBlendCpu(bg, fg);
    case BlendMode::SoftLight:    return softLightBlendCpu(bg, fg);
    case BlendMode::HardLight:    return hardLightBlendCpu(bg, fg);
    case BlendMode::VividLight:   return vividLightBlendCpu(bg, fg);
    case BlendMode::LinearLight:  return linearLightBlendCpu(bg, fg);
    case BlendMode::PinLight:     return pinLightBlendCpu(bg, fg);
    case BlendMode::HardMix:      return hardMixBlendCpu(bg, fg);
    case BlendMode::Difference:   return differenceBlendCpu(bg, fg);
    case BlendMode::Exclusion:    return exclusionBlendCpu(bg, fg);
    case BlendMode::Subtract:     return subtractBlendCpu(bg, fg);
    case BlendMode::Divide:       return divideBlendCpu(bg, fg);
    case BlendMode::Hue:          return hueBlendCpu(bg, fg);
    case BlendMode::Saturation:   return saturationBlendCpu(bg, fg);
    case BlendMode::Color:        return colorBlendCpu(bg, fg);
    case BlendMode::Luminosity:   return luminosityBlendCpu(bg, fg);
    default:                      return normalBlendCpu(bg, fg);
    }
}

// survivor(bg)にvictim(fg)をvictim自身のopacity/clipping/blendModeで焼き込む。
// survivor側のopacityも同時に焼き込まれる(呼び出し側でsurvivorのopacityは
// 1.0にリセットすること)。
QByteArray mergeTilePixelsCpu(const QByteArray &survivorRaw, const QByteArray &victimRaw,
                               float survivorOpacity, float victimOpacity,
                               bool victimClipping, BlendMode victimMode,
                               int tileOriginX, int tileOriginY)
{
    QByteArray out(survivorRaw.size(), Qt::Uninitialized);
    const uint8_t *bgPix = reinterpret_cast<const uint8_t*>(survivorRaw.constData());
    const uint8_t *fgPix = reinterpret_cast<const uint8_t*>(victimRaw.constData());
    uint8_t *outPix = reinterpret_cast<uint8_t*>(out.data());

    const int pixelCount = survivorRaw.size() / 4;
    for (int i = 0; i < pixelCount; i++) {
        PremulColor bg = toPremul(bgPix + i * 4);
        PremulColor fg = toPremul(fgPix + i * 4);

        bg.r *= survivorOpacity; bg.g *= survivorOpacity; bg.b *= survivorOpacity; bg.a *= survivorOpacity;
        fg.r *= victimOpacity;   fg.g *= victimOpacity;   fg.b *= victimOpacity;   fg.a *= victimOpacity;

        if (victimClipping) {
            // 直近の非クリッピングレイヤー(≒結合先)のアルファでマスクする近似
            float clip = bg.a;
            fg.r *= clip; fg.g *= clip; fg.b *= clip; fg.a *= clip;
        }

        float px = float(tileOriginX + i % TILE_SIZE);
        float py = float(tileOriginY + i / TILE_SIZE);
        PremulColor merged = applyBlendCpu(bg, fg, victimMode, px, py);
        fromPremul(merged, outPix + i * 4);
    }
    return out;
}

} // namespace

// ===========================================================================
// 生成・破棄
// ===========================================================================
GLWidget::GLWidget(ToolConfig *toolCfg, QWidget *parent)
    : QOpenGLWidget(parent),
      toolCfg_(toolCfg),
      penTool_(false, &smoothingStrength, toolCfg_),
      eraserTool_(true, &smoothingStrength, toolCfg_),
      airbrushTool_(toolCfg_)
{
    setMinimumSize(100, 100);
    setMouseTracking(true);
    // ストローク中の部分再描画(paintGL()のシザー処理参照)のため、フレーム間で
    // FBOの内容を保持する。既定のNoPartialUpdateだとpaint間で内容が保証されず、
    // 「変更された矩形だけ描き直して残りは前フレームを使う」ことができない。
    setUpdateBehavior(QOpenGLWidget::PartialUpdate);

    // CanvasDocumentはGLWidgetが所有する(1タブ = 1GLWidget = 1CanvasDocument)。
    // sliceAllocFn/sliceFreeFnはコールバックを保持するだけなので、GLコンテキストが
    // まだ無いこの時点で構築しても問題ない。
    doc_ = std::make_unique<CanvasDocument>(sliceAllocFn(), sliceFreeFn());

    setupToolContext();
    wireDocumentNotifications();

    // アクション群(色調整/フィルター等)のコントローラに host(=this)を渡し、登録する。
    actions_.setHost(*this);
    registerCanvasActions();

    fillTool_.setToolConfig(toolCfg_);
    dropperTool_.setToolConfig(toolCfg_);
    blurTool_.setToolConfig(toolCfg_);
    warpTool_.setToolConfig(toolCfg_);
    selectTool_.setToolConfig(toolCfg_);

    // フレームレート律速バッチ用の定期フラッシュタイマー(GLWidget.hのコメント参照)。
    // ストローク中(mousePressEvent〜mouseReleaseEvent)だけ動かす。
    // このタイマーは「入力が途切れた瞬間」に末尾のスタンプを描くフォールバック。
    // ストローク中の主たる描画駆動はmouseMoveEvent内の時間スロットリングrepaintが
    // 行う(Windowsでは連続入力中WM_TIMERが配信されないため、タイマーはドラッグ中は
    // ほぼ発火せず、指を止めてキューが空いた瞬間に発火して取りこぼしを拾う)。
    inputFlushTimer_ = new QTimer(this);
    connect(inputFlushTimer_, &QTimer::timeout, this, [this]() {
        // 【重要】まだ画面に出していない入力が溜まっているときだけ動く。
        //
        // このタイマーの役目は「入力が途切れて mouseMoveEvent 側の同期repaint()が
        // 来なくなったときに、貯めたままの末尾を描く」ことだけ。溜まっていないなら
        // 画面は既に最新なので、flushもupdateも要らない。
        //
        // 以前は溜まりの有無を見ずに毎回 update() を積んでいた。ペンのように
        // 「連続入力中はWM_TIMERが配信されない」ツールでは滅多に発火しないので
        // 表には出なかったが、移動/回転ツールでは事情が逆になる ―― 1フレームの
        // 同期描画(=ウィンドウ全体の再合成+present)がこの環境で15〜65msかかるため、
        // その間に入力キューが空になってタイマーが必ず配信される。結果、
        //   本物のフレーム → 同じ絵の無駄なフレーム → 本物のフレーム → …
        // と交互に走り、1入力あたりのpresentが2回になっていた(実測でそのとおりの
        // 並びがログに出ている)。ボタンを押したまま指を止めている間も、同じ絵の
        // 再合成が25msごとに永久に続いていた。これがビュー変換のカクつきの主因。
        Tool *t = currentTool();
        if (!t || !t->hasPendingInput()) return;
        makeCurrent();
        t->flushPendingInput(toolCtx_);
        // 直前に同期repaint()が走っているなら、ここで積んでも同じ絵をもう1枚
        // 描き直すだけで、しかもその時点ではダーティ矩形が消費済みのため部分再描画
        // (シザー)が効かず全面描画になる。presentの回数も倍になり、そのぶん入力処理が
        // 止まる。前回の同期描画から間隔ぶん経っているときだけ積む。
        //
        // 基準は strokePaintClock_ ではなく「repaint()が終わった時刻」にすること。
        // strokePaintClock_ は repaint() の開始時に restart される(そちらのコメント
        // 参照)ので、repaint() 自体が間隔より長い場合 ― present待ちを含むと普通に
        // そうなる ― 戻ってきた瞬間には既に間隔を超えており、この判定が素通しに
        // なってしまう。結果、1フレームごとに必ず全面描画がもう1枚積まれていた
        // (実測: 移動ツールのドラッグ中、1フレームに paintGL が2回)。
        if (!lastRepaintDoneClock_.isValid() || lastRepaintDoneClock_.elapsed() >= strokePaintIntervalMs_)
            update();
    });
    inputFlushTimer_->setInterval(12);

    // 事前合成キャッシュの先読み作成(GLWidget.hのcompositeCachePrewarmTimer_参照)。
    // レイヤー操作は連続して起きる(スライダーのドラッグ、複数枚の追加など)ので、
    // 落ち着いてから1回だけ作るようデバウンスする。
    compositeCachePrewarmTimer_ = new QTimer(this);
    compositeCachePrewarmTimer_->setSingleShot(true);
    compositeCachePrewarmTimer_->setInterval(200);
    connect(compositeCachePrewarmTimer_, &QTimer::timeout, this, [this] { prewarmCompositeCaches(); });
}

void GLWidget::wireDocumentNotifications()
{
    if (!doc_) return;

    // doc_ の変更通知を1箇所だけで受け取り、必要な範囲だけ再描画/キャッシュ更新/
    // シグナル発行を行う。各アクションは元の各セッターが個別に呼んでいたものと
    // 1対1で対応させてあり、安易な統合はしていない
    // (例: 空のレーンに切り替えた直後にupdateCaches()を呼ぶとクラッシュするため、
    //  ActiveLayerChangedとRepaintOnlyは意図的に分けてある)。
    doc_->onChanged = [this](CanvasDocument::ChangeKind kind) {
        if (m_initializing || m_suppressDocNotify) return;

        // 事前合成キャッシュ(アクティブより下/上)は「アクティブレイヤーとその上下の
        // スタックが変化しない」間だけ有効。レイヤー切り替え・追加/削除/並べ替え・
        // ブレンド/不透明度/表示切り替え・Undo/Redo など、ストロークの実描画以外の
        // あらゆる文書変更で作り直す必要がある。ここで毎回無効化しておけば、次の
        // ストローク開始時(onMousePress)に必要なら作り直される。
        //
        // 重要: ストローク中・終了時のプレビュー更新(ctx.requestRepaint /
        // notifyLayersChanged / commitStrokeUndo)は doc_->onChanged を一切経由しない
        // ため、ここでの無効化は「連続して短い線を引く」ケースでのキャッシュ保持を
        // 妨げない。同じアクティブレイヤーへ続けて描く限りキャッシュは再利用され、
        // 1ストロークごとに全画面の事前合成をやり直すこと(=書き始めが遅くなる原因)は
        // 起きない。作り直すのはレイヤーを切り替えた等、本当に必要なときだけ。
        invalidateBelowCompositeCache();
        invalidateAboveCompositeCache();
        invalidateFilterChain();
        // 作り直しは「次のストローク開始時」ではなく、ここから少し落ち着いた
        // アイドル時に先回りして行う(GLWidget.hのcompositeCachePrewarmTimer_参照)。
        // 全面ディスパッチのGPU処理をペンを置く前に済ませておくのが狙い。
        scheduleCompositeCachePrewarm();

        switch (kind) {
        case CanvasDocument::ChangeKind::CacheAndNotify:
            update();
            emit layersChanged();
            break;
        case CanvasDocument::ChangeKind::NotifyOnly:
            emit layersChanged();
            break;
        case CanvasDocument::ChangeKind::RepaintOnly:
            update();
            break;
        case CanvasDocument::ChangeKind::ActiveLayerChanged:
            update();
            break;
        }
    };
}

void GLWidget::setupToolContext() {
    toolCtx_.gl   = this;
    toolCtx_.doc  = doc_.get();
    toolCtx_.view = &view_;
    toolCtx_.canvasW = canvasW;
    toolCtx_.canvasH = canvasH;
    toolCtx_.tileSize = TILE_SIZE;
    toolCtx_.wrapX = doc_ ? doc_->wrapX() : false;
    toolCtx_.wrapY = doc_ ? doc_->wrapY() : false;

    syncBanksToToolContext();
    toolCtx_.maskTex       = maskTex;
    toolCtx_.penTipTex     = penTipTex;
    toolCtx_.paperTex      = paperTex;
    toolCtx_.toneCurveLUTTex = toneCurveLUTTex;
    toolCtx_.gradientMapLUTTex = gradientMapLUTTex;
    toolCtx_.strokeStampSSBO = strokeStampSSBO_;
    toolCtx_.brushPaintSSBO  = brushPaintSSBO_;
    toolCtx_.computeBrushStateProgram = computeBrushStateProgram;
    toolCtx_.belowCompositeTex         = belowCompositeTex;
    toolCtx_.belowCompositeClipBaseTex = belowCompositeClipBaseTex;
    toolCtx_.fullLayerTex  = fullLayerTex;
    toolCtx_.selectionMaskTex = selectionMaskTex;
    toolCtx_.transformSrcTex        = transformSrcTex;
    toolCtx_.transformSrcSelMaskTex = transformSrcSelMaskTex;

    toolCtx_.compositedTex     = compositedTex;
    toolCtx_.compositedTileArr = compositedTileArr;

    toolCtx_.computeDrawProgram      = computeDrawProgram;
    toolCtx_.computeBakeProgram      = computeBakeProgram;
    toolCtx_.computeMaskClearProgram = computeMaskClearProgram;
    toolCtx_.computeCompositeProgram = computeCompositeProgram;
    toolCtx_.computeTransformProgram = computeTransformProgram;
    toolCtx_.computeFreeTransformProgram = computeFreeTransformProgram;
    toolCtx_.computeHueSatLightProgram = computeHueSatLightProgram;
    toolCtx_.computeBrightnessContrastProgram = computeBrightnessContrastProgram;
    toolCtx_.computeColorBalanceProgram = computeColorBalanceProgram;
    toolCtx_.computeToneCurveProgram = computeToneCurveProgram;
    toolCtx_.computeGaussianBlurFilterProgram = computeGaussianBlurFilterProgram;
    toolCtx_.computeMosaicReduceProgram = computeMosaicReduceProgram;
    toolCtx_.computeMosaicFilterProgram = computeMosaicFilterProgram;
    toolCtx_.computeMotionBlurFilterProgram = computeMotionBlurFilterProgram;
    toolCtx_.computeNoiseFilterProgram      = computeNoiseFilterProgram;
    toolCtx_.computeChromaticAberrationFilterProgram = computeChromaticAberrationFilterProgram;
    toolCtx_.computeLensBlurFilterProgram            = computeLensBlurFilterProgram;
    toolCtx_.computeGradientMapProgram               = computeGradientMapProgram;
    toolCtx_.resizeCanvasKeepingContent = [this](int w, int h, int ox, int oy) { return resizeCanvasKeepingContent(w, h, ox, oy); };
    toolCtx_.resampleCanvasResolution = [this](int w, int h) { return resampleCanvasResolution(w, h); };
    toolCtx_.growLayerBounds = [this](int layerIndex, int minTx, int minTy, int maxTxEx, int maxTyEx) {
        return growLayerBoundsToCoverCanvasTiles(layerIndex, minTx, minTy, maxTxEx, maxTyEx);
    };
    toolCtx_.ensureTransformScratchSize = [this](int w, int h) { ensureTransformScratchSize(w, h); };
    toolCtx_.ensureFilterScratch = [this](int w, int h) { return ensureFilterScratch(w, h); };

    toolCtx_.ssboLayerOpacity   = ssboLayerOpacity;
    toolCtx_.ssboLayerVisible   = ssboLayerVisible;
    toolCtx_.ssboLayerBaseSlice = ssboLayerBaseSlice;
    toolCtx_.ssboLayerMaskBaseSlice = ssboLayerMaskBaseSlice;
    toolCtx_.ssboLayerAncestorMaskSlices = ssboLayerAncestorMaskSlices;
    toolCtx_.ssboLayerBlendMode = ssboLayerBlendMode;
    toolCtx_.ssboLayerClipping  = ssboLayerClipping;
    toolCtx_.ssboLayerOriginTx  = ssboLayerOriginTx;
    toolCtx_.ssboLayerOriginTy  = ssboLayerOriginTy;
    toolCtx_.ssboLayerTilesX    = ssboLayerTilesX;
    toolCtx_.ssboLayerTilesY    = ssboLayerTilesY;
    toolCtx_.ssboLayerIsSolidColor = ssboLayerIsSolidColor;
    toolCtx_.ssboLayerSolidColor   = ssboLayerSolidColor;
    toolCtx_.ssboLayerAdjKind   = ssboLayerAdjKind;
    toolCtx_.ssboLayerAdjParams = ssboLayerAdjParams;
    toolCtx_.maxLayers          = MAX_LAYERS;

    toolCtx_.viewDpr       = viewDpr();
    toolCtx_.widgetToPixel = [this](const QPointF &p) { return widgetToPixel(p); };
    toolCtx_.pixelToWidget = [this](const QVector2D &canvasPos) -> QPointF {
        // ビューはデバイスピクセルで動くので、ウィジェット座標(論理px)へ戻す
        QVector4D p = view_.matrix() * QVector4D(canvasPos.x(), canvasPos.y(), 0.0f, 1.0f);
        const float d = viewDpr();
        return QPointF(p.x() / d, (viewHeight() - p.y()) / d);
    };
    // ビュー空間(デバイスpx)での中心。回転ツールがここを軸に回す。
    toolCtx_.widgetCenter  = [this] { return QVector2D(viewWidth() / 2.0f, viewHeight() / 2.0f); };
    toolCtx_.setHasSelection = [this](bool v) {
        if (hasSelection_ == v) return;
        hasSelection_ = v;
        emit selectionChanged(hasSelection_);
    };
    toolCtx_.getHasSelection = [this] { return hasSelection_; };
    toolCtx_.invalidateSelectionOutline = [this] { invalidateSelectionOutlineCache(); };
    toolCtx_.beginSelectionUndo  = [this] { beginSelectionUndo(); };
    toolCtx_.commitSelectionUndo = [this](const QByteArray &afterRaw, int x, int y, int w, int h) {
        commitSelectionUndo(afterRaw, x, y, w, h);
    };
    toolCtx_.clearSelection      = [this] { clearSelection(); };
    toolCtx_.getPixelColor = [this](const QPointF &p, bool referenceCanvas) { return getPixelColor(p, referenceCanvas); };
    toolCtx_.activeBrushPreMulColor = [this] {
        return toPreMulColor(toolCfg_->color().rawRGBA(), toolCfg_->pen().opacity());
    };
    toolCtx_.notifyColorDropped = [this](QColor c, bool transparent) {
        emit colorDropperd(c, transparent);
    };
    toolCtx_.ensureStrokeColorTex = [this] { return ensureStrokeColorTex(); };
    toolCtx_.startOrEditTextBox   = [this](int boxIndex) { startOrEditTextBox(boxIndex); };
    toolCtx_.requestTextRasterize = [this](int layerIndex) { scheduleTextRasterize(layerIndex); };
    toolCtx_.beginStrokeUndo          = [this] { beginStrokeUndo(); };
    toolCtx_.expandStrokeUndoRegion   = [this](int a,int b,int c,int d) { expandStrokeUndoRegion(a,b,c,d); };
    toolCtx_.commitStrokeUndo         = [this] { commitStrokeUndo(); };
    toolCtx_.updateBelowCompositeCache     = [this](int upto) {
        if (!WinLog::enabled()) { updateBelowCompositeCache(upto); return; }
        const bool wasValid = belowCompositeCacheValid_;
        QElapsedTimer t; t.start();
        updateBelowCompositeCache(upto);
        WINLOG(QStringLiteral("PERF stroke: belowCache %1 %2ms")
                   .arg(wasValid ? QStringLiteral("hit") : QStringLiteral("BUILD")).arg(t.nsecsElapsed() / 1e6, 0, 'f', 2));
    };
    toolCtx_.invalidateBelowCompositeCache = [this] { invalidateBelowCompositeCache(); };
    toolCtx_.updateAboveCompositeCache     = [this](int active) {
        if (!WinLog::enabled()) { updateAboveCompositeCache(active); return; }
        const bool wasValid = aboveCompositeCacheValid_;
        QElapsedTimer t; t.start();
        updateAboveCompositeCache(active);
        WINLOG(QStringLiteral("PERF stroke: aboveCache %1 %2ms")
                   .arg(wasValid ? QStringLiteral("hit") : QStringLiteral("BUILD")).arg(t.nsecsElapsed() / 1e6, 0, 'f', 2));
    };
    toolCtx_.invalidateAboveCompositeCache = [this] { invalidateAboveCompositeCache(); };
    // MoveTool(パン)/RotateTool(回転)はctx.view->pan()/rotate()を直接呼んだ後
    // ctx.requestRepaint()するだけなので、ここでまとめてviewChanged()も発行する
    // (NavigatorDockの表示範囲枠を追従させるため。他の大多数の呼び出し元にとっては
    // 無害な余分なemitになるだけ)。
    toolCtx_.requestRepaint           = [this] {
        // ストローク中(GLWidget側が一定間隔でrepaint()を直接呼んで駆動している間)は
        // ここでupdate()を積まない。積むと、同じ内容をもう1枚——しかもストローク中の
        // 部分再描画(シザー)が効かない全面描画で——描き直すことになる。present の
        // 回数も倍になり、そのぶんvsync待ちで入力処理が止まる(実測でストローク中、
        // partial=1のフレームと交互にpartial=0のフレームが挟まっていた)。
        // 駆動は mouseMoveEvent 側の時間スロットリングrepaint()と、入力が途切れた
        // ときに末尾を拾う inputFlushTimer_ に任せる。
        Tool *t = currentTool();
        if (!(t && t->isActive() && t->needsCanvasRepaintWhileActive()))
            update();
        // viewChanged() は MoveTool(パン)/RotateTool(回転)がビューを動かしたことを
        // NavigatorDockへ伝えるためのもの。ここは全ツール共通の再描画要求なので、
        // ペンのようにビューを動かさないツールでも入力イベントごとに発行していた。
        // するとNavigatorDockが毎回再描画され、そのトップレベルのバックingストア同期に
        // 巻き込まれてGLWidgetのpaintGL()が「全面」でもう1回呼ばれる(ストローク中の
        // シザー部分再描画が帳消しになるうえ、presentの回数も倍になり、そのぶん
        // vsync待ちで入力処理が止まる)。実際にビュー変換が変わったときだけ発行する。
        const QMatrix4x4 m = view_.matrix();
        if (m == lastEmittedViewMatrix_) return;
        lastEmittedViewMatrix_ = m;

        // 「変わったときだけ」の条件は、ビューを動かさないペン等には効くが、
        // 動かすのが仕事の移動/回転ツールには何の効果も無い ― ドラッグ中は毎回
        // 変わるので毎イベント発行され、上に書いたNavigatorDockの再描画コストを
        // 丸ごと被る。実測(移動ツールでキャンバスをドラッグ)では
        //   paintGL 本体 0.29ms に対して repaint() 全体が 25〜33ms、
        //   1フレームに paintGL が2回、フレーム間隔 45〜60ms(約20fps)
        // となっていて、これが「キャンバスがゆっくり付いてくる」の正体だった。
        //
        // 【重要】ドラッグ中は1回も出さない(間引きではなく完全に止める)。
        //
        // 以前はここを100msに1回まで間引いていたが、それでも足りなかった。この環境で
        // 効くのは「発行の回数」ではなく「ウィンドウを再合成してpresentする回数」で、
        // その1回が実測15〜65msかかる。NavigatorDockのupdate()は自分の枠を描き直す
        // だけの軽い処理に見えて、こちらのrepaint()とは別のタイミングでもう1回
        // ウィンドウ全体の再合成を起こすため、間引いた10回/秒がそのまま
        // 「本物のフレームの合間に挟まる無駄なフレーム10枚/秒」になっていた
        // (実測ログでは viewChanged() の直後に必ず update 由来の paintGL が1枚入る)。
        //
        // NavigatorDockが出しているのは表示範囲の枠とズーム値だけで、ドラッグを
        // 離した時点(mouseReleaseEvent → emitViewChangedIfPending)で必ず1回出すので、
        // 最終的な表示は正しい位置に落ち着く。動かしている最中だけ枠が追従しなくなるが、
        // そのぶんキャンバス自体がポインタに素直に付いてくる方を採る。
        const bool dragging = (t && t->isActive());
        if (dragging) {
            viewChangedPending_ = true;
            return;
        }
        viewChangedPending_ = false;
        emit viewChanged();
    };
    toolCtx_.notifyLayersChanged      = [this] { emit layersChanged(); };
    toolCtx_.noteStrokeDirtyRegion    = [this](float a,float b,float c,float d) { noteStrokeDirtyRegion(a,b,c,d); };
    // 各所でのglBindFramebuffer(ctx.defaultFbo())は、一時FBOでの作業(glReadPixels/
    // glBlitFramebuffer等)が終わった後の後片付け(バインドを外すだけ)のためだけに
    // 使われており、その後Qt自身のpaintGL()が呼ばれる際に改めて自分のFBOを明示的に
    // 束縛し直すため、ここでどのFBOに戻すかは実質どうでもよい。
    // QOpenGLWidget::defaultFramebufferObject()自体は、一部の統合GPUドライバ環境
    // (Intel UHD Graphics 630で確認)でmousePressEvent等のペイントサイクル外から
    // 呼ぶと不定期にクラッシュすることが分かったため、素の0に固定しておく
    // (0はQt自身の描画先ではないが、上記の理由によりここでは無害)。
    toolCtx_.defaultFbo               = [] { return (GLuint)0; };
    toolCtx_.updateCompositedTex      = [this] { updateCompositedTex(); };
}

void GLWidget::freeTextures() {
    // キャンバスサイズ変更(rebuildCanvasFromSnapshots)の直前には、captureAllLayerSnapshots()で
    // 大量のglReadPixelsを発行した直後にここへ来る。glReadPixelsはブロッキングだが、
    // その前後で発行されたテクスチャ確保/削除コマンド自体の完了は保証しないため、
    // 削除→即座に再生成という激しいリソース入れ替えの直前で明示的にglFinish()して
    // GPU側の処理を確実に完了させてから削除する(タイミング依存のクラッシュを防ぐ)。
    glFinish();

    for (int i = 0; i < MAX_TILE_BANKS; i++)
        if (layerTexBanks[i]) { glDeleteTextures(1, &layerTexBanks[i]); layerTexBanks[i] = 0; }
    if (maskTex)         { glDeleteTextures(1, &maskTex);         maskTex         = 0; }
    if (strokeColorTex)  { glDeleteTextures(1, &strokeColorTex);  strokeColorTex  = 0; }
    if (fullLayerTex)    { glDeleteTextures(1, &fullLayerTex);    fullLayerTex    = 0; }
    if (selectionMaskTex){ glDeleteTextures(1, &selectionMaskTex);selectionMaskTex= 0; }
    if (transformSrcTex)        { glDeleteTextures(1, &transformSrcTex);        transformSrcTex        = 0; }
    if (transformSrcSelMaskTex) { glDeleteTextures(1, &transformSrcSelMaskTex); transformSrcSelMaskTex = 0; }
    transformScratchW_ = 0;
    transformScratchH_ = 0;
    if (filterScratchTex_) { glDeleteTextures(1, &filterScratchTex_); filterScratchTex_ = 0; }
    filterScratchW_ = 0;
    filterScratchH_ = 0;
    freeFilterChainTextures();
    if (wallTex)         { glDeleteTextures(1, &wallTex);         wallTex         = 0; }
    if (outerJfaTex)     { glDeleteTextures(1, &outerJfaTex);     outerJfaTex     = 0; }
    if (innerJfaTex)     { glDeleteTextures(1, &innerJfaTex);     innerJfaTex     = 0; }
    if (sdfTex)          { glDeleteTextures(1, &sdfTex);          sdfTex          = 0; }
    if (compositedTex)       { glDeleteTextures(1, &compositedTex);       compositedTex       = 0; }
    if (compositedTileArr)   { glDeleteTextures(1, &compositedTileArr);   compositedTileArr   = 0; }
    if (belowCompositeTex)         { glDeleteTextures(1, &belowCompositeTex);         belowCompositeTex         = 0; }
    if (belowCompositeClipBaseTex) { glDeleteTextures(1, &belowCompositeClipBaseTex); belowCompositeClipBaseTex = 0; }
    if (aboveCompositeTex)          { glDeleteTextures(1, &aboveCompositeTex);          aboveCompositeTex          = 0; }
    if (aboveCompositeClipScratch_) { glDeleteTextures(1, &aboveCompositeClipScratch_); aboveCompositeClipScratch_ = 0; }
    belowCompositeCacheValid_ = false;

    GLuint ssbos[] = { ssboLayerOpacity, ssboLayerVisible, ssboLayerBaseSlice, ssboLayerMaskBaseSlice,
                   ssboLayerAncestorMaskSlices,
                   ssboLayerBlendMode, ssboLayerClipping,
                   ssboLayerOriginTx, ssboLayerOriginTy, ssboLayerTilesX, ssboLayerTilesY,
                   ssboLayerIsSolidColor, ssboLayerSolidColor,
                   ssboLayerAdjKind, ssboLayerAdjParams };
    glDeleteBuffers(15, ssbos);
}

GLWidget::~GLWidget() {
    // 一度も表示されなかったGLWidgetでは initializeGL() が走っていない。
    //
    // そのときは initializeOpenGLFunctions() も呼ばれていないので、QOpenGLFunctions
    // の関数テーブルが未解決のまま。この状態で glFinish() などを呼ぶと未解決の
    // ポインタを辿ってアクセス違反で落ちる。実際 MainWindow::createWidgets() が
    // Dockのコンストラクタ用に作る deckGLWidget_ がまさにこれで、アプリを終了する
    // たびに毎回ここで落ちていた(終了コード 0xC0000005。ウィンドウは先に閉じ、
    // 設定の保存も済んでいるので気づきにくいが、終了が遅くなる原因になっていた)。
    //
    // initializeGL() が走っていなければGL資源も1つも作られていないので、
    // 解放すべきものは何も無い。まるごと飛ばす。
    if (!glFunctionsReady_) {
        WINLOG(QStringLiteral("~GLWidget: initializeGL()未実行のためGLの後始末は不要"));
        return;
    }

    makeCurrent();
    actions_.releaseAllGL(); // CustomShaderActionの動的コンパイル済みプログラム等をcontextがあるうちに解放する

    // シェーダープログラムはここで delete しないこと。initializeGL() の
    // programCache がプロセス全体で1組だけ持ち、全タブで共有している
    // (共有グループ内ではプログラムを使い回せる。詳細はそちらのコメント参照)。
    // 以前はここで全部 delete していたが、共有した状態でそれをやると
    // 「1つのタブを閉じただけで他のタブのプログラムが壊れる」ことになる。
    // GL資源はプロセス終了時、共有グループの破棄と一緒に解放される。

    freeTextures();
    if (penTipTex) glDeleteTextures(1, &penTipTex);
    if (paperTex) glDeleteTextures(1, &paperTex);
    if (toneCurveLUTTex) glDeleteTextures(1, &toneCurveLUTTex);
    if (gradientMapLUTTex) glDeleteTextures(1, &gradientMapLUTTex);
    if (dummyVAO) glDeleteVertexArrays(1, &dummyVAO);
    doneCurrent();
}

// ===========================================================================
// ツール / ブラシ API
// ===========================================================================
void GLWidget::setActiveTool(ToolType tool)
{
    // 色調整/フィルター系パネル(色相・彩度・明度/明るさ・コントラスト/カラー
    // バランス/ガウスぼかし/モザイク)が開いている間は、移動・回転ツール以外への
    // 切り替えを無視する(パネルを開いたまま視点だけ調整できるようにするため。
    // それ以外のツールショートカットは事実上無効になる)。Enter/Escapeは
    // confirm/cancelの別経路で処理されるためこのガードの対象外。
    // 色調整/フィルター系パネル(controller管理アクション)が入力をブロックしている間は、
    // 移動・回転ツール以外への切り替えを無視する(パネルを開いたまま視点操作できるように)。
    const bool colorPanelActive = actions_.toolInputBlocked();
    if (colorPanelActive && tool != ToolType::Move && tool != ToolType::Rotate) return;

    // 他のツールに切り替えたら、実行中の変形アクションは破棄して終了する。
    // 色調整/フィルター系パネルは、上のガードにより「移動/回転へ切替」時しかここに来ないため、
    // その場合はパネルを開いたまま(cancelしない)にする
    // (colorPanelActiveなら現在のcontroller管理アクションは色調整/フィルター系のはずなので触らない)。
    if (!colorPanelActive) actions_.cancelActive();

    previousTool = activeTool;
    activeTool   = tool;
    emit activeToolChanged(tool);
    updateCursor();
    update();
}

void GLWidget::returnToPreviousTool()
{
    setActiveTool(previousTool);
}

// 色調整/フィルター系パネル(移動/回転ツールへの切替だけ素通しするタイプ)を開く
// 直前に呼ばれる。開いた瞬間からキャンバスを動かせるよう、その場で移動ツールへ
// 自動的に切り替える(回転ツールへはこれまで通りショートカットで切り替えられる)。
void GLWidget::hostNotifyToolBlockingActionStarted()
{
    toolBeforeBlockingAction_ = activeTool;
    toolBeforeBlockingActionSaved_ = true;
    setActiveTool(ToolType::Move);
}

// 上記パネルが確定/キャンセルで閉じた直後に呼ばれる。パネルを開く前に使っていた
// ツールへ戻す(パネル表示中に移動/回転ツールへ切り替えていた場合も、それは無視して
// 開く前のツールへ戻す)。
void GLWidget::hostNotifyToolBlockingActionEnded()
{
    if (!toolBeforeBlockingActionSaved_) return;
    toolBeforeBlockingActionSaved_ = false;
    setActiveTool(toolBeforeBlockingAction_);
}

void GLWidget::setActiveToolPreset(ToolType tool, int index)
{
    if (IToolPresetList *list = toolCfg_->toolPresetList(tool))
        list->setActiveIndex(index);

    if (activeTool != tool) {
        setActiveTool(tool); // ToolType自体も切り替わる場合はactiveToolChanged側に委ねる
        return;
    }
    emit activeToolPresetChanged(tool, index);
    updateCursor();
    update();
}

// ===========================================================================
// カーソル
// ---------------------------------------------------------------------------
// currentTool()->cursor() がカスタムカーソル(ペンの円、スポイトの色プレビュー等)を
// 返せばそれを使い、std::nullopt ならToolRegistryに登録されたアイコンを
// カーソルとして使う(新しいツールを追加した際、cursor()をオーバーライドしなくても
// 自動でそれらしいカーソルになる)。
// ===========================================================================
void GLWidget::updateCursor()
{
    // 色調整/フィルター系パネル表示中は、移動・回転ツールに切り替えていればそのツール
    // 本来のカーソルを見せる(実際に操作できるため)。それ以外(変形/キャンバスサイズ等の
    // 他のアクション中、あるいは移動・回転以外のツールのまま)は通常の矢印カーソルにする。
    const bool colorPanelActive = actions_.toolInputBlocked();
    // 変形/自由変形/キャンバスサイズ変更は blocksToolInput()=false(ハンドルドラッグの
    // ためキャンバスへのマウス入力自体は必要)だが、カーソル表示上は他のアクション同様
    // 通常の矢印にしたいので、ここでは isActive() で判定する(入力ブロック判定とは別軸)。
    // 画像解像度変更/レイヤー編集系パネルはblocksAllToolInput()=true(移動・回転への
    // 切替も許さない)なので、色調整/フィルター系と違って常に矢印カーソルにする。
    const bool otherActionBlocking = (actions_.isBusy() && !colorPanelActive)
        || actions_.toolInputFullyBlocked();
    if (otherActionBlocking ||
        (colorPanelActive && activeTool != ToolType::Move && activeTool != ToolType::Rotate)) {
        applyUnsetCursor();
        return;
    }
    if (Tool *t = currentTool()) {
        if (std::optional<QCursor> c = t->cursor(toolCtx_)) {
            applyCursor(*c);
            return;
        }
    }
    for (const ToolTypeMeta &meta : toolTypeRegistry()) {
        if (meta.type == activeTool) {
            const QString &cursorPath = meta.cursorIconPath.isEmpty() ? meta.iconPath : meta.cursorIconPath;
            applyCursor(CursorUtils::makeIconCursor(cursorPath, 24));
            return;
        }
    }
    applyUnsetCursor();
}

// QCursorには比較演算子が無いので、「同じ見た目か」を安く判定するための識別子を作る。
// 画像カーソルはQPixmapのcacheKey(暗黙共有なのでコピーは浅く、cacheKey取得も安い)、
// 標準カーソルは形状値から作る。CursorUtils::makeCircleCursor()等は同じ引数に対して
// キャッシュ済みの同一QCursorを返すため、見た目が変わらない限り同じ値になる。
static qint64 cursorIdentity(const QCursor &c)
{
    if (c.shape() != Qt::BitmapCursor)
        return -(qint64)c.shape() - 1; // 標準カーソル(画像カーソルのcacheKey>0と衝突しない負値)
    return c.pixmap().cacheKey();
}

// 【重要・ペンタブの遅延対策】カーソルが実際に変化したときだけOSへ反映する。
//
// updateCursor()はマウス/ペンの移動イベントごとに呼ばれるが、ストローク中に
// カーソルの見た目が変わることはまずない。にもかかわらず毎回setCursor()＋
// forceCursorRedrawIfUnderMouse()を実行していると、後者の中で呼ぶ
// QCursor::setPos()(= Win32 SetCursorPos)が入力イベントを1件システムの入力
// キューへ注入してしまう。
//
// これはペンタブでだけ深刻な害になる:
//  - マウスのWM_MOUSEMOVEはWindowsが間引く(キューに高々1件)ため、同じ座標への
//    SetCursorPosはほぼ無害。
//  - ペンはパケットが間引かれず200Hz超で届くので、1パケットごとに1件ずつ
//    入力イベントが注入され続ける。結果として「入力キューが空にならない」状態が
//    自作自演で維持され、低優先度メッセージであるWM_PAINT/WM_TIMERが配信されなく
//    なる(このファイルのmouseMoveEvent()のコメント参照)。「ペンだと線が遅れて
//    ついてくる/止めた瞬間にまとめて出る」の主因。
//  - さらにSetCursorPosはプロセス横断で直列化される重いシステムコールで、
//    ペンのデジタイザが決めたカーソル位置と毎回競合する。
//
// 元々forceCursorRedrawIfUnderMouse()が必要だったのは「ブラシサイズを変えたのに
// ペンが静止していて新しいカーソル画像に切り替わらない」ケース、つまりカーソルが
// 変化したときだけ。移動中はポインタ自体が動いておりOSが自然に再評価するので不要。
// よって変化検出を挟むことで、当初の不具合を直したまま注入を完全に止められる。
void GLWidget::applyCursor(const QCursor &c)
{
    const qint64 id = cursorIdentity(c);
    if (id == appliedCursorId_) return; // 見た目が同じ: OSには一切触らない
    appliedCursorId_ = id;
    setCursor(c);
    forceCursorRedrawIfUnderMouse();
}

void GLWidget::applyUnsetCursor()
{
    if (appliedCursorId_ == 0) return;
    appliedCursorId_ = 0;
    unsetCursor();
}

// Windowsでは、setCursor()でウィジェットのカーソルを変えても、OSは
// 「WM_SETCURSORを受け取ったとき」にしかカーソル画像を評価し直さない。
// WM_SETCURSORはマウスメッセージに伴って飛ぶので、
//   ・マウス使用時   … わずかな手ぶれで即座に飛ぶため、ほぼ問題にならない
//   ・ペンタブ使用時 … ペン先が静止していれば当然飛ばないし、さらにこのアプリは
//                      ストローク中のレガシーマウスメッセージを止めている
//                      (tabletEvent()の直接駆動)ため、ペンを動かしていても
//                      飛んでこないことがある
// という差が出る。結果として「ショートカットでブラシサイズやツールを変えたのに、
// 実際の設定だけ変わってカーソルの絵が古いまま」という症状になる。キャンバス外の
// UIへ一度出して戻したりマウスで動かすと直るのは、そこで実マウスメッセージが
// 発生してWM_SETCURSORが飛ぶため。
//
// そこで、カーソルが実際に変化したとき(applyCursor参照)だけ、OSへ明示的に
// 再評価を要求する。
void GLWidget::forceCursorRedrawIfUnderMouse()
{
#ifdef Q_OS_WIN
    // カーソルがこのウィジェット上にあるときだけ行う(他のウィジェット上にある間に
    // 呼んでも無意味なうえ、下のQCursor::setPos()が無関係なウィンドウへ影響する)。
    //
    // 判定にQWidget::underMouse()は使えない。あれはQtのenter/leave追跡に基づくが、
    // その追跡は実マウスイベント由来であり、ペンタブ直接駆動中はまさにそれを
    // 止めているため false のままになることがある(この関数が呼ばれても素通りして
    // しまい、上記の症状が直らない原因そのもの)。座標で直接判定する。
    const QPoint globalPos = QCursor::pos();
    if (!rect().contains(mapFromGlobal(globalPos))) return;

    // (a) 本命: 自分のトップレベルウィンドウへWM_SETCURSORを送り、カーソル画像を
    //     評価し直させる。Qtのウィンドウプロシージャがこれを受けて、ポインタ下の
    //     ウィジェットのカーソルを適用してくれる。入力イベントを注入しないので、
    //     ペン入力のキューを乱さない(applyCursorのコメント参照)。
    //     winId()はトップレベル(既にネイティブ)に対して呼ぶ。QOpenGLWidget自身に
    //     対して呼ぶとネイティブウィンドウ化を強制してしまい、合成方法が変わるため避ける。
    if (QWidget *top = window()) {
        if (QWindow *wh = top->windowHandle()) {
            HWND hwnd = reinterpret_cast<HWND>(wh->winId());
            if (hwnd)
                SendMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        }
    }

    // (b) 保険: カーソル位置を現在位置へ「0px移動」させ、実際にポインタを動かさずに
    //     OSへ再評価させる。(a)が効かない環境(ドライバがカーソルを独自に描いている等)
    //     向けのフォールバック。カーソルが変化したときにしか通らないので、以前のように
    //     ペンのパケットごとに入力を注入してしまう問題は起きない。
    QCursor::setPos(globalPos);
#endif
}


// ===========================================================================
// 選択範囲
// ===========================================================================
// ---------------------------------------------------------------------------
// 選択範囲のUndo/Redo
// ---------------------------------------------------------------------------
// マスクはR8でキャンバス全域ぶんあるが、実際には0か255が大きな塊で続くだけなので
// qCompressでよく縮む(通常の選択なら数KB程度)。履歴に何十件積んでも問題にならない。
QByteArray GLWidget::captureSelectionMask()
{
    if (selectionMaskTex == 0 || canvasW <= 0 || canvasH <= 0) return QByteArray();
    // 選択マスクはcompute shaderのimageStoreでも書かれる(変形の焼き込み等)。
    // 読み戻しの前にバリア+完了待ちが必要(SelectTool::finishPenSelectの詳しいコメント参照)。
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glFinish();

    QVector<uint8_t> buf(canvasW * canvasH, 255);
    glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, buf.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    // ここでは圧縮しない(生データを返す)。圧縮はcommit時に「実際に変わった矩形」
    // だけに対して行うので、全域2MBを毎回圧縮する必要がなくなった。
    return QByteArray(reinterpret_cast<const char *>(buf.constData()), buf.size());
}

void GLWidget::restoreSelectionMaskRect(const QByteArray &compressed, int x, int y, int w, int h, bool hasSel)
{
    if (selectionMaskTex == 0 || compressed.isEmpty() || w <= 0 || h <= 0) return;
    const QByteArray raw = qUncompress(compressed);
    if (raw.size() != (qsizetype)w * h) return; // 記録時とサイズが合わない: 復元しない
    if (x < 0 || y < 0 || x + w > canvasW || y + h > canvasH) return;

    glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RED, GL_UNSIGNED_BYTE, raw.constData());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glMemoryBarrier(GL_ALL_BARRIER_BITS); // 以降のcompute shaderのimageLoadから見えるようにする
    invalidateSelectionOutlineCache();

    if (hasSelection_ != hasSel) {
        hasSelection_ = hasSel;
        emit selectionChanged(hasSelection_);
    }
    update();
}

void GLWidget::beginSelectionUndo()
{
    if (!glReady_) return;
    makeCurrent();
    pendingSelectionUndoBefore_ = captureSelectionMask();
    pendingSelectionUndoHadSel_ = hasSelection_;
    pendingSelectionUndoValid_  = !pendingSelectionUndoBefore_.isEmpty();
}

void GLWidget::commitSelectionUndo(const QByteArray &afterRaw, int x, int y, int w, int h)
{
    if (!pendingSelectionUndoValid_) return;
    pendingSelectionUndoValid_ = false; // 空振りでも必ず降ろす(次のbeginまで持ち越さない)

    if (w <= 0 || h <= 0) return;                       // 変化なし
    if (x < 0 || y < 0 || x + w > canvasW || y + h > canvasH) return;
    if (pendingSelectionUndoBefore_.size() != (qsizetype)canvasW * canvasH) return;

    // 呼び出し側が「貼ったばかりの矩形のマスク」を持っているならそれを使い、
    // 無ければGPUから読み戻す(読み戻しは重いので基本は前者を使う)。
    QByteArray afterRect = afterRaw;
    if (afterRect.size() != (qsizetype)w * h) {
        makeCurrent();
        const QByteArray full = captureSelectionMask();
        if (full.size() != (qsizetype)canvasW * canvasH) return;
        afterRect.resize((qsizetype)w * h);
        for (int r = 0; r < h; r++)
            memcpy(afterRect.data() + (qsizetype)r * w,
                   full.constData() + (qsizetype)(y + r) * canvasW + x, w);
    }

    // 「変更前」も同じ矩形だけ切り出す。全域を持つのではなく矩形だけにすることで、
    // 圧縮コストもメモリも「実際に編集した面積」に比例するようになる。
    QByteArray beforeRect((qsizetype)w * h, Qt::Uninitialized);
    for (int r = 0; r < h; r++)
        memcpy(beforeRect.data() + (qsizetype)r * w,
               pendingSelectionUndoBefore_.constData() + (qsizetype)(y + r) * canvasW + x, w);

    // 中身も選択状態も変わっていなければ履歴を汚さない
    if (beforeRect == afterRect && hasSelection_ == pendingSelectionUndoHadSel_) return;

    UndoEntry entry;
    entry.kind = UndoKind::Selection;
    entry.selection.rectX = x; entry.selection.rectY = y;
    entry.selection.rectW = w; entry.selection.rectH = h;
    // レベル1で十分縮む(0/255が塊で続くデータ)。矩形だけなので元々小さい。
    entry.selection.beforeMask = qCompress(reinterpret_cast<const uchar *>(beforeRect.constData()),
                                            beforeRect.size(), 1);
    entry.selection.afterMask  = qCompress(reinterpret_cast<const uchar *>(afterRect.constData()),
                                            afterRect.size(), 1);
    entry.selection.hadSelectionBefore = pendingSelectionUndoHadSel_;
    entry.selection.hasSelectionAfter  = hasSelection_;
    entry.selection.maskW = canvasW;
    entry.selection.maskH = canvasH;
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());
}

void GLWidget::applySelectionUndoEntry(const UndoEntry &entry, bool toBefore)
{
    makeCurrent();
    const SelectionUndoData &sel = entry.selection;
    restoreSelectionMaskRect(toBefore ? sel.beforeMask : sel.afterMask,
                             sel.rectX, sel.rectY, sel.rectW, sel.rectH,
                             toBefore ? sel.hadSelectionBefore : sel.hasSelectionAfter);
}

void GLWidget::clearSelection()
{
    if (!hasSelection_ && selectionMaskTex == 0) return;
    makeCurrent();
    beginSelectionUndo();

    QVector<uint8_t> fullSel(canvasW * canvasH, 255);
    glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvasW, canvasH,
                    GL_RED, GL_UNSIGNED_BYTE, fullSel.constData());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glMemoryBarrier(GL_ALL_BARRIER_BITS); // 以降のcompute shaderのimageLoadから確実に見えるようにする
    invalidateSelectionOutlineCache();

    if (hasSelection_) {
        hasSelection_ = false;
        emit selectionChanged(false);
    }
    // 貼ったばかりのマスク(全域255)をそのまま渡して読み戻しを1回省く
    commitSelectionUndo(QByteArray(reinterpret_cast<const char *>(fullSel.constData()), fullSel.size()),
                        0, 0, canvasW, canvasH);
    update();
}

void GLWidget::selectAll()
{
    if (!doc_ || doc_->layers.isEmpty()) return;
    makeCurrent();
    beginSelectionUndo();

    // clearSelection()と同じ「全域255」のマスクを敷く。選択範囲マスク自体が
    // キャンバスサイズなので、レイヤーがキャンバスよりはみ出ていてもその部分は
    // 選択されない(=キャンバス範囲のみを選択したことになる)。
    QVector<uint8_t> fullSel(canvasW * canvasH, 255);
    glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvasW, canvasH,
                    GL_RED, GL_UNSIGNED_BYTE, fullSel.constData());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    invalidateSelectionOutlineCache();

    if (!hasSelection_) {
        hasSelection_ = true;
        emit selectionChanged(true);
    }
    // 貼ったばかりのマスク(全域255)をそのまま渡して読み戻しを1回省く
    commitSelectionUndo(QByteArray(reinterpret_cast<const char *>(fullSel.constData()), fullSel.size()),
                        0, 0, canvasW, canvasH);
    update();
}


namespace {
const char *kPasteOriginMimeType = "application/x-tiepolo-paste-origin";
}

void GLWidget::copySelection()
{
    if (!doc_ || doc_->layers.isEmpty()) return;
    const Layer &activeLayer = doc_->activeLayer();
    // 単色/調整レイヤーは実ピクセルを持たないためコピー対象にできない。
    if (activeLayer.layerType != LayerType::Normal && activeLayer.layerType != LayerType::Text) return;

    makeCurrent();

    int rectX, rectY, rectW, rectH;
    QVector<uint8_t> maskBuf; // hasSelection_のときだけ使う(キャンバスサイズ)

    if (hasSelection_) {
        maskBuf.resize(canvasW * canvasH);
        glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, maskBuf.data());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, 0);

        int minX = canvasW, maxX = -1, minY = canvasH, maxY = -1;
        for (int y = 0; y < canvasH; y++) {
            for (int x = 0; x < canvasW; x++) {
                if (maskBuf[y * canvasW + x] == 0) continue;
                minX = qMin(minX, x); maxX = qMax(maxX, x);
                minY = qMin(minY, y); maxY = qMax(maxY, y);
            }
        }
        if (maxX < minX) return; // 選択範囲が空
        rectX = minX; rectY = minY; rectW = maxX - minX + 1; rectH = maxY - minY + 1;
    } else {
        // レイヤーの全内容(キャンバス外にはみ出た部分も含む)
        rectX = activeLayer.originTx * TILE_SIZE;
        rectY = activeLayer.originTy * TILE_SIZE;
        rectW = activeLayer.tilesX() * TILE_SIZE;
        rectH = activeLayer.tilesY() * TILE_SIZE;
    }
    if (rectW <= 0 || rectH <= 0) return;

    QImage img(rectW, rectH, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    for (int ty = 0; ty < activeLayer.tilesY(); ty++) {
        for (int tx = 0; tx < activeLayer.tilesX(); tx++) {
            const int canvasTx = activeLayer.originTx + tx;
            const int canvasTy = activeLayer.originTy + ty;
            const int tilePxX  = canvasTx * TILE_SIZE;
            const int tilePxY  = canvasTy * TILE_SIZE;
            const int ox0 = qMax(tilePxX, rectX), oy0 = qMax(tilePxY, rectY);
            const int ox1 = qMin(tilePxX + TILE_SIZE, rectX + rectW);
            const int oy1 = qMin(tilePxY + TILE_SIZE, rectY + rectH);
            if (ox0 >= ox1 || oy0 >= oy1) continue;

            const QByteArray raw = readSlicePixels(activeLayer.tiles[ty][tx]);
            for (int y = oy0; y < oy1; y++) {
                const uchar *srcRow = reinterpret_cast<const uchar*>(raw.constData())
                                    + ((y - tilePxY) * TILE_SIZE + (ox0 - tilePxX)) * 4;
                uchar *dstRow = img.scanLine(y - rectY) + (ox0 - rectX) * 4;
                memcpy(dstRow, srcRow, (ox1 - ox0) * 4);
            }
        }
    }

    if (hasSelection_) {
        // 選択範囲の形状でマスクする(bbox内でも非選択部分は透過にする)
        for (int y = 0; y < rectH; y++) {
            uchar *row = img.scanLine(y);
            const uint8_t *maskRow = maskBuf.constData() + (size_t)(y + rectY) * canvasW + rectX;
            for (int x = 0; x < rectW; x++) {
                if (maskRow[x] == 0) memset(row + x * 4, 0, 4);
            }
        }
    }

    // タイル格納の行順(原点左下・Y上向き)を、標準画像/OSクリップボードが期待する
    // 行順(原点左上・Y下向き)へ変換してからクリップボードへ渡す
    // (loadImageAsSingleLayer等と対の反転。ここが無いとソフト外へコピーした画像や
    // ソフト外からの貼り付けが上下逆になる)。
    img = img.mirrored(false, true);

    QMimeData *mime = new QMimeData();
    mime->setImageData(img);
    mime->setData(QString::fromLatin1(kPasteOriginMimeType),
                  QByteArray::number(rectX) + ',' + QByteArray::number(rectY));
    QGuiApplication::clipboard()->setMimeData(mime);
}

void GLWidget::pasteClipboard()
{
    if (!doc_ || doc_->layers.isEmpty()) return;
    const int layerIndex = doc_->activeLayerIndex();
    if (doc_->layerRef(layerIndex).layerType != LayerType::Normal
        && doc_->layerRef(layerIndex).layerType != LayerType::Text) return;

    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasImage()) return;
    // 標準画像/OSクリップボードの行順(原点左上・Y下向き)を、タイル格納が期待する
    // 行順(原点左下・Y上向き)へ変換する(loadImageAsSingleLayer等と対の反転。
    // これが無いとソフト外からの貼り付けが上下逆になる)。copySelection()側も
    // 対称に反転しているので、本ソフト内でのコピー&ペーストは従来通り正しい向き。
    const QImage img = qvariant_cast<QImage>(mime->imageData()).convertToFormat(QImage::Format_RGBA8888_Premultiplied).mirrored(false, true);
    if (img.isNull() || img.width() <= 0 || img.height() <= 0) return;

    int pasteX, pasteY;
    bool hasOrigin = false;
    if (mime->hasFormat(QString::fromLatin1(kPasteOriginMimeType))) {
        const QList<QByteArray> parts = mime->data(QString::fromLatin1(kPasteOriginMimeType)).split(',');
        if (parts.size() == 2) {
            bool ok1 = false, ok2 = false;
            const int x = parts[0].toInt(&ok1), y = parts[1].toInt(&ok2);
            if (ok1 && ok2) { pasteX = x; pasteY = y; hasOrigin = true; }
        }
    }
    if (!hasOrigin) {
        // 外部由来の画像等、コピー元位置が無い場合はキャンバス中央へ貼り付ける
        pasteX = (canvasW - img.width()) / 2;
        pasteY = (canvasH - img.height()) / 2;
    }

    makeCurrent();

    const int minTx   = qFloor((double)pasteX / TILE_SIZE);
    const int minTy   = qFloor((double)pasteY / TILE_SIZE);
    const int maxTxEx = qCeil((double)(pasteX + img.width()) / TILE_SIZE);
    const int maxTyEx = qCeil((double)(pasteY + img.height()) / TILE_SIZE);
    growLayerBoundsToCoverCanvasTiles(layerIndex, minTx, minTy, maxTxEx, maxTyEx);

    const Layer &layer = doc_->layerRef(layerIndex);

    beginStrokeUndo();
    // 貼り付けで実際に書き換えるタイルの範囲だけをUndo対象にする
    const int touchTxMin = qMax(minTx, layer.originTx);
    const int touchTyMin = qMax(minTy, layer.originTy);
    const int touchTxMax = qMin(maxTxEx - 1, layer.originTx + layer.tilesX() - 1);
    const int touchTyMax = qMin(maxTyEx - 1, layer.originTy + layer.tilesY() - 1);
    expandStrokeUndoRegion(touchTxMin, touchTxMax, touchTyMin, touchTyMax);

    for (int ty = 0; ty < layer.tilesY(); ty++) {
        for (int tx = 0; tx < layer.tilesX(); tx++) {
            const int canvasTx = layer.originTx + tx;
            const int canvasTy = layer.originTy + ty;
            const int tilePxX  = canvasTx * TILE_SIZE;
            const int tilePxY  = canvasTy * TILE_SIZE;
            const int ox0 = qMax(tilePxX, pasteX), oy0 = qMax(tilePxY, pasteY);
            const int ox1 = qMin(tilePxX + TILE_SIZE, pasteX + img.width());
            const int oy1 = qMin(tilePxY + TILE_SIZE, pasteY + img.height());
            if (ox0 >= ox1 || oy0 >= oy1) continue;

            const int slice = layer.tiles[ty][tx];
            QByteArray buf = readSlicePixels(slice);
            for (int y = oy0; y < oy1; y++) {
                const uchar *srcRow = img.constScanLine(y - pasteY) + (ox0 - pasteX) * 4;
                uchar *dstRow = reinterpret_cast<uchar*>(buf.data()) + ((y - tilePxY) * TILE_SIZE + (ox0 - tilePxX)) * 4;
                // 通常のアルファ合成(Source Over、プリマル済みRGBA前提)で貼り付ける
                for (int x = 0; x < ox1 - ox0; x++) {
                    const uchar sa = srcRow[x * 4 + 3];
                    if (sa == 255) { memcpy(dstRow + x * 4, srcRow + x * 4, 4); continue; }
                    if (sa == 0) continue;
                    const int inv = 255 - sa;
                    for (int c = 0; c < 4; c++)
                        dstRow[x * 4 + c] = (uchar)qMin(255, srcRow[x * 4 + c] + (dstRow[x * 4 + c] * inv) / 255);
                }
            }
            writeSlicePixels(slice, buf);
        }
    }

    commitStrokeUndo();
    updateCompositedTex();
    emit layersChanged();
    update();
}

// ===========================================================================
// 拡大・縮小・回転 / 自由変形(アクション)
// ---------------------------------------------------------------------------
// 実体は src/actions/TransformActions.h の TransformAction/FreeTransformAction へ
// 移動した。ここに残るのはメニュー/ショートカットの入口(他アクションを畳んでから
// controller.start() するだけ)。確定/キャンセルは confirmActiveAction()/
// cancelActiveAction() に集約された(ヘッダで直接 actions_.confirmActive() 等へ転送)。
// ===========================================================================
void GLWidget::startTransformAction()
{
    cancelNonControllerActions();
    actions_.start(transformAction_);
}

void GLWidget::startFreeTransformAction()
{
    cancelNonControllerActions();
    actions_.start(freeTransformAction_);
}

// ===========================================================================
// 色相・彩度・明度(アクション)
// ---------------------------------------------------------------------------
// Transform/FreeTransformと違いキャンバス上のドラッグ操作は無く、GLWidgetの
// 子ウィジェットとして浮かせたHueSatLightPanel(実際のQSlider3本)から値を受け取る。
// パネル自体がクリックを受け取るため、実行中はGLWidgetのマウスイベントを
// (パネル外へのクリックも)すべて無視し、activeToolへの委譲も止める。
// ===========================================================================
// ===========================================================================
// 色調整/フィルター系アクションの登録と入口(フォワーダ)
// ---------------------------------------------------------------------------
// 各アクションの中身(ツール activate/confirm、パネル生成・配線、描画 uniform 等)は
// src/actions/ の CanvasAction サブクラスへ移動した。ここに残るのは:
//   - registerCanvasActions(): controller へアクションを登録(コンストラクタから)
//   - cancelNonControllerActions(): まだ GLWidget 側に残る変形/キャンバス/レイヤー編集
//     アクションを cancel する(controller 管理アクションとの相互排他のため)
//   - startXxxAction(): メニュー/ショートカットの入口。上記で他アクションを畳んでから
//     controller.start() するだけ。
// ===========================================================================
void GLWidget::registerCanvasActions()
{
    hueSatLightAction_        = actions_.add<HueSatLightAction>();
    brightnessContrastAction_ = actions_.add<BrightnessContrastAction>();
    colorBalanceAction_       = actions_.add<ColorBalanceAction>();
    toneCurveAction_          = actions_.add<ToneCurveAction>();
    gaussianBlurAction_       = actions_.add<GaussianBlurAction>();
    customShaderAction_       = actions_.add<CustomShaderAction>();
    mosaicAction_             = actions_.add<MosaicAction>();
    motionBlurAction_         = actions_.add<MotionBlurAction>();
    noiseAction_              = actions_.add<NoiseAction>();
    transformAction_          = actions_.add<TransformAction>();
    freeTransformAction_      = actions_.add<FreeTransformAction>();
    canvasSizeAction_         = actions_.add<CanvasSizeAction>();
    imageResolutionAction_    = actions_.add<ImageResolutionAction>();
    adjustmentLayerEditAction_ = actions_.add<AdjustmentLayerEditAction>();
    solidColorLayerEditAction_ = actions_.add<SolidColorLayerEditAction>();
    textBoxEditAction_         = actions_.add<TextBoxEditAction>();
    // ガウスぼかしフィルターレイヤーは無料版でも使えるため、FilterLayerEditAction
    // 自体は常に登録する(色収差はPro限定だが、editFilterLayer側が種類ごとに
    // ライセンス確認を行うので、アクションの登録可否では分岐しない)。
    filterLayerEditAction_     = actions_.add<FilterLayerEditAction>();
#ifdef TIEPOLO_PRO_BUILD
    chromaticAberrationAction_ = actions_.add<ChromaticAberrationAction>();
    lensBlurAction_            = actions_.add<LensBlurAction>();
    gradientMapAction_         = actions_.add<GradientMapAction>();
#endif
}

void GLWidget::cancelNonControllerActions()
{
    // 変形/キャンバス/レイヤー編集系を含め、全14アクションが controller 管理下に
    // 移行したため、相互排他は actions_.start()/cancelActive() だけで完結するように
    // なった。呼び出し側(startXxxAction群)を変更せずに済むよう、この関数自体は
    // 呼び出し互換のために残してあるだけの no-op。
}

void GLWidget::startHueSatLightAction()        { cancelNonControllerActions(); actions_.start(hueSatLightAction_); }
void GLWidget::startBrightnessContrastAction() { cancelNonControllerActions(); actions_.start(brightnessContrastAction_); }

// ===========================================================================
// 調整レイヤー編集(アクション)
// ---------------------------------------------------------------------------
// レイヤー自体はLayerDockの「新規調整レイヤー」で先に作成済み(色相・彩度・明度、
// 全パラメータ0)のものを使う。LayerDockでそのレイヤーのプレビューをダブルクリック
// すると開始し、パネルの値変更のたびにlayer.adjustmentを更新してその場で下の
// レイヤーへ非破壊にプレビューする。キャンセル時は編集開始時点のAdjustmentParams
// へ戻すだけで、テキストレイヤーと同様にレイヤー自体の削除は行わない
// (何度でも開き直して編集できる)。
// ===========================================================================
void GLWidget::editAdjustmentLayer(int layerIndex)
{
    if (!doc_ || layerIndex < 0 || layerIndex >= doc_->layerCount()) return;

    // トーンカーブ・グラデーションマップはPro限定(破壊的な各ツールと同じ扱い。
    // startToneCurveAction/editFilterLayer参照)。カラーバランス・色相・彩度・明度・
    // 明るさ・コントラストは無料版でも常に開ける。
    const AdjustmentKind editKind = doc_->layers[layerIndex].adjustment.kind;
    if (editKind == AdjustmentKind::ToneCurve || editKind == AdjustmentKind::GradientMap) {
        const QString featureName = (editKind == AdjustmentKind::ToneCurve)
            ? QStringLiteral("トーンカーブ") : QStringLiteral("グラデーションマップ");
#ifdef TIEPOLO_PRO_BUILD
        if (!LicenseManager::instance().isUnlocked()) {
            ProFeatureDialog::show(this, featureName);
            return;
        }
#else
        ProFeatureDialog::show(this, featureName);
        return;
#endif
    }

    adjustmentLayerEditAction_->setTarget(layerIndex);
    actions_.start(adjustmentLayerEditAction_);
}

void GLWidget::editFilterLayer(int layerIndex)
{
    if (!doc_ || layerIndex < 0 || layerIndex >= doc_->layerCount()) return;

    // Pro限定なのは種類ごとに決まる(色収差・レンズぼかし)。ガウスぼかし・移動ぼかしは
    // 無料版でも常に開ける。ここで種類を見て、Pro限定の種類のときだけライセンス確認する。
    const FilterKind editKind = doc_->layers[layerIndex].filter.kind;
    if (editKind == FilterKind::ChromaticAberration || editKind == FilterKind::LensBlur) {
        const QString featureName = (editKind == FilterKind::ChromaticAberration)
            ? QStringLiteral("色収差") : QStringLiteral("レンズぼかし");
#ifdef TIEPOLO_PRO_BUILD
        if (!LicenseManager::instance().isUnlocked()) {
            ProFeatureDialog::show(this, featureName);
            return;
        }
#else
        ProFeatureDialog::show(this, featureName);
        return;
#endif
    }

    filterLayerEditAction_->setTarget(layerIndex);
    actions_.start(filterLayerEditAction_);
}

// ===========================================================================
// 単色レイヤー編集(アクション)
// ---------------------------------------------------------------------------
// レイヤー自体はLayerDockの「新規単色レイヤー」で先に作成済みのものを使う。
// LayerDockでそのレイヤーのプレビューをダブルクリックすると開始し、パネル
// (OKLCHベースのカラーサークル、ColorCircleDockと共通のColorWheelWidgetを使う)の
// 値変更のたびにlayer.solidColorを更新してその場でプレビューする。キャンセル時は
// 編集開始時点の色へ戻すだけで、テキスト/調整レイヤーと同様にレイヤー自体の
// 削除は行わない(何度でも開き直して編集できる)。
// ===========================================================================
void GLWidget::editSolidColorLayer(int layerIndex)
{
    solidColorLayerEditAction_->setTarget(layerIndex);
    actions_.start(solidColorLayerEditAction_);
}

// ===========================================================================
// テキストレイヤー編集(アクション)
// ---------------------------------------------------------------------------
// レイヤー自体はLayerDockの「新規テキストレイヤー」で先に作成済み(空のテキスト
// レイヤー)のものを使う。1レイヤーは複数のテキストボックス(layer.textBoxes)を
// 持てる。TextToolで新規ボックスをtextBoxesへ追加/既存ボックスを選択した後、
// これを呼んで編集パネルを開く。TextLayerPanelの値変更のたびに対象ボックスを
// 更新してrasterizeTextLayer()で実タイルへ焼き込む(=キャンバス上にリアル
// タイムでプレビューされる)。キャンセル時は編集開始時点のTextParamsへ戻す。
// 確定/キャンセルいずれでも、結果として文字列が空になったボックスは
// textBoxesから削除する(空ボックスを残さない)。
// ===========================================================================
void GLWidget::startOrEditTextBox(int boxIndex)
{
    if (!doc_ || doc_->layers.isEmpty()) return;
    const int idx = doc_->activeLayerIndex();
    textBoxEditAction_->setTarget(idx, boxIndex);
    actions_.start(textBoxEditAction_);
}

// TextToolでのドラッグ操作(移動/拡縮/回転)中に呼ばれる。1文字入力ごと/1ドラッグ
// フレームごとにrasterizeTextLayer(テクスチャ再確保を伴いうる)を連打すると
// GPU側の処理が詰まってクラッシュしうるため、入力/操作が止まってから一定時間後に
// まとめてラスタライズする(デバウンス。TextBoxEditActionが編集パネル用に持つ
// 別のデバウンスタイマーとは独立)。
void GLWidget::scheduleTextRasterize(int layerIndex)
{
    textRasterizeLayerIndex_ = layerIndex;
    if (!textRasterizeTimer_) {
        textRasterizeTimer_ = new QTimer(this);
        textRasterizeTimer_->setSingleShot(true);
        connect(textRasterizeTimer_, &QTimer::timeout, this, [this]() {
            if (textRasterizeLayerIndex_ < 0) return;
            rasterizeTextLayer(textRasterizeLayerIndex_);
            update();
        });
    }
    textRasterizeTimer_->start(150);
}

// layer.textBoxesの内容をそれぞれQImageへラスタライズして1枚に合成し、レイヤーの
// 実タイルへ焼き込む。ボックスが1つも無い場合は既存タイルを透明クリアするだけ
// (縮小はしない)。
void GLWidget::rasterizeTextLayer(int layerIndex)
{
    if (!doc_ || layerIndex < 0 || layerIndex >= doc_->layerCount()) return;
    if (doc_->layerRef(layerIndex).layerType != LayerType::Text) return;

    makeCurrent();
    // grow中の再確保に影響されないようコピー
    const std::vector<TextParams> boxes = doc_->layerRef(layerIndex).textBoxes;

    // 全ボックスの回転後AABBの和集合を画像サイズとする(+余白)。
    QRect unionRect;
    for (const TextParams &box : boxes) {
        if (box.text.isEmpty()) continue;
        const float hw = box.width  * box.scale / 2.0f;
        const float hh = box.height * box.scale / 2.0f;
        const float diag = std::sqrt(hw * hw + hh * hh) + 2.0f; // 回転を考慮した安全マージン
        const QRect r(qFloor(box.cx - diag), qFloor(box.cy - diag), qCeil(diag * 2), qCeil(diag * 2));
        unionRect = unionRect.isNull() ? r : unionRect.united(r);
    }

    QImage img;
    int imgX = 0, imgY = 0, w = 0, h = 0;
    if (!unionRect.isNull()) {
        imgX = unionRect.x();
        imgY = unionRect.y();
        w = qMax(1, unionRect.width());
        h = qMax(1, unionRect.height());

        img = QImage(w, h, QImage::Format_RGBA8888_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        for (const TextParams &box : boxes) {
            if (box.text.isEmpty()) continue;
            QFont font(box.fontFamily, box.fontSize);
            font.setBold(box.bold);
            font.setItalic(box.italic);
            p.save();
            // 後で画像全体を1回だけ上下反転する(下記)ため、反転後にbox.cyへ
            // 正しく戻るよう、あらかじめ反転を見込んだY座標(imgY + h - box.cy)へ
            // 描画しておく。単純にbox.cy - imgYを使うと、そのボックスが合成画像
            // 全体の垂直中心にある場合(=ボックスが1つだけの場合)のみ正しく、
            // 複数ボックスが同じ画像に収まる場合は他ボックスの位置に応じてズレる
            // (1つのボックスを動かすと他のボックスも動いて見えるバグの原因だった)。
            p.translate(box.cx - imgX, imgY + h - box.cy);
            p.rotate(box.rotation);
            p.scale(box.scale, box.scale);
            p.setFont(font);
            p.setPen(box.color);
            p.drawText(QRectF(-box.width / 2.0, -box.height / 2.0, box.width, box.height),
                       Qt::TextWordWrap | Qt::AlignHCenter | Qt::AlignVCenter, box.text);
            p.restore();
        }
        p.end();

        // QPainterはY下向き(通常の画像座標系)で描画するが、キャンバスピクセル座標は
        // Y上向き(widgetToCanvas参照)なので、タイルへ書き込む前に上下反転させる。
        img = img.mirrored(false, true);

        const int minTx   = qFloor((double)imgX / TILE_SIZE);
        const int minTy   = qFloor((double)imgY / TILE_SIZE);
        const int maxTxEx = qCeil((double)(imgX + w) / TILE_SIZE);
        const int maxTyEx = qCeil((double)(imgY + h) / TILE_SIZE);
        growLayerBoundsToCoverCanvasTiles(layerIndex, minTx, minTy, maxTxEx, maxTyEx);
    }

    Layer &layer = doc_->layerRef(layerIndex);
    static const QByteArray zeroTile(TILE_SIZE * TILE_SIZE * 4, 0);
    for (int ty = 0; ty < layer.tilesY(); ty++) {
        for (int tx = 0; tx < layer.tilesX(); tx++) {
            const int canvasTx = layer.originTx + tx;
            const int canvasTy = layer.originTy + ty;
            const int tilePxX  = canvasTx * TILE_SIZE;
            const int tilePxY  = canvasTy * TILE_SIZE;
            const int slice    = layer.tiles[ty][tx];

            if (img.isNull()) { writeSlicePixels(slice, zeroTile); continue; }

            const int ox0 = qMax(tilePxX, imgX);
            const int oy0 = qMax(tilePxY, imgY);
            const int ox1 = qMin(tilePxX + TILE_SIZE, imgX + w);
            const int oy1 = qMin(tilePxY + TILE_SIZE, imgY + h);
            if (ox0 >= ox1 || oy0 >= oy1) { writeSlicePixels(slice, zeroTile); continue; }

            QByteArray buf = zeroTile;
            for (int y = oy0; y < oy1; y++) {
                const uchar *srcRow = img.constScanLine(y - imgY) + (ox0 - imgX) * 4;
                uchar *dstRow = reinterpret_cast<uchar*>(buf.data()) + ((y - tilePxY) * TILE_SIZE + (ox0 - tilePxX)) * 4;
                memcpy(dstRow, srcRow, (ox1 - ox0) * 4);
            }
            writeSlicePixels(slice, buf);
        }
    }

    // 1文字入力/1ドラッグフレームごとに呼ばれるため、連打するとgrowLayerBounds
    // (テクスチャ再確保を伴いうる)~書き込みが連続で走る。GPU側の処理が完了しない
    // うちに次の呼び出しでまたテクスチャを確保し直す(rebuildCanvasFromSnapshotsと
    // 同種のタイミング依存クラッシュ)のを防ぐため、ここで明示的に完了を待つ。
    glFinish();

    updateCompositedTex();
}

// ===========================================================================
// カラーバランス(アクション)
// ---------------------------------------------------------------------------
// HueSatLightアクションと全く同じ構造。GLWidgetの子ウィジェットとして浮かせた
// ColorBalancePanel(C/M/Yの3本のQSlider)から値を受け取る。
// ===========================================================================
// ===========================================================================
// 色調整/フィルター系アクションの入口(フォワーダ)続き。実装は src/actions/ 側。
// ===========================================================================
void GLWidget::startColorBalanceAction() { cancelNonControllerActions(); actions_.start(colorBalanceAction_); }
void GLWidget::startGaussianBlurAction() { cancelNonControllerActions(); actions_.start(gaussianBlurAction_); }
void GLWidget::startCustomShaderAction() { cancelNonControllerActions(); actions_.start(customShaderAction_); }
void GLWidget::startMosaicAction()       { cancelNonControllerActions(); actions_.start(mosaicAction_); }
void GLWidget::startMotionBlurAction()   { cancelNonControllerActions(); actions_.start(motionBlurAction_); }
void GLWidget::startNoiseAction()        { cancelNonControllerActions(); actions_.start(noiseAction_); }

// トーンカーブはPro限定機能(色収差と同じ案内ダイアログパターン)。ツール自体は
// 無料版にも含まれる(CMakeLists上はPro専用に分離していない)ため、ここで
// ライセンス確認だけを行う。
void GLWidget::startToneCurveAction()
{
#ifdef TIEPOLO_PRO_BUILD
    if (!LicenseManager::instance().isUnlocked()) {
        ProFeatureDialog::show(this, QStringLiteral("トーンカーブ"));
        return;
    }
    cancelNonControllerActions();
    actions_.start(toneCurveAction_);
#else
    ProFeatureDialog::show(this, QStringLiteral("トーンカーブ"));
#endif
}

void GLWidget::startChromaticAberrationAction()
{
#ifdef TIEPOLO_PRO_BUILD
    // Pro版でもライセンス未認証なら実処理せず案内ダイアログを出す。
    if (!LicenseManager::instance().isUnlocked()) {
        ProFeatureDialog::show(this, QStringLiteral("色収差"));
        return;
    }
    cancelNonControllerActions();
    actions_.start(chromaticAberrationAction_);
#else
    // 無料版ビルドでは色収差アクション自体を持たない。案内ダイアログのみ。
    ProFeatureDialog::show(this, QStringLiteral("色収差"));
#endif
}

void GLWidget::startLensBlurAction()
{
#ifdef TIEPOLO_PRO_BUILD
    // Pro版でもライセンス未認証なら実処理せず案内ダイアログを出す(色収差と同じパターン)。
    if (!LicenseManager::instance().isUnlocked()) {
        ProFeatureDialog::show(this, QStringLiteral("レンズぼかし"));
        return;
    }
    cancelNonControllerActions();
    actions_.start(lensBlurAction_);
#else
    // 無料版ビルドではレンズぼかしアクション自体を持たない。案内ダイアログのみ。
    ProFeatureDialog::show(this, QStringLiteral("レンズぼかし"));
#endif
}

void GLWidget::startGradientMapAction()
{
#ifdef TIEPOLO_PRO_BUILD
    // Pro版でもライセンス未認証なら実処理せず案内ダイアログを出す(色収差と同じパターン)。
    if (!LicenseManager::instance().isUnlocked()) {
        ProFeatureDialog::show(this, QStringLiteral("グラデーションマップ"));
        return;
    }
    cancelNonControllerActions();
    actions_.start(gradientMapAction_);
#else
    // 無料版ビルドではグラデーションマップアクション自体を持たない。案内ダイアログのみ。
    ProFeatureDialog::show(this, QStringLiteral("グラデーションマップ"));
#endif
}

// ===========================================================================
// キャンバスサイズ変更 / 画像解像度変更(アクション)
// ---------------------------------------------------------------------------
// 実体は src/actions/CanvasSizeActions.h の CanvasSizeAction/ImageResolutionAction
// へ移動した。ここに残るのはメニュー/ショートカットの入口だけ。
// ===========================================================================
void GLWidget::startCanvasSizeAction()
{
    cancelNonControllerActions();
    actions_.start(canvasSizeAction_);
}

void GLWidget::startImageResolutionAction()
{
    cancelNonControllerActions();
    actions_.start(imageResolutionAction_);
}

// ===========================================================================
// 初期化
// ===========================================================================
void GLWidget::initializeGL() {
    // 計測用。QOpenGLWidgetは親が変わるとコンテキストごと作り直されるため、
    // タブを別ペインへ移す(=分割)たびにここが再実行されうる。その場合は
    // シェーダーを全部コンパイルし直すので、何回呼ばれて各段が何msかかっているかを
    // 見られるようにしておく(TIEPOLO_WINLOG=1 のときだけ)。
    static int initCount = 0;
    ++initCount;
    QElapsedTimer initTimer;
    initTimer.start();

    initializeOpenGLFunctions();
    glFunctionsReady_ = true;

    // シェーダープログラムはプロセス全体で1組だけ作り、全タブ(GLWidget)で共有する。
    // 実体と事前コンパイルは ShaderCache が持つ(理由と計測値はそちらのコメント参照)。
    // ここで引くときには、たいてい起動直後の背景コンパイルで出来上がっている。
    computeDrawProgram         = ShaderCache::compute(":/shaders/paint/stroke.comp");
    computeBakeProgram         = ShaderCache::compute(":/shaders/paint/bake.comp");
    computeBrushStateProgram   = ShaderCache::compute(":/shaders/paint/brushState.comp");
    computeMaskClearProgram    = ShaderCache::compute(":/shaders/paint/maskclear.comp");
    computeLayerClearProgram   = ShaderCache::compute(":/shaders/paint/layerclear.comp");
    computeCompositeProgram    = ShaderCache::compute(":/shaders/render/composite.comp");
    computeWallProgram         = ShaderCache::compute(":/shaders/fill/wall.comp");
    computeJfaInitOuterProgram = ShaderCache::compute(":/shaders/fill/jfaInitOuter.comp");
    computeJfaInitInnerProgram = ShaderCache::compute(":/shaders/fill/jfaInitInner.comp");
    computeJfaProgram          = ShaderCache::compute(":/shaders/fill/jfa.comp");
    computeJfaFinalizeProgram  = ShaderCache::compute(":/shaders/fill/jfaFinalize.comp");
    computeBlurProgram         = ShaderCache::compute(":/shaders/paint/blur.comp");
    computeGaussianBlurFilterProgram = ShaderCache::compute(":/shaders/paint/gaussianBlurFilter.comp");
    computeMosaicReduceProgram = ShaderCache::compute(":/shaders/paint/mosaicReduce.comp");
    computeMosaicFilterProgram = ShaderCache::compute(":/shaders/paint/mosaicFilter.comp");
    computeMotionBlurFilterProgram = ShaderCache::compute(":/shaders/paint/motionBlurFilter.comp");
    computeNoiseFilterProgram      = ShaderCache::compute(":/shaders/paint/noiseFilter.comp");
    computeGaussianBlurLayerProgram = ShaderCache::compute(":/shaders/render/gaussianBlurLayer.comp"); // 無料版でも使える
    computeMotionBlurLayerProgram   = ShaderCache::compute(":/shaders/render/motionBlurLayer.comp");   // 無料版でも使える
    computeMosaicReduceLayerProgram = ShaderCache::compute(":/shaders/render/mosaicReduceLayer.comp"); // 無料版でも使える
    computeMosaicLayerProgram       = ShaderCache::compute(":/shaders/render/mosaicLayer.comp");       // 無料版でも使える
    computeNoiseLayerProgram        = ShaderCache::compute(":/shaders/render/noiseLayer.comp");        // 無料版でも使える
#ifdef TIEPOLO_PRO_BUILD
    computeChromaticAberrationFilterProgram = ShaderCache::compute(":/shaders/paint/chromaticAberrationFilter.comp");
    computeChromaticAberrationLayerProgram  = ShaderCache::compute(":/shaders/render/chromaticAberrationLayer.comp");
    computeLensBlurFilterProgram            = ShaderCache::compute(":/shaders/paint/lensBlurFilter.comp");
    computeLensBlurLayerProgram             = ShaderCache::compute(":/shaders/render/lensBlurLayer.comp");
    computeGradientMapProgram               = ShaderCache::compute(":/shaders/paint/gradientMap.comp");
#endif
    computeWarpProgram         = ShaderCache::compute(":/shaders/paint/warp.comp");
    computeTransformProgram    = ShaderCache::compute(":/shaders/paint/transform.comp");
    computeFreeTransformProgram = ShaderCache::compute(":/shaders/paint/freeTransform.comp");
    computeHueSatLightProgram   = ShaderCache::compute(":/shaders/paint/hueSatLight.comp");
    computeBrightnessContrastProgram = ShaderCache::compute(":/shaders/paint/brightnessContrast.comp");
    computeColorBalanceProgram       = ShaderCache::compute(":/shaders/paint/colorBalance.comp");
    computeToneCurveProgram          = ShaderCache::compute(":/shaders/paint/toneCurve.comp");
    computeBelowCompositeProgram     = ShaderCache::compute(":/shaders/render/belowComposite.comp");
    renderProgram                    = ShaderCache::render();

    // ストローク色バッファ用のテクスチャユニットが取れるか。render.fragは
    // 0〜7を個別のテクスチャ、8〜15をレイヤーバンクで使い切っているので16番が要る。
    // GL4.3の下限がちょうど16なので、ここで実際の上限を確かめておく
    // (足りない環境ではスタンプごとの色の経路自体を使わない)。
    {
        GLint maxTexUnits = 0;
        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxTexUnits);
        strokeColorSupported_ = (maxTexUnits > kStrokeColorTexUnit);
        WINLOG(QStringLiteral("GL_MAX_TEXTURE_IMAGE_UNITS=%1 → ストローク色バッファ %2")
                   .arg(maxTexUnits).arg(strokeColorSupported_ ? "利用可" : "利用不可"));
    }

    const qint64 shaderMs = initTimer.elapsed();
    initTextures();
    const qint64 texMs = initTimer.elapsed() - shaderMs;
    glGenVertexArrays(1, &dummyVAO);
    // ペン先画像(キャンバスサイズに依存しないのでinitTextures()とは別に一度だけ確保する)。
    // 保存済み設定にパスが残っていればそれを、読み込みに失敗したら既定の丸ブラシを使う。
    if (!setPenTipImage(toolCfg_->pen().tipImagePath()))
        setPenTipImage(":/textures/penTip/circle.png");

    // 紙質テクスチャも同様。既定は「なし」(空パス)なので、通常はここで何もしない。
    if (!setPaperTexture(toolCfg_->pen().paperTexPath()))
        setPaperTexture(QString());

    // トーンカーブLUT(256x1, R8)。キャンバスサイズに依存しないので一度だけ確保し、
    // 恒等カーブ(出力=入力)で初期化しておく。以後はToneCurveTool::uploadLutが
    // glTexSubImage2Dで中身だけ差し替える。GL_LINEARで補間することで、256段階の
    // 粗さを感じさせずになめらかに色を変換できる。
    {
        glGenTextures(1, &toneCurveLUTTex);
        glBindTexture(GL_TEXTURE_2D, toneCurveLUTTex);
        quint8 identity[256];
        for (int i = 0; i < 256; i++) identity[i] = (quint8)i;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 1, 0, GL_RED, GL_UNSIGNED_BYTE, identity);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // グラデーションマップ(Pro限定)用のLUT。上のトーンカーブLUTと同じ扱いだが、
    // 輝度1つから色(RGB)を引くのでRGBA8。初期値は黒→白の素直なグラデーション。
    // 無料版ビルドではこのLUTを書き換えるツール自体が存在しないが、render.fragは
    // 常にユニット0へこれを束縛するので、テクスチャ自体は両版で確保しておく。
    {
        glGenTextures(1, &gradientMapLUTTex);
        glBindTexture(GL_TEXTURE_2D, gradientMapLUTTex);
        quint8 blackToWhite[256 * 4];
        for (int i = 0; i < 256; i++) {
            blackToWhite[i * 4 + 0] = (quint8)i;
            blackToWhite[i * 4 + 1] = (quint8)i;
            blackToWhite[i * 4 + 2] = (quint8)i;
            blackToWhite[i * 4 + 3] = 255;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blackToWhite);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // ペンストロークのバッチスタンプ用SSBO(stroke.comp参照)。中身は使用のたびに
    // glBufferSubData/glBufferDataで書き換えるので、ここでは器だけ確保しておく。
    glGenBuffers(1, &strokeStampSSBO_);
    // 筆に乗っている絵の具(vec4 1個)。中身はストローク開始時にCPU側が初期化する。
    glGenBuffers(1, &brushPaintSSBO_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, brushPaintSSBO_);
    {
        const float init[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(init), init, GL_DYNAMIC_DRAW);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // シェーダー/テクスチャが揃ったので toolCtx_ を実体で埋め直す
    // (コンストラクタ時点ではまだ全部nullptr/0だったため)
    setupToolContext();

    // FillTool を初期化してテクスチャ/シェーダーを注入
    fillTool_.initialize(context());
    fillTool_.setTextures({layerTexBanks[0], maskTex, wallTex, outerJfaTex, innerJfaTex, sdfTex});
    fillTool_.setPrograms({
        computeWallProgram,
        computeJfaInitOuterProgram,
        computeJfaInitInnerProgram,
        computeJfaProgram,
        computeJfaFinalizeProgram,
        computeBakeProgram,
        computeMaskClearProgram
    });

    blurTool_.initialize(context());
    blurTool_.setProgram(computeBlurProgram);

    warpTool_.initialize(context());
    warpTool_.setProgram(computeWarpProgram);

    selectTool_.initialize(context());

    // 色調整/フィルター/変形/キャンバスサイズ系ツールの initialize は
    // 各 CanvasAction (src/actions/) へ移動。
    actions_.initializeAll(context());

    // ここまででシェーダーのコンパイル/リンクとtoolCtx_の初期化が完了する。
    // initializeGL()完了前(シェーダーコンパイル中など)にマウス操作が届いても、
    // 未初期化のtoolCtx_/GL資源に触れてしまわないよう、マウスイベント側で
    // glReady_をガードに使う。
    glReady_ = true;
    // 開いた直後の1本目のストロークでも事前合成キャッシュが出来ているように、
    // ここでも先読み作成を予約しておく(文書の初期化中は doc_->onChanged が
    // 抑止されているため、ここを入れないと1本目だけ書き始めが重くなる)。
    scheduleCompositeCachePrewarm();

    WINLOG(QStringLiteral("PERF GLWidget::initializeGL #%1 total=%2ms (shaders=%3ms textures=%4ms rest=%5ms)")
               .arg(initCount).arg(initTimer.elapsed())
               .arg(shaderMs).arg(texMs).arg(initTimer.elapsed() - shaderMs - texMs));

    updateCursor();
    emit initialized();
}

// ===========================================================================
// テクスチャ初期化
// ===========================================================================
GLuint GLWidget::makeTexture2D(GLenum internalFormat, int w, int h, GLenum filter)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    return tex;
}

void GLWidget::syncBanksToToolContext()
{
    for (int i = 0; i < MAX_TILE_BANKS; i++) toolCtx_.layerTexBanks[i] = layerTexBanks[i];
    toolCtx_.slicesPerBank = slicesPerBank_;
}

void GLWidget::reserveTileSlices(int count)
{
    if (count <= 0) return;
    makeCurrent();
    sliceAllocator_.reserve(count);
    syncBanksToToolContext(); // reserve()内でonTextureRecreatedも呼ばれるが念のため
}

void GLWidget::bindLayerBanksForSampling(QOpenGLShaderProgram *prog)
{
    GLint units[MAX_TILE_BANKS];
    for (int i = 0; i < MAX_TILE_BANKS; i++) {
        glActiveTexture(GL_TEXTURE0 + LAYER_BANK_TEXUNIT_BASE + i);
        // 未生成バンクのスロットにもbank0を割り当てておく(サンプラー配列の全要素が
        // 有効なテクスチャを指している必要があるため。実際にサンプルされるのは
        // si < 生成済み容量 の範囲だけなので中身は問われない)。
        glBindTexture(GL_TEXTURE_2D_ARRAY, layerTexBanks[i] ? layerTexBanks[i] : layerTexBanks[0]);
        units[i] = LAYER_BANK_TEXUNIT_BASE + i;
    }
    // sampler2DArray配列は要素ごとに名前を組み立てず、配列名でまとめて設定する
    // (要素名を毎回QByteArrayで作ると環境によってはQt内部でアサートに当たるため)。
    prog->setUniformValueArray("layerTexBanks", units, MAX_TILE_BANKS);
    prog->setUniformValue("uSlicesPerBank", slicesPerBank_);
    glActiveTexture(GL_TEXTURE0);
}

// [base, base+count) の連続グローバルスライス範囲を uClearColor でGPUクリアする。
// 1回のディスパッチ(layerclear.comp)は image2DArray 1本(=1バンク)に閉じている
// 必要があるため、バンク境界でセグメントに分割し、各バンクを image unit 1 にバインド
// して「そのバンク内ローカルなuBaseSlice」で個別にディスパッチする。
void GLWidget::clearSliceRange(int base, int count, float r, float g, float b, float a)
{
    if (count <= 0 || slicesPerBank_ <= 0) return;
    int s = base, remaining = count;
    computeLayerClearProgram->bind();
    computeLayerClearProgram->setUniformValue("uClearColor", r, g, b, a);
    while (remaining > 0) {
        const int bank  = s / slicesPerBank_;
        const int local = s % slicesPerBank_;
        const int seg   = qMin(remaining, slicesPerBank_ - local);
        glBindImageTexture(1, layerTexBanks[bank], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);
        computeLayerClearProgram->setUniformValue("uBaseSlice", local);
        glDispatchCompute((TILE_SIZE + 15) / 16, (TILE_SIZE + 15) / 16, seg);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        s += seg;
        remaining -= seg;
    }
    computeLayerClearProgram->release();
}

void GLWidget::initTextures(bool createDefaultLayers) {
    // layerTexArrayに確保できるスライス数の上限を、固定の小さい定数ではなく
    // このGPU/ドライバが実際に許容する最大値(GL_MAX_ARRAY_TEXTURE_LAYERS)から
    // 決める。これによりPhotoshop同様、キャンバスサイズ・レイヤー数(=タイル数)を
    // 「VRAM/ドライバの限界まで」増やせるようになる(伸長ロジック自体は
    // LayerSliceAllocator::grow()が2倍ずつ確保し直す形で既に実装済み)。
    // MAX_SLICESはOpenGL仕様上の最低保証値なので、問い合わせが異常な値を返した
    // 場合のフォールバック下限として使う。
    // 1バンクあたりのスライス数を、固定の小さい定数ではなくGPU/ドライバが実際に
    // 許容する最大値(GL_MAX_ARRAY_TEXTURE_LAYERS)にする。さらにそのバンクを
    // 最大 MAX_TILE_BANKS 本まで並べるので、タイル数の上限は実質「VRAM/バンク数」まで
    // 引き上がる(詳細は CanvasDocument.h / LayerSliceAllocator.h)。MAX_SLICESは
    // OpenGL仕様上の最低保証値で、問い合わせが異常値を返した場合の下限フォールバック。
    GLint maxArrayLayers = 0;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxArrayLayers);
    slicesPerBank_ = qMax((int)MAX_SLICES, (int)maxArrayLayers);

    // スライス確保ロジックのセットアップ(バンクテクスチャ群 layerTexBanks[] は
    // GLWidgetが所有し続け、allocatorが生成・伸長・差し替えを行う)。
    sliceAllocator_.setup(this, layerTexBanks, TILE_SIZE, slicesPerBank_);
    sliceAllocator_.onTextureRecreated = [this] {
        syncBanksToToolContext();
        fillTool_.setTextures({layerTexBanks[0], maskTex, wallTex,
                                 outerJfaTex, innerJfaTex, sdfTex});
    };

    // maskTex (R8)
    // フィルタはlayerTexArray(GL_LINEAR)に合わせる。ここをGL_NEARESTのままにすると、
    // ストローク中のプレビュー(render.fragがmaskTexをtexture()でサンプルする経路)だけ
    // ズーム時にドット単位でカクカクした境界になり、ベイク後(同じアルファ値が
    // layerTexArrayへ焼き込まれ、GL_LINEARで滑らかに補間される)と見た目が変わってしまう。
    // ストローク色バッファはキャンバスと寿命を共にするが、使う設定になるまで確保しない
    // (ensureStrokeColorTex参照)。ここではハンドルを空にしておくだけ。
    strokeColorTex = 0;

    glGenTextures(1, &maskTex);
    glBindTexture(GL_TEXTURE_2D, maskTex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, canvasW, canvasH);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    QVector<uint8_t> emptyMask(canvasW * canvasH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvasW, canvasH,
                    GL_RED, GL_UNSIGNED_BYTE, emptyMask.constData());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindImageTexture(0, maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);

    computeMaskClearProgram->bind();
    glBindImageTexture(0, maskTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    // uDispatchOriginはプログラムオブジェクトに紐づくuniform状態であり、
    // AirbrushTool(スタンプごとに部分範囲だけ処理するため非ゼロ値を設定する)が
    // 最後にこのプログラムを使った際の値がそのまま残っていることがある。
    // ここはキャンバス全域を対象にした呼び出しなので、明示的に(0,0)へ戻す。
    {
        GLint loc = glGetUniformLocation(computeMaskClearProgram->programId(), "uDispatchOrigin");
        if (loc >= 0) glUniform2i(loc, 0, 0);
    }
    glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    computeMaskClearProgram->release();

    // タイルグリッドを doc に登録
    doc_->initTileGrid(canvasW, canvasH);
 
    // layerTexArray（mipなし）
    sliceAllocator_.createInitial(16);

    int mipLevels = 1 + (int)std::floor(std::log2(std::max(canvasW, canvasH)));
    glGenTextures(1, &compositedTex);
    glBindTexture(GL_TEXTURE_2D, compositedTex);
    glTexStorage2D(GL_TEXTURE_2D, mipLevels, GL_RGBA8, canvasW, canvasH);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // キャッシュ用
    int tileCount = doc_->tilesX() * doc_->tilesY();

    // ナビゲーター用作業テクスチャ(以前は updateCompositedTex() 内で遅延生成していたが、
    // CanvasCompositor はテクスチャを所有しないため、ここで確保しておく)
    glGenTextures(1, &compositedTileArr);
    glBindTexture(GL_TEXTURE_2D_ARRAY, compositedTileArr);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, TILE_SIZE, TILE_SIZE, tileCount);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // 塗りつぶし用
    wallTex     = makeTexture2D(GL_R8,    canvasW, canvasH);
    outerJfaTex = makeTexture2D(GL_RG16F, canvasW, canvasH);
    innerJfaTex = makeTexture2D(GL_RG16F, canvasW, canvasH);
    sdfTex      = makeTexture2D(GL_R32F,  canvasW, canvasH);

    fullLayerTex = makeTexture2D(GL_RGBA8, canvasW, canvasH);

    // 「アクティブレイヤーより下」の合成結果キャッシュ(item4)。キャンバスサイズに
    // 連動するので他の canvasW x canvasH テクスチャと同じくここで確保する
    // (中身は使われる直前に updateBelowCompositeCache() が必ず書くので、
    // 初期値は未定でよい)。
    // フィルターは GL_LINEAR にしておく必要がある。これらは render.frag が
    // 画面表示のために直接サンプリングするテクスチャで、キャッシュを使わない経路が
    // 読むレイヤーのタイル配列も GL_LINEAR(LayerSliceAllocator参照)だからである。
    // 既定の GL_NEAREST のままだと、キャッシュが有効になった瞬間(=ストローク開始)に
    // 表示だけが最近傍補間に切り替わり、キャンバス全体のアンチエイリアスが
    // 失われたように見える(レイヤー操作やUndoでキャッシュが破棄されると戻る)。
    belowCompositeTex         = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
    belowCompositeClipBaseTex = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
    aboveCompositeTex         = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
    aboveCompositeClipScratch_ = makeTexture2D(GL_RGBA8, canvasW, canvasH);
    belowCompositeCacheValid_ = false;
    aboveCompositeCacheValid_ = false;

    // 選択範囲マスク(R8)。未選択状態=全域255(どこでも塗れる)で初期化する。
    selectionMaskTex = makeTexture2D(GL_R8, canvasW, canvasH);
    {
        QVector<uint8_t> fullSel(canvasW * canvasH, 255);
        glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvasW, canvasH,
                        GL_RED, GL_UNSIGNED_BYTE, fullSel.constData());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    hasSelection_ = false;
    invalidateSelectionOutlineCache(); // selectionMaskTex自体を作り直したので古いキャッシュは無効

    // 変形ツール確定時の作業用スナップショット(初期値は使われないので未初期化のままでよい)
    transformSrcTex        = makeTexture2D(GL_RGBA8, canvasW, canvasH);
    transformSrcSelMaskTex = makeTexture2D(GL_R8,    canvasW, canvasH);
    transformScratchW_ = canvasW;
    transformScratchH_ = canvasH;

    // SSBO 初期化 (すべてMAX_LAYERS個ぶん、フラットなレイヤーindexでそのまま引ける)
    glGenBuffers(1, &ssboLayerOpacity);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerOpacity);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerVisible);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerVisible);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerBaseSlice);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerBaseSlice);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    // レイヤーマスクのベーススライス(-1なら無し)。binding=1は他のSSBOと違いテクスチャ/
    // 画像の名前空間とも重ならない空きスロット(CanvasCompositor.cppのコメント参照)。
    glGenBuffers(1, &ssboLayerMaskBaseSlice);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerMaskBaseSlice);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerBlendMode);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerBlendMode);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerClipping);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerClipping);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerOriginTx);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerOriginTx);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerOriginTy);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerOriginTy);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerTilesX);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerTilesX);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerTilesY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerTilesY);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerIsSolidColor);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerIsSolidColor);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerSolidColor);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerSolidColor);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerAdjKind);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerAdjKind);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    // vec3配列はstd430で要素ストライドが16バイト(vec4扱い)になるため、
    // レイヤーあたりfloat4分(x,y,z,pad)を確保する。
    glGenBuffers(1, &ssboLayerAdjParams);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerAdjParams);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    // フォルダー単位のマスクカスケード用(ivec4、レイヤーあたり最大4階層ぶんの
    // 祖先フォルダーのマスクベーススライス)。
    glGenBuffers(1, &ssboLayerAncestorMaskSlices);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerAncestorMaskSlices);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * 4 * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // レイヤー作成(この後 CacheAndNotify通知が飛ぶ)より前に toolCtx_ を実体で埋める。
    // ここまでに作ったテクスチャ/SSBO/シェーダーが出揃っているため。
    setupToolContext();

    // addLayer()自体がdoc_->onChanged経由でupdate()を呼び得るため、
    // レイヤーがまだ0件のこの時点で誤発火しないよう、
    // 初期構築が終わるまでは先にガードを立てておく
    if (createDefaultLayers) {
        m_initializing = true;

        // 最初のレイヤーを2枚作成 (下: 常に白い単色レイヤー、上: 透明な作業レイヤー)
        addSolidColorLayer("用紙");
        addLayer("レイヤー1");
        doc_->setActiveLayer(1);

        m_initializing = false;
    }
    // createDefaultLayers=false の場合はレイヤーを1枚も作らずに返す
    // (キャンバスサイズ変更アクションが、既存レイヤーの中身を復元するため)

    update();
}

// ナビゲーター用: 全レイヤー合成して compositedTex を更新
void GLWidget::updateCompositedTex() {
    makeCurrent();
    compositor_.updateCompositedTex(toolCtx_);
}

// ===========================================================================
// 変形確定用の作業テクスチャを、指定サイズに合わせて作り直す
// (レイヤーの矩形がgrowLayerBoundsで変わるため、キャンバスサイズ固定では足りない)
// ===========================================================================
void GLWidget::ensureTransformScratchSize(int w, int h)
{
    if (w == transformScratchW_ && h == transformScratchH_ && fullLayerTex != 0 && transformSrcTex != 0)
        return;

    if (fullLayerTex)    { glDeleteTextures(1, &fullLayerTex);    fullLayerTex    = 0; }
    if (transformSrcTex) { glDeleteTextures(1, &transformSrcTex); transformSrcTex = 0; }

    fullLayerTex    = makeTexture2D(GL_RGBA8, w, h);
    transformSrcTex = makeTexture2D(GL_RGBA8, w, h);
    transformScratchW_ = w;
    transformScratchH_ = h;

    toolCtx_.fullLayerTex   = fullLayerTex;
    toolCtx_.transformSrcTex = transformSrcTex;
}

// ===========================================================================
// 多段パスのフィルター用の中間バッファを確保する(GLWidget.hのコメント参照)
// ===========================================================================
GLuint GLWidget::ensureFilterScratch(int w, int h)
{
    if (w == filterScratchW_ && h == filterScratchH_ && filterScratchTex_ != 0)
        return filterScratchTex_;

    if (filterScratchTex_) { glDeleteTextures(1, &filterScratchTex_); filterScratchTex_ = 0; }

    filterScratchTex_ = makeTexture2D(GL_RGBA8, w, h);
    filterScratchW_ = w;
    filterScratchH_ = h;
    return filterScratchTex_;
}

// ===========================================================================
// レイヤーの矩形を、キャンバスタイル座標系で指定範囲を覆うように拡張する
// (CanvasDocument::growLayerBoundsへ委譲し、GPU側のタイルコピー/クリアを担当する)
// ===========================================================================
bool GLWidget::growLayerBoundsToCoverCanvasTiles(int layerIndex, int minTx, int minTy, int maxTxEx, int maxTyEx)
{
    if (!doc_) return false;

    auto copyFn = [this](int srcSlice, int dstSlice) {
        writeSlicePixels(dstSlice, readSlicePixels(srcSlice));
    };
    auto clearFn = [this](int sliceIndex) {
        static const QByteArray zero(TILE_SIZE * TILE_SIZE * 4, 0);
        writeSlicePixels(sliceIndex, zero);
    };

    bool grew = doc_->growLayerBounds(layerIndex, minTx, minTy, maxTxEx, maxTyEx, copyFn, clearFn);
    if (grew)
        doc_->invalidateLayerTileUndoHistory(layerIndex);
    return grew;
}

// ===========================================================================
// リサイズ / 描画
// ===========================================================================
void GLWidget::resizeGL(int w, int h) {
    // モニター間の移動や表示スケール変更でDPRが変わるので、ここで追従させる
    // (ツール側はこの値でマウス座標をビュー空間へ換算する)。
    toolCtx_.viewDpr = viewDpr();
    // 注意: QOpenGLWidgetのresizeGL()に渡ってくるw/hは論理ピクセル(実測で
    // width()/height()と一致)。ここでのglViewport()はQtがpaintGL()の直前に
    // デバイスピクセルで設定し直すため実質上書きされる(実測: FBO=1128x998に対し
    // paintGL時点のviewport=1127x997)。値を信用しないこと。
    glViewport(0, 0, w, h);
    fitCanvasToView();
    actions_.positionActivePanel(); // 変形/色調整/フィルター/レイヤー編集系(controller管理アクション)
}

void GLWidget::fitCanvasToView()
{
    // ビューはデバイスピクセルで動く(GLWidget.hのviewDpr()のコメント参照)
    const float w = viewWidth();
    const float h = viewHeight();
    if (w <= 0.0f || h <= 0.0f) return;

    float scaleX = w / canvasW;
    float scaleY = h / canvasH;
    float s = qMin(scaleX, scaleY) * 0.9f;

    view_.setScale(s);
    // キャンバス中心をウィジェット中心に合わせる
    view_.setOffset(QVector2D(
        w / 2.0f - (canvasW / 2.0f) * s,
        h / 2.0f - (canvasH / 2.0f) * s
    ));
    view_.setRotation(0.0f);

    updateCursor(); // 表示倍率が変わるとペン円カーソルの画面上サイズも変わる
    update();
    emit viewChanged();
}

void GLWidget::setViewScaleCentered(float scale)
{
    scale = qMax(0.02f, scale);
    const QVector2D center(viewWidth() / 2.0f, viewHeight() / 2.0f);
    const float factor = scale / view_.scale();
    view_.zoomAround(center, factor);
    updateCursor();
    update();
    emit viewChanged();
}

void GLWidget::setFlippedX(bool flip)
{
    if (view_.flipX() == flip) return;

    // ViewTransform::matrix()は「回転→スケール(反転)→平行移動(offset)」の順で
    // 合成されており(T*S*R)、offsetは最後に screen 空間へそのまま足される。
    // そのため反転(xスケールの符号反転)だけをそのまま行うと、キャンバス原点(0,0)が
    // 画面上のどこにあるか(=offset.x)を軸にミラーされてしまい、パンしていると
    // 変な位置を軸に反転して見える。
    // screen = offset + S(±scale,scale)*R(rot)*canvasPos なので、反転前後で
    // 「現在画面中央に映っている点」が画面中央に留まるようにするには、
    // offset.x を ウィジェット幅基準で鏡映(offset.x -> width - offset.x)させれば
    // よい(この補正はoffset.yやcanvasPos自体に依存しない、常に成り立つ関係式)。
    QVector2D off = view_.offset();
    off.setX(viewWidth() - off.x()); // offsetはビュー空間(デバイスpx)
    view_.setOffset(off);

    view_.setFlipX(flip);
    update();
    emit viewChanged();
}

QPolygonF GLWidget::visibleCanvasRectPolygon() const
{
    QPolygonF poly;
    // ビュー空間(デバイスpx)の四隅
    const QPointF corners[4] = {
        QPointF(0, 0), QPointF((qreal)viewWidth(), 0),
        QPointF((qreal)viewWidth(), (qreal)viewHeight()), QPointF(0, (qreal)viewHeight())
    };
    for (const QPointF &c : corners) {
        QVector2D canvasPos = view_.widgetToCanvas(c, qRound(viewHeight())); // キャンバスピクセル座標(Y上向き)
        poly << QPointF(canvasPos.x(), canvasH - canvasPos.y());  // QImageと同じY下向きへ変換
    }
    return poly;
}

void GLWidget::panByCanvasDelta(const QVector2D &canvasDeltaYDown)
{
    // visibleCanvasRectPolygon()と同じ規則(Y下向き⇔ViewTransform内部のY上向き)で
    // 符号を揃えてから、平行移動を除いた線形部分(回転・拡縮・左右反転)だけを
    // screen空間(offset_と同じ座標系)へ写像し、offsetに加算する。
    const QVector2D canvasDeltaYUp(canvasDeltaYDown.x(), -canvasDeltaYDown.y());
    const QVector3D screenDelta = view_.matrix().mapVector(QVector3D(canvasDeltaYUp, 0.0f));
    view_.setOffset(view_.offset() + QVector2D(screenDelta.x(), screenDelta.y()));
    updateCursor();
    update();
    emit viewChanged();
}

void GLWidget::noteStrokeDirtyRegion(float minX, float minY, float maxX, float maxY)
{
    if (!strokeDirtyValid_) {
        strokeDirtyMinX_ = minX; strokeDirtyMinY_ = minY;
        strokeDirtyMaxX_ = maxX; strokeDirtyMaxY_ = maxY;
        strokeDirtyValid_ = true;
    } else {
        strokeDirtyMinX_ = qMin(strokeDirtyMinX_, minX);
        strokeDirtyMinY_ = qMin(strokeDirtyMinY_, minY);
        strokeDirtyMaxX_ = qMax(strokeDirtyMaxX_, maxX);
        strokeDirtyMaxY_ = qMax(strokeDirtyMaxY_, maxY);
    }
}

void GLWidget::paintGL() {

    paintGlCalls_++;
    // ドラッグ中に、こちらが呼んだ同期repaint()以外の経路(どこかのupdate())で
    // 描かれたフレーム。同じ絵をもう1枚描くだけの無駄で、1枚につきウィンドウ全体の
    // 再合成とpresentが増える。ここが並ぶようなら、その update() を止められないかを
    // 疑うこと(inputFlushTimer_ / requestRepaint の各コメント参照)。
    if (!inSyncRepaint_ && WinLog::enabled() && viewDiagCount_ < 40) {
        viewDiagCount_++;
        WINLOG(QStringLiteral("PERF view: 余分なpaintGL(update由来)"));
    }

    // 計測用(初回のみ): 実際に描き込んでいるフレームバッファの画素数を問い合わせる。
    // ここが「ウィジェットの論理サイズ」なのか「デバイスピクセル(論理×DPR)」なのかで、
    // キャンバスが実画素どおりに出ているかが決まる。
    if (WinLog::enabled()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            GLint vp[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_VIEWPORT, vp);
            GLint objName = 0, objType = 0, fbW = 0, fbH = 0;
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                   GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &objType);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                   GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &objName);
            if (objType == GL_TEXTURE && objName) {
                GLint prev = 0;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
                glBindTexture(GL_TEXTURE_2D, (GLuint)objName);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &fbW);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &fbH);
                glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
            }
            WINLOG(QStringLiteral("PERF paintGL: FBO=%1x%2 viewport=%3x%4 widget(logical)=%5x%6 dpr=%7")
                       .arg(fbW).arg(fbH).arg(vp[2]).arg(vp[3])
                       .arg(width()).arg(height()).arg(devicePixelRatioF()));
            // presentが1回30ms近くかかっている件の切り分け用。ここが1なら、
            // main.cppでswapInterval(0)にしたつもりが効いていない(=垂直帰線待ち)。
            const QWindow *topWin = window() ? window()->windowHandle() : nullptr;
            WINLOG(QStringLiteral("PERF swapInterval: default=%1 widgetCtx=%2 topWindow=%3")
                       .arg(QSurfaceFormat::defaultFormat().swapInterval())
                       .arg(context() ? context()->format().swapInterval() : -1)
                       .arg(topWin ? topWin->format().swapInterval() : -1));
        }
    }

    // paintGL本体の所要時間。ストローク中の同期描画間隔の適応に使う
    // (GLWidget.hのlastPaintGlCostNs_のコメント参照。repaint()全体の時間ではなく
    //  ここを測るのが要点)。
    QElapsedTimer paintClock;
    paintClock.start();
    const bool logPaint = WinLog::enabled() && strokeFrameLogCount_ < 12;

    // 【重要】Qtがこのフレーム用に束縛したFBOを控えておく。
    //
    // この関数はこの後 flushPendingInput() と prepareCompositeBase() を呼ぶが、
    // その先(StrokeUndoRecorder::expandRegion のタイル読み戻し、AirbrushToolの
    // スタンプ焼き込み、CanvasCompositorの各種オフスクリーン合成)は一時FBOを使い、
    // 後始末でフレームバッファのバインドを 0 に戻す。それらは元々「マウスイベント
    // 処理中に呼ばれる(=次のpaintGL()が改めて自前のFBOを束縛し直す)」前提で
    // 書かれていたが、実際にはこの関数の中からも呼ばれる経路がある。
    // 戻さないままだと、このフレームのclear/drawがウィジェットのFBOではなく
    // フレームバッファ0へ行き、描画結果がまるごと捨てられる。
    // 症状: ストローク開始直後の部分再描画(シザー)が画面に出ず、あとで全面
    // 再描画が来たときに初めてポンと現れる。以前は毎フレーム余分な全面再描画が
    // 走っていたためそれに隠れていたが、その無駄を削った結果表に出た。
    GLint prevDrawFbo = 0, prevReadFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);

    // フレームレート律速バッチ(Tool::flushPendingInput参照)。ペンタブの高頻度
    // サンプルをonMouseMove()側で貯めておき、実際に画面を更新するこのタイミングで
    // まとめて1回だけGPUディスパッチする(この中からnoteStrokeDirtyRegion()が
    // 呼ばれて下のシザー矩形が更新されるため、必ずシザー計算より先に行う)。
    if (Tool *t = currentTool()) t->flushPendingInput(toolCtx_);

    // ストローク中の部分再描画: 前回paint以降に実際に変更されたキャンバス領域が
    // 分かっている場合、その矩形(+マージン)だけをシザーで描き直し、残りは前フレームの
    // FBO内容(PartialUpdate)をそのまま使う。1回の描画コストが「ウィンドウ全画素×
    // アクティブ以上のレイヤー数」から「ブラシ周辺×同」へ激減し、レイヤー数や
    // ウィンドウサイズにほぼ依存しなくなる(他のペイントソフトと同じダーティ矩形方式)。
    bool partialPaint = false;
    int  scissorW = 0, scissorH = 0; // 診断ログ用
    if (strokeDirtyValid_) {
        Tool *t = currentTool();
        if (t && t->isActive()) {
            // キャンバスpx(Y上向き) → ウィジェット座標。回転・反転があっても正しく
            // 覆えるよう、矩形の4隅を変換してそのバウンディングボックスをとる。
            const QPointF c0 = toolCtx_.pixelToWidget(QVector2D(strokeDirtyMinX_, strokeDirtyMinY_));
            const QPointF c1 = toolCtx_.pixelToWidget(QVector2D(strokeDirtyMaxX_ + 1.0f, strokeDirtyMinY_));
            const QPointF c2 = toolCtx_.pixelToWidget(QVector2D(strokeDirtyMinX_, strokeDirtyMaxY_ + 1.0f));
            const QPointF c3 = toolCtx_.pixelToWidget(QVector2D(strokeDirtyMaxX_ + 1.0f, strokeDirtyMaxY_ + 1.0f));
            const qreal margin = 2.0;
            const qreal wx0 = qMin(qMin(c0.x(), c1.x()), qMin(c2.x(), c3.x())) - margin;
            const qreal wy0 = qMin(qMin(c0.y(), c1.y()), qMin(c2.y(), c3.y())) - margin;
            const qreal wx1 = qMax(qMax(c0.x(), c1.x()), qMax(c2.x(), c3.x())) + margin;
            const qreal wy1 = qMax(qMax(c0.y(), c1.y()), qMax(c2.y(), c3.y())) + margin;

            // ウィジェット座標(Y下向き) → FBO座標(Y上向き、物理px)
            const qreal dpr = devicePixelRatioF();
            const int fbW = qMax(1, (int)std::lround(width()  * dpr));
            const int fbH = qMax(1, (int)std::lround(height() * dpr));
            int sx0 = qBound(0, (int)std::floor(wx0 * dpr), fbW);
            int sx1 = qBound(0, (int)std::ceil (wx1 * dpr), fbW);
            int syTop    = qBound(0, (int)std::floor(wy0 * dpr), fbH);
            int syBottom = qBound(0, (int)std::ceil (wy1 * dpr), fbH);
            const int sy0 = fbH - syBottom; // GLのシザーは左下原点
            const int sw  = sx1 - sx0;
            const int sh  = syBottom - syTop;

            glEnable(GL_SCISSOR_TEST);
            glScissor(sx0, sy0, qMax(0, sw), qMax(0, sh));
            partialPaint = true;
            scissorW = qMax(0, sw);
            scissorH = qMax(0, sh);
        }
    }
    strokeDirtyValid_ = false;

    // フィルターレイヤーがある文書では、ここでオフスクリーンの連鎖を組み直して
    // 「どこまで合成済みか」と、その結果テクスチャを受け取る。フィルターが無ければ
    // 従来通りストローク中の下キャッシュだけを見る(戻り値<0で事前合成なし)。
    // renderProgram をbindする前に済ませること(内部で別のプログラムをbindするため)。
    GLuint compositeBaseTex     = belowCompositeTex;
    GLuint compositeBaseClipTex = belowCompositeClipBaseTex;
    const int compositeStartZ   = prepareCompositeBase(compositeBaseTex, compositeBaseClipTex);
    const qint64 nsAfterBase    = logPaint ? paintClock.nsecsElapsed() : 0;

    // 上のflushPendingInput()/prepareCompositeBase()が一時FBOを使って
    // バインドを外していたら、ここでこのフレーム用のFBOへ戻す(理由は関数冒頭の
    // prevDrawFbo のコメント参照)。以降のclear/drawがこのFBOへ入る。
    {
        GLint nowDraw = 0, nowRead = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &nowDraw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &nowRead);
        if (nowDraw != prevDrawFbo || nowRead != prevReadFbo) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDrawFbo);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevReadFbo);
            WINLOG(QStringLiteral("PERF paintGL: FBO rebound (draw %1->%2 read %3->%4) "
                                  "— これが無いとこのフレームは捨てられていた")
                       .arg(nowDraw).arg(prevDrawFbo).arg(nowRead).arg(prevReadFbo));
        }
    }

    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT); // シザー有効時はシザー矩形内だけクリアされる
    if (!renderProgram->bind()) {
        if (partialPaint) glDisable(GL_SCISSOR_TEST);
        lastPaintGlCostNs_ = paintClock.nsecsElapsed();
        paintGlNsAccum_ += lastPaintGlCostNs_;
        return;
    }

    setLayerUniformsForRender(renderProgram);

    renderProgram->setUniformValue("uViewMatrix",        view_.matrix());
    renderProgram->setUniformValue("uViewMatrixInverse", view_.inverseMatrix());
    renderProgram->setUniformValue("uActiveLayerIndex",  doc_->activeLayerIndex());
    renderProgram->setUniformValue("uIsEditingMaskLayer",
        (editingMaskLayerIndex_ >= 0 && editingMaskLayerIndex_ == doc_->activeLayerIndex()) ? 1 : 0);
    renderProgram->setUniformValue("uCanvasSize",        QVector2D(canvasW, canvasH));
    // ビューポート(=FBO)はデバイスピクセルなので、ここも合わせる。論理pxを渡すと
    // 拡大率1.0でも常にDPR倍へ拡大リサンプルされる(GLWidget.hのviewDpr()参照)。
    renderProgram->setUniformValue("uWindowSize",        QVector2D(viewWidth(), viewHeight()));
    // 拡大表示中だけ画素中心へ吸着させる判定に使う(render.frag の uViewScale 参照)
    renderProgram->setUniformValue("uViewScale",         view_.scale());
    // 縮小表示時の面積平均のサンプル数(1辺)。1画素が覆うキャンバスpxぶんを刻む。
    // 縮小するほどキャンバスが占める画面画素数は倍率の二乗で減るので、
    // 1画素あたりのサンプル数を1/倍率まで増やしても画面全体の処理量はほぼ一定。
    // 上限4は保険(極端な縮小でサンプル数が爆発しないように)。
    int minifySamples = qBound(1, (int)std::ceil(1.0f / qMax(view_.scale(), 0.0001f)), 4);

    // 【重要】キャンバスを移動/回転している間は1点サンプルまで落とす。
    //
    // このサンプル数は1画面画素あたりの composeCanvasAt() 呼び出し回数を
    // 二乗で増やす(3なら9回、上限の4なら16回)。実測(35%表示・キャンバス全面)で
    // この描画のGPU実処理は
    //     minify=1 → 約10ms / minify=2 → 約29ms / minify=3 → 約60ms
    // で、ほぼサンプル数の二乗に比例していた(TIEPOLO_GLFINISH=1 で実測)。ビュー変換のドラッグ中は
    // 毎フレーム全面を描き直す(ストロークのような部分再描画が効かない)ので、
    // これがそのまま1フレームの時間になり、60ms=約16fpsまで落ちる。これが
    // 「ビュー変換がカクつく」の本体だった。
    //
    // 面積平均は「止まっている絵の斜め線を階段状に見せない」ための品質処理で、
    // 動かしている最中は見えない。動かしている間だけ落とし、指を離した時点の
    // 再描画(mouseReleaseEvent)で本来の品質に戻す。
    //
    // 【計測の注意】paintGL本体の所要時間(lastPaintGlCostNs_)にはこのコストは
    // 現れない。GLの呼び出しは非同期で、CPUは命令を積んだら戻ってくるため
    // (実測0.3ms)。GPUの実処理はその後のpresentで待たされる形で現れるので、
    // 一見「Qtのウィンドウ合成が遅い」ように見える。切り分けるには
    // TIEPOLO_GLFINISH=1 を使うこと(下の glFinish のところ)。
    if (Tool *t = currentTool(); t && t->isActive() && t->transformsViewWhileActive())
        minifySamples = 1;
    renderProgram->setUniformValue("uMinifySamples", minifySamples);
    if (WinLog::enabled()) {
        static int lastLogged = -1;
        if (minifySamples != lastLogged) {
            lastLogged = minifySamples;
            WINLOG(QStringLiteral("PERF uViewScale=%1 uMinifySamples=%2 (loc=%3, -1ならシェーダーに届いていない)")
                       .arg(view_.scale()).arg(minifySamples)
                       .arg(renderProgram->uniformLocation("uMinifySamples")));
        }
    }
    renderProgram->setUniformValue("uTileSize",          TILE_SIZE);
    renderProgram->setUniformValue("uCanvasTilesX",      doc_->tilesX()); // レイヤーマスクのタイル参照(常にキャンバス全体)用

    // ブラシ色。消しゴムツールと透明色は「消す」動作になり、そのときの uBrushColor.a は
    // 色の不透明度ではなく「消す強さ」を表す(ToolConfig.h の EraseBrush / bake.comp 参照)。
    const float brushOpacity = (activeTool == ToolType::Airbrush) ? toolCfg_->airbrush().opacity()
                                                                  : toolCfg_->pen().opacity();
    const EraseBrush erase = eraseBrushFor(activeTool == ToolType::Eraser,
                                           toolCfg_->color(), brushOpacity);
    QColor col = erase.active ? erase.shaderColor()
                              : toPreMulColor(toolCfg_->color().rawRGBA(), brushOpacity);
    renderProgram->setUniformValue("uEraseMode", erase.active ? 1 : 0);
    // ブラシの合成モードはペン専用の設定(消す動作のときは意味を持たない)。
    renderProgram->setUniformValue("uBrushBlendMode",
        (activeTool == ToolType::Pen && !erase.active) ? toolCfg_->pen().brushBlendMode() : 0);
    // スタンプごとの色(色のランダム等)。焼き込み側(PenEraserTool)と同じ判定にする。
    const bool useStrokeColor = (activeTool == ToolType::Pen && !erase.active
                                 && toolCfg_->pen().usesPerStampColor() && strokeColorTex != 0);
    renderProgram->setUniformValue("uUseStrokeColor", useStrokeColor ? 1 : 0);
    if (useStrokeColor) {
        glActiveTexture(GL_TEXTURE0 + kStrokeColorTexUnit);
        glBindTexture(GL_TEXTURE_2D, strokeColorTex);
        glActiveTexture(GL_TEXTURE0);
    }
    renderProgram->setUniformValue("uBrushColor",
        col.redF(), col.greenF(), col.blueF(), col.alphaF());

    // レイヤーマスク編集中のライブプレビュー用。焼き込み側(PenEraserTool::onMouseRelease /
    // AirbrushTool::stampAndBake)がbake.compへ渡すのと同じ色をシェーダーへ渡し、
    // render.frag側で同じ式を先回りして「焼き込んだらこうなる」マスクを表示させる
    // (両者がずれるとプレビューと確定結果が食い違う。MaskBrush.hのコメント参照)。
    // マスクは色ではなく濃淡なので、「消す」ときは黒(=隠す)へその強さで寄せる。
    QColor maskCol = erase.active
        ? maskBrushColor(true, toolCfg_->color().rawRGBA(), erase.strength)
        : maskBrushColor(false, toolCfg_->color().rawRGBA(), brushOpacity);
    renderProgram->setUniformValue("uMaskBrushColor",
        maskCol.redF(), maskCol.greenF(), maskCol.blueF(), maskCol.alphaF());

    renderProgram->setUniformValue("uHasSelection",    hasSelection_ ? 1 : 0);
    renderProgram->setUniformValue("uIsSelectionTool", (activeTool == ToolType::Selection) ? 1 : 0);

    // 変形/自由変形/色調整/フィルター系(拡大・縮小・回転/自由変形/色相・彩度・明度/
    // 明るさ・コントラスト/カラーバランス/トーンカーブ/ガウスぼかし/モザイク/
    // カスタムシェーダー/色収差)の uIsXxxTool + origin/size 等の uniform は、
    // 各 CanvasAction (src/actions/) の applyRenderState() へ移動した。非アクティブな
    // アクションも自分の uniform を 0 に戻すため、登録済み全アクションに対して
    // 毎フレーム呼ぶ(コントローラ側の実装を参照)。
    actions_.applyRenderState(renderProgram);

    // 表示上の見た目だけを変えるカラーモード(RGB/CMYK擬似/グレースケール)。
    // ツールのactivate/deactivateとは無関係に、常にtoolCfg_->colorMode()の現在値を反映する。
    renderProgram->setUniformValue("uColorMode", (int)toolCfg_->colorMode().mode());

    // モニターキャリブレーション。値域はBrightnessContrastTool/ColorBalanceToolと
    // 同じ-100〜100の整数なので、同じ換算式でfloatに直す(GLWidget.cpp内の
    // uBrightnessShift/uContrastFactor/uCyanShift等の設定箇所と揃えてある)。
    const CalibrationConfig &cal = toolCfg_->calibration();
    renderProgram->setUniformValue("uCalBrightness", cal.brightness() / 200.0f);
    renderProgram->setUniformValue("uCalContrast",   1.0f + cal.contrast() / 100.0f);
    renderProgram->setUniformValue("uCalCyan",       cal.cyan()    / 100.0f);
    renderProgram->setUniformValue("uCalMagenta",    cal.magenta() / 100.0f);
    renderProgram->setUniformValue("uCalYellow",     cal.yellow()  / 100.0f);

    // キャンバス外側の背景色(実データには無関係の表示設定)
    {
        const QColor bg = toolCfg_->canvasBackground().color();
        renderProgram->setUniformValue("uCanvasOutsideBg",
            QVector3D(bg.redF(), bg.greenF(), bg.blueF()));
    }

    // 市松模様の2色(先端画像プレビュー・レイヤープレビューと共通のTheme値)
    renderProgram->setUniformValue("uCheckerColorA",
        QVector3D(Theme::checkerDark.redF(), Theme::checkerDark.greenF(), Theme::checkerDark.blueF()));
    renderProgram->setUniformValue("uCheckerColorB",
        QVector3D(Theme::checkerLight.redF(), Theme::checkerLight.greenF(), Theme::checkerLight.blueF()));

    // binding 番号は render.frag の layout(binding=N) に合わせる。
    // タイル配列(旧 layerTexArray)は複数バンクへ分割されたので、専用ヘルパーで
    // ユニット8..15へまとめてバインドし sampler2DArray 配列 uniform を設定する。
    bindLayerBanksForSampling(renderProgram);
    renderProgram->setUniformValue("maskTex",           2);
    renderProgram->setUniformValue("selectionMaskTex",  3);
    renderProgram->setUniformValue("gaussianBlurPreviewTex", 4);
    renderProgram->setUniformValue("toneCurveLUTTex", 5);
    renderProgram->setUniformValue("gradientMapLUTTex", 0);
    renderProgram->setUniformValue("belowCompositeTex",         6);
    renderProgram->setUniformValue("belowCompositeClipBaseTex", 7);
    renderProgram->setUniformValue("aboveCompositeTex",         1);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, aboveCompositeTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, maskTex);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, transformSrcTex);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, toneCurveLUTTex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gradientMapLUTTex);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, compositeBaseTex);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, compositeBaseClipTex);

    // item4: ストローク中(PenEraserToolがupdateBelowCompositeCache()で埋めた間)は、
    // 「アクティブレイヤーより下」をキャッシュテクスチャから読み、render.frag側は
    // z=アクティブレイヤー以降だけを合成し直す。フィルターレイヤーがある文書では
    // 代わりに連鎖の結果(=一番上のフィルターレイヤーまで合成済み)から始める。
    // どちらでもなければ毎フレーム通常通りz=0から全レイヤーを合成する。
    renderProgram->setUniformValue("uUseBelowCompositeCache", compositeStartZ >= 0 ? 1 : 0);
    renderProgram->setUniformValue("uCompositeStartZ",        compositeStartZ >= 0 ? compositeStartZ : 0);
    renderProgram->setUniformValue("uUseAboveCompositeCache", aboveCompositeCacheValid_ ? 1 : 0);

    glBindVertexArray(dummyVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    // 【計測用】TIEPOLO_GLFINISH=1 のときだけ有効。
    //
    // GLの呼び出しは非同期なので、paintGL本体の所要時間にはGPUの実処理が含まれない
    // (CPUが命令を積むまでの時間しか測れていない。実測0.3ms)。実処理はその後の
    // presentで待たされる形で現れるため、放っておくと「Qtのウィンドウ合成が遅い」と
    // 誤読する ―― 実際その取り違えをした。ここで待たせると、この描画のGPU実処理が
    // paintGL 側に現れて切り分けられる(常用すると描画が直列化して遅くなるので、
    // 計測のときだけ付けること)。
    if (qEnvironmentVariableIsSet("TIEPOLO_GLFINISH")) {
        QElapsedTimer gpuClock;
        gpuClock.start();
        glFinish();
        if (WinLog::enabled() && strokeFrameLogCount_ < 12)
            WINLOG(QStringLiteral("PERF gpu: 描画のGPU実処理 %1ms (minify=%2 layers=%3)")
                       .arg(gpuClock.nsecsElapsed() / 1e6, 0, 'f', 2)
                       .arg(minifySamples).arg(doc_ ? doc_->layerCount() : 0));
    }

    renderProgram->release();

    if (logPaint)
        WINLOG(QStringLiteral("PERF paintGL: %1 base=%2ms draw=%3ms")
                   .arg(partialPaint ? QStringLiteral("partial %1x%2").arg(scissorW).arg(scissorH)
                                     : QStringLiteral("FULL"))
                   .arg(nsAfterBase / 1e6, 0, 'f', 2)
                   .arg((paintClock.nsecsElapsed() - nsAfterBase) / 1e6, 0, 'f', 2));

    if (partialPaint) {
        // 部分再描画のときはQPainterオーバーレイ(選択範囲の破線等)は描き直さない。
        // 前フレームのFBOに描かれた分がそのまま残っており、ストローク中に
        // オーバーレイの形状は変わらないため見た目は維持される(破線のアニメーション
        // だけ一時停止するが実害はない)。ストローク終了後の通常paintで再開する。
        glDisable(GL_SCISSOR_TEST);
        lastPaintGlCostNs_ = paintClock.nsecsElapsed();
        paintGlNsAccum_ += lastPaintGlCostNs_;
        return;
    }

    // 投げ縄選択の軌跡プレビューなど、GLの描画が終わった後にQPainterで重ねる
    // オーバーレイ(ほとんどのツールはpaintOverlay()が空実装なので何も描かれない)。
    // 変形アクション実行中はactiveToolに関係なくtransformTool_の枠を描く。
    //
    // 【試して駄目だったこと】ビュー変換のドラッグ中にこのブロックを丸ごと省いて
    // みたが、1フレームは 22.5ms → 22.4ms でまったく変わらなかった(2026-08-12)。
    // ここは重くない。
    {
        QPainter painter(this);
        paintSelectionOutline(painter);
        if (!actions_.paintActiveOverlay(painter, toolCtx_)) {
            // controller管理アクション(変形/自由変形/キャンバスサイズ/色収差の円形ハンドル等)
            // が何も描かなければ、通常ツールのオーバーレイを描く。
            if (Tool *t = currentTool()) t->paintOverlay(painter, toolCtx_);
        }
    }
    lastPaintGlCostNs_ = paintClock.nsecsElapsed();
    paintGlNsAccum_ += lastPaintGlCostNs_;
}

// 選択範囲マスク(selectionMaskTex)の境界形状(キャンバスpx座標系)をキャッシュへ
// 再計算する。QRegion(QBitmap::fromImage(...))で境界を求めることで、投げ縄/ペン選択の
// ような任意形状にも正しく対応する。selectionMaskTexの中身が実際に変わった時
// (selectAll/clearSelection/投げ縄・ペン選択の確定等)だけ呼べばよい重い処理
// (glGetTexImage+全ピクセル走査+QRegion分解)なので、paintSelectionOutline()から
// 分離してある。
void GLWidget::rebuildSelectionOutlineCache()
{
    QVector<uint8_t> buf(canvasW * canvasH);
    glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, buf.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);

    QImage maskImg(canvasW, canvasH, QImage::Format_Mono);
    maskImg.setColor(0, qRgb(0, 0, 0));
    maskImg.setColor(1, qRgb(255, 255, 255));
    maskImg.fill(0);
    for (int y = 0; y < canvasH; y++) {
        for (int x = 0; x < canvasW; x++) {
            if (buf[y * canvasW + x] != 0) maskImg.setPixel(x, y, 1);
        }
    }

    const QRegion region(QBitmap::fromImage(maskImg));
    // QBitmap::fromImage()は全面白(=全域選択。全選択アクション直後がこれにあたる)の
    // 画像を変換すると空のQRegionになる(QBitmapは伝統的に「黒=set」の解釈を持つため、
    // 全白画像には有効ビットが1つも無いと判定される)。ここでhasSelection_は既にtrueと
    // 分かっているため(関数冒頭でfalseなら早期return済み)、regionが空ならそれは
    // 「選択が無い」のではなく「キャンバス全域が選択されている」ことを意味する。
    // その場合はキャンバス全体を囲む矩形を輪郭として使う。
    QPainterPath canvasPath;
    if (region.isEmpty()) {
        canvasPath.addRect(0, 0, canvasW, canvasH);
    } else {
        canvasPath.addRegion(region);
        // addRegion()は矩形の集合(1走査行の連続run単位でまとめられるため、斜めの辺を
        // 持つ形状だと行ごとに別々の矩形になり、多いと数百枚)をそのまま返す。この矩形群を
        // simplified()せずに輪郭線として描画すると、隣り合う矩形どうしの内部辺(本来は
        // 見えるべきでない、形状内部の水平な仕切り線)まで大量に重なって描かれてしまい、
        // 選択範囲の縦幅ぶんが太い帯のように潰れて見えてしまっていた。simplified()で
        // 矩形群を1つの輪郭(外周だけ)に統合してから描画する。
        canvasPath = canvasPath.simplified();
    }

    selectionOutlineCachePx_ = canvasPath;
}

void GLWidget::paintSelectionOutline(QPainter &painter)
{
    if (!hasSelection_ || canvasW <= 0 || canvasH <= 0 || !toolCtx_.pixelToWidget) return;

    if (selectionOutlineCacheDirty_) {
        rebuildSelectionOutlineCache();
        selectionOutlineCacheDirty_ = false;
    }

    // 変形中は selectionMaskTex 自体がまだ動いていない(確定時に transform.comp が
    // 動かす)ため、輪郭キャッシュも元の位置のまま。何もしないとドラッグ中だけ点線が
    // 取り残されるので、アクティブなアクションに各点をプレビューと同じ変換で写して
    // もらう(CanvasAction::mapSelectionOutlinePoint参照。変形系以外は何もしない)。
    CanvasAction *activeAct = actions_.activeAction();

    // 【軽量化】ウィジェット座標へ変換したパスをキャッシュする。
    //
    // 輪郭キャッシュ(selectionOutlineCachePx_)はキャンバスpx座標系なので、描くには
    // ウィジェット座標へ移す必要がある。以前はこれを毎フレーム「点ごとに」やっていたが、
    // ペンで作った選択範囲はQRegion由来で数千セグメントになるため、この作り直しだけで
    // 1フレーム十数msかかっていた(実測で paintGL の97%がこのオーバーレイだった)。
    //
    // 実際にパスが変わるのは「選択範囲そのものが変わった(dirtyフラグ)」「ビュー変換が
    // 変わった(パン・ズーム・回転・反転)」「ウィジェットサイズが変わった」ときだけ。
    // 投げ縄のドラッグ中はどれも変わらないので、作り直しは完全に無駄だった。
    // 変形アクション中だけは毎フレーム写る位置が変わるのでキャッシュしない。
    const QMatrix4x4 viewMatrix = view_.matrix();
    const bool mapsPerFrame = (activeAct != nullptr); // 変形系はドラッグ中パスが動く
    if (!selectionOutlineWidgetPathValid_ || mapsPerFrame
        || viewMatrix != selectionOutlineWidgetPathView_
        || size() != selectionOutlineWidgetPathSize_) {
        QPainterPath widgetPath;
        for (int i = 0; i < selectionOutlineCachePx_.elementCount(); i++) {
            const QPainterPath::Element el = selectionOutlineCachePx_.elementAt(i);
            QPointF cp(el.x, el.y);
            if (activeAct) activeAct->mapSelectionOutlinePoint(cp);
            const QPointF wp = toolCtx_.pixelToWidget(QVector2D(cp.x(), cp.y()));
            if (el.type == QPainterPath::MoveToElement) widgetPath.moveTo(wp);
            else widgetPath.lineTo(wp); // addRegion()は矩形の集合なのでMoveTo/LineToのみ
        }
        selectionOutlineWidgetPath_     = widgetPath;
        selectionOutlineWidgetPathView_ = viewMatrix;
        selectionOutlineWidgetPathSize_ = size();
        // 変形中は次フレームも作り直す必要があるのでキャッシュ有効にはしない
        selectionOutlineWidgetPathValid_ = !mapsPerFrame;
    }
    const QPainterPath &widgetPath = selectionOutlineWidgetPath_;

    // マーチングアンツはQRegionの境界(=軸平行の1px線)なので、アンチエイリアスを
    // 掛けても見た目はほぼ変わらないのに、数千セグメントの破線ストロークでは
    // ラスタライズ費用が跳ね上がる。切っておく。
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setBrush(Qt::NoBrush);

    QPen whitePen(QColor(255, 255, 255, 235));
    whitePen.setWidthF(1.0);
    whitePen.setStyle(Qt::DashLine);
    painter.setPen(whitePen);
    painter.drawPath(widgetPath);

    QPen blackPen(QColor(0, 0, 0, 235));
    blackPen.setWidthF(1.0);
    blackPen.setStyle(Qt::DashLine);
    blackPen.setDashOffset(4.0); // 白の破線と半周期ずらして交互に見せる
    painter.setPen(blackPen);
    painter.drawPath(widgetPath);
}

void GLWidget::setLayerUniformsForRender(QOpenGLShaderProgram *prog)
{
    compositor_.setLayerUniformsForRender(toolCtx_, prog);
}

// ===========================================================================
// マウスイベント
// ===========================================================================
// mousePressEvent/mouseMoveEventのどちらでも使う、このイベントに適用すべき筆圧。
//
// 以前はQMouseEvent::source()で「タブレット由来の合成イベントか、素のマウスか」を
// 判定し(NotSynthesizedなら素のマウスとみなして1.0にリセット)、素のマウスならば
// event->points().first().pressure()を、そうでなければtabletEvent()由来の値を
// 使い分けていた。しかしWindows環境ではOSのペン→マウス互換レイヤーが、Qtが
// タブレットイベントから合成する正規のマウスイベントとは別に、同じ物理接触に対して
// 本物の(=event->source()では区別が付かない)マウスイベントを重複して送ってくる
// ことがある。この重複イベントのpoints().first().pressure()は実際の筆圧を反映して
// いないことが多く、従来のロジックでは「タブレットで描いている最中なのに素の
// マウス操作と誤認してフル筆圧(1.0)を使ってしまう」ことがあった
// (書き始め・ストローク中を問わず、太い点が混じる不具合の原因)。
//
// tabletEvent()は同じ物理サンプルについて必ずこれらマウスイベントより先に届き、
// 実際のハードウェア筆圧を直接持っている。直近(数十ms以内)にtabletEvent()が
// 届いていればそれを信頼できるタブレット操作中とみなしてその値を使い、
// 届いていなければ素のマウス操作とみなしてevent->points()の値(通常1.0)を使う。
float GLWidget::resolvePointerPressure(const QMouseEvent *event) const
{
    constexpr qint64 kTabletFreshnessMs = 80;
    if (lastTabletEventClock_.isValid() && lastTabletEventClock_.elapsed() < kTabletFreshnessMs)
        return lastTabletPressure_;
    if (!event->points().isEmpty())
        return event->points().first().pressure();
    return 1.0f;
}

// 生の筆圧(0..1)を、実際にツールへ渡す筆圧へ写像する。
//
// 環境設定の「全体の筆圧カーブ」→ アクティブツールの筆圧カーブ、の順に通す
// (PressureCurve のコメント参照)。全体側でタブレットの硬さの癖を一度ならし、
// そのうえでツール/プリセットごとの効き方を作る、という二段構え。
//
// 保持している生の値(brushPressure / lastTabletPressure_)は素のままにしておき、
// Tool::setPressure() へ渡す直前のここだけで写像する。そうしておかないと、
// tabletEvent() が合成マウスイベントを流す経路(driveMouse)で二重に適用される。
// 直近のタブレットイベント由来の傾き・回転をツールへ渡す。
// 判定は resolvePointerPressure と同じ「直近にタブレットイベントが来ているか」で、
// マウス操作中は傾きも回転も無いものとして0を渡す。
void GLWidget::applyPointerTilt(Tool *tool) const
{
    if (!tool) return;
    constexpr qint64 kTabletFreshnessMs = 80;
    const bool tablet = lastTabletEventClock_.isValid()
                     && lastTabletEventClock_.elapsed() < kTabletFreshnessMs;
    if (tablet) tool->setTilt(lastTabletTiltAmount_, lastTabletTiltAngle_, lastTabletRotation_);
    else        tool->setTilt(0.0f, 0.0f, 0.0f);
}

float GLWidget::mapPressure(float rawPressure) const
{
    if (!toolCfg_) return rawPressure;

    float p = toolCfg_->globalPressureCurve().apply(rawPressure);

    // 筆圧を使うツールだけがカーブを持つ(ToolConfig参照)。
    switch (activeTool) {
    case ToolType::Pen:      p = toolCfg_->pen().pressureCurve().apply(p);      break;
    case ToolType::Eraser:   p = toolCfg_->eraser().pressureCurve().apply(p);   break;
    case ToolType::Airbrush: p = toolCfg_->airbrush().pressureCurve().apply(p); break;
    case ToolType::Blur:     p = toolCfg_->blur().pressureCurve().apply(p);     break;
    case ToolType::Warp:     p = toolCfg_->warp().pressureCurve().apply(p);     break;
    default: break;
    }
    return p;
}

void GLWidget::mousePressEvent(QMouseEvent *event) {
    // 分割表示での操作権の移動(クリックしたペインをアクティブにする)は、キャンバスに
    // 限らずスタートページやタブバーのクリックでも効く必要があるため、ここではなく
    // MainWindow::eventFilter()側でqApp全体のマウス押下を見て一括で処理している。
    if (!glReady_) return; // initializeGL()完了前にマウス操作が届いた場合は無視する
    // ペン先ストロークをtabletEvent()から直接駆動している間、OS互換レイヤー由来の
    // 実マウスイベントは重複ストリームなので無視する(GLWidget.hのtabletDriving_
    // コメント参照)。ただしTabletReleaseの取りこぼし等でフラグが残った場合に
    // マウスが死なないよう、タブレットイベントが途絶えて久しければ解除する。
    if (tabletDriving_ && !dispatchingSyntheticTabletMouse_) {
        if (lastTabletEventClock_.isValid() && lastTabletEventClock_.elapsed() > 500)
            tabletDriving_ = false; // 保険(以後は通常のマウス操作として続行)
        else
            return;
    }
    if (event->button() != Qt::LeftButton) return;
    // controller管理アクション(色収差の円形ハンドル等)は、パネル表示中でも
    // (activeToolに関係なく)ヒットした場合だけ最優先で処理する。ヒットしなければ
    // 何もせず、この後の通常のツール排他ガード(Move/Rotateのみ通す)に委ねる。
    if (actions_.routeMousePress(event, toolCtx_)) {
        updateCursor();
        return;
    }
    // パネル自体はQWidgetなので自分でイベントを受け取る。パネル外は無視。ただし
    // 色調整/フィルター系パネル表示中に移動・回転ツールへ切り替えている場合だけは、
    // パネルを開いたまま視点操作できるように素通しする。
    {
        const bool colorPanelActive = actions_.toolInputBlocked();
        const bool otherPanelBlocking = actions_.toolInputFullyBlocked();
        if (otherPanelBlocking) return;
        if (colorPanelActive && activeTool != ToolType::Move && activeTool != ToolType::Rotate) return;
    }

    makeCurrent();
    if (Tool *t = currentTool()) {
        QElapsedTimer pressClock;
        if (WinLog::enabled()) pressClock.start();
        t->setPressure(mapPressure(resolvePointerPressure(event)));
        applyPointerTilt(t);
        t->onMousePress(event, toolCtx_);
        // 入力の滞留を測る基準をここに置く(1ドラッグごとにリセット)。
        inputBaseTimestamp_ = event->timestamp();
        inputBaseClock_.start();
        inputAgeMs_ = 0;
        const qint64 nsAfterTool = WinLog::enabled() ? pressClock.nsecsElapsed() : 0;
        // フレームレート律速バッチ(GLWidget.hのコメント参照)。ストローク中だけ
        // 定期フラッシュタイマーを動かす。最初の1点は待たせず即flushして、
        // 打ち始めの点がすぐ出るようにする。
        if (t->isActive()) {
            // 最初の1点を即flush + 同期描画(repaint)で置いた瞬間に出す。
            // 以降のストローク描画はmouseMoveEvent内の時間スロットリングrepaintが駆動し、
            // inputFlushTimer_は「入力が途切れた瞬間」に末尾を拾うフォールバック。
            t->flushPendingInput(toolCtx_);
            const qint64 nsAfterFlush = WinLog::enabled() ? pressClock.nsecsElapsed() : 0;
            inputFlushTimer_->start();
            strokePaintIntervalMs_ = kStrokePaintIntervalMs; // 適応間隔をリセット
            strokePaintClock_.start();
            strokeFrameLogCount_ = 0;
            viewDiagCount_ = 0;
            inSyncRepaint_ = true;
            repaint();
            inSyncRepaint_ = false;
            lastRepaintDoneClock_.restart();
            if (WinLog::enabled()) {
                WINLOG(QStringLiteral("PERF stroke: press onMousePress=%1ms flush=%2ms repaint=%3ms total=%4ms")
                           .arg(nsAfterTool / 1e6, 0, 'f', 2)
                           .arg((nsAfterFlush - nsAfterTool) / 1e6, 0, 'f', 2)
                           .arg((pressClock.nsecsElapsed() - nsAfterFlush) / 1e6, 0, 'f', 2)
                           .arg(pressClock.nsecsElapsed() / 1e6, 0, 'f', 2));
            }
        }
    }
    updateCursor();
}

void GLWidget::mouseMoveEvent(QMouseEvent *event) {
    if (!glReady_) return;
    // タブレット直接駆動中の実マウスイベントは無視(mousePressEvent参照)
    if (tabletDriving_ && !dispatchingSyntheticTabletMouse_) return;
    // controller管理アクション(色収差の中心ハンドルドラッグ等)は、activeToolに
    // 関係なく最優先で処理し続ける(mousePressEvent側でヒットした時にのみ
    // ドラッグ中フラグが立ち、以後 routeMouseMove が消費し続ける)。
    if (actions_.routeMouseMove(event, toolCtx_)) {
        updateCursor();
        update();
        return;
    }
    // 各ToolがonMouseMove内部で「自分がドラッグ中か」を自己判定するので、
    // ここでは無条件に委譲するだけでよい。
    {
        const bool colorPanelActive = actions_.toolInputBlocked();
        const bool otherPanelBlocking = actions_.toolInputFullyBlocked();
        if (otherPanelBlocking) return;
        if (colorPanelActive && activeTool != ToolType::Move && activeTool != ToolType::Rotate) return;
    }
    if (Tool *t = currentTool()) {
        // 【重要】onMouseMove()の中でGLを触るツールがあるので、先にコンテキストを
        // カレントにしておく(mousePressEvent/mouseReleaseEventは元から同じことをしている)。
        //
        // ペン/消しゴム/エアブラシのonMouseMove()はCPUで入力を貯めるだけで、実際の
        // ディスパッチは下のflushPendingInput()(makeCurrent()済み)で行うため問題に
        // ならなかったが、選択ツール(SelectTool)やぼかし/ゆがみはonMouseMove()から
        // 直接コンピュートシェーダーをディスパッチする。カレントでないコンテキストへの
        // GL呼び出しは黙って捨てられるため、ペン選択では「マウスを押した瞬間の1点
        // (mousePressEvent側なのでmakeCurrent済み)しかマスクに残らない」=
        // ドラッグし始めだけ選択される、あるいは何も残らず選択が変わらない、という
        // 症状になっていた。タブが複数あるとコンテキストが切り替わるため再現性も
        // まちまちだった。
        //
        // 既にカレントなら実質早期リターンで済むので、ストローク中の追従性への影響は
        // 無視できる(以前ここから除去したQCursor::setPos()のようなプロセス横断の
        // 重いシステムコールとは性質が違う)。
        makeCurrent();
        t->setPressure(mapPressure(resolvePointerPressure(event)));
        applyPointerTilt(t);
        t->onMouseMove(event, toolCtx_); // ペン系はここでは入力を貯めるだけ(CPUのみ)

        // 【重要】Windowsでは、ペンを連続で動かしている間は入力キューが空にならないため、
        // WM_TIMER(QTimer)もupdate()が積むペイントイベントも一切配信されない
        // (どちらもキューが空のとき初めて処理される低優先度メッセージ)。つまり
        // タイマー駆動でもupdate()でも、ドラッグ中は画面が全く更新されず、指を止めて
        // 初めて溜まった線がまとめて出る——「点だけ→全体が現れる」「レイヤーが多いと
        // いきなり全部出る」の正体はこれ。連続入力中に確実に描くには、この
        // 入力ハンドラの中から同期描画(repaint)を直接呼ぶしかない。
        //
        // ただし毎イベントrepaintするとpaintGLの完了待ちで入力が詰まるので、前回の
        // 実描画から一定時間経ったときだけrepaintする(自然にpaintGLの処理レートへ
        // 律速され、速いほど滑らか・遅くても入力を溜め込まない)。GPU flushもここで
        // まとめて行う(貯めたスタンプを1回のディスパッチに)。
        // 【重要・ペンタブ】溜まった入力を先に捌いてから描く。
        //
        // repaint() は同期で、present(vsync待ち)を含めて実測15〜20msかかる。
        // 一方この判定は「前回の描画開始から12ms経ったか」なので、repaint()から
        // 戻った時点で既に成立している ― つまり "1イベント処理するたびに1回描く"
        // 動きになる。処理できるのは毎秒 1000/17 ≒ 59イベントが上限。
        //
        // マウスはOSが移動イベントを間引くので、アプリに届くのは実測50〜90件/秒。
        // 上限内に収まるので問題にならなかった。ところがペンタブ(WM_POINTER由来の
        // QTabletEvent)は間引かれず、実測191件/秒届く。処理が追いつかず入力キューが
        // 際限なく伸び、画面はポインタから遅れる一方になる ― これが
        // 「ペン先にたどり着くまで非常に遅い」の正体。
        //
        // そこで「今処理しているイベントが発生してから何ms経っているか」を見る。
        // 溜まっている間は描画を見送り、返って残りのイベントを捌かせる(1件あたり
        // 数µsなのですぐ追いつく)。追いついた=最新のイベントになった時点で描くので、
        // 結果として「溜まったぶんを捨てずに捌いて、最新の位置を1回描く」になる。
        //
        // 追いついているときは inputAgeMs_ がほぼ0なので、従来どおり最速で描く
        // (マウス操作やペンの遅い動きでの追従性は変わらない)。
        // 時計が信用できない環境で描画が止まらないよう、一定時間描いていなければ
        // 無条件で描く保険も入れてある。
        constexpr qint64 kMaxInputAgeMs   = 6;   // これを超えていたら「溜まっている」
        constexpr qint64 kForcePaintAfter = 40;  // 保険: これだけ描いていなければ描く
        updateInputAge(event);
        const bool caughtUp = (inputAgeMs_ <= kMaxInputAgeMs);
        const bool starved  = lastRepaintDoneClock_.isValid()
                           && lastRepaintDoneClock_.elapsed() >= kForcePaintAfter;
        if (t->isActive() && t->needsCanvasRepaintWhileActive() && (caughtUp || starved)
            && (!strokePaintClock_.isValid() || strokePaintClock_.elapsed() >= strokePaintIntervalMs_)) {
            const qint64 sinceLastPaintMs = strokePaintClock_.isValid() ? strokePaintClock_.elapsed() : -1;
            makeCurrent();
            t->flushPendingInput(toolCtx_);
            // 【重要】間隔の起点は repaint() の「開始」に置く(終了後にrestartしない)。
            // repaint()の中にはpresent(vsync待ち)が含まれ、実測で1回あたり13〜26ms
            // かかる。終了時点を起点にすると 実質間隔 = present待ち + 12ms となり、
            // 60Hzの画面に対して35fps程度まで落ちてしまう(そのぶん線がペン先から
            // 遅れる)。開始時点を起点にすれば 実質間隔 = max(12ms, present待ち) に
            // なり、画面が出せる最大レートでそのまま追従できる。
            strokePaintClock_.restart();
            QElapsedTimer paintCost;
            paintCost.start();
            const quint64 callsBefore = paintGlCalls_;
            const qint64  glNsBefore  = paintGlNsAccum_;
            inSyncRepaint_ = true;
            repaint();
            inSyncRepaint_ = false;
            lastRepaintDoneClock_.restart();
            if (WinLog::enabled() && strokeFrameLogCount_ < 12) {
                strokeFrameLogCount_++;
                // repaint() の中身を「paintGL本体(CPU)」と「それ以外」に割る。
                //
                // 【読み方の注意】paintGL の値はCPUが命令を積むまでの時間でしかない。
                // GLの呼び出しは非同期なので、自分の描画のGPU実処理は「それ以外」の側に
                // 入ってくる ―― つまりここが大きくても、Qtのウィンドウ合成が遅いとは
                // 限らない(実測でその取り違えをした)。内訳を確かめるには
                // TIEPOLO_GLFINISH=1 を付けて PERF gpu の行を見ること。
                const double totalMs = paintCost.nsecsElapsed() / 1e6;
                const double glMs    = (paintGlNsAccum_ - glNsBefore) / 1e6;
                WINLOG(QStringLiteral("PERF stroke: frame#%1 gapSincePrev=%2ms repaint=%3ms "
                                      "paintGL(CPU)=%4ms x%5 GPU待ち+合成=%6ms prevInterval=%7ms")
                           .arg(strokeFrameLogCount_).arg(sinceLastPaintMs)
                           .arg(totalMs, 0, 'f', 2)
                           .arg(glMs, 0, 'f', 2)
                           .arg(paintGlCalls_ - callsBefore)
                           .arg(totalMs - glMs, 0, 'f', 2)
                           .arg(strokePaintIntervalMs_));
            }
            // 実測した描画コストに応じて次回の間隔を適応させる(ヘッダの
            // strokePaintIntervalMs_コメント参照)。部分再描画により通常は数ms以下に
            // 収まり最短間隔のままになるが、万一重い環境・状況でも入力処理が
            // 半分以上の時間を確保できるため、入力キューが溜まって線が大きく
            // 遅れて追いかけてくる状態にはならない。
            //
            // 材料は repaint() 全体ではなく paintGL() 本体の時間
            // (lastPaintGlCostNs_)。repaint()にはpresent(vsync待ち・GPUキューの
            // 消化待ち)が含まれ、それを「重い」と解釈して間隔を倍にすると、
            // 表示可能な速度より遅く描くことになり、書き始めのカクつきそのものを
            // 生んでいた(ヘッダのlastPaintGlCostNs_のコメント参照)。
            strokePaintIntervalMs_ =
                qBound(kStrokePaintIntervalMs, (int)(lastPaintGlCostNs_ / 1000000) * 2, 100);
        }
    }
    updateCursor();
}

void GLWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (!glReady_) return;
    // タブレット直接駆動中の実マウスイベントは無視(mousePressEvent参照)。
    // 特に実マウスのReleaseを通すとストロークが途中で確定されてしまう。
    if (tabletDriving_ && !dispatchingSyntheticTabletMouse_) return;
    if (event->button() != Qt::LeftButton) return;
    if (actions_.routeMouseRelease(event, toolCtx_)) {
        updateCursor();
        return;
    }
    {
        const bool colorPanelActive = actions_.toolInputBlocked();
        const bool otherPanelBlocking = actions_.toolInputFullyBlocked();
        if (otherPanelBlocking) return;
        if (colorPanelActive && activeTool != ToolType::Move && activeTool != ToolType::Rotate) return;
    }

    makeCurrent();
    // ビュー変換のドラッグだったか(onMouseRelease でisActive()が落ちるので先に見る)
    bool wasViewTransform = false;
    if (Tool *t = currentTool()) {
        wasViewTransform = t->isActive() && t->transformsViewWhileActive();
        t->onMouseRelease(event, toolCtx_);
    }
    // ドラッグ中は縮小表示のサンプル数を1に落としているので、離した時点で必ず
    // 1枚描き直して本来の品質に戻す(paintGL の minifySamples のコメント参照)。
    // 移動/回転ツールの onMouseRelease は再描画を要求しないため、ここで出さないと
    // 荒いままの絵が残る。
    if (wasViewTransform) update();
    // ドラッグ中に止めていた viewChanged() をここで1回だけ出す
    // (NavigatorDockの表示範囲枠が最終位置へ揃う)。
    emitViewChangedIfPending();
    // フレームレート律速バッチ用の定期フラッシュタイマーはストローク中だけ動かす。
    inputFlushTimer_->stop();
    updateCursor();
}

// 今処理している入力イベントが「発生してから何ms経っているか」を更新する。
//
// event->timestamp() はOSがイベントを作った時刻(ms)。これとこちらの経過時間を
// 比べると、入力キューにどれだけ溜まっているかが分かる。時計の基準が違うので、
// ドラッグ開始時点を0として「そこからどれだけ余分に遅れたか」を見る。
// タイムスタンプが取れない経路では0(=遅れ無し)として扱い、判定を素通しさせる。
void GLWidget::updateInputAge(const QMouseEvent *event)
{
    if (inputBaseTimestamp_ == 0 || event->timestamp() == 0 || !inputBaseClock_.isValid()) {
        inputAgeMs_ = 0;
        return;
    }
    const qint64 osElapsed = (qint64)event->timestamp() - (qint64)inputBaseTimestamp_;
    inputAgeMs_ = qMax<qint64>(0, inputBaseClock_.elapsed() - osElapsed);
}

// ドラッグ中に止めていた viewChanged() が残っていれば、ここで1回だけ出す
// (止める理由は setupToolContext() の requestRepaint のコメント参照)。
void GLWidget::emitViewChangedIfPending()
{
    if (!viewChangedPending_) return;
    viewChangedPending_ = false;
    emit viewChanged();
}

void GLWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    if (!glReady_) return;
    // タブレット直接駆動中の実マウスイベントは無視(mousePressEvent参照)
    if (tabletDriving_ && !dispatchingSyntheticTabletMouse_) return;
    if (event->button() != Qt::LeftButton) return;
    if (actions_.routeMouseDoubleClick(event, toolCtx_)) {
        updateCursor();
        return;
    }
    {
        const bool colorPanelActive = actions_.toolInputBlocked();
        const bool otherPanelBlocking = actions_.toolInputFullyBlocked();
        if (otherPanelBlocking) return;
        if (colorPanelActive && activeTool != ToolType::Move && activeTool != ToolType::Rotate) return;
    }
    if (isTransformActionActive() || isFreeTransformActionActive() || isCanvasSizeActionActive()) return;

    makeCurrent();
    if (Tool *t = currentTool()) t->onMouseDoubleClick(event, toolCtx_);
    updateCursor();
}

void GLWidget::wheelEvent(QWheelEvent *event) {
    if (!glReady_) return;

    // Shift+スクロール: ズームではなく横方向のパンにする(横スクロールホイールが
    // 無いマウスでも、キャンバスを横に広く見たいときに片手で操作できるようにするため)。
    if (event->modifiers() & Qt::ShiftModifier) {
        // ビューはデバイスピクセルなので、スクロール量(論理px相当)を換算して渡す
        const float dx = event->angleDelta().y() / 120.0f * 80.0f * viewDpr();
        view_.pan(QVector2D(dx, 0.0f));
        update();
        emit viewChanged();
        return;
    }

    float factor  = std::pow(1.15f, event->angleDelta().y() / 120.0f);
    const float d = viewDpr(); // マウス位置(論理px)をビュー空間(デバイスpx)へ
    QVector2D pos(event->position().x() * d, event->position().y() * d);
    view_.zoomAround(pos, factor);
    updateCursor(); // ズームでペン円カーソルの画面上サイズが変わるため
    update();
    emit viewChanged();
}

void GLWidget::tabletEvent(QTabletEvent *event) {
    brushPressure = event->pressure();
    lastTabletPressure_ = brushPressure;

    // 傾きとペン軸まわりの回転。
    //
    // xTilt/yTilt は度数(おおむね±60°が最大)で、ペンをどちらへどれだけ寝かせたかを
    // 2軸で表す。ツール側が使いやすいよう「寝かせ具合(0〜1)」と「寝かせた向き」へ
    // 分解して渡す。yTilt は画面下向きが正なので、キャンバス座標(Y上向き)に
    // 合わせるため符号を反転してから角度を求める(先端の回転角と同じ座標系にする)。
    //
    // rotation はアートペンなど一部のペンだけが返す軸回転。非対応機では常に0。
    {
        constexpr float kMaxTiltDeg = 60.0f;
        const float tx = event->xTilt();
        const float ty = -event->yTilt();
        const float mag = std::hypot(tx, ty);
        lastTabletTiltAmount_ = qBound(0.0f, mag / kMaxTiltDeg, 1.0f);
        // 垂直に近いときの角度は数値的に暴れるので、その場合は前回の向きを保つ
        if (mag > 1.0f) lastTabletTiltAngle_ = std::atan2(ty, tx);
        lastTabletRotation_ = qDegreesToRadians(event->rotation());

        WINLOG_V(QStringLiteral("TABLET tilt=(%1,%2)deg amount=%3 angle=%4deg rot=%5deg press=%6")
                     .arg(event->xTilt()).arg(event->yTilt())
                     .arg(lastTabletTiltAmount_, 0, 'f', 3)
                     .arg(qRadiansToDegrees(lastTabletTiltAngle_), 0, 'f', 1)
                     .arg(event->rotation(), 0, 'f', 1)
                     .arg(brushPressure, 0, 'f', 3));
    }

    lastTabletEventClock_.start();
    if (Tool *t = currentTool()) {
        t->setPressure(mapPressure(brushPressure));
        applyPointerTilt(t);
    }

    // ペン先(左ボタン相当)のダウン/ムーブ/アップは、OS/Qtのマウスイベント合成を
    // 待たずここから直接駆動する。合成経路に任せると、Windowsの
    // 「プレス&ホールドで右クリック」ジェスチャ判定のため、ペンを置いてから
    // 合成マウスダウンが届くまで数百ms(またはペンが一定距離動くまで)保留される
    // ことがあり、「ペンを置いてから線が出るまでがかなり長い」
    // 「短い線(文字)ほど毎回最大まで待たされて書けない」の原因になっていた。
    // tabletEvent自体はWM_POINTER由来でジェスチャ判定に保留されず即座に届く。
    //
    // accept()してQt側の合成を止めた上で、OS互換レイヤー由来の「本物の」マウス
    // ストリームが重複して届く環境に備え、駆動中(tabletDriving_)は
    // mousePressEvent等の実マウスイベントを無視する(合成の見分け方は
    // GLWidget.hのdispatchingSyntheticTabletMouse_コメント参照)。
    const auto driveMouse = [this, event](QEvent::Type type) {
        QMouseEvent me(type, event->position(), event->globalPosition(),
                       Qt::LeftButton,
                       (type == QEvent::MouseButtonRelease) ? Qt::NoButton : Qt::LeftButton,
                       event->modifiers());
        // 元のタブレットイベントの発生時刻を引き継ぐ。合成したQMouseEventは既定で
        // タイムスタンプが0になり、「このイベントはいつ発生したものか」が失われる。
        // 入力が溜まっているかどうかの判定(mouseMoveEventの入力滞留チェック)は
        // これを基準にしているので、引き継がないとペンタブ経路だけ判定が効かない。
        me.setTimestamp(event->timestamp());
        dispatchingSyntheticTabletMouse_ = true;
        switch (type) {
        case QEvent::MouseButtonPress:    mousePressEvent(&me);       break;
        case QEvent::MouseMove:           mouseMoveEvent(&me);        break;
        case QEvent::MouseButtonRelease:  mouseReleaseEvent(&me);     break;
        case QEvent::MouseButtonDblClick: mouseDoubleClickEvent(&me); break;
        default: break;
        }
        dispatchingSyntheticTabletMouse_ = false;
    };

    switch (event->type()) {
    case QEvent::TabletPress:
        if (event->button() == Qt::LeftButton) {
            tabletDriving_ = true;
            driveMouse(QEvent::MouseButtonPress);
            // マウス合成を止めるとダブルクリックも合成されなくなるため、
            // ダブルタップ(時間・距離とも近い2度目のダウン)を自前で検出して
            // ダブルクリック動作(テキストボックスの再編集等)も発火させる。
            if (tabletLastPressClock_.isValid() && tabletLastPressClock_.elapsed() <= 400
                && (event->position() - tabletLastPressPos_).manhattanLength() <= 8.0)
                driveMouse(QEvent::MouseButtonDblClick);
            tabletLastPressClock_.start();
            tabletLastPressPos_ = event->position();
            event->accept();
            return;
        }
        break;
    case QEvent::TabletMove:
        if (tabletDriving_) {
            driveMouse(QEvent::MouseMove);
            event->accept();
            return;
        }
        break;
    case QEvent::TabletRelease:
        if (tabletDriving_) {
            driveMouse(QEvent::MouseButtonRelease);
            tabletDriving_ = false;
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    // ペン先以外(サイドボタン=右クリック等)やホバー移動は従来通り
    // Qtのマウスイベント合成に任せる。
    event->ignore();
}

#ifdef Q_OS_WIN
bool GLWidget::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    // プレス&ホールド(長押し右クリック)等のペンジェスチャをこのウィンドウでは
    // 無効化する(tabletEvent()の直接駆動コメント参照。レガシー経路がまだ使われる
    // 環境向けの保険)。
    MSG *msg = static_cast<MSG *>(message);
    if (msg->message == 0x02CC /*WM_TABLET_QUERYSYSTEMGESTURESTATUS*/) {
        *result = 0x00000001 /*TABLET_DISABLE_PRESSANDHOLD*/
                | 0x00000008 /*TABLET_DISABLE_PENTAPFEEDBACK*/
                | 0x00000010 /*TABLET_DISABLE_PENBARRELFEEDBACK*/
                | 0x00010000 /*TABLET_DISABLE_FLICKS*/;
        return true;
    }
    return QOpenGLWidget::nativeEvent(eventType, message, result);
}
#endif

void GLWidget::showEvent(QShowEvent *event) {
    QOpenGLWidget::showEvent(event);
#ifdef Q_OS_WIN
    // Windows標準のペン/タッチ視覚フィードバック(タップの波紋・長押しを示す丸
    // サークル等)は、ペンタブでキャンバスへ描画する際に絵と重なって邪魔になる
    // ため、このウィジェットのネイティブウィンドウに限定して無効化する
    // (システム設定やほかのウィンドウには影響しない)。ネイティブウィンドウが
    // 実在する初回表示時に一度だけ行えばよい。
    if (!penTouchFeedbackDisabled_) {
        penTouchFeedbackDisabled_ = true;
        const FEEDBACK_TYPE kTypes[] = {
            FEEDBACK_TOUCH_CONTACTVISUALIZATION,
            FEEDBACK_PEN_BARRELVISUALIZATION,
            FEEDBACK_PEN_TAP,
            FEEDBACK_PEN_DOUBLETAP,
            FEEDBACK_PEN_PRESSANDHOLD,
            FEEDBACK_PEN_RIGHTTAP,
            FEEDBACK_TOUCH_TAP,
            FEEDBACK_TOUCH_DOUBLETAP,
            FEEDBACK_TOUCH_PRESSANDHOLD,
            FEEDBACK_TOUCH_RIGHTTAP,
            FEEDBACK_GESTURE_PRESSANDTAP,
        };
        BOOL disabled = FALSE;
        for (FEEDBACK_TYPE t : kTypes)
            SetWindowFeedbackSetting(reinterpret_cast<HWND>(winId()), t, 0, sizeof(disabled), &disabled);

        // 視覚フィードバックだけでなく、プレス&ホールド(長押し右クリック)などの
        // ペンジェスチャ判定自体もこのウィンドウでは無効化する。有効なままだと、
        // ペンを置いてもジェスチャ判定が終わるまで合成マウスダウンの発生が
        // 数百ms保留され、書き始めの遅延になる(tabletEvent()の直接駆動と
        // nativeEvent()のWM_TABLET_QUERYSYSTEMGESTURESTATUS応答と合わせて三重の保険)。
        const DWORD tabletGestureOff =
            0x00000001 /*TABLET_DISABLE_PRESSANDHOLD*/ |
            0x00000008 /*TABLET_DISABLE_PENTAPFEEDBACK*/ |
            0x00000010 /*TABLET_DISABLE_PENBARRELFEEDBACK*/ |
            0x00010000 /*TABLET_DISABLE_FLICKS*/;
        SetPropW(reinterpret_cast<HWND>(winId()), L"MicrosoftTabletPenServiceProperty",
                 reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(tabletGestureOff)));
    }
#endif
}

bool GLWidget::isInkingBusy()
{
    if (Tool *t = currentTool(); t && t->isActive()) return true;
    // ペンがホバー中(ストロークとストロークの間)も忙しい扱いにする。文字書きの
    // ように短いストロークを連打しているとき、重いUI再生成が次のペンダウンの
    // 直前・最中に走ってイベントループを塞ぐのを防ぐ。
    return lastTabletEventClock_.isValid() && lastTabletEventClock_.elapsed() < 250;
}

Tool *GLWidget::currentTool() {
    // テキストレイヤーがアクティブな間は、テキストツール以外での描画/操作を
    // 一切受け付けない(ペンでの書き込みなど、非破壊のテキストデータと矛盾する
    // 操作を防ぐため)。
    if (doc_ && doc_->layerCount() > 0 &&
        doc_->layers[doc_->activeLayerIndex()].layerType == LayerType::Text &&
        activeTool != ToolType::Text)
        return nullptr;

    switch (activeTool) {
    case ToolType::Pen:    return &penTool_;
    case ToolType::Eraser:  return &eraserTool_;
    case ToolType::Fill:    return &fillTool_;
    case ToolType::Move:    return &moveTool_;
    case ToolType::Rotate:  return &rotateTool_;
    case ToolType::Dropper: return &dropperTool_;
    case ToolType::Blur:    return &blurTool_;
    case ToolType::Warp:    return &warpTool_;
    case ToolType::Selection: return &selectTool_;
    case ToolType::Text:    return &textTool_;
    case ToolType::Airbrush: return &airbrushTool_;
    default: return nullptr;
    }
}

// ===========================================================================
// レイヤー操作
// ===========================================================================
bool GLWidget::addLayer(const QString &name, int insertIndex, bool clipping,
                         int originTx, int originTy, int tilesXOverride, int tilesYOverride,
                         LayerType layerType, const QVector<int> &ancestorFolders)
{
    // テクスチャのゼロクリアが終わるまでdoc_の自動通知を一時的に抑制する
    // (先に通知が飛ぶと、新レイヤーの中身が未初期化のまま合成されてしまうため)
    m_suppressDocNotify = true;
    bool ok = doc_->addLayer(name, insertIndex, clipping, originTx, originTy, tilesXOverride, tilesYOverride,
                              layerType, ancestorFolders);
    m_suppressDocNotify = false;

    if (!ok) return false;

    // 新規レイヤーの全タイルをゼロクリアする(単色レイヤーはタイルを持たないので何もしない)。
    // m_initializing中(initTextures()からの初期デフォルトレイヤー作成)でも必ず行う。
    // 「新規glTexStorage3Dはドライバがゼロ初期化してくれる」という前提に頼っていたが、
    // 複数タブ化により同一プロセス内でinitTextures()が複数回(タブごとに)呼ばれるようになり、
    // 直前にfreeTextures()で解放した直後の再確保では、解放前の中身が残った同一メモリが
    // 返ってくることがあり、この前提はもう成り立たない。
    // CPU側でゼロ埋めバッファを都度アップロードする(glTexSubImage3D、タイル1枚あたり
    // TILE_SIZE*TILE_SIZE*4バイトのCPU→GPU転送)のではなく、compute shader
    // (layerclear.comp)でGPU側から直接書き込む。レイヤーのタイルは常に連続した
    // スライス範囲として確保される(CanvasDocument::addLayer参照)ため、1回のディスパッチ
    // (z=タイル数)でまとめてクリアできる。
    const Layer &layer = doc_->activeLayer();
    if (!layer.tiles.isEmpty()) {
        const int base  = layer.tiles[0][0];
        const int count = layer.tilesX() * layer.tilesY();
        clearSliceRange(base, count, 0.0f, 0.0f, 0.0f, 0.0f);
    }

    if (!m_initializing) {
        update();
        emit layersChanged();
    }
    return true;
}

bool GLWidget::addSolidColorLayer(const QString &name, int insertIndex, const QColor &color,
                                   const QVector<int> &ancestorFolders)
{
    if (!addLayer(name, insertIndex, /*clipping=*/false,
                  /*originTx=*/0, /*originTy=*/0,
                  /*tilesXOverride=*/0, /*tilesYOverride=*/0,
                  LayerType::SolidColor, ancestorFolders))
        return false;
    doc_->layerRef(doc_->activeLayerIndex()).solidColor = color; // addLayer()が新規レイヤーをアクティブにする
    update();
    return true;
}

bool GLWidget::addLayerMask(int layerIndex)
{
    if (!doc_) return false;
    if (layerIndex < 0 || layerIndex >= doc_->layerCount()) return false;

    makeCurrent();
    if (!doc_->addLayerMask(layerIndex)) return false;

    // 新規マスクは白(=全面表示)でクリアする(addLayer()の透明クリアと同じ仕組み、
    // 色だけ違う)。マスクは常にキャンバス全体を覆う連続スライスブロックなので
    // レイヤー本体と同様1回のディスパッチでまとめてクリアできる。
    const Layer &layer = doc_->layers[layerIndex];
    const int base  = layer.maskTiles[0][0];
    const int count = layer.maskTilesX() * layer.maskTilesY();
    clearSliceRange(base, count, 1.0f, 1.0f, 1.0f, 1.0f);
    doc_->layers[layerIndex].maskDirty = false; // 生成直後の白マスクは未編集扱い

    if (!m_initializing) {
        update();
        emit layersChanged();
    }
    return true;
}

bool GLWidget::removeLayerMask(int layerIndex)
{
    if (!doc_) return false;
    makeCurrent();
    if (!doc_->removeLayerMask(layerIndex)) return false;
    if (!m_initializing) {
        update();
        emit layersChanged();
    }
    return true;
}

void GLWidget::setEditingMaskLayer(int layerIndex)
{
    if (!doc_) return;
    const int newVal = (editingMaskLayerIndex_ == layerIndex) ? -1 : layerIndex;

    // これまで編集していたマスクを自分で自動生成していて、一度も描かれていなければ
    // (一律の不透明度で足りるので)破棄して数値管理へ戻す。別レイヤーへ切り替える
    // ときも同様に後始末する。
    const int prev = editingMaskLayerIndex_;
    if (prev >= 0 && prev == autoCreatedMaskLayer_ && prev != newVal) {
        if (prev < doc_->layerCount() && doc_->layers[prev].hasMask
                && !doc_->layers[prev].maskDirty) {
            removeLayerMask(prev);
        }
        autoCreatedMaskLayer_ = -1;
    }

    editingMaskLayerIndex_ = newVal;

    // 編集モードに入ったのにマスクがまだ無ければ、描画先として白マスクを自動生成する
    // (「クリックで編集開始→そのまま描ける」を成立させるため)。
    if (editingMaskLayerIndex_ >= 0 && editingMaskLayerIndex_ < doc_->layerCount()
            && !doc_->layers[editingMaskLayerIndex_].hasMask) {
        if (addLayerMask(editingMaskLayerIndex_))
            autoCreatedMaskLayer_ = editingMaskLayerIndex_;
    }

    toolCtx_.editingMaskLayerIndex = editingMaskLayerIndex_;
    emit layersChanged(); // LayerDock側の不透明度プレビュー(枠ハイライト/濃淡)を更新させる
    updateCursor();
    update();
}

// ===========================================================================
// ストローク色バッファ
// ---------------------------------------------------------------------------
// スタンプごとに色が変わる設定(色のランダム、将来の混色・水彩)のときだけ使う
// キャンバスサイズのRGBA16F。使わない人にキャンバス1枚ぶんの追加メモリを
// 払わせないよう、最初に必要になった時点で確保する。
// ===========================================================================
GLuint GLWidget::ensureStrokeColorTex()
{
    if (!strokeColorSupported_) return 0;
    if (strokeColorTex) return strokeColorTex;
    if (canvasW <= 0 || canvasH <= 0) return 0;

    glGenTextures(1, &strokeColorTex);
    glBindTexture(GL_TEXTURE_2D, strokeColorTex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, canvasW, canvasH);
    // maskTexと同じ理由でGL_LINEAR(プレビューとベイク後で見た目を変えない)。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    // 中身のクリアは不要。stroke.compは「マスクが0の画素は色も0から始める」ため、
    // 前のストロークの残りを読むことがない(あちらのコメント参照)。
    WINLOG(QStringLiteral("strokeColorTex %1x%2 RGBA16F (%3MB) を確保")
               .arg(canvasW).arg(canvasH).arg(qint64(canvasW) * canvasH * 8 / (1024 * 1024)));
    return strokeColorTex;
}

// ===========================================================================
// ペン先(スタンプ)画像
// ===========================================================================
bool GLWidget::setPenTipImage(const QString &path)
{
    QImage img(path);
    if (img.isNull()) return false;

    // GLコンテキストの関数ポインタがまだ解決されていない(initializeGL()が一度も
    // 走っていない)段階でGL関数を呼ぶとクラッシュする。ToolPropDockの構築時など、
    // ウィンドウ表示前にこの関数が呼ばれることがあるため、その場合はパスの記録だけ
    // 行い、実際のアップロードはinitializeGL()側の呼び出しに任せる。
    if (!glFunctionsReady_) {
        penTipTexPath_ = path;
        return true;
    }

    img = img.convertToFormat(QImage::Format_RGBA8888);

    makeCurrent();
    if (penTipTex == 0)
        glGenTextures(1, &penTipTex);

    // キャンバステクスチャ群と違い、ペン先画像はユーザーが好きなタイミングで
    // 差し替える(サイズも変わりうる)ので、glTexStorage2Dで固定サイズ確保するのではなく
    // glTexImage2Dで都度作り直す(頻度が低いのでコストは無視できる)。
    glBindTexture(GL_TEXTURE_2D, penTipTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width(), img.height(), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // 境界外(端の1px外)を参照してもテクスチャの端の色が伸びるだけにし、
    // 反対側が回り込んで見える(スタンプの端が変になる)のを防ぐ
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    penTipTexPath_ = path;
    toolCtx_.penTipTex = penTipTex;
    return true;
}

// ===========================================================================
// 紙質テクスチャ
// ===========================================================================
bool GLWidget::setPaperTexture(const QString &path)
{
    // 空パスは「紙質なし」。テクスチャは残したまま、パスだけ空にする
    // (PenEraserTool側は設定の適用量が0かパスが空なら紙質を使わない)。
    if (path.isEmpty()) {
        paperTexPath_.clear();
        return true;
    }

    QImage img(path);
    if (img.isNull()) return false;

    // setPenTipImage()と同じ理由(GL関数ポインタ未解決の段階で呼ばれうる)。
    if (!glFunctionsReady_) {
        paperTexPath_ = path;
        return true;
    }

    img = img.convertToFormat(QImage::Format_RGBA8888);

    makeCurrent();
    if (paperTex == 0)
        glGenTextures(1, &paperTex);

    glBindTexture(GL_TEXTURE_2D, paperTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width(), img.height(), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
    // 紙の目はキャンバス全体に敷き詰めるので繰り返しで貼る(同梱テクスチャは
    // 上下左右がシームレスに繋がるように作ってある。PaperTexPresets参照)。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    // 拡大率を1.0未満にすると1キャンバスpxが複数テクセルにまたがり、そのままでは
    // モアレになる。ミップを作っておき、stroke.comp側は拡大率から求めたLODで
    // textureLod()する(コンピュートシェーダーは微分が使えず自動LODが効かない)。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    paperTexPath_      = path;
    toolCtx_.paperTex  = paperTex;
    return true;
}

void GLWidget::applyDisplayConfig()
{
    update();
}

// PNG/JPEG/BMP等の1枚の画像ファイルを、キャンバス全面を覆う1枚の通常レイヤーとして
// 取り込む。PsdCodec::load()内のaddFlattenedNormalLayer(フラット画像PSDの取り込み)
// と同じパターン(標準画像の行順=原点左上・Y下向きを、タイル格納が期待する
// 行順=原点左下・Y上向きに変換するため上下反転してから書き込む)。
bool GLWidget::loadImageAsSingleLayer(const QImage &image, const QString &layerName)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) return false;

    makeCurrent();
    recreateCanvas(image.width(), image.height(), /*createDefaultLayers=*/false);

    beginBulkLayerImport();

    addLayer(layerName, -1, /*clipping=*/false, 0, 0, -1, -1, LayerType::Normal);
    const int idx = doc_->layerCount() - 1;

    const QImage img = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied).mirrored(false, true);

    const Layer &layer = doc_->layerRef(idx);
    for (int ty = 0; ty < layer.tilesY(); ty++) {
        for (int tx = 0; tx < layer.tilesX(); tx++) {
            const int tilePxX = tx * TILE_SIZE;
            const int tilePxY = ty * TILE_SIZE;
            QByteArray buf(TILE_SIZE * TILE_SIZE * 4, char(0));
            const int ox0 = tilePxX, oy0 = tilePxY;
            const int ox1 = qMin(tilePxX + TILE_SIZE, img.width());
            const int oy1 = qMin(tilePxY + TILE_SIZE, img.height());
            if (ox0 < ox1 && oy0 < oy1) {
                for (int y = oy0; y < oy1; y++) {
                    const uchar *src = img.constScanLine(y) + ox0 * 4;
                    char *dst = buf.data() + ((y - tilePxY) * TILE_SIZE + (ox0 - tilePxX)) * 4;
                    memcpy(dst, src, (ox1 - ox0) * 4);
                }
            }
            writeSlicePixels(layer.tiles[ty][tx], buf);
        }
    }

    endBulkLayerImport();
    return true;
}

// 「画像を追加」: 既存ドキュメントへ、現在のアクティブレイヤーの直上に画像を新規
// 通常レイヤーとして挿入する。キャンバス中央に配置し、画像がキャンバスよりはみ出す
// 場合はレイヤーの矩形がその分キャンバス外へ広がる(タイル座標は負値・キャンバス
// 範囲外も許容される、Layer::originTx/originTyの既存の仕組みをそのまま使う)。
bool GLWidget::insertImageLayerAboveActive(const QImage &image, const QString &layerName)
{
    if (!doc_ || doc_->layers.isEmpty()) return false;
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) return false;

    makeCurrent();

    const int imgW = image.width(), imgH = image.height();
    // left/tiepoloYMinは、addFlattenedNormalLayer(PsdCodec.cpp)のleft/topPsdから
    // 導出したtiepoloYMinと同じ意味(タイル格納の行順=原点左下基準でのオフセット)。
    const int left = (canvasW - imgW) / 2;
    const int tiepoloYMin = (canvasH - imgH) / 2;

    const int minTx   = qFloor((double)left / TILE_SIZE);
    const int minTy   = qFloor((double)tiepoloYMin / TILE_SIZE);
    const int maxTxEx = qCeil((double)(left + imgW) / TILE_SIZE);
    const int maxTyEx = qCeil((double)(tiepoloYMin + imgH) / TILE_SIZE);
    const int tilesX  = qMax(1, maxTxEx - minTx);
    const int tilesY  = qMax(1, maxTyEx - minTy);
    const int insertIndex = doc_->activeLayerIndex() + 1;

    // 増えるのはこの1枚だけなので、その中身だけをUndoに持つ
    // (UndoKind::LayerAdd + hasContent。以前は前後2回 captureAllLayerSnapshots() で
    //  全レイヤーを読み戻していて、レイヤーが多いほど遅かった)。
    // ファイルを開く経路(loadImageAsSingleLayer等)も同じbeginBulkLayerImportを
    // 使うが、あちらは新規ドキュメントの構築なのでUndo対象にしない。
    beginLayerAddUndo();

    beginBulkLayerImport();

    if (!addLayer(layerName, insertIndex, /*clipping=*/false, minTx, minTy, tilesX, tilesY, LayerType::Normal)) {
        endBulkLayerImport();
        abortLayerAddUndo();
        return false;
    }
    doc_->setActiveLayer(insertIndex);

    // 標準的な画像ファイル(原点左上、Y下向き)を、タイル格納が期待する行順
    // (原点左下、Y上向き)に変換するため上下反転する。
    const QImage img = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied).mirrored(false, true);

    const Layer &layer = doc_->layerRef(insertIndex);
    for (int ty = 0; ty < layer.tilesY(); ty++) {
        for (int tx = 0; tx < layer.tilesX(); tx++) {
            const int tilePxX = (layer.originTx + tx) * TILE_SIZE;
            const int tilePxY = (layer.originTy + ty) * TILE_SIZE;
            QByteArray buf(TILE_SIZE * TILE_SIZE * 4, char(0));
            const int ox0 = qMax(tilePxX, left), oy0 = qMax(tilePxY, tiepoloYMin);
            const int ox1 = qMin(tilePxX + TILE_SIZE, left + imgW), oy1 = qMin(tilePxY + TILE_SIZE, tiepoloYMin + imgH);
            if (ox0 < ox1 && oy0 < oy1) {
                for (int y = oy0; y < oy1; y++) {
                    const uchar *src = img.constScanLine(y - tiepoloYMin) + (ox0 - left) * 4;
                    char *dst = buf.data() + ((y - tilePxY) * TILE_SIZE + (ox0 - tilePxX)) * 4;
                    memcpy(dst, src, (ox1 - ox0) * 4);
                }
            }
            writeSlicePixels(layer.tiles[ty][tx], buf);
        }
    }

    // endBulkLayerImport()のglFinish()より後にcommitする(書き込みがGPU側で
    // 完了する前に読み戻すと、途中状態が焼き付いてしまう)。
    endBulkLayerImport();
    commitLayerAddUndo(insertIndex, /*ancestorFolders=*/{}, /*duplicateSourceIndex=*/-1,
                       /*captureContent=*/true);
    return true;
}

void GLWidget::beginBulkLayerImport()
{
    m_initializing = true;
}

void GLWidget::endBulkLayerImport()
{
    m_initializing = false;

    // 大量のglTexSubImage3D/writeSlicePixels書き込みがGPU側でまだ実行中のうちに
    // 次のフレームのpaintGL()がlayerTexArrayを読みに行くと、タイミング次第で
    // ドライバ側の状態が不整合になりうる(rebuildCanvasFromSnapshotsと同様の理由)。
    makeCurrent();
    glFinish();

    emit layersChanged();
    update();
}

// ---------------------------------------------------------------------------
// レイヤー追加のUndo (UndoKind::LayerAdd)
// ---------------------------------------------------------------------------
// 追加直後のレイヤーは必ず中身が空なので、削除/複製/結合が使う全レイヤー
// スナップショット方式(pushLayerStructureUndo)ではなく、作り直しに要る
// パラメータだけを持つ軽量エントリを積む(LayerAddUndoDataのコメント参照)。
//
// addLayer()の中ではなく呼び出し側で挟む形にしてあるのは、種類ごとの後追い設定
// (単色レイヤーの色、調整レイヤーの種類、アクティブレイヤーの変更)がaddLayer()の
// 後に行われるため。それらが済んでからcommitしないと、Redoで設定が失われる。
void GLWidget::beginLayerAddUndo()
{
    if (!doc_) return;
    pendingLayerAddActiveBefore_ = doc_->activeLayerIndex();
}

void GLWidget::commitLayerAddUndo(int insertedIndex, const QVector<int> &ancestorFolders,
                                   int duplicateSourceIndex, bool captureContent)
{
    if (!doc_ || pendingLayerAddActiveBefore_ < 0) return;
    const int activeBefore = pendingLayerAddActiveBefore_;
    pendingLayerAddActiveBefore_ = -1;
    if (insertedIndex < 0 || insertedIndex >= doc_->layerCount()) return;

    const Layer &ly = doc_->layerAt(insertedIndex);

    UndoEntry entry;
    entry.kind = UndoKind::LayerAdd;
    LayerAddUndoData &d = entry.layerAdd;
    d.insertIndex     = insertedIndex;
    d.ancestorFolders = ancestorFolders;
    d.activeBefore    = activeBefore;
    d.duplicateSourceIndex = duplicateSourceIndex;
    d.name       = ly.name;
    d.layerType  = ly.layerType;
    d.clipping   = ly.clipping;
    d.originTx   = ly.originTx;
    d.originTy   = ly.originTy;
    d.opacity    = ly.opacity;
    d.visible    = ly.visible;
    d.blendMode  = ly.blendMode;
    // tilesX()/tilesY()をそのままoverrideとして渡し直す。タイルを持たない種類
    // (単色/調整/フォルダー)は0なので、CanvasDocument::addLayer側で同じく0タイルになる。
    d.tilesX     = ly.tilesX();
    d.tilesY     = ly.tilesY();
    d.solidColor = ly.solidColor;
    d.adjustment = ly.adjustment;
    d.filter     = ly.filter;

    // 画像インポートのように中身のあるレイヤーが増える場合は、その1枚ぶんだけ
    // 中身も持っておく(Redoで作り直すため)。呼び出し側はピクセルを書き終えてから
    // commitすること。
    if (captureContent) {
        makeCurrent();
        d.content    = captureRemovedLayer(insertedIndex);
        d.hasContent = true;
    }

    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());
}

void GLWidget::abortLayerAddUndo()
{
    pendingLayerAddActiveBefore_ = -1;
}

// toBefore=true : 追加を取り消す(そのレイヤーを削除する)
// toBefore=false: 追加をやり直す(同じ設定で作り直す)
void GLWidget::applyLayerAddUndoEntry(const UndoEntry &entry, bool toBefore)
{
    if (!doc_) return;
    const LayerAddUndoData &d = entry.layerAdd;
    makeCurrent();

    if (toBefore) {
        if (d.insertIndex < 0 || d.insertIndex >= doc_->layerCount()) return;
        // GLWidget::removeLayer(公開版)ではなくdoc_を直接触る。公開版は自分で
        // 構造Undoを積んでしまうため、Undo適用中に呼ぶと履歴が壊れる。
        doc_->removeLayer(d.insertIndex, d.ancestorFolders, /*includeContents=*/true);
        doc_->setActiveLayer(qBound(0, d.activeBefore, doc_->layerCount() - 1));
    } else {
        if (d.duplicateSourceIndex >= 0) {
            // 複製のRedo: ピクセルは複製元から作り直せるので保存していない。
            // Undo/Redoは線形なので、この時点の複製元の中身は複製した当時と一致する。
            if (duplicateLayer(d.duplicateSourceIndex, d.insertIndex, d.ancestorFolders) < 0)
                return;
        } else if (d.hasContent) {
            // 画像インポートのRedo: 保存しておいた中身ごと作り直す
            if (!restoreRemovedLayer(d.content, d.insertIndex, d.ancestorFolders))
                return;
        } else if (!addLayer(d.name, d.insertIndex, d.clipping, d.originTx, d.originTy,
                             d.tilesX, d.tilesY, d.layerType, d.ancestorFolders)) {
            return;
        }
        // 作成直後の既定値ではなく、記録しておいた設定へ戻す(複製の場合、呼び出し側が
        // 複製後にclippingを立てる等の後追い変更をしていることがあるため)。
        Layer &ly = doc_->layerRef(d.insertIndex);
        ly.name       = d.name;
        ly.clipping   = d.clipping;
        ly.opacity    = d.opacity;
        ly.visible    = d.visible;
        ly.blendMode  = d.blendMode;
        ly.solidColor = d.solidColor;
        ly.adjustment = d.adjustment;
        ly.filter     = d.filter;
        doc_->setActiveLayer(d.insertIndex);
    }

    update();
    emit layersChanged();
}

// ---------------------------------------------------------------------------
// レイヤー削除のUndo (UndoKind::LayerRemove)
// ---------------------------------------------------------------------------
// 1枚ぶんの中身をタイルの生データごと読み出す。QImage経由のLayerSnapshotDataと違い、
// キャンバス外へはみ出した領域(Layer::originTx等)もそのまま保持できる。
RemovedLayerData GLWidget::captureRemovedLayer(int layerIndex)
{
    RemovedLayerData d;
    if (!doc_ || layerIndex < 0 || layerIndex >= doc_->layerCount()) return d;

    const Layer &ly = doc_->layerAt(layerIndex);
    d.name       = ly.name;
    d.layerType  = ly.layerType;
    d.clipping   = ly.clipping;
    d.visible    = ly.visible;
    d.opacity    = ly.opacity;
    d.blendMode  = ly.blendMode;
    d.originTx   = ly.originTx;
    d.originTy   = ly.originTy;
    d.tilesX     = ly.tilesX();
    d.tilesY     = ly.tilesY();
    d.childCount = ly.childCount;
    d.solidColor = ly.solidColor;
    d.adjustment = ly.adjustment;
    d.filter     = ly.filter;
    d.textBoxes  = ly.textBoxes;
    d.hasMask    = ly.hasMask;

    d.tiles.reserve(d.tilesX * d.tilesY);
    for (int ty = 0; ty < d.tilesY; ty++)
        for (int tx = 0; tx < d.tilesX; tx++)
            d.tiles.append(readSlicePixels(ly.tiles[ty][tx]));

    if (d.hasMask) {
        d.maskTiles.reserve(ly.maskTilesX() * ly.maskTilesY());
        for (int ty = 0; ty < ly.maskTilesY(); ty++)
            for (int tx = 0; tx < ly.maskTilesX(); tx++)
                d.maskTiles.append(readSlicePixels(ly.maskTiles[ty][tx]));
    }
    return d;
}

// captureRemovedLayer()で保存した内容から、insertIndexの位置へレイヤーを作り直す。
bool GLWidget::restoreRemovedLayer(const RemovedLayerData &d, int insertIndex,
                                    const QVector<int> &ancestorFolders)
{
    if (!doc_) return false;

    // 元の矩形(キャンバス外へのはみ出しを含む)をそのまま再現する
    if (!addLayer(d.name, insertIndex, d.clipping, d.originTx, d.originTy,
                  d.tilesX, d.tilesY, d.layerType, ancestorFolders))
        return false;

    Layer &ly = doc_->layerRef(insertIndex);
    ly.visible    = d.visible;
    ly.opacity    = d.opacity;
    ly.blendMode  = d.blendMode;
    ly.solidColor = d.solidColor;
    ly.adjustment = d.adjustment;
    ly.textBoxes  = d.textBoxes;
    // フォルダーのchildCountは、中身を1枚ずつ復元する過程でaddLayer()が
    // ancestorFoldersから増やすのではなく、保存しておいた値をそのまま入れる
    // (中身の復元時に渡すancestorFoldersは「削除時に渡された外側の階層」だけなので)。
    ly.childCount = d.childCount;

    for (int ty = 0, i = 0; ty < d.tilesY; ty++)
        for (int tx = 0; tx < d.tilesX; tx++, i++)
            if (i < d.tiles.size()) writeSlicePixels(doc_->layerAt(insertIndex).tiles[ty][tx], d.tiles[i]);

    if (d.hasMask && addLayerMask(insertIndex)) {
        const Layer &withMask = doc_->layerAt(insertIndex);
        for (int ty = 0, i = 0; ty < withMask.maskTilesY(); ty++)
            for (int tx = 0; tx < withMask.maskTilesX(); tx++, i++)
                if (i < d.maskTiles.size()) writeSlicePixels(withMask.maskTiles[ty][tx], d.maskTiles[i]);
    }
    return true;
}

// toBefore=true : 削除を取り消す(保存しておいたレイヤーを挿入し直す)
// toBefore=false: 削除をやり直す(同じ引数でもう一度削除する)
void GLWidget::applyLayerRemoveUndoEntry(const UndoEntry &entry, bool toBefore)
{
    if (!doc_) return;
    const LayerRemoveUndoData &d = entry.layerRemove;
    makeCurrent();

    if (toBefore) {
        m_suppressDocNotify = true;
        for (int i = 0; i < d.layers.size(); i++)
            restoreRemovedLayer(d.layers[i], d.insertIndex + i, d.ancestorFolders);
        m_suppressDocNotify = false;
        doc_->setActiveLayer(qBound(0, d.activeBefore, doc_->layerCount() - 1));
    } else {
        doc_->removeLayer(d.insertIndex, d.ancestorFolders, d.includeContents);
    }

    glFinish(); // 大量のwriteSlicePixels後にpaintGLが読みに行くのを避ける(endBulkLayerImportと同じ理由)
    update();
    emit layersChanged();
}

bool GLWidget::removeLayer(int layerIndex, const QVector<int> &ancestorFolders, bool includeContents) {
    if (!doc_) return false;
    if (layerIndex < 0 || layerIndex >= doc_->layerCount()) return false;
    if (doc_->layerCount() <= 1) return false; // 最後の1枚は削除できない(doc_->removeLayer内でも拒否されるが、Undo記録前に弾く)

    makeCurrent();

    // 消える範囲だけを保存する(CanvasDocument::removeLayerと同じ範囲計算)。
    // 以前は前後2回 captureAllLayerSnapshots() していたが、復元に要るのは
    // 消えるレイヤーだけなので、無関係なレイヤーまで読み戻す必要は無い。
    const bool cascade = includeContents
                       && doc_->layerAt(layerIndex).layerType == LayerType::Folder;
    const int count = cascade ? 1 + doc_->layerAt(layerIndex).childCount : 1;
    if (doc_->layerCount() <= count) return false;

    LayerRemoveUndoData d;
    d.insertIndex     = layerIndex;
    d.ancestorFolders = ancestorFolders;
    d.includeContents = includeContents;
    d.activeBefore    = doc_->activeLayerIndex();
    d.layers.reserve(count);
    for (int i = 0; i < count; i++)
        d.layers.append(captureRemovedLayer(layerIndex + i));

    // doc_->removeLayer 内でコールバック経由 freeSlice が呼ばれ、
    // 通知(CacheAndNotify)も内部で発行される。フォルダーを丸ごと消して
    // ドキュメントが空になってしまう等の理由で失敗した場合は何もしない。
    if (!doc_->removeLayer(layerIndex, ancestorFolders, includeContents)) return false;

    UndoEntry entry;
    entry.kind        = UndoKind::LayerRemove;
    entry.layerRemove = std::move(d);
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());
    return true;
}

int GLWidget::mergeLayers(int survivorIndex, int victimIndex, const QVector<int> &ancestorFolders)
{
    LayerMergeUndoData d;
    const int result = mergeLayersInternal(survivorIndex, victimIndex, ancestorFolders, &d);
    if (result < 0) return -1;

    UndoEntry entry;
    entry.kind       = UndoKind::LayerMerge;
    entry.layerMerge = std::move(d);
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());
    return result;
}

// toBefore=true : 結合を取り消す(survivorのピクセル/不透明度を戻し、victimを挿入し直す)
// toBefore=false: 結合をやり直す(同じ引数でもう一度結合する)
void GLWidget::applyLayerMergeUndoEntry(const UndoEntry &entry, bool toBefore)
{
    if (!doc_) return;
    const LayerMergeUndoData &d = entry.layerMerge;
    makeCurrent();

    if (toBefore) {
        // victimを挿入し直す前のsurvivorの位置(mergeLayersInternalの戻り値と同じ計算)
        const int survivorNow = (d.victimIndex < d.survivorIndex) ? d.survivorIndex - 1 : d.survivorIndex;
        if (survivorNow < 0 || survivorNow >= doc_->layerCount()) return;

        m_suppressDocNotify = true;
        const Layer &sv = doc_->layerAt(survivorNow);
        for (int ty = 0, i = 0; ty < d.survivorTilesY; ty++)
            for (int tx = 0; tx < d.survivorTilesX; tx++, i++)
                if (ty < sv.tilesY() && tx < sv.tilesX() && i < d.survivorTiles.size())
                    writeSlicePixels(sv.tiles[ty][tx], d.survivorTiles[i]);
        doc_->layerRef(survivorNow).opacity = d.survivorOpacityBefore;

        restoreRemovedLayer(d.victim, d.victimIndex, d.ancestorFolders);
        m_suppressDocNotify = false;
        doc_->setActiveLayer(qBound(0, d.activeBefore, doc_->layerCount() - 1));
    } else {
        mergeLayersInternal(d.survivorIndex, d.victimIndex, d.ancestorFolders, nullptr);
    }

    glFinish(); // 大量のwriteSlicePixels後にpaintGLが読みに行くのを避ける
    update();
    emit layersChanged();
}

// 結合の本体。undoOut != nullptr のとき、Undoに必要な2枚(矩形拡張後・ピクセル結合前の
// survivor / 消える直前のvictim)だけをそこへ書き出す。
// 以前はここで前後2回 captureAllLayerSnapshots() をしていたが、全レイヤーを非圧縮で
// GPU→CPUへ読み戻すため、レイヤーが多いドキュメントでは非常に遅かった。
int GLWidget::mergeLayersInternal(int survivorIndex, int victimIndex,
                                   const QVector<int> &ancestorFolders,
                                   LayerMergeUndoData *undoOut)
{
    if (!doc_) return -1;
    if (survivorIndex < 0 || survivorIndex >= doc_->layerCount()) return -1;
    if (victimIndex   < 0 || victimIndex   >= doc_->layerCount()) return -1;
    if (survivorIndex == victimIndex) return -1;

    makeCurrent();

    // victim削除で参照が無効化されうるので、必要な値は先にコピーしておく
    const Layer survivorInfo0 = doc_->layerAt(survivorIndex);
    const Layer victimInfo    = doc_->layerAt(victimIndex);

    // 単色レイヤーは実ピクセルデータを持たないため、通常の結合(ピクセル読み書き)は
    // 対応できない(データが失われる)。どちらかが単色レイヤーなら結合しない。
    if (survivorInfo0.layerType != LayerType::Normal || victimInfo.layerType != LayerType::Normal)
        return -1;

    if (undoOut) {
        undoOut->survivorIndex          = survivorIndex;
        undoOut->victimIndex            = victimIndex;
        undoOut->ancestorFolders        = ancestorFolders;
        undoOut->activeBefore           = doc_->activeLayerIndex();
        undoOut->survivorOpacityBefore  = survivorInfo0.opacity;
        undoOut->victim                 = captureRemovedLayer(victimIndex);
    }

    // survivor/victimどちらかがキャンバスからはみ出している(またはsurvivorが
    // victimより小さい)場合、キャンバスタイル範囲だけを回すとvictimのはみ出し
    // ピクセルが黙って失われる。結合前にsurvivorの矩形をsurvivor+victimの
    // 和集合まで広げておく(新規タイルは透明クリアされる)。
    const int unionMinTx   = qMin(survivorInfo0.originTx, victimInfo.originTx);
    const int unionMinTy   = qMin(survivorInfo0.originTy, victimInfo.originTy);
    const int unionMaxTxEx = qMax(survivorInfo0.originTx + survivorInfo0.tilesX(),
                                   victimInfo.originTx + victimInfo.tilesX());
    const int unionMaxTyEx = qMax(survivorInfo0.originTy + survivorInfo0.tilesY(),
                                   victimInfo.originTy + victimInfo.tilesY());
    if (unionMaxTxEx > unionMinTx && unionMaxTyEx > unionMinTy)
        growLayerBoundsToCoverCanvasTiles(survivorIndex, unionMinTx, unionMinTy, unionMaxTxEx, unionMaxTyEx);

    const Layer survivorInfo = doc_->layerAt(survivorIndex); // 拡張後の最新の矩形/スライスを取り直す

    // 拡張が終わった(=矩形が確定した)この時点のsurvivorを保存する。Undoでは
    // これを書き戻すだけで、矩形を縮めなくても結合前と同じ見た目に戻せる。
    if (undoOut) {
        undoOut->survivorTilesX = survivorInfo.tilesX();
        undoOut->survivorTilesY = survivorInfo.tilesY();
        undoOut->survivorTiles.reserve(survivorInfo.tilesX() * survivorInfo.tilesY());
        for (int ty = 0; ty < survivorInfo.tilesY(); ty++)
            for (int tx = 0; tx < survivorInfo.tilesX(); tx++)
                undoOut->survivorTiles.append(readSlicePixels(survivorInfo.tiles[ty][tx]));
    }

    // 通知は最後(removeLayer)でまとめて1回だけ発行させる
    m_suppressDocNotify = true;
    for (int ty = unionMinTy; ty < unionMaxTyEx; ty++) {
        for (int tx = unionMinTx; tx < unionMaxTxEx; tx++) {
            int survivorSlice = survivorInfo.tileSliceAtCanvasTile(tx, ty);
            int victimSlice   = victimInfo.tileSliceAtCanvasTile(tx, ty);
            if (survivorSlice < 0 || victimSlice < 0) continue; // victim側にデータが無いタイルはスキップ

            QByteArray bgRaw = readSlicePixels(survivorSlice);
            QByteArray fgRaw = readSlicePixels(victimSlice);
            QByteArray merged = mergeTilePixelsCpu(bgRaw, fgRaw,
                survivorInfo.opacity, victimInfo.opacity,
                victimInfo.clipping, victimInfo.blendMode,
                tx * TILE_SIZE, ty * TILE_SIZE);
            writeSlicePixels(survivorSlice, merged);
        }
    }

    // 不透明度を焼き込んだので1.0にリセットする(通知は抑制済みなので直接書き換え)
    doc_->layerRef(survivorIndex).opacity = 1.0f;
    m_suppressDocNotify = false;

    int adjustedSurvivor = (victimIndex < survivorIndex) ? survivorIndex - 1 : survivorIndex;
    doc_->removeLayer(victimIndex, ancestorFolders); // GPUスライス解放 + 通知(CacheAndNotify)
    doc_->setActiveLayer(adjustedSurvivor);

    return adjustedSurvivor;
}

int GLWidget::duplicateLayer(int sourceIndex, int insertIndex, const QVector<int> &ancestorFolders)
{
    if (!doc_) return -1;
    if (sourceIndex < 0 || sourceIndex >= doc_->layerCount()) return -1;

    // Undoは呼び出し側が beginLayerAddUndo()/commitLayerAddUndo(..., sourceIndex) で挟む
    // (UndoKind::LayerAdd の複製版)。以前はここで前後2回 captureAllLayerSnapshots() を
    // していたが、全レイヤーを非圧縮でGPU→CPUへ読み戻すため、フォルダーの複製
    // (中身1枚ごとにこの関数を呼ぶ)では枚数×2回ぶんかかって非常に遅かった。
    // 複製されたレイヤーの中身は複製元のコピーなので、Undo用に保存する必要は無い。
    makeCurrent();

    const Layer src = doc_->layerAt(sourceIndex); // 値コピー(以降のaddLayerで参照が動いても安全)

    // addLayer内部の通知("空のタイル"の状態)は抑制し、複製し終えてから1回だけ通知する
    // 複製元の矩形(キャンバスより大きい/はみ出している場合を含む)をそのまま引き継ぐ。
    // フォルダーの場合、中身(childCount枚)は複製されない(空のフォルダーとして
    // 複製される。addLayerのtilesX()/tilesY()は0のままなので通常のaddLayerで足りる)。
    m_suppressDocNotify = true;
    bool ok = doc_->addLayer(src.name + " コピー", insertIndex, src.clipping,
                              src.originTx, src.originTy, src.tilesX(), src.tilesY(),
                              src.layerType, ancestorFolders);
    if (!ok) {
        m_suppressDocNotify = false;
        return -1;
    }
    int newIndex = doc_->activeLayerIndex(); // addLayerが新規レイヤーをアクティブにする

    const Layer newLayer = doc_->layerAt(newIndex);
    // src/newLayerは同じ形(タイル数・原点)で確保されているので、ローカル座標を
    // そのまま1対1で対応させられる
    for (int ty = 0; ty < src.tilesY(); ty++) {
        for (int tx = 0; tx < src.tilesX(); tx++) {
            int srcSlice = src.tiles[ty][tx];
            int dstSlice = newLayer.tiles[ty][tx];
            glCopyImageSubData(bankTexOf(srcSlice), GL_TEXTURE_2D_ARRAY, 0, 0, 0, localSliceOf(srcSlice),
                                bankTexOf(dstSlice), GL_TEXTURE_2D_ARRAY, 0, 0, 0, localSliceOf(dstSlice),
                                TILE_SIZE, TILE_SIZE, 1);
        }
    }

    doc_->layerRef(newIndex).opacity = src.opacity; // 通知抑制中なので直接書き換え
    doc_->layerRef(newIndex).adjustment = src.adjustment; // 調整レイヤー以外では無意味だが無害
    doc_->layerRef(newIndex).filter     = src.filter;     // 同上(フィルターレイヤー用)
    doc_->layerRef(newIndex).textBoxes = src.textBoxes; // テキストレイヤー以外では無意味だが無害
    doc_->layerRef(newIndex).solidColor = src.solidColor; // 単色レイヤー以外では無意味だが無害

    // マスクを持っていれば、複製先にも新しいタイルを確保して中身をそのままコピーする
    // (マスクは常にキャンバス全体を覆う固定形なので、srcとnewLayerで1対1に対応する)。
    if (src.hasMask && doc_->addLayerMask(newIndex)) {
        const Layer &newLayerWithMask = doc_->layerAt(newIndex);
        for (int ty = 0; ty < src.maskTilesY(); ty++) {
            for (int tx = 0; tx < src.maskTilesX(); tx++) {
                int srcSlice = src.maskTiles[ty][tx];
                int dstSlice = newLayerWithMask.maskTiles[ty][tx];
                glCopyImageSubData(bankTexOf(srcSlice), GL_TEXTURE_2D_ARRAY, 0, 0, 0, localSliceOf(srcSlice),
                                    bankTexOf(dstSlice), GL_TEXTURE_2D_ARRAY, 0, 0, 0, localSliceOf(dstSlice),
                                    TILE_SIZE, TILE_SIZE, 1);
            }
        }
    }
    m_suppressDocNotify = false;

    update();
    emit layersChanged();
    return newIndex;
}

void GLWidget::cutSelection()
{
    if (!doc_ || doc_->layers.isEmpty()) return;
    const int layerIndex = doc_->activeLayerIndex();
    const Layer &layer0 = doc_->layerRef(layerIndex);
    if (layer0.layerType != LayerType::Normal && layer0.layerType != LayerType::Text) return;

    // まずクリップボードへコピーする(選択範囲があればその範囲、無ければレイヤー全体)。
    copySelection();

    makeCurrent();

    int rectX, rectY, rectW, rectH;
    QVector<uint8_t> maskBuf; // hasSelection_のときだけ使う(キャンバスサイズ)
    const bool selActive = hasSelection_;

    if (selActive) {
        maskBuf.resize(canvasW * canvasH);
        glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, maskBuf.data());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, 0);

        int minX = canvasW, maxX = -1, minY = canvasH, maxY = -1;
        for (int y = 0; y < canvasH; y++) {
            for (int x = 0; x < canvasW; x++) {
                if (maskBuf[y * canvasW + x] == 0) continue;
                minX = qMin(minX, x); maxX = qMax(maxX, x);
                minY = qMin(minY, y); maxY = qMax(maxY, y);
            }
        }
        if (maxX < minX) return; // 選択範囲が空
        rectX = minX; rectY = minY; rectW = maxX - minX + 1; rectH = maxY - minY + 1;
    } else {
        rectX = layer0.originTx * TILE_SIZE;
        rectY = layer0.originTy * TILE_SIZE;
        rectW = layer0.tilesX() * TILE_SIZE;
        rectH = layer0.tilesY() * TILE_SIZE;
    }
    if (rectW <= 0 || rectH <= 0) return;

    const int minTx   = qFloor((double)rectX / TILE_SIZE);
    const int minTy   = qFloor((double)rectY / TILE_SIZE);
    const int maxTxEx = qCeil((double)(rectX + rectW) / TILE_SIZE);
    const int maxTyEx = qCeil((double)(rectY + rectH) / TILE_SIZE);

    const Layer &layer = doc_->layerRef(layerIndex);

    beginStrokeUndo();
    const int touchTxMin = qMax(minTx, layer.originTx);
    const int touchTyMin = qMax(minTy, layer.originTy);
    const int touchTxMax = qMin(maxTxEx - 1, layer.originTx + layer.tilesX() - 1);
    const int touchTyMax = qMin(maxTyEx - 1, layer.originTy + layer.tilesY() - 1);
    expandStrokeUndoRegion(touchTxMin, touchTxMax, touchTyMin, touchTyMax);

    for (int ty = 0; ty < layer.tilesY(); ty++) {
        for (int tx = 0; tx < layer.tilesX(); tx++) {
            const int canvasTx = layer.originTx + tx;
            const int canvasTy = layer.originTy + ty;
            const int tilePxX  = canvasTx * TILE_SIZE;
            const int tilePxY  = canvasTy * TILE_SIZE;
            const int ox0 = qMax(tilePxX, rectX), oy0 = qMax(tilePxY, rectY);
            const int ox1 = qMin(tilePxX + TILE_SIZE, rectX + rectW);
            const int oy1 = qMin(tilePxY + TILE_SIZE, rectY + rectH);
            if (ox0 >= ox1 || oy0 >= oy1) continue;

            const int slice = layer.tiles[ty][tx];
            QByteArray buf = readSlicePixels(slice);
            for (int y = oy0; y < oy1; y++) {
                uchar *dstRow = reinterpret_cast<uchar*>(buf.data()) + ((y - tilePxY) * TILE_SIZE + (ox0 - tilePxX)) * 4;
                if (!selActive) {
                    memset(dstRow, 0, (ox1 - ox0) * 4);
                    continue;
                }
                const uint8_t *maskRow = maskBuf.constData() + (size_t)y * canvasW + ox0;
                for (int x = 0; x < ox1 - ox0; x++) {
                    if (maskRow[x] != 0) memset(dstRow + x * 4, 0, 4);
                }
            }
            writeSlicePixels(slice, buf);
        }
    }

    commitStrokeUndo();
    updateCompositedTex();
    emit layersChanged();
    update();
}

void GLWidget::nudgeActiveContent(int dx, int dy)
{
    if (dx == 0 && dy == 0) return;
    const QVector2D delta((float)dx, (float)dy);
    if (actions_.nudgeActive(delta)) {
        update();
    } else {
        nudgeActiveLayer(dx, dy);
    }
}

// レイヤーの実ピクセル内容全体(キャンバス外にはみ出た部分も含む)を(dx,dy)だけ
// 平行移動する。captureAllLayerSnapshotsと違い、はみ出た部分も含めて読み出すため、
// cutSelection()の「選択なし」時のレイヤー全読み出しと同じlayer.originTx/originTy
// 基準のループを使う。移動後にレイヤー矩形がキャンバス範囲外へ広がる分は
// growLayerBoundsToCoverCanvasTilesで確保し(既存矩形との和集合)、レイヤーの
// 全タイルを移動後の内容で描き直す(=元の位置の内容は自動的に消える)。
void GLWidget::nudgeActiveLayer(int dx, int dy)
{
    if (!doc_ || doc_->layers.isEmpty()) return;
    const int layerIndex = doc_->activeLayerIndex();
    const Layer &layer0 = doc_->layerRef(layerIndex);
    if (layer0.layerType != LayerType::Normal && layer0.layerType != LayerType::Text) return;

    makeCurrent();

    const int srcX = layer0.originTx * TILE_SIZE;
    const int srcY = layer0.originTy * TILE_SIZE;
    const int srcW = layer0.tilesX() * TILE_SIZE;
    const int srcH = layer0.tilesY() * TILE_SIZE;
    if (srcW <= 0 || srcH <= 0) return;

    QImage img(srcW, srcH, QImage::Format_RGBA8888_Premultiplied);
    for (int ty = 0; ty < layer0.tilesY(); ty++) {
        for (int tx = 0; tx < layer0.tilesX(); tx++) {
            const QByteArray raw = readSlicePixels(layer0.tiles[ty][tx]);
            for (int y = 0; y < TILE_SIZE; y++) {
                uchar *dstRow = img.scanLine(ty * TILE_SIZE + y) + tx * TILE_SIZE * 4;
                const uchar *srcRow = reinterpret_cast<const uchar*>(raw.constData()) + y * TILE_SIZE * 4;
                memcpy(dstRow, srcRow, TILE_SIZE * 4);
            }
        }
    }

    const int dstX = srcX + dx;
    const int dstY = srcY + dy;

    const int minTx   = qFloor((double)dstX / TILE_SIZE);
    const int minTy   = qFloor((double)dstY / TILE_SIZE);
    const int maxTxEx = qCeil((double)(dstX + srcW) / TILE_SIZE);
    const int maxTyEx = qCeil((double)(dstY + srcH) / TILE_SIZE);
    growLayerBoundsToCoverCanvasTiles(layerIndex, minTx, minTy, maxTxEx, maxTyEx);

    const Layer &layer = doc_->layerRef(layerIndex);

    beginStrokeUndo();
    expandStrokeUndoRegion(layer.originTx, layer.originTx + layer.tilesX() - 1,
                            layer.originTy, layer.originTy + layer.tilesY() - 1);

    static const QByteArray zeroTile(TILE_SIZE * TILE_SIZE * 4, 0);
    for (int ty = 0; ty < layer.tilesY(); ty++) {
        for (int tx = 0; tx < layer.tilesX(); tx++) {
            const int canvasTx = layer.originTx + tx;
            const int canvasTy = layer.originTy + ty;
            const int tilePxX  = canvasTx * TILE_SIZE;
            const int tilePxY  = canvasTy * TILE_SIZE;
            const int slice    = layer.tiles[ty][tx];

            const int ox0 = qMax(tilePxX, dstX), oy0 = qMax(tilePxY, dstY);
            const int ox1 = qMin(tilePxX + TILE_SIZE, dstX + srcW);
            const int oy1 = qMin(tilePxY + TILE_SIZE, dstY + srcH);
            if (ox0 >= ox1 || oy0 >= oy1) { writeSlicePixels(slice, zeroTile); continue; }

            QByteArray buf = zeroTile;
            for (int y = oy0; y < oy1; y++) {
                const uchar *srcRow = img.constScanLine(y - dstY) + (ox0 - dstX) * 4;
                uchar *dstRow = reinterpret_cast<uchar*>(buf.data()) + ((y - tilePxY) * TILE_SIZE + (ox0 - tilePxX)) * 4;
                memcpy(dstRow, srcRow, (ox1 - ox0) * 4);
            }
            writeSlicePixels(slice, buf);
        }
    }

    commitStrokeUndo();
    updateCompositedTex();
    emit layersChanged();
    update();
}

// ===========================================================================
// レイヤープレビュー
// ===========================================================================
QImage GLWidget::getLayerPreview(int zStart, int zEnd, int size) {
    makeCurrent();
    return compositor_.renderLayerPreview(toolCtx_, zStart, zEnd, size);
}

QImage GLWidget::getNavigatorPreview(int size, bool fullCanvas)
{
    makeCurrent();
    return compositor_.renderNavigatorPreview(toolCtx_, size, fullCanvas);
}

QImage GLWidget::getMaskPreview(int layerIndex, int size)
{
    if (!doc_ || layerIndex < 0 || layerIndex >= doc_->layerCount()) return QImage();
    if (!doc_->layers[layerIndex].hasMask) return QImage();
    makeCurrent();
    return compositor_.renderMaskPreview(toolCtx_, layerIndex, size);
}

// ===========================================================================
// 塗りつぶし
// ===========================================================================
void GLWidget::executeFill(const QPointF &widgetPos, float wallThreshold)
{
    fillTool_.execute(toolCtx_, widgetPos);
}

// ===========================================================================
// Undo / Redo
// ===========================================================================
UndoEntry GLWidget::captureAllTiles(int layerIndex)
{
    return undoRecorder_.captureAllTiles(toolCtx_, layerIndex);
}

UndoEntry GLWidget::captureTilesLike(int layerIndex, const QVector<TileUndo> &shape)
{
    return undoRecorder_.captureTilesLike(toolCtx_, layerIndex, shape);
}

void GLWidget::pushUndoSnapshot()
{
    UndoEntry entry = captureAllTiles(doc_->activeLayerIndex());
    doc_->clearRedo();
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());
}

void GLWidget::undo()
{
    if (!doc_->canUndo()) return;
    makeCurrent();
    // Undo/Redoはレイヤー内容を書き換えるが doc_->onChanged を経由しない(直接update()する)
    // ため、事前合成キャッシュをここで明示的に無効化する。次の描画開始時に作り直される。
    invalidateBelowCompositeCache();
    invalidateAboveCompositeCache();
    scheduleCompositeCachePrewarm(); // 次のストローク開始を待たずアイドル中に作り直す
    // フィルターレイヤーの連鎖(GLWidget.h の filterChainResult_ 参照)も同じ理由で無効化が
    // 要る。これが抜けていると、フィルターレイヤーより下のレイヤーへ描いてUndoしても
    // 画面は古い(Undo前の)合成結果のキャッシュのまま変わらず、次に別のストロークを
    // 開始してようやく作り直されてUndoの結果が反映される、という不具合になっていた。
    invalidateFilterChain();

    if (doc_->topUndoKind() == UndoKind::CanvasResize) {
        UndoEntry restored = doc_->applyUndo(UndoEntry{}); // currentは使われない(kind==CanvasResize)
        applyCanvasResizeUndoEntry(restored, /*toBefore=*/true);
    } else if (doc_->topUndoKind() == UndoKind::Selection) {
        // CanvasResizeと同じく変更前後を自己完結して持つのでcurrentは使われない
        UndoEntry restored = doc_->applyUndo(UndoEntry{});
        applySelectionUndoEntry(restored, /*toBefore=*/true);
    } else if (doc_->topUndoKind() == UndoKind::LayerAdd) {
        UndoEntry restored = doc_->applyUndo(UndoEntry{});
        applyLayerAddUndoEntry(restored, /*toBefore=*/true);
    } else if (doc_->topUndoKind() == UndoKind::LayerRemove) {
        UndoEntry restored = doc_->applyUndo(UndoEntry{});
        applyLayerRemoveUndoEntry(restored, /*toBefore=*/true);
    } else if (doc_->topUndoKind() == UndoKind::LayerMerge) {
        UndoEntry restored = doc_->applyUndo(UndoEntry{});
        applyLayerMergeUndoEntry(restored, /*toBefore=*/true);
    } else {
        // "current"(取り消す直前の状態)は、これから復元する差分エントリが実際に
        // 触れるタイルだけをキャプチャすれば十分(restoreLayer()もそのタイルしか
        // 書き換えないため)。レイヤー全体を読み出すより大幅に軽い。
        UndoEntry current = captureTilesLike(doc_->activeLayerIndex(), doc_->peekUndo().tiles);
        UndoEntry restored = doc_->applyUndo(current);
        restoreLayer(restored);
        update();
        emit layersChanged();
    }
    emit modifiedChanged(doc_->isModified());
}

void GLWidget::redo()
{
    if (!doc_->canRedo()) return;
    makeCurrent();
    invalidateBelowCompositeCache(); // undo()と同じ理由
    invalidateAboveCompositeCache();
    scheduleCompositeCachePrewarm();
    invalidateFilterChain();

    if (doc_->topRedoKind() == UndoKind::CanvasResize) {
        UndoEntry restored = doc_->applyRedo(UndoEntry{});
        applyCanvasResizeUndoEntry(restored, /*toBefore=*/false);
    } else if (doc_->topRedoKind() == UndoKind::Selection) {
        UndoEntry restored = doc_->applyRedo(UndoEntry{});
        applySelectionUndoEntry(restored, /*toBefore=*/false);
    } else if (doc_->topRedoKind() == UndoKind::LayerAdd) {
        UndoEntry restored = doc_->applyRedo(UndoEntry{});
        applyLayerAddUndoEntry(restored, /*toBefore=*/false);
    } else if (doc_->topRedoKind() == UndoKind::LayerRemove) {
        UndoEntry restored = doc_->applyRedo(UndoEntry{});
        applyLayerRemoveUndoEntry(restored, /*toBefore=*/false);
    } else if (doc_->topRedoKind() == UndoKind::LayerMerge) {
        UndoEntry restored = doc_->applyRedo(UndoEntry{});
        applyLayerMergeUndoEntry(restored, /*toBefore=*/false);
    } else {
        // undo()と同じ理由で、redoしようとしているエントリが触れるタイルだけをキャプチャする。
        UndoEntry current = captureTilesLike(doc_->activeLayerIndex(), doc_->peekRedo().tiles);
        UndoEntry restored = doc_->applyRedo(current);
        restoreLayer(restored);
        update();
        emit layersChanged();
    }
    emit modifiedChanged(doc_->isModified());
}

void GLWidget::restoreLayer(const UndoEntry &entry)
{
    undoRecorder_.restoreLayer(toolCtx_, entry);
}

void GLWidget::beginStrokeUndo()
{
    undoRecorder_.beginStroke(toolCtx_);
}

void GLWidget::expandStrokeUndoRegion(int txMin, int txMax, int tyMin, int tyMax)
{
    undoRecorder_.expandRegion(toolCtx_, txMin, txMax, tyMin, tyMax);
}

void GLWidget::commitStrokeUndo()
{
    if (!undoRecorder_.hasPending()) return;

    UndoEntry entry = undoRecorder_.takeStrokeEntry(toolCtx_, doc_->activeLayerIndex());
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());

    // フィルターレイヤーより下のレイヤーへ描いている間は、prepareCompositeBase()が
    // ストローク中ずっと(withPaintPreview=trueで)連鎖を毎フレーム作り直しているが、
    // これはマウスを離した瞬間(=ここ)に止まる。ここで無効化しておかないと、
    // 「ストローク中の最後のプレビュー1枚」がそのまま固定表示され続け、実際に
    // 焼き込まれた本物のピクセルはUndoするか次のストロークを始めるまで一切
    // 反映されない(見た目にフィルターが効いていないように見える不具合になっていた)。
    // ここで無効化しておけば、次のpaintGL()がwithPaintPreview=falseの通常の連鎖を
    // 1回組み直し、焼き込み後の実データを正しく反映する。
    invalidateFilterChain();
}

// 「アクティブレイヤーより下(z < uptoExclusiveIndex)」を合成してキャッシュへ書く。
// 呼び出し側(PenEraserTool)は、これから編集するアクティブレイヤーのindexを渡す。
// belowComposite.compの詳細はそのシェーダーファイルのコメント参照。
void GLWidget::updateBelowCompositeCache(int uptoExclusiveIndex)
{
    // 既に有効なら作り直さない。無効化はレイヤー構成が変わったとき(doc_->onChanged)に
    // だけ行われるので、valid==true は「現在のアクティブ構成に対して正しい」ことを意味する。
    // これにより同じレイヤーへ連続で線を引くとき、全画面の事前合成を毎回やり直さずに済む。
    // ただしキャッシュの中身は uptoExclusiveIndex に依存するので、先読み作成
    // (prewarmCompositeCaches)時と違うレイヤーへ描き始めた場合は作り直す。
    if (belowCompositeCacheValid_ && belowCompositeCacheUpto_ == uptoExclusiveIndex) return;

    if (!doc_ || doc_->layerCount() == 0 || !computeBelowCompositeProgram) {
        belowCompositeCacheValid_ = false;
        return;
    }

    compositor_.updateLayerSSBOs(toolCtx_);

    computeBelowCompositeProgram->bind();
    bindLayerBanksForSampling(computeBelowCompositeProgram); // タイル配列(全バンク)をサンプル用にバインド
    glBindImageTexture(0, belowCompositeTex,         0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(1, belowCompositeClipBaseTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    // 初期値とライブプレビューはこの用途では使わない(uUseInit=0 / uPaintPreview=0)。
    // ただしシェーダーが宣言しているイメージユニットは有効なテクスチャで埋めておく。
    glBindImageTexture(2, belowCompositeTex,         0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(3, belowCompositeClipBaseTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(4, maskTex,          0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    glBindImageTexture(5, selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    computeBelowCompositeProgram->setUniformValue("uLayerCount", doc_->layerCount());
    computeBelowCompositeProgram->setUniformValue("uTargetStart", 0);
    computeBelowCompositeProgram->setUniformValue("uTargetEnd",  uptoExclusiveIndex - 1);
    computeBelowCompositeProgram->setUniformValue("uTileSize",   TILE_SIZE);
    computeBelowCompositeProgram->setUniformValue("uCanvasTilesX", doc_->tilesX());
    computeBelowCompositeProgram->setUniformValue("uUseInit",      0);
    computeBelowCompositeProgram->setUniformValue("uPaintPreview", 0);
    glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    computeBelowCompositeProgram->release();

    // イメージユニット0は、このコードベースの他の場所(stroke.comp/maskclear.comp)
    // では「呼び出し側が直前に明示的にbindし直さなくても、常にmaskTexが
    // バインドされている」という前提で使われている(それぞれの呼び出し末尾で
    // maskTexへ戻す形で維持されている暗黙の不変条件)。ここで一時的にimageユニット0を
    // belowCompositeTexへ差し替えたままにしておくと、この直後にPenEraserTool::
    // onMousePress()が行う「前回ストロークの範囲だけマスクをクリアする」処理
    // (item3)がmaskTexではなくbelowCompositeTexの方を書き換えてしまい、
    // そのクリア範囲(=直前のストロークの矩形)がキャッシュ上で透明の穴として
    // 抜けて見える不具合の原因になっていた。呼び出し前の状態へ戻す。
    glBindImageTexture(0, maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);

    belowCompositeCacheValid_ = true;
    belowCompositeCacheUpto_  = uptoExclusiveIndex;
}

// 事前合成キャッシュの先読み作成を予約する(GLWidget.hのcompositeCachePrewarmTimer_参照)。
void GLWidget::scheduleCompositeCachePrewarm()
{
    compositeCachePrewarmDone_ = false;
    if (compositeCachePrewarmTimer_) compositeCachePrewarmTimer_->start();
}

// 上下の事前合成キャッシュを、ストローク開始を待たずにここで作っておく。
void GLWidget::prewarmCompositeCaches()
{
    if (!glReady_ || !doc_ || doc_->layerCount() == 0) return;
    if (compositeCachePrewarmDone_) return;

    // ストローク中(またはドラッグ操作中)はGPUを取り合わないよう見送り、
    // 終わってからやり直す。ストローク開始時に必要ならその場で作られるので、
    // 見送っても正しさには影響しない。
    if (Tool *t = currentTool(); t && t->isActive()) {
        if (compositeCachePrewarmTimer_) compositeCachePrewarmTimer_->start();
        return;
    }
    // 非表示のタブ(別ペインへ切り替え済み等)では作らない。表示に戻ったときに
    // またレイヤー通知が来るか、最悪ストローク開始時に作られる。
    if (!isVisible()) return;

    compositeCachePrewarmDone_ = true;

    makeCurrent();
    const int active = doc_->activeLayerIndex();
    QElapsedTimer clock;
    clock.start();
    updateBelowCompositeCache(active);
    updateAboveCompositeCache(active);
    // ここまでで発行したディスパッチはGPUキューに積まれただけで、実処理は
    // 「次にpresentするフレーム」が待つことになる。その次のフレームがストロークの
    // 1枚目にならないよう、ここで通常の再描画を1回入れてアイドル中に消化させる。
    update();
    WINLOG(QStringLiteral("PERF prewarm: composite caches submitted in %1ms (below=%2 above=%3)")
               .arg(clock.nsecsElapsed() / 1e6, 0, 'f', 2)
               .arg(belowCompositeCacheValid_ ? 1 : 0)
               .arg(aboveCompositeCacheValid_ ? 1 : 0));
}

void GLWidget::updateAboveCompositeCache(int activeIndex)
{
    // 既に有効なら作り直さない(updateBelowCompositeCacheの同種コメント参照)。
    if (aboveCompositeCacheValid_) return;

    aboveCompositeCacheValid_ = false;
    if (!doc_ || doc_->layerCount() == 0 || !computeBelowCompositeProgram) return;

    const int n = doc_->layerCount();
    if (activeIndex < 0 || activeIndex >= n - 1) return; // 上にレイヤーが無ければ作る意味なし

    // フォルダーのマスクを編集中は事前合成しない。フォルダーの子は layers 上で
    // フォルダー自身の直後(=「上」)に並ぶため、このキャッシュを作ると子が丸ごと
    // 1枚のテクスチャに焼かれてしまい、render.fragのマスク編集ライブプレビュー
    // (previewMaskAlphaOf。祖先マスク経由で子へ効く)がストローク中に反映されなくなる。
    if (doc_->layers[activeIndex].layerType == LayerType::Folder
        && editingMaskLayerIndex_ == activeIndex)
        return;

    // 有効化条件: アクティブより上のレイヤーが全て「通常ブレンド・非クリッピング・
    // 非調整レイヤー」であること。この条件下なら上のスタックは互いに source-over
    // (通常合成)だけで積み上がり、結合的なので1枚へ事前合成→最後に1回重ねる、で
    // 通常経路と同一の結果になる。1つでも非通常ブレンド/クリッピング/調整レイヤーが
    // あると事前合成が下の合成結果に依存してしまい正しくないため、諦めて従来通り
    // 毎フレーム個別合成へフォールバックする。
    for (int z = activeIndex + 1; z < n; z++) {
        const Layer &ly = doc_->layers[z];
        if (ly.layerType == LayerType::Adjustment) return;
        // フィルターレイヤーも同じ理由で事前合成できない(効果が下の合成結果に
        // 依存するうえ、近傍参照なので1枚に畳んでから重ねることができない)。
        if (ly.layerType == LayerType::Filter) return;
        if (ly.blendMode != BlendMode::Normal) return;
        if (ly.clipping) return;
    }

    compositor_.updateLayerSSBOs(toolCtx_);

    computeBelowCompositeProgram->bind();
    bindLayerBanksForSampling(computeBelowCompositeProgram);
    glBindImageTexture(0, aboveCompositeTex,          0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(1, aboveCompositeClipScratch_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8); // clipBase出力は使わない捨て先
    glBindImageTexture(2, aboveCompositeTex,          0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(3, aboveCompositeClipScratch_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(4, maskTex,          0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    glBindImageTexture(5, selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    computeBelowCompositeProgram->setUniformValue("uLayerCount", n);
    computeBelowCompositeProgram->setUniformValue("uTargetStart", activeIndex + 1);
    computeBelowCompositeProgram->setUniformValue("uTargetEnd",  n - 1);
    computeBelowCompositeProgram->setUniformValue("uTileSize",   TILE_SIZE);
    computeBelowCompositeProgram->setUniformValue("uCanvasTilesX", doc_->tilesX());
    computeBelowCompositeProgram->setUniformValue("uUseInit",      0);
    computeBelowCompositeProgram->setUniformValue("uPaintPreview", 0);
    glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    computeBelowCompositeProgram->release();

    // イメージユニット0をmaskTexへ戻す(updateBelowCompositeCacheの同種コメント参照)。
    glBindImageTexture(0, maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);

    aboveCompositeCacheValid_ = true;
}

// ===========================================================================
// フィルターレイヤーの連鎖 (GLWidget.h の filterChainResult_ のコメント参照)
// ===========================================================================

// このレイヤーが「実際に表示へ効いているフィルターレイヤー」か。
// 非表示・不透明度0・非表示フォルダーの中にあるものは効いていない扱いにして、
// 連鎖から外す(=オフスクリーンの合成そのものを省ける)。
static bool filterLayerIsActive(const CanvasDocument &doc, int z,
                                const QVector<QVector<int>> &ancestorsPerLayer)
{
    const Layer &ly = doc.layers[z];
    if (ly.layerType != LayerType::Filter) return false;
    if (!ly.visible || ly.opacity <= 0.0f)  return false;
    for (int f : ancestorsPerLayer[z]) {
        if (f < 0 || f >= doc.layers.size()) continue;
        const Layer &folder = doc.layers[f];
        if (!folder.visible || folder.opacity <= 0.0f) return false;
    }
    return true;
}

int GLWidget::topmostActiveFilterLayer() const
{
    if (!doc_ || doc_->layerCount() == 0) return -1;
    // フィルターレイヤーが1枚も無い文書(大多数)では、ここで即座に抜けて
    // computeAncestorFolders()のツリー走査すら行わない。
    bool any = false;
    for (const Layer &ly : doc_->layers)
        if (ly.layerType == LayerType::Filter) { any = true; break; }
    if (!any) return -1;

    const QVector<QVector<int>> ancestors = doc_->computeAncestorFolders();
    int top = -1;
    for (int z = 0; z < doc_->layerCount(); z++)
        if (filterLayerIsActive(*doc_, z, ancestors)) top = z;
    return top;
}

bool GLWidget::ensureFilterChainTextures()
{
    if (canvasW <= 0 || canvasH <= 0) return false;
    if (filterChainResult_[0]) return true;
    for (int i = 0; i < 2; i++) {
        // GL_LINEAR は必須。render.frag がこれを画面表示のために直接サンプリング
        // するので、キャッシュを使わない経路が読むレイヤーのタイル配列
        // (LayerSliceAllocator、GL_LINEAR)と揃っていないと、フィルターレイヤーの
        // 有無だけで表示のアンチエイリアスが変わってしまう
        // (belowCompositeTex等で実際に起きた不具合。initTextures()のコメント参照)。
        filterChainResult_[i] = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
        filterChainClip_[i]   = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
    }
    filterChainBlurScratch_ = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
    return true;
}

void GLWidget::freeFilterChainTextures()
{
    for (int i = 0; i < 2; i++) {
        if (filterChainResult_[i]) { glDeleteTextures(1, &filterChainResult_[i]); filterChainResult_[i] = 0; }
        if (filterChainClip_[i])   { glDeleteTextures(1, &filterChainClip_[i]);   filterChainClip_[i]   = 0; }
    }
    if (filterChainBlurScratch_) { glDeleteTextures(1, &filterChainBlurScratch_); filterChainBlurScratch_ = 0; }
    filterChainOutResult_ = 0;
    filterChainOutClip_   = 0;
    filterChainValid_     = false;
    filterChainStartZ_    = -1;
}

void GLWidget::dispatchCompositeSegment(int targetStart, int targetEnd,
                                        bool useInit, int initResultIdx, int initClipIdx,
                                        int dstResultIdx, int dstClipIdx, bool withPaintPreview)
{
    auto *prog = computeBelowCompositeProgram;
    prog->bind();
    bindLayerBanksForSampling(prog);

    glBindImageTexture(0, filterChainResult_[dstResultIdx], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(1, filterChainClip_[dstClipIdx],     0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    // 初期値。使わない場合でも、シェーダーが宣言している以上イメージユニットには
    // 有効なテクスチャを繋いでおく(読み書きが同じテクスチャにならないよう、
    // 出力先ではない方のping-pong面を指す)。
    const int inR = useInit ? initResultIdx : (dstResultIdx ^ 1);
    const int inC = useInit ? initClipIdx   : (dstClipIdx   ^ 1);
    glBindImageTexture(2, filterChainResult_[inR], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(3, filterChainClip_[inC],   0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(4, maskTex,          0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    glBindImageTexture(5, selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);

    prog->setUniformValue("uLayerCount",   doc_->layerCount());
    prog->setUniformValue("uTargetStart",  targetStart);
    prog->setUniformValue("uTargetEnd",    targetEnd);
    prog->setUniformValue("uTileSize",     TILE_SIZE);
    prog->setUniformValue("uCanvasTilesX", doc_->tilesX());
    prog->setUniformValue("uUseInit",      useInit ? 1 : 0);

    // ライブプレビュー(フィルターレイヤーより下に描いているときだけ)。値は
    // paintGL が render.frag へ渡すものと同じでなければならない(食い違うと
    // ストローク中とストローク後で線の見た目が変わる)。
    prog->setUniformValue("uPaintPreview", withPaintPreview ? 1 : 0);
    if (withPaintPreview) {
        // paintGL が render.frag へ渡すのと同じ考え方(あちらのコメント参照)。
        const float brushOpacity = (activeTool == ToolType::Airbrush)
                                 ? toolCfg_->airbrush().opacity() : toolCfg_->pen().opacity();
        const EraseBrush erase = eraseBrushFor(activeTool == ToolType::Eraser,
                                               toolCfg_->color(), brushOpacity);
        const QColor col = erase.active ? erase.shaderColor()
                                        : toPreMulColor(toolCfg_->color().rawRGBA(), brushOpacity);
        const QColor maskCol = erase.active
            ? maskBrushColor(true, toolCfg_->color().rawRGBA(), erase.strength)
            : maskBrushColor(false, toolCfg_->color().rawRGBA(), brushOpacity);
        prog->setUniformValue("uActiveLayerIndex", doc_->activeLayerIndex());
        prog->setUniformValue("uEraseMode", erase.active ? 1 : 0);
        prog->setUniformValue("uBrushBlendMode",
            (activeTool == ToolType::Pen && !erase.active) ? toolCfg_->pen().brushBlendMode() : 0);
        const bool useStrokeColor = (activeTool == ToolType::Pen && !erase.active
                                     && toolCfg_->pen().usesPerStampColor() && strokeColorTex != 0);
        prog->setUniformValue("uUseStrokeColor", useStrokeColor ? 1 : 0);
        if (useStrokeColor)
            glBindImageTexture(6, strokeColorTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        prog->setUniformValue("uIsEditingMaskLayer",
            (editingMaskLayerIndex_ >= 0 && editingMaskLayerIndex_ == doc_->activeLayerIndex()) ? 1 : 0);
        prog->setUniformValue("uBrushColor", col.redF(), col.greenF(), col.blueF(), col.alphaF());
        prog->setUniformValue("uMaskBrushColor",
            maskCol.redF(), maskCol.greenF(), maskCol.blueF(), maskCol.alphaF());
        prog->setUniformValue("uHasSelection", hasSelection_ ? 1 : 0);
    }

    glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    prog->release();
}

bool GLWidget::applyFilterLayer(int z, int srcIdx)
{
    const Layer &ly = doc_->layers[z];

    // フォルダーの不透明度・マスクをカスケードする(CanvasCompositor::updateLayerSSBOs
    // と同じ考え方。フィルターレイヤーは合成ループを通らないのでここで自前に行う)。
    // 両方の種類(色収差/ぼかし)で共通して使う。
    const QVector<QVector<int>> ancestors = doc_->computeAncestorFolders();
    float effOpacity = ly.opacity;
    int   ancMask[4] = { -1, -1, -1, -1 };
    int   maskSlot = 0;
    for (int f : ancestors[z]) {
        if (f < 0 || f >= doc_->layerCount()) continue;
        const Layer &folder = doc_->layers[f];
        effOpacity *= folder.opacity;
        if (folder.hasMask && !folder.maskTiles.isEmpty() && maskSlot < 4)
            ancMask[maskSlot++] = folder.maskTiles[0][0];
    }
    const int ownMask = (ly.hasMask && !ly.maskTiles.isEmpty()) ? ly.maskTiles[0][0] : -1;

    // 「不透明度×レイヤーマスク」に関わるuniformは2シェーダーで名前も意味も同じ
    // (chromaticAberrationLayer.comp / gaussianBlurLayer.comp 共通)なのでまとめる。
    auto setMaskOpacityUniforms = [&](QOpenGLShaderProgram *prog) {
        prog->setUniformValue("uOpacity",    effOpacity);
        prog->setUniformValue("uTileSize",   TILE_SIZE);
        prog->setUniformValue("uCanvasTilesX", doc_->tilesX());
        prog->setUniformValue("uMaskBase",   ownMask);
        GLint loc = glGetUniformLocation(prog->programId(), "uAncestorMaskBases");
        if (loc >= 0) glUniform4i(loc, ancMask[0], ancMask[1], ancMask[2], ancMask[3]);
    };

    if (ly.filter.kind == FilterKind::ChromaticAberration) {
        auto *prog = computeChromaticAberrationLayerProgram;
        if (!prog) return false; // 無料版ビルド: フィルターレイヤーは効果なしで素通り

        prog->bind();
        bindLayerBanksForSampling(prog);
        glBindImageTexture(0, filterChainResult_[srcIdx],     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainResult_[srcIdx ^ 1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        prog->setUniformValue("uMode",       ly.filter.caMode);
        prog->setUniformValue("uAngleRad",   (float)qDegreesToRadians(ly.filter.caAngleDeg));
        prog->setUniformValue("uDistancePx", ly.filter.caDistancePx);
        prog->setUniformValue("uCenterPx",   QVector2D(ly.filter.caCenterU * canvasW,
                                                       ly.filter.caCenterV * canvasH));
        setMaskOpacityUniforms(prog);

        glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        prog->release();
        return true;
    }

    if (ly.filter.kind == FilterKind::GaussianBlur) {
        auto *prog = computeGaussianBlurLayerProgram;
        if (!prog) return false; // 通常は常に存在する(無料版でも使える機能のため)

        // 2次元ガウスは分離できるので「横1D→縦1D」の2パス
        // (詳細は gaussianBlurLayer.comp のコメント参照)。中間バッファは
        // 読み込み元(filterChainResult_[srcIdx])とも書き込み先
        // (filterChainResult_[srcIdx^1])とも別の1枚が要る。
        const float sigma = qMax(0.5f, ly.filter.blurRadiusPx / 2.0f);
        const int   radius = qMax(1, (int)qRound(ly.filter.blurRadiusPx));
        const GLuint gx = (canvasW + 15) / 16, gy = (canvasH + 15) / 16;

        prog->bind();
        bindLayerBanksForSampling(prog);
        prog->setUniformValue("uKernelRadius", radius);
        prog->setUniformValue("uSigma", sigma);
        prog->setUniformValue("uWrapX", doc_->wrapX() ? 1 : 0);
        prog->setUniformValue("uWrapY", doc_->wrapY() ? 1 : 0);
        setMaskOpacityUniforms(prog);

        // 1パス目: 横ぼかしを中間バッファへ
        glBindImageTexture(0, filterChainResult_[srcIdx], 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainBlurScratch_,     0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glBindImageTexture(2, filterChainResult_[srcIdx],  0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8); // uOrig(1パス目では未使用)
        {
            GLint loc = glGetUniformLocation(prog->programId(), "uDir");
            if (loc >= 0) glUniform2i(loc, 1, 0);
        }
        prog->setUniformValue("uFinalPass", 0);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

        // 2パス目: 縦ぼかし + 不透明度・マスクでのブレンドを仕上げる
        glBindImageTexture(0, filterChainBlurScratch_,         0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainResult_[srcIdx ^ 1],  0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glBindImageTexture(2, filterChainResult_[srcIdx],      0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8); // uOrig
        {
            GLint loc = glGetUniformLocation(prog->programId(), "uDir");
            if (loc >= 0) glUniform2i(loc, 0, 1);
        }
        prog->setUniformValue("uFinalPass", 1);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

        prog->release();
        return true;
    }

    if (ly.filter.kind == FilterKind::MotionBlur) {
        auto *prog = computeMotionBlurLayerProgram;
        if (!prog) return false; // 通常は常に存在する(無料版でも使える機能のため)

        // 経路長に見合ったサンプル数(MotionBlurTool::sampleCountと同じ考え方だが、
        // 特定のレイヤーに紐付かないのでキャンバスサイズを基準にする)。
        float pathPx;
        if (ly.filter.mbMode == 0) {
            pathPx = ly.filter.mbDistancePx;
        } else {
            const float cx = ly.filter.mbCenterU * canvasW;
            const float cy = ly.filter.mbCenterV * canvasH;
            const float dx = qMax(cx, (float)canvasW - cx);
            const float dy = qMax(cy, (float)canvasH - cy);
            const float maxR = std::sqrt(dx * dx + dy * dy);
            pathPx = maxR * qDegreesToRadians(ly.filter.mbAngleSpanDeg);
        }
        const int samples = qBound(1, (int)std::lround(pathPx) + 1, 128);

        prog->bind();
        bindLayerBanksForSampling(prog);
        glBindImageTexture(0, filterChainResult_[srcIdx],     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainResult_[srcIdx ^ 1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        prog->setUniformValue("uMode",         ly.filter.mbMode);
        prog->setUniformValue("uAngleRad",     (float)qDegreesToRadians(ly.filter.mbAngleDeg));
        prog->setUniformValue("uDistancePx",   ly.filter.mbDistancePx);
        prog->setUniformValue("uCenterPx",     QVector2D(ly.filter.mbCenterU * canvasW, ly.filter.mbCenterV * canvasH));
        prog->setUniformValue("uAngleSpanRad", (float)qDegreesToRadians(ly.filter.mbAngleSpanDeg));
        prog->setUniformValue("uSamples",      samples);
        setMaskOpacityUniforms(prog);

        glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        prog->release();
        return true;
    }

    if (ly.filter.kind == FilterKind::LensBlur) {
        auto *prog = computeLensBlurLayerProgram;
        if (!prog) return false; // 無料版ビルド: フィルターレイヤーは効果なしで素通り

        // LensBlurTool::sampleCount/bokehGamma と同じ式。
        const int   samples    = qBound(12, (int)std::lround(ly.filter.lbRadiusPx * 6.0f), 192);
        const float bokehGamma = 1.0f + ly.filter.lbHighlightBoost * 3.0f;

        prog->bind();
        bindLayerBanksForSampling(prog);
        glBindImageTexture(0, filterChainResult_[srcIdx],     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainResult_[srcIdx ^ 1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        prog->setUniformValue("uRadiusPx",       ly.filter.lbRadiusPx);
        prog->setUniformValue("uSamples",        samples);
        prog->setUniformValue("uBlades",         ly.filter.lbBlades);
        prog->setUniformValue("uBladeRotRad",    (float)qDegreesToRadians(ly.filter.lbBladeRotDeg));
        prog->setUniformValue("uBokehGamma",     bokehGamma);
        prog->setUniformValue("uThreshold",      ly.filter.lbThreshold);
        // シェーダー側の重みは 1 + boost * smoothstep(...) なので、LensBlurToolと
        // 同じくスライダー1.0でハイライトが周囲の数倍の重みになるようスケールする。
        prog->setUniformValue("uHighlightBoost", ly.filter.lbHighlightBoost * 8.0f);
        setMaskOpacityUniforms(prog);

        glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        prog->release();
        return true;
    }

    if (ly.filter.kind == FilterKind::Mosaic) {
        auto *reduceProg = computeMosaicReduceLayerProgram;
        auto *prog = computeMosaicLayerProgram;
        if (!reduceProg || !prog) return false; // 通常は常に存在する(無料版でも使える機能のため)

        const int blockSize = qBound(2, ly.filter.mzBlockSize, 256);
        const int gridX = (canvasW + blockSize - 1) / blockSize;
        const int gridY = (canvasH + blockSize - 1) / blockSize;

        // 1パス目: ブロック平均を filterChainBlurScratch_(ガウスぼかしレイヤーと
        // 共用の中間バッファ)の左上へ集約する(詳細は mosaicReduceLayer.comp 参照)。
        reduceProg->bind();
        glBindImageTexture(0, filterChainResult_[srcIdx], 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainBlurScratch_,     0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        reduceProg->setUniformValue("uBlockSize", blockSize);
        glDispatchCompute((gridX + 7) / 8, (gridY + 7) / 8, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        reduceProg->release();

        // 2パス目: ブロック平均を引いて不透明度・マスクでブレンドする
        prog->bind();
        bindLayerBanksForSampling(prog);
        glBindImageTexture(0, filterChainResult_[srcIdx],     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainResult_[srcIdx ^ 1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glBindImageTexture(2, filterChainBlurScratch_,         0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        prog->setUniformValue("uBlockSize", blockSize);
        setMaskOpacityUniforms(prog);

        glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        prog->release();
        return true;
    }

    if (ly.filter.kind == FilterKind::Noise) {
        auto *prog = computeNoiseLayerProgram;
        if (!prog) return false; // 通常は常に存在する(無料版でも使える機能のため)

        prog->bind();
        bindLayerBanksForSampling(prog);
        glBindImageTexture(0, filterChainResult_[srcIdx],     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainResult_[srcIdx ^ 1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        // NoiseTool::updatePreviewと同じ式(スライダー100%でストレート色を最大±0.5ずらす)。
        prog->setUniformValue("uAmount",     ly.filter.nsStrength * 0.5f);
        prog->setUniformValue("uMonochrome", ly.filter.nsMonochrome ? 1 : 0);
        prog->setUniformValue("uGrainPx",    ly.filter.nsGrainPx);
        {
            GLint loc = glGetUniformLocation(prog->programId(), "uSeed");
            if (loc >= 0) glUniform1ui(loc, (GLuint)ly.filter.nsSeed);
        }
        setMaskOpacityUniforms(prog);

        glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        prog->release();
        return true;
    }

    return false;
}

void GLWidget::rebuildFilterChain(int startZ, bool withPaintPreview)
{
    filterChainValid_ = false;
    if (!doc_ || !computeBelowCompositeProgram) return;
    if (!ensureFilterChainTextures()) return;

    compositor_.updateLayerSSBOs(toolCtx_);
    const QVector<QVector<int>> ancestors = doc_->computeAncestorFolders();

    int  ri = 0, ci = 0;      // 現在の結果/clipBaseが入っているping-pong面
    bool haveInit = false;    // ri/ci が有効か(=1区間でも合成済みか)
    int  segStart = 0;

    // [segStart, segEnd] を合成して ri/ci を進める
    auto composeUpTo = [&](int segEnd) {
        if (segEnd < segStart) return;
        const int dr = haveInit ? (ri ^ 1) : 0;
        const int dc = haveInit ? (ci ^ 1) : 0;
        dispatchCompositeSegment(segStart, segEnd, haveInit, ri, ci, dr, dc, withPaintPreview);
        ri = dr; ci = dc; haveInit = true;
    };

    for (int z = 0; z < startZ && z < doc_->layerCount(); z++) {
        if (!filterLayerIsActive(*doc_, z, ancestors)) continue;

        composeUpTo(z - 1);
        if (!haveInit) {
            // 一番下がフィルターレイヤーで、その下に何も無い場合。合成結果は空なので
            // 空の区間を1回合成して(=透明で埋めて)から進める。
            dispatchCompositeSegment(0, -1, false, 0, 0, 0, 0, withPaintPreview);
            ri = 0; ci = 0; haveInit = true;
        }
        if (applyFilterLayer(z, ri)) ri ^= 1; // clipBaseはフィルターを通さないのでそのまま
        segStart = z + 1;
    }

    // 一番上のフィルターレイヤーより上に残っている区間(表示用途では空、
    // 書き出し用途では最上位レイヤーまで)。
    composeUpTo(qMin(startZ, doc_->layerCount()) - 1);

    if (!haveInit) {
        // フィルターレイヤーが1枚も効いていない、かつ合成すべき区間も無い
        // (startZ==0)。呼び出し側が事前合成なしとして扱えるようvalidにしない。
        return;
    }

    filterChainOutResult_ = filterChainResult_[ri];
    filterChainOutClip_   = filterChainClip_[ci];
    filterChainStartZ_    = startZ;
    filterChainValid_     = true;

    // イメージユニット0をmaskTexへ戻す(updateBelowCompositeCacheの同種コメント参照)。
    glBindImageTexture(0, maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);
}

int GLWidget::prepareCompositeBase(GLuint &outResult, GLuint &outClip)
{
    outResult = belowCompositeTex;
    outClip   = belowCompositeClipBaseTex;

    const int topFilter = topmostActiveFilterLayer();
    if (topFilter < 0) {
        // フィルターレイヤーが無い(大多数の文書)。従来通り、ストローク中の
        // 下キャッシュだけを使う。
        return belowCompositeCacheValid_ ? doc_->activeLayerIndex() : -1;
    }

    // フィルターレイヤーがある間は、ストローク用の下キャッシュ(フィルターの効果を
    // 含まない)は使えないので、必ず連鎖の結果から始める。
    const int startZ = topFilter + 1;
    // アクティブレイヤーがフィルターより下にある間は、ストロークの線そのものが
    // 連鎖の入力になるので毎フレーム作り直す必要がある。
    const bool strokingBelowFilter =
        (currentTool() && currentTool()->isActive() && doc_->activeLayerIndex() <= topFilter);

    if (!filterChainValid_ || filterChainStartZ_ != startZ || strokingBelowFilter)
        rebuildFilterChain(startZ, strokingBelowFilter);

    if (!filterChainValid_) return -1;
    outResult = filterChainOutResult_;
    outClip   = filterChainOutClip_;
    return startZ;
}


// ===========================================================================
// 書き出し
// ===========================================================================
QImage GLWidget::exportCanvas()
{
    makeCurrent();
    // フィルターレイヤーを含む文書は、タイル単位の composite.comp では正しく
    // 書き出せない(近傍参照なのでタイルの外が見えない)。キャンバス全面を扱う
    // 連鎖(rebuildFilterChain)を最上位レイヤーまで通した結果を読み戻す。
    if (topmostActiveFilterLayer() >= 0) {
        QImage img = renderExportViaFilterChain();
        if (!img.isNull()) return img;
        qWarning() << "exportCanvas: フィルター連鎖の書き出しに失敗したため、"
                      "フィルターレイヤーを無視した結果を返します";
    }
    return compositor_.renderExport(toolCtx_);
}

// 全レイヤー(フィルター適用済み)をキャンバス全面で合成して読み戻す。
// 読み戻しの手順と最後の上下反転は CanvasCompositor::renderExport と同じ
// (連鎖の出力テクスチャは、あちらが組み立てる exportTex と同じ向き ―― タイル
//  (tx,ty)がピクセル(tx*TILE_SIZE, ty*TILE_SIZE)に対応する ―― になっている)。
QImage GLWidget::renderExportViaFilterChain()
{
    if (!doc_ || doc_->layerCount() == 0) return QImage();

    rebuildFilterChain(doc_->layerCount(), /*withPaintPreview=*/false);
    // 表示用のキャッシュとしては「最上位まで合成済み」は使えないので、
    // 次の paintGL で表示用に組み直させる。
    const bool ok = filterChainValid_;
    const GLuint outTex = filterChainOutResult_;
    invalidateFilterChain();
    if (!ok || !outTex) return QImage();

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outTex, 0);

    QVector<uint8_t> buf(canvasW * canvasH * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, canvasW, canvasH, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    glDeleteFramebuffers(1, &fbo);

    QImage img(buf.constData(), canvasW, canvasH,
               canvasW * 4, QImage::Format_RGBA8888_Premultiplied);
    return img.copy().convertToFormat(QImage::Format_RGBA8888).mirrored(false, true);
}

bool GLWidget::exportToImage(const QString &filePath)
{
    // 既存の関数を呼び出して合成済みの QImage を取得
    QImage img = exportCanvas();

    if (img.isNull()) {
        return false; // 画像の取得に失敗した場合
    }

    // 指定されたパスに画像を保存 (フォーマットは拡張子から Qt が自動判定してくれます)
    // 最高の品質(100)で保存する場合は第3引数に100を指定できます
    return img.save(filePath, nullptr, 100);
}

// ===========================================================================
// 保存
// ===========================================================================
void GLWidget::resetDocument()
{
    makeCurrent();
 
    // 使用中の全スライスをゼロクリア(バンク境界をまたいでも clearSliceRange が分割処理する)
    clearSliceRange(0, sliceAllocator_.nextSlice(), 0.0f, 0.0f, 0.0f, 0.0f);
 
    // スライス管理をリセット
    sliceAllocator_.resetAllocationState();

    // CanvasDocument をリセット: 全レイヤーを捨てる
    // (所有権はGLWidget外にあるので、作り直すのではなく中身だけ空にする)
    //
    // addLayer()自体がdoc_->onChanged経由でupdate()を呼び得るため、
    // レイヤーがまだ0件のこの時点で誤発火しないようガードする
    // (呼び出し元がこの後レイヤーを追加してからemit layersChanged()する想定)
    m_initializing = true;
    doc_->resetToBlank();
    doc_->initTileGrid(canvasW, canvasH);
    m_initializing = false;

    emit layersChanged();
}

QByteArray GLWidget::readSlicePixels(int texArraySlice)
{
    makeCurrent();
    return undoRecorder_.readSlicePixels(toolCtx_, texArraySlice);
}
 
void GLWidget::writeSlicePixels(int texArraySlice, const QByteArray &raw)
{
    makeCurrent();
    undoRecorder_.writeSlicePixels(toolCtx_, texArraySlice, raw);
}

void GLWidget::writeSlicePixelsBatch(const QVector<int> &slices,
                                     const QVector<const QByteArray *> &data,
                                     const std::function<void(int)> &onProgress)
{
    makeCurrent();
    undoRecorder_.writeSlicesBatch(toolCtx_, slices, data, onProgress);
}

QVector<QByteArray> GLWidget::readSlicePixelsBatch(const QVector<int> &slices)
{
    makeCurrent();
    return undoRecorder_.readSlicesBatch(toolCtx_, slices);
}

// ===========================================================================
// 内部ヘルパー
// ===========================================================================
// growLayerTexArray() / allocContiguousSlices() / freeSlice() は
// LayerSliceAllocator に移動した(sliceAllocator_)

QByteArray GLWidget::loadShaderSource(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open shader:" << path;
        return {};
    }
    return file.readAll();
}

QColor GLWidget::toPreMulColor(const QColor &rawColor, float opacity) {
    float a = rawColor.alphaF() * opacity;
    return QColor::fromRgbF(
        rawColor.redF()   * a,
        rawColor.greenF() * a,
        rawColor.blueF()  * a,
        a
    );
}

QColor GLWidget::getPixelColor(const QPointF &widgetPos, bool referenceCanvas)
{
    if (doc_->layers.isEmpty()) return Qt::transparent;

    makeCurrent();

    QVector2D canvasPx = widgetToPixel(widgetPos);
    int sx = qRound(canvasPx.x());
    int sy = qRound(canvasPx.y());

    if (sx < 0 || sx >= canvasW || sy < 0 || sy >= canvasH) {
        return Qt::transparent;
    }

    // 全レイヤーを合成したキャンバス(compositedTex)を最新化する。
    // 【重要】この呼び出しは、下で読み取り用FBOを作って束縛するより「前」でなければ
    // ならない。updateCompositedTex()は内部で自前のFBOを使い、最後に
    // glBindFramebuffer(GL_FRAMEBUFFER, 0)(=読み書き両方を0に戻す)するため、
    // 先にこちらのFBOを束縛してから呼ぶと、それが外されたまま次の
    // glFramebufferTexture2D()がデフォルトフレームバッファに対する不正な操作になり、
    // 続くglReadPixels()がデフォルトフレームバッファ(Qtの描画先ではない)を読んで
    // 常に空=透明を返していた(スポイトの「プレビューから取得」が必ず透明になる不具合)。
    if (referenceCanvas)
        updateCompositedTex();

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);

    int readX = sx, readY = sy;

    if (referenceCanvas) {
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, toolCtx_.compositedTex, 0);
    } else {
        // 現在のレイヤーのみから拾う
        int tx = sx / TILE_SIZE;
        int ty = sy / TILE_SIZE;
        int si = doc_->activeLayer().tileSliceAtCanvasTile(tx, ty);
        if (si < 0) {
            glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
            glDeleteFramebuffers(1, &fbo);
            return Qt::transparent; // 通常起こらない(レイヤーは常にキャンバス全体を覆う)
        }
        readX = sx % TILE_SIZE;
        readY = sy % TILE_SIZE;
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  bankTexOf(si), 0, localSliceOf(si));
    }

    uint8_t pixel[4] = {};
    glReadPixels(readX, readY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    glDeleteFramebuffers(1, &fbo);

    // レイヤーのタイルも合成結果(compositedTex)も、中身は「乗算済みアルファ
    // (premultiplied)」で格納されている(描画色を作るtoPreMulColor()と、
    // common.glslのnormalBlend()=fg+bg*(1-fg.a)がその形式を前提にしている)。
    // 一方、この関数の戻り値を受け取る側(スポイト→ToolConfig::setRawRGBA())が
    // 扱うのは非乗算(ストレート)の生の色なので、ここでアルファを割り戻す。
    //
    // 割り戻しを忘れると、拾った色をそのまま描画色にして塗る→また拾う…の
    // 繰り返しで、塗るたびにRGBへアルファが二重・三重に掛かっていき、色が
    // 循環のたびに暗く(=RGBが0へ向かうので明度も彩度も低下)なっていく。
    const float a = pixel[3] / 255.0f;
    if (a <= 0.0f) return Qt::transparent; // 完全透明: 色情報が無い(呼び出し側で判断する)
    const auto unpremul = [a](uint8_t c) {
        return (int)qBound(0.0f, std::round(c / a), 255.0f);
    };
    return QColor(unpremul(pixel[0]), unpremul(pixel[1]), unpremul(pixel[2]), pixel[3]);
}

// ===========================================================================
// キャンバスの再構築（MainWindowから呼ばれる）
// ===========================================================================
void GLWidget::recreateCanvas(int w, int h, bool createDefaultLayers, bool wrapX, bool wrapY) {
    makeCurrent(); // OpenGLコンテキストをアクティブにする（必須）

    // 1. 古いテクスチャを破棄
    freeTextures();

    // 2. サイズ変数の更新
    canvasW = w;
    canvasH = h;

    // 3. ドキュメントの完全リセット
    // (所有権はGLWidget外にあるので、作り直すのではなく中身だけ空にする)
    sliceAllocator_.resetAllocationState();
    doc_->resetToBlank();
    doc_->setWrap(wrapX, wrapY);

    // 4. 新しいサイズでテクスチャ群を再生成し、初期レイヤーを作る
    // (createDefaultLayers=falseの場合はレイヤーを1枚も作らない。PsdCodec::loadが
    //  PSD側のレイヤー構成をそのまま復元するために使う)
    initTextures(createDefaultLayers);

    // canvasW/H・テクスチャIDが変わったので toolCtx_ も更新
    setupToolContext();

    fitCanvasToView();

    emit layersChanged();
    emit selectionChanged(hasSelection_); // initTextures()内でhasSelection_=falseにリセット済み
    update();
}

// ===========================================================================
// キャンバスサイズ変更(編集アクション「キャンバスサイズ変更」の確定処理)
// ---------------------------------------------------------------------------
// recreateCanvas()と違い、既存レイヤーの中身を保持したままキャンバスの
// 縦横サイズを変える(内側へのトリミング/外側への拡張どちらも可)。
// タイル境界の再配置を伴うため、いったん全レイヤーをCPU側のQImageへ読み出し、
// オフセット付きで新サイズのQImageに描き直してからタイルへ書き戻す
// (この操作の頻度は低いため、GPU compute化はせずCPUで完結させている)。
//
// タイルグリッド自体が変わる(=通常のTileUndo差分方式が使えない)ため、
// Undoは変更前後の全レイヤースナップショットを丸ごと保持するCanvasResizeエントリで行う
// (CanvasDocument::UndoKind::CanvasResize)。
// ===========================================================================
QVector<LayerSnapshotData> GLWidget::captureAllLayerSnapshots()
{
    QVector<LayerSnapshotData> result;
    result.reserve(doc_->layerCount());

    const int tilesX = doc_->tilesX(), tilesY = doc_->tilesY();
    for (int li = 0; li < doc_->layerCount(); li++) {
        const Layer &layer = doc_->layers[li];

        // 単色レイヤー/調整レイヤーは実ピクセルデータを持たない(単色は常に手続き的な
        // 不透明白、調整レイヤーは下のレイヤーへの色調整のみ)ため、タイルを読み出す
        // 必要はない。layerType/adjustmentだけスナップショットに残し、復元側
        // (rebuildCanvasFromSnapshots)で同じ種類のレイヤーとして作り直す。
        // テキストレイヤーはNormalと同様に実ピクセルを持つので、下の通常経路で
        // 画像として読み出す(text自体もメタデータとして一緒に保存する)。
        // マスクを持つ場合、種類に関わらずキャンバス全体ぶんのグレースケール画像として
        // 読み出しておく(単色/調整/フォルダーレイヤーでもマスク自体は独立して持てる)。
        QImage maskImg;
        if (layer.hasMask) {
            QVector<int> maskSlices;
            QVector<QPoint> maskTileCoords;
            for (int ty = 0; ty < tilesY; ty++) {
                for (int tx = 0; tx < tilesX; tx++) {
                    int si = layer.maskTileSlice(tx, ty);
                    if (si < 0) continue;
                    maskSlices.append(si);
                    maskTileCoords.append(QPoint(tx, ty));
                }
            }
            const QVector<QByteArray> rawMaskTiles = readSlicePixelsBatch(maskSlices);
            maskImg = QImage(canvasW, canvasH, QImage::Format_RGBA8888_Premultiplied);
            maskImg.fill(Qt::white);
            QPainter mp(&maskImg);
            mp.setCompositionMode(QPainter::CompositionMode_Source);
            for (int i = 0; i < maskSlices.size(); i++) {
                const int tx = maskTileCoords[i].x(), ty = maskTileCoords[i].y();
                const QByteArray &raw = rawMaskTiles[i];
                QImage tileImg(reinterpret_cast<const uchar*>(raw.constData()),
                                TILE_SIZE, TILE_SIZE, TILE_SIZE * 4, QImage::Format_RGBA8888_Premultiplied);
                const int dstX = tx * TILE_SIZE, dstY = ty * TILE_SIZE;
                const int w = qMin(TILE_SIZE, canvasW - dstX), h = qMin(TILE_SIZE, canvasH - dstY);
                mp.drawImage(QRect(dstX, dstY, w, h), tileImg, QRect(0, 0, w, h));
            }
            mp.end();
        }

        if (layer.layerType == LayerType::SolidColor || layer.layerType == LayerType::Adjustment
            || layer.layerType == LayerType::Filter || layer.layerType == LayerType::Folder) {
            LayerSnapshotData snap{ layer.name, layer.opacity, layer.visible, layer.blendMode,
                             layer.clipping, layer.layerType, layer.adjustment, layer.filter, layer.textBoxes,
                             layer.solidColor, QImage(), layer.childCount, layer.hasMask };
            snap.maskImage = maskImg;
            result.append(snap);
            continue;
        }

        // まずこのレイヤーの全タイルのslice番号(存在するもののみ)とキャンバス上の
        // タイル座標(tx,ty)を集め、readSlicePixelsBatch()でPBOパイプライン化して
        // まとめて読み出す(タイルごとに逐次glReadPixelsするより、GPU→CPU転送の
        // ストールが少ない)。
        QVector<int> slices;
        QVector<QPoint> tileCoords;
        for (int ty = 0; ty < tilesY; ty++) {
            for (int tx = 0; tx < tilesX; tx++) {
                int si = layer.tileSliceAtCanvasTile(tx, ty);
                if (si < 0) continue; // 通常起こらない(レイヤーは常にキャンバス全体を覆う)
                slices.append(si);
                tileCoords.append(QPoint(tx, ty));
            }
        }
        const QVector<QByteArray> rawTiles = readSlicePixelsBatch(slices);

        QImage img(canvasW, canvasH, QImage::Format_RGBA8888_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        for (int i = 0; i < slices.size(); i++) {
            const int tx = tileCoords[i].x(), ty = tileCoords[i].y();
            const QByteArray &raw = rawTiles[i]; // TILE_SIZE*TILE_SIZE*4 RGBA8
            QImage tileImg(reinterpret_cast<const uchar*>(raw.constData()),
                            TILE_SIZE, TILE_SIZE, TILE_SIZE * 4, QImage::Format_RGBA8888_Premultiplied);
            const int dstX = tx * TILE_SIZE, dstY = ty * TILE_SIZE;
            const int w = qMin(TILE_SIZE, canvasW - dstX), h = qMin(TILE_SIZE, canvasH - dstY);
            p.drawImage(QRect(dstX, dstY, w, h), tileImg, QRect(0, 0, w, h));
        }
        p.end();
        LayerSnapshotData snap{ layer.name, layer.opacity, layer.visible, layer.blendMode,
                         layer.clipping, layer.layerType, layer.adjustment, layer.filter, layer.textBoxes,
                         layer.solidColor, img, layer.childCount, layer.hasMask };
        snap.maskImage = maskImg;
        result.append(snap);
    }
    return result;
}

// snaps: 復元するレイヤー内容(各QImageは既にnewW x newHの解像度に合わせて描画済みである必要はなく、
// offsetX,offsetYを使って新キャンバス上の正しい位置に配置される)。
void GLWidget::rebuildCanvasFromSnapshots(int newW, int newH, const QVector<LayerSnapshotData> &snaps,
                                           int activeIndex, int offsetX, int offsetY)
{
    freeTextures();
    canvasW = newW;
    canvasH = newH;
    sliceAllocator_.resetAllocationState();
    doc_->resetToBlank();
    initTextures(/*createDefaultLayers=*/false);
    setupToolContext();

    // 途中経過のシグナル/GL更新は不要なので抑制し、最後にまとめて通知する。
    m_initializing = true;
    const int tilesX = doc_->tilesX(), tilesY = doc_->tilesY();
    for (const LayerSnapshotData &snap : snaps) {
        addLayer(snap.name, /*insertIndex=*/-1, snap.clipping,
                 /*originTx=*/0, /*originTy=*/0, /*tilesXOverride=*/-1, /*tilesYOverride=*/-1,
                 snap.layerType);
        const int newIdx = doc_->layerCount() - 1;
        doc_->setLayerOpacity(newIdx, snap.opacity);
        doc_->setLayerVisible(newIdx, snap.visible);
        doc_->setLayerBlendMode(newIdx, snap.blendMode);
        doc_->layerRef(newIdx).adjustment = snap.adjustment;
        doc_->layerRef(newIdx).filter     = snap.filter;
        doc_->layerRef(newIdx).textBoxes  = snap.textBoxes;
        doc_->layerRef(newIdx).solidColor = snap.solidColor;
        doc_->layerRef(newIdx).childCount = snap.childCount; // Folderのときのみ意味を持つ

        // マスクを持っていた場合、新しいキャンバスサイズ用にタイルを確保し直し、
        // 色レイヤーの中身と同じくoffsetX/offsetYぶんずらして書き戻す(マスクも
        // レイヤー本体と同じ位置関係を保つ必要があるため)。
        if (snap.hasMask && addLayerMask(newIdx)) {
            QImage newMaskImg(newW, newH, QImage::Format_RGBA8888_Premultiplied);
            newMaskImg.fill(Qt::white);
            {
                QPainter mp(&newMaskImg);
                mp.setCompositionMode(QPainter::CompositionMode_Source);
                mp.drawImage(QPoint(-offsetX, -offsetY), snap.maskImage);
            }
            const Layer &maskLayer = doc_->layers[newIdx];
            for (int ty = 0; ty < tilesY; ty++) {
                for (int tx = 0; tx < tilesX; tx++) {
                    int si = maskLayer.maskTiles[ty][tx];
                    QImage tile(TILE_SIZE, TILE_SIZE, QImage::Format_RGBA8888_Premultiplied);
                    tile.fill(Qt::white);
                    {
                        QPainter tp(&tile);
                        tp.setCompositionMode(QPainter::CompositionMode_Source);
                        tp.drawImage(0, 0, newMaskImg, tx * TILE_SIZE, ty * TILE_SIZE, TILE_SIZE, TILE_SIZE);
                    }
                    QByteArray raw(reinterpret_cast<const char*>(tile.constBits()), TILE_SIZE * TILE_SIZE * 4);
                    writeSlicePixels(si, raw);
                }
            }
        }

        // 単色レイヤー/調整レイヤー/フィルターレイヤー/フォルダーはタイルを持たない
        // (単色は手続き的な不透明白、調整とフィルターは下のレイヤーへの加工のみ、
        // フォルダーはUI上のマーカーのみ)ので、ピクセルの書き戻しは不要。
        // テキストレイヤーはNormalと同様に実ピクセルを持つので下の経路で書き戻す。
        if (snap.layerType == LayerType::SolidColor || snap.layerType == LayerType::Adjustment
            || snap.layerType == LayerType::Filter || snap.layerType == LayerType::Folder)
            continue;

        QImage newImg(newW, newH, QImage::Format_RGBA8888_Premultiplied);
        newImg.fill(Qt::transparent);
        {
            QPainter p(&newImg);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(QPoint(-offsetX, -offsetY), snap.image);
        }

        const Layer &newLayer = doc_->layers[newIdx];
        for (int ty = 0; ty < tilesY; ty++) {
            for (int tx = 0; tx < tilesX; tx++) {
                int si = newLayer.tiles[ty][tx];
                QImage tile(TILE_SIZE, TILE_SIZE, QImage::Format_RGBA8888_Premultiplied);
                tile.fill(Qt::transparent);
                {
                    QPainter tp(&tile);
                    tp.setCompositionMode(QPainter::CompositionMode_Source);
                    tp.drawImage(0, 0, newImg, tx * TILE_SIZE, ty * TILE_SIZE, TILE_SIZE, TILE_SIZE);
                }
                QByteArray raw(reinterpret_cast<const char*>(tile.constBits()), TILE_SIZE * TILE_SIZE * 4);
                writeSlicePixels(si, raw);
            }
        }
    }
    m_initializing = false;

    if (doc_->layerCount() > 0)
        doc_->setActiveLayer(qBound(0, activeIndex, doc_->layerCount() - 1));

    // 大量のglTexSubImage3D書き込み(writeSlicePixels)がGPU側でまだ実行中のうちに
    // 次のフレームのpaintGL()がこのlayerTexArrayを読みに行くと、タイミング次第で
    // ドライバ側の状態が不整合になりクラッシュしうる。update()で再描画を要求する前に
    // ここまでの書き込みを確実に完了させる。
    glFinish();

    fitCanvasToView();
    emit layersChanged();
    emit selectionChanged(hasSelection_); // initTextures()内でhasSelection_=falseにリセット済み
    update();
}

void GLWidget::applyCanvasResizeUndoEntry(const UndoEntry &entry, bool toBefore)
{
    const CanvasResizeUndoData &rd = entry.resize;
    if (toBefore) {
        rebuildCanvasFromSnapshots(rd.oldW, rd.oldH, rd.beforeLayers, rd.activeLayerIndexBefore, 0, 0);
        // rebuildCanvasFromSnapshots内のresetToBlank()でredoStackも失われるため、
        // このリサイズ自身をredoできるよう積み直す
        doc_->restoreRedoEntryAfterRebuild(entry);
    } else {
        rebuildCanvasFromSnapshots(rd.newW, rd.newH, rd.afterLayers, rd.activeLayerIndexAfter, 0, 0);
        doc_->restoreUndoEntryAfterRebuild(entry);
    }
}

bool GLWidget::resizeCanvasKeepingContent(int newW, int newH, int offsetX, int offsetY)
{
    if (newW <= 0 || newH <= 0) return false;
    if (!doc_ || doc_->layers.isEmpty()) return false;

    makeCurrent();

    const int oldW = canvasW, oldH = canvasH;
    const QVector<LayerSnapshotData> beforeLayers = captureAllLayerSnapshots();
    const int savedActiveIndex = doc_->activeLayerIndex();

    rebuildCanvasFromSnapshots(newW, newH, beforeLayers, savedActiveIndex, offsetX, offsetY);

    // Undo用に、変更後の状態も丸ごとスナップショットしておく
    // (低頻度の操作なので、GPUから読み直すシンプルな実装で構わない)
    UndoEntry entry;
    entry.kind = UndoKind::CanvasResize;
    entry.resize.oldW = oldW;
    entry.resize.oldH = oldH;
    entry.resize.newW = newW;
    entry.resize.newH = newH;
    entry.resize.beforeLayers = beforeLayers;
    entry.resize.afterLayers  = captureAllLayerSnapshots();
    entry.resize.activeLayerIndexBefore = qBound(0, savedActiveIndex, beforeLayers.size() - 1);
    entry.resize.activeLayerIndexAfter  = doc_->activeLayerIndex();
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());

    return true;
}

// ===========================================================================
// キャンバスの回転・反転(編集メニュー「ファイル」の回転/反転コマンド)
// ---------------------------------------------------------------------------
// 見た目(ViewTransform)ではなく、全レイヤーの実ピクセルデータを書き換える。
// resizeCanvasKeepingContent/resampleCanvasResolutionと同じ「スナップショット
// 取得→各レイヤーのQImageを変換→rebuildCanvasFromSnapshotsで焼き直す→
// CanvasResize Undoエントリを積む」パターンを流用する。
// ===========================================================================
bool GLWidget::rotateCanvas(int ccwDegrees)
{
    if (ccwDegrees != 90 && ccwDegrees != 180 && ccwDegrees != 270) return false;
    if (!doc_ || doc_->layers.isEmpty()) return false;

    // レイヤー構成を丸ごと作り直す破壊的な操作なので、他の編集アクションは
    // 実行前に確定せずキャンセルする(他のstart*Action系と同じ排他パターン)。
    actions_.cancelActive(); // 変形/色調整/フィルター/レイヤー編集系(controller管理アクション)は互いに排他

    makeCurrent();

    const int oldW = canvasW, oldH = canvasH;
    const bool swapDims = (ccwDegrees == 90 || ccwDegrees == 270);
    const int newW = swapDims ? oldH : oldW;
    const int newH = swapDims ? oldW : oldH;

    const QVector<LayerSnapshotData> beforeLayers = captureAllLayerSnapshots();
    const int savedActiveIndex = doc_->activeLayerIndex();

    // タイル格納(≒QImage)はY下向きの通常の画像座標系を採用しているため、
    // QTransform::rotate()に正の角度を渡すと画面上は時計回りになる。反時計回り
    // にするため符号を反転する。
    QTransform t;
    t.rotate(-ccwDegrees);
    QVector<LayerSnapshotData> rotatedLayers = beforeLayers;
    for (LayerSnapshotData &snap : rotatedLayers)
        if (!snap.image.isNull())
            snap.image = snap.image.transformed(t);

    rebuildCanvasFromSnapshots(newW, newH, rotatedLayers, savedActiveIndex, 0, 0);

    UndoEntry entry;
    entry.kind = UndoKind::CanvasResize;
    entry.resize.oldW = oldW;
    entry.resize.oldH = oldH;
    entry.resize.newW = newW;
    entry.resize.newH = newH;
    entry.resize.beforeLayers = beforeLayers;
    entry.resize.afterLayers  = captureAllLayerSnapshots();
    entry.resize.activeLayerIndexBefore = qBound(0, savedActiveIndex, beforeLayers.size() - 1);
    entry.resize.activeLayerIndexAfter  = doc_->activeLayerIndex();
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());

    return true;
}

bool GLWidget::flipCanvasHorizontal()
{
    return flipCanvasBy(true, false);
}

bool GLWidget::flipCanvasVertical()
{
    return flipCanvasBy(false, true);
}

bool GLWidget::flipCanvasBy(bool horizontal, bool vertical)
{
    if (!doc_ || doc_->layers.isEmpty()) return false;

    // レイヤー構成を丸ごと作り直す破壊的な操作なので、他の編集アクションは
    // 実行前に確定せずキャンセルする(他のstart*Action系と同じ排他パターン)。
    actions_.cancelActive(); // 変形/色調整/フィルター/レイヤー編集系(controller管理アクション)は互いに排他

    makeCurrent();

    const QVector<LayerSnapshotData> beforeLayers = captureAllLayerSnapshots();
    const int savedActiveIndex = doc_->activeLayerIndex();

    QVector<LayerSnapshotData> flippedLayers = beforeLayers;
    for (LayerSnapshotData &snap : flippedLayers)
        if (!snap.image.isNull()) snap.image = snap.image.mirrored(horizontal, vertical);

    rebuildCanvasFromSnapshots(canvasW, canvasH, flippedLayers, savedActiveIndex, 0, 0);

    UndoEntry entry;
    entry.kind = UndoKind::CanvasResize;
    entry.resize.oldW = canvasW;
    entry.resize.oldH = canvasH;
    entry.resize.newW = canvasW;
    entry.resize.newH = canvasH;
    entry.resize.beforeLayers = beforeLayers;
    entry.resize.afterLayers  = captureAllLayerSnapshots();
    entry.resize.activeLayerIndexBefore = qBound(0, savedActiveIndex, beforeLayers.size() - 1);
    entry.resize.activeLayerIndexAfter  = doc_->activeLayerIndex();
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());

    return true;
}

// ===========================================================================
// 画像解像度変更(編集アクション「画像解像度変更」の確定処理)
// ---------------------------------------------------------------------------
// キャンバスサイズ変更(resizeCanvasKeepingContent)と違い、内容を新キャンバス上の
// 同じ位置に置くのではなく、新しい解像度いっぱいに拡大縮小(リサンプル)して置く
// (見た目の縦横比・表示上のサイズは変えず、ピクセル密度だけを変える)。
// Undoの仕組みはキャンバスサイズ変更と全く同じ CanvasResize エントリを流用する
// (beforeLayers/afterLayersが自己完結した全体スナップショットなので、
// 「配置方法が offset か 拡大縮小 か」の違いはUndo/Redo適用時には関係ない)。
// ===========================================================================
bool GLWidget::resampleCanvasResolution(int newW, int newH)
{
    if (newW <= 0 || newH <= 0) return false;
    if (!doc_ || doc_->layers.isEmpty()) return false;

    makeCurrent();

    const int oldW = canvasW, oldH = canvasH;
    const QVector<LayerSnapshotData> beforeLayers = captureAllLayerSnapshots();
    const int savedActiveIndex = doc_->activeLayerIndex();

    // 各レイヤーの内容を新しい解像度いっぱいに拡大縮小してから配置する
    // (offsetX=offsetY=0で、既に新サイズちょうどに合わせた画像をそのまま貼るだけでよい)
    QVector<LayerSnapshotData> scaledLayers = beforeLayers;
    for (LayerSnapshotData &snap : scaledLayers)
        snap.image = snap.image.scaled(newW, newH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    rebuildCanvasFromSnapshots(newW, newH, scaledLayers, savedActiveIndex, 0, 0);

    UndoEntry entry;
    entry.kind = UndoKind::CanvasResize;
    entry.resize.oldW = oldW;
    entry.resize.oldH = oldH;
    entry.resize.newW = newW;
    entry.resize.newH = newH;
    entry.resize.beforeLayers = beforeLayers;
    entry.resize.afterLayers  = captureAllLayerSnapshots();
    entry.resize.activeLayerIndexBefore = qBound(0, savedActiveIndex, beforeLayers.size() - 1);
    entry.resize.activeLayerIndexAfter  = doc_->activeLayerIndex();
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());

    return true;
}