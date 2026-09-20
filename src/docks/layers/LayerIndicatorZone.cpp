#include "docks/layers/LayerIndicatorZone.h"

#include "canvas/CanvasWidget.h"
#include "components/ThemeColors.h"
#include "docks/layers/LayerDockLayout.h"

#include <QPainter>
#include <cmath>

LayerIndicatorZone::LayerIndicatorZone(CanvasWidget *gl, QWidget *parent) : QWidget(parent), glWidget(gl)
{
        setMinimumWidth(90);
        setMouseTracking(false);
    }

void LayerIndicatorZone::setCanvasWidget(CanvasWidget *gl)
{ glWidget = gl; update(); }

void LayerIndicatorZone::setScope(int scope)
{
        if (scope_ == scope) return;
        scope_ = scope;
        dots_.clear();
        update();
    }

void LayerIndicatorZone::paintEvent(QPaintEvent *)
{
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // レイヤーリスト側(splitter左)と背景色を揃える。
        p.fillRect(rect(), Theme::bgBase);

        const CanvasDocument &doc = glWidget->document();
        QVector<QVector<int>> rows = computeRows(doc, scope_);
        dots_.clear();
        if (rows.isEmpty()) return;

        const int rowCount = rows.size();
        const int active = doc.activeLayerIndex();

        // ドラッグ中に隠す(=浮かせて描く)べきレイヤー群。
        QVector<int> draggedLayers;
        if (dragging_) {
            if (dragRowMode_) {
                for (const auto &r : rows) if (r.contains(dragLayer_)) { draggedLayers = r; break; }
            } else {
                draggedLayers = { dragLayer_ };
            }
        }

        HoverTarget hover;
        bool showGap = dragging_ && computeHoverTarget(dragPos_, rows, hover, dragRowMode_);

        // 行の可視位置(0=一番上)ごとに、実際に描画するY座標を計算する。
        int slotCount = rowCount + ((showGap && hover.isRowInsert) ? 1 : 0);
        qreal rowH = qreal(height()) / qMax(1, slotCount);

        // 可視行rv(0=最前面)→ 隙間を考慮した描画スロット位置。
        auto rowSlot = [&](int rv) -> int {
            if (!(showGap && hover.isRowInsert)) return rv;
            int dataRow = rowCount - 1 - rv;
            // 挿入境界hover.boundaryBより下(dataRowが小さい側)は変化なし、境界以上のdataRowを持つ行は1スロット分下にずれる。
            int gapSlotFromTop = rowCount - hover.boundaryB; // 隙間の可視スロット位置
            return (rv >= gapSlotFromTop) ? rv + 1 : rv;
        };

        QVector<QPointF> col0Centers;

        for (int rv = 0; rv < rowCount; rv++) {
            int dataRow = rowCount - 1 - rv;
            const QVector<int> &cols = rows[dataRow];
            int slot = rowSlot(rv);
            qreal cy = rowH * slot + rowH / 2.0;

            bool colGapHere = showGap && !hover.isRowInsert && hover.dataRow == dataRow;
            int maxCols = 1;
            for (const auto &r : rows) maxCols = qMax(maxCols, r.size());
            if (colGapHere) maxCols = qMax(maxCols, cols.size() + 1);
            qreal colW = qreal(width()) / maxCols;

            QPointF firstCenter, lastCenter;
            bool any = false;
            for (int c = 0; c < cols.size(); c++) {
                int slotC = (colGapHere && c >= hover.col) ? c + 1 : c;
                qreal cx = colW * slotC + colW / 2.0;
                QPointF center(cx, cy);
                if (!any) { firstCenter = center; any = true; }
                if (c == 0) col0Centers.append(center);
                lastCenter = center;
                if (dragging_ && draggedLayers.contains(cols[c])) continue; // ドラッグ中の本体はグリップ位置に描かない
                dots_.append({ center, cols[c] });
            }

            if (cols.size() > 1) {
                p.setPen(QPen(Theme::hoverBg, 2));
                p.drawLine(firstCenter, lastCenter);
            }
        }

        if (col0Centers.size() > 1) {
            p.setPen(QPen(Theme::hoverBg, 2));
            p.drawLine(col0Centers.first(), col0Centers.last());
        }

        // 挿入先の隙間を枠線で示す。
        if (showGap) {
            if (hover.isRowInsert) {
                int gapSlot = rowCount - hover.boundaryB;
                QRectF gapRect(4, rowH * gapSlot + 2, width() - 8, rowH - 4);
                p.setPen(QPen(Theme::scrollHandleHover, 2, Qt::DashLine));
                p.setBrush(Theme::bgButton);
                p.drawRoundedRect(gapRect, 5, 5);
            } else {
                int rv = rowCount - 1 - hover.dataRow;
                int slot = rowSlot(rv);
                qreal cy = rowH * slot + rowH / 2.0;
                int maxCols = 1;
                for (const auto &r : rows) maxCols = qMax(maxCols, r.size());
                maxCols = qMax(maxCols, int(rows[hover.dataRow].size()) + 1);
                qreal colW = qreal(width()) / maxCols;
                qreal cx = colW * hover.col + colW / 2.0;
                qreal dotSize = qMin(qMin(rowH, colW) * 0.55, 18.0);
                QRectF gapRect(cx - dotSize / 2 - 4, cy - dotSize / 2 - 4, dotSize + 8, dotSize + 8);
                p.setPen(QPen(Theme::scrollHandleHover, 2, Qt::DashLine));
                p.setBrush(Theme::bgButton);
                p.drawRoundedRect(gapRect, 5, 5);
            }
        }

        qreal dotSizeBase = qMin(qMin(qreal(height()) / qMax(1, slotCount), qreal(width())) * 0.55, 18.0);
        for (const DotInfo &info : dots_) {
            const Layer &layer = doc.layers[info.layerIndex];
            bool selected = (info.layerIndex == active);
            QRectF r(info.center.x() - dotSizeBase / 2, info.center.y() - dotSizeBase / 2, dotSizeBase, dotSizeBase);
            p.setBrush(blendModeColor(layer.blendMode));
            p.setPen(selected ? QPen(Theme::textBright, 2) : QPen(Qt::NoPen));
            drawLayerIndicatorShape(p, r, layer.layerType);
        }

        // ドラッグ中の本体はカーソル位置に浮かせて描く。
        if (dragging_ && dragLayer_ >= 0 && dragLayer_ < doc.layerCount()) {
            qreal ghostSize = dotSizeBase * 1.25;
            if (dragRowMode_ && draggedLayers.size() > 1) {
                qreal spacing = ghostSize * 0.7;
                qreal startX  = dragPos_.x() - spacing * (draggedLayers.size() - 1) / 2.0;
                for (int i = 0; i < draggedLayers.size(); i++) {
                    int li = draggedLayers[i];
                    if (li < 0 || li >= doc.layerCount()) continue;
                    const Layer &layer = doc.layers[li];
                    QPointF c(startX + spacing * i, dragPos_.y());
                    QRectF r(c.x() - ghostSize / 2, c.y() - ghostSize / 2, ghostSize, ghostSize);
                    p.setBrush(blendModeColor(layer.blendMode));
                    p.setPen(QPen(Theme::textBright, 2.5));
                    drawLayerIndicatorShape(p, r, layer.layerType);
                }
            } else {
                const Layer &layer = doc.layers[dragLayer_];
                QRectF r(dragPos_.x() - ghostSize / 2, dragPos_.y() - ghostSize / 2, ghostSize, ghostSize);
                p.setBrush(blendModeColor(layer.blendMode));
                p.setPen(QPen(Theme::textBright, 2.5));
                drawLayerIndicatorShape(p, r, layer.layerType);
            }
        }
    }

void LayerIndicatorZone::mousePressEvent(QMouseEvent *event)
{
        if (event->button() != Qt::LeftButton) return;
        QPointF pos = event->position();
        qreal best = 1e9;
        int bestLayer = -1;
        for (const DotInfo &info : dots_) {
            qreal d = QLineF(pos, info.center).length();
            if (d < best) { best = d; bestLayer = info.layerIndex; }
        }
        if (bestLayer >= 0 && best < 30.0) {
            pressLayer_ = bestLayer;
            pressPos_   = pos;
        }
    }

void LayerIndicatorZone::mouseMoveEvent(QMouseEvent *event)
{
        if (pressLayer_ < 0) return;
        QPointF pos = event->position();
        if (!dragging_) {
            if (QLineF(pos, pressPos_).length() < 6.0) return;
            dragging_  = true;
            dragLayer_ = pressLayer_;
            // root列(一番左)をつかんでいたら、その行のクリッピングレイヤーごと移動する「行ドラッグ」モードにする(この場合、上下の並べ替えのみ許可)。
            dragRowMode_ = false;
            const CanvasDocument &doc = glWidget->document();
            QVector<QVector<int>> rows = computeRows(doc, scope_);
            for (const auto &r : rows) {
                if (r.contains(dragLayer_)) { dragRowMode_ = (r.first() == dragLayer_); break; }
            }
        }
        dragPos_ = pos;
        update();
    }

void LayerIndicatorZone::mouseReleaseEvent(QMouseEvent *event)
{
        if (event->button() != Qt::LeftButton) return;
        if (dragging_) {
            commitDrag();
            dragging_    = false;
            dragLayer_   = -1;
            dragRowMode_ = false;
        } else if (pressLayer_ >= 0) {
            emit layerClicked(pressLayer_);
        }
        pressLayer_ = -1;
        update();
    }

void LayerIndicatorZone::mouseDoubleClickEvent(QMouseEvent *event)
{
        if (event->button() != Qt::LeftButton) return;
        QPointF pos = event->position();
        qreal best = 1e9;
        int bestLayer = -1;
        for (const DotInfo &info : dots_) {
            qreal d = QLineF(pos, info.center).length();
            if (d < best) { best = d; bestLayer = info.layerIndex; }
        }
        if (bestLayer < 0 || best >= 30.0) return;
        const CanvasDocument &doc = glWidget->document();
        if (bestLayer >= doc.layerCount()) return;
        if (doc.layers[bestLayer].layerType == LayerType::Folder) {
            pressLayer_ = -1;
            emit folderDoubleClicked(bestLayer);
        }
    }

bool LayerIndicatorZone::computeHoverTarget(const QPointF &pos, const QVector<QVector<int>> &rows,
                                            HoverTarget &out, bool rowOnly) const
{
        int rowCount = rows.size();
        if (rowCount == 0) return false;

        qreal rowH = qreal(height()) / rowCount;
        int rv = qBound(0, int(pos.y() / rowH), rowCount - 1);
        qreal rowLocalY = pos.y() - rv * rowH;
        int dataRow = rowCount - 1 - rv;

        if (rowOnly) {
            out.isRowInsert = true;
            out.boundaryB   = (rowLocalY < rowH / 2.0) ? (rowCount - rv) : (rowCount - rv - 1);
            return true;
        }

        qreal edgeMargin = rowH * 0.28;
        if (rowLocalY < edgeMargin) {
            out.isRowInsert = true;
            out.boundaryB   = rowCount - rv;
            return true;
        }
        if (rowLocalY > rowH - edgeMargin) {
            out.isRowInsert = true;
            out.boundaryB   = rowCount - rv - 1;
            return true;
        }

        int maxCols = 1;
        for (const auto &r : rows) maxCols = qMax(maxCols, r.size());
        qreal colW = qreal(width()) / maxCols;
        const QVector<int> &cols = rows[dataRow];

        int col = qRound(pos.x() / colW);
        col = qBound(1, col, cols.size());

        out.isRowInsert = false;
        out.dataRow = dataRow;
        out.col     = col;
        return true;
    }

void LayerIndicatorZone::commitDrag()
{
        CanvasDocument &doc = glWidget->document();
        QVector<QVector<int>> rows = computeRows(doc, scope_);
        if (rows.isEmpty()) return;

        HoverTarget t;
        if (!computeHoverTarget(dragPos_, rows, t, dragRowMode_)) return;

        if (dragRowMode_) {
            // root列をドラッグした場合: その行(クリッピングレイヤーごと)を、上下の並べ替えとしてのみ移動する(列=クリップ関係は変えない)。
            int fromStart = -1, count = 1;
            for (const auto &r : rows) {
                if (r.contains(dragLayer_)) {
                    fromStart = r.first();
                    count = (doc.layers[r.first()].layerType == LayerType::Folder)
                        ? 1 + doc.layers[r.first()].childCount
                        : r.size();
                    break;
                }
            }
            if (fromStart < 0) return;

            int b = t.boundaryB; // rowOnly指定によりisRowInsertは常にtrue
            int flatInsertBefore = (b <= 0) ? (scope_ < 0 ? 0 : scope_ + 1) : afterLayerBlock(doc, rows[b - 1].last());
            int toIndex = (flatInsertBefore > fromStart) ? flatInsertBefore - count : flatInsertBefore;
            if (doc.moveLayerBlock(fromStart, count, toIndex))
                doc.setActiveLayer(toIndex);
            return;
        }

        // クリップ列としての挿入(列ドラッグ)は、フォルダーへは対応しない
        if (!t.isRowInsert && doc.layers[rows[t.dataRow][0]].layerType == LayerType::Folder)
            return;

        int fromIndex = dragLayer_;
        int flatInsertBefore;
        bool newClipping;

        if (t.isRowInsert) {
            int b = t.boundaryB;
            flatInsertBefore = (b <= 0) ? (scope_ < 0 ? 0 : scope_ + 1) : afterLayerBlock(doc, rows[b - 1].last());
            newClipping = false;
        } else {
            const QVector<int> &cols = rows[t.dataRow];
            int c = t.col;
            flatInsertBefore = (c < cols.size()) ? cols[c] : afterLayerBlock(doc, cols.last());
            newClipping = true;
        }

        int toIndex = (flatInsertBefore > fromIndex) ? flatInsertBefore - 1 : flatInsertBefore;
        doc.moveLayer(fromIndex, toIndex, newClipping);
        doc.setActiveLayer(toIndex);
    }
