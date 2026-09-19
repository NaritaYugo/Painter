#include "docks/ToolDock.h"
#include "tools/core/ToolRegistry.h"
#include "shortcuts/ShortcutRegistry.h"

#include <QVBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QButtonGroup>
#include <QLabel>
#include <QIcon>
#include <QResizeEvent>
#include <QKeySequence>

// ツールボタン1つを生成するヘルパー
static QPushButton *makeToolBtn(const QIcon &icon, const QString &tooltip)
{
    auto *btn = new QPushButton();
    btn->setToolTip(tooltip);
    btn->setIcon(icon);
    btn->setCheckable(true);
    btn->setFixedSize(ToolDock::BTN_SIZE, ToolDock::BTN_SIZE);
    btn->setStyleSheet(R"(
        QPushButton {
            background-color: transparent;
            border: none;
            border-radius: 15px;
            padding: 6px, 6px;
        }
        QPushButton:hover {
            background-color: #666666;
        }
        QPushButton:pressed {
            background-color: #5695dd;
        }
        QPushButton:checked {
            background-color: #5695dd;
        }
    )");
    return btn;
}

// ボタン幅・マージンから適切な列数を計算する
// 最低1列、幅が広がるごとに列数を増やす
static int calcCols(int widgetWidth)
{
    int cols = widgetWidth / ToolDock::BTN_SIZE;
    return qMax(1, cols);
}

// グリッドから全ウィジェットを取り外す（削除はしない）
static void clearGrid(QGridLayout *grid)
{
    while (grid->count() > 0) {
        QLayoutItem *item = grid->takeAt(0);
        // ウィジェット自体は親ウィジェットが所有しているので delete しない
        delete item; // QLayoutItem だけ解放
    }
}

// ===========================================================================
ToolDock::ToolDock(GLWidget *gl, const ShortcutRegistry &shortcuts, QWidget *parent)
    : QWidget(parent), glWidget(gl), shortcuts_(shortcuts)
{
    // ボタン1個分 + 左右マージンを最小幅に設定
    setMinimumWidth(BTN_SIZE);
    setBaseSize(BTN_SIZE, 500);

    QVBoxLayout *vLayout = new QVBoxLayout(this);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(0);

    buttonGroup = new QButtonGroup(this);
    buttonGroup->setExclusive(true);

    grid = new QGridLayout();
    grid->setSpacing(0);
    vLayout->addLayout(grid);

    for (const ToolTypeMeta &d : toolTypeRegistry()) {
        auto *btn = makeToolBtn(QIcon(d.iconPath), d.label);
        buttonGroup->addButton(btn, (int)d.type);
        btns.append({ btn, (int)d.type, d.type, d.label });
    }

    vLayout->addStretch();

    refreshTooltips();


    // 初期配置（列数 = 1 で仮置き。resizeEvent で正しく組み直される）
    reflowGrids(1);

    // 初期選択: Pen
    buttonGroup->button((int)ToolType::Pen)->setChecked(true);

    // シグナル接続
    connect(buttonGroup, &QButtonGroup::idClicked, this, [this](int id) {
        ToolType tool     = static_cast<ToolType>(id);
        glWidget->setActiveTool(tool);
        emit toolChanged(tool);
    });
}

// ---------------------------------------------------------------------------
void ToolDock::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    const int cols = calcCols(event->size().width());
    if (cols == lastCols) return; // 列数が変わらなければ何もしない
    reflowGrids(cols);
}

// ---------------------------------------------------------------------------
void ToolDock::reflowGrids(int cols)
{
    lastCols = cols;

    clearGrid(grid);
    for (int i = 0; i < btns.size(); ++i)
        grid->addWidget(btns[i].btn, i / cols, i % cols);
        
    for (int c = 0; c < cols; ++c)
        grid->setColumnStretch(c, 0);

    grid->setColumnStretch(cols, 1);
}

// ---------------------------------------------------------------------------
void ToolDock::refreshTooltips()
{
    for (const BtnDef &d : btns) {
        const int key = shortcuts_.keyForTool(d.tool);
        const QString keyName = key != 0
            ? QKeySequence(key).toString(QKeySequence::NativeText)
            : QString();
        d.btn->setToolTip(keyName.isEmpty() ? d.baseLabel
                                             : QString("%1(%2)").arg(d.baseLabel, keyName));
    }
}

// ---------------------------------------------------------------------------
void ToolDock::syncButton(ToolType tool)
{
    // シグナルを出さずにボタンだけ切り替える
    buttonGroup->blockSignals(true);
    auto *btn = buttonGroup->button((int)tool);
    if (btn) btn->setChecked(true);
    buttonGroup->blockSignals(false);
}