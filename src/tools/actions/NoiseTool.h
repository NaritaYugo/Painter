#pragma once
#include "tools/core/ToolContext.h"
#include <QOpenGLFunctions_4_3_Core>

// ---------------------------------------------------------------------------
// NoiseTool
// ---------------------------------------------------------------------------
// フィルターメニュー「ノイズ」アクション本体。GaussianBlurTool/MosaicTool と全く
// 同じ構造(activate()時にアクティブレイヤーを ctx.fullLayerTex へ集約し、パラメータが
// 変わるたびに noiseFilter.comp を1回ディスパッチして ctx.transformSrcTex へプレビューを
// 焼く。render.frag はそれをサンプルするだけ。confirm()でタイルへ書き戻し、
// キャンセルはレイヤー本体に触れない)。
//
// 絵にざらざらした質感(フィルムグレイン)を足すフィルターで、キャンバス上のハンドルは
// 持たないためマウス横取りは不要(パネルのみで完結する)。
// ---------------------------------------------------------------------------
class NoiseTool : protected QOpenGLFunctions_4_3_Core
{
public:
    void initialize(QOpenGLContext *ctx);

    void activate(ToolContext &ctx);
    void deactivate();
    void confirm(ToolContext &ctx);

    bool engaged() const { return engaged_; }

    // パラメータ変更 → その場でプレビュー再計算
    void setStrength(ToolContext &ctx, float strength01); // 0..1(パネルの0-100%/100)
    void setMonochrome(ToolContext &ctx, bool mono);
    void setGrainPx(ToolContext &ctx, float grainPx);

    float strength()   const { return strength_; }
    bool  monochrome() const { return monochrome_; }
    float grainPx()    const { return grainPx_; }

    QVector2D previewOriginPx() const { return previewOriginPx_; }
    QVector2D previewSizePx()   const { return QVector2D((float)layerW_, (float)layerH_); }

private:
    bool  engaged_ = false;
    float strength_ = 0.25f;
    bool  monochrome_ = true; // 既定は明暗だけが揺れる古典的なフィルムグレイン
    float grainPx_ = 1.0f;
    unsigned int seed_ = 0;   // activate()時に1度だけ決める(スライダー操作中に粒が動かないように)
    int  layerW_ = 0, layerH_ = 0;
    int  layerIndex_ = -1;
    QVector2D previewOriginPx_;

    void updatePreview(ToolContext &ctx);
};
