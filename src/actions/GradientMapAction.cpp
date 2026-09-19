#include "actions/GradientMapAction.h"
#include "actions/CanvasActionHost.h"
#include "actions/CanvasActionController.h"
#include "tools/core/ToolContext.h"
#include "backend/CanvasDocument.h"

#include "dialogs/GradientMapPanel.h"

#include <QOpenGLShaderProgram>
#include <QWidget>

bool GradientMapAction::canActivate() const
{
    // 色調補正は実ピクセルを持つレイヤー(Normal/Text)のみ対象。
    CanvasDocument *doc = host_.hostDocument();
    if (!doc || doc->layers.isEmpty()) return false;
    const LayerType lt = doc->activeLayer().layerType;
    return lt == LayerType::Normal || lt == LayerType::Text;
}

bool GradientMapAction::onActivate()
{
    ToolContext &ctx = host_.hostToolContext();
    tool_.activate(ctx);
    if (!tool_.engaged()) return false;
    if (!panel_) {
        QWidget *host = host_.hostWidget();
        panel_ = new GradientMapPanel(host);
        QObject::connect(panel_, &GradientMapPanel::stopsChanged, host,
                         [this](const QVector<GradientStripEditor::Stop> &stops) {
            tool_.setStops(host_.hostToolContext(), stops);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &GradientMapPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &GradientMapPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    // ツール側もactivate()で黒→白へ戻しているので、パネルの帯も同じ状態に揃える。
    panel_->resetGradient();
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void GradientMapAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
    if (panel_) panel_->hide();
}

void GradientMapAction::onCancel()
{
    tool_.deactivate();
    if (panel_) panel_->hide();
}

void GradientMapAction::positionPanel() { centerPanelTop(panel_); }

void GradientMapAction::applyRenderState(QOpenGLShaderProgram *p)
{
    p->setUniformValue("uIsGradientMapTool", (isActive() && tool_.engaged()) ? 1 : 0);
}
