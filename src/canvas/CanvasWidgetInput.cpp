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

// Pointer/tablet input routing and input scheduling.

float CanvasWidget::resolvePointerPressure(const QMouseEvent *event) const
{
    constexpr qint64 kTabletFreshnessMs = 80;
    if (lastTabletEventClock_.isValid() && lastTabletEventClock_.elapsed() < kTabletFreshnessMs)
        return lastTabletPressure_;
    if (!event->points().isEmpty())
        return event->points().first().pressure();
    return 1.0f;
}

// 生の筆圧(0..1)を、実際にツールへ渡す筆圧へ写像する。
//
// 環境設定の「全体の筆圧カーブ」→ アクティブツールの筆圧カーブ、の順に通す
// (PressureCurve のコメント参照)。全体側でタブレットの硬さの癖を一度ならし、
// そのうえでツール/プリセットごとの効き方を作る、という二段構え。
//
// 保持している生の値(brushPressure / lastTabletPressure_)は素のままにしておき、
// Tool::setPressure() へ渡す直前のここだけで写像する。そうしておかないと、
// tabletEvent() が合成マウスイベントを流す経路(driveMouse)で二重に適用される。
// 直近のタブレットイベント由来の傾き・回転をツールへ渡す。
// 判定は resolvePointerPressure と同じ「直近にタブレットイベントが来ているか」で、
// マウス操作中は傾きも回転も無いものとして0を渡す。
void CanvasWidget::applyPointerTilt(Tool *tool) const
{
    if (!tool) return;
    constexpr qint64 kTabletFreshnessMs = 80;
    const bool tablet = lastTabletEventClock_.isValid()
                     && lastTabletEventClock_.elapsed() < kTabletFreshnessMs;
    if (tablet) tool->setTilt(lastTabletTiltAmount_, lastTabletTiltAngle_, lastTabletRotation_);
    else        tool->setTilt(0.0f, 0.0f, 0.0f);
}

float CanvasWidget::mapPressure(float rawPressure) const
{
    if (!toolCfg_) return rawPressure;

    float p = toolCfg_->globalPressureCurve().apply(rawPressure);

    // 筆圧を使うツールだけがカーブを持つ(ToolConfig参照)。
    switch (activeTool) {
    case ToolType::Pen:      p = toolCfg_->pen().pressureCurve().apply(p);      break;
    case ToolType::Eraser:   p = toolCfg_->eraser().pressureCurve().apply(p);   break;
    case ToolType::Airbrush: p = toolCfg_->airbrush().pressureCurve().apply(p); break;
    case ToolType::Blur:     p = toolCfg_->blur().pressureCurve().apply(p);     break;
    case ToolType::Warp:     p = toolCfg_->warp().pressureCurve().apply(p);     break;
    default: break;
    }
    return p;
}

void CanvasWidget::mousePressEvent(QMouseEvent *event) {
    // 分割表示での操作権の移動(クリックしたペインをアクティブにする)は、キャンバスに
    // 限らずスタートページやタブバーのクリックでも効く必要があるため、ここではなく
    // MainWindow::eventFilter()側でqApp全体のマウス押下を見て一括で処理している。
    if (!glReady_) return; // initializeGL()完了前にマウス操作が届いた場合は無視する
    // ペン先ストロークをtabletEvent()から直接駆動している間、OS互換レイヤー由来の
    // 実マウスイベントは重複ストリームなので無視する(CanvasWidget.hのtabletDriving_
    // コメント参照)。ただしTabletReleaseの取りこぼし等でフラグが残った場合に
    // マウスが死なないよう、タブレットイベントが途絶えて久しければ解除する。
    if (tabletDriving_ && !dispatchingSyntheticTabletMouse_) {
        if (lastTabletEventClock_.isValid() && lastTabletEventClock_.elapsed() > 500)
            tabletDriving_ = false; // 保険(以後は通常のマウス操作として続行)
        else
            return;
    }
    if (event->button() != Qt::LeftButton) return;
    // controller管理アクション(色収差の円形ハンドル等)は、パネル表示中でも
    // (activeToolに関係なく)ヒットした場合だけ最優先で処理する。ヒットしなければ
    // 何もせず、この後の通常のツール排他ガード(Move/Rotateのみ通す)に委ねる。
    if (actions_.routeMousePress(event, toolCtx_)) {
        updateCursor();
        return;
    }
    // パネル自体はQWidgetなので自分でイベントを受け取る。パネル外は無視。ただし
    // 色調整/フィルター系パネル表示中に移動・回転ツールへ切り替えている場合だけは、
    // パネルを開いたまま視点操作できるように素通しする。
    {
        const bool colorPanelActive = actions_.toolInputBlocked();
        const bool otherPanelBlocking = actions_.toolInputFullyBlocked();
        if (otherPanelBlocking) return;
        if (colorPanelActive && activeTool != ToolType::Move && activeTool != ToolType::Rotate) return;
    }

    makeCurrent();
    if (Tool *t = currentTool()) {
        QElapsedTimer pressClock;
        if (WinLog::enabled()) pressClock.start();
        t->setPressure(mapPressure(resolvePointerPressure(event)));
        applyPointerTilt(t);
        t->onMousePress(event, toolCtx_);
        // 入力の滞留を測る基準をここに置く(1ドラッグごとにリセット)。
        inputBaseTimestamp_ = event->timestamp();
        inputBaseClock_.start();
        inputAgeMs_ = 0;
        const qint64 nsAfterTool = WinLog::enabled() ? pressClock.nsecsElapsed() : 0;
        // フレームレート律速バッチ(CanvasWidget.hのコメント参照)。ストローク中だけ
        // 定期フラッシュタイマーを動かす。最初の1点は待たせず即flushして、
        // 打ち始めの点がすぐ出るようにする。
        if (t->isActive()) {
            // 最初の1点を即flush + 同期描画(repaint)で置いた瞬間に出す。
            // 以降のストローク描画はmouseMoveEvent内の時間スロットリングrepaintが駆動し、
            // inputFlushTimer_は「入力が途切れた瞬間」に末尾を拾うフォールバック。
            t->flushPendingInput(toolCtx_);
            const qint64 nsAfterFlush = WinLog::enabled() ? pressClock.nsecsElapsed() : 0;
            inputFlushTimer_->start();
            strokePaintIntervalMs_ = kStrokePaintIntervalMs; // 適応間隔をリセット
            strokePaintClock_.start();
            strokeFrameLogCount_ = 0;
            viewDiagCount_ = 0;
            inSyncRepaint_ = true;
            repaint();
            inSyncRepaint_ = false;
            lastRepaintDoneClock_.restart();
            if (WinLog::enabled()) {
                WINLOG(QStringLiteral("PERF stroke: press onMousePress=%1ms flush=%2ms repaint=%3ms total=%4ms")
                           .arg(nsAfterTool / 1e6, 0, 'f', 2)
                           .arg((nsAfterFlush - nsAfterTool) / 1e6, 0, 'f', 2)
                           .arg((pressClock.nsecsElapsed() - nsAfterFlush) / 1e6, 0, 'f', 2)
                           .arg(pressClock.nsecsElapsed() / 1e6, 0, 'f', 2));
            }
        }
    }
    updateCursor();
}

void CanvasWidget::mouseMoveEvent(QMouseEvent *event) {
    if (!glReady_) return;
    // タブレット直接駆動中の実マウスイベントは無視(mousePressEvent参照)
    if (tabletDriving_ && !dispatchingSyntheticTabletMouse_) return;
    // controller管理アクション(色収差の中心ハンドルドラッグ等)は、activeToolに
    // 関係なく最優先で処理し続ける(mousePressEvent側でヒットした時にのみ
    // ドラッグ中フラグが立ち、以後 routeMouseMove が消費し続ける)。
    if (actions_.routeMouseMove(event, toolCtx_)) {
        updateCursor();
        update();
        return;
    }
    // 各ToolがonMouseMove内部で「自分がドラッグ中か」を自己判定するので、
    // ここでは無条件に委譲するだけでよい。
    {
        const bool colorPanelActive = actions_.toolInputBlocked();
        const bool otherPanelBlocking = actions_.toolInputFullyBlocked();
        if (otherPanelBlocking) return;
        if (colorPanelActive && activeTool != ToolType::Move && activeTool != ToolType::Rotate) return;
    }
    if (Tool *t = currentTool()) {
        // 【重要】onMouseMove()の中でGLを触るツールがあるので、先にコンテキストを
        // カレントにしておく(mousePressEvent/mouseReleaseEventは元から同じことをしている)。
        //
        // ペン/消しゴム/エアブラシのonMouseMove()はCPUで入力を貯めるだけで、実際の
        // ディスパッチは下のflushPendingInput()(makeCurrent()済み)で行うため問題に
        // ならなかったが、選択ツール(SelectTool)やぼかし/ゆがみはonMouseMove()から
        // 直接コンピュートシェーダーをディスパッチする。カレントでないコンテキストへの
        // GL呼び出しは黙って捨てられるため、ペン選択では「マウスを押した瞬間の1点
        // (mousePressEvent側なのでmakeCurrent済み)しかマスクに残らない」=
        // ドラッグし始めだけ選択される、あるいは何も残らず選択が変わらない、という
        // 症状になっていた。タブが複数あるとコンテキストが切り替わるため再現性も
        // まちまちだった。
        //
        // 既にカレントなら実質早期リターンで済むので、ストローク中の追従性への影響は
        // 無視できる(以前ここから除去したQCursor::setPos()のようなプロセス横断の
        // 重いシステムコールとは性質が違う)。
        makeCurrent();
        t->setPressure(mapPressure(resolvePointerPressure(event)));
        applyPointerTilt(t);
        t->onMouseMove(event, toolCtx_); // ペン系はここでは入力を貯めるだけ(CPUのみ)

        // 【重要】Windowsでは、ペンを連続で動かしている間は入力キューが空にならないため、
        // WM_TIMER(QTimer)もupdate()が積むペイントイベントも一切配信されない
        // (どちらもキューが空のとき初めて処理される低優先度メッセージ)。つまり
        // タイマー駆動でもupdate()でも、ドラッグ中は画面が全く更新されず、指を止めて
        // 初めて溜まった線がまとめて出る——「点だけ→全体が現れる」「レイヤーが多いと
        // いきなり全部出る」の正体はこれ。連続入力中に確実に描くには、この
        // 入力ハンドラの中から同期描画(repaint)を直接呼ぶしかない。
        //
        // ただし毎イベントrepaintするとpaintGLの完了待ちで入力が詰まるので、前回の
        // 実描画から一定時間経ったときだけrepaintする(自然にpaintGLの処理レートへ
        // 律速され、速いほど滑らか・遅くても入力を溜め込まない)。GPU flushもここで
        // まとめて行う(貯めたスタンプを1回のディスパッチに)。
        // 【重要・ペンタブ】溜まった入力を先に捌いてから描く。
        //
        // repaint() は同期で、present(vsync待ち)を含めて実測15〜20msかかる。
        // 一方この判定は「前回の描画開始から12ms経ったか」なので、repaint()から
        // 戻った時点で既に成立している ― つまり "1イベント処理するたびに1回描く"
        // 動きになる。処理できるのは毎秒 1000/17 ≒ 59イベントが上限。
        //
        // マウスはOSが移動イベントを間引くので、アプリに届くのは実測50〜90件/秒。
        // 上限内に収まるので問題にならなかった。ところがペンタブ(WM_POINTER由来の
        // QTabletEvent)は間引かれず、実測191件/秒届く。処理が追いつかず入力キューが
        // 際限なく伸び、画面はポインタから遅れる一方になる ― これが
        // 「ペン先にたどり着くまで非常に遅い」の正体。
        //
        // そこで「今処理しているイベントが発生してから何ms経っているか」を見る。
        // 溜まっている間は描画を見送り、返って残りのイベントを捌かせる(1件あたり
        // 数µsなのですぐ追いつく)。追いついた=最新のイベントになった時点で描くので、
        // 結果として「溜まったぶんを捨てずに捌いて、最新の位置を1回描く」になる。
        //
        // 追いついているときは inputAgeMs_ がほぼ0なので、従来どおり最速で描く
        // (マウス操作やペンの遅い動きでの追従性は変わらない)。
        // 時計が信用できない環境で描画が止まらないよう、一定時間描いていなければ
        // 無条件で描く保険も入れてある。
        constexpr qint64 kMaxInputAgeMs   = 6;   // これを超えていたら「溜まっている」
        constexpr qint64 kForcePaintAfter = 40;  // 保険: これだけ描いていなければ描く
        updateInputAge(event);
        const bool caughtUp = (inputAgeMs_ <= kMaxInputAgeMs);
        const bool starved  = lastRepaintDoneClock_.isValid()
                           && lastRepaintDoneClock_.elapsed() >= kForcePaintAfter;
        if (t->isActive() && t->needsCanvasRepaintWhileActive() && (caughtUp || starved)
            && (!strokePaintClock_.isValid() || strokePaintClock_.elapsed() >= strokePaintIntervalMs_)) {
            const qint64 sinceLastPaintMs = strokePaintClock_.isValid() ? strokePaintClock_.elapsed() : -1;
            makeCurrent();
            t->flushPendingInput(toolCtx_);
            // 【重要】間隔の起点は repaint() の「開始」に置く(終了後にrestartしない)。
            // repaint()の中にはpresent(vsync待ち)が含まれ、実測で1回あたり13〜26ms
            // かかる。終了時点を起点にすると 実質間隔 = present待ち + 12ms となり、
            // 60Hzの画面に対して35fps程度まで落ちてしまう(そのぶん線がペン先から
            // 遅れる)。開始時点を起点にすれば 実質間隔 = max(12ms, present待ち) に
            // なり、画面が出せる最大レートでそのまま追従できる。
            strokePaintClock_.restart();
            QElapsedTimer paintCost;
            paintCost.start();
            const quint64 callsBefore = paintGlCalls_;
            const qint64  glNsBefore  = paintGlNsAccum_;
            inSyncRepaint_ = true;
            repaint();
            inSyncRepaint_ = false;
            lastRepaintDoneClock_.restart();
            if (WinLog::enabled() && strokeFrameLogCount_ < 12) {
                strokeFrameLogCount_++;
                // repaint() の中身を「paintGL本体(CPU)」と「それ以外」に割る。
                //
                // 【読み方の注意】paintGL の値はCPUが命令を積むまでの時間でしかない。
                // GLの呼び出しは非同期なので、自分の描画のGPU実処理は「それ以外」の側に
                // 入ってくる ―― つまりここが大きくても、Qtのウィンドウ合成が遅いとは
                // 限らない(実測でその取り違えをした)。内訳を確かめるには
                // TIEPOLO_GLFINISH=1 を付けて PERF gpu の行を見ること。
                const double totalMs = paintCost.nsecsElapsed() / 1e6;
                const double glMs    = (paintGlNsAccum_ - glNsBefore) / 1e6;
                WINLOG(QStringLiteral("PERF stroke: frame#%1 gapSincePrev=%2ms repaint=%3ms "
                                      "paintGL(CPU)=%4ms x%5 GPU待ち+合成=%6ms prevInterval=%7ms")
                           .arg(strokeFrameLogCount_).arg(sinceLastPaintMs)
                           .arg(totalMs, 0, 'f', 2)
                           .arg(glMs, 0, 'f', 2)
                           .arg(paintGlCalls_ - callsBefore)
                           .arg(totalMs - glMs, 0, 'f', 2)
                           .arg(strokePaintIntervalMs_));
            }
            // 実測した描画コストに応じて次回の間隔を適応させる(ヘッダの
            // strokePaintIntervalMs_コメント参照)。部分再描画により通常は数ms以下に
            // 収まり最短間隔のままになるが、万一重い環境・状況でも入力処理が
            // 半分以上の時間を確保できるため、入力キューが溜まって線が大きく
            // 遅れて追いかけてくる状態にはならない。
            //
            // 材料は repaint() 全体ではなく paintGL() 本体の時間
            // (lastPaintGlCostNs_)。repaint()にはpresent(vsync待ち・GPUキューの
            // 消化待ち)が含まれ、それを「重い」と解釈して間隔を倍にすると、
            // 表示可能な速度より遅く描くことになり、書き始めのカクつきそのものを
            // 生んでいた(ヘッダのlastPaintGlCostNs_のコメント参照)。
            strokePaintIntervalMs_ =
                qBound(kStrokePaintIntervalMs, (int)(lastPaintGlCostNs_ / 1000000) * 2, 100);
        }
    }
    updateCursor();
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (!glReady_) return;
    // タブレット直接駆動中の実マウスイベントは無視(mousePressEvent参照)。
    // 特に実マウスのReleaseを通すとストロークが途中で確定されてしまう。
    if (tabletDriving_ && !dispatchingSyntheticTabletMouse_) return;
    if (event->button() != Qt::LeftButton) return;
    if (actions_.routeMouseRelease(event, toolCtx_)) {
        updateCursor();
        return;
    }
    {
        const bool colorPanelActive = actions_.toolInputBlocked();
        const bool otherPanelBlocking = actions_.toolInputFullyBlocked();
        if (otherPanelBlocking) return;
        if (colorPanelActive && activeTool != ToolType::Move && activeTool != ToolType::Rotate) return;
    }

    makeCurrent();
    // ビュー変換のドラッグだったか(onMouseRelease でisActive()が落ちるので先に見る)
    bool wasViewTransform = false;
    if (Tool *t = currentTool()) {
        wasViewTransform = t->isActive() && t->transformsViewWhileActive();
        t->onMouseRelease(event, toolCtx_);
    }
    // ドラッグ中は縮小表示のサンプル数を1に落としているので、離した時点で必ず
    // 1枚描き直して本来の品質に戻す(paintGL の minifySamples のコメント参照)。
    // 移動/回転ツールの onMouseRelease は再描画を要求しないため、ここで出さないと
    // 荒いままの絵が残る。
    if (wasViewTransform) update();
    // ドラッグ中に止めていた viewChanged() をここで1回だけ出す
    // (NavigatorDockの表示範囲枠が最終位置へ揃う)。
    emitViewChangedIfPending();
    // フレームレート律速バッチ用の定期フラッシュタイマーはストローク中だけ動かす。
    inputFlushTimer_->stop();
    updateCursor();
}

// 今処理している入力イベントが「発生してから何ms経っているか」を更新する。
//
// event->timestamp() はOSがイベントを作った時刻(ms)。これとこちらの経過時間を
// 比べると、入力キューにどれだけ溜まっているかが分かる。時計の基準が違うので、
// ドラッグ開始時点を0として「そこからどれだけ余分に遅れたか」を見る。
// タイムスタンプが取れない経路では0(=遅れ無し)として扱い、判定を素通しさせる。
void CanvasWidget::updateInputAge(const QMouseEvent *event)
{
    if (inputBaseTimestamp_ == 0 || event->timestamp() == 0 || !inputBaseClock_.isValid()) {
        inputAgeMs_ = 0;
        return;
    }
    const qint64 osElapsed = (qint64)event->timestamp() - (qint64)inputBaseTimestamp_;
    inputAgeMs_ = qMax<qint64>(0, inputBaseClock_.elapsed() - osElapsed);
}

// ドラッグ中に止めていた viewChanged() が残っていれば、ここで1回だけ出す
// (止める理由は setupToolContext() の requestRepaint のコメント参照)。
void CanvasWidget::emitViewChangedIfPending()
{
    if (!viewChangedPending_) return;
    viewChangedPending_ = false;
    emit viewChanged();
}

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    if (!glReady_) return;
    // タブレット直接駆動中の実マウスイベントは無視(mousePressEvent参照)
    if (tabletDriving_ && !dispatchingSyntheticTabletMouse_) return;
    if (event->button() != Qt::LeftButton) return;
    if (actions_.routeMouseDoubleClick(event, toolCtx_)) {
        updateCursor();
        return;
    }
    {
        const bool colorPanelActive = actions_.toolInputBlocked();
        const bool otherPanelBlocking = actions_.toolInputFullyBlocked();
        if (otherPanelBlocking) return;
        if (colorPanelActive && activeTool != ToolType::Move && activeTool != ToolType::Rotate) return;
    }
    if (isTransformActionActive() || isFreeTransformActionActive() || isCanvasSizeActionActive()) return;

    makeCurrent();
    if (Tool *t = currentTool()) t->onMouseDoubleClick(event, toolCtx_);
    updateCursor();
}

void CanvasWidget::wheelEvent(QWheelEvent *event) {
    if (!glReady_) return;

    // Shift+スクロール: ズームではなく横方向のパンにする(横スクロールホイールが
    // 無いマウスでも、キャンバスを横に広く見たいときに片手で操作できるようにするため)。
    if (event->modifiers() & Qt::ShiftModifier) {
        // ビューはデバイスピクセルなので、スクロール量(論理px相当)を換算して渡す
        const float dx = event->angleDelta().y() / 120.0f * 80.0f * viewDpr();
        view_.pan(QVector2D(dx, 0.0f));
        update();
        emit viewChanged();
        return;
    }

    float factor  = std::pow(1.15f, event->angleDelta().y() / 120.0f);
    const float d = viewDpr(); // マウス位置(論理px)をビュー空間(デバイスpx)へ
    QVector2D pos(event->position().x() * d, event->position().y() * d);
    view_.zoomAround(pos, factor);
    updateCursor(); // ズームでペン円カーソルの画面上サイズが変わるため
    update();
    emit viewChanged();
}

void CanvasWidget::tabletEvent(QTabletEvent *event) {
    brushPressure = event->pressure();
    lastTabletPressure_ = brushPressure;

    // 傾きとペン軸まわりの回転。
    //
    // xTilt/yTilt は度数(おおむね±60°が最大)で、ペンをどちらへどれだけ寝かせたかを
    // 2軸で表す。ツール側が使いやすいよう「寝かせ具合(0〜1)」と「寝かせた向き」へ
    // 分解して渡す。yTilt は画面下向きが正なので、キャンバス座標(Y上向き)に
    // 合わせるため符号を反転してから角度を求める(先端の回転角と同じ座標系にする)。
    //
    // rotation はアートペンなど一部のペンだけが返す軸回転。非対応機では常に0。
    {
        constexpr float kMaxTiltDeg = 60.0f;
        const float tx = event->xTilt();
        const float ty = -event->yTilt();
        const float mag = std::hypot(tx, ty);
        lastTabletTiltAmount_ = qBound(0.0f, mag / kMaxTiltDeg, 1.0f);
        // 垂直に近いときの角度は数値的に暴れるので、その場合は前回の向きを保つ
        if (mag > 1.0f) lastTabletTiltAngle_ = std::atan2(ty, tx);
        lastTabletRotation_ = qDegreesToRadians(event->rotation());

        WINLOG_V(QStringLiteral("TABLET tilt=(%1,%2)deg amount=%3 angle=%4deg rot=%5deg press=%6")
                     .arg(event->xTilt()).arg(event->yTilt())
                     .arg(lastTabletTiltAmount_, 0, 'f', 3)
                     .arg(qRadiansToDegrees(lastTabletTiltAngle_), 0, 'f', 1)
                     .arg(event->rotation(), 0, 'f', 1)
                     .arg(brushPressure, 0, 'f', 3));
    }

    lastTabletEventClock_.start();
    if (Tool *t = currentTool()) {
        t->setPressure(mapPressure(brushPressure));
        applyPointerTilt(t);
    }

    // ペン先(左ボタン相当)のダウン/ムーブ/アップは、OS/Qtのマウスイベント合成を
    // 待たずここから直接駆動する。合成経路に任せると、Windowsの
    // 「プレス&ホールドで右クリック」ジェスチャ判定のため、ペンを置いてから
    // 合成マウスダウンが届くまで数百ms(またはペンが一定距離動くまで)保留される
    // ことがあり、「ペンを置いてから線が出るまでがかなり長い」
    // 「短い線(文字)ほど毎回最大まで待たされて書けない」の原因になっていた。
    // tabletEvent自体はWM_POINTER由来でジェスチャ判定に保留されず即座に届く。
    //
    // accept()してQt側の合成を止めた上で、OS互換レイヤー由来の「本物の」マウス
    // ストリームが重複して届く環境に備え、駆動中(tabletDriving_)は
    // mousePressEvent等の実マウスイベントを無視する(合成の見分け方は
    // CanvasWidget.hのdispatchingSyntheticTabletMouse_コメント参照)。
    const auto driveMouse = [this, event](QEvent::Type type) {
        QMouseEvent me(type, event->position(), event->globalPosition(),
                       Qt::LeftButton,
                       (type == QEvent::MouseButtonRelease) ? Qt::NoButton : Qt::LeftButton,
                       event->modifiers());
        // 元のタブレットイベントの発生時刻を引き継ぐ。合成したQMouseEventは既定で
        // タイムスタンプが0になり、「このイベントはいつ発生したものか」が失われる。
        // 入力が溜まっているかどうかの判定(mouseMoveEventの入力滞留チェック)は
        // これを基準にしているので、引き継がないとペンタブ経路だけ判定が効かない。
        me.setTimestamp(event->timestamp());
        dispatchingSyntheticTabletMouse_ = true;
        switch (type) {
        case QEvent::MouseButtonPress:    mousePressEvent(&me);       break;
        case QEvent::MouseMove:           mouseMoveEvent(&me);        break;
        case QEvent::MouseButtonRelease:  mouseReleaseEvent(&me);     break;
        case QEvent::MouseButtonDblClick: mouseDoubleClickEvent(&me); break;
        default: break;
        }
        dispatchingSyntheticTabletMouse_ = false;
    };

    switch (event->type()) {
    case QEvent::TabletPress:
        if (event->button() == Qt::LeftButton) {
            tabletDriving_ = true;
            driveMouse(QEvent::MouseButtonPress);
            // マウス合成を止めるとダブルクリックも合成されなくなるため、
            // ダブルタップ(時間・距離とも近い2度目のダウン)を自前で検出して
            // ダブルクリック動作(テキストボックスの再編集等)も発火させる。
            if (tabletLastPressClock_.isValid() && tabletLastPressClock_.elapsed() <= 400
                && (event->position() - tabletLastPressPos_).manhattanLength() <= 8.0)
                driveMouse(QEvent::MouseButtonDblClick);
            tabletLastPressClock_.start();
            tabletLastPressPos_ = event->position();
            event->accept();
            return;
        }
        break;
    case QEvent::TabletMove:
        if (tabletDriving_) {
            driveMouse(QEvent::MouseMove);
            event->accept();
            return;
        }
        break;
    case QEvent::TabletRelease:
        if (tabletDriving_) {
            driveMouse(QEvent::MouseButtonRelease);
            tabletDriving_ = false;
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    // ペン先以外(サイドボタン=右クリック等)やホバー移動は従来通り
    // Qtのマウスイベント合成に任せる。
    event->ignore();
}

#ifdef Q_OS_WIN
bool CanvasWidget::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    // プレス&ホールド(長押し右クリック)等のペンジェスチャをこのウィンドウでは
    // 無効化する(tabletEvent()の直接駆動コメント参照。レガシー経路がまだ使われる
    // 環境向けの保険)。
    MSG *msg = static_cast<MSG *>(message);
    if (msg->message == 0x02CC /*WM_TABLET_QUERYSYSTEMGESTURESTATUS*/) {
        *result = 0x00000001 /*TABLET_DISABLE_PRESSANDHOLD*/
                | 0x00000008 /*TABLET_DISABLE_PENTAPFEEDBACK*/
                | 0x00000010 /*TABLET_DISABLE_PENBARRELFEEDBACK*/
                | 0x00010000 /*TABLET_DISABLE_FLICKS*/;
        return true;
    }
    return QOpenGLWidget::nativeEvent(eventType, message, result);
}
#endif

void CanvasWidget::showEvent(QShowEvent *event) {
    QOpenGLWidget::showEvent(event);
#ifdef Q_OS_WIN
    // Windows標準のペン/タッチ視覚フィードバック(タップの波紋・長押しを示す丸
    // サークル等)は、ペンタブでキャンバスへ描画する際に絵と重なって邪魔になる
    // ため、このウィジェットのネイティブウィンドウに限定して無効化する
    // (システム設定やほかのウィンドウには影響しない)。ネイティブウィンドウが
    // 実在する初回表示時に一度だけ行えばよい。
    if (!penTouchFeedbackDisabled_) {
        penTouchFeedbackDisabled_ = true;
        const FEEDBACK_TYPE kTypes[] = {
            FEEDBACK_TOUCH_CONTACTVISUALIZATION,
            FEEDBACK_PEN_BARRELVISUALIZATION,
            FEEDBACK_PEN_TAP,
            FEEDBACK_PEN_DOUBLETAP,
            FEEDBACK_PEN_PRESSANDHOLD,
            FEEDBACK_PEN_RIGHTTAP,
            FEEDBACK_TOUCH_TAP,
            FEEDBACK_TOUCH_DOUBLETAP,
            FEEDBACK_TOUCH_PRESSANDHOLD,
            FEEDBACK_TOUCH_RIGHTTAP,
            FEEDBACK_GESTURE_PRESSANDTAP,
        };
        BOOL disabled = FALSE;
        for (FEEDBACK_TYPE t : kTypes)
            SetWindowFeedbackSetting(reinterpret_cast<HWND>(winId()), t, 0, sizeof(disabled), &disabled);

        // 視覚フィードバックだけでなく、プレス&ホールド(長押し右クリック)などの
        // ペンジェスチャ判定自体もこのウィンドウでは無効化する。有効なままだと、
        // ペンを置いてもジェスチャ判定が終わるまで合成マウスダウンの発生が
        // 数百ms保留され、書き始めの遅延になる(tabletEvent()の直接駆動と
        // nativeEvent()のWM_TABLET_QUERYSYSTEMGESTURESTATUS応答と合わせて三重の保険)。
        const DWORD tabletGestureOff =
            0x00000001 /*TABLET_DISABLE_PRESSANDHOLD*/ |
            0x00000008 /*TABLET_DISABLE_PENTAPFEEDBACK*/ |
            0x00000010 /*TABLET_DISABLE_PENBARRELFEEDBACK*/ |
            0x00010000 /*TABLET_DISABLE_FLICKS*/;
        SetPropW(reinterpret_cast<HWND>(winId()), L"MicrosoftTabletPenServiceProperty",
                 reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(tabletGestureOff)));
    }
#endif
}

bool CanvasWidget::isInkingBusy()
{
    if (Tool *t = currentTool(); t && t->isActive()) return true;
    // ペンがホバー中(ストロークとストロークの間)も忙しい扱いにする。文字書きの
    // ように短いストロークを連打しているとき、重いUI再生成が次のペンダウンの
    // 直前・最中に走ってイベントループを塞ぐのを防ぐ。
    return lastTabletEventClock_.isValid() && lastTabletEventClock_.elapsed() < 250;
}

Tool *CanvasWidget::currentTool() {
    // テキストレイヤーがアクティブな間は、テキストツール以外での描画/操作を
    // 一切受け付けない(ペンでの書き込みなど、非破壊のテキストデータと矛盾する
    // 操作を防ぐため)。
    if (doc_ && doc_->layerCount() > 0 &&
        doc_->layers[doc_->activeLayerIndex()].layerType == LayerType::Text &&
        activeTool != ToolType::Text)
        return nullptr;

    switch (activeTool) {
    case ToolType::Pen:    return &penTool_;
    case ToolType::Eraser:  return &eraserTool_;
    case ToolType::Fill:    return &fillTool_;
    case ToolType::Move:    return &moveTool_;
    case ToolType::Rotate:  return &rotateTool_;
    case ToolType::Dropper: return &dropperTool_;
    case ToolType::Blur:    return &blurTool_;
    case ToolType::Warp:    return &warpTool_;
    case ToolType::Selection: return &selectTool_;
    case ToolType::Text:    return &textTool_;
    case ToolType::Airbrush: return &airbrushTool_;
    default: return nullptr;
    }
}

// ===========================================================================
// レイヤー操作
// ===========================================================================
