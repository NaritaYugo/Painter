#include "dialogs/ToneCurveEditor.h"
#include "tools/core/ToneCurveMath.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>

namespace {
constexpr double kMargin = 10.0;
constexpr double kHitRadius = 9.0;   // 節点をつまめる当たり判定(ウィジェット座標系px)
constexpr double kMinXGap = 4.0;     // 隣接節点との最小x間隔(曲線座標系0..255)
} // namespace

ToneCurveEditor::ToneCurveEditor(QWidget *parent)
    : QWidget(parent)
{
    points_ = { QPointF(0, 0), QPointF(255, 255) };
    setFixedSize(editorSize_, editorSize_);
    setMouseTracking(false);
}

void ToneCurveEditor::setEditorSize(int px)
{
    editorSize_ = std::max(80, px);
    setFixedSize(editorSize_, editorSize_);
    updateGeometry();
    update();
}

void ToneCurveEditor::reset()
{
    points_ = { QPointF(0, 0), QPointF(255, 255) };
    selectedIndex_ = -1;
    dragging_ = false;
    emit selectionChanged(false);
    emitChanged();
}

void ToneCurveEditor::setPoints(const QVector<QPointF> &points)
{
    points_ = points.size() >= 2 ? points : QVector<QPointF>{ QPointF(0, 0), QPointF(255, 255) };
    selectedIndex_ = -1;
    dragging_ = false;
    emit selectionChanged(false);
    update();
}

bool ToneCurveEditor::canRemoveSelected() const
{
    return selectedIndex_ > 0 && selectedIndex_ < points_.size() - 1;
}

void ToneCurveEditor::removeSelected()
{
    if (!canRemoveSelected()) return;
    const int removedIndex = selectedIndex_;
    points_.remove(removedIndex);

    // 削除ボタンの連打で連続して消せるように、まだ削除可能な点(端点以外)が
    // 残っていればその中から1つを選択状態にする。削除した点の位置に来た点
    // (無ければ直前の点)を選ぶ。
    const int n = points_.size();
    selectedIndex_ = (n > 2) ? std::clamp(removedIndex, 1, n - 2) : -1;

    emit selectionChanged(canRemoveSelected());
    emitChanged();
}

QPointF ToneCurveEditor::toCurve(const QPointF &widgetPos) const
{
    const double w = std::max(1.0, width()  - 2.0 * kMargin);
    const double h = std::max(1.0, height() - 2.0 * kMargin);
    double x = (widgetPos.x() - kMargin) / w * 255.0;
    double y = 255.0 - (widgetPos.y() - kMargin) / h * 255.0;
    return QPointF(std::clamp(x, 0.0, 255.0), std::clamp(y, 0.0, 255.0));
}

QPointF ToneCurveEditor::toWidget(const QPointF &curvePos) const
{
    const double w = width()  - 2.0 * kMargin;
    const double h = height() - 2.0 * kMargin;
    double x = kMargin + curvePos.x() / 255.0 * w;
    double y = kMargin + (255.0 - curvePos.y()) / 255.0 * h;
    return QPointF(x, y);
}

int ToneCurveEditor::hitTestPoint(const QPointF &widgetPos) const
{
    for (int i = 0; i < points_.size(); i++) {
        QPointF wp = toWidget(points_[i]);
        double dx = wp.x() - widgetPos.x(), dy = wp.y() - widgetPos.y();
        if (dx * dx + dy * dy <= kHitRadius * kHitRadius) return i;
    }
    return -1;
}

void ToneCurveEditor::emitChanged()
{
    emit pointsChanged(points_);
    update();
}

void ToneCurveEditor::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    const QPointF pos = event->position();

    int hit = hitTestPoint(pos);
    if (hit >= 0) {
        selectedIndex_ = hit;
        dragging_ = true;
        emit selectionChanged(canRemoveSelected());
        update();
        return;
    }

    // 節点が無い場所をクリックしたら、その位置に新しい節点を追加する
    // (隣接節点とx方向に近すぎる場合は接線が不安定になるので追加しない)。
    QPointF cp = toCurve(pos);
    int insertAt = 0;
    while (insertAt < points_.size() && points_[insertAt].x() < cp.x()) insertAt++;
    const bool tooCloseLeft  = insertAt > 0 && (cp.x() - points_[insertAt - 1].x()) < kMinXGap;
    const bool tooCloseRight = insertAt < points_.size() && (points_[insertAt].x() - cp.x()) < kMinXGap;
    if (tooCloseLeft || tooCloseRight) return;

    points_.insert(insertAt, cp);
    selectedIndex_ = insertAt;
    dragging_ = true;
    emit selectionChanged(canRemoveSelected());
    emitChanged();
}

void ToneCurveEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (!dragging_ || selectedIndex_ < 0) return;
    QPointF cp = toCurve(event->position());

    const bool isFirst = selectedIndex_ == 0;
    const bool isLast  = selectedIndex_ == points_.size() - 1;
    double x = points_[selectedIndex_].x();
    if (!isFirst && !isLast) {
        double minX = points_[selectedIndex_ - 1].x() + kMinXGap;
        double maxX = points_[selectedIndex_ + 1].x() - kMinXGap;
        if (minX <= maxX) x = std::clamp(cp.x(), minX, maxX);
    }
    // 先頭/末尾はx=0/255に固定し、yだけ動かせるようにする(黒点/白点の定義)。

    points_[selectedIndex_] = QPointF(x, cp.y());
    emitChanged();
}

void ToneCurveEditor::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    dragging_ = false;
}

void ToneCurveEditor::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QPainterPath bg;
    bg.addRoundedRect(rect(), 6, 6);
    painter.fillPath(bg, QColor(20, 20, 20, 220));

    // グリッド線(4分割)
    painter.setPen(QPen(QColor(255, 255, 255, 40), 1));
    for (int i = 1; i < 4; i++) {
        double t = i / 4.0;
        double vx = toWidget(QPointF(255.0 * t, 0)).x();
        double hy = toWidget(QPointF(0, 255.0 * t)).y();
        painter.drawLine(QPointF(vx, kMargin), QPointF(vx, height() - kMargin));
        painter.drawLine(QPointF(kMargin, hy), QPointF(width() - kMargin, hy));
    }

    // 枠線
    painter.setPen(QPen(QColor(255, 255, 255, 120), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(toWidget(QPointF(0, 0)), toWidget(QPointF(255, 255))));

    // 恒等カーブ(参考線)
    painter.setPen(QPen(QColor(255, 255, 255, 60), 1, Qt::DashLine));
    painter.drawLine(toWidget(QPointF(0, 0)), toWidget(QPointF(255, 255)));

    // 曲線本体
    const QVector<double> tangents = ToneCurveMath::computeTangents(points_);
    QPainterPath curve;
    curve.moveTo(toWidget(QPointF(0, ToneCurveMath::evalCurveY(points_, tangents, 0))));
    for (int x = 1; x <= 255; x++)
        curve.lineTo(toWidget(QPointF(x, ToneCurveMath::evalCurveY(points_, tangents, x))));
    painter.setPen(QPen(QColor(60, 180, 255), 2));
    painter.drawPath(curve);

    // 節点
    for (int i = 0; i < points_.size(); i++) {
        QPointF wp = toWidget(points_[i]);
        const bool selected = (i == selectedIndex_);
        painter.setPen(QPen(Qt::white, 1.5));
        painter.setBrush(selected ? QColor(255, 200, 60) : QColor(60, 180, 255));
        painter.drawEllipse(wp, selected ? 5.5 : 4.5, selected ? 5.5 : 4.5);
    }
}
