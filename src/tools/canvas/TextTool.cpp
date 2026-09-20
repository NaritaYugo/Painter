#include "tools/canvas/TextTool.h"
#include "document/CanvasDocument.h"

#include <QMouseEvent>
#include <QPainter>
#include <QLineF>
#include <QtMath>
#include <array>
#include <cmath>

namespace {

// ボックスの回転後4隅(キャンバスpx座標)。0=TL,1=TR,2=BR,3=BL(ローカル空間での並び)。
std::array<QVector2D, 4> rotatedBoxCorners(const TextParams &box)
{
    const float hw = box.width  * box.scale / 2.0f;
    const float hh = box.height * box.scale / 2.0f;
    const float rad = qDegreesToRadians(box.rotation);
    const float c = std::cos(rad), s = std::sin(rad);
    const std::array<QVector2D, 4> local = {
        QVector2D(-hw, -hh), QVector2D(hw, -hh), QVector2D(hw, hh), QVector2D(-hw, hh)
    };
    std::array<QVector2D, 4> out;
    for (int i = 0; i < 4; i++) {
        out[i] = QVector2D(box.cx + local[i].x() * c - local[i].y() * s,
                            box.cy + local[i].x() * s + local[i].y() * c);
    }
    return out;
}

// pがボックスの矩形内(回転を考慮したローカル空間で判定)にあるか。marginはボックスの
// 外側にさらに広げる許容範囲(回転ハンドル用の「枠のすぐ外」判定に使う)。
bool pointInRotatedBox(const TextParams &box, const QVector2D &p, float marginCanvasPx = 0.0f)
{
    const float rad = qDegreesToRadians(-box.rotation);
    const float c = std::cos(rad), s = std::sin(rad);
    const float dx = p.x() - box.cx, dy = p.y() - box.cy;
    const float lx = dx * c - dy * s;
    const float ly = dx * s + dy * c;
    const float hw = box.width  * box.scale / 2.0f + marginCanvasPx;
    const float hh = box.height * box.scale / 2.0f + marginCanvasPx;
    return std::abs(lx) <= hw && std::abs(ly) <= hh;
}

// スクリーン(ウィジェット)px単位の距離を、指定したキャンバス座標付近でのキャンバスpx
// 単位に変換する(ズーム倍率に依らずハンドル当たり判定/スナップ閾値を一定の見た目の
// 大きさにするため)。FreeTransformToolの12pxハンドル判定と同じ考え方。
float screenPxToCanvasPx(const ToolContext &ctx, const QVector2D &atCanvasPos, float screenPx)
{
    if (!ctx.pixelToWidget) return screenPx;
    const QPointF p0 = ctx.pixelToWidget(atCanvasPos);
    const QPointF p1 = ctx.pixelToWidget(atCanvasPos + QVector2D(1.0f, 0.0f));
    const double scale = qMax(1e-4, QLineF(p0, p1).length());
    return float(screenPx / scale);
}

// 移動中のボックスのAABB(width/height * scale基準、回転は考慮せずAABBで近似)と、
// 他ボックスのAABB/キャンバス中央にスナップする。cx,cyを直接書き換える。
void applySnap(ToolContext &ctx, const std::vector<TextParams> &boxes, int selfIndex,
               float &cx, float &cy, float halfW, float halfH,
               std::optional<float> &guideX, std::optional<float> &guideY)
{
    guideX.reset();
    guideY.reset();
    const float thresh = screenPxToCanvasPx(ctx, QVector2D(cx, cy), 8.0f);

    // 自分のAABB各エッジ/中心の候補値(x方向・y方向それぞれ)
    const float myXs[3] = { cx - halfW, cx, cx + halfW };
    const float myYs[3] = { cy - halfH, cy, cy + halfH };

    // 比較対象: 他ボックスのAABBエッジ/中心 + キャンバス中央
    std::vector<float> targetXs, targetYs;
    targetXs.push_back(ctx.canvasW / 2.0f);
    targetYs.push_back(ctx.canvasH / 2.0f);
    for (int i = 0; i < (int)boxes.size(); i++) {
        if (i == selfIndex) continue;
        const TextParams &b = boxes[i];
        const float bhw = b.width  * b.scale / 2.0f;
        const float bhh = b.height * b.scale / 2.0f;
        targetXs.push_back(b.cx - bhw); targetXs.push_back(b.cx); targetXs.push_back(b.cx + bhw);
        targetYs.push_back(b.cy - bhh); targetYs.push_back(b.cy); targetYs.push_back(b.cy + bhh);
    }

    float bestDx = thresh, bestNewCx = 0.0f, bestGuideX = 0.0f; bool foundX = false;
    for (float mx : myXs) {
        for (float tx : targetXs) {
            const float d = std::abs(mx - tx);
            if (d < bestDx) { bestDx = d; bestNewCx = cx + (tx - mx); bestGuideX = tx; foundX = true; }
        }
    }
    float bestDy = thresh, bestNewCy = 0.0f, bestGuideY = 0.0f; bool foundY = false;
    for (float my : myYs) {
        for (float ty : targetYs) {
            const float d = std::abs(my - ty);
            if (d < bestDy) { bestDy = d; bestNewCy = cy + (ty - my); bestGuideY = ty; foundY = true; }
        }
    }

    if (foundX) { cx = bestNewCx; guideX = bestGuideX; }
    if (foundY) { cy = bestNewCy; guideY = bestGuideY; }
}

} // namespace

void TextTool::validateSelection(ToolContext &ctx)
{
    if (!ctx.doc || ctx.doc->layerCount() == 0) { selectedBox_ = -1; return; }
    const int layerIdx = ctx.doc->activeLayerIndex();
    if (selectedLayerIndex_ != layerIdx) { selectedBox_ = -1; selectedLayerIndex_ = -1; return; }
    if (ctx.doc->layerRef(layerIdx).layerType != LayerType::Text) { selectedBox_ = -1; return; }
    const Layer &layer = ctx.doc->layerRef(layerIdx);
    if (selectedBox_ < 0 || selectedBox_ >= (int)layer.textBoxes.size()) selectedBox_ = -1;
}

void TextTool::onMousePress(QMouseEvent *event, ToolContext &ctx)
{
    if (!ctx.doc || ctx.doc->layerCount() == 0) return;
    if (ctx.doc->activeLayer().layerType != LayerType::Text) { selectedBox_ = -1; return; }
    if (!ctx.widgetToPixel || !ctx.pixelToWidget) return;

    const int layerIdx = ctx.doc->activeLayerIndex();
    validateSelection(ctx);
    Layer &layer = ctx.doc->layerRef(layerIdx);
    const QVector2D mouseCanvas = ctx.widgetToPixel(event->position());

    // 1) 選択中ボックスがあれば、まずその4隅のハンドルに当たっているか判定する
    if (selectedBox_ >= 0) {
        const TextParams &box = layer.textBoxes[selectedBox_];
        const auto corners = rotatedBoxCorners(box);
        const float hitRadiusWidget = 12.0f;
        for (int i = 0; i < 4; i++) {
            const QPointF cWidget = ctx.pixelToWidget(corners[i]);
            if (QLineF(cWidget, event->position()).length() <= hitRadiusWidget) {
                dragMode_          = DragMode::Corner;
                activeCorner_      = i;
                dragStartScale_    = box.scale;
                dragStartMouseDist_ = qMax(1.0f, (mouseCanvas - QVector2D(box.cx, box.cy)).length());
                return;
            }
        }
        // 2) ハンドルには当たらないが、ボックスのすぐ外側(回転リング)なら回転
        const float ringMargin = screenPxToCanvasPx(ctx, QVector2D(box.cx, box.cy), 34.0f);
        if (!pointInRotatedBox(box, mouseCanvas) && pointInRotatedBox(box, mouseCanvas, ringMargin)) {
            dragMode_        = DragMode::Rotate;
            rotateCenter_    = QVector2D(box.cx, box.cy);
            lastMouseCanvas_ = mouseCanvas;
            return;
        }
    }

    // 3) 全ボックスを最前面から順にヒットテストして選択+移動開始
    for (int i = (int)layer.textBoxes.size() - 1; i >= 0; i--) {
        if (pointInRotatedBox(layer.textBoxes[i], mouseCanvas)) {
            selectedLayerIndex_ = layerIdx;
            selectedBox_        = i;
            dragMode_           = DragMode::Move;
            pressMouseCanvas_   = mouseCanvas;
            dragStartCx_        = layer.textBoxes[i].cx;
            dragStartCy_        = layer.textBoxes[i].cy;
            return;
        }
    }

    // 4) どこにも当たらない -> 新規ボックスを作成して選択し、即座に編集を開始する
    TextParams box;
    box.cx = mouseCanvas.x();
    box.cy = mouseCanvas.y();
    layer.textBoxes.push_back(box);
    selectedLayerIndex_ = layerIdx;
    selectedBox_        = (int)layer.textBoxes.size() - 1;
    dragMode_           = DragMode::None;
    if (ctx.startOrEditTextBox) ctx.startOrEditTextBox(selectedBox_);
}

void TextTool::onMouseMove(QMouseEvent *event, ToolContext &ctx)
{
    if (dragMode_ == DragMode::None) return;
    if (!ctx.doc || !ctx.widgetToPixel) { dragMode_ = DragMode::None; return; }
    validateSelection(ctx);
    if (selectedBox_ < 0) { dragMode_ = DragMode::None; return; }

    Layer &layer = ctx.doc->layerRef(selectedLayerIndex_);
    TextParams &box = layer.textBoxes[selectedBox_];
    const QVector2D mouseCanvas = ctx.widgetToPixel(event->position());

    if (dragMode_ == DragMode::Move) {
        float targetCx = dragStartCx_ + (mouseCanvas.x() - pressMouseCanvas_.x());
        float targetCy = dragStartCy_ + (mouseCanvas.y() - pressMouseCanvas_.y());
        applySnap(ctx, layer.textBoxes, selectedBox_, targetCx, targetCy,
                  box.width * box.scale / 2.0f, box.height * box.scale / 2.0f,
                  snapGuideX_, snapGuideY_);
        box.cx = targetCx;
        box.cy = targetCy;
    } else if (dragMode_ == DragMode::Corner) {
        const float dist = qMax(1.0f, (mouseCanvas - QVector2D(box.cx, box.cy)).length());
        box.scale = qMax(0.05f, dragStartScale_ * (dist / dragStartMouseDist_));
    } else if (dragMode_ == DragMode::Rotate) {
        const QVector2D d0 = lastMouseCanvas_ - rotateCenter_;
        const QVector2D d1 = mouseCanvas - rotateCenter_;
        const float a0 = std::atan2(d0.y(), d0.x());
        const float a1 = std::atan2(d1.y(), d1.x());
        box.rotation += qRadiansToDegrees(a1 - a0);
        lastMouseCanvas_ = mouseCanvas;
    }

    if (ctx.requestTextRasterize) ctx.requestTextRasterize(selectedLayerIndex_);
    if (ctx.requestRepaint) ctx.requestRepaint();
}

void TextTool::onMouseRelease(QMouseEvent *event, ToolContext &ctx)
{
    Q_UNUSED(event); Q_UNUSED(ctx);
    dragMode_     = DragMode::None;
    activeCorner_ = -1;
    snapGuideX_.reset();
    snapGuideY_.reset();
}

void TextTool::onMouseDoubleClick(QMouseEvent *event, ToolContext &ctx)
{
    if (!ctx.doc || ctx.doc->layerCount() == 0) return;
    if (ctx.doc->activeLayer().layerType != LayerType::Text) return;
    if (!ctx.widgetToPixel) return;

    const int layerIdx = ctx.doc->activeLayerIndex();
    Layer &layer = ctx.doc->layerRef(layerIdx);
    const QVector2D mouseCanvas = ctx.widgetToPixel(event->position());

    for (int i = (int)layer.textBoxes.size() - 1; i >= 0; i--) {
        if (pointInRotatedBox(layer.textBoxes[i], mouseCanvas)) {
            selectedLayerIndex_ = layerIdx;
            selectedBox_        = i;
            dragMode_           = DragMode::None;
            if (ctx.startOrEditTextBox) ctx.startOrEditTextBox(i);
            return;
        }
    }
}

void TextTool::paintOverlay(QPainter &painter, const ToolContext &ctx) const
{
    if (!ctx.doc || !ctx.pixelToWidget) return;
    if (ctx.doc->layerCount() == 0 || ctx.doc->activeLayer().layerType != LayerType::Text) return;
    const int layerIdx = ctx.doc->activeLayerIndex();
    const Layer &layer = ctx.doc->layerRef(layerIdx);

    painter.setRenderHint(QPainter::Antialiasing);

    const bool hasSelection = selectedBox_ >= 0 && selectedLayerIndex_ == layerIdx
                            && selectedBox_ < (int)layer.textBoxes.size();

    // 選択中以外の全ボックスは、位置の目安として点線の枠だけを表示する。
    for (int i = 0; i < (int)layer.textBoxes.size(); i++) {
        if (hasSelection && i == selectedBox_) continue;
        const auto corners = rotatedBoxCorners(layer.textBoxes[i]);
        QPolygonF poly;
        for (int c = 0; c < 4; c++) poly << ctx.pixelToWidget(corners[c]);
        QPen dashPen(QColor(160, 160, 160, 200));
        dashPen.setStyle(Qt::DashLine);
        dashPen.setWidthF(1.0);
        painter.setPen(dashPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(poly);
    }

    if (!hasSelection) return;

    const TextParams &box = layer.textBoxes[selectedBox_];
    const auto corners = rotatedBoxCorners(box);
    QPointF w[4];
    for (int i = 0; i < 4; i++) w[i] = ctx.pixelToWidget(corners[i]);

    QPolygonF poly;
    for (int i = 0; i < 4; i++) poly << w[i];
    QPen boxPen(QColor(90, 220, 140, 230));
    boxPen.setWidthF(1.5);
    painter.setPen(boxPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(poly);

    painter.setPen(QPen(QColor(90, 220, 140, 230), 1.2));
    painter.setBrush(QColor(255, 255, 255, 230));
    for (int i = 0; i < 4; i++)
        painter.drawRect(QRectF(w[i].x() - 4, w[i].y() - 4, 8, 8));

    if (snapGuideX_) {
        const QPointF a = ctx.pixelToWidget(QVector2D(*snapGuideX_, 0.0f));
        const QPointF b = ctx.pixelToWidget(QVector2D(*snapGuideX_, float(ctx.canvasH)));
        QPen guidePen(QColor(255, 90, 200, 220));
        guidePen.setStyle(Qt::DashLine);
        painter.setPen(guidePen);
        painter.drawLine(a, b);
    }
    if (snapGuideY_) {
        const QPointF a = ctx.pixelToWidget(QVector2D(0.0f, *snapGuideY_));
        const QPointF b = ctx.pixelToWidget(QVector2D(float(ctx.canvasW), *snapGuideY_));
        QPen guidePen(QColor(255, 90, 200, 220));
        guidePen.setStyle(Qt::DashLine);
        painter.setPen(guidePen);
        painter.drawLine(a, b);
    }
}
