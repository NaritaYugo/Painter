#pragma once

#include <QTabBar>
#include <QPoint>
#include <QByteArray>

class QTabWidget;

// ---------------------------------------------------------------------------
// CanvasTabBar  ―  CanvasPane内のタブバー。ドラッグでの並べ替え・他ペインへの
// 移動・分割(ペイン端へドロップ)をすべて自前のQDragで処理する。
//
// QTabBar::setMovable(true)の内蔵ドラッグ並べ替えは使わない(呼び出し側で
// setMovable(false)にしておくこと)。内蔵の並べ替えと自前のクロスペインドラッグを
// 両方有効にすると、Qt内部のドラッグ状態機械と競合するため、ドラッグ経路を1本
// (このクラスのmousePress/Move + QDrag)に統一している。同じペイン内での並べ替えも、
// このバー自身がドロップ先として受けるQDragの1種類として扱う(結果、他ペインからの
// 移動と全く同じコードパスになる。見た目上、内蔵実装のような「他のタブが滑って
// 場所を空ける」アニメーションは無いが、並べ替え自体は機能する)。
//
// ドラッグ中に運ぶMIMEデータ(kMimeType)は、移動元のQTabWidgetポインタとタブindexを
// 生バイト列としてエンコードしたもの。同一プロセス内でしか使わない前提
// (ポインタをそのまま埋め込む)。
// ---------------------------------------------------------------------------
class CanvasTabBar : public QTabBar
{
    Q_OBJECT
public:
    explicit CanvasTabBar(QWidget *parent = nullptr);

    static const char *kMimeType;

    // このバーが乗っているQTabWidget(=ドラッグ開始元・ドロップ先の実体)。
    // CanvasPane側がsetTabBar()した直後に必ず設定すること。
    void setOwnerTabs(QTabWidget *tabs) { ownerTabs_ = tabs; }
    QTabWidget *ownerTabs() const { return ownerTabs_; }

    // MIMEデータへのエンコード/デコード(CanvasPaneのドロップ処理からも使う)。
    static QByteArray encode(QTabWidget *sourceTabs, int sourceIndex);
    static bool decode(const QByteArray &data, QTabWidget **outTabs, int *outIndex);

signals:
    // 他のペイン(または自分自身)のタブがこのバーへドロップされた
    // (destIndexは挿入位置)。同じペイン内での並べ替えも同じシグナルで通知する。
    void tabDropped(QTabWidget *sourceTabs, int sourceIndex, int destIndex);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QTabWidget *ownerTabs_ = nullptr;
    QPoint dragStartPos_;
    int    pressedIndex_ = -1;

    void startDragFromIndex(int index);
};
