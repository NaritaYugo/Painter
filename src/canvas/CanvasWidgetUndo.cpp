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

// Document and stroke undo/redo coordination.

UndoEntry CanvasWidget::captureAllTiles(int layerIndex)
{
    return undoRecorder_.captureAllTiles(toolCtx_, layerIndex);
}

UndoEntry CanvasWidget::captureTilesLike(int layerIndex, const QVector<TileUndo> &shape)
{
    return undoRecorder_.captureTilesLike(toolCtx_, layerIndex, shape);
}

void CanvasWidget::pushUndoSnapshot()
{
    UndoEntry entry = captureAllTiles(doc_->activeLayerIndex());
    doc_->clearRedo();
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());
}

void CanvasWidget::undo()
{
    if (!doc_->canUndo()) return;
    makeCurrent();
    // Undo/Redoはレイヤー内容を書き換えるが doc_->onChanged を経由しない(直接update()する)ため、事前合成キャッシュをここで明示的に無効化する。
    invalidateBelowCompositeCache();
    invalidateAboveCompositeCache();
    scheduleCompositeCachePrewarm(); // 次のストローク開始を待たずアイドル中に作り直す
    // フィルターレイヤーの連鎖(CanvasWidget.h の filterChainResult_ 参照)も同じ理由で無効化が要る。
    invalidateFilterChain();

    if (doc_->topUndoKind() == UndoKind::CanvasResize) {
        UndoEntry restored = doc_->applyUndo(UndoEntry{}); // currentは使われない(kind==CanvasResize)
        applyCanvasResizeUndoEntry(restored, /*toBefore=*/true);
    } else if (doc_->topUndoKind() == UndoKind::Selection) {
        // CanvasResizeと同じく変更前後を自己完結して持つのでcurrentは使われない。
        UndoEntry restored = doc_->applyUndo(UndoEntry{});
        applySelectionUndoEntry(restored, /*toBefore=*/true);
    } else if (doc_->topUndoKind() == UndoKind::LayerAdd) {
        UndoEntry restored = doc_->applyUndo(UndoEntry{});
        applyLayerAddUndoEntry(restored, /*toBefore=*/true);
    } else if (doc_->topUndoKind() == UndoKind::LayerRemove) {
        UndoEntry restored = doc_->applyUndo(UndoEntry{});
        applyLayerRemoveUndoEntry(restored, /*toBefore=*/true);
    } else if (doc_->topUndoKind() == UndoKind::LayerMerge) {
        UndoEntry restored = doc_->applyUndo(UndoEntry{});
        applyLayerMergeUndoEntry(restored, /*toBefore=*/true);
    } else {
        // "current"(取り消す直前の状態)は、これから復元する差分エントリが実際に触れるタイルだけをキャプチャすれば十分(restoreLayer()もそのタイルしか書き換えないため)。
        UndoEntry current = captureTilesLike(doc_->activeLayerIndex(), doc_->peekUndo().tiles);
        UndoEntry restored = doc_->applyUndo(current);
        restoreLayer(restored);
        update();
        emit layersChanged();
    }
    emit modifiedChanged(doc_->isModified());
}

void CanvasWidget::redo()
{
    if (!doc_->canRedo()) return;
    makeCurrent();
    invalidateBelowCompositeCache(); // undo()と同じ理由
    invalidateAboveCompositeCache();
    scheduleCompositeCachePrewarm();
    invalidateFilterChain();

    if (doc_->topRedoKind() == UndoKind::CanvasResize) {
        UndoEntry restored = doc_->applyRedo(UndoEntry{});
        applyCanvasResizeUndoEntry(restored, /*toBefore=*/false);
    } else if (doc_->topRedoKind() == UndoKind::Selection) {
        UndoEntry restored = doc_->applyRedo(UndoEntry{});
        applySelectionUndoEntry(restored, /*toBefore=*/false);
    } else if (doc_->topRedoKind() == UndoKind::LayerAdd) {
        UndoEntry restored = doc_->applyRedo(UndoEntry{});
        applyLayerAddUndoEntry(restored, /*toBefore=*/false);
    } else if (doc_->topRedoKind() == UndoKind::LayerRemove) {
        UndoEntry restored = doc_->applyRedo(UndoEntry{});
        applyLayerRemoveUndoEntry(restored, /*toBefore=*/false);
    } else if (doc_->topRedoKind() == UndoKind::LayerMerge) {
        UndoEntry restored = doc_->applyRedo(UndoEntry{});
        applyLayerMergeUndoEntry(restored, /*toBefore=*/false);
    } else {
        // undo()と同じ理由で、redoしようとしているエントリが触れるタイルだけをキャプチャする。
        UndoEntry current = captureTilesLike(doc_->activeLayerIndex(), doc_->peekRedo().tiles);
        UndoEntry restored = doc_->applyRedo(current);
        restoreLayer(restored);
        update();
        emit layersChanged();
    }
    emit modifiedChanged(doc_->isModified());
}

void CanvasWidget::restoreLayer(const UndoEntry &entry)
{
    undoRecorder_.restoreLayer(toolCtx_, entry);
}

void CanvasWidget::beginStrokeUndo()
{
    undoRecorder_.beginStroke(toolCtx_);
}

void CanvasWidget::expandStrokeUndoRegion(int txMin, int txMax, int tyMin, int tyMax)
{
    undoRecorder_.expandRegion(toolCtx_, txMin, txMax, tyMin, tyMax);
}

void CanvasWidget::commitStrokeUndo()
{
    if (!undoRecorder_.hasPending()) return;

    UndoEntry entry = undoRecorder_.takeStrokeEntry(toolCtx_, doc_->activeLayerIndex());
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());

    // フィルターレイヤーより下のレイヤーへ描いている間は、prepareCompositeBase()がストローク中ずっと(withPaintPreview=trueで)連鎖を毎フレーム作り直しているが、
    // これはマウスを離した瞬間(=ここ)に止まる。
    invalidateFilterChain();
}

// 「アクティブレイヤーより下(z < uptoExclusiveIndex)」を合成してキャッシュへ書く。
