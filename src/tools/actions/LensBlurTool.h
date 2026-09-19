#pragma once
#include "tools/core/ToolContext.h"
#include <QOpenGLFunctions_4_3_Core>

// ---------------------------------------------------------------------------
// LensBlurTool (Pro限定)
// ---------------------------------------------------------------------------
// フィルターメニュー「レンズぼかし」アクション本体。GaussianBlurTool/MosaicTool/
// ChromaticAberrationTool と全く同じ構造(activate()時にアクティブレイヤーを
// ctx.fullLayerTex へ集約し、パラメータが変わるたびに lensBlurFilter.comp を1回
// ディスパッチして ctx.transformSrcTex へプレビューを焼く。render.frag はそれを
// サンプルするだけ。confirm()でタイルへ書き戻し、キャンセルはレイヤー本体に触れない)。
//
// 深度マップは扱わず、レイヤー全体へ一様に「絞りの形」のぼけを与える。玉ボケを
// 出すための一様円板カーネル+ハイライト強調の理屈は lensBlurFilter.comp のコメント参照。
// 中心ハンドルの類は持たないので、マウス横取りは不要(パネルのみで完結する)。
// ---------------------------------------------------------------------------
class LensBlurTool : protected QOpenGLFunctions_4_3_Core
{
public:
    void initialize(QOpenGLContext *ctx);

    void activate(ToolContext &ctx);
    void deactivate();
    void confirm(ToolContext &ctx);

    bool engaged() const { return engaged_; }

    // パラメータ変更 → その場でプレビュー再計算
    void setRadiusPx(ToolContext &ctx, float radiusPx);
    void setBlades(ToolContext &ctx, int blades);              // 0=円, 3以上=正多角形の辺数
    void setBladeRotDeg(ToolContext &ctx, float rotDeg);
    void setHighlightBoost(ToolContext &ctx, float boost);     // 0..1 の正規化値(パネルの0-100/100)
    void setThreshold(ToolContext &ctx, float threshold);      // 0..1

    float radiusPx()       const { return radiusPx_; }
    int   blades()         const { return blades_; }
    float bladeRotDeg()    const { return bladeRotDeg_; }
    float highlightBoost() const { return highlightBoost_; }
    float threshold()      const { return threshold_; }

    QVector2D previewOriginPx() const { return previewOriginPx_; }
    QVector2D previewSizePx()   const { return QVector2D((float)layerW_, (float)layerH_); }

private:
    bool  engaged_ = false;
    float radiusPx_ = 16.0f;
    int   blades_ = 0;            // 既定は円形絞り
    float bladeRotDeg_ = 0.0f;
    float highlightBoost_ = 0.6f; // 既定でもそれなりに玉ボケが見えるようにしておく
    float threshold_ = 0.7f;
    int  layerW_ = 0, layerH_ = 0;
    int  layerIndex_ = -1;
    QVector2D previewOriginPx_;

    void updatePreview(ToolContext &ctx);
    // 半径に見合ったスパイラルのサンプル数(円板が埋まる程度を確保しつつ上限で頭打ち)
    int  sampleCount() const;
    // ハイライト強調の重み付けと対になるγ(パネルには出さず boost から決める)
    float bokehGamma() const;
};
