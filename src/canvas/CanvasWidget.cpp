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

// ===========================================================================
// 生成・破棄
// ===========================================================================
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
    // ストローク中の部分再描画(paintGL()のシザー処理参照)のため、フレーム間で
    // FBOの内容を保持する。既定のNoPartialUpdateだとpaint間で内容が保証されず、
    // 「変更された矩形だけ描き直して残りは前フレームを使う」ことができない。
    setUpdateBehavior(QOpenGLWidget::PartialUpdate);

    // CanvasDocumentはCanvasWidgetが所有する(1タブ = 1CanvasWidget = 1CanvasDocument)。
    // sliceAllocFn/sliceFreeFnはコールバックを保持するだけなので、GLコンテキストが
    // まだ無いこの時点で構築しても問題ない。
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
    // ストローク中(mousePressEvent〜mouseReleaseEvent)だけ動かす。
    // このタイマーは「入力が途切れた瞬間」に末尾のスタンプを描くフォールバック。
    // ストローク中の主たる描画駆動はmouseMoveEvent内の時間スロットリングrepaintが
    // 行う(Windowsでは連続入力中WM_TIMERが配信されないため、タイマーはドラッグ中は
    // ほぼ発火せず、指を止めてキューが空いた瞬間に発火して取りこぼしを拾う)。
    inputFlushTimer_ = new QTimer(this);
    connect(inputFlushTimer_, &QTimer::timeout, this, [this]() {
        // 【重要】まだ画面に出していない入力が溜まっているときだけ動く。
        //
        // このタイマーの役目は「入力が途切れて mouseMoveEvent 側の同期repaint()が
        // 来なくなったときに、貯めたままの末尾を描く」ことだけ。溜まっていないなら
        // 画面は既に最新なので、flushもupdateも要らない。
        //
        // 以前は溜まりの有無を見ずに毎回 update() を積んでいた。ペンのように
        // 「連続入力中はWM_TIMERが配信されない」ツールでは滅多に発火しないので
        // 表には出なかったが、移動/回転ツールでは事情が逆になる ―― 1フレームの
        // 同期描画(=ウィンドウ全体の再合成+present)がこの環境で15〜65msかかるため、
        // その間に入力キューが空になってタイマーが必ず配信される。結果、
        //   本物のフレーム → 同じ絵の無駄なフレーム → 本物のフレーム → …
        // と交互に走り、1入力あたりのpresentが2回になっていた(実測でそのとおりの
        // 並びがログに出ている)。ボタンを押したまま指を止めている間も、同じ絵の
        // 再合成が25msごとに永久に続いていた。これがビュー変換のカクつきの主因。
        Tool *t = currentTool();
        if (!t || !t->hasPendingInput()) return;
        makeCurrent();
        t->flushPendingInput(toolCtx_);
        // 直前に同期repaint()が走っているなら、ここで積んでも同じ絵をもう1枚
        // 描き直すだけで、しかもその時点ではダーティ矩形が消費済みのため部分再描画
        // (シザー)が効かず全面描画になる。presentの回数も倍になり、そのぶん入力処理が
        // 止まる。前回の同期描画から間隔ぶん経っているときだけ積む。
        //
        // 基準は strokePaintClock_ ではなく「repaint()が終わった時刻」にすること。
        // strokePaintClock_ は repaint() の開始時に restart される(そちらのコメント
        // 参照)ので、repaint() 自体が間隔より長い場合 ― present待ちを含むと普通に
        // そうなる ― 戻ってきた瞬間には既に間隔を超えており、この判定が素通しに
        // なってしまう。結果、1フレームごとに必ず全面描画がもう1枚積まれていた
        // (実測: 移動ツールのドラッグ中、1フレームに paintGL が2回)。
        if (!lastRepaintDoneClock_.isValid() || lastRepaintDoneClock_.elapsed() >= strokePaintIntervalMs_)
            update();
    });
    inputFlushTimer_->setInterval(12);

    // 事前合成キャッシュの先読み作成(CanvasWidget.hのcompositeCachePrewarmTimer_参照)。
    // レイヤー操作は連続して起きる(スライダーのドラッグ、複数枚の追加など)ので、
    // 落ち着いてから1回だけ作るようデバウンスする。
    compositeCachePrewarmTimer_ = new QTimer(this);
    compositeCachePrewarmTimer_->setSingleShot(true);
    compositeCachePrewarmTimer_->setInterval(200);
    connect(compositeCachePrewarmTimer_, &QTimer::timeout, this, [this] { prewarmCompositeCaches(); });
}

void CanvasWidget::wireDocumentNotifications()
{
    if (!doc_) return;

    // doc_ の変更通知を1箇所だけで受け取り、必要な範囲だけ再描画/キャッシュ更新/
    // シグナル発行を行う。各アクションは元の各セッターが個別に呼んでいたものと
    // 1対1で対応させてあり、安易な統合はしていない
    // (例: 空のレーンに切り替えた直後にupdateCaches()を呼ぶとクラッシュするため、
    //  ActiveLayerChangedとRepaintOnlyは意図的に分けてある)。
    doc_->onChanged = [this](CanvasDocument::ChangeKind kind) {
        if (m_initializing || m_suppressDocNotify) return;

        // 事前合成キャッシュ(アクティブより下/上)は「アクティブレイヤーとその上下の
        // スタックが変化しない」間だけ有効。レイヤー切り替え・追加/削除/並べ替え・
        // ブレンド/不透明度/表示切り替え・Undo/Redo など、ストロークの実描画以外の
        // あらゆる文書変更で作り直す必要がある。ここで毎回無効化しておけば、次の
        // ストローク開始時(onMousePress)に必要なら作り直される。
        //
        // 重要: ストローク中・終了時のプレビュー更新(ctx.requestRepaint /
        // notifyLayersChanged / commitStrokeUndo)は doc_->onChanged を一切経由しない
        // ため、ここでの無効化は「連続して短い線を引く」ケースでのキャッシュ保持を
        // 妨げない。同じアクティブレイヤーへ続けて描く限りキャッシュは再利用され、
        // 1ストロークごとに全画面の事前合成をやり直すこと(=書き始めが遅くなる原因)は
        // 起きない。作り直すのはレイヤーを切り替えた等、本当に必要なときだけ。
        invalidateBelowCompositeCache();
        invalidateAboveCompositeCache();
        invalidateFilterChain();
        // 作り直しは「次のストローク開始時」ではなく、ここから少し落ち着いた
        // アイドル時に先回りして行う(CanvasWidget.hのcompositeCachePrewarmTimer_参照)。
        // 全面ディスパッチのGPU処理をペンを置く前に済ませておくのが狙い。
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
            // マスク編集は対象レイヤーを選択している間だけ有効。レイヤードックの
            // クリック以外にも追加・削除・Undo等でアクティブレイヤーは変わるため、
            // UI側ではなく文書通知の集約点で解除する。これにより描画先を本体へ戻し、
            // マスク編集を示す青いキャンバス背景も同時に通常色へ戻る。
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
        // ビューはデバイスピクセルで動くので、ウィジェット座標(論理px)へ戻す
        QVector4D p = view_.matrix() * QVector4D(canvasPos.x(), canvasPos.y(), 0.0f, 1.0f);
        const float d = viewDpr();
        return QPointF(p.x() / d, (viewHeight() - p.y()) / d);
    };
    // ビュー空間(デバイスpx)での中心。回転ツールがここを軸に回す。
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
    // MoveTool(パン)/RotateTool(回転)はctx.view->pan()/rotate()を直接呼んだ後
    // ctx.requestRepaint()するだけなので、ここでまとめてviewChanged()も発行する
    // (NavigatorDockの表示範囲枠を追従させるため。他の大多数の呼び出し元にとっては
    // 無害な余分なemitになるだけ)。
    toolCtx_.requestRepaint           = [this] {
        // ストローク中(CanvasWidget側が一定間隔でrepaint()を直接呼んで駆動している間)は
        // ここでupdate()を積まない。積むと、同じ内容をもう1枚——しかもストローク中の
        // 部分再描画(シザー)が効かない全面描画で——描き直すことになる。present の
        // 回数も倍になり、そのぶんvsync待ちで入力処理が止まる(実測でストローク中、
        // partial=1のフレームと交互にpartial=0のフレームが挟まっていた)。
        // 駆動は mouseMoveEvent 側の時間スロットリングrepaint()と、入力が途切れた
        // ときに末尾を拾う inputFlushTimer_ に任せる。
        Tool *t = currentTool();
        if (!(t && t->isActive() && t->needsCanvasRepaintWhileActive()))
            update();
        // viewChanged() は MoveTool(パン)/RotateTool(回転)がビューを動かしたことを
        // NavigatorDockへ伝えるためのもの。ここは全ツール共通の再描画要求なので、
        // ペンのようにビューを動かさないツールでも入力イベントごとに発行していた。
        // するとNavigatorDockが毎回再描画され、そのトップレベルのバックingストア同期に
        // 巻き込まれてCanvasWidgetのpaintGL()が「全面」でもう1回呼ばれる(ストローク中の
        // シザー部分再描画が帳消しになるうえ、presentの回数も倍になり、そのぶん
        // vsync待ちで入力処理が止まる)。実際にビュー変換が変わったときだけ発行する。
        const QMatrix4x4 m = view_.matrix();
        if (m == lastEmittedViewMatrix_) return;
        lastEmittedViewMatrix_ = m;

        // 「変わったときだけ」の条件は、ビューを動かさないペン等には効くが、
        // 動かすのが仕事の移動/回転ツールには何の効果も無い ― ドラッグ中は毎回
        // 変わるので毎イベント発行され、上に書いたNavigatorDockの再描画コストを
        // 丸ごと被る。実測(移動ツールでキャンバスをドラッグ)では
        //   paintGL 本体 0.29ms に対して repaint() 全体が 25〜33ms、
        //   1フレームに paintGL が2回、フレーム間隔 45〜60ms(約20fps)
        // となっていて、これが「キャンバスがゆっくり付いてくる」の正体だった。
        //
        // 【重要】ドラッグ中は1回も出さない(間引きではなく完全に止める)。
        //
        // 以前はここを100msに1回まで間引いていたが、それでも足りなかった。この環境で
        // 効くのは「発行の回数」ではなく「ウィンドウを再合成してpresentする回数」で、
        // その1回が実測15〜65msかかる。NavigatorDockのupdate()は自分の枠を描き直す
        // だけの軽い処理に見えて、こちらのrepaint()とは別のタイミングでもう1回
        // ウィンドウ全体の再合成を起こすため、間引いた10回/秒がそのまま
        // 「本物のフレームの合間に挟まる無駄なフレーム10枚/秒」になっていた
        // (実測ログでは viewChanged() の直後に必ず update 由来の paintGL が1枚入る)。
        //
        // NavigatorDockが出しているのは表示範囲の枠とズーム値だけで、ドラッグを
        // 離した時点(mouseReleaseEvent → emitViewChangedIfPending)で必ず1回出すので、
        // 最終的な表示は正しい位置に落ち着く。動かしている最中だけ枠が追従しなくなるが、
        // そのぶんキャンバス自体がポインタに素直に付いてくる方を採る。
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
    // 各所でのglBindFramebuffer(ctx.defaultFbo())は、一時FBOでの作業(glReadPixels/
    // glBlitFramebuffer等)が終わった後の後片付け(バインドを外すだけ)のためだけに
    // 使われており、その後Qt自身のpaintGL()が呼ばれる際に改めて自分のFBOを明示的に
    // 束縛し直すため、ここでどのFBOに戻すかは実質どうでもよい。
    // QOpenGLWidget::defaultFramebufferObject()自体は、一部の統合GPUドライバ環境
    // (Intel UHD Graphics 630で確認)でmousePressEvent等のペイントサイクル外から
    // 呼ぶと不定期にクラッシュすることが分かったため、素の0に固定しておく
    // (0はQt自身の描画先ではないが、上記の理由によりここでは無害)。
    toolCtx_.defaultFbo               = [] { return (GLuint)0; };
    toolCtx_.updateCompositedTex      = [this] { updateCompositedTex(); };
}

void CanvasWidget::freeTextures() {
    // キャンバスサイズ変更(rebuildCanvasFromSnapshots)の直前には、captureAllLayerSnapshots()で
    // 大量のglReadPixelsを発行した直後にここへ来る。glReadPixelsはブロッキングだが、
    // その前後で発行されたテクスチャ確保/削除コマンド自体の完了は保証しないため、
    // 削除→即座に再生成という激しいリソース入れ替えの直前で明示的にglFinish()して
    // GPU側の処理を確実に完了させてから削除する(タイミング依存のクラッシュを防ぐ)。
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
    //
    // そのときは initializeOpenGLFunctions() も呼ばれていないので、QOpenGLFunctions
    // の関数テーブルが未解決のまま。この状態で glFinish() などを呼ぶと未解決の
    // ポインタを辿ってアクセス違反で落ちる。実際 MainWindow::createWidgets() が
    // Dockのコンストラクタ用に作る deckCanvasWidget_ がまさにこれで、アプリを終了する
    // たびに毎回ここで落ちていた(終了コード 0xC0000005。ウィンドウは先に閉じ、
    // 設定の保存も済んでいるので気づきにくいが、終了が遅くなる原因になっていた)。
    //
    // initializeGL() が走っていなければGL資源も1つも作られていないので、
    // 解放すべきものは何も無い。まるごと飛ばす。
    if (!glFunctionsReady_) {
        WINLOG(QStringLiteral("~CanvasWidget: initializeGL()未実行のためGLの後始末は不要"));
        return;
    }

    makeCurrent();
    actions_.releaseAllGL(); // CustomShaderActionの動的コンパイル済みプログラム等をcontextがあるうちに解放する

    // シェーダープログラムはここで delete しないこと。initializeGL() の
    // programCache がプロセス全体で1組だけ持ち、全タブで共有している
    // (共有グループ内ではプログラムを使い回せる。詳細はそちらのコメント参照)。
    // 以前はここで全部 delete していたが、共有した状態でそれをやると
    // 「1つのタブを閉じただけで他のタブのプログラムが壊れる」ことになる。
    // GL資源はプロセス終了時、共有グループの破棄と一緒に解放される。

    freeTextures();
    if (penTipTex) glDeleteTextures(1, &penTipTex);
    if (paperTex) glDeleteTextures(1, &paperTex);
    if (toneCurveLUTTex) glDeleteTextures(1, &toneCurveLUTTex);
    if (gradientMapLUTTex) glDeleteTextures(1, &gradientMapLUTTex);
    if (dummyVAO) glDeleteVertexArrays(1, &dummyVAO);
    doneCurrent();
}

// ===========================================================================
// ツール / ブラシ API
// ===========================================================================
void CanvasWidget::setActiveTool(ToolType tool)
{
    // 色調整/フィルター系パネル(色相・彩度・明度/明るさ・コントラスト/カラー
    // バランス/ガウスぼかし/モザイク)が開いている間は、移動・回転ツール以外への
    // 切り替えを無視する(パネルを開いたまま視点だけ調整できるようにするため。
    // それ以外のツールショートカットは事実上無効になる)。Enter/Escapeは
    // confirm/cancelの別経路で処理されるためこのガードの対象外。
    // 色調整/フィルター系パネル(controller管理アクション)が入力をブロックしている間は、
    // 移動・回転ツール以外への切り替えを無視する(パネルを開いたまま視点操作できるように)。
    const bool colorPanelActive = actions_.toolInputBlocked();
    if (colorPanelActive && tool != ToolType::Move && tool != ToolType::Rotate) return;

    // 他のツールに切り替えたら、実行中の変形アクションは破棄して終了する。
    // 色調整/フィルター系パネルは、上のガードにより「移動/回転へ切替」時しかここに来ないため、
    // その場合はパネルを開いたまま(cancelしない)にする
    // (colorPanelActiveなら現在のcontroller管理アクションは色調整/フィルター系のはずなので触らない)。
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

// 色調整/フィルター系パネル(移動/回転ツールへの切替だけ素通しするタイプ)を開く
// 直前に呼ばれる。開いた瞬間からキャンバスを動かせるよう、その場で移動ツールへ
// 自動的に切り替える(回転ツールへはこれまで通りショートカットで切り替えられる)。
void CanvasWidget::hostNotifyToolBlockingActionStarted()
{
    toolBeforeBlockingAction_ = activeTool;
    toolBeforeBlockingActionSaved_ = true;
    setActiveTool(ToolType::Move);
}

// 上記パネルが確定/キャンセルで閉じた直後に呼ばれる。パネルを開く前に使っていた
// ツールへ戻す(パネル表示中に移動/回転ツールへ切り替えていた場合も、それは無視して
// 開く前のツールへ戻す)。
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

// ===========================================================================
// カーソル
// ---------------------------------------------------------------------------
// currentTool()->cursor() がカスタムカーソル(ペンの円、スポイトの色プレビュー等)を
// 返せばそれを使い、std::nullopt ならToolRegistryに登録されたアイコンを
// カーソルとして使う(新しいツールを追加した際、cursor()をオーバーライドしなくても
// 自動でそれらしいカーソルになる)。
// ===========================================================================
void CanvasWidget::updateCursor()
{
    // 色調整/フィルター系パネル表示中は、移動・回転ツールに切り替えていればそのツール
    // 本来のカーソルを見せる(実際に操作できるため)。それ以外(変形/キャンバスサイズ等の
    // 他のアクション中、あるいは移動・回転以外のツールのまま)は通常の矢印カーソルにする。
    const bool colorPanelActive = actions_.toolInputBlocked();
    // 変形/自由変形/キャンバスサイズ変更は blocksToolInput()=false(ハンドルドラッグの
    // ためキャンバスへのマウス入力自体は必要)だが、カーソル表示上は他のアクション同様
    // 通常の矢印にしたいので、ここでは isActive() で判定する(入力ブロック判定とは別軸)。
    // 画像解像度変更/レイヤー編集系パネルはblocksAllToolInput()=true(移動・回転への
    // 切替も許さない)なので、色調整/フィルター系と違って常に矢印カーソルにする。
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
// 画像カーソルはQPixmapのcacheKey(暗黙共有なのでコピーは浅く、cacheKey取得も安い)、
// 標準カーソルは形状値から作る。CursorUtils::makeCircleCursor()等は同じ引数に対して
// キャッシュ済みの同一QCursorを返すため、見た目が変わらない限り同じ値になる。
static qint64 cursorIdentity(const QCursor &c)
{
    if (c.shape() != Qt::BitmapCursor)
        return -(qint64)c.shape() - 1; // 標準カーソル(画像カーソルのcacheKey>0と衝突しない負値)
    return c.pixmap().cacheKey();
}

// 【重要・ペンタブの遅延対策】カーソルが実際に変化したときだけOSへ反映する。
//
// updateCursor()はマウス/ペンの移動イベントごとに呼ばれるが、ストローク中に
// カーソルの見た目が変わることはまずない。にもかかわらず毎回setCursor()＋
// forceCursorRedrawIfUnderMouse()を実行していると、後者の中で呼ぶ
// QCursor::setPos()(= Win32 SetCursorPos)が入力イベントを1件システムの入力
// キューへ注入してしまう。
//
// これはペンタブでだけ深刻な害になる:
//  - マウスのWM_MOUSEMOVEはWindowsが間引く(キューに高々1件)ため、同じ座標への
//    SetCursorPosはほぼ無害。
//  - ペンはパケットが間引かれず200Hz超で届くので、1パケットごとに1件ずつ
//    入力イベントが注入され続ける。結果として「入力キューが空にならない」状態が
//    自作自演で維持され、低優先度メッセージであるWM_PAINT/WM_TIMERが配信されなく
//    なる(このファイルのmouseMoveEvent()のコメント参照)。「ペンだと線が遅れて
//    ついてくる/止めた瞬間にまとめて出る」の主因。
//  - さらにSetCursorPosはプロセス横断で直列化される重いシステムコールで、
//    ペンのデジタイザが決めたカーソル位置と毎回競合する。
//
// 元々forceCursorRedrawIfUnderMouse()が必要だったのは「ブラシサイズを変えたのに
// ペンが静止していて新しいカーソル画像に切り替わらない」ケース、つまりカーソルが
// 変化したときだけ。移動中はポインタ自体が動いておりOSが自然に再評価するので不要。
// よって変化検出を挟むことで、当初の不具合を直したまま注入を完全に止められる。
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

// Windowsでは、setCursor()でウィジェットのカーソルを変えても、OSは
// 「WM_SETCURSORを受け取ったとき」にしかカーソル画像を評価し直さない。
// WM_SETCURSORはマウスメッセージに伴って飛ぶので、
//   ・マウス使用時   … わずかな手ぶれで即座に飛ぶため、ほぼ問題にならない
//   ・ペンタブ使用時 … ペン先が静止していれば当然飛ばないし、さらにこのアプリは
//                      ストローク中のレガシーマウスメッセージを止めている
//                      (tabletEvent()の直接駆動)ため、ペンを動かしていても
//                      飛んでこないことがある
// という差が出る。結果として「ショートカットでブラシサイズやツールを変えたのに、
// 実際の設定だけ変わってカーソルの絵が古いまま」という症状になる。キャンバス外の
// UIへ一度出して戻したりマウスで動かすと直るのは、そこで実マウスメッセージが
// 発生してWM_SETCURSORが飛ぶため。
//
// そこで、カーソルが実際に変化したとき(applyCursor参照)だけ、OSへ明示的に
// 再評価を要求する。
void CanvasWidget::forceCursorRedrawIfUnderMouse()
{
#ifdef Q_OS_WIN
    // カーソルがこのウィジェット上にあるときだけ行う(他のウィジェット上にある間に
    // 呼んでも無意味なうえ、下のQCursor::setPos()が無関係なウィンドウへ影響する)。
    //
    // 判定にQWidget::underMouse()は使えない。あれはQtのenter/leave追跡に基づくが、
    // その追跡は実マウスイベント由来であり、ペンタブ直接駆動中はまさにそれを
    // 止めているため false のままになることがある(この関数が呼ばれても素通りして
    // しまい、上記の症状が直らない原因そのもの)。座標で直接判定する。
    const QPoint globalPos = QCursor::pos();
    if (!rect().contains(mapFromGlobal(globalPos))) return;

    // (a) 本命: 自分のトップレベルウィンドウへWM_SETCURSORを送り、カーソル画像を
    //     評価し直させる。Qtのウィンドウプロシージャがこれを受けて、ポインタ下の
    //     ウィジェットのカーソルを適用してくれる。入力イベントを注入しないので、
    //     ペン入力のキューを乱さない(applyCursorのコメント参照)。
    //     winId()はトップレベル(既にネイティブ)に対して呼ぶ。QOpenGLWidget自身に
    //     対して呼ぶとネイティブウィンドウ化を強制してしまい、合成方法が変わるため避ける。
    if (QWidget *top = window()) {
        if (QWindow *wh = top->windowHandle()) {
            HWND hwnd = reinterpret_cast<HWND>(wh->winId());
            if (hwnd)
                SendMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        }
    }

    // (b) 保険: カーソル位置を現在位置へ「0px移動」させ、実際にポインタを動かさずに
    //     OSへ再評価させる。(a)が効かない環境(ドライバがカーソルを独自に描いている等)
    //     向けのフォールバック。カーソルが変化したときにしか通らないので、以前のように
    //     ペンのパケットごとに入力を注入してしまう問題は起きない。
    QCursor::setPos(globalPos);
#endif
}


// ===========================================================================
// 選択範囲
// ===========================================================================
// ---------------------------------------------------------------------------
// 選択範囲のUndo/Redo
// ---------------------------------------------------------------------------
// マスクはR8でキャンバス全域ぶんあるが、実際には0か255が大きな塊で続くだけなので
// qCompressでよく縮む(通常の選択なら数KB程度)。履歴に何十件積んでも問題にならない。
