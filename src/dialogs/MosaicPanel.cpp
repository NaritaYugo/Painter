#include "dialogs/MosaicPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>

MosaicPanel::MosaicPanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("モザイク"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    auto *row = new QHBoxLayout();
    row->setSpacing(8);

    auto *nameLabel = new QLabel(QStringLiteral("サイズ"), this);
    nameLabel->setObjectName("panelLabel");
    nameLabel->setFixedWidth(32);

    blockSizeSlider_ = new QSlider(Qt::Horizontal, this);
    blockSizeSlider_->setRange(2, 256);
    blockSizeSlider_->setValue(16);

    blockSizeValueLabel_ = new QLabel("16px", this);
    blockSizeValueLabel_->setObjectName("panelLabel");
    blockSizeValueLabel_->setFixedWidth(36);
    blockSizeValueLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    row->addWidget(nameLabel);
    row->addWidget(blockSizeSlider_, 1);
    row->addWidget(blockSizeValueLabel_);
    layout->addLayout(row);

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

    connect(blockSizeSlider_, &QSlider::valueChanged, this, [this](int v) {
        blockSizeValueLabel_->setText(QString("%1px").arg(v));
        emit blockSizeChanged(v);
    });
    connect(confirmBtn, &QPushButton::clicked, this, &MosaicPanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &MosaicPanel::cancelled);

    setFixedWidth(280);
}

void MosaicPanel::setBlockSize(int blockSize)
{
    QSignalBlocker blocker(blockSizeSlider_);
    blockSizeSlider_->setValue(blockSize);
    blockSizeValueLabel_->setText(QString("%1px").arg(blockSize));
}
