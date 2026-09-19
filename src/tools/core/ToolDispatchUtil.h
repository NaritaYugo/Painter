#pragma once

#include "backend/CanvasDocument.h"
#include <functional>

// ---------------------------------------------------------------------------
// forEachLayerTileInRange
// ---------------------------------------------------------------------------
// 指定レイヤーの、キャンバスタイル範囲[txMin,txMax]x[tyMin,tyMax](両端含む)を
// 走査し、各タイルについて layerTexArray 中のスライス番号siと、キャンバス座標系
// での書き込み/読み込み先矩形(dstX,dstY,w,h。端タイルはw/hがtileSize未満に
// クランプされる)を求めてfnを呼ぶ。si<0(通常起こらない。レイヤーは常に
// キャンバス全体を覆う)ならスキップする。
//
// ColorBalanceTool/ToneCurveTool/HueSatLightTool/BrightnessContrastToolの
// 「アクティブレイヤー全体をfullLayerTexへ集める/書き戻す」ループと、
// BlurTool/WarpTool/PenEraserToolの同種コピーループ(範囲がキャンバス全体
// ではなくブラシ影響範囲に限定される点のみ異なる)を集約したもの。
// ---------------------------------------------------------------------------
using CanvasLayerTileFn = std::function<void(int tx, int ty, int si, int dstX, int dstY, int w, int h)>;

inline void forEachLayerTileInRange(const Layer &layer,
                                     int canvasW, int canvasH, int tileSize,
                                     int txMin, int txMax, int tyMin, int tyMax,
                                     const CanvasLayerTileFn &fn,
                                     bool useMask = false)
{
    for (int ty = tyMin; ty <= tyMax; ty++) {
        for (int tx = txMin; tx <= txMax; tx++) {
            int si = useMask ? layer.maskTileSlice(tx, ty) : layer.tileSliceAtCanvasTile(tx, ty);
            if (si < 0) continue;
            int dstX = tx * tileSize, dstY = ty * tileSize;
            int w = qMin(tileSize, canvasW - dstX);
            int h = qMin(tileSize, canvasH - dstY);
            fn(tx, ty, si, dstX, dstY, w, h);
        }
    }
}

// アクティブレイヤー全体(キャンバス全タイル)を走査する便利版。
// ColorBalance/ToneCurve/HueSatLight/BrightnessContrast の各Toolが使う。
inline void forEachActiveLayerTile(const CanvasDocument &doc, const Layer &layer,
                                    int canvasW, int canvasH, int tileSize,
                                    const CanvasLayerTileFn &fn)
{
    forEachLayerTileInRange(layer, canvasW, canvasH, tileSize,
                             0, doc.tilesX() - 1, 0, doc.tilesY() - 1, fn);
}
