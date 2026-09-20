#include "components/DockResizeController.h"

#include <QApplication>
#include <QDockWidget>
#include <QEvent>
#include <QMainWindow>
#include <QMouseEvent>
#include <QTimer>
#include <QWidget>

#include <algorithm>

namespace {

constexpr int kHitThickness = 8;
constexpr int kBoundaryTolerance = 12;
constexpr int kMinimumDockExtent = 32;

int overlapLength(int firstStart, int firstEnd, int secondStart, int secondEnd)
{
    return std::min(firstEnd, secondEnd) - std::max(firstStart, secondStart) + 1;
}

QRect verticalHitRect(int x, int top, int bottom)
{
    return {x - kHitThickness / 2, top, kHitThickness, bottom - top + 1};
}

QRect horizontalHitRect(int y, int left, int right)
{
    return {left, y - kHitThickness / 2, right - left + 1, kHitThickness};
}

} // namespace

DockResizeController::DockResizeController(QMainWindow &window)
    : QObject(&window), window_(window)
{
    // 子ウィジェットへ届くマウスイベントも境界判定する必要があるため、アプリ全体の
    // フィルタを使う。window()が対象MainWindowと一致するイベントだけを扱う。
    qApp->installEventFilter(this);
    scheduleRefresh();
}

DockResizeController::~DockResizeController()
{
    if (overrideCursorActive_)
        QApplication::restoreOverrideCursor();
}

bool DockResizeController::eventFilter(QObject *watched, QEvent *event)
{
    if (auto *target = qobject_cast<QWidget *>(watched); target && target->window() == &window_) {
        auto *mouse = dynamic_cast<QMouseEvent *>(event);
        if (mouse) {
            const QPoint globalPosition = mouse->globalPosition().toPoint();
            const QPoint windowPosition = window_.mapFromGlobal(globalPosition);

            if (event->type() == QEvent::MouseButtonPress
                && mouse->button() == Qt::LeftButton && !dragging_) {
                const int boundaryIndex = boundaryAt(windowPosition);
                if (boundaryIndex >= 0) {
                    beginDrag(boundaryIndex, target, globalPosition);
                    mouse->accept();
                    return true;
                }
            } else if (event->type() == QEvent::MouseMove) {
                if (dragging_) {
                    updateDrag(globalPosition);
                    mouse->accept();
                    return true;
                }
                updateHoverCursor(boundaryAt(windowPosition));
            } else if (event->type() == QEvent::MouseButtonRelease
                       && mouse->button() == Qt::LeftButton && dragging_) {
                endDrag();
                mouse->accept();
                return true;
            }
        }
        if (watched == &window_ && event->type() == QEvent::Leave)
            updateHoverCursor(-1);
    }

    switch (event->type()) {
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::LayoutRequest:
    case QEvent::ParentChange:
        if (watched == &window_ || qobject_cast<QDockWidget *>(watched))
            scheduleRefresh();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void DockResizeController::scheduleRefresh()
{
    if (dragging_) {
        refreshAfterDrag_ = true;
        return;
    }
    if (refreshScheduled_)
        return;

    refreshScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        refreshScheduled_ = false;
        if (dragging_) {
            refreshAfterDrag_ = true;
            return;
        }
        refreshBoundaries();
    });
}

void DockResizeController::refreshBoundaries()
{
    boundaries_.clear();

    QList<QDockWidget *> docks;
    const auto allDocks = window_.findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QDockWidget *dock : allDocks) {
        if (!watchedDocks_.contains(dock)) {
            watchedDocks_.insert(dock);
            connect(dock, &QObject::destroyed, this, [this, dock] {
                watchedDocks_.remove(dock);
                scheduleRefresh();
            });
        }
        if (!dock->contentsMargins().isNull())
            dock->setContentsMargins(0, 0, 0, 0);
        if (dock->isVisible() && !dock->isFloating() && !dock->geometry().isEmpty())
            docks.append(dock);
    }

    auto addBoundary = [this](const QRect &hitRect, Qt::Orientation orientation,
                              QDockWidget *first, QDockWidget *second, int dragDirection) {
        const QRect clipped = hitRect.intersected(window_.rect());
        if (!clipped.isEmpty())
            boundaries_.append({clipped, orientation, first, second, dragDirection});
    };

    // 各ドック領域と中央領域の境界。
    for (QDockWidget *dock : docks) {
        const QRect rect = dock->geometry();
        switch (window_.dockWidgetArea(dock)) {
        case Qt::LeftDockWidgetArea:
            addBoundary(verticalHitRect(rect.right() + 1, rect.top(), rect.bottom()),
                        Qt::Horizontal, dock, nullptr, +1);
            break;
        case Qt::RightDockWidgetArea:
            addBoundary(verticalHitRect(rect.left(), rect.top(), rect.bottom()),
                        Qt::Horizontal, dock, nullptr, -1);
            break;
        case Qt::TopDockWidgetArea:
            addBoundary(horizontalHitRect(rect.bottom() + 1, rect.left(), rect.right()),
                        Qt::Vertical, dock, nullptr, +1);
            break;
        case Qt::BottomDockWidgetArea:
            addBoundary(horizontalHitRect(rect.top(), rect.left(), rect.right()),
                        Qt::Vertical, dock, nullptr, -1);
            break;
        default:
            break;
        }
    }

    // ネストした同一領域内のドック同士の境界。後から追加して優先的にヒットさせる。
    for (qsizetype i = 0; i < docks.size(); ++i) {
        for (qsizetype j = i + 1; j < docks.size(); ++j) {
            QDockWidget *first = docks.at(i);
            QDockWidget *second = docks.at(j);
            if (window_.dockWidgetArea(first) != window_.dockWidgetArea(second))
                continue;

            QRect a = first->geometry();
            QRect b = second->geometry();
            if (a.left() > b.left()) {
                std::swap(a, b);
                std::swap(first, second);
            }
            const int horizontalGap = b.left() - (a.right() + 1);
            if (qAbs(horizontalGap) <= kBoundaryTolerance) {
                const int top = std::max(a.top(), b.top());
                const int bottom = std::min(a.bottom(), b.bottom());
                if (overlapLength(a.top(), a.bottom(), b.top(), b.bottom()) > kHitThickness)
                    addBoundary(verticalHitRect(a.right() + 1 + horizontalGap / 2, top, bottom),
                                Qt::Horizontal, first, second, +1);
            }

            first = docks.at(i);
            second = docks.at(j);
            a = first->geometry();
            b = second->geometry();
            if (a.top() > b.top()) {
                std::swap(a, b);
                std::swap(first, second);
            }
            const int verticalGap = b.top() - (a.bottom() + 1);
            if (qAbs(verticalGap) <= kBoundaryTolerance) {
                const int left = std::max(a.left(), b.left());
                const int right = std::min(a.right(), b.right());
                if (overlapLength(a.left(), a.right(), b.left(), b.right()) > kHitThickness)
                    addBoundary(horizontalHitRect(a.bottom() + 1 + verticalGap / 2, left, right),
                                Qt::Vertical, first, second, +1);
            }
        }
    }
}

int DockResizeController::boundaryAt(const QPoint &windowPosition) const
{
    // 同じ位置に中央境界とドック間境界が重なった場合、後から追加したドック間を優先。
    for (qsizetype i = boundaries_.size(); i > 0; --i) {
        if (boundaries_.at(i - 1).hitRect.contains(windowPosition))
            return int(i - 1);
    }
    return -1;
}

void DockResizeController::updateHoverCursor(int boundaryIndex)
{
    if (boundaryIndex >= 0) {
        const Qt::CursorShape shape = boundaries_.at(boundaryIndex).orientation == Qt::Horizontal
            ? Qt::SplitHCursor : Qt::SplitVCursor;
        if (!overrideCursorActive_) {
            QApplication::setOverrideCursor(shape);
            overrideCursorActive_ = true;
        } else {
            QApplication::changeOverrideCursor(shape);
        }
    } else if (overrideCursorActive_ && !dragging_) {
        QApplication::restoreOverrideCursor();
        overrideCursorActive_ = false;
    }
}

void DockResizeController::beginDrag(int boundaryIndex, QWidget *eventTarget,
                                     const QPoint &globalPosition)
{
    if (boundaryIndex < 0 || boundaryIndex >= boundaries_.size())
        return;
    const Boundary &boundary = boundaries_.at(boundaryIndex);
    if (!boundary.firstDock)
        return;

    dragging_ = true;
    activeBoundary_ = boundaryIndex;
    dragStartPosition_ = globalPosition;
    firstExtent_ = dockExtent(boundary.firstDock, boundary.orientation);
    secondExtent_ = dockExtent(boundary.secondDock, boundary.orientation);
    lastAppliedDelta_ = 0;
    dragEventTarget_ = eventTarget;
    if (dragEventTarget_)
        dragEventTarget_->grabMouse();
    updateHoverCursor(boundaryIndex);
}

void DockResizeController::updateDrag(const QPoint &globalPosition)
{
    if (!dragging_ || activeBoundary_ < 0 || activeBoundary_ >= boundaries_.size())
        return;
    const Boundary &boundary = boundaries_.at(activeBoundary_);
    if (!boundary.firstDock) {
        endDrag();
        return;
    }

    const QPoint movement = globalPosition - dragStartPosition_;
    const int delta = (boundary.orientation == Qt::Horizontal ? movement.x() : movement.y())
                      * boundary.dragDirection;
    if (delta == lastAppliedDelta_)
        return;
    lastAppliedDelta_ = delta;

    QList<QDockWidget *> docks{boundary.firstDock.data()};
    QList<int> extents{std::max(kMinimumDockExtent, firstExtent_ + delta)};
    if (boundary.secondDock) {
        docks.append(boundary.secondDock.data());
        extents.append(std::max(kMinimumDockExtent, secondExtent_ - delta));
    }
    window_.resizeDocks(docks, extents, boundary.orientation);
}

void DockResizeController::endDrag()
{
    if (dragEventTarget_)
        dragEventTarget_->releaseMouse();
    dragEventTarget_.clear();
    dragging_ = false;
    activeBoundary_ = -1;

    if (overrideCursorActive_) {
        QApplication::restoreOverrideCursor();
        overrideCursorActive_ = false;
    }
    refreshAfterDrag_ = false;
    scheduleRefresh();
}

int DockResizeController::dockExtent(const QDockWidget *dock, Qt::Orientation orientation)
{
    if (!dock)
        return 0;
    return orientation == Qt::Horizontal ? dock->width() : dock->height();
}
