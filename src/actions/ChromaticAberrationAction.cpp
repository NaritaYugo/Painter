#include "actions/ChromaticAberrationAction.h"
#include "actions/CanvasActionHost.h"
#include "actions/CanvasActionController.h"
#include "tools/core/ToolContext.h"
#include "document/CanvasDocument.h"

#include "dialogs/ChromaticAberrationPanel.h"

#include <QOpenGLShaderProgram>
#include <QWidget>

bool ChromaticAberrationAction::canActivate() const
{
    CanvasDocument *doc = host_.hostDocument();
    if (!doc || doc->layers.isEmpty()) return false;
    const LayerType lt = doc->activeLayer().layerType;
    return lt == LayerType::Normal || lt == LayerType::Text;
}

bool ChromaticAberrationAction::onActivate()
{
    ToolContext &ctx = host_.hostToolContext();
    tool_.activate(ctx);
    if (!tool_.engaged()) return false; // レイヤーが0x0など(通常起こらない)
    if (!panel_) {
        QWidget *host = host_.hostWidget();
        panel_ = new ChromaticAberrationPanel(host);
        QObject::connect(panel_, &ChromaticAberrationPanel::modeChanged, host, [this](int m) {
            tool_.setMode(host_.hostToolContext(), (ChromaticAberrationTool::Mode)m);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &ChromaticAberrationPanel::angleChanged, host, [this](float a) {
            tool_.setAngleDeg(host_.hostToolContext(), a);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &ChromaticAberrationPanel::distanceChanged, host, [this](float d) {
            tool_.setDistancePx(host_.hostToolContext(), d);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &ChromaticAberrationPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &ChromaticAberrationPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->setMode((int)tool_.mode());
    panel_->setAngleDeg(tool_.angleDeg());
    panel_->setDistancePx(tool_.distancePx());
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void ChromaticAberrationAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
    if (panel_) panel_->hide();
}

void ChromaticAberrationAction::onCancel()
{
    tool_.deactivate();
    if (panel_) panel_->hide();
}

void ChromaticAberrationAction::positionPanel() { centerPanelTop(panel_); }

void ChromaticAberrationAction::applyRenderState(QOpenGLShaderProgram *p)
{
    const bool on = isActive() && tool_.engaged();
    p->setUniformValue("uIsChromaticAberrationTool", on ? 1 : 0);
    if (on) {
        p->setUniformValue("uChromaticAberrationOriginPx", tool_.previewOriginPx());
        p->setUniformValue("uChromaticAberrationSizePx",   tool_.previewSizePx());
    }
}

bool ChromaticAberrationAction::paintOverlay(QPainter &painter, const ToolContext &ctx)
{
    // 円形モードでは中心ハンドルを描く。アクティブな間はオーバーレイのスロットを
    // 消費する(平行モードで何も描かない場合でも通常ツールのオーバーレイは描かせない)。
    tool_.paintOverlay(painter, ctx);
    return true;
}

bool ChromaticAberrationAction::handleMousePress(QMouseEvent *e, ToolContext &ctx)
{
    // 円形モードの中心ハンドルにヒットした場合だけ true(=消費)。
    // 外れた場合は false を返し、CanvasWidget 側の通常ガード(Move/Rotateのみ通す)へ委ねる。
    host_.hostMakeCurrent();
    return tool_.onMousePress(e, ctx);
}

bool ChromaticAberrationAction::handleMouseMove(QMouseEvent *e, ToolContext &ctx)
{
    if (!tool_.isDraggingCenter()) return false;
    host_.hostMakeCurrent();
    tool_.onMouseMove(e, ctx);
    host_.hostUpdate();
    return true;
}

bool ChromaticAberrationAction::handleMouseRelease(QMouseEvent *e, ToolContext &ctx)
{
    if (!tool_.isDraggingCenter()) return false;
    host_.hostMakeCurrent();
    tool_.onMouseRelease(e, ctx);
    return true;
}
