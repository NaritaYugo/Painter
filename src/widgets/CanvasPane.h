#pragma once

#include <QWidget>

class QTabWidget;
class CanvasTabBar;

// ---------------------------------------------------------------------------
// CanvasPane  ―  分割表示の1区画。内部にQTabWidget(独自CanvasTabBar差し込み済み)を
// 1つ持ち、中央のペインツリー(MainWindow::paneTreeRoot_。QSplitterで入れ子になり
// うる)の葉として使われる。
//
// タブのコンテンツ表示エリア(タブバー下)にタブがドラッグされると、カーソル位置を
// 5分割で判定する: 左右上下25%ずつ=そちら側へ分割(splitRequested)、中央50%=この
// ペインへタブとして挿入(tabInsertRequested。タブバーへ直接ドロップしたのと同じ扱い)。
// ドラッグ中は半透明のオーバーレイでハイライト矩形を表示する。
//
// setAcceptDrops(true)は本クラスとCanvasTabBarだけに付けてある。カーソルが実際の
// タブバー上にあるときは、Qtのドラッグイベント配送(acceptDrops=falseな子は
// 素通りして上位のacceptDrops=trueな祖先へ配送される)により自動的にCanvasTabBar側の
// 処理(同じペイン内の並べ替え/他ペインからのタブ挿入)が優先されるため、
// このクラス側で明示的に調停する必要はない。
// ---------------------------------------------------------------------------
class CanvasPane : public QWidget
{
    Q_OBJECT
public:
    explicit CanvasPane(QWidget *parent = nullptr);

    QTabWidget *tabWidget() const { return tabs_; }

    // アクティブペインの見た目(枠線)を切り替える。QSSセレクタ
    // (CanvasPane[active="true"])で描画するための動的プロパティ経由。
    void setActive(bool active);
    bool isActivePane() const { return active_; }

signals:
    void currentTabChanged(CanvasPane *pane, int index);
    void tabCloseRequested(CanvasPane *pane, int index);
    // ペイン端へドロップ=分割。orientationとinsertBeforeで新ペインをどちら側に
    // 置くかを表す(insertBefore=trueなら新ペインが左/上、falseなら右/下)。
    void splitRequested(CanvasPane *targetPane, Qt::Orientation orientation, bool insertBefore,
                         QTabWidget *sourceTabs, int sourceIndex);
    // ペイン中央またはタブバーへドロップ=分割せずこのペインへタブとして挿入。
    void tabInsertRequested(CanvasPane *targetPane, QTabWidget *sourceTabs, int sourceIndex, int destIndex);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    enum class DropZone { None, Left, Right, Top, Bottom, Center };

    QTabWidget   *tabs_    = nullptr;
    CanvasTabBar *tabBar_  = nullptr;
    QWidget      *overlay_ = nullptr; // ドラッグ中のハイライト表示専用(入力は透過)
    bool loggedFirstDragMove_ = false; // 診断ログ用(最初のdragMoveEventだけ記録する)
    bool          active_  = false;

    DropZone zoneAt(const QPoint &posInPaneCoords) const;
    QRect contentAreaRect() const; // タブバーを除いた表示エリア
    void updateOverlayRect(DropZone zone);
};
