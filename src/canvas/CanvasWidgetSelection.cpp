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

// Selection state, clipboard operations, and selection undo integration.

QByteArray CanvasWidget::captureSelectionMask()
{
    if (selectionMaskTex == 0 || canvasW <= 0 || canvasH <= 0) return QByteArray();
    // 選択マスクはcompute shaderのimageStoreでも書かれる(変形の焼き込み等)。
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glFinish();

    QVector<uint8_t> buf(canvasW * canvasH, 255);
    glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, buf.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    // ここでは圧縮しない(生データを返す)。
    return QByteArray(reinterpret_cast<const char *>(buf.constData()), buf.size());
}

void CanvasWidget::restoreSelectionMaskRect(const QByteArray &compressed, int x, int y, int w, int h, bool hasSel)
{
    if (selectionMaskTex == 0 || compressed.isEmpty() || w <= 0 || h <= 0) return;
    const QByteArray raw = qUncompress(compressed);
    if (raw.size() != (qsizetype)w * h) return; // 記録時とサイズが合わない: 復元しない
    if (x < 0 || y < 0 || x + w > canvasW || y + h > canvasH) return;

    glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RED, GL_UNSIGNED_BYTE, raw.constData());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glMemoryBarrier(GL_ALL_BARRIER_BITS); // 以降のcompute shaderのimageLoadから見えるようにする
    invalidateSelectionOutlineCache();

    if (hasSelection_ != hasSel) {
        hasSelection_ = hasSel;
        emit selectionChanged(hasSelection_);
    }
    update();
}

void CanvasWidget::beginSelectionUndo()
{
    if (!glReady_) return;
    makeCurrent();
    pendingSelectionUndoBefore_ = captureSelectionMask();
    pendingSelectionUndoHadSel_ = hasSelection_;
    pendingSelectionUndoValid_  = !pendingSelectionUndoBefore_.isEmpty();
}

void CanvasWidget::commitSelectionUndo(const QByteArray &afterRaw, int x, int y, int w, int h)
{
    if (!pendingSelectionUndoValid_) return;
    pendingSelectionUndoValid_ = false; // 空振りでも必ず降ろす(次のbeginまで持ち越さない)

    if (w <= 0 || h <= 0) return;                       // 変化なし
    if (x < 0 || y < 0 || x + w > canvasW || y + h > canvasH) return;
    if (pendingSelectionUndoBefore_.size() != (qsizetype)canvasW * canvasH) return;

    // 呼び出し側が「貼ったばかりの矩形のマスク」を持っているならそれを使い、無ければGPUから読み戻す(読み戻しは重いので基本は前者を使う)。
    QByteArray afterRect = afterRaw;
    if (afterRect.size() != (qsizetype)w * h) {
        makeCurrent();
        const QByteArray full = captureSelectionMask();
        if (full.size() != (qsizetype)canvasW * canvasH) return;
        afterRect.resize((qsizetype)w * h);
        for (int r = 0; r < h; r++)
            memcpy(afterRect.data() + (qsizetype)r * w,
                   full.constData() + (qsizetype)(y + r) * canvasW + x, w);
    }

    // 「変更前」も同じ矩形だけ切り出す。
    QByteArray beforeRect((qsizetype)w * h, Qt::Uninitialized);
    for (int r = 0; r < h; r++)
        memcpy(beforeRect.data() + (qsizetype)r * w,
               pendingSelectionUndoBefore_.constData() + (qsizetype)(y + r) * canvasW + x, w);

    // 中身も選択状態も変わっていなければ履歴を汚さない。
    if (beforeRect == afterRect && hasSelection_ == pendingSelectionUndoHadSel_) return;

    UndoEntry entry;
    entry.kind = UndoKind::Selection;
    entry.selection.rectX = x; entry.selection.rectY = y;
    entry.selection.rectW = w; entry.selection.rectH = h;
    // レベル1で十分縮む(0/255が塊で続くデータ)。
    entry.selection.beforeMask = qCompress(reinterpret_cast<const uchar *>(beforeRect.constData()),
                                            beforeRect.size(), 1);
    entry.selection.afterMask  = qCompress(reinterpret_cast<const uchar *>(afterRect.constData()),
                                            afterRect.size(), 1);
    entry.selection.hadSelectionBefore = pendingSelectionUndoHadSel_;
    entry.selection.hasSelectionAfter  = hasSelection_;
    entry.selection.maskW = canvasW;
    entry.selection.maskH = canvasH;
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());
}

void CanvasWidget::applySelectionUndoEntry(const UndoEntry &entry, bool toBefore)
{
    makeCurrent();
    const SelectionUndoData &sel = entry.selection;
    restoreSelectionMaskRect(toBefore ? sel.beforeMask : sel.afterMask,
                             sel.rectX, sel.rectY, sel.rectW, sel.rectH,
                             toBefore ? sel.hadSelectionBefore : sel.hasSelectionAfter);
}

void CanvasWidget::clearSelection()
{
    if (!hasSelection_ && selectionMaskTex == 0) return;
    makeCurrent();
    beginSelectionUndo();

    QVector<uint8_t> fullSel(canvasW * canvasH, 255);
    glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvasW, canvasH,
                    GL_RED, GL_UNSIGNED_BYTE, fullSel.constData());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glMemoryBarrier(GL_ALL_BARRIER_BITS); // 以降のcompute shaderのimageLoadから確実に見えるようにする
    invalidateSelectionOutlineCache();

    if (hasSelection_) {
        hasSelection_ = false;
        emit selectionChanged(false);
    }
    // 貼ったばかりのマスク(全域255)をそのまま渡して読み戻しを1回省く。
    commitSelectionUndo(QByteArray(reinterpret_cast<const char *>(fullSel.constData()), fullSel.size()),
                        0, 0, canvasW, canvasH);
    update();
}

void CanvasWidget::selectAll()
{
    if (!doc_ || doc_->layers.isEmpty()) return;
    makeCurrent();
    beginSelectionUndo();

    // clearSelection()と同じ「全域255」のマスクを敷く。
    QVector<uint8_t> fullSel(canvasW * canvasH, 255);
    glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvasW, canvasH,
                    GL_RED, GL_UNSIGNED_BYTE, fullSel.constData());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    invalidateSelectionOutlineCache();

    if (!hasSelection_) {
        hasSelection_ = true;
        emit selectionChanged(true);
    }
    // 貼ったばかりのマスク(全域255)をそのまま渡して読み戻しを1回省く。
    commitSelectionUndo(QByteArray(reinterpret_cast<const char *>(fullSel.constData()), fullSel.size()),
                        0, 0, canvasW, canvasH);
    update();
}


namespace {
const char *kPasteOriginMimeType = "application/x-tiepolo-paste-origin";
}

void CanvasWidget::copySelection()
{
    if (!doc_ || doc_->layers.isEmpty()) return;
    const Layer &activeLayer = doc_->activeLayer();
    // 単色/調整レイヤーは実ピクセルを持たないためコピー対象にできない。
    if (activeLayer.layerType != LayerType::Normal && activeLayer.layerType != LayerType::Text) return;

    makeCurrent();

    int rectX, rectY, rectW, rectH;
    QVector<uint8_t> maskBuf; // hasSelection_のときだけ使う(キャンバスサイズ)

    if (hasSelection_) {
        maskBuf.resize(canvasW * canvasH);
        glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, maskBuf.data());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, 0);

        int minX = canvasW, maxX = -1, minY = canvasH, maxY = -1;
        for (int y = 0; y < canvasH; y++) {
            for (int x = 0; x < canvasW; x++) {
                if (maskBuf[y * canvasW + x] == 0) continue;
                minX = qMin(minX, x); maxX = qMax(maxX, x);
                minY = qMin(minY, y); maxY = qMax(maxY, y);
            }
        }
        if (maxX < minX) return; // 選択範囲が空
        rectX = minX; rectY = minY; rectW = maxX - minX + 1; rectH = maxY - minY + 1;
    } else {
        // レイヤーの全内容(キャンバス外にはみ出た部分も含む)。
        rectX = activeLayer.originTx * TILE_SIZE;
        rectY = activeLayer.originTy * TILE_SIZE;
        rectW = activeLayer.tilesX() * TILE_SIZE;
        rectH = activeLayer.tilesY() * TILE_SIZE;
    }
    if (rectW <= 0 || rectH <= 0) return;

    QImage img(rectW, rectH, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    for (int ty = 0; ty < activeLayer.tilesY(); ty++) {
        for (int tx = 0; tx < activeLayer.tilesX(); tx++) {
            const int canvasTx = activeLayer.originTx + tx;
            const int canvasTy = activeLayer.originTy + ty;
            const int tilePxX  = canvasTx * TILE_SIZE;
            const int tilePxY  = canvasTy * TILE_SIZE;
            const int ox0 = qMax(tilePxX, rectX), oy0 = qMax(tilePxY, rectY);
            const int ox1 = qMin(tilePxX + TILE_SIZE, rectX + rectW);
            const int oy1 = qMin(tilePxY + TILE_SIZE, rectY + rectH);
            if (ox0 >= ox1 || oy0 >= oy1) continue;

            const QByteArray raw = readSlicePixels(activeLayer.tiles[ty][tx]);
            for (int y = oy0; y < oy1; y++) {
                const uchar *srcRow = reinterpret_cast<const uchar*>(raw.constData())
                                    + ((y - tilePxY) * TILE_SIZE + (ox0 - tilePxX)) * 4;
                uchar *dstRow = img.scanLine(y - rectY) + (ox0 - rectX) * 4;
                memcpy(dstRow, srcRow, (ox1 - ox0) * 4);
            }
        }
    }

    if (hasSelection_) {
        // 選択範囲の形状でマスクする(bbox内でも非選択部分は透過にする)。
        for (int y = 0; y < rectH; y++) {
            uchar *row = img.scanLine(y);
            const uint8_t *maskRow = maskBuf.constData() + (size_t)(y + rectY) * canvasW + rectX;
            for (int x = 0; x < rectW; x++) {
                if (maskRow[x] == 0) memset(row + x * 4, 0, 4);
            }
        }
    }

    // タイル格納の行順(原点左下・Y上向き)を、標準画像/OSクリップボードが期待する行順(原点左上・Y下向き)へ変換してからクリップボードへ渡す(loadImageAsSingleLayer等と対の反転。ここが無いとソフト外へコピーした画像やソフ
    // ト外からの貼り付けが上下逆になる)。
    img = img.mirrored(false, true);

    QMimeData *mime = new QMimeData();
    mime->setImageData(img);
    mime->setData(QString::fromLatin1(kPasteOriginMimeType),
                  QByteArray::number(rectX) + ',' + QByteArray::number(rectY));
    QGuiApplication::clipboard()->setMimeData(mime);
}

void CanvasWidget::pasteClipboard()
{
    if (!doc_ || doc_->layers.isEmpty()) return;
    const int layerIndex = doc_->activeLayerIndex();
    if (doc_->layerRef(layerIndex).layerType != LayerType::Normal
        && doc_->layerRef(layerIndex).layerType != LayerType::Text) return;

    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasImage()) return;
    // 標準画像/OSクリップボードの行順(原点左上・Y下向き)を、タイル格納が期待する行順(原点左下・Y上向き)へ変換する(loadImageAsSingleLayer等と対の反転。これが無いとソフト外からの貼り付けが上下逆になる)。
    const QImage img = qvariant_cast<QImage>(mime->imageData()).convertToFormat(QImage::Format_RGBA8888_Premultiplied).mirrored(false, true);
    if (img.isNull() || img.width() <= 0 || img.height() <= 0) return;

    int pasteX, pasteY;
    bool hasOrigin = false;
    if (mime->hasFormat(QString::fromLatin1(kPasteOriginMimeType))) {
        const QList<QByteArray> parts = mime->data(QString::fromLatin1(kPasteOriginMimeType)).split(',');
        if (parts.size() == 2) {
            bool ok1 = false, ok2 = false;
            const int x = parts[0].toInt(&ok1), y = parts[1].toInt(&ok2);
            if (ok1 && ok2) { pasteX = x; pasteY = y; hasOrigin = true; }
        }
    }
    if (!hasOrigin) {
        // 外部由来の画像等、コピー元位置が無い場合はキャンバス中央へ貼り付ける。
        pasteX = (canvasW - img.width()) / 2;
        pasteY = (canvasH - img.height()) / 2;
    }

    makeCurrent();

    const int minTx   = qFloor((double)pasteX / TILE_SIZE);
    const int minTy   = qFloor((double)pasteY / TILE_SIZE);
    const int maxTxEx = qCeil((double)(pasteX + img.width()) / TILE_SIZE);
    const int maxTyEx = qCeil((double)(pasteY + img.height()) / TILE_SIZE);
    growLayerBoundsToCoverCanvasTiles(layerIndex, minTx, minTy, maxTxEx, maxTyEx);

    const Layer &layer = doc_->layerRef(layerIndex);

    beginStrokeUndo();
    // 貼り付けで実際に書き換えるタイルの範囲だけをUndo対象にする。
    const int touchTxMin = qMax(minTx, layer.originTx);
    const int touchTyMin = qMax(minTy, layer.originTy);
    const int touchTxMax = qMin(maxTxEx - 1, layer.originTx + layer.tilesX() - 1);
    const int touchTyMax = qMin(maxTyEx - 1, layer.originTy + layer.tilesY() - 1);
    expandStrokeUndoRegion(touchTxMin, touchTxMax, touchTyMin, touchTyMax);

    for (int ty = 0; ty < layer.tilesY(); ty++) {
        for (int tx = 0; tx < layer.tilesX(); tx++) {
            const int canvasTx = layer.originTx + tx;
            const int canvasTy = layer.originTy + ty;
            const int tilePxX  = canvasTx * TILE_SIZE;
            const int tilePxY  = canvasTy * TILE_SIZE;
            const int ox0 = qMax(tilePxX, pasteX), oy0 = qMax(tilePxY, pasteY);
            const int ox1 = qMin(tilePxX + TILE_SIZE, pasteX + img.width());
            const int oy1 = qMin(tilePxY + TILE_SIZE, pasteY + img.height());
            if (ox0 >= ox1 || oy0 >= oy1) continue;

            const int slice = layer.tiles[ty][tx];
            QByteArray buf = readSlicePixels(slice);
            for (int y = oy0; y < oy1; y++) {
                const uchar *srcRow = img.constScanLine(y - pasteY) + (ox0 - pasteX) * 4;
                uchar *dstRow = reinterpret_cast<uchar*>(buf.data()) + ((y - tilePxY) * TILE_SIZE + (ox0 - tilePxX)) * 4;
                // 通常のアルファ合成(Source Over、プリマル済みRGBA前提)で貼り付ける。
                for (int x = 0; x < ox1 - ox0; x++) {
                    const uchar sa = srcRow[x * 4 + 3];
                    if (sa == 255) { memcpy(dstRow + x * 4, srcRow + x * 4, 4); continue; }
                    if (sa == 0) continue;
                    const int inv = 255 - sa;
                    for (int c = 0; c < 4; c++)
                        dstRow[x * 4 + c] = (uchar)qMin(255, srcRow[x * 4 + c] + (dstRow[x * 4 + c] * inv) / 255);
                }
            }
            writeSlicePixels(slice, buf);
        }
    }

    commitStrokeUndo();
    updateCompositedTex();
    emit layersChanged();
    update();
}

// 拡大・縮小・回転 / 自由変形(アクション)
