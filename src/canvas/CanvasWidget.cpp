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

// CanvasWidget lifecycle, dependency wiring, and tool routing (the hub).

// 生成・破棄。
CanvasWidget::CanvasWidget(ToolConfig *toolCfg, QWidget *parent)
    : QOpenGLWidget(parent),
      toolCfg_(toolCfg),
      penTool_(false, &smoothingStrength, toolCfg_),
      eraserTool_(true, &smoothingStrength, toolCfg_),
      airbrushTool_(toolCfg_)
{
    setObjectName(QStringLiteral("canvasView"));
    setContentsMargins(0, 0, 0, 0);
    setMinimumSize(100, 100);
    setMouseTracking(true);
    // ストローク中の部分再描画(paintGL()のシザー処理参照)のため、フレーム間でFBOの内容を保持する。
    setUpdateBehavior(QOpenGLWidget::PartialUpdate);

    // CanvasDocumentはCanvasWidgetが所有する(1タブ = 1CanvasWidget = 1CanvasDocument)。
    doc_ = std::make_unique<CanvasDocument>(sliceAllocFn(), sliceFreeFn());

    setupToolContext();
    wireDocumentNotifications();

    // アクション群(色調整/フィルター等)のコントローラに host(=this)を渡し、登録する。
    actions_.setHost(*this);
    registerCanvasActions();

    fillTool_.setToolConfig(toolCfg_);
    dropperTool_.setToolConfig(toolCfg_);
    blurTool_.setToolConfig(toolCfg_);
    warpTool_.setToolConfig(toolCfg_);
    selectTool_.setToolConfig(toolCfg_);

    // フレームレート律速バッチ用の定期フラッシュタイマー(CanvasWidget.hのコメント参照)。
    inputFlushTimer_ = new QTimer(this);
    connect(inputFlushTimer_, &QTimer::timeout, this, [this]() {
        // 未描画の入力があるときだけ処理する。
        Tool *t = currentTool();
        if (!t || !t->hasPendingInput()) return;
        makeCurrent();
        t->flushPendingInput(toolCtx_);
        // 直前に同期repaint()が走っているなら、ここで積んでも同じ絵をもう1枚描き直すだけで、しかもその時点ではダーティ矩形が消費済みのため部分再描画(シザー)が効かず全面描画になる。
        if (!lastRepaintDoneClock_.isValid() || lastRepaintDoneClock_.elapsed() >= strokePaintIntervalMs_)
            update();
    });
    inputFlushTimer_->setInterval(12);

    // 事前合成キャッシュの先読み作成(CanvasWidget.hのcompositeCachePrewarmTimer_参照)。
    compositeCachePrewarmTimer_ = new QTimer(this);
    compositeCachePrewarmTimer_->setSingleShot(true);
    compositeCachePrewarmTimer_->setInterval(200);
    connect(compositeCachePrewarmTimer_, &QTimer::timeout, this, [this] { prewarmCompositeCaches(); });
}

void CanvasWidget::wireDocumentNotifications()
{
    if (!doc_) return;

    // doc_ の変更通知を1箇所だけで受け取り、必要な範囲だけ再描画/キャッシュ更新/シグナル発行を行う。
    doc_->onChanged = [this](CanvasDocument::ChangeKind kind) {
        if (m_initializing || m_suppressDocNotify) return;

        // 事前合成キャッシュ(アクティブより下/上)は「アクティブレイヤーとその上下のスタックが変化しない」間だけ有効。
        invalidateBelowCompositeCache();
        invalidateAboveCompositeCache();
        invalidateFilterChain();
        // 作り直しは「次のストローク開始時」ではなく、ここから少し落ち着いたアイドル時に先回りして行う(CanvasWidget.hのcompositeCachePrewarmTimer_参照)。
        scheduleCompositeCachePrewarm();

        switch (kind) {
        case CanvasDocument::ChangeKind::CacheAndNotify:
            update();
            emit layersChanged();
            break;
        case CanvasDocument::ChangeKind::NotifyOnly:
            emit layersChanged();
            break;
        case CanvasDocument::ChangeKind::RepaintOnly:
            update();
            break;
        case CanvasDocument::ChangeKind::ActiveLayerChanged:
            // マスク編集は対象レイヤーを選択している間だけ有効。
            if (editingMaskLayerIndex_ >= 0
                    && editingMaskLayerIndex_ != doc_->activeLayerIndex()) {
                const int previousMaskLayer = editingMaskLayerIndex_;
                setEditingMaskLayer(previousMaskLayer); // 同じindexを渡すとトグル解除
            }
            update();
            break;
        }
    };
}

void CanvasWidget::setupToolContext() {
    toolCtx_.gl   = this;
    toolCtx_.doc  = doc_.get();
    toolCtx_.view = &view_;
    toolCtx_.canvasW = canvasW;
    toolCtx_.canvasH = canvasH;
    toolCtx_.tileSize = TILE_SIZE;
    toolCtx_.wrapX = doc_ ? doc_->wrapX() : false;
    toolCtx_.wrapY = doc_ ? doc_->wrapY() : false;

    syncBanksToToolContext();
    toolCtx_.maskTex       = maskTex;
    toolCtx_.penTipTex     = penTipTex;
    toolCtx_.paperTex      = paperTex;
    toolCtx_.toneCurveLUTTex = toneCurveLUTTex;
    toolCtx_.gradientMapLUTTex = gradientMapLUTTex;
    toolCtx_.strokeStampSSBO = strokeStampSSBO_;
    toolCtx_.brushPaintSSBO  = brushPaintSSBO_;
    toolCtx_.computeBrushStateProgram = computeBrushStateProgram;
    toolCtx_.belowCompositeTex         = belowCompositeTex;
    toolCtx_.belowCompositeClipBaseTex = belowCompositeClipBaseTex;
    toolCtx_.fullLayerTex  = fullLayerTex;
    toolCtx_.selectionMaskTex = selectionMaskTex;
    toolCtx_.transformSrcTex        = transformSrcTex;
    toolCtx_.transformSrcSelMaskTex = transformSrcSelMaskTex;

    toolCtx_.compositedTex     = compositedTex;
    toolCtx_.compositedTileArr = compositedTileArr;

    toolCtx_.computeDrawProgram      = computeDrawProgram;
    toolCtx_.computeBakeProgram      = computeBakeProgram;
    toolCtx_.computeMaskClearProgram = computeMaskClearProgram;
    toolCtx_.computeCompositeProgram = computeCompositeProgram;
    toolCtx_.computeTransformProgram = computeTransformProgram;
    toolCtx_.computeFreeTransformProgram = computeFreeTransformProgram;
    toolCtx_.computeHueSatLightProgram = computeHueSatLightProgram;
    toolCtx_.computeBrightnessContrastProgram = computeBrightnessContrastProgram;
    toolCtx_.computeColorBalanceProgram = computeColorBalanceProgram;
    toolCtx_.computeToneCurveProgram = computeToneCurveProgram;
    toolCtx_.computeGaussianBlurFilterProgram = computeGaussianBlurFilterProgram;
    toolCtx_.computeMosaicReduceProgram = computeMosaicReduceProgram;
    toolCtx_.computeMosaicFilterProgram = computeMosaicFilterProgram;
    toolCtx_.computeMotionBlurFilterProgram = computeMotionBlurFilterProgram;
    toolCtx_.computeNoiseFilterProgram      = computeNoiseFilterProgram;
    toolCtx_.computeChromaticAberrationFilterProgram = computeChromaticAberrationFilterProgram;
    toolCtx_.computeLensBlurFilterProgram            = computeLensBlurFilterProgram;
    toolCtx_.computeGradientMapProgram               = computeGradientMapProgram;
    toolCtx_.resizeCanvasKeepingContent = [this](int w, int h, int ox, int oy) { return resizeCanvasKeepingContent(w, h, ox, oy); };
    toolCtx_.resampleCanvasResolution = [this](int w, int h) { return resampleCanvasResolution(w, h); };
    toolCtx_.growLayerBounds = [this](int layerIndex, int minTx, int minTy, int maxTxEx, int maxTyEx) {
        return growLayerBoundsToCoverCanvasTiles(layerIndex, minTx, minTy, maxTxEx, maxTyEx);
    };
    toolCtx_.ensureTransformScratchSize = [this](int w, int h) { ensureTransformScratchSize(w, h); };
    toolCtx_.ensureFilterScratch = [this](int w, int h) { return ensureFilterScratch(w, h); };

    toolCtx_.ssboLayerOpacity   = ssboLayerOpacity;
    toolCtx_.ssboLayerVisible   = ssboLayerVisible;
    toolCtx_.ssboLayerBaseSlice = ssboLayerBaseSlice;
    toolCtx_.ssboLayerMaskBaseSlice = ssboLayerMaskBaseSlice;
    toolCtx_.ssboLayerAncestorMaskSlices = ssboLayerAncestorMaskSlices;
    toolCtx_.ssboLayerBlendMode = ssboLayerBlendMode;
    toolCtx_.ssboLayerClipping  = ssboLayerClipping;
    toolCtx_.ssboLayerOriginTx  = ssboLayerOriginTx;
    toolCtx_.ssboLayerOriginTy  = ssboLayerOriginTy;
    toolCtx_.ssboLayerTilesX    = ssboLayerTilesX;
    toolCtx_.ssboLayerTilesY    = ssboLayerTilesY;
    toolCtx_.ssboLayerIsSolidColor = ssboLayerIsSolidColor;
    toolCtx_.ssboLayerSolidColor   = ssboLayerSolidColor;
    toolCtx_.ssboLayerAdjKind   = ssboLayerAdjKind;
    toolCtx_.ssboLayerAdjParams = ssboLayerAdjParams;
    toolCtx_.maxLayers          = MAX_LAYERS;

    toolCtx_.viewDpr       = viewDpr();
    toolCtx_.widgetToPixel = [this](const QPointF &p) { return widgetToPixel(p); };
    toolCtx_.pixelToWidget = [this](const QVector2D &canvasPos) -> QPointF {
        // ビューはデバイスピクセルで動くので、ウィジェット座標(論理px)へ戻す。
        QVector4D p = view_.matrix() * QVector4D(canvasPos.x(), canvasPos.y(), 0.0f, 1.0f);
        const float d = viewDpr();
        return QPointF(p.x() / d, (viewHeight() - p.y()) / d);
    };
    // ビュー空間(デバイスpx)での中心。
    toolCtx_.widgetCenter  = [this] { return QVector2D(viewWidth() / 2.0f, viewHeight() / 2.0f); };
    toolCtx_.setHasSelection = [this](bool v) {
        if (hasSelection_ == v) return;
        hasSelection_ = v;
        emit selectionChanged(hasSelection_);
    };
    toolCtx_.getHasSelection = [this] { return hasSelection_; };
    toolCtx_.invalidateSelectionOutline = [this] { invalidateSelectionOutlineCache(); };
    toolCtx_.beginSelectionUndo  = [this] { beginSelectionUndo(); };
    toolCtx_.commitSelectionUndo = [this](const QByteArray &afterRaw, int x, int y, int w, int h) {
        commitSelectionUndo(afterRaw, x, y, w, h);
    };
    toolCtx_.clearSelection      = [this] { clearSelection(); };
    toolCtx_.getPixelColor = [this](const QPointF &p, bool referenceCanvas) { return getPixelColor(p, referenceCanvas); };
    toolCtx_.activeBrushPreMulColor = [this] {
        return toPreMulColor(toolCfg_->color().rawRGBA(), toolCfg_->pen().opacity());
    };
    toolCtx_.notifyColorDropped = [this](QColor c, bool transparent) {
        emit colorDropperd(c, transparent);
    };
    toolCtx_.ensureStrokeColorTex = [this] { return ensureStrokeColorTex(); };
    toolCtx_.startOrEditTextBox   = [this](int boxIndex) { startOrEditTextBox(boxIndex); };
    toolCtx_.requestTextRasterize = [this](int layerIndex) { scheduleTextRasterize(layerIndex); };
    toolCtx_.beginStrokeUndo          = [this] { beginStrokeUndo(); };
    toolCtx_.expandStrokeUndoRegion   = [this](int a,int b,int c,int d) { expandStrokeUndoRegion(a,b,c,d); };
    toolCtx_.commitStrokeUndo         = [this] { commitStrokeUndo(); };
    toolCtx_.updateBelowCompositeCache     = [this](int upto) {
        if (!WinLog::enabled()) { updateBelowCompositeCache(upto); return; }
        const bool wasValid = belowCompositeCacheValid_;
        QElapsedTimer t; t.start();
        updateBelowCompositeCache(upto);
        WINLOG(QStringLiteral("PERF stroke: belowCache %1 %2ms")
                   .arg(wasValid ? QStringLiteral("hit") : QStringLiteral("BUILD")).arg(t.nsecsElapsed() / 1e6, 0, 'f', 2));
    };
    toolCtx_.invalidateBelowCompositeCache = [this] { invalidateBelowCompositeCache(); };
    toolCtx_.updateAboveCompositeCache     = [this](int active) {
        if (!WinLog::enabled()) { updateAboveCompositeCache(active); return; }
        const bool wasValid = aboveCompositeCacheValid_;
        QElapsedTimer t; t.start();
        updateAboveCompositeCache(active);
        WINLOG(QStringLiteral("PERF stroke: aboveCache %1 %2ms")
                   .arg(wasValid ? QStringLiteral("hit") : QStringLiteral("BUILD")).arg(t.nsecsElapsed() / 1e6, 0, 'f', 2));
    };
    toolCtx_.invalidateAboveCompositeCache = [this] { invalidateAboveCompositeCache(); };
    // MoveTool(パン)/RotateTool(回転)はctx.view->pan()/rotate()を直接呼んだ後ctx.requestRepaint()するだけなので、
    // ここでまとめてviewChanged()も発行する(NavigatorDockの表示範囲枠を追従させるため。他の大多数の呼び出し元にとっては無害な余分なemitになるだけ)。
    toolCtx_.requestRepaint           = [this] {
        // ストローク中(CanvasWidget側が一定間隔でrepaint()を直接呼んで駆動している間)はここでupdate()を積まない。
        Tool *t = currentTool();
        if (!(t && t->isActive() && t->needsCanvasRepaintWhileActive()))
            update();
        // viewChanged() は MoveTool(パン)/RotateTool(回転)がビューを動かしたことをNavigatorDockへ伝えるためのもの。
        const QMatrix4x4 m = view_.matrix();
        if (m == lastEmittedViewMatrix_) return;
        lastEmittedViewMatrix_ = m;

        // 「変わったときだけ」の条件は、ビューを動かさないペン等には効くが、動かすのが仕事の移動/回転ツールには何の効果も無い ― ドラッグ中は毎回変わるので毎イベント発行され、上に書いたNavigatorDockの再描画コストを丸ごと被る。
        const bool dragging = (t && t->isActive());
        if (dragging) {
            viewChangedPending_ = true;
            return;
        }
        viewChangedPending_ = false;
        emit viewChanged();
    };
    toolCtx_.notifyLayersChanged      = [this] { emit layersChanged(); };
    toolCtx_.noteStrokeDirtyRegion    = [this](float a,float b,float c,float d) { noteStrokeDirtyRegion(a,b,c,d); };
    // 各所でのglBindFramebuffer(ctx.defaultFbo())は、一時FBOでの作業。
    toolCtx_.defaultFbo               = [] { return (GLuint)0; };
    toolCtx_.updateCompositedTex      = [this] { updateCompositedTex(); };
}

void CanvasWidget::freeTextures() {
    // キャンバスサイズ変更(rebuildCanvasFromSnapshots)の直前には、captureAllLayerSnapshots()で大量のglReadPixelsを発行した直後にここへ来る。
    glFinish();

    for (int i = 0; i < MAX_TILE_BANKS; i++)
        if (layerTexBanks[i]) { glDeleteTextures(1, &layerTexBanks[i]); layerTexBanks[i] = 0; }
    if (maskTex)         { glDeleteTextures(1, &maskTex);         maskTex         = 0; }
    if (strokeColorTex)  { glDeleteTextures(1, &strokeColorTex);  strokeColorTex  = 0; }
    if (fullLayerTex)    { glDeleteTextures(1, &fullLayerTex);    fullLayerTex    = 0; }
    if (selectionMaskTex){ glDeleteTextures(1, &selectionMaskTex);selectionMaskTex= 0; }
    if (transformSrcTex)        { glDeleteTextures(1, &transformSrcTex);        transformSrcTex        = 0; }
    if (transformSrcSelMaskTex) { glDeleteTextures(1, &transformSrcSelMaskTex); transformSrcSelMaskTex = 0; }
    transformScratchW_ = 0;
    transformScratchH_ = 0;
    if (filterScratchTex_) { glDeleteTextures(1, &filterScratchTex_); filterScratchTex_ = 0; }
    filterScratchW_ = 0;
    filterScratchH_ = 0;
    freeFilterChainTextures();
    if (wallTex)         { glDeleteTextures(1, &wallTex);         wallTex         = 0; }
    if (outerJfaTex)     { glDeleteTextures(1, &outerJfaTex);     outerJfaTex     = 0; }
    if (innerJfaTex)     { glDeleteTextures(1, &innerJfaTex);     innerJfaTex     = 0; }
    if (sdfTex)          { glDeleteTextures(1, &sdfTex);          sdfTex          = 0; }
    if (compositedTex)       { glDeleteTextures(1, &compositedTex);       compositedTex       = 0; }
    if (compositedTileArr)   { glDeleteTextures(1, &compositedTileArr);   compositedTileArr   = 0; }
    if (belowCompositeTex)         { glDeleteTextures(1, &belowCompositeTex);         belowCompositeTex         = 0; }
    if (belowCompositeClipBaseTex) { glDeleteTextures(1, &belowCompositeClipBaseTex); belowCompositeClipBaseTex = 0; }
    if (aboveCompositeTex)          { glDeleteTextures(1, &aboveCompositeTex);          aboveCompositeTex          = 0; }
    if (aboveCompositeClipScratch_) { glDeleteTextures(1, &aboveCompositeClipScratch_); aboveCompositeClipScratch_ = 0; }
    belowCompositeCacheValid_ = false;

    GLuint ssbos[] = { ssboLayerOpacity, ssboLayerVisible, ssboLayerBaseSlice, ssboLayerMaskBaseSlice,
                   ssboLayerAncestorMaskSlices,
                   ssboLayerBlendMode, ssboLayerClipping,
                   ssboLayerOriginTx, ssboLayerOriginTy, ssboLayerTilesX, ssboLayerTilesY,
                   ssboLayerIsSolidColor, ssboLayerSolidColor,
                   ssboLayerAdjKind, ssboLayerAdjParams };
    glDeleteBuffers(15, ssbos);
}

CanvasWidget::~CanvasWidget() {
    // 一度も表示されなかったCanvasWidgetでは initializeGL() が走っていない。
    if (!glFunctionsReady_) {
        WINLOG(QStringLiteral("~CanvasWidget: initializeGL()未実行のためGLの後始末は不要"));
        return;
    }

    makeCurrent();
    actions_.releaseAllGL(); // CustomShaderActionの動的コンパイル済みプログラム等をcontextがあるうちに解放する

    // シェーダープログラムはここで delete しないこと。

    freeTextures();
    if (penTipTex) glDeleteTextures(1, &penTipTex);
    if (paperTex) glDeleteTextures(1, &paperTex);
    if (toneCurveLUTTex) glDeleteTextures(1, &toneCurveLUTTex);
    if (gradientMapLUTTex) glDeleteTextures(1, &gradientMapLUTTex);
    if (dummyVAO) glDeleteVertexArrays(1, &dummyVAO);
    doneCurrent();
}

// ツール / ブラシ API。
void CanvasWidget::setActiveTool(ToolType tool)
{
    // 色調整/フィルター系パネル(色相・彩度・明度/明るさ・コントラスト/カラーバランス/ガウスぼかし/モザイク)が開いている間は、
    // 移動・回転ツール以外への切り替えを無視する(パネルを開いたまま視点だけ調整できるようにするため。それ以外のツールショートカットは事実上無効になる)。
    const bool colorPanelActive = actions_.toolInputBlocked();
    if (colorPanelActive && tool != ToolType::Move && tool != ToolType::Rotate) return;

    // 他のツールに切り替えたら、実行中の変形アクションは破棄して終了する。
    if (!colorPanelActive) actions_.cancelActive();

    previousTool = activeTool;
    activeTool   = tool;
    emit activeToolChanged(tool);
    updateCursor();
    update();
}

void CanvasWidget::returnToPreviousTool()
{
    setActiveTool(previousTool);
}

// 色調整/フィルター系パネル(移動/回転ツールへの切替だけ素通しするタイプ)を開く直前に呼ばれる。
void CanvasWidget::hostNotifyToolBlockingActionStarted()
{
    toolBeforeBlockingAction_ = activeTool;
    toolBeforeBlockingActionSaved_ = true;
    setActiveTool(ToolType::Move);
}

// 上記パネルが確定/キャンセルで閉じた直後に呼ばれる。
void CanvasWidget::hostNotifyToolBlockingActionEnded()
{
    if (!toolBeforeBlockingActionSaved_) return;
    toolBeforeBlockingActionSaved_ = false;
    setActiveTool(toolBeforeBlockingAction_);
}

void CanvasWidget::setActiveToolPreset(ToolType tool, int index)
{
    if (IToolPresetList *list = toolCfg_->toolPresetList(tool))
        list->setActiveIndex(index);

    if (activeTool != tool) {
        setActiveTool(tool); // ToolType自体も切り替わる場合はactiveToolChanged側に委ねる
        return;
    }
    emit activeToolPresetChanged(tool, index);
    updateCursor();
    update();
}

// カーソル
void CanvasWidget::updateCursor()
{
    // 色調整/フィルター系パネル表示中は、移動・回転ツールに切り替えていればそのツール本来のカーソルを見せる(実際に操作できるため)。
    const bool colorPanelActive = actions_.toolInputBlocked();
    // 変形/自由変形/キャンバスサイズ変更は blocksToolInput()=false(ハンドルドラッグのためキャンバスへのマウス入力自体は必要)だが、カーソル表示上は他のアクション同様通常の矢印にしたいので、
    // ここでは isActive() で判定する(入力ブロック判定とは別軸)。
    const bool otherActionBlocking = (actions_.isBusy() && !colorPanelActive)
        || actions_.toolInputFullyBlocked();
    if (otherActionBlocking ||
        (colorPanelActive && activeTool != ToolType::Move && activeTool != ToolType::Rotate)) {
        applyUnsetCursor();
        return;
    }
    if (Tool *t = currentTool()) {
        if (std::optional<QCursor> c = t->cursor(toolCtx_)) {
            applyCursor(*c);
            return;
        }
    }
    for (const ToolTypeMeta &meta : toolTypeRegistry()) {
        if (meta.type == activeTool) {
            const QString &cursorPath = meta.cursorIconPath.isEmpty() ? meta.iconPath : meta.cursorIconPath;
            applyCursor(CursorUtils::makeIconCursor(cursorPath, 24));
            return;
        }
    }
    applyUnsetCursor();
}

// QCursorには比較演算子が無いので、「同じ見た目か」を安く判定するための識別子を作る。
static qint64 cursorIdentity(const QCursor &c)
{
    if (c.shape() != Qt::BitmapCursor)
        return -(qint64)c.shape() - 1; // 標準カーソル(画像カーソルのcacheKey>0と衝突しない負値)
    return c.pixmap().cacheKey();
}

// 変化したカーソルだけをOSへ反映する。
void CanvasWidget::applyCursor(const QCursor &c)
{
    const qint64 id = cursorIdentity(c);
    if (id == appliedCursorId_) return; // 見た目が同じ: OSには一切触らない
    appliedCursorId_ = id;
    setCursor(c);
    forceCursorRedrawIfUnderMouse();
}

void CanvasWidget::applyUnsetCursor()
{
    if (appliedCursorId_ == 0) return;
    appliedCursorId_ = 0;
    unsetCursor();
}

// Windowsでは、setCursor()でウィジェットのカーソルを変えても、OSは「WM_SETCURSORを受け取ったとき」にしかカーソル画像を評価し直さない。
void CanvasWidget::forceCursorRedrawIfUnderMouse()
{
#ifdef Q_OS_WIN
    // カーソルがこのウィジェット上にあるときだけ行う(他のウィジェット上にある間に呼んでも無意味なうえ、下のQCursor::setPos()が無関係なウィンドウへ影響する)。
    const QPoint globalPos = QCursor::pos();
    if (!rect().contains(mapFromGlobal(globalPos))) return;

    // (a) 本命: 自分のトップレベルウィンドウへWM_SETCURSORを送り、カーソル画像を評価し直させる。
    if (QWidget *top = window()) {
        if (QWindow *wh = top->windowHandle()) {
            HWND hwnd = reinterpret_cast<HWND>(wh->winId());
            if (hwnd)
                SendMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        }
    }

    // (b) 保険: カーソル位置を現在位置へ「0px移動」させ、実際にポインタを動かさずにOSへ再評価させる。
    QCursor::setPos(globalPos);
#endif
}


// 選択範囲
