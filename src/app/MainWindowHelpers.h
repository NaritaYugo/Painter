#pragma once

#include "tools/core/ToolConfig.h"
#include "canvas/CanvasPane.h"

#include <QSplitter>
#include <QVector>

namespace MainWindowHelpers {

inline int brushSizeFor(const ToolConfig *config, ToolType tool)
{
    if (tool == ToolType::Eraser)    return config->eraser().size();
    if (tool == ToolType::Blur)      return config->blur().size();
    if (tool == ToolType::Warp)      return config->warp().size();
    if (tool == ToolType::Selection) return config->selection().size();
    if (tool == ToolType::Airbrush)  return config->airbrush().size();
    return config->pen().size();
}

inline void setBrushSizeFor(ToolConfig *config, ToolType tool, int pixels)
{
    if (tool == ToolType::Eraser)         config->eraser().setSize(pixels);
    else if (tool == ToolType::Blur)      config->blur().setSize(pixels);
    else if (tool == ToolType::Warp)      config->warp().setSize(pixels);
    else if (tool == ToolType::Selection) config->selection().setSize(pixels);
    else if (tool == ToolType::Airbrush)  config->airbrush().setSize(pixels);
    else                                  config->pen().setSize(pixels);
}

inline void collectPanes(QWidget *node, QVector<CanvasPane *> &out)
{
    if (!node) return;
    if (auto *pane = qobject_cast<CanvasPane *>(node)) {
        out.append(pane);
        return;
    }
    if (auto *split = qobject_cast<QSplitter *>(node)) {
        for (int i = 0; i < split->count(); ++i)
            collectPanes(split->widget(i), out);
    }
}

inline CanvasPane *firstPaneIn(QWidget *node)
{
    QVector<CanvasPane *> panes;
    collectPanes(node, panes);
    return panes.isEmpty() ? nullptr : panes.first();
}

} // namespace MainWindowHelpers
