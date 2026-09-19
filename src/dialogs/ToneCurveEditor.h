#pragma once
#include <QWidget>
#include <QVector>
#include <QPointF>

// ---------------------------------------------------------------------------
// ToneCurveEditor
// ---------------------------------------------------------------------------
// トーンカーブパネル内の正方形の曲線編集エリア。横軸=入力(0..255,左から右)、
// 縦軸=出力(0..255,下から上)。曲線上のクリックで節点を追加、既存の節点を
// ドラッグして移動、選択中の節点はremoveSelected()で削除できる(端点は削除不可)。
// 曲線自体はCatmull-Romスプラインで補間して描画する(ToneCurveTool::rebuildLutと
// 同じ補間方式)。
// ---------------------------------------------------------------------------
class ToneCurveEditor : public QWidget
{
    Q_OBJECT
public:
    explicit ToneCurveEditor(QWidget *parent = nullptr);

    void reset(); // 恒等カーブ(2点: (0,0),(255,255))に戻す
    void setPoints(const QVector<QPointF> &points); // シグナルを発火させずに現在の制御点を反映する(調整レイヤーの再編集用)

    bool canRemoveSelected() const; // 端点以外が選択されていればtrue
    void removeSelected();          // 選択中の節点を削除する(端点なら何もしない)

    // 正方形の一辺(px)。既定は240。狭いドック(ツール設定の筆圧カーブ)へ
    // 埋め込むときに小さくするためのもの。
    void setEditorSize(int px);

    QSize sizeHint() const override { return QSize(editorSize_, editorSize_); }

signals:
    void pointsChanged(const QVector<QPointF> &points); // x昇順の制御点(x,yともに0..255)
    void selectionChanged(bool canRemove);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QVector<QPointF> points_; // x昇順、常に先頭x=0/末尾x=255
    int selectedIndex_ = -1;
    bool dragging_ = false;
    int editorSize_ = 240;

    QPointF toCurve(const QPointF &widgetPos) const;  // ウィジェット座標 -> 曲線座標(0..255)
    QPointF toWidget(const QPointF &curvePos) const;  // 曲線座標 -> ウィジェット座標
    int     hitTestPoint(const QPointF &widgetPos) const; // ヒットした節点のindex(無ければ-1)
    void    emitChanged();
};
