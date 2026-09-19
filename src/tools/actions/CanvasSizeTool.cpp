#include "tools/actions/CanvasSizeTool.h"

#include <QPainter>
#include <QPainterPath>
#include <QLineF>
#include <QFont>
#include <algorithm>

// ---------------------------------------------------------------------------
QVector2D CanvasSizeTool::handlePoint(int index, int x0, int y0, int w, int h)
{
    const int x1 = x0 + w, y1 = y0 + h;
    const float mx = (x0 + x1) / 2.0f, my = (y0 + y1) / 2.0f;
    switch (index) {
    case 0: return QVector2D(x0, y0); // TL
    case 1: return QVector2D(mx, y0); // TM
    case 2: return QVector2D(x1, y0); // TR
    case 3: return QVector2D(x0, my); // ML
    case 4: return QVector2D(x1, my); // MR
    case 5: return QVector2D(x0, y1); // BL
    case 6: return QVector2D(mx, y1); // BM
    default: return QVector2D(x1, y1); // BR (7)
    }
}

// ---------------------------------------------------------------------------
void CanvasSizeTool::activate(ToolContext &ctx)
{
    origW_ = ctx.canvasW;
    origH_ = ctx.canvasH;
    x0_ = 0; y0_ = 0;
    w_ = origW_; h_ = origH_;
    anchorIndex_ = 4; // 中央
    activeHandle_ = -1;
    engaged_ = true;
}

void CanvasSizeTool::deactivate()
{
    engaged_ = false;
    activeHandle_ = -1;
}

void CanvasSizeTool::setAnchorAndSize(int anchorIndex, int newW, int newH)
{
    anchorIndex_ = anchorIndex;
    w_ = std::max(1, newW);
    h_ = std::max(1, newH);

    const float ax = (anchorIndex_ % 3) / 2.0f; // 列: 0,0.5,1
    const float ay = (anchorIndex_ / 3) / 2.0f; // 行: 0,0.5,1
    const float anchorX = ax * origW_;
    const float anchorY = ay * origH_;
    x0_ = qRound(anchorX - ax * w_);
    y0_ = qRound(anchorY - ay * h_);
}

// ---------------------------------------------------------------------------
void CanvasSizeTool::paintOverlay(QPainter &painter, const ToolContext &ctx) const
{
    if (!engaged_ || !ctx.pixelToWidget) return;

    auto toWidget = [&](const QVector2D &p) { return ctx.pixelToWidget(p); };

    QPolygonF oldPoly;
    oldPoly << toWidget(QVector2D(0, 0)) << toWidget(QVector2D(origW_, 0))
            << toWidget(QVector2D(origW_, origH_)) << toWidget(QVector2D(0, origH_));

    QPolygonF newPoly;
    newPoly << toWidget(handlePoint(0, x0_, y0_, w_, h_)) << toWidget(handlePoint(2, x0_, y0_, w_, h_))
            << toWidget(handlePoint(7, x0_, y0_, w_, h_)) << toWidget(handlePoint(5, x0_, y0_, w_, h_));

    painter.setRenderHint(QPainter::Antialiasing);

    // 旧キャンバスのうち、新しい範囲の外(=切り取られる部分)を暗く示す
    QPainterPath oldPath; oldPath.addPolygon(oldPoly); oldPath.closeSubpath();
    QPainterPath newPath; newPath.addPolygon(newPoly); newPath.closeSubpath();
    QPainterPath removedPath = oldPath.subtracted(newPath);
    painter.fillPath(removedPath, QColor(0, 0, 0, 120));

    // 新しいキャンバス範囲の外枠
    QPen boxPen(QColor(255, 170, 40, 230));
    boxPen.setWidthF(1.8);
    painter.setPen(boxPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(newPoly);

    // ハンドル
    painter.setPen(QPen(QColor(255, 170, 40, 230), 1.2));
    painter.setBrush(QColor(255, 255, 255, 230));
    for (int i = 0; i < 8; i++) {
        QPointF p = toWidget(handlePoint(i, x0_, y0_, w_, h_));
        painter.drawRect(QRectF(p.x() - 4, p.y() - 4, 8, 8));
    }
}

// ---------------------------------------------------------------------------
void CanvasSizeTool::onMousePress(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    if (!engaged_) return;

    const QPointF widgetPos = event->position();

    const float hitRadiusWidget = 12.0f;
    for (int i = 0; i < 8; i++) {
        QVector2D hCanvas = handlePoint(i, x0_, y0_, w_, h_);
        QPointF   hWidget = ctx.pixelToWidget(hCanvas);
        const double dist = QLineF(hWidget, widgetPos).length();
        if (dist <= hitRadiusWidget) {
            activeHandle_ = i;
            dragFixedX0_ = x0_; dragFixedY0_ = y0_;
            dragFixedX1_ = x0_ + w_; dragFixedY1_ = y0_ + h_;
            return;
        }
    }
}

void CanvasSizeTool::onMouseMove(QMouseEvent *event, ToolContext &ctx)
{
    if (activeHandle_ == -1) return;
    const QVector2D mouseCanvas = ctx.widgetToPixel(event->position());
    const int mx = qRound(mouseCanvas.x()), my = qRound(mouseCanvas.y());

    int nx0 = dragFixedX0_, ny0 = dragFixedY0_, nx1 = dragFixedX1_, ny1 = dragFixedY1_;
    switch (activeHandle_) {
    case 0: nx0 = mx; ny0 = my; break; // TL
    case 1: ny0 = my; break;           // TM
    case 2: nx1 = mx; ny0 = my; break; // TR
    case 3: nx0 = mx; break;           // ML
    case 4: nx1 = mx; break;           // MR
    case 5: nx0 = mx; ny1 = my; break; // BL
    case 6: ny1 = my; break;           // BM
    default: nx1 = mx; ny1 = my; break; // BR (7)
    }

    const int MIN_SIZE = 8;
    if (nx0 > dragFixedX1_ - MIN_SIZE) nx0 = dragFixedX1_ - MIN_SIZE;
    if (nx1 < dragFixedX0_ + MIN_SIZE) nx1 = dragFixedX0_ + MIN_SIZE;
    if (ny0 > dragFixedY1_ - MIN_SIZE) ny0 = dragFixedY1_ - MIN_SIZE;
    if (ny1 < dragFixedY0_ + MIN_SIZE) ny1 = dragFixedY0_ + MIN_SIZE;

    x0_ = nx0; y0_ = ny0;
    w_  = nx1 - nx0; h_ = ny1 - ny0;

    ctx.requestRepaint();
}

void CanvasSizeTool::onMouseRelease(QMouseEvent *event, ToolContext &ctx)
{
    Q_UNUSED(event); Q_UNUSED(ctx);
    activeHandle_ = -1;
}

// ---------------------------------------------------------------------------
void CanvasSizeTool::confirm(ToolContext &ctx)
{
    if (!engaged_) return;

    const bool identity = (x0_ == 0 && y0_ == 0 && w_ == origW_ && h_ == origH_);
    if (!identity && ctx.resizeCanvasKeepingContent) {
        ctx.resizeCanvasKeepingContent(w_, h_, x0_, y0_);
    }

    deactivate();
    ctx.requestRepaint();
}
