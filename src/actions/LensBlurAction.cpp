#include "actions/LensBlurAction.h"
#include "actions/CanvasActionHost.h"
#include "actions/CanvasActionController.h"
#include "tools/core/ToolContext.h"
#include "document/CanvasDocument.h"

#include "dialogs/LensBlurPanel.h"

#include <QOpenGLShaderProgram>
#include <QWidget>

bool LensBlurAction::canActivate() const
{
    // フィルターは実ピクセルを持つレイヤー(Normal/Text)のみ対象。
    CanvasDocument *doc = host_.hostDocument();
    if (!doc || doc->layers.isEmpty()) return false;
    const LayerType lt = doc->activeLayer().layerType;
    return lt == LayerType::Normal || lt == LayerType::Text;
}

bool LensBlurAction::onActivate()
{
    ToolContext &ctx = host_.hostToolContext();
    tool_.activate(ctx);
    if (!tool_.engaged()) return false; // レイヤーが0x0など(通常起こらない)
    if (!panel_) {
        QWidget *host = host_.hostWidget();
        panel_ = new LensBlurPanel(host);
        QObject::connect(panel_, &LensBlurPanel::radiusChanged, host, [this](float r) {
            tool_.setRadiusPx(host_.hostToolContext(), r);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &LensBlurPanel::bladesChanged, host, [this](int b) {
            tool_.setBlades(host_.hostToolContext(), b);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &LensBlurPanel::bladeRotChanged, host, [this](float d) {
            tool_.setBladeRotDeg(host_.hostToolContext(), d);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &LensBlurPanel::highlightBoostChanged, host, [this](float v) {
            tool_.setHighlightBoost(host_.hostToolContext(), v);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &LensBlurPanel::thresholdChanged, host, [this](float v) {
            tool_.setThreshold(host_.hostToolContext(), v);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &LensBlurPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &LensBlurPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->setRadiusPx(tool_.radiusPx());
    panel_->setBlades(tool_.blades());
    panel_->setBladeRotDeg(tool_.bladeRotDeg());
    panel_->setHighlightBoost(tool_.highlightBoost());
    panel_->setThreshold(tool_.threshold());
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void LensBlurAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
    if (panel_) panel_->hide();
}

void LensBlurAction::onCancel()
{
    tool_.deactivate();
    if (panel_) panel_->hide();
}

void LensBlurAction::positionPanel() { centerPanelTop(panel_); }

void LensBlurAction::applyRenderState(QOpenGLShaderProgram *p)
{
    const bool on = isActive() && tool_.engaged();
    p->setUniformValue("uIsLensBlurTool", on ? 1 : 0);
    if (on) {
        p->setUniformValue("uLensBlurOriginPx", tool_.previewOriginPx());
        p->setUniformValue("uLensBlurSizePx",   tool_.previewSizePx());
    }
}
