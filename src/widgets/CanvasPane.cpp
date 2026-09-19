#include "widgets/CanvasPane.h"
#include "widgets/CanvasTabBar.h"
#include "widgets/NativeWindowLog.h" // 診断ログ(TIEPOLO_WINLOG)。ドラッグ開始の計測に使う
#include "components/ThemeColors.h"

#include <QVBoxLayout>
#include <QTabWidget>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QStyle>

namespace {

// QTabWidget::setTabBar()はprotectedなので、独自CanvasTabBarを差し込むための
// 薄いサブクラスが要る。振る舞いの追加は無い(構築時にタブバーを差し替えるだけ)。
class TabHost : public QTabWidget
{
public:
    explicit TabHost(QWidget *parent = nullptr) : QTabWidget(parent)
    {
        auto *bar = new CanvasTabBar(this);
        setTabBar(bar);
        bar->setOwnerTabs(this);
    }
};

} // namespace

CanvasPane::CanvasPane(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true); // QSSの枠線をプレーンQWidgetでも描かせる
    setAcceptDrops(true);

    auto *host = new TabHost(this);
    tabs_   = host;
    tabBar_ = static_cast<CanvasTabBar *>(host->tabBar());
    tabs_->setTabsClosable(true);
    // 内蔵のドラッグ並べ替えは使わない。CanvasTabBarの自前QDrag実装に一本化する
    // (CanvasTabBar.hのコメント参照)。
    tabs_->setMovable(false);
    tabs_->setTabPosition(QTabWidget::North);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tabs_);

    overlay_ = new QWidget(this);
    overlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
    overlay_->hide();

    connect(tabs_, &QTabWidget::currentChanged, this, [this](int idx) { emit currentTabChanged(this, idx); });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int idx) { emit tabCloseRequested(this, idx); });
    connect(tabBar_, &CanvasTabBar::tabDropped, this, [this](QTabWidget *srcTabs, int srcIdx, int destIdx) {
        emit tabInsertRequested(this, srcTabs, srcIdx, destIdx);
    });
}

void CanvasPane::setActive(bool active)
{
    if (active_ == active) return;
    active_ = active;
    setProperty("active", active);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

QRect CanvasPane::contentAreaRect() const
{
    // tabs_はレイアウト(余白0)でこのペイン全体を埋めているので、そのgeometry()は
    // そのままこのウィジェットの座標系での矩形になる。タブバーぶんだけ上を削る。
    QRect r = tabs_->geometry();
    if (tabBar_ && tabBar_->isVisible())
        r.setTop(r.top() + tabBar_->height());
    return r;
}

CanvasPane::DropZone CanvasPane::zoneAt(const QPoint &posInPaneCoords) const
{
    const QRect r = contentAreaRect();
    if (!r.contains(posInPaneCoords)) return DropZone::None;
    const QPoint p = posInPaneCoords - r.topLeft();
    const double xr = r.width()  > 0 ? double(p.x()) / r.width()  : 0.5;
    const double yr = r.height() > 0 ? double(p.y()) / r.height() : 0.5;
    if (xr < 0.25) return DropZone::Left;
    if (xr > 0.75) return DropZone::Right;
    if (yr < 0.25) return DropZone::Top;
    if (yr > 0.75) return DropZone::Bottom;
    return DropZone::Center;
}

void CanvasPane::updateOverlayRect(DropZone zone)
{
    if (zone == DropZone::None) {
        overlay_->hide();
        return;
    }

    QRect r = contentAreaRect();
    QRect hi = r;
    switch (zone) {
    case DropZone::Left:   hi.setWidth(r.width() / 2); break;
    case DropZone::Right:  hi.setLeft(r.left() + r.width() / 2); break;
    case DropZone::Top:    hi.setHeight(r.height() / 2); break;
    case DropZone::Bottom: hi.setTop(r.top() + r.height() / 2); break;
    case DropZone::Center: break; // 全体
    case DropZone::None:   break; // 到達しない(上でreturn済み)
    }

    // ドラッグ中しか使わないので、都度Theme::accentから組み立てる
    // (ライト/ダークテーマの切替にも自然に追従する)。
    const QColor &c = Theme::accent;
    overlay_->setStyleSheet(QString(
        "background-color: rgba(%1, %2, %3, 90); border: 2px solid rgba(%1, %2, %3, 200);")
        .arg(c.red()).arg(c.green()).arg(c.blue()));

    overlay_->setGeometry(hi);
    overlay_->show();
    overlay_->raise();
}

void CanvasPane::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(CanvasTabBar::kMimeType)) {
        // 計測用: QDrag::exec()へ入ってから最初のドラッグイベントが届くまでの間隔を
        // 見たい。ここが空いていれば、遅いのはアプリのコードではなくOS/Qt側の
        // ドラッグ開始処理ということになる。
        WINLOG(QStringLiteral("PERF CanvasPane::dragEnterEvent"));
        event->acceptProposedAction();
    }
}

void CanvasPane::dragMoveEvent(QDragMoveEvent *event)
{
    if (!event->mimeData()->hasFormat(CanvasTabBar::kMimeType)) return;
    event->acceptProposedAction();
    if (!loggedFirstDragMove_) {
        loggedFirstDragMove_ = true;
        WINLOG(QStringLiteral("PERF CanvasPane::dragMoveEvent (最初の1回)"));
    }
    updateOverlayRect(zoneAt(event->position().toPoint()));
}

void CanvasPane::dragLeaveEvent(QDragLeaveEvent *event)
{
    Q_UNUSED(event);
    overlay_->hide();
}

void CanvasPane::dropEvent(QDropEvent *event)
{
    overlay_->hide();
    if (!event->mimeData()->hasFormat(CanvasTabBar::kMimeType)) return;

    QTabWidget *sourceTabs = nullptr;
    int sourceIndex = -1;
    if (!CanvasTabBar::decode(event->mimeData()->data(CanvasTabBar::kMimeType), &sourceTabs, &sourceIndex))
        return;

    const DropZone zone = zoneAt(event->position().toPoint());
    event->acceptProposedAction();

    switch (zone) {
    case DropZone::Left:   emit splitRequested(this, Qt::Horizontal, true,  sourceTabs, sourceIndex); break;
    case DropZone::Right:  emit splitRequested(this, Qt::Horizontal, false, sourceTabs, sourceIndex); break;
    case DropZone::Top:    emit splitRequested(this, Qt::Vertical,   true,  sourceTabs, sourceIndex); break;
    case DropZone::Bottom: emit splitRequested(this, Qt::Vertical,   false, sourceTabs, sourceIndex); break;
    case DropZone::Center: emit tabInsertRequested(this, sourceTabs, sourceIndex, tabs_->count()); break;
    case DropZone::None:   break;
    }
}

void CanvasPane::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (overlay_ && overlay_->isVisible())
        overlay_->setGeometry(contentAreaRect());
}
