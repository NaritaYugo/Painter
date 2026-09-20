#include "document/CanvasDocument.h"
#include <QDebug>
#include <algorithm>
#include <functional>

CanvasDocument::CanvasDocument(SliceAllocFn allocFn, SliceFreeFn freeFn)
    : allocFn_(std::move(allocFn))
    , freeFn_(std::move(freeFn))
{}

void CanvasDocument::initTileGrid(int canvasW, int canvasH)
{
    tilesX_ = (canvasW  + TILE_SIZE - 1) / TILE_SIZE;
    tilesY_ = (canvasH  + TILE_SIZE - 1) / TILE_SIZE;
}

// ===========================================================================
// レイヤー操作
// ===========================================================================
bool CanvasDocument::addLayer(const QString &name, int insertIndex, bool clipping,
                               int originTx, int originTy, int tilesXOverride, int tilesYOverride,
                               LayerType layerType, const QVector<int> &ancestorFolders)
{
    // 単色レイヤー/調整レイヤー/フィルターレイヤー/フォルダーはピクセルデータを
    // 一切持たない(常に0x0、原点も(0,0)固定)。単色レイヤーはシェーダー側で手続き的に
    // 不透明白として、調整レイヤーとフィルターレイヤーは下のレイヤーの合成結果への
    // 加工として扱われるためタイルは不要。フォルダーはUI上のマーカーに過ぎず
    // キャンバスに一切描画されないため同様に不要。
    const bool hasNoTiles = (layerType == LayerType::SolidColor || layerType == LayerType::Adjustment
                              || layerType == LayerType::Filter || layerType == LayerType::Folder);

    const int w = hasNoTiles ? 0 : (tilesXOverride < 0) ? tilesX_ : tilesXOverride;
    const int h = hasNoTiles ? 0 : (tilesYOverride < 0) ? tilesY_ : tilesYOverride;
    const int ox = hasNoTiles ? 0 : (tilesXOverride < 0) ? 0 : originTx;
    const int oy = hasNoTiles ? 0 : (tilesYOverride < 0) ? 0 : originTy;

    Layer layer;
    layer.name      = name;
    layer.clipping  = clipping;
    layer.originTx  = ox;
    layer.originTy  = oy;
    layer.layerType = layerType;

    // タイルを連番で確保（render.frag の baseSlice + ty*tilesX + tx 計算のため必須）。
    // タイル数0(単色レイヤー)の場合は確保自体が不要(allocFn_(0)は失敗扱いになるため呼ばない)。
    const int tileCount = w * h;
    if (tileCount > 0) {
        int base = allocFn_(tileCount);
        if (base < 0) {
            qWarning() << "addLayer: contiguous slice allocation failed";
            return false;
        }
        layer.tiles.resize(h);
        for (int ty = 0; ty < h; ty++) {
            layer.tiles[ty].resize(w);
            for (int tx = 0; tx < w; tx++)
                layer.tiles[ty][tx] = base + ty * w + tx;
        }
    }

    int at = (insertIndex < 0 || insertIndex > layers.size()) ? layers.size() : insertIndex;
    layers.insert(at, layer);
    activeLayer_ = at;
    reindexUndoOnInsert(at);

    // ancestorFoldersで渡された各フォルダー(呼び出し側が「atは実際にこのフォルダーの
    // 中身の範囲内」と保証している)のchildCountを+1する。渡されたインデックスは
    // すべてatより前(祖先は常に子より前に位置する)なので、挿入によるインデックス
    // シフトの影響を受けず、そのまま使える。
    for (int f : ancestorFolders)
        if (f >= 0 && f < layers.size() && f != at)
            layers[f].childCount++;

    notify(ChangeKind::CacheAndNotify);
    return true;
}

bool CanvasDocument::addLayerMask(int layerIndex)
{
    if (!layerIndexValid(layerIndex)) return false;
    Layer &layer = layers[layerIndex];
    if (layer.hasMask) return false;

    // マスクは常にキャンバス全体(tilesX_ x tilesY_)を覆う。tiles同様、連続した
    // スライスブロックとして確保する。
    const int tileCount = tilesX_ * tilesY_;
    if (tileCount <= 0) return false;
    int base = allocFn_(tileCount);
    if (base < 0) {
        qWarning() << "addLayerMask: contiguous slice allocation failed";
        return false;
    }
    layer.maskTiles.resize(tilesY_);
    for (int ty = 0; ty < tilesY_; ty++) {
        layer.maskTiles[ty].resize(tilesX_);
        for (int tx = 0; tx < tilesX_; tx++)
            layer.maskTiles[ty][tx] = base + ty * tilesX_ + tx;
    }
    layer.hasMask = true;
    notify(ChangeKind::CacheAndNotify);
    return true;
}

QVector<QVector<int>> CanvasDocument::computeAncestorFolders() const
{
    QVector<QVector<int>> result(layers.size());
    QVector<int> stack;
    std::function<void(int, int)> walk = [&](int start, int end) {
        int p = start;
        while (p < end) {
            result[p] = stack;
            if (layers[p].layerType == LayerType::Folder) {
                // childCountがレイヤー配列と矛盾していても配列外へ出ないよう必ず丸める
                // (他形式からの変換や壊れたファイルで矛盾した値が入りうる。
                //  本来はsanitizeFolderChildCounts()で読み込み直後に正しておくが、
                //  ここが落ちると原因追跡が難しい形のクラッシュになるため二重に防ぐ)。
                const int childEnd = qMin<int>(end, p + 1 + layers[p].childCount);
                stack.append(p);
                walk(p + 1, childEnd);
                stack.removeLast();
                p = childEnd;
            } else {
                p++;
            }
        }
    };
    walk(0, layers.size());
    return result;
}

// フォルダーのchildCountを、レイヤー配列に対して木構造として成立する値へ丸める。
//
// childCountは「自分の直後に何枚が入れ子で続くか」なので、p + 1 + childCount が
// 親の範囲(最上位ならlayers.size())を超えてはいけない。他形式からの変換
// (PsdCodec)や壊れたファイルではこの前提が崩れることがあり、そのままだと
// childCountを使ってindexを進める箇所(computeAncestorFolders、LayerDockの
// computeRows/afterLayerBlock 等)がすべて配列外アクセスになる。
// 読み込み直後に一度ここを通すことで、以降のコードは前提を信頼できる。
void CanvasDocument::sanitizeFolderChildCounts()
{
    std::function<void(int, int)> fix = [&](int start, int end) {
        int p = start;
        while (p < end) {
            if (layers[p].layerType == LayerType::Folder) {
                const int cc = qBound(0, layers[p].childCount, end - p - 1);
                layers[p].childCount = cc;
                fix(p + 1, p + 1 + cc);
                p += 1 + cc;
            } else {
                p++;
            }
        }
    };
    fix(0, (int)layers.size());
}

bool CanvasDocument::removeLayerMask(int layerIndex)
{
    if (!layerIndexValid(layerIndex)) return false;
    Layer &layer = layers[layerIndex];
    if (!layer.hasMask) return false;

    for (const auto &row : layer.maskTiles)
        for (int si : row)
            freeFn_(si);
    layer.maskTiles.clear();
    layer.hasMask = false;
    notify(ChangeKind::CacheAndNotify);
    return true;
}

bool CanvasDocument::ensureAdjustmentLutSlice(int layerIndex)
{
    if (!layerIndexValid(layerIndex)) return false;
    Layer &layer = layers[layerIndex];
    if (layer.lutSlice >= 0) return true;

    const int base = allocFn_(1);
    if (base < 0) {
        qWarning() << "ensureAdjustmentLutSlice: slice allocation failed";
        return false;
    }
    layer.lutSlice = base;
    return true;
}

void CanvasDocument::freeAdjustmentLutSlice(int layerIndex)
{
    if (!layerIndexValid(layerIndex)) return;
    Layer &layer = layers[layerIndex];
    if (layer.lutSlice < 0) return;
    freeFn_(layer.lutSlice);
    layer.lutSlice = -1;
}

bool CanvasDocument::growLayerBounds(int layerIndex, int minTx, int minTy,
                                      int maxTxEx, int maxTyEx,
                                      const CopyTileFn &copyFn, const ClearTileFn &clearFn)
{
    if (!layerIndexValid(layerIndex)) return false;
    Layer &layer = layers[layerIndex];

    const int curW = layer.tilesX();
    const int curH = layer.tilesY();
    const int curMinTx = layer.originTx, curMinTy = layer.originTy;
    const int curMaxTx = curMinTx + curW, curMaxTy = curMinTy + curH;

    const int newMinTx = qMin(curMinTx, minTx);
    const int newMinTy = qMin(curMinTy, minTy);
    const int newMaxTx = qMax(curMaxTx, maxTxEx);
    const int newMaxTy = qMax(curMaxTy, maxTyEx);

    if (newMinTx == curMinTx && newMinTy == curMinTy &&
        newMaxTx == curMaxTx && newMaxTy == curMaxTy) {
        return false; // 既に範囲を満たしている
    }

    const int newW = newMaxTx - newMinTx;
    const int newH = newMaxTy - newMinTy;
    const int newBase = allocFn_ ? allocFn_(newW * newH) : -1;
    if (newBase < 0) {
        qWarning() << "growLayerBounds: contiguous slice allocation failed";
        return false;
    }

    QVector<int> oldSlicesToFree;
    oldSlicesToFree.reserve(curW * curH);

    QVector<QVector<int>> newTiles(newH, QVector<int>(newW, -1));
    for (int ty = 0; ty < newH; ty++) {
        for (int tx = 0; tx < newW; tx++) {
            const int newSlice = newBase + ty * newW + tx;
            newTiles[ty][tx] = newSlice;

            const int oldLocalTx = (newMinTx + tx) - curMinTx;
            const int oldLocalTy = (newMinTy + ty) - curMinTy;
            if (oldLocalTx >= 0 && oldLocalTx < curW && oldLocalTy >= 0 && oldLocalTy < curH) {
                const int oldSlice = layer.tiles[oldLocalTy][oldLocalTx];
                if (copyFn) copyFn(oldSlice, newSlice);
                oldSlicesToFree.append(oldSlice);
            } else {
                if (clearFn) clearFn(newSlice);
            }
        }
    }

    if (freeFn_)
        for (int s : oldSlicesToFree) freeFn_(s);

    layer.tiles    = newTiles;
    layer.originTx = newMinTx;
    layer.originTy = newMinTy;
    return true;
}

void CanvasDocument::invalidateLayerTileUndoHistory(int layerIndex)
{
    auto isStale = [layerIndex](const UndoEntry &e) {
        return e.kind == UndoKind::LayerTiles && e.layerIndex == layerIndex;
    };
    undoStack.erase(std::remove_if(undoStack.begin(), undoStack.end(), isStale), undoStack.end());
    redoStack.erase(std::remove_if(redoStack.begin(), redoStack.end(), isStale), redoStack.end());
}

bool CanvasDocument::removeLayer(int layerIndex, const QVector<int> &ancestorFolders, bool includeContents) {
    if (!layerIndexValid(layerIndex)) return false;

    // フォルダーを削除する場合は、その中身(childCount枚)もまとめて1ブロックとして
    // 削除する(フォルダーだけ消えて中身が孤立する/繰り上がることを避ける)。
    // includeContents=falseならマーカー1枚だけを対象にする(中身は残す)。
    const bool cascade = includeContents && (layers[layerIndex].layerType == LayerType::Folder);
    const int count = cascade ? 1 + layers[layerIndex].childCount : 1;
    if (layers.size() <= count) return false; // ドキュメントに最低1枚は残す

    for (int i = layerIndex; i < layerIndex + count; i++) {
        for (const auto &row : layers[i].tiles)
            for (int si : row)
                freeFn_(si);
        for (const auto &row : layers[i].maskTiles)
            for (int si : row)
                freeFn_(si);
        if (layers[i].lutSlice >= 0)
            freeFn_(layers[i].lutSlice);
    }

    layers.remove(layerIndex, count);
    activeLayer_ = qBound(0, activeLayer_, layers.size() - 1);

    if (count == 1) {
        reindexUndoOnRemove(layerIndex);
    } else {
        // 複数枚の一括削除(フォルダー丸ごと)。moveLayerBlock同様の一般化: 削除範囲内の
        // 履歴は復元不能として破棄し、範囲より後ろはcount分だけ繰り上げる。
        auto fix = [layerIndex, count](QVector<UndoEntry> &stack) {
            for (int i = stack.size() - 1; i >= 0; i--) {
                UndoEntry &e = stack[i];
                if (e.kind != UndoKind::LayerTiles) continue;
                if (e.layerIndex >= layerIndex && e.layerIndex < layerIndex + count)
                    stack.remove(i);
                else if (e.layerIndex >= layerIndex + count)
                    e.layerIndex -= count;
            }
        };
        fix(undoStack);
        fix(redoStack);
    }

    // ancestorFoldersで渡された各フォルダー(削除対象が実際に属している外側の階層
    // チェーン)のchildCountを、削除された総枚数ぶん減算する。渡されたインデックスは
    // すべてlayerIndexより前なので、削除によるインデックスシフトの影響を受けない。
    for (int f : ancestorFolders)
        if (f >= 0 && f < layerIndex)
            layers[f].childCount = qMax(0, layers[f].childCount - count);

    notify(ChangeKind::CacheAndNotify);
    return true;
}

bool CanvasDocument::moveLayer(int fromIndex, int toIndex, bool newClipping) {
    if (!layerIndexValid(fromIndex)) return false;
    toIndex = qBound(0, toIndex, layers.size() - 1);

    bool wasActive = (activeLayer_ == fromIndex);

    Layer moved = layers.takeAt(fromIndex);
    moved.clipping = newClipping;
    layers.insert(toIndex, moved);

    if (wasActive) {
        activeLayer_ = toIndex;
    } else if (fromIndex < activeLayer_ && toIndex >= activeLayer_) {
        activeLayer_--;
    } else if (fromIndex > activeLayer_ && toIndex <= activeLayer_) {
        activeLayer_++;
    }

    reindexUndoOnMove(fromIndex, toIndex);

    notify(ChangeKind::CacheAndNotify);
    return true;
}

bool CanvasDocument::moveLayerBlock(int fromStart, int count, int toIndex)
{
    if (count <= 0) return false;
    if (fromStart < 0 || fromStart + count > layers.size()) return false;
    toIndex = qBound(0, toIndex, layers.size() - count);
    if (toIndex == fromStart) return true; // 位置が変わらない

    // moveLayer/reindexUndoOnMove と同じ「takeAt+insert」操作を、ブロック全体に
    // 一般化したもの。ブロック外のインデックスidxの移動後位置を求める。
    auto mapOutside = [fromStart, count, toIndex](int idx) {
        int afterRemoval   = (idx >= fromStart + count) ? idx - count : idx;
        int afterInsertion = (afterRemoval >= toIndex) ? afterRemoval + count : afterRemoval;
        return afterInsertion;
    };
    // ブロック内のインデックスidx(fromStart..fromStart+count-1)の移動後位置。
    auto mapInside = [fromStart, toIndex](int idx) { return toIndex + (idx - fromStart); };

    QVector<Layer> block = layers.mid(fromStart, count);
    layers.remove(fromStart, count);
    layers = layers.mid(0, toIndex) + block + layers.mid(toIndex);

    if (activeLayer_ >= fromStart && activeLayer_ < fromStart + count) {
        activeLayer_ = mapInside(activeLayer_);
    } else {
        activeLayer_ = mapOutside(activeLayer_);
    }

    auto remap = [&](int idx) {
        return (idx >= fromStart && idx < fromStart + count) ? mapInside(idx) : mapOutside(idx);
    };
    auto fix = [&](QVector<UndoEntry> &stack) {
        for (UndoEntry &e : stack) {
            if (e.kind != UndoKind::LayerTiles) continue;
            e.layerIndex = remap(e.layerIndex);
        }
    };
    fix(undoStack);
    fix(redoStack);

    notify(ChangeKind::CacheAndNotify);
    return true;
}

// ---------------------------------------------------------------------------
void CanvasDocument::reindexUndoOnInsert(int insertedAt)
{
    auto fix = [insertedAt](QVector<UndoEntry> &stack) {
        for (UndoEntry &e : stack) {
            if (e.kind != UndoKind::LayerTiles) continue;
            if (e.layerIndex >= insertedAt) e.layerIndex++;
        }
    };
    fix(undoStack);
    fix(redoStack);
}

void CanvasDocument::reindexUndoOnRemove(int removedIndex)
{
    auto fix = [removedIndex](QVector<UndoEntry> &stack) {
        for (int i = stack.size() - 1; i >= 0; i--) {
            UndoEntry &e = stack[i];
            if (e.kind != UndoKind::LayerTiles) continue;
            if (e.layerIndex == removedIndex) {
                // 削除されたレイヤー自身の差分は、そのスライスが解放されフリーリストへ
                // 戻る(=無関係な将来のレイヤーに再利用されうる)ため、復元不能として
                // 履歴ごと破棄する(残すとUndo適用時に別レイヤーの中身を上書きしうる)。
                stack.remove(i);
            } else if (e.layerIndex > removedIndex) {
                e.layerIndex--;
            }
        }
    };
    fix(undoStack);
    fix(redoStack);
}

void CanvasDocument::reindexUndoOnMove(int fromIndex, int toIndex)
{
    // activeLayer_の追従ロジック(moveLayer本体)と同じ場合分けを、任意のインデックスに
    // 一般化したもの(takeAt(fromIndex)+insert(toIndex)という同一の配列操作が対象なので、
    // 同じ場合分けがそのまま成り立つ)。
    auto remap = [fromIndex, toIndex](int idx) {
        if (idx == fromIndex) return toIndex;
        if (fromIndex < idx && toIndex >= idx) return idx - 1;
        if (fromIndex > idx && toIndex <= idx) return idx + 1;
        return idx;
    };
    auto fix = [&](QVector<UndoEntry> &stack) {
        for (UndoEntry &e : stack) {
            if (e.kind != UndoKind::LayerTiles) continue;
            e.layerIndex = remap(e.layerIndex);
        }
    };
    fix(undoStack);
    fix(redoStack);
}

Layer CanvasDocument::layerAt(int layerIndex) const {
    if (!layerIndexValid(layerIndex)) return {};
    return layers[layerIndex];
}

Layer &CanvasDocument::layerRef(int layerIndex) {
    Q_ASSERT(layerIndexValid(layerIndex));
    return layers[layerIndex];
}

QString   CanvasDocument::layerName(int i)       const { return layerIndexValid(i) ? layers[i].name       : QString{}; }
float     CanvasDocument::layerOpacity(int i)    const { return layerIndexValid(i) ? layers[i].opacity    : 1.0f; }
bool      CanvasDocument::layerVisible(int i)    const { return layerIndexValid(i) ? layers[i].visible    : true; }
BlendMode CanvasDocument::layerBlendMode(int i)  const { return layerIndexValid(i) ? layers[i].blendMode  : BlendMode::Normal; }
bool      CanvasDocument::layerClipping(int i)   const { return layerIndexValid(i) ? layers[i].clipping   : false; }

void CanvasDocument::setLayerOpacity(int i, float v) {
    if (layerIndexValid(i)) { layers[i].opacity = qBound(0.0f, v, 1.0f); notify(ChangeKind::CacheAndNotify); }
}
void CanvasDocument::setLayerVisible(int i, bool v) {
    // opacity/blendMode/clippingと同様、見た目(キャンバスの合成結果)に直接影響する
    // ため、NotifyOnly(layersChanged()のみ)ではキャンバスが再描画されない。
    // CacheAndNotifyでupdate()も呼ばせる。
    if (layerIndexValid(i)) { layers[i].visible = v; notify(ChangeKind::CacheAndNotify); }
}
void CanvasDocument::setLayerName(int i, const QString &v) {
    if (layerIndexValid(i)) { layers[i].name = v; notify(ChangeKind::NotifyOnly); }
}
void CanvasDocument::setLayerBlendMode(int i, BlendMode v) {
    if (layerIndexValid(i)) { layers[i].blendMode = v; notify(ChangeKind::CacheAndNotify); }
}
void CanvasDocument::setLayerClipping(int i, bool v) {
    if (layerIndexValid(i)) { layers[i].clipping = v; notify(ChangeKind::CacheAndNotify); }
}

// ===========================================================================
// アクティブ状態
// ===========================================================================
void CanvasDocument::setActiveLayer(int i) {
    if (!layerIndexValid(i)) return;
    activeLayer_ = i;
    notify(ChangeKind::ActiveLayerChanged);
}

Layer &CanvasDocument::activeLayer() {
    return layers[activeLayer_];
}
const Layer &CanvasDocument::activeLayer() const {
    return layers[activeLayer_];
}

// ===========================================================================
// Undo / Redo
// ===========================================================================
void CanvasDocument::pushUndo(const UndoEntry &entry) {
    undoStack.push_back(entry);
    if (undoStack.size() > maxUndo_)
        undoStack.removeFirst();
    redoStack.clear();
    modifiedCount_++;
}

void CanvasDocument::clearRedo() {
    redoStack.clear();
}

UndoEntry CanvasDocument::applyUndo(const UndoEntry &current) {
    Q_ASSERT(canUndo());
    UndoEntry entry = undoStack.takeLast();

    if (entry.kind == UndoKind::CanvasResize || entry.kind == UndoKind::Selection
        || entry.kind == UndoKind::LayerAdd || entry.kind == UndoKind::LayerRemove
        || entry.kind == UndoKind::LayerMerge) {
        // CanvasResize / Selection / LayerAdd は変更前後を自己完結して持っているため、
        // currentは使わずエントリ自身をそのままredoStackへ積む
        // (redo時にafter側 = LayerAddならレイヤーを作り直す側を使えばよい)。
        redoStack.push_back(entry);
        if (redoStack.size() > maxUndo_)
            redoStack.removeFirst();
        modifiedCount_--;
        return entry;
    }

    redoStack.push_back(current);
    if (redoStack.size() > maxUndo_)
        redoStack.removeFirst();
    // reindexUndoOnRemove/Insert/Move で通常は常に有効な範囲に保たれるはずだが、
    // 万一の不整合でも layers[] への範囲外アクセス(クラッシュ)だけは起こさないよう
    // 防御的にクランプする。
    activeLayer_ = qBound(0, entry.layerIndex, layers.size() - 1);
    modifiedCount_--;
    return entry;
}

UndoEntry CanvasDocument::applyRedo(const UndoEntry &current) {
    Q_ASSERT(canRedo());
    UndoEntry entry = redoStack.takeLast();

    if (entry.kind == UndoKind::CanvasResize || entry.kind == UndoKind::Selection
        || entry.kind == UndoKind::LayerAdd || entry.kind == UndoKind::LayerRemove
        || entry.kind == UndoKind::LayerMerge) {
        undoStack.push_back(entry);
        modifiedCount_++;
        return entry;
    }

    undoStack.push_back(current);
    // applyUndo()と同じ理由で防御的にクランプする。
    activeLayer_ = qBound(0, entry.layerIndex, layers.size() - 1);
    modifiedCount_++;
    return entry;
}

void CanvasDocument::resetToBlank()
{
    // 全レイヤーのタイルスライスを解放
    for (const Layer &layer : layers)
        for (const auto &row : layer.tiles)
            for (int si : row)
                freeFn_(si);

    layers.clear();
    undoStack.clear();
    redoStack.clear();
    activeLayer_    = 0;
    modifiedCount_  = 0;
    // allocFn_ / freeFn_ / onChanged はそのまま維持する
    // (呼び出し側が新しいレイヤーを作るのはこのメソッドの後の責務)
}
