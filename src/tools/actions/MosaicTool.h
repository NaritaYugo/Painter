#pragma once
#include "tools/core/ToolContext.h"
#include <QOpenGLFunctions_4_3_Core>

// ---------------------------------------------------------------------------
// MosaicTool
// ---------------------------------------------------------------------------
// フィルターメニュー「モザイク」アクション本体。GaussianBlurToolと全く同じ構造・
// 理由(ブロック平均のため近傍サンプリングが必要で、毎フレームのシェーダー内計算
// では重すぎる)で、activate()時にアクティブレイヤーを ctx.fullLayerTex へ集約した
// 上で、ブロックサイズが変わるたびに mosaicReduce.comp(ブロックごとの平均色を
// 1テクセルへ集約)→ mosaicFilter.comp(それを引いて仕上げる)の2パスを
// ディスパッチして ctx.transformSrcTex へプレビュー結果を焼く。render.frag側はそれをそのまま
// サンプルして表示するだけ。confirm()時はプレビュー結果をタイルへ書き戻すだけで、
// キャンセルはレイヤー本体に一切触れないので常に安全。
// ---------------------------------------------------------------------------
class MosaicTool : protected QOpenGLFunctions_4_3_Core
{
public:
    void initialize(QOpenGLContext *ctx);

    void activate(ToolContext &ctx);   // アクション開始: レイヤーを集約し、既定ブロックサイズでプレビューを1回作る
    void deactivate();                 // キャンセル(GPU上のレイヤー本体には一切書き込まない)
    void confirm(ToolContext &ctx);    // プレビュー結果をタイルへ焼き込んで終了する

    bool engaged() const { return engaged_; }

    void setBlockSize(ToolContext &ctx, int blockSize); // ブロックサイズ変更 → その場でプレビュー再計算
    int  blockSize() const { return blockSize_; }

    // render.frag用: プレビュー結果(ctx.transformSrcTex)がキャンバス座標系のどこに
    // 対応するか(レイヤー原点のピクセル座標とサイズ)。
    QVector2D previewOriginPx() const { return previewOriginPx_; }
    QVector2D previewSizePx()   const { return QVector2D((float)layerW_, (float)layerH_); }

private:
    bool engaged_ = false;
    int  blockSize_ = 16; // px
    int  layerW_ = 0, layerH_ = 0; // activate()時に集約したレイヤーのピクセルサイズ
    int  layerIndex_ = -1;
    QVector2D previewOriginPx_;

    void updatePreview(ToolContext &ctx);
};
