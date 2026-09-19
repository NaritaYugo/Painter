#include "actions/MotionBlurAction.h"
#include "actions/CanvasActionHost.h"
#include "actions/CanvasActionController.h"
#include "tools/core/ToolContext.h"
#include "backend/CanvasDocument.h"

#include "dialogs/MotionBlurPanel.h"

#include <QOpenGLShaderProgram>
#include <QWidget>

bool MotionBlurAction::canActivate() const
{
    // フィルターは実ピクセルを持つレイヤー(Normal/Text)のみ対象。
    CanvasDocument *doc = host_.hostDocument();
    if (!doc || doc->layers.isEmpty()) return false;
    const LayerType lt = doc->activeLayer().layerType;
    return lt == LayerType::Normal || lt == LayerType::Text;
}

bool MotionBlurAction::onActivate()
{
    ToolContext &ctx = host_.hostToolContext();
    tool_.activate(ctx);
    if (!tool_.engaged()) return false; // レイヤーが0x0など(通常起こらない)
    if (!panel_) {
        QWidget *host = host_.hostWidget();
        panel_ = new MotionBlurPanel(host);
        QObject::connect(panel_, &MotionBlurPanel::modeChanged, host, [this](int m) {
            tool_.setMode(host_.hostToolContext(), (MotionBlurTool::Mode)m);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &MotionBlurPanel::angleChanged, host, [this](float a) {
            tool_.setAngleDeg(host_.hostToolContext(), a);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &MotionBlurPanel::distanceChanged, host, [this](float d) {
            tool_.setDistancePx(host_.hostToolContext(), d);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &MotionBlurPanel::angleSpanChanged, host, [this](float s) {
            tool_.setAngleSpanDeg(host_.hostToolContext(), s);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &MotionBlurPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &MotionBlurPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->setMode((int)tool_.mode());
    panel_->setAngleDeg(tool_.angleDeg());
    panel_->setDistancePx(tool_.distancePx());
    panel_->setAngleSpanDeg(tool_.angleSpanDeg());
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void MotionBlurAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
    if (panel_) panel_->hide();
}

void MotionBlurAction::onCancel()
{
    tool_.deactivate();
    if (panel_) panel_->hide();
}

void MotionBlurAction::positionPanel() { centerPanelTop(panel_); }

void MotionBlurAction::applyRenderState(QOpenGLShaderProgram *p)
{
    const bool on = isActive() && tool_.engaged();
    p->setUniformValue("uIsMotionBlurTool", on ? 1 : 0);
    if (on) {
        p->setUniformValue("uMotionBlurOriginPx", tool_.previewOriginPx());
        p->setUniformValue("uMotionBlurSizePx",   tool_.previewSizePx());
    }
}

bool MotionBlurAction::paintOverlay(QPainter &painter, const ToolContext &ctx)
{
    // 円形モードでは中心ハンドルを描く。アクティブな間はオーバーレイのスロットを
    // 消費する(平行モードで何も描かない場合でも通常ツールのオーバーレイは描かせない)。
    tool_.paintOverlay(painter, ctx);
    return true;
}

bool MotionBlurAction::handleMousePress(QMouseEvent *e, ToolContext &ctx)
{
    // 円形モードの中心ハンドルにヒットした場合だけ true(=消費)。
    // 外れた場合は false を返し、GLWidget 側の通常ガード(Move/Rotateのみ通す)へ委ねる。
    host_.hostMakeCurrent();
    return tool_.onMousePress(e, ctx);
}

bool MotionBlurAction::handleMouseMove(QMouseEvent *e, ToolContext &ctx)
{
    if (!tool_.isDraggingCenter()) return false;
    host_.hostMakeCurrent();
    tool_.onMouseMove(e, ctx);
    host_.hostUpdate();
    return true;
}

bool MotionBlurAction::handleMouseRelease(QMouseEvent *e, ToolContext &ctx)
{
    if (!tool_.isDraggingCenter()) return false;
    host_.hostMakeCurrent();
    tool_.onMouseRelease(e, ctx);
    return true;
}
