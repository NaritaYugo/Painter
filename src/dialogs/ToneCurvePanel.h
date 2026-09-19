#pragma once
#include "dialogs/DraggablePanel.h"
#include <QVector>
#include <QPointF>

class QPushButton;
class ToneCurveEditor;

// ---------------------------------------------------------------------------
// ToneCurvePanel
// ---------------------------------------------------------------------------
// 処理>色調補正「トーンカーブ」アクション用のポップアップパネル。ColorBalancePanel
// 等と同様、GLWidgetの子として「キャンバス上に」浮かせて表示する非モーダルの
// QWidget。中央に正方形の曲線編集エリア(ToneCurveEditor)、その下に選択中の
// 節点の削除ボタン、確定/キャンセルボタンを並べる。
// ---------------------------------------------------------------------------
class ToneCurvePanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit ToneCurvePanel(QWidget *parent = nullptr);

    void resetValues(); // 恒等カーブに戻す
    void setPoints(const QVector<QPointF> &points); // シグナルを発火させずに現在の制御点を反映する(調整レイヤーの再編集用)

signals:
    void pointsChanged(const QVector<QPointF> &points);
    void confirmed();
    void cancelled();

protected:

private:
    ToneCurveEditor *editor_    = nullptr;
    QPushButton     *deleteBtn_ = nullptr;
};
