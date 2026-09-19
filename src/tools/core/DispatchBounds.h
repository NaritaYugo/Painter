#pragma once

#include <QVector2D>
#include <QVector>
#include <QtGlobal>
#include <cmath>

// ---------------------------------------------------------------------------
// computeStrokeDispatchBounds
// ---------------------------------------------------------------------------
// ブラシストローク(区間from->to、半径radius)が影響しうる範囲を、キャンバス/タイル
// グリッドへクランプして求める。BlurTool::applyBlur / WarpTool::applyWarp で
// ほぼ丸ごと重複していたクランプ計算(ピクセルbbox→書き込みタイル範囲→
// マージン(カーネル半径等)込みの収集タイル範囲→タイル整列済み領域)を1箇所に
// まとめたもの。PenEraserTool::dispatchStrokeSegment の書き込みタイル範囲
// (Undo領域)計算もこれと数値的に同じ結果になるため、wTxMin等はそちらからも
// margin=0で流用できる。
//
// 将来のキャンバスループ機能では、この関数が「クランプ」ではなく「周回」を
// 行うようになる(呼び出し側のBlur/Warp/PenEraserは変更不要)想定の choke point。
// ---------------------------------------------------------------------------
struct StrokeDispatchBounds {
    int pxMinX = 0, pxMaxX = -1; // クランプ済みピクセルbbox(両端含む。pxMaxX<pxMinXなら範囲なし)
    int pxMinY = 0, pxMaxY = -1;

    // クランプ前の生ピクセルbbox(両端含む。キャンバス範囲外(負値/canvasW,H以上)もあり得る)。
    // ラップ時に「反対側へどれだけはみ出したか」を判定するために使う。
    int rawPxMinX = 0, rawPxMaxX = -1;
    int rawPxMinY = 0, rawPxMaxY = -1;

    int wTxMin = 0, wTxMax = -1; // 書き込み対象タイル範囲(両端含む)
    int wTyMin = 0, wTyMax = -1;

    int gTxMin = 0, gTxMax = -1; // margin込みの収集タイル範囲(両端含む)
    int gTyMin = 0, gTyMax = -1;

    int regionMinX = 0, regionMinY = 0; // wTx/wTyをタイル境界に整列したピクセル領域
    int regionMaxX = -1, regionMaxY = -1;

    bool empty() const { return pxMaxX < pxMinX || pxMaxY < pxMinY; }
};

inline StrokeDispatchBounds computeStrokeDispatchBounds(
    int canvasW, int canvasH, int tileSize, int tilesX, int tilesY,
    const QVector2D &from, const QVector2D &to, float radius, int margin)
{
    StrokeDispatchBounds b;

    b.rawPxMinX = (int)std::floor(qMin(from.x(), to.x()) - radius);
    b.rawPxMaxX = (int)std::ceil (qMax(from.x(), to.x()) + radius);
    b.rawPxMinY = (int)std::floor(qMin(from.y(), to.y()) - radius);
    b.rawPxMaxY = (int)std::ceil (qMax(from.y(), to.y()) + radius);

    b.pxMinX = qBound(0, b.rawPxMinX, canvasW - 1);
    b.pxMaxX = qBound(0, b.rawPxMaxX, canvasW - 1);
    b.pxMinY = qBound(0, b.rawPxMinY, canvasH - 1);
    b.pxMaxY = qBound(0, b.rawPxMaxY, canvasH - 1);
    if (b.empty()) return b;

    b.wTxMin = qBound(0, b.pxMinX / tileSize, tilesX - 1);
    b.wTxMax = qBound(0, b.pxMaxX / tileSize, tilesX - 1);
    b.wTyMin = qBound(0, b.pxMinY / tileSize, tilesY - 1);
    b.wTyMax = qBound(0, b.pxMaxY / tileSize, tilesY - 1);

    b.gTxMin = qBound(0, (b.pxMinX - margin) / tileSize, tilesX - 1);
    b.gTxMax = qBound(0, (b.pxMaxX + margin) / tileSize, tilesX - 1);
    b.gTyMin = qBound(0, (b.pxMinY - margin) / tileSize, tilesY - 1);
    b.gTyMax = qBound(0, (b.pxMaxY + margin) / tileSize, tilesY - 1);

    b.regionMinX = b.wTxMin * tileSize;
    b.regionMinY = b.wTyMin * tileSize;
    b.regionMaxX = qMin(canvasW - 1, (b.wTxMax + 1) * tileSize - 1);
    b.regionMaxY = qMin(canvasH - 1, (b.wTyMax + 1) * tileSize - 1);

    return b;
}

// ---------------------------------------------------------------------------
// computeExtraWrapTileRanges
// ---------------------------------------------------------------------------
// ラップ有効時、ブラシの生bbox(rawPxMin/Max)がキャンバス端をはみ出している分だけ、
// 反対側に周回して実際に書き込まれる追加のタイル矩形を返す(最大4つ: 上下左右の
// はみ出しそれぞれ1つ + 両方はみ出した場合の対角コーナー1つ)。呼び出し側は
// 通常の書き込みタイル範囲(wTxMin..wTyMax)に加えて、これらの矩形についても
// Undoキャプチャ/書き戻しを行う必要がある。
// ---------------------------------------------------------------------------
struct WrapTileRange { int txMin, txMax, tyMin, tyMax; };

inline QVector<WrapTileRange> computeExtraWrapTileRanges(
    int canvasW, int canvasH, int tileSize, int tilesX, int tilesY,
    bool wrapX, bool wrapY,
    int rawPxMinX, int rawPxMaxX, int rawPxMinY, int rawPxMaxY,
    int normalTxMin, int normalTxMax, int normalTyMin, int normalTyMax)
{
    QVector<WrapTileRange> result;
    auto tileIdxOf = [&](int px, int tiles) { return qBound(0, px / tileSize, tiles - 1); };
    // 剰余は(C++の%は被除数の符号に従うため)常に非負になるよう補正する。
    // ブラシ中心(from/to)自体はキャンバス外(ビュー上の余白)にも位置しうるため、
    // 1周分を超えてはみ出すケースでも正しく1周期内へ畳み込む。
    auto wrapPx = [&](int px, int size) { return ((px % size) + size) % size; };

    // キャンバスの幅/高さがタイル1枚分以下(例: 256x256でtileSize=256)の場合、
    // 折り返し先のタイルが「今まさに書いている通常側」のタイルと同じになることが
    // ある。その場合は本当の意味でのラップは発生しない(タイルが1枚しかないので
    // 折り返す先も自分自身)ため、追加のタイル/リージョンを作らない
    // (作ってしまうと同じピクセルへ2回書き込むことになり、書き込み順序が
    // 不定なcompute shaderでは結果が不安定になる)。
    auto overlapsNormal = [&](int tx, int normMin, int normMax) { return tx >= normMin && tx <= normMax; };

    const bool overflowLeft  = wrapX && rawPxMinX < 0
        && !overlapsNormal(tileIdxOf(wrapPx(rawPxMinX, canvasW), tilesX), normalTxMin, normalTxMax);
    const bool overflowRight = wrapX && rawPxMaxX > canvasW - 1
        && !overlapsNormal(tileIdxOf(wrapPx(rawPxMaxX, canvasW), tilesX), normalTxMin, normalTxMax);
    const bool overflowUp    = wrapY && rawPxMinY < 0
        && !overlapsNormal(tileIdxOf(wrapPx(rawPxMinY, canvasH), tilesY), normalTyMin, normalTyMax);
    const bool overflowDown  = wrapY && rawPxMaxY > canvasH - 1
        && !overlapsNormal(tileIdxOf(wrapPx(rawPxMaxY, canvasH), tilesY), normalTyMin, normalTyMax);

    const int wrappedLeftTx  = overflowLeft  ? tileIdxOf(wrapPx(rawPxMinX, canvasW), tilesX) : 0;
    const int wrappedRightTx = overflowRight ? tileIdxOf(wrapPx(rawPxMaxX, canvasW), tilesX) : 0;
    const int wrappedUpTy    = overflowUp    ? tileIdxOf(wrapPx(rawPxMinY, canvasH), tilesY) : 0;
    const int wrappedDownTy  = overflowDown  ? tileIdxOf(wrapPx(rawPxMaxY, canvasH), tilesY) : 0;

    if (overflowLeft)  result.append({ wrappedLeftTx, tilesX - 1, normalTyMin, normalTyMax });
    if (overflowRight) result.append({ 0, wrappedRightTx, normalTyMin, normalTyMax });
    if (overflowUp)    result.append({ normalTxMin, normalTxMax, wrappedUpTy, tilesY - 1 });
    if (overflowDown)  result.append({ normalTxMin, normalTxMax, 0, wrappedDownTy });

    // 四隅(x/yどちらもはみ出した場合)
    if (overflowLeft  && overflowUp)   result.append({ wrappedLeftTx,  tilesX - 1, wrappedUpTy,   tilesY - 1 });
    if (overflowLeft  && overflowDown) result.append({ wrappedLeftTx,  tilesX - 1, 0, wrappedDownTy });
    if (overflowRight && overflowUp)   result.append({ 0, wrappedRightTx, wrappedUpTy, tilesY - 1 });
    if (overflowRight && overflowDown) result.append({ 0, wrappedRightTx, 0, wrappedDownTy });

    return result;
}

// ---------------------------------------------------------------------------
// extendRegionForWrap
// ---------------------------------------------------------------------------
// BlurTool/WarpToolのように「タイル単位でスクラッチバッファ(blurTex_/warpTex_)から
// layerTexArrayへ書き戻す」実装では、書き戻しは常にタイル全体をコピーする
// (computeStrokeDispatchBounds()のregionMinX/MaxX等が、書き込みタイル範囲を
// タイル境界ぴったりに整列させているのはこのため — dispatch領域 ⊇
// 書き戻すタイル全体、という不変条件を保つ設計になっている)。
//
// ラップで反対側に追加のタイル(computeExtraWrapTileRanges参照)を書き戻す場合も
// 同じ不変条件が必要: 単純にブラシの生bbox(raw px)ぶんだけregionを拡張すると、
// 折り返し先のタイルのうちshaderが実際に処理した(ブラシが届く)ごく一部分だけが
// 正しく書かれ、そのタイルの残り(未処理・スクラッチバッファの古いゴミ/透明データ)
// をまるごと書き戻してしまい、実際のレイヤー内容を破壊してしまう
// (「反対側の端のタイルが丸ごと透明になる」という報告の原因)。
// これを避けるため、region拡張は必ず折り返し先のタイル境界ぴったりまで
// (中途半端な位置で止めず)行う。
// ---------------------------------------------------------------------------
inline void extendRegionForWrap(
    int canvasW, int canvasH, int tileSize, int tilesX, int tilesY,
    bool wrapX, bool wrapY,
    int rawPxMinX, int rawPxMaxX, int rawPxMinY, int rawPxMaxY,
    int normalTxMin, int normalTxMax, int normalTyMin, int normalTyMax,
    int &regionMinX, int &regionMaxX, int &regionMinY, int &regionMaxY)
{
    auto wrapPx = [&](int px, int size) { return ((px % size) + size) % size; };
    auto tileIdxOf = [&](int px, int size, int tiles) { return qBound(0, wrapPx(px, size) / tileSize, tiles - 1); };
    auto overlapsNormal = [&](int tx, int normMin, int normMax) { return tx >= normMin && tx <= normMax; };

    // computeExtraWrapTileRanges()と同じく、折り返し先タイルが既に「通常側」の
    // 書き込みタイル範囲と重なる(=キャンバスがタイル1枚分以下しかない等)場合は
    // 拡張しない(同じピクセルへの二重書き込み=結果不定を避けるため)。
    if (wrapX) {
        if (rawPxMinX < 0) {
            const int wrappedTile = tileIdxOf(rawPxMinX, canvasW, tilesX);
            if (!overlapsNormal(wrappedTile, normalTxMin, normalTxMax))
                regionMinX = qMin(regionMinX, wrappedTile * tileSize - canvasW);
        }
        if (rawPxMaxX > canvasW - 1) {
            const int wrappedTile = tileIdxOf(rawPxMaxX, canvasW, tilesX);
            if (!overlapsNormal(wrappedTile, normalTxMin, normalTxMax))
                regionMaxX = qMax(regionMaxX, canvasW + (wrappedTile + 1) * tileSize - 1);
        }
    }
    if (wrapY) {
        if (rawPxMinY < 0) {
            const int wrappedTile = tileIdxOf(rawPxMinY, canvasH, tilesY);
            if (!overlapsNormal(wrappedTile, normalTyMin, normalTyMax))
                regionMinY = qMin(regionMinY, wrappedTile * tileSize - canvasH);
        }
        if (rawPxMaxY > canvasH - 1) {
            const int wrappedTile = tileIdxOf(rawPxMaxY, canvasH, tilesY);
            if (!overlapsNormal(wrappedTile, normalTyMin, normalTyMax))
                regionMaxY = qMax(regionMaxY, canvasH + (wrappedTile + 1) * tileSize - 1);
        }
    }
}
