#pragma once
#include "dialogs/DraggablePanel.h"
#include "dialogs/GradientStripEditor.h"
#include <QVector>

class QPushButton;

// ---------------------------------------------------------------------------
// GradientMapPanel (Pro限定)
// ---------------------------------------------------------------------------
// 処理>色調補正の「グラデーションマップ」アクション用のポップアップパネル。
// ToneCurvePanel と同様、編集UIの本体(GradientStripEditor)を入れ子にして持ち、
// その下に補助ボタン(色を変更 / 削除 / リセット)と確定・キャンセルを並べる。
// ---------------------------------------------------------------------------
class GradientMapPanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit GradientMapPanel(QWidget *parent = nullptr);

    void resetGradient(); // 既定(黒→白)に戻す
    void setStops(const QVector<GradientStripEditor::Stop> &stops); // シグナルを発火させずに現在のストップ列を反映する(調整レイヤーの再編集用)

signals:
    void stopsChanged(const QVector<GradientStripEditor::Stop> &stops);
    void confirmed();
    void cancelled();

private:
    GradientStripEditor *editor_ = nullptr;
    QPushButton *colorBtn_  = nullptr;
    QPushButton *removeBtn_ = nullptr;
};
