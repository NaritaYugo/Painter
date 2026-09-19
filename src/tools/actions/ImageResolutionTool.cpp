#include "tools/actions/ImageResolutionTool.h"

void ImageResolutionTool::activate(ToolContext &ctx)
{
    origW_ = ctx.canvasW;
    origH_ = ctx.canvasH;
    w_ = origW_;
    h_ = origH_;
    engaged_ = true;
}

void ImageResolutionTool::deactivate()
{
    engaged_ = false;
}

void ImageResolutionTool::confirm(ToolContext &ctx)
{
    if (!engaged_) return;

    const bool identity = (w_ == origW_ && h_ == origH_);
    if (!identity && w_ > 0 && h_ > 0 && ctx.resampleCanvasResolution) {
        ctx.resampleCanvasResolution(w_, h_);
    }

    deactivate();
    ctx.requestRepaint();
}
