#pragma once

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QVector>

class QDockWidget;
class QMainWindow;
class QWidget;

// QMainWindow標準のセパレータをレイアウト上は0pxにしたまま、境界付近の
// マウスイベントを横取りしてドックのリサイズ操作を提供する。
// 入力用QWidgetをキャンバスへ重ねないため、QOpenGLWidgetの合成も妨げない。
class DockResizeController final : public QObject
{
public:
    explicit DockResizeController(QMainWindow &window);
    ~DockResizeController() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Boundary {
        QRect hitRect;
        Qt::Orientation orientation = Qt::Horizontal;
        QPointer<QDockWidget> firstDock;
        QPointer<QDockWidget> secondDock;
        int dragDirection = 1;
    };

    void scheduleRefresh();
    void refreshBoundaries();
    int boundaryAt(const QPoint &windowPosition) const;
    void updateHoverCursor(int boundaryIndex);
    void beginDrag(int boundaryIndex, QWidget *eventTarget, const QPoint &globalPosition);
    void updateDrag(const QPoint &globalPosition);
    void endDrag();
    static int dockExtent(const QDockWidget *dock, Qt::Orientation orientation);

    QMainWindow &window_;
    QSet<QDockWidget *> watchedDocks_;
    QVector<Boundary> boundaries_;
    QPointer<QWidget> dragEventTarget_;
    QPoint dragStartPosition_;
    int activeBoundary_ = -1;
    int firstExtent_ = 0;
    int secondExtent_ = 0;
    int lastAppliedDelta_ = 0;
    bool refreshScheduled_ = false;
    bool dragging_ = false;
    bool refreshAfterDrag_ = false;
    bool overrideCursorActive_ = false;
};
