#include "actions/CanvasActionController.h"
#include "actions/CanvasActionHost.h"

// blocksToolInput()==true かつ blocksAllToolInput()==false(=移動/回転ツールへの切替だけ素通しする色調整/フィルター系)のときだけ、host へ開始/終了を通知する。
static bool needsToolBlockNotification(const CanvasAction *a)
{
    return a && a->blocksToolInput() && !a->blocksAllToolInput();
}

void CanvasActionController::start(CanvasAction *action)
{
    if (!action) return;
    // 相互排他: 別のアクションが動いていたらキャンセルしてから開始する。
    cancelActive();
    if (action->begin()) {
        active_ = action;
        if (needsToolBlockNotification(action) && host_) host_->hostNotifyToolBlockingActionStarted();
    }
}

void CanvasActionController::confirmActive()
{
    if (!active_) return;
    CanvasAction *a = active_;
    active_ = nullptr; // 先に外しておく(confirm 内から再入しても安全に)
    const bool notify = needsToolBlockNotification(a);
    a->confirm();
    if (notify && host_) host_->hostNotifyToolBlockingActionEnded();
}

void CanvasActionController::cancelActive()
{
    if (!active_) return;
    CanvasAction *a = active_;
    active_ = nullptr;
    const bool notify = needsToolBlockNotification(a);
    a->cancel();
    if (notify && host_) host_->hostNotifyToolBlockingActionEnded();
}

void CanvasActionController::initializeAll(QOpenGLContext *ctx)
{
    for (auto &a : actions_)
        a->initialize(ctx);
}

void CanvasActionController::releaseAllGL()
{
    for (auto &a : actions_)
        a->releaseGL();
}

void CanvasActionController::applyRenderState(QOpenGLShaderProgram *renderProg)
{
    // render.frag の uIsXxxTool 系 uniform はプログラムに保持され続けるため、毎フレーム全アクションに設定させる(非アクティブなら自分の uniform を 0 に、アクティブなら 1 + パラメータに)。
    for (auto &a : actions_)
        a->applyRenderState(renderProg);
}

bool CanvasActionController::paintActiveOverlay(QPainter &painter, const ToolContext &ctx)
{
    return active_ && active_->paintOverlay(painter, ctx);
}

void CanvasActionController::positionActivePanel()
{
    if (active_) active_->positionPanel();
}

bool CanvasActionController::routeMousePress(QMouseEvent *e, ToolContext &ctx)
{
    if (!active_) return false;
    const bool consumed = active_->handleMousePress(e, ctx);
    // 変形/自由変形などは確定・キャンセルボタンのクリックをhandleMousePress内で自己判定して isActive() を false にすることがある。
    if (active_ && !active_->isActive()) active_ = nullptr;
    return consumed;
}

bool CanvasActionController::routeMouseMove(QMouseEvent *e, ToolContext &ctx)
{
    return active_ && active_->handleMouseMove(e, ctx);
}

bool CanvasActionController::routeMouseRelease(QMouseEvent *e, ToolContext &ctx)
{
    return active_ && active_->handleMouseRelease(e, ctx);
}

bool CanvasActionController::routeMouseDoubleClick(QMouseEvent *e, ToolContext &ctx)
{
    return active_ && active_->handleMouseDoubleClick(e, ctx);
}

bool CanvasActionController::nudgeActive(const QVector2D &delta)
{
    return active_ && active_->nudge(delta);
}
