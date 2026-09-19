#pragma once
#include <QWidget>
#include <QVector>
#include <QColor>

// ---------------------------------------------------------------------------
// GradientStripEditor
// ---------------------------------------------------------------------------
// グラデーションマップパネル内の、横長の「グラデーションの帯」編集エリア。
// 帯そのものが1次元のプレビューになっていて、その下に各色の位置を示すつまみ
// (ストップ)が並ぶ。ToneCurveEditor の考え方をそのまま1次元にしたもの。
//
//   ・帯またはつまみの無い場所をクリック  … その位置に新しいストップを追加する
//                                            (色はその時点のグラデーション色を引き継ぐ)
//   ・つまみをクリック                    … 選択
//   ・つまみをドラッグ                    … 位置を変更(0..1でクランプ)
//   ・つまみをダブルクリック              … 色を変更(ColorWheelWidgetのポップアップ)
//   ・removeSelected()                    … 選択中のストップを削除(最低2個は残す)
//
// 色の選択は、アプリ内で色選択UIを統一するため QColorDialog ではなく
// ColorWheelWidget のポップアップを使う(TextLayerPanel/SolidColorPickerPanelと同じ)。
// ---------------------------------------------------------------------------
class GradientStripEditor : public QWidget
{
    Q_OBJECT
public:
    struct Stop {
        float  pos;   // 0..1
        QColor color;
    };

    explicit GradientStripEditor(QWidget *parent = nullptr);

    void reset(); // 既定(黒→白)に戻す
    void setStops(const QVector<Stop> &stops); // シグナルを発火させずに現在のストップ列を反映する(調整レイヤーの再編集用)

    const QVector<Stop> &stops() const { return stops_; }

    bool hasSelection() const { return selectedIndex_ >= 0 && selectedIndex_ < stops_.size(); }
    bool canRemoveSelected() const; // 選択中があり、かつ3個以上あればtrue
    void removeSelected();
    // 選択中のストップの色を変更するポップアップを出す(パネルのボタンからも呼べる)
    void openColorPickerForSelected();

    QSize sizeHint() const override { return QSize(280, 74); }

signals:
    void stopsChanged(const QVector<GradientStripEditor::Stop> &stops); // pos昇順
    void selectionChanged(bool canRemove);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QVector<Stop> stops_; // pos昇順を常に保つ
    int  selectedIndex_ = -1;
    bool dragging_ = false;

    QRect  bandRect() const;                 // グラデーションの帯の矩形
    int    handleY() const;                  // つまみの中心Y
    float  toPos(int widgetX) const;         // ウィジェットX -> 0..1
    int    toX(float pos) const;             // 0..1 -> ウィジェットX
    int    hitTestHandle(const QPoint &p) const; // ヒットしたつまみのindex(無ければ-1)
    QColor sampleAt(float pos) const;        // 現在のグラデーションのpos位置の色
    void   sortAndKeepSelection();           // pos昇順へ並べ直し、selectedIndex_を追従させる
    void   emitChanged();
};
