#pragma once

#include "tools/core/ToolContext.h"
#include <functional>

// CanvasCompositor
class CanvasCompositor
{
public:
    // render.frag 用のレイヤーuniform+SSBOをセットする(毎フレーム呼ぶ想定)。
    void setLayerUniformsForRender(ToolContext &ctx, QOpenGLShaderProgram *prog);

    // composite.comp 用のレイヤーuniformをセットする(tx,ty はタイル位置)。
    void setLayerUniforms(ToolContext &ctx, QOpenGLShaderProgram *prog, int tx, int ty);

    // レイヤーのopacity/visible/blendMode/clipping/baseSliceをSSBOへ書き込む。
    void updateLayerSSBOs(ToolContext &ctx);

    // プレビュー / 書き出し
    QImage renderLayerPreview(ToolContext &ctx, int zStart, int zEnd, int size);

    // ナビゲーター用プレビュー。
    QImage renderNavigatorPreview(ToolContext &ctx, int size, bool fullCanvas);

    // 指定レイヤーのマスク濃淡(キャンバス全体を覆う)を size に収まるよう縮小したグレースケールプレビューを返す。
    QImage renderMaskPreview(ToolContext &ctx, int layerIndex, int size);

    // ナビゲーター用: 全レイヤーを合成して compositedTex (単一2Dテクスチャ) を更新する。
    void updateCompositedTex(ToolContext &ctx);

    // 全レイヤーを合成してキャンバスサイズのQImageとして書き出す。
    QImage renderExport(ToolContext &ctx);

    // タイル走査の共通化
    using TileVisitFn = std::function<void(int tx, int ty)>;
    void forEachCanvasTile(const CanvasDocument &doc, const TileVisitFn &fn);

private:
    // renderLayerPreview()の軽量パス。
    QImage renderSingleLayerPreviewLight(ToolContext &ctx, int z, int size);
};
