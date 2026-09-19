#pragma once

#include <QWidget>

class QGridLayout;
class QButtonGroup;

// ===========================================================================
// BrushSizeDock  ―  ブラシサイズ選択パネル
//
//  ウィジェットのサイズから「何ボタン入るか」を計算し、
//  1〜1000px を等比数列（対数スケール）で N 分割したサイズ一覧を生成する。
//
//  リサイズのたびにボタン数・サイズが再計算されるので、
//  常に小サイズ〜大サイズまで均等にカバーされる。
// ===========================================================================
class BrushSizeDock : public QWidget
{
    Q_OBJECT
public:
    explicit BrushSizeDock(QWidget *parent = nullptr);
    static constexpr int BTN_WIDTH  = 40; // ボタンの正方形サイズ (px)
    static constexpr int BTN_HEIGHT  = 52; // ボタンの正方形サイズ (px)

    // 外部から選択を同期（シグナルは出ない）
    // px が現在のリストにない場合は最近傍を選ぶ
    void syncSize(int px);
    void stepUp();
    void stepDown();

signals:
    void brushSizeChanged(int px);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    // ---- レイアウト定数 ----
    static constexpr int GAP       =  2; // グリッドスペーシング (px)
    static constexpr int H_MARGIN  = 4; // 左右マージン合計 (px)
    static constexpr int MIN_SIZE  =  1; // ブラシ最小サイズ
    static constexpr int MAX_SIZE = 1000;

    QButtonGroup *m_group     = nullptr;
    QGridLayout  *m_grid      = nullptr;
    int           m_selectedPx = 5;  // 現在選択中のサイズ
    int           m_lastCount  = -1; // 前回のボタン数（変化検出用）

    // cols × rows から生成すべきサイズリストを返す（等比数列）
    static QList<int> generateSizes(int count);

    // ウィジェットサイズからボタン数・列数を計算
    static int calcCols(int w);
    static int calcRows(int h);

    // ボタンを全破棄して count 個で作り直す
    void rebuildButtons(int cols, int rows);

    // 現在のリストから px に最も近いサイズを返す
    int nearestSize(int px) const;
};