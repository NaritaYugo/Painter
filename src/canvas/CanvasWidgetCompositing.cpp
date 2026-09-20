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

// Composite caches, filter chains, previews, and export rendering.

void CanvasWidget::updateBelowCompositeCache(int uptoExclusiveIndex)
{
    // 既に有効なら作り直さない。
    if (belowCompositeCacheValid_ && belowCompositeCacheUpto_ == uptoExclusiveIndex) return;

    if (!doc_ || doc_->layerCount() == 0 || !computeBelowCompositeProgram) {
        belowCompositeCacheValid_ = false;
        return;
    }

    compositor_.updateLayerSSBOs(toolCtx_);

    computeBelowCompositeProgram->bind();
    bindLayerBanksForSampling(computeBelowCompositeProgram); // タイル配列(全バンク)をサンプル用にバインド
    glBindImageTexture(0, belowCompositeTex,         0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(1, belowCompositeClipBaseTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    // 初期値とライブプレビューはこの用途では使わない(uUseInit=0 / uPaintPreview=0)。
    glBindImageTexture(2, belowCompositeTex,         0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(3, belowCompositeClipBaseTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(4, maskTex,          0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    glBindImageTexture(5, selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    computeBelowCompositeProgram->setUniformValue("uLayerCount", doc_->layerCount());
    computeBelowCompositeProgram->setUniformValue("uTargetStart", 0);
    computeBelowCompositeProgram->setUniformValue("uTargetEnd",  uptoExclusiveIndex - 1);
    computeBelowCompositeProgram->setUniformValue("uTileSize",   TILE_SIZE);
    computeBelowCompositeProgram->setUniformValue("uCanvasTilesX", doc_->tilesX());
    computeBelowCompositeProgram->setUniformValue("uUseInit",      0);
    computeBelowCompositeProgram->setUniformValue("uPaintPreview", 0);
    glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    computeBelowCompositeProgram->release();

    // イメージユニット0は、このコードベースの他の場所(stroke.comp/maskclear.comp)では「呼び出し側が直前に明示的にbindし直さなくても、
    // 常にmaskTexがバインドされている」という前提で使われている(それぞれの呼び出し末尾でmaskTexへ戻す形で維持されている暗黙の不変条件)。
    glBindImageTexture(0, maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);

    belowCompositeCacheValid_ = true;
    belowCompositeCacheUpto_  = uptoExclusiveIndex;
}

// 事前合成キャッシュの先読み作成を予約する(CanvasWidget.hのcompositeCachePrewarmTimer_参照)。
void CanvasWidget::scheduleCompositeCachePrewarm()
{
    compositeCachePrewarmDone_ = false;
    if (compositeCachePrewarmTimer_) compositeCachePrewarmTimer_->start();
}

// 上下の事前合成キャッシュを、ストローク開始を待たずにここで作っておく。
void CanvasWidget::prewarmCompositeCaches()
{
    if (!glReady_ || !doc_ || doc_->layerCount() == 0) return;
    if (compositeCachePrewarmDone_) return;

    // ストローク中(またはドラッグ操作中)はGPUを取り合わないよう見送り、終わってからやり直す。
    if (Tool *t = currentTool(); t && t->isActive()) {
        if (compositeCachePrewarmTimer_) compositeCachePrewarmTimer_->start();
        return;
    }
    // 非表示のタブ(別ペインへ切り替え済み等)では作らない。
    if (!isVisible()) return;

    compositeCachePrewarmDone_ = true;

    makeCurrent();
    const int active = doc_->activeLayerIndex();
    QElapsedTimer clock;
    clock.start();
    updateBelowCompositeCache(active);
    updateAboveCompositeCache(active);
    // ここまでで発行したディスパッチはGPUキューに積まれただけで、実処理は「次にpresentするフレーム」が待つことになる。
    update();
    WINLOG(QStringLiteral("PERF prewarm: composite caches submitted in %1ms (below=%2 above=%3)")
               .arg(clock.nsecsElapsed() / 1e6, 0, 'f', 2)
               .arg(belowCompositeCacheValid_ ? 1 : 0)
               .arg(aboveCompositeCacheValid_ ? 1 : 0));
}

void CanvasWidget::updateAboveCompositeCache(int activeIndex)
{
    // 既に有効なら作り直さない(updateBelowCompositeCacheの同種コメント参照)。
    if (aboveCompositeCacheValid_) return;

    aboveCompositeCacheValid_ = false;
    if (!doc_ || doc_->layerCount() == 0 || !computeBelowCompositeProgram) return;

    const int n = doc_->layerCount();
    if (activeIndex < 0 || activeIndex >= n - 1) return; // 上にレイヤーが無ければ作る意味なし

    // フォルダーのマスクを編集中は事前合成しない。
    if (doc_->layers[activeIndex].layerType == LayerType::Folder
        && editingMaskLayerIndex_ == activeIndex)
        return;

    // 有効化条件: アクティブより上のレイヤーが全て「通常ブレンド・非クリッピング・非調整レイヤー」であること。
    for (int z = activeIndex + 1; z < n; z++) {
        const Layer &ly = doc_->layers[z];
        if (ly.layerType == LayerType::Adjustment) return;
        // フィルターレイヤーも同じ理由で事前合成できない(効果が下の合成結果に依存するうえ、近傍参照なので1枚に畳んでから重ねることができない)。
        if (ly.layerType == LayerType::Filter) return;
        if (ly.blendMode != BlendMode::Normal) return;
        if (ly.clipping) return;
    }

    compositor_.updateLayerSSBOs(toolCtx_);

    computeBelowCompositeProgram->bind();
    bindLayerBanksForSampling(computeBelowCompositeProgram);
    glBindImageTexture(0, aboveCompositeTex,          0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(1, aboveCompositeClipScratch_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8); // clipBase出力は使わない捨て先
    glBindImageTexture(2, aboveCompositeTex,          0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(3, aboveCompositeClipScratch_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(4, maskTex,          0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    glBindImageTexture(5, selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    computeBelowCompositeProgram->setUniformValue("uLayerCount", n);
    computeBelowCompositeProgram->setUniformValue("uTargetStart", activeIndex + 1);
    computeBelowCompositeProgram->setUniformValue("uTargetEnd",  n - 1);
    computeBelowCompositeProgram->setUniformValue("uTileSize",   TILE_SIZE);
    computeBelowCompositeProgram->setUniformValue("uCanvasTilesX", doc_->tilesX());
    computeBelowCompositeProgram->setUniformValue("uUseInit",      0);
    computeBelowCompositeProgram->setUniformValue("uPaintPreview", 0);
    glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    computeBelowCompositeProgram->release();

    // イメージユニット0をmaskTexへ戻す(updateBelowCompositeCacheの同種コメント参照)。
    glBindImageTexture(0, maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);

    aboveCompositeCacheValid_ = true;
}

// フィルターレイヤーの連鎖 (CanvasWidget.h の filterChainResult_ のコメント参照)。

// このレイヤーが「実際に表示へ効いているフィルターレイヤー」か。
static bool filterLayerIsActive(const CanvasDocument &doc, int z,
                                const QVector<QVector<int>> &ancestorsPerLayer)
{
    const Layer &ly = doc.layers[z];
    if (ly.layerType != LayerType::Filter) return false;
    if (!ly.visible || ly.opacity <= 0.0f)  return false;
    for (int f : ancestorsPerLayer[z]) {
        if (f < 0 || f >= doc.layers.size()) continue;
        const Layer &folder = doc.layers[f];
        if (!folder.visible || folder.opacity <= 0.0f) return false;
    }
    return true;
}

int CanvasWidget::topmostActiveFilterLayer() const
{
    if (!doc_ || doc_->layerCount() == 0) return -1;
    // フィルターレイヤーが1枚も無い文書(大多数)では、ここで即座に抜けてcomputeAncestorFolders()のツリー走査すら行わない。
    bool any = false;
    for (const Layer &ly : doc_->layers)
        if (ly.layerType == LayerType::Filter) { any = true; break; }
    if (!any) return -1;

    const QVector<QVector<int>> ancestors = doc_->computeAncestorFolders();
    int top = -1;
    for (int z = 0; z < doc_->layerCount(); z++)
        if (filterLayerIsActive(*doc_, z, ancestors)) top = z;
    return top;
}

bool CanvasWidget::ensureFilterChainTextures()
{
    if (canvasW <= 0 || canvasH <= 0) return false;
    if (filterChainResult_[0]) return true;
    for (int i = 0; i < 2; i++) {
        // GL_LINEAR は必須。
        filterChainResult_[i] = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
        filterChainClip_[i]   = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
    }
    filterChainBlurScratch_ = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
    return true;
}

void CanvasWidget::freeFilterChainTextures()
{
    for (int i = 0; i < 2; i++) {
        if (filterChainResult_[i]) { glDeleteTextures(1, &filterChainResult_[i]); filterChainResult_[i] = 0; }
        if (filterChainClip_[i])   { glDeleteTextures(1, &filterChainClip_[i]);   filterChainClip_[i]   = 0; }
    }
    if (filterChainBlurScratch_) { glDeleteTextures(1, &filterChainBlurScratch_); filterChainBlurScratch_ = 0; }
    filterChainOutResult_ = 0;
    filterChainOutClip_   = 0;
    filterChainValid_     = false;
    filterChainStartZ_    = -1;
}

void CanvasWidget::dispatchCompositeSegment(int targetStart, int targetEnd,
                                        bool useInit, int initResultIdx, int initClipIdx,
                                        int dstResultIdx, int dstClipIdx, bool withPaintPreview)
{
    auto *prog = computeBelowCompositeProgram;
    prog->bind();
    bindLayerBanksForSampling(prog);

    glBindImageTexture(0, filterChainResult_[dstResultIdx], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(1, filterChainClip_[dstClipIdx],     0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    // 初期値。
    const int inR = useInit ? initResultIdx : (dstResultIdx ^ 1);
    const int inC = useInit ? initClipIdx   : (dstClipIdx   ^ 1);
    glBindImageTexture(2, filterChainResult_[inR], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(3, filterChainClip_[inC],   0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(4, maskTex,          0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    glBindImageTexture(5, selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);

    prog->setUniformValue("uLayerCount",   doc_->layerCount());
    prog->setUniformValue("uTargetStart",  targetStart);
    prog->setUniformValue("uTargetEnd",    targetEnd);
    prog->setUniformValue("uTileSize",     TILE_SIZE);
    prog->setUniformValue("uCanvasTilesX", doc_->tilesX());
    prog->setUniformValue("uUseInit",      useInit ? 1 : 0);

    // ライブプレビュー(フィルターレイヤーより下に描いているときだけ)。
    prog->setUniformValue("uPaintPreview", withPaintPreview ? 1 : 0);
    if (withPaintPreview) {
        // paintGL が render.frag へ渡すのと同じ考え方(あちらのコメント参照)。
        const float brushOpacity = (activeTool == ToolType::Airbrush)
                                 ? toolCfg_->airbrush().opacity() : toolCfg_->pen().opacity();
        const EraseBrush erase = eraseBrushFor(activeTool == ToolType::Eraser,
                                               toolCfg_->color(), brushOpacity);
        const QColor col = erase.active ? erase.shaderColor()
                                        : toPreMulColor(toolCfg_->color().rawRGBA(), brushOpacity);
        const QColor maskCol = erase.active
            ? maskBrushColor(true, toolCfg_->color().rawRGBA(), erase.strength)
            : maskBrushColor(false, toolCfg_->color().rawRGBA(), brushOpacity);
        prog->setUniformValue("uActiveLayerIndex", doc_->activeLayerIndex());
        prog->setUniformValue("uEraseMode", erase.active ? 1 : 0);
        prog->setUniformValue("uBrushBlendMode",
            (activeTool == ToolType::Pen && !erase.active) ? toolCfg_->pen().brushBlendMode() : 0);
        const bool useStrokeColor = (activeTool == ToolType::Pen && !erase.active
                                     && toolCfg_->pen().usesPerStampColor() && strokeColorTex != 0);
        prog->setUniformValue("uUseStrokeColor", useStrokeColor ? 1 : 0);
        if (useStrokeColor)
            glBindImageTexture(6, strokeColorTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        prog->setUniformValue("uIsEditingMaskLayer",
            (editingMaskLayerIndex_ >= 0 && editingMaskLayerIndex_ == doc_->activeLayerIndex()) ? 1 : 0);
        prog->setUniformValue("uBrushColor", col.redF(), col.greenF(), col.blueF(), col.alphaF());
        prog->setUniformValue("uMaskBrushColor",
            maskCol.redF(), maskCol.greenF(), maskCol.blueF(), maskCol.alphaF());
        prog->setUniformValue("uHasSelection", hasSelection_ ? 1 : 0);
    }

    glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    prog->release();
}

bool CanvasWidget::applyFilterLayer(int z, int srcIdx)
{
    const Layer &ly = doc_->layers[z];

    // フォルダーの不透明度・マスクをカスケードする(CanvasCompositor::updateLayerSSBOsと同じ考え方。フィルターレイヤーは合成ループを通らないのでここで自前に行う)。
    const QVector<QVector<int>> ancestors = doc_->computeAncestorFolders();
    float effOpacity = ly.opacity;
    int   ancMask[4] = { -1, -1, -1, -1 };
    int   maskSlot = 0;
    for (int f : ancestors[z]) {
        if (f < 0 || f >= doc_->layerCount()) continue;
        const Layer &folder = doc_->layers[f];
        effOpacity *= folder.opacity;
        if (folder.hasMask && !folder.maskTiles.isEmpty() && maskSlot < 4)
            ancMask[maskSlot++] = folder.maskTiles[0][0];
    }
    const int ownMask = (ly.hasMask && !ly.maskTiles.isEmpty()) ? ly.maskTiles[0][0] : -1;

    // 「不透明度×レイヤーマスク」に関わるuniformは2シェーダーで名前も意味も同じ(chromaticAberrationLayer.comp / gaussianBlurLayer.comp 共通)なのでまとめる。
    auto setMaskOpacityUniforms = [&](QOpenGLShaderProgram *prog) {
        prog->setUniformValue("uOpacity",    effOpacity);
        prog->setUniformValue("uTileSize",   TILE_SIZE);
        prog->setUniformValue("uCanvasTilesX", doc_->tilesX());
        prog->setUniformValue("uMaskBase",   ownMask);
        GLint loc = glGetUniformLocation(prog->programId(), "uAncestorMaskBases");
        if (loc >= 0) glUniform4i(loc, ancMask[0], ancMask[1], ancMask[2], ancMask[3]);
    };

    if (ly.filter.kind == FilterKind::ChromaticAberration) {
        auto *prog = computeChromaticAberrationLayerProgram;
        if (!prog) return false; // 無料版ビルド: フィルターレイヤーは効果なしで素通り

        prog->bind();
        bindLayerBanksForSampling(prog);
        glBindImageTexture(0, filterChainResult_[srcIdx],     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainResult_[srcIdx ^ 1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        prog->setUniformValue("uMode",       ly.filter.caMode);
        prog->setUniformValue("uAngleRad",   (float)qDegreesToRadians(ly.filter.caAngleDeg));
        prog->setUniformValue("uDistancePx", ly.filter.caDistancePx);
        prog->setUniformValue("uCenterPx",   QVector2D(ly.filter.caCenterU * canvasW,
                                                       ly.filter.caCenterV * canvasH));
        setMaskOpacityUniforms(prog);

        glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        prog->release();
        return true;
    }

    if (ly.filter.kind == FilterKind::GaussianBlur) {
        auto *prog = computeGaussianBlurLayerProgram;
        if (!prog) return false; // 通常は常に存在する(無料版でも使える機能のため)

        // 2次元ガウスは分離できるので「横1D→縦1D」の2パス
        const float sigma = qMax(0.5f, ly.filter.blurRadiusPx / 2.0f);
        const int   radius = qMax(1, (int)qRound(ly.filter.blurRadiusPx));
        const GLuint gx = (canvasW + 15) / 16, gy = (canvasH + 15) / 16;

        prog->bind();
        bindLayerBanksForSampling(prog);
        prog->setUniformValue("uKernelRadius", radius);
        prog->setUniformValue("uSigma", sigma);
        prog->setUniformValue("uWrapX", doc_->wrapX() ? 1 : 0);
        prog->setUniformValue("uWrapY", doc_->wrapY() ? 1 : 0);
        setMaskOpacityUniforms(prog);

        // 1パス目: 横ぼかしを中間バッファへ。
        glBindImageTexture(0, filterChainResult_[srcIdx], 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainBlurScratch_,     0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glBindImageTexture(2, filterChainResult_[srcIdx],  0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8); // uOrig(1パス目では未使用)
        {
            GLint loc = glGetUniformLocation(prog->programId(), "uDir");
            if (loc >= 0) glUniform2i(loc, 1, 0);
        }
        prog->setUniformValue("uFinalPass", 0);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

        // 2パス目: 縦ぼかし + 不透明度・マスクでのブレンドを仕上げる。
        glBindImageTexture(0, filterChainBlurScratch_,         0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainResult_[srcIdx ^ 1],  0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glBindImageTexture(2, filterChainResult_[srcIdx],      0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8); // uOrig
        {
            GLint loc = glGetUniformLocation(prog->programId(), "uDir");
            if (loc >= 0) glUniform2i(loc, 0, 1);
        }
        prog->setUniformValue("uFinalPass", 1);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

        prog->release();
        return true;
    }

    if (ly.filter.kind == FilterKind::MotionBlur) {
        auto *prog = computeMotionBlurLayerProgram;
        if (!prog) return false; // 通常は常に存在する(無料版でも使える機能のため)

        // 経路長に見合ったサンプル数(MotionBlurTool::sampleCountと同じ考え方だが、特定のレイヤーに紐付かないのでキャンバスサイズを基準にする)。
        float pathPx;
        if (ly.filter.mbMode == 0) {
            pathPx = ly.filter.mbDistancePx;
        } else {
            const float cx = ly.filter.mbCenterU * canvasW;
            const float cy = ly.filter.mbCenterV * canvasH;
            const float dx = qMax(cx, (float)canvasW - cx);
            const float dy = qMax(cy, (float)canvasH - cy);
            const float maxR = std::sqrt(dx * dx + dy * dy);
            pathPx = maxR * qDegreesToRadians(ly.filter.mbAngleSpanDeg);
        }
        const int samples = qBound(1, (int)std::lround(pathPx) + 1, 128);

        prog->bind();
        bindLayerBanksForSampling(prog);
        glBindImageTexture(0, filterChainResult_[srcIdx],     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainResult_[srcIdx ^ 1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        prog->setUniformValue("uMode",         ly.filter.mbMode);
        prog->setUniformValue("uAngleRad",     (float)qDegreesToRadians(ly.filter.mbAngleDeg));
        prog->setUniformValue("uDistancePx",   ly.filter.mbDistancePx);
        prog->setUniformValue("uCenterPx",     QVector2D(ly.filter.mbCenterU * canvasW, ly.filter.mbCenterV * canvasH));
        prog->setUniformValue("uAngleSpanRad", (float)qDegreesToRadians(ly.filter.mbAngleSpanDeg));
        prog->setUniformValue("uSamples",      samples);
        setMaskOpacityUniforms(prog);

        glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        prog->release();
        return true;
    }

    if (ly.filter.kind == FilterKind::LensBlur) {
        auto *prog = computeLensBlurLayerProgram;
        if (!prog) return false; // 無料版ビルド: フィルターレイヤーは効果なしで素通り

        // LensBlurTool::sampleCount/bokehGamma と同じ式。
        const int   samples    = qBound(12, (int)std::lround(ly.filter.lbRadiusPx * 6.0f), 192);
        const float bokehGamma = 1.0f + ly.filter.lbHighlightBoost * 3.0f;

        prog->bind();
        bindLayerBanksForSampling(prog);
        glBindImageTexture(0, filterChainResult_[srcIdx],     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainResult_[srcIdx ^ 1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        prog->setUniformValue("uRadiusPx",       ly.filter.lbRadiusPx);
        prog->setUniformValue("uSamples",        samples);
        prog->setUniformValue("uBlades",         ly.filter.lbBlades);
        prog->setUniformValue("uBladeRotRad",    (float)qDegreesToRadians(ly.filter.lbBladeRotDeg));
        prog->setUniformValue("uBokehGamma",     bokehGamma);
        prog->setUniformValue("uThreshold",      ly.filter.lbThreshold);
        // シェーダー側の重みは 1 + boost * smoothstep(...) なので、LensBlurToolと同じくスライダー1.0でハイライトが周囲の数倍の重みになるようスケールする。
        prog->setUniformValue("uHighlightBoost", ly.filter.lbHighlightBoost * 8.0f);
        setMaskOpacityUniforms(prog);

        glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        prog->release();
        return true;
    }

    if (ly.filter.kind == FilterKind::Mosaic) {
        auto *reduceProg = computeMosaicReduceLayerProgram;
        auto *prog = computeMosaicLayerProgram;
        if (!reduceProg || !prog) return false; // 通常は常に存在する(無料版でも使える機能のため)

        const int blockSize = qBound(2, ly.filter.mzBlockSize, 256);
        const int gridX = (canvasW + blockSize - 1) / blockSize;
        const int gridY = (canvasH + blockSize - 1) / blockSize;

        // 1パス目: ブロック平均を filterChainBlurScratch_(ガウスぼかしレイヤーと共用の中間バッファ)の左上へ集約する(詳細は mosaicReduceLayer.comp 参照)。
        reduceProg->bind();
        glBindImageTexture(0, filterChainResult_[srcIdx], 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainBlurScratch_,     0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        reduceProg->setUniformValue("uBlockSize", blockSize);
        glDispatchCompute((gridX + 7) / 8, (gridY + 7) / 8, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        reduceProg->release();

        // 2パス目: ブロック平均を引いて不透明度・マスクでブレンドする。
        prog->bind();
        bindLayerBanksForSampling(prog);
        glBindImageTexture(0, filterChainResult_[srcIdx],     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainResult_[srcIdx ^ 1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glBindImageTexture(2, filterChainBlurScratch_,         0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        prog->setUniformValue("uBlockSize", blockSize);
        setMaskOpacityUniforms(prog);

        glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        prog->release();
        return true;
    }

    if (ly.filter.kind == FilterKind::Noise) {
        auto *prog = computeNoiseLayerProgram;
        if (!prog) return false; // 通常は常に存在する(無料版でも使える機能のため)

        prog->bind();
        bindLayerBanksForSampling(prog);
        glBindImageTexture(0, filterChainResult_[srcIdx],     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
        glBindImageTexture(1, filterChainResult_[srcIdx ^ 1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        // NoiseTool::updatePreviewと同じ式(スライダー100%でストレート色を最大±0.5ずらす)。
        prog->setUniformValue("uAmount",     ly.filter.nsStrength * 0.5f);
        prog->setUniformValue("uMonochrome", ly.filter.nsMonochrome ? 1 : 0);
        prog->setUniformValue("uGrainPx",    ly.filter.nsGrainPx);
        {
            GLint loc = glGetUniformLocation(prog->programId(), "uSeed");
            if (loc >= 0) glUniform1ui(loc, (GLuint)ly.filter.nsSeed);
        }
        setMaskOpacityUniforms(prog);

        glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        prog->release();
        return true;
    }

    return false;
}

void CanvasWidget::rebuildFilterChain(int startZ, bool withPaintPreview)
{
    filterChainValid_ = false;
    if (!doc_ || !computeBelowCompositeProgram) return;
    if (!ensureFilterChainTextures()) return;

    compositor_.updateLayerSSBOs(toolCtx_);
    const QVector<QVector<int>> ancestors = doc_->computeAncestorFolders();

    int  ri = 0, ci = 0;      // 現在の結果/clipBaseが入っているping-pong面
    bool haveInit = false;    // ri/ci が有効か(=1区間でも合成済みか)
    int  segStart = 0;

    // [segStart, segEnd] を合成して ri/ci を進める。
    auto composeUpTo = [&](int segEnd) {
        if (segEnd < segStart) return;
        const int dr = haveInit ? (ri ^ 1) : 0;
        const int dc = haveInit ? (ci ^ 1) : 0;
        dispatchCompositeSegment(segStart, segEnd, haveInit, ri, ci, dr, dc, withPaintPreview);
        ri = dr; ci = dc; haveInit = true;
    };

    for (int z = 0; z < startZ && z < doc_->layerCount(); z++) {
        if (!filterLayerIsActive(*doc_, z, ancestors)) continue;

        composeUpTo(z - 1);
        if (!haveInit) {
            // 一番下がフィルターレイヤーで、その下に何も無い場合。
            dispatchCompositeSegment(0, -1, false, 0, 0, 0, 0, withPaintPreview);
            ri = 0; ci = 0; haveInit = true;
        }
        if (applyFilterLayer(z, ri)) ri ^= 1; // clipBaseはフィルターを通さないのでそのまま
        segStart = z + 1;
    }

    // 一番上のフィルターレイヤーより上に残っている区間(表示用途では空、書き出し用途では最上位レイヤーまで)。
    composeUpTo(qMin(startZ, doc_->layerCount()) - 1);

    if (!haveInit) {
        // フィルターレイヤーが1枚も効いていない、かつ合成すべき区間も無い
        return;
    }

    filterChainOutResult_ = filterChainResult_[ri];
    filterChainOutClip_   = filterChainClip_[ci];
    filterChainStartZ_    = startZ;
    filterChainValid_     = true;

    // イメージユニット0をmaskTexへ戻す(updateBelowCompositeCacheの同種コメント参照)。
    glBindImageTexture(0, maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);
}

int CanvasWidget::prepareCompositeBase(GLuint &outResult, GLuint &outClip)
{
    outResult = belowCompositeTex;
    outClip   = belowCompositeClipBaseTex;

    const int topFilter = topmostActiveFilterLayer();
    if (topFilter < 0) {
        // フィルターレイヤーが無い(大多数の文書)。
        return belowCompositeCacheValid_ ? doc_->activeLayerIndex() : -1;
    }

    // フィルターレイヤーがある間は、ストローク用の下キャッシュ(フィルターの効果を含まない)は使えないので、必ず連鎖の結果から始める。
    const int startZ = topFilter + 1;
    // アクティブレイヤーがフィルターより下にある間は、ストロークの線そのものが連鎖の入力になるので毎フレーム作り直す必要がある。
    const bool strokingBelowFilter =
        (currentTool() && currentTool()->isActive() && doc_->activeLayerIndex() <= topFilter);

    if (!filterChainValid_ || filterChainStartZ_ != startZ || strokingBelowFilter)
        rebuildFilterChain(startZ, strokingBelowFilter);

    if (!filterChainValid_) return -1;
    outResult = filterChainOutResult_;
    outClip   = filterChainOutClip_;
    return startZ;
}


// 書き出し。
QImage CanvasWidget::exportCanvas()
{
    makeCurrent();
    // フィルターレイヤーを含む文書は、タイル単位の composite.comp では正しく書き出せない(近傍参照なのでタイルの外が見えない)。
    if (topmostActiveFilterLayer() >= 0) {
        QImage img = renderExportViaFilterChain();
        if (!img.isNull()) return img;
        qWarning() << "exportCanvas: フィルター連鎖の書き出しに失敗したため、"
                      "フィルターレイヤーを無視した結果を返します";
    }
    return compositor_.renderExport(toolCtx_);
}

// 全レイヤー(フィルター適用済み)をキャンバス全面で合成して読み戻す。
QImage CanvasWidget::renderExportViaFilterChain()
{
    if (!doc_ || doc_->layerCount() == 0) return QImage();

    rebuildFilterChain(doc_->layerCount(), /*withPaintPreview=*/false);
    // 表示用のキャッシュとしては「最上位まで合成済み」は使えないので、次の paintGL で表示用に組み直させる。
    const bool ok = filterChainValid_;
    const GLuint outTex = filterChainOutResult_;
    invalidateFilterChain();
    if (!ok || !outTex) return QImage();

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outTex, 0);

    QVector<uint8_t> buf(canvasW * canvasH * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, canvasW, canvasH, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    glDeleteFramebuffers(1, &fbo);

    QImage img(buf.constData(), canvasW, canvasH,
               canvasW * 4, QImage::Format_RGBA8888_Premultiplied);
    return img.copy().convertToFormat(QImage::Format_RGBA8888).mirrored(false, true);
}

bool CanvasWidget::exportToImage(const QString &filePath)
{
    // 既存の関数を呼び出して合成済みの QImage を取得。
    QImage img = exportCanvas();

    if (img.isNull()) {
        return false; // 画像の取得に失敗した場合
    }

    // 指定されたパスに画像を保存 (フォーマットは拡張子から Qt が自動判定してくれます)。
    return img.save(filePath, nullptr, 100);
}

// 保存。
