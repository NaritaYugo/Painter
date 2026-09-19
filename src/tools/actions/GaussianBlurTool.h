#pragma once
#include "tools/core/ToolContext.h"
#include <QOpenGLFunctions_4_3_Core>

// ---------------------------------------------------------------------------
// GaussianBlurTool
// ---------------------------------------------------------------------------
// フィルターメニュー「ガウスぼかし」アクション本体。ColorBalanceTool等の色調整系と
// 違い、ぼかしは近傍サンプリングが必要で毎フレームのシェーダー内計算では重すぎる。
// そのため activate() 時にアクティブレイヤーを ctx.fullLayerTex へ集約した上で、
// 半径が変わるたびに gaussianBlurFilter.comp をディスパッチして
// ctx.transformSrcTex へプレビュー結果を焼く(2次元ガウスは分離可能なので
// 「横1D → 縦1D」の2パス。詳細はそのシェーダーのコメント参照)。render.frag 側はこの
// transformSrcTex をそのままサンプルして表示するだけなので、毎フレームのコストは
// 通常のテクスチャ参照と変わらない。
// confirm() 時は、その時点の transformSrcTex の内容をそのままタイルへ書き戻すだけ
// でよい(レイヤー本体のGPUピクセルはconfirmするまで一切変更しない=キャンセルは
// 常に安全な no-op)。
// ---------------------------------------------------------------------------
class GaussianBlurTool : protected QOpenGLFunctions_4_3_Core
{
public:
    void initialize(QOpenGLContext *ctx);

    void activate(ToolContext &ctx);   // アクション開始: レイヤーを集約し、既定半径でプレビューを1回作る
    void deactivate();                 // キャンセル(GPU上のレイヤー本体には一切書き込まない)
    void confirm(ToolContext &ctx);    // プレビュー結果をタイルへ焼き込んで終了する

    bool engaged() const { return engaged_; }

    void setRadius(ToolContext &ctx, int radius); // 半径変更 → その場でプレビュー再計算
    int  radius() const { return radius_; }

    // render.frag用: プレビュー結果(ctx.transformSrcTex)がキャンバス座標系のどこに
    // 対応するか(レイヤー原点のピクセル座標とサイズ)。
    QVector2D previewOriginPx() const { return previewOriginPx_; }
    QVector2D previewSizePx()   const { return QVector2D((float)layerW_, (float)layerH_); }

private:
    bool engaged_ = false;
    int  radius_  = 8; // px
    int  layerW_ = 0, layerH_ = 0; // activate()時に集約したレイヤーのピクセルサイズ
    int  layerIndex_ = -1;
    QVector2D previewOriginPx_;

    void updatePreview(ToolContext &ctx);
};
