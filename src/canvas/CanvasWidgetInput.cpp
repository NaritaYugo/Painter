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
    // 分割表示での操作権の移動(クリックしたペインをアクティブにする)は、キャンバスに限らずスタートページやタブバーのクリックでも効く必要があるため、
    // ここではなくMainWindow::eventFilter()側でqApp全体のマウス押下を見て一括で処理している。
    if (!glReady_) return; // initializeGL()完了前にマウス操作が届いた場合は無視する
    // ペン先ストロークをtabletEvent()から直接駆動している間、OS互換レイヤー由来の実マウスイベントは重複ストリームなので無視する(CanvasWidget.hのtabletDriving_コメント参照)。
    if (tabletDriving_ && !dispatchingSyntheticTabletMouse_) {
        if (lastTabletEventClock_.isValid() && lastTabletEventClock_.elapsed() > 500)
            tabletDriving_ = false; // 保険(以後は通常のマウス操作として続行)
        else
            return;
    }
    if (event->button() != Qt::LeftButton) return;
    // controller管理アクション(色収差の円形ハンドル等)は、パネル表示中でも(activeToolに関係なく)ヒットした場合だけ最優先で処理する。
    if (actions_.routeMousePress(event, toolCtx_)) {
        updateCursor();
        return;
    }
    // パネル自体はQWidgetなので自分でイベントを受け取る。
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
        // フレームレート律速バッチ(CanvasWidget.hのコメント参照)。
        if (t->isActive()) {
            // 最初の1点を即flush + 同期描画(repaint)で置いた瞬間に出す。
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
    // タブレット直接駆動中の実マウスイベントは無視(mousePressEvent参照)。
    if (tabletDriving_ && !dispatchingSyntheticTabletMouse_) return;
    // controller管理アクション(色収差の中心ハンドルドラッグ等)は、activeToolに関係なく最優先で処理し続ける(mousePressEvent側でヒットした時にのみドラッグ中フラグが立ち、
    // 以後 routeMouseMove が消費し続ける)。
    if (actions_.routeMouseMove(event, toolCtx_)) {
        updateCursor();
        update();
        return;
    }
    // 各ToolがonMouseMove内部で「自分がドラッグ中か」を自己判定するので、ここでは無条件に委譲するだけでよい。
    {
        const bool colorPanelActive = actions_.toolInputBlocked();
        const bool otherPanelBlocking = actions_.toolInputFullyBlocked();
        if (otherPanelBlocking) return;
        if (colorPanelActive && activeTool != ToolType::Move && activeTool != ToolType::Rotate) return;
    }
    if (Tool *t = currentTool()) {
        // GLを使うツールに備えてコンテキストをカレントにする。
        makeCurrent();
        t->setPressure(mapPressure(resolvePointerPressure(event)));
        applyPointerTilt(t);
        t->onMouseMove(event, toolCtx_); // ペン系はここでは入力を貯めるだけ(CPUのみ)

        // Windowsでは連続したペン入力中にタイマーとpaintイベントが遅延する。
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
            // repaint開始時刻を次フレームの基準にする。
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
            // 実測した描画コストに応じて次回の間隔を調整する。
            strokePaintIntervalMs_ =
                qBound(kStrokePaintIntervalMs, (int)(lastPaintGlCostNs_ / 1000000) * 2, 100);
        }
    }
    updateCursor();
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (!glReady_) return;
    // タブレット直接駆動中の実マウスイベントは無視(mousePressEvent参照)。
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
    // ビュー変換のドラッグだったか(onMouseRelease でisActive()が落ちるので先に見る)。
    bool wasViewTransform = false;
    if (Tool *t = currentTool()) {
        wasViewTransform = t->isActive() && t->transformsViewWhileActive();
        t->onMouseRelease(event, toolCtx_);
    }
    // ドラッグ中は縮小表示のサンプル数を1に落としているので、離した時点で必ず1枚描き直して本来の品質に戻す(paintGL の minifySamples のコメント参照)。
    if (wasViewTransform) update();
    // ドラッグ中に止めていた viewChanged() をここで1回だけ出す(NavigatorDockの表示範囲枠が最終位置へ揃う)。
    emitViewChangedIfPending();
    // フレームレート律速バッチ用の定期フラッシュタイマーはストローク中だけ動かす。
    inputFlushTimer_->stop();
    updateCursor();
}

// 今処理している入力イベントが「発生してから何ms経っているか」を更新する。
void CanvasWidget::updateInputAge(const QMouseEvent *event)
{
    if (inputBaseTimestamp_ == 0 || event->timestamp() == 0 || !inputBaseClock_.isValid()) {
        inputAgeMs_ = 0;
        return;
    }
    const qint64 osElapsed = (qint64)event->timestamp() - (qint64)inputBaseTimestamp_;
    inputAgeMs_ = qMax<qint64>(0, inputBaseClock_.elapsed() - osElapsed);
}

// ドラッグ中に止めていた viewChanged() が残っていれば、ここで1回だけ出す(止める理由は setupToolContext() の requestRepaint のコメント参照)。
void CanvasWidget::emitViewChangedIfPending()
{
    if (!viewChangedPending_) return;
    viewChangedPending_ = false;
    emit viewChanged();
}

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    if (!glReady_) return;
    // タブレット直接駆動中の実マウスイベントは無視(mousePressEvent参照)。
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

    // Shift+スクロール: ズームではなく横方向のパンにする(横スクロールホイールが無いマウスでも、キャンバスを横に広く見たいときに片手で操作できるようにするため)。
    if (event->modifiers() & Qt::ShiftModifier) {
        // ビューはデバイスピクセルなので、スクロール量(論理px相当)を換算して渡す。
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
    {
        constexpr float kMaxTiltDeg = 60.0f;
        const float tx = event->xTilt();
        const float ty = -event->yTilt();
        const float mag = std::hypot(tx, ty);
        lastTabletTiltAmount_ = qBound(0.0f, mag / kMaxTiltDeg, 1.0f);
        // 垂直に近いときの角度は数値的に暴れるので、その場合は前回の向きを保つ。
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

    // ペン先(左ボタン相当)のダウン/ムーブ/アップは、OS/Qtのマウスイベント合成を待たずここから直接駆動する。
    const auto driveMouse = [this, event](QEvent::Type type) {
        QMouseEvent me(type, event->position(), event->globalPosition(),
                       Qt::LeftButton,
                       (type == QEvent::MouseButtonRelease) ? Qt::NoButton : Qt::LeftButton,
                       event->modifiers());
        // 元のタブレットイベントの発生時刻を引き継ぐ。
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
            // マウス合成を止めるとダブルクリックも合成されなくなるため、ダブルタップ(時間・距離とも近い2度目のダウン)を自前で検出してダブルクリック動作(テキストボックスの再編集等)も発火させる。
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
    event->ignore();
}

#ifdef Q_OS_WIN
bool CanvasWidget::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    // プレス&ホールド(長押し右クリック)等のペンジェスチャをこのウィンドウでは無効化する(tabletEvent()の直接駆動コメント参照。レガシー経路がまだ使われる環境向けの保険)。
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
    // Windows標準のペン/タッチ視覚フィードバック(タップの波紋・長押しを示す丸サークル等)は、ペンタブでキャンバスへ描画する際に絵と重なって邪魔になるため、
    // このウィジェットのネイティブウィンドウに限定して無効化する(システム設定やほかのウィンドウには影響しない)。
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

        // 視覚フィードバックだけでなく、プレス&ホールド(長押し右クリック)などのペンジェスチャ判定自体もこのウィンドウでは無効化する。
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
    // ペンがホバー中(ストロークとストロークの間)も忙しい扱いにする。
    return lastTabletEventClock_.isValid() && lastTabletEventClock_.elapsed() < 250;
}

Tool *CanvasWidget::currentTool() {
    // テキストレイヤーがアクティブな間は、テキストツール以外での描画/操作を一切受け付けない(ペンでの書き込みなど、非破壊のテキストデータと矛盾する操作を防ぐため)。
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

// レイヤー操作。
