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

// Canvas recreation, pixel transfer, resize, rotate, and flip operations.

void CanvasWidget::resetDocument()
{
    makeCurrent();
 
    // 使用中の全スライスをゼロクリア(バンク境界をまたいでも clearSliceRange が分割処理する)
    clearSliceRange(0, sliceAllocator_.nextSlice(), 0.0f, 0.0f, 0.0f, 0.0f);
 
    // スライス管理をリセット
    sliceAllocator_.resetAllocationState();

    // CanvasDocument をリセット: 全レイヤーを捨てる
    // (所有権はCanvasWidget外にあるので、作り直すのではなく中身だけ空にする)
    //
    // addLayer()自体がdoc_->onChanged経由でupdate()を呼び得るため、
    // レイヤーがまだ0件のこの時点で誤発火しないようガードする
    // (呼び出し元がこの後レイヤーを追加してからemit layersChanged()する想定)
    m_initializing = true;
    doc_->resetToBlank();
    doc_->initTileGrid(canvasW, canvasH);
    m_initializing = false;

    emit layersChanged();
}

QByteArray CanvasWidget::readSlicePixels(int texArraySlice)
{
    makeCurrent();
    return undoRecorder_.readSlicePixels(toolCtx_, texArraySlice);
}
 
void CanvasWidget::writeSlicePixels(int texArraySlice, const QByteArray &raw)
{
    makeCurrent();
    undoRecorder_.writeSlicePixels(toolCtx_, texArraySlice, raw);
}

void CanvasWidget::writeSlicePixelsBatch(const QVector<int> &slices,
                                     const QVector<const QByteArray *> &data,
                                     const std::function<void(int)> &onProgress)
{
    makeCurrent();
    undoRecorder_.writeSlicesBatch(toolCtx_, slices, data, onProgress);
}

QVector<QByteArray> CanvasWidget::readSlicePixelsBatch(const QVector<int> &slices)
{
    makeCurrent();
    return undoRecorder_.readSlicesBatch(toolCtx_, slices);
}

// ===========================================================================
// 内部ヘルパー
// ===========================================================================
// growLayerTexArray() / allocContiguousSlices() / freeSlice() は
// LayerSliceAllocator に移動した(sliceAllocator_)

QByteArray CanvasWidget::loadShaderSource(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open shader:" << path;
        return {};
    }
    return file.readAll();
}

QColor CanvasWidget::toPreMulColor(const QColor &rawColor, float opacity) {
    float a = rawColor.alphaF() * opacity;
    return QColor::fromRgbF(
        rawColor.redF()   * a,
        rawColor.greenF() * a,
        rawColor.blueF()  * a,
        a
    );
}

QColor CanvasWidget::getPixelColor(const QPointF &widgetPos, bool referenceCanvas)
{
    if (doc_->layers.isEmpty()) return Qt::transparent;

    makeCurrent();

    QVector2D canvasPx = widgetToPixel(widgetPos);
    int sx = qRound(canvasPx.x());
    int sy = qRound(canvasPx.y());

    if (sx < 0 || sx >= canvasW || sy < 0 || sy >= canvasH) {
        return Qt::transparent;
    }

    // 全レイヤーを合成したキャンバス(compositedTex)を最新化する。
    // 【重要】この呼び出しは、下で読み取り用FBOを作って束縛するより「前」でなければ
    // ならない。updateCompositedTex()は内部で自前のFBOを使い、最後に
    // glBindFramebuffer(GL_FRAMEBUFFER, 0)(=読み書き両方を0に戻す)するため、
    // 先にこちらのFBOを束縛してから呼ぶと、それが外されたまま次の
    // glFramebufferTexture2D()がデフォルトフレームバッファに対する不正な操作になり、
    // 続くglReadPixels()がデフォルトフレームバッファ(Qtの描画先ではない)を読んで
    // 常に空=透明を返していた(スポイトの「プレビューから取得」が必ず透明になる不具合)。
    if (referenceCanvas)
        updateCompositedTex();

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);

    int readX = sx, readY = sy;

    if (referenceCanvas) {
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, toolCtx_.compositedTex, 0);
    } else {
        // 現在のレイヤーのみから拾う
        int tx = sx / TILE_SIZE;
        int ty = sy / TILE_SIZE;
        int si = doc_->activeLayer().tileSliceAtCanvasTile(tx, ty);
        if (si < 0) {
            glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
            glDeleteFramebuffers(1, &fbo);
            return Qt::transparent; // 通常起こらない(レイヤーは常にキャンバス全体を覆う)
        }
        readX = sx % TILE_SIZE;
        readY = sy % TILE_SIZE;
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  bankTexOf(si), 0, localSliceOf(si));
    }

    uint8_t pixel[4] = {};
    glReadPixels(readX, readY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    glDeleteFramebuffers(1, &fbo);

    // レイヤーのタイルも合成結果(compositedTex)も、中身は「乗算済みアルファ
    // (premultiplied)」で格納されている(描画色を作るtoPreMulColor()と、
    // common.glslのnormalBlend()=fg+bg*(1-fg.a)がその形式を前提にしている)。
    // 一方、この関数の戻り値を受け取る側(スポイト→ToolConfig::setRawRGBA())が
    // 扱うのは非乗算(ストレート)の生の色なので、ここでアルファを割り戻す。
    //
    // 割り戻しを忘れると、拾った色をそのまま描画色にして塗る→また拾う…の
    // 繰り返しで、塗るたびにRGBへアルファが二重・三重に掛かっていき、色が
    // 循環のたびに暗く(=RGBが0へ向かうので明度も彩度も低下)なっていく。
    const float a = pixel[3] / 255.0f;
    if (a <= 0.0f) return Qt::transparent; // 完全透明: 色情報が無い(呼び出し側で判断する)
    const auto unpremul = [a](uint8_t c) {
        return (int)qBound(0.0f, std::round(c / a), 255.0f);
    };
    return QColor(unpremul(pixel[0]), unpremul(pixel[1]), unpremul(pixel[2]), pixel[3]);
}

// ===========================================================================
// キャンバスの再構築（MainWindowから呼ばれる）
// ===========================================================================
void CanvasWidget::recreateCanvas(int w, int h, bool createDefaultLayers, bool wrapX, bool wrapY) {
    makeCurrent(); // OpenGLコンテキストをアクティブにする（必須）

    // 1. 古いテクスチャを破棄
    freeTextures();

    // 2. サイズ変数の更新
    canvasW = w;
    canvasH = h;

    // 3. ドキュメントの完全リセット
    // (所有権はCanvasWidget外にあるので、作り直すのではなく中身だけ空にする)
    sliceAllocator_.resetAllocationState();
    doc_->resetToBlank();
    doc_->setWrap(wrapX, wrapY);

    // 4. 新しいサイズでテクスチャ群を再生成し、初期レイヤーを作る
    // (createDefaultLayers=falseの場合はレイヤーを1枚も作らない。PsdCodec::loadが
    //  PSD側のレイヤー構成をそのまま復元するために使う)
    initTextures(createDefaultLayers);

    // canvasW/H・テクスチャIDが変わったので toolCtx_ も更新
    setupToolContext();

    fitCanvasToView();

    emit layersChanged();
    emit selectionChanged(hasSelection_); // initTextures()内でhasSelection_=falseにリセット済み
    update();
}

// ===========================================================================
// キャンバスサイズ変更(編集アクション「キャンバスサイズ変更」の確定処理)
// ---------------------------------------------------------------------------
// recreateCanvas()と違い、既存レイヤーの中身を保持したままキャンバスの
// 縦横サイズを変える(内側へのトリミング/外側への拡張どちらも可)。
// タイル境界の再配置を伴うため、いったん全レイヤーをCPU側のQImageへ読み出し、
// オフセット付きで新サイズのQImageに描き直してからタイルへ書き戻す
// (この操作の頻度は低いため、GPU compute化はせずCPUで完結させている)。
//
// タイルグリッド自体が変わる(=通常のTileUndo差分方式が使えない)ため、
// Undoは変更前後の全レイヤースナップショットを丸ごと保持するCanvasResizeエントリで行う
// (CanvasDocument::UndoKind::CanvasResize)。
// ===========================================================================
QVector<LayerSnapshotData> CanvasWidget::captureAllLayerSnapshots()
{
    QVector<LayerSnapshotData> result;
    result.reserve(doc_->layerCount());

    const int tilesX = doc_->tilesX(), tilesY = doc_->tilesY();
    for (int li = 0; li < doc_->layerCount(); li++) {
        const Layer &layer = doc_->layers[li];

        // 単色レイヤー/調整レイヤーは実ピクセルデータを持たない(単色は常に手続き的な
        // 不透明白、調整レイヤーは下のレイヤーへの色調整のみ)ため、タイルを読み出す
        // 必要はない。layerType/adjustmentだけスナップショットに残し、復元側
        // (rebuildCanvasFromSnapshots)で同じ種類のレイヤーとして作り直す。
        // テキストレイヤーはNormalと同様に実ピクセルを持つので、下の通常経路で
        // 画像として読み出す(text自体もメタデータとして一緒に保存する)。
        // マスクを持つ場合、種類に関わらずキャンバス全体ぶんのグレースケール画像として
        // 読み出しておく(単色/調整/フォルダーレイヤーでもマスク自体は独立して持てる)。
        QImage maskImg;
        if (layer.hasMask) {
            QVector<int> maskSlices;
            QVector<QPoint> maskTileCoords;
            for (int ty = 0; ty < tilesY; ty++) {
                for (int tx = 0; tx < tilesX; tx++) {
                    int si = layer.maskTileSlice(tx, ty);
                    if (si < 0) continue;
                    maskSlices.append(si);
                    maskTileCoords.append(QPoint(tx, ty));
                }
            }
            const QVector<QByteArray> rawMaskTiles = readSlicePixelsBatch(maskSlices);
            maskImg = QImage(canvasW, canvasH, QImage::Format_RGBA8888_Premultiplied);
            maskImg.fill(Qt::white);
            QPainter mp(&maskImg);
            mp.setCompositionMode(QPainter::CompositionMode_Source);
            for (int i = 0; i < maskSlices.size(); i++) {
                const int tx = maskTileCoords[i].x(), ty = maskTileCoords[i].y();
                const QByteArray &raw = rawMaskTiles[i];
                QImage tileImg(reinterpret_cast<const uchar*>(raw.constData()),
                                TILE_SIZE, TILE_SIZE, TILE_SIZE * 4, QImage::Format_RGBA8888_Premultiplied);
                const int dstX = tx * TILE_SIZE, dstY = ty * TILE_SIZE;
                const int w = qMin(TILE_SIZE, canvasW - dstX), h = qMin(TILE_SIZE, canvasH - dstY);
                mp.drawImage(QRect(dstX, dstY, w, h), tileImg, QRect(0, 0, w, h));
            }
            mp.end();
        }

        if (layer.layerType == LayerType::SolidColor || layer.layerType == LayerType::Adjustment
            || layer.layerType == LayerType::Filter || layer.layerType == LayerType::Folder) {
            LayerSnapshotData snap{ layer.name, layer.opacity, layer.visible, layer.blendMode,
                             layer.clipping, layer.layerType, layer.adjustment, layer.filter, layer.textBoxes,
                             layer.solidColor, QImage(), layer.childCount, layer.hasMask };
            snap.maskImage = maskImg;
            result.append(snap);
            continue;
        }

        // まずこのレイヤーの全タイルのslice番号(存在するもののみ)とキャンバス上の
        // タイル座標(tx,ty)を集め、readSlicePixelsBatch()でPBOパイプライン化して
        // まとめて読み出す(タイルごとに逐次glReadPixelsするより、GPU→CPU転送の
        // ストールが少ない)。
        QVector<int> slices;
        QVector<QPoint> tileCoords;
        for (int ty = 0; ty < tilesY; ty++) {
            for (int tx = 0; tx < tilesX; tx++) {
                int si = layer.tileSliceAtCanvasTile(tx, ty);
                if (si < 0) continue; // 通常起こらない(レイヤーは常にキャンバス全体を覆う)
                slices.append(si);
                tileCoords.append(QPoint(tx, ty));
            }
        }
        const QVector<QByteArray> rawTiles = readSlicePixelsBatch(slices);

        QImage img(canvasW, canvasH, QImage::Format_RGBA8888_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        for (int i = 0; i < slices.size(); i++) {
            const int tx = tileCoords[i].x(), ty = tileCoords[i].y();
            const QByteArray &raw = rawTiles[i]; // TILE_SIZE*TILE_SIZE*4 RGBA8
            QImage tileImg(reinterpret_cast<const uchar*>(raw.constData()),
                            TILE_SIZE, TILE_SIZE, TILE_SIZE * 4, QImage::Format_RGBA8888_Premultiplied);
            const int dstX = tx * TILE_SIZE, dstY = ty * TILE_SIZE;
            const int w = qMin(TILE_SIZE, canvasW - dstX), h = qMin(TILE_SIZE, canvasH - dstY);
            p.drawImage(QRect(dstX, dstY, w, h), tileImg, QRect(0, 0, w, h));
        }
        p.end();
        LayerSnapshotData snap{ layer.name, layer.opacity, layer.visible, layer.blendMode,
                         layer.clipping, layer.layerType, layer.adjustment, layer.filter, layer.textBoxes,
                         layer.solidColor, img, layer.childCount, layer.hasMask };
        snap.maskImage = maskImg;
        result.append(snap);
    }
    return result;
}

// snaps: 復元するレイヤー内容(各QImageは既にnewW x newHの解像度に合わせて描画済みである必要はなく、
// offsetX,offsetYを使って新キャンバス上の正しい位置に配置される)。
void CanvasWidget::rebuildCanvasFromSnapshots(int newW, int newH, const QVector<LayerSnapshotData> &snaps,
                                           int activeIndex, int offsetX, int offsetY)
{
    freeTextures();
    canvasW = newW;
    canvasH = newH;
    sliceAllocator_.resetAllocationState();
    doc_->resetToBlank();
    initTextures(/*createDefaultLayers=*/false);
    setupToolContext();

    // 途中経過のシグナル/GL更新は不要なので抑制し、最後にまとめて通知する。
    m_initializing = true;
    const int tilesX = doc_->tilesX(), tilesY = doc_->tilesY();
    for (const LayerSnapshotData &snap : snaps) {
        addLayer(snap.name, /*insertIndex=*/-1, snap.clipping,
                 /*originTx=*/0, /*originTy=*/0, /*tilesXOverride=*/-1, /*tilesYOverride=*/-1,
                 snap.layerType);
        const int newIdx = doc_->layerCount() - 1;
        doc_->setLayerOpacity(newIdx, snap.opacity);
        doc_->setLayerVisible(newIdx, snap.visible);
        doc_->setLayerBlendMode(newIdx, snap.blendMode);
        doc_->layerRef(newIdx).adjustment = snap.adjustment;
        doc_->layerRef(newIdx).filter     = snap.filter;
        doc_->layerRef(newIdx).textBoxes  = snap.textBoxes;
        doc_->layerRef(newIdx).solidColor = snap.solidColor;
        doc_->layerRef(newIdx).childCount = snap.childCount; // Folderのときのみ意味を持つ

        // マスクを持っていた場合、新しいキャンバスサイズ用にタイルを確保し直し、
        // 色レイヤーの中身と同じくoffsetX/offsetYぶんずらして書き戻す(マスクも
        // レイヤー本体と同じ位置関係を保つ必要があるため)。
        if (snap.hasMask && addLayerMask(newIdx)) {
            QImage newMaskImg(newW, newH, QImage::Format_RGBA8888_Premultiplied);
            newMaskImg.fill(Qt::white);
            {
                QPainter mp(&newMaskImg);
                mp.setCompositionMode(QPainter::CompositionMode_Source);
                mp.drawImage(QPoint(-offsetX, -offsetY), snap.maskImage);
            }
            const Layer &maskLayer = doc_->layers[newIdx];
            for (int ty = 0; ty < tilesY; ty++) {
                for (int tx = 0; tx < tilesX; tx++) {
                    int si = maskLayer.maskTiles[ty][tx];
                    QImage tile(TILE_SIZE, TILE_SIZE, QImage::Format_RGBA8888_Premultiplied);
                    tile.fill(Qt::white);
                    {
                        QPainter tp(&tile);
                        tp.setCompositionMode(QPainter::CompositionMode_Source);
                        tp.drawImage(0, 0, newMaskImg, tx * TILE_SIZE, ty * TILE_SIZE, TILE_SIZE, TILE_SIZE);
                    }
                    QByteArray raw(reinterpret_cast<const char*>(tile.constBits()), TILE_SIZE * TILE_SIZE * 4);
                    writeSlicePixels(si, raw);
                }
            }
        }

        // 単色レイヤー/調整レイヤー/フィルターレイヤー/フォルダーはタイルを持たない
        // (単色は手続き的な不透明白、調整とフィルターは下のレイヤーへの加工のみ、
        // フォルダーはUI上のマーカーのみ)ので、ピクセルの書き戻しは不要。
        // テキストレイヤーはNormalと同様に実ピクセルを持つので下の経路で書き戻す。
        if (snap.layerType == LayerType::SolidColor || snap.layerType == LayerType::Adjustment
            || snap.layerType == LayerType::Filter || snap.layerType == LayerType::Folder)
            continue;

        QImage newImg(newW, newH, QImage::Format_RGBA8888_Premultiplied);
        newImg.fill(Qt::transparent);
        {
            QPainter p(&newImg);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(QPoint(-offsetX, -offsetY), snap.image);
        }

        const Layer &newLayer = doc_->layers[newIdx];
        for (int ty = 0; ty < tilesY; ty++) {
            for (int tx = 0; tx < tilesX; tx++) {
                int si = newLayer.tiles[ty][tx];
                QImage tile(TILE_SIZE, TILE_SIZE, QImage::Format_RGBA8888_Premultiplied);
                tile.fill(Qt::transparent);
                {
                    QPainter tp(&tile);
                    tp.setCompositionMode(QPainter::CompositionMode_Source);
                    tp.drawImage(0, 0, newImg, tx * TILE_SIZE, ty * TILE_SIZE, TILE_SIZE, TILE_SIZE);
                }
                QByteArray raw(reinterpret_cast<const char*>(tile.constBits()), TILE_SIZE * TILE_SIZE * 4);
                writeSlicePixels(si, raw);
            }
        }
    }
    m_initializing = false;

    if (doc_->layerCount() > 0)
        doc_->setActiveLayer(qBound(0, activeIndex, doc_->layerCount() - 1));

    // 大量のglTexSubImage3D書き込み(writeSlicePixels)がGPU側でまだ実行中のうちに
    // 次のフレームのpaintGL()がこのlayerTexArrayを読みに行くと、タイミング次第で
    // ドライバ側の状態が不整合になりクラッシュしうる。update()で再描画を要求する前に
    // ここまでの書き込みを確実に完了させる。
    glFinish();

    fitCanvasToView();
    emit layersChanged();
    emit selectionChanged(hasSelection_); // initTextures()内でhasSelection_=falseにリセット済み
    update();
}

void CanvasWidget::applyCanvasResizeUndoEntry(const UndoEntry &entry, bool toBefore)
{
    const CanvasResizeUndoData &rd = entry.resize;
    if (toBefore) {
        rebuildCanvasFromSnapshots(rd.oldW, rd.oldH, rd.beforeLayers, rd.activeLayerIndexBefore, 0, 0);
        // rebuildCanvasFromSnapshots内のresetToBlank()でredoStackも失われるため、
        // このリサイズ自身をredoできるよう積み直す
        doc_->restoreRedoEntryAfterRebuild(entry);
    } else {
        rebuildCanvasFromSnapshots(rd.newW, rd.newH, rd.afterLayers, rd.activeLayerIndexAfter, 0, 0);
        doc_->restoreUndoEntryAfterRebuild(entry);
    }
}

bool CanvasWidget::resizeCanvasKeepingContent(int newW, int newH, int offsetX, int offsetY)
{
    if (newW <= 0 || newH <= 0) return false;
    if (!doc_ || doc_->layers.isEmpty()) return false;

    makeCurrent();

    const int oldW = canvasW, oldH = canvasH;
    const QVector<LayerSnapshotData> beforeLayers = captureAllLayerSnapshots();
    const int savedActiveIndex = doc_->activeLayerIndex();

    rebuildCanvasFromSnapshots(newW, newH, beforeLayers, savedActiveIndex, offsetX, offsetY);

    // Undo用に、変更後の状態も丸ごとスナップショットしておく
    // (低頻度の操作なので、GPUから読み直すシンプルな実装で構わない)
    UndoEntry entry;
    entry.kind = UndoKind::CanvasResize;
    entry.resize.oldW = oldW;
    entry.resize.oldH = oldH;
    entry.resize.newW = newW;
    entry.resize.newH = newH;
    entry.resize.beforeLayers = beforeLayers;
    entry.resize.afterLayers  = captureAllLayerSnapshots();
    entry.resize.activeLayerIndexBefore = qBound(0, savedActiveIndex, beforeLayers.size() - 1);
    entry.resize.activeLayerIndexAfter  = doc_->activeLayerIndex();
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());

    return true;
}

// ===========================================================================
// キャンバスの回転・反転(編集メニュー「ファイル」の回転/反転コマンド)
// ---------------------------------------------------------------------------
// 見た目(ViewTransform)ではなく、全レイヤーの実ピクセルデータを書き換える。
// resizeCanvasKeepingContent/resampleCanvasResolutionと同じ「スナップショット
// 取得→各レイヤーのQImageを変換→rebuildCanvasFromSnapshotsで焼き直す→
// CanvasResize Undoエントリを積む」パターンを流用する。
// ===========================================================================
bool CanvasWidget::rotateCanvas(int ccwDegrees)
{
    if (ccwDegrees != 90 && ccwDegrees != 180 && ccwDegrees != 270) return false;
    if (!doc_ || doc_->layers.isEmpty()) return false;

    // レイヤー構成を丸ごと作り直す破壊的な操作なので、他の編集アクションは
    // 実行前に確定せずキャンセルする(他のstart*Action系と同じ排他パターン)。
    actions_.cancelActive(); // 変形/色調整/フィルター/レイヤー編集系(controller管理アクション)は互いに排他

    makeCurrent();

    const int oldW = canvasW, oldH = canvasH;
    const bool swapDims = (ccwDegrees == 90 || ccwDegrees == 270);
    const int newW = swapDims ? oldH : oldW;
    const int newH = swapDims ? oldW : oldH;

    const QVector<LayerSnapshotData> beforeLayers = captureAllLayerSnapshots();
    const int savedActiveIndex = doc_->activeLayerIndex();

    // タイル格納(≒QImage)はY下向きの通常の画像座標系を採用しているため、
    // QTransform::rotate()に正の角度を渡すと画面上は時計回りになる。反時計回り
    // にするため符号を反転する。
    QTransform t;
    t.rotate(-ccwDegrees);
    QVector<LayerSnapshotData> rotatedLayers = beforeLayers;
    for (LayerSnapshotData &snap : rotatedLayers)
        if (!snap.image.isNull())
            snap.image = snap.image.transformed(t);

    rebuildCanvasFromSnapshots(newW, newH, rotatedLayers, savedActiveIndex, 0, 0);

    UndoEntry entry;
    entry.kind = UndoKind::CanvasResize;
    entry.resize.oldW = oldW;
    entry.resize.oldH = oldH;
    entry.resize.newW = newW;
    entry.resize.newH = newH;
    entry.resize.beforeLayers = beforeLayers;
    entry.resize.afterLayers  = captureAllLayerSnapshots();
    entry.resize.activeLayerIndexBefore = qBound(0, savedActiveIndex, beforeLayers.size() - 1);
    entry.resize.activeLayerIndexAfter  = doc_->activeLayerIndex();
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());

    return true;
}

bool CanvasWidget::flipCanvasHorizontal()
{
    return flipCanvasBy(true, false);
}

bool CanvasWidget::flipCanvasVertical()
{
    return flipCanvasBy(false, true);
}

bool CanvasWidget::flipCanvasBy(bool horizontal, bool vertical)
{
    if (!doc_ || doc_->layers.isEmpty()) return false;

    // レイヤー構成を丸ごと作り直す破壊的な操作なので、他の編集アクションは
    // 実行前に確定せずキャンセルする(他のstart*Action系と同じ排他パターン)。
    actions_.cancelActive(); // 変形/色調整/フィルター/レイヤー編集系(controller管理アクション)は互いに排他

    makeCurrent();

    const QVector<LayerSnapshotData> beforeLayers = captureAllLayerSnapshots();
    const int savedActiveIndex = doc_->activeLayerIndex();

    QVector<LayerSnapshotData> flippedLayers = beforeLayers;
    for (LayerSnapshotData &snap : flippedLayers)
        if (!snap.image.isNull()) snap.image = snap.image.mirrored(horizontal, vertical);

    rebuildCanvasFromSnapshots(canvasW, canvasH, flippedLayers, savedActiveIndex, 0, 0);

    UndoEntry entry;
    entry.kind = UndoKind::CanvasResize;
    entry.resize.oldW = canvasW;
    entry.resize.oldH = canvasH;
    entry.resize.newW = canvasW;
    entry.resize.newH = canvasH;
    entry.resize.beforeLayers = beforeLayers;
    entry.resize.afterLayers  = captureAllLayerSnapshots();
    entry.resize.activeLayerIndexBefore = qBound(0, savedActiveIndex, beforeLayers.size() - 1);
    entry.resize.activeLayerIndexAfter  = doc_->activeLayerIndex();
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());

    return true;
}

// ===========================================================================
// 画像解像度変更(編集アクション「画像解像度変更」の確定処理)
// ---------------------------------------------------------------------------
// キャンバスサイズ変更(resizeCanvasKeepingContent)と違い、内容を新キャンバス上の
// 同じ位置に置くのではなく、新しい解像度いっぱいに拡大縮小(リサンプル)して置く
// (見た目の縦横比・表示上のサイズは変えず、ピクセル密度だけを変える)。
// Undoの仕組みはキャンバスサイズ変更と全く同じ CanvasResize エントリを流用する
// (beforeLayers/afterLayersが自己完結した全体スナップショットなので、
// 「配置方法が offset か 拡大縮小 か」の違いはUndo/Redo適用時には関係ない)。
// ===========================================================================
bool CanvasWidget::resampleCanvasResolution(int newW, int newH)
{
    if (newW <= 0 || newH <= 0) return false;
    if (!doc_ || doc_->layers.isEmpty()) return false;

    makeCurrent();

    const int oldW = canvasW, oldH = canvasH;
    const QVector<LayerSnapshotData> beforeLayers = captureAllLayerSnapshots();
    const int savedActiveIndex = doc_->activeLayerIndex();

    // 各レイヤーの内容を新しい解像度いっぱいに拡大縮小してから配置する
    // (offsetX=offsetY=0で、既に新サイズちょうどに合わせた画像をそのまま貼るだけでよい)
    QVector<LayerSnapshotData> scaledLayers = beforeLayers;
    for (LayerSnapshotData &snap : scaledLayers)
        snap.image = snap.image.scaled(newW, newH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    rebuildCanvasFromSnapshots(newW, newH, scaledLayers, savedActiveIndex, 0, 0);

    UndoEntry entry;
    entry.kind = UndoKind::CanvasResize;
    entry.resize.oldW = oldW;
    entry.resize.oldH = oldH;
    entry.resize.newW = newW;
    entry.resize.newH = newH;
    entry.resize.beforeLayers = beforeLayers;
    entry.resize.afterLayers  = captureAllLayerSnapshots();
    entry.resize.activeLayerIndexBefore = qBound(0, savedActiveIndex, beforeLayers.size() - 1);
    entry.resize.activeLayerIndexAfter  = doc_->activeLayerIndex();
    doc_->pushUndo(entry);
    emit modifiedChanged(doc_->isModified());

    return true;
}
