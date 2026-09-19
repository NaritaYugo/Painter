#include "dialogs/CanvasSizePanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QButtonGroup>

namespace {
// 行優先: 0=左上 1=上中央 2=右上 3=左 4=中央 5=右 6=左下 7=下中央 8=右下
const char *kAnchorTips[9] = {
    "左上", "上中央", "右上",
    "左",   "中央",   "右",
    "左下", "下中央", "右下",
};
}

CanvasSizePanel::CanvasSizePanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("キャンバスサイズ変更"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    // ---- アンカー選択(3x3) ----
    auto *anchorRow = new QHBoxLayout();
    auto *anchorGrid = new QGridLayout();
    anchorGrid->setSpacing(4);
    anchorGroup_ = new QButtonGroup(this);
    anchorGroup_->setExclusive(true);
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int id = row * 3 + col;
            auto *btn = new QPushButton(this);
            btn->setCheckable(true);
            btn->setFixedSize(26, 26);
            btn->setToolTip(QString::fromUtf8(kAnchorTips[id]));
            btn->setObjectName("anchorButton");
            anchorGroup_->addButton(btn, id);
            anchorGrid->addWidget(btn, row, col);
            if (id == 4) btn->setChecked(true);
        }
    }
    anchorRow->addWidget(new QLabel(QStringLiteral("原点:"), this));
    anchorRow->addStretch();
    anchorRow->addLayout(anchorGrid);
    layout->addLayout(anchorRow);

    // ---- 幅/高さ ----
    auto *sizeGrid = new QGridLayout();
    sizeGrid->setSpacing(8);
    auto *wLabel = new QLabel(QStringLiteral("幅"), this);
    auto *hLabel = new QLabel(QStringLiteral("高さ"), this);
    wLabel->setObjectName("panelLabel");
    hLabel->setObjectName("panelLabel");
    widthSpin_  = new QSpinBox(this);
    heightSpin_ = new QSpinBox(this);
    widthSpin_->setRange(1, 16384);
    heightSpin_->setRange(1, 16384);
    sizeGrid->addWidget(wLabel,      0, 0);
    sizeGrid->addWidget(widthSpin_,  0, 1);
    sizeGrid->addWidget(hLabel,      1, 0);
    sizeGrid->addWidget(heightSpin_, 1, 1);
    layout->addLayout(sizeGrid);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto *cancelBtn  = new QPushButton(QStringLiteral("キャンセル"), this);
    auto *confirmBtn = new QPushButton(QStringLiteral("確定"), this);
    confirmBtn->setDefault(true);
    cancelBtn->setObjectName("panelCancelButton");
    confirmBtn->setObjectName("panelConfirmButton");
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(confirmBtn);
    layout->addLayout(btnRow);

    connect(anchorGroup_, &QButtonGroup::idToggled, this, [this](int, bool checked) {
        if (checked) emitSettingsChanged();
    });
    connect(widthSpin_,  &QSpinBox::valueChanged, this, &CanvasSizePanel::emitSettingsChanged);
    connect(heightSpin_, &QSpinBox::valueChanged, this, &CanvasSizePanel::emitSettingsChanged);
    connect(confirmBtn, &QPushButton::clicked, this, &CanvasSizePanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &CanvasSizePanel::cancelled);

    setFixedWidth(300);
}

void CanvasSizePanel::emitSettingsChanged()
{
    emit settingsChanged(anchorGroup_->checkedId(), widthSpin_->value(), heightSpin_->value());
}

void CanvasSizePanel::resetTo(int w, int h)
{
    const QSignalBlocker b1(widthSpin_), b2(heightSpin_), b3(anchorGroup_);
    widthSpin_->setValue(w);
    heightSpin_->setValue(h);
    if (QAbstractButton *b = anchorGroup_->button(4)) b->setChecked(true);
}

void CanvasSizePanel::setSizeFields(int w, int h)
{
    const QSignalBlocker b1(widthSpin_), b2(heightSpin_);
    widthSpin_->setValue(w);
    heightSpin_->setValue(h);
}
