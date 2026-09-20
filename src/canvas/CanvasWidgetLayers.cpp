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

#include "canvas/CanvasWidget.h"
#include "app/NativeWindowLog.h" // 診断ログ(TIEPOLO_WINLOG)。初期化時間の計測に使う
#include "rendering/ShaderCache.h"
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
#include "licensing/LicenseManager.h"
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

// Layer/resource operations and layer-oriented editing.

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

bool CanvasWidget::addLayer(const QString &name, int insertIndex, bool clipping,
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

bool CanvasWidget::addSolidColorLayer(const QString &name, int insertIndex, const QColor &color,
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

bool CanvasWidget::addLayerMask(int layerIndex)
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

bool CanvasWidget::removeLayerMask(int layerIndex)
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

void CanvasWidget::setEditingMaskLayer(int layerIndex)
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
    emit layersChanged(); // LayerDockのマスクプレビューと不透明度表示を更新させる
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
GLuint CanvasWidget::ensureStrokeColorTex()
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
bool CanvasWidget::setPenTipImage(const QString &path)
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
bool CanvasWidget::setPaperTexture(const QString &path)
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

void CanvasWidget::applyDisplayConfig()
{
    update();
}

// PNG/JPEG/BMP等の1枚の画像ファイルを、キャンバス全面を覆う1枚の通常レイヤーとして
// 取り込む。PsdCodec::load()内のaddFlattenedNormalLayer(フラット画像PSDの取り込み)
// と同じパターン(標準画像の行順=原点左上・Y下向きを、タイル格納が期待する
// 行順=原点左下・Y上向きに変換するため上下反転してから書き込む)。
bool CanvasWidget::loadImageAsSingleLayer(const QImage &image, const QString &layerName)
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
bool CanvasWidget::insertImageLayerAboveActive(const QImage &image, const QString &layerName)
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

void CanvasWidget::beginBulkLayerImport()
{
    m_initializing = true;
}

void CanvasWidget::endBulkLayerImport()
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
void CanvasWidget::beginLayerAddUndo()
{
    if (!doc_) return;
    pendingLayerAddActiveBefore_ = doc_->activeLayerIndex();
}

void CanvasWidget::commitLayerAddUndo(int insertedIndex, const QVector<int> &ancestorFolders,
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

void CanvasWidget::abortLayerAddUndo()
{
    pendingLayerAddActiveBefore_ = -1;
}

// toBefore=true : 追加を取り消す(そのレイヤーを削除する)
// toBefore=false: 追加をやり直す(同じ設定で作り直す)
void CanvasWidget::applyLayerAddUndoEntry(const UndoEntry &entry, bool toBefore)
{
    if (!doc_) return;
    const LayerAddUndoData &d = entry.layerAdd;
    makeCurrent();

    if (toBefore) {
        if (d.insertIndex < 0 || d.insertIndex >= doc_->layerCount()) return;
        // CanvasWidget::removeLayer(公開版)ではなくdoc_を直接触る。公開版は自分で
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
RemovedLayerData CanvasWidget::captureRemovedLayer(int layerIndex)
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
bool CanvasWidget::restoreRemovedLayer(const RemovedLayerData &d, int insertIndex,
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
void CanvasWidget::applyLayerRemoveUndoEntry(const UndoEntry &entry, bool toBefore)
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

bool CanvasWidget::removeLayer(int layerIndex, const QVector<int> &ancestorFolders, bool includeContents) {
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

int CanvasWidget::mergeLayers(int survivorIndex, int victimIndex, const QVector<int> &ancestorFolders)
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
void CanvasWidget::applyLayerMergeUndoEntry(const UndoEntry &entry, bool toBefore)
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
int CanvasWidget::mergeLayersInternal(int survivorIndex, int victimIndex,
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

int CanvasWidget::duplicateLayer(int sourceIndex, int insertIndex, const QVector<int> &ancestorFolders)
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

void CanvasWidget::cutSelection()
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

void CanvasWidget::nudgeActiveContent(int dx, int dy)
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
void CanvasWidget::nudgeActiveLayer(int dx, int dy)
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
QImage CanvasWidget::getLayerPreview(int zStart, int zEnd, int size) {
    makeCurrent();
    return compositor_.renderLayerPreview(toolCtx_, zStart, zEnd, size);
}

QImage CanvasWidget::getNavigatorPreview(int size, bool fullCanvas)
{
    makeCurrent();
    return compositor_.renderNavigatorPreview(toolCtx_, size, fullCanvas);
}

QImage CanvasWidget::getMaskPreview(int layerIndex, int size)
{
    if (!doc_ || layerIndex < 0 || layerIndex >= doc_->layerCount()) return QImage();
    if (!doc_->layers[layerIndex].hasMask) return QImage();
    makeCurrent();
    return compositor_.renderMaskPreview(toolCtx_, layerIndex, size);
}

// ===========================================================================
// 塗りつぶし
// ===========================================================================
void CanvasWidget::executeFill(const QPointF &widgetPos, float wallThreshold)
{
    fillTool_.execute(toolCtx_, widgetPos);
}

// ===========================================================================
// Undo / Redo
// ===========================================================================
