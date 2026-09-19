#pragma once

#include <QWidget>
#include <QAbstractButton>
#include "widgets/GLWidget.h"

class QButtonGroup;
class QGridLayout;
class ShortcutRegistry;

// ===========================================================================
// ToolDock  ―  ツール選択パネル
//   ドック幅に応じてボタンの列数を自動調整する
// ===========================================================================
class ToolDock : public QWidget
{
    Q_OBJECT
public:
    explicit ToolDock(GLWidget *gl, const ShortcutRegistry &shortcuts, QWidget *parent = nullptr);
    void syncButton(ToolType tool);

    // ショートカットキー設定が変更された際に、ツールチップの表示(例:「ペン(Q)」)を
    // 最新のキー割り当てに合わせて更新する。
    void refreshTooltips();

    // タブ切替時に、表示対象のGLWidget(=キャンバス)を差し替える
    void setGLWidget(GLWidget *gl) { glWidget = gl; syncButton(glWidget->getActiveTool()); }
    
    static constexpr int BTN_SIZE    = 30; // ボタン1辺のピクセル数
    static constexpr int BTN_SPACING = 4;  // グリッドスペーシング
    static constexpr int H_MARGIN    = 16; // 左右マージン合計 (8*2)

signals:
    void toolChanged(ToolType tool);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    // グリッドを cols 列で組み直す
    void reflowGrids(int cols);

    GLWidget                *glWidget  = nullptr;
    const ShortcutRegistry  &shortcuts_;
    QButtonGroup            *buttonGroup = nullptr;

    QGridLayout  *grid   = nullptr;

    // ボタン定義をキャッシュして再レイアウト時に使い回す
    // baseLabel はショートカット表記を付ける前の素のツール名(例:「ペン」)
    struct BtnDef { QAbstractButton *btn; int groupIndex; ToolType tool; QString baseLabel; };
    QList<BtnDef> btns;

    int lastCols = -1; // 前回の列数（不要な再レイアウトを防ぐ）
};