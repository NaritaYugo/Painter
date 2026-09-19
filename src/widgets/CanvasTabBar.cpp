#include "widgets/CanvasTabBar.h"
#include "widgets/NativeWindowLog.h" // 診断ログ(TIEPOLO_WINLOG)。ドラッグ開始の計測に使う
#include "components/ThemeColors.h"  // ドラッグプレビューを自前で描くための配色

#include <QApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QPainter>
#include <QPixmap>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QTabWidget>
#include <QWidget>

const char *CanvasTabBar::kMimeType = "application/x-tiepolo-pane-tab";

CanvasTabBar::CanvasTabBar(QWidget *parent) : QTabBar(parent)
{
    setAcceptDrops(true);
}

QByteArray CanvasTabBar::encode(QTabWidget *sourceTabs, int sourceIndex)
{
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out << (quintptr)sourceTabs << (qint32)sourceIndex;
    return data;
}

bool CanvasTabBar::decode(const QByteArray &data, QTabWidget **outTabs, int *outIndex)
{
    QDataStream in(data);
    quintptr tabsPtr = 0;
    qint32 index = -1;
    in >> tabsPtr >> index;
    if (in.status() != QDataStream::Ok || tabsPtr == 0) return false;
    *outTabs = reinterpret_cast<QTabWidget *>(tabsPtr);
    *outIndex = index;
    return true;
}

void CanvasTabBar::mousePressEvent(QMouseEvent *event)
{
    QTabBar::mousePressEvent(event);
    if (event->button() == Qt::LeftButton) {
        dragStartPos_ = event->pos();
        pressedIndex_ = tabAt(event->pos());
    }
}

void CanvasTabBar::mouseMoveEvent(QMouseEvent *event)
{
    if (pressedIndex_ >= 0 && (event->buttons() & Qt::LeftButton)
        && (event->pos() - dragStartPos_).manhattanLength() >= QApplication::startDragDistance()) {
        const int idx = pressedIndex_;
        pressedIndex_ = -1; // QDrag::exec()がブロックしている間の再入を防ぐ
        startDragFromIndex(idx);
        return;
    }
    QTabBar::mouseMoveEvent(event);
}

void CanvasTabBar::startDragFromIndex(int index)
{
    if (!ownerTabs_ || index < 0 || index >= ownerTabs_->count()) return;

    // 計測用: 「つまんでから実際にタブがマウスに追従し始めるまで」が起動後1回目だけ
    // 極端に遅いという報告があるため、この関数の各段を分けて出す
    // (TIEPOLO_WINLOG=1 のときだけ)。
    QElapsedTimer dragTimer;
    dragTimer.start();

    auto *mime = new QMimeData();
    mime->setData(kMimeType, encode(ownerTabs_, index));

    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    const qint64 tMime = dragTimer.elapsed();

    // ドラッグ中のプレビューにはタブ自体の見た目を使う(ラスタ描画なので即座に取れる)。
    //
    // 以前はページ(GLWidget)をgrab()して実キャンバスのサムネイルを出していたが、
    // QOpenGLWidget::grab()はmakeCurrent+全再合成+glReadPixels+QImage変換を伴う同期処理で、
    // 特に初回は数百msブロックしうる。その間にユーザーがボタンを離してしまうと、
    // DoDragDrop開始時点で「ボタンが押されていない」状態になる。Windowsのドラッグ元
    // (QWindowsOleDropSource::QueryContinueDrag)は最初のコールバックでボタン状態を
    // 記憶する作りのため、そこでNoButtonだと押下待ちのまま素のカーソルに追従し続け、
    // 次のクリックで初めて記憶が入り、その離上でドロップ判定になる
    // (=「離すとくっついてきて、もう一度クリックすると離せる」挙動)。
    // GL読み戻しが温まる2回目以降は速いので初回だけ症状が出ていた。
    // ドラッグ中のプレビュー画像は、ウィジェットを描画させずに自前で組み立てる。
    //
    // ここで grab()/render() を使ってはいけない。ウィジェット描画はトップレベルの
    // バッキングストア経由になり、同じウィンドウ内にまだ初期化されていない
    // QOpenGLWidget(=キャンバス)があると、その initializeGL() を同期的に走らせて
    // しまう。初期化はシェーダーを約40本コンパイルするため数秒かかる(実測2875ms)。
    //
    // その数秒の間にユーザーはとっくにマウスを動かしており、QDrag::exec()へ入る頃には
    // ボタンの押下状態が失われている。Windowsのドラッグ元
    // (QWindowsOleDropSource::QueryContinueDrag)は最初のコールバックでボタン状態を
    // 記憶する作りなので、そこでNoButtonだとドラッグが始まらず、次のクリックまで
    // タブがマウスに追従しない ―― これが「起動後1回目だけタブを掴めるまで長い」の正体。
    // (以前ページ側のgrab()を外した時と同じ罠で、タブバー側に残っていた)
    const QRect r = tabRect(index);
    if (!r.isEmpty()) {
        const qreal dpr = devicePixelRatioF();
        QPixmap pix(r.size() * dpr);
        pix.setDevicePixelRatio(dpr);
        pix.fill(Qt::transparent);
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRect local(QPoint(0, 0), r.size());
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::bgPanel);
        p.drawRoundedRect(local.adjusted(0, 0, -1, -1), 4, 4);
        p.setPen(Theme::text);
        p.setFont(font());
        p.drawText(local.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                    fontMetrics().elidedText(tabText(index), Qt::ElideRight, local.width() - 16));
        p.end();

        drag->setPixmap(pix);
        drag->setHotSpot(dragStartPos_ - r.topLeft()); // つまんだ位置をそのまま保つ
    }
    const qint64 tGrab = dragTimer.elapsed();

    WINLOG(QStringLiteral("PERF startDrag: mime=%1ms grab=%2ms -> QDrag::exec() へ入る")
               .arg(tMime).arg(tGrab - tMime));
    drag->exec(Qt::MoveAction);
    WINLOG(QStringLiteral("PERF startDrag: QDrag::exec() から戻った (合計 %1ms)").arg(dragTimer.elapsed()));
}

void CanvasTabBar::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(kMimeType))
        event->acceptProposedAction();
}

void CanvasTabBar::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasFormat(kMimeType))
        event->acceptProposedAction();
}

void CanvasTabBar::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasFormat(kMimeType)) return;

    QTabWidget *sourceTabs = nullptr;
    int sourceIndex = -1;
    if (!decode(event->mimeData()->data(kMimeType), &sourceTabs, &sourceIndex)) return;

    int destIndex = tabAt(event->position().toPoint());
    if (destIndex < 0) destIndex = count();

    event->acceptProposedAction();
    emit tabDropped(sourceTabs, sourceIndex, destIndex);
}
