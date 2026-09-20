#pragma once

#include <QMouseEvent>
#include <QPointF>
#include <QVector>
#include <QWidget>

class CanvasWidget;

// レイヤーとクリッピング構造全体の俯瞰表示兼ドラッグ操作領域。
class LayerIndicatorZone : public QWidget
{
    Q_OBJECT
public:
    explicit LayerIndicatorZone(CanvasWidget *gl, QWidget *parent = nullptr);

    // タブ切替時に、表示対象のCanvasWidget(=キャンバス)を差し替える。
    void setCanvasWidget(CanvasWidget *gl);

    // 現在表示すべき階層(-1=ルート、それ以外はフォルダーのレイヤーindex)。
    void setScope(int scope);

signals:
    void layerClicked(int layerIndex);
    // 星形(フォルダー)のドットをダブルクリックしたときに発火する。
    void folderDoubleClicked(int layerIndex);

protected:
    void paintEvent(QPaintEvent *) override;

    void mousePressEvent(QMouseEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;

    void mouseReleaseEvent(QMouseEvent *event) override;

    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    struct DotInfo { QPointF center; int layerIndex; };

    // ドラッグ中カーソルが指している挿入先。
    struct HoverTarget {
        bool isRowInsert = true;
        int  boundaryB   = 0;
        int  dataRow     = 0;
        int  col         = 1;
    };

    // rowOnly==true(root列=行ドラッグ中)のときは、常に行の挿入位置(上下の並べ替え)だけを返し、クリップ列への挿入は一切候補にしない。
    bool computeHoverTarget(const QPointF &pos, const QVector<QVector<int>> &rows, HoverTarget &out,
                            bool rowOnly = false) const;

    void commitDrag();

    CanvasWidget *glWidget;
    QVector<DotInfo> dots_;
    int scope_ = -1;

    int    pressLayer_ = -1;
    QPointF pressPos_;

    bool    dragging_    = false;
    int     dragLayer_   = -1;
    bool    dragRowMode_ = false;
    QPointF dragPos_;
};
