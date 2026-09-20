#include "actions/CanvasSizeActions.h"
#include "actions/CanvasActionHost.h"
#include "actions/CanvasActionController.h"
#include "document/CanvasDocument.h"
#include "dialogs/CanvasSizePanel.h"
#include "dialogs/ImageResolutionPanel.h"

#include <QWidget>

// ===========================================================================
// CanvasSizeAction
// ===========================================================================
bool CanvasSizeAction::canActivate() const
{
    CanvasDocument *doc = host_.hostDocument();
    return doc && !doc->layers.isEmpty();
}

bool CanvasSizeAction::onActivate()
{
    ToolContext &ctx = host_.hostToolContext();
    tool_.activate(ctx);

    if (!panel_) {
        QWidget *host = host_.hostWidget();
        panel_ = new CanvasSizePanel(host);
        QObject::connect(panel_, &CanvasSizePanel::settingsChanged, host,
            [this](int anchorIndex, int w, int h) {
                tool_.setAnchorAndSize(anchorIndex, w, h);
                host_.hostUpdate();
            });
        QObject::connect(panel_, &CanvasSizePanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &CanvasSizePanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->resetTo(ctx.canvasW, ctx.canvasH);
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void CanvasSizeAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
    if (panel_) panel_->hide();
}

void CanvasSizeAction::onCancel()
{
    tool_.deactivate();
    if (panel_) panel_->hide();
}

void CanvasSizeAction::positionPanel()
{
    centerPanelTop(panel_);
}

bool CanvasSizeAction::handleMousePress(QMouseEvent *e, ToolContext &ctx)
{
    tool_.onMousePress(e, ctx);
    if (!tool_.engaged()) active_ = false;
    if (panel_) panel_->setSizeFields(tool_.newWidth(), tool_.newHeight());
    return true;
}

bool CanvasSizeAction::handleMouseMove(QMouseEvent *e, ToolContext &ctx)
{
    tool_.onMouseMove(e, ctx);
    if (panel_) panel_->setSizeFields(tool_.newWidth(), tool_.newHeight());
    return true;
}

// ===========================================================================
// ImageResolutionAction
// ===========================================================================
bool ImageResolutionAction::canActivate() const
{
    CanvasDocument *doc = host_.hostDocument();
    return doc && !doc->layers.isEmpty();
}

bool ImageResolutionAction::onActivate()
{
    ToolContext &ctx = host_.hostToolContext();
    tool_.activate(ctx);

    if (!panel_) {
        QWidget *host = host_.hostWidget();
        panel_ = new ImageResolutionPanel(host);
        QObject::connect(panel_, &ImageResolutionPanel::sizeChanged, host,
            [this](int w, int h) { tool_.setSize(w, h); });
        QObject::connect(panel_, &ImageResolutionPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &ImageResolutionPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->resetTo(ctx.canvasW, ctx.canvasH);
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void ImageResolutionAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
    if (panel_) panel_->hide();
}

void ImageResolutionAction::onCancel()
{
    tool_.deactivate();
    if (panel_) panel_->hide();
}

void ImageResolutionAction::positionPanel()
{
    centerPanelTop(panel_);
}
