#pragma once

#include "tools/core/Tool.h"
#include <cmath>

// ---------------------------------------------------------------------------
// RotateTool
// ---------------------------------------------------------------------------
// 旧 CanvasWidget::mousePressEvent/mouseMoveEvent の ToolType::Rotate 分岐を移したもの。
// ウィジェット中心を基準に、マウス角度の差分だけ ViewTransform::rotate() を呼ぶ。
// ---------------------------------------------------------------------------
class RotateTool : public Tool
{
public:
    // ctx.widgetCenter() はビュー空間(デバイスpx)を返し、view->rotate() もその座標系で
    // offsetを回すため、マウス座標(論理px)は viewDpr を掛けて揃える
    // (ToolContext::viewDpr のコメント参照)。角度だけなら倍率は影響しないが、
    // 回転の軸として渡す center と単位が食い違うとキャンバスが軸からずれて回る。
    void onMousePress(QMouseEvent *event, ToolContext &ctx) override
    {
        QVector2D center = ctx.widgetCenter();
        float dx = event->position().x() * ctx.viewDpr - center.x();
        float dy = event->position().y() * ctx.viewDpr - center.y();
        lastAngle_ = std::atan2(dy, dx);
        dragging_  = true;
    }

    void onMouseMove(QMouseEvent *event, ToolContext &ctx) override
    {
        if (!dragging_ || !(event->buttons() & Qt::LeftButton)) return;

        QVector2D center = ctx.widgetCenter();
        float dx = event->position().x() * ctx.viewDpr - center.x();
        float dy = event->position().y() * ctx.viewDpr - center.y();
        float curAngle = std::atan2(dy, dx);

        ctx.view->rotate(center, lastAngle_ - curAngle);
        lastAngle_ = curAngle;
        ctx.requestRepaint();
    }

    void onMouseRelease(QMouseEvent *event, ToolContext &ctx) override
    {
        Q_UNUSED(event); Q_UNUSED(ctx);
        dragging_ = false;
    }

    bool isActive() const override { return dragging_; }
    bool transformsViewWhileActive() const override { return true; }

private:
    bool  dragging_  = false;
    float lastAngle_ = 0.0f;
};