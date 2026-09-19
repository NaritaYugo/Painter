#pragma once

#include "tools/core/ToolContext.h"
#include <functional>

// ---------------------------------------------------------------------------
// CanvasCompositor
// ---------------------------------------------------------------------------
// CanvasDocumentの現在の状態を読んで、共有テクスチャ/SSBOを更新したり
// 合成結果を書き出したりする。内部状態は持たない
// (読み書きする対象はすべてGLWidgetが所有する共有GL資源)。
//
// 合成アルゴリズム(Photoshop準拠のクリッピングマスク、両シェーダー共通):
//   レイヤーを下から上へ順に処理し、非クリッピングレイヤーに出会うたびに
//   そのレイヤーの(不透明度適用後の)色を「クリップ基準」として記録する。
//   clipping=true のレイヤーは、そのクリップ基準のアルファでマスクしてから
//   通常通りブレンドする(基準が非表示なら基準アルファ0=クリップ側も透明になる)。
// ---------------------------------------------------------------------------
class CanvasCompositor
{
public:
    // render.frag 用のレイヤーuniform+SSBOをセットする(毎フレーム呼ぶ想定)
    void setLayerUniformsForRender(ToolContext &ctx, QOpenGLShaderProgram *prog);

    // composite.comp 用のレイヤーuniformをセットする(tx,ty はタイル位置)。
    // SSBOは呼び出し側が事前に updateLayerSSBOs() で更新しておくこと。
    void setLayerUniforms(ToolContext &ctx, QOpenGLShaderProgram *prog, int tx, int ty);

    // レイヤーのopacity/visible/blendMode/clipping/baseSliceをSSBOへ書き込む
    void updateLayerSSBOs(ToolContext &ctx);

    // ------------------------------------------------------------------
    // プレビュー / 書き出し
    // ------------------------------------------------------------------
    // 指定レイヤーindex範囲(下から数えたz、両端含む)を合成して size×size のサムネイルを作る
    // (レイヤーパネルのプレビュー用)
    QImage renderLayerPreview(ToolContext &ctx, int zStart, int zEnd, int size);

    // ナビゲーター用プレビュー。fullCanvas=true なら全レイヤー合成(compositedTexを再利用/更新)、
    // false ならアクティブレイヤー単体を縮小する
    QImage renderNavigatorPreview(ToolContext &ctx, int size, bool fullCanvas);

    // 指定レイヤーのマスク濃淡(キャンバス全体を覆う)を size に収まるよう縮小した
    // グレースケールプレビューを返す。不透明度プレビューUIが表示に使う。
    QImage renderMaskPreview(ToolContext &ctx, int layerIndex, int size);

    // ナビゲーター用: 全レイヤーを合成して compositedTex (単一2Dテクスチャ) を更新する
    // 重いので頻繁には呼ばない
    void updateCompositedTex(ToolContext &ctx);

    // 全レイヤーを合成してキャンバスサイズのQImageとして書き出す
    QImage renderExport(ToolContext &ctx);

    // ------------------------------------------------------------------
    // タイル走査の共通化
    // ------------------------------------------------------------------
    // doc.tilesX() x doc.tilesY() の範囲を走査してfnを呼ぶ(このファイル内で
    // 4箇所ほぼ同じ形をしていた「for(ty) for(tx)」を集約したもの)。
    // 各タイルで実際に何をするかは呼び出し側のfnに委ねる。
    // 将来のキャンバスループ機能では、ここに折り返し分の走査を足すだけで
    // 4つの呼び出し元すべてに対応できるようにするための choke point。
    using TileVisitFn = std::function<void(int tx, int ty)>;
    void forEachCanvasTile(const CanvasDocument &doc, const TileVisitFn &fn);

private:
    // renderLayerPreview()の軽量パス。zStart==zEnd(単一レイヤーのプレビュー、
    // LayerDockのサムネイル用途はすべてこれ)のとき、composite.compによる
    // キャンバス全面ぶんのcompute dispatchを経由せず、そのレイヤー自身のタイルを
    // 直接縮小blitして作る(他レイヤーとの合成が絡まないため、汎用パスは不要)。
    QImage renderSingleLayerPreviewLight(ToolContext &ctx, int z, int size);
};
