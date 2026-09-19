#include "dialogs/GaussianBlurPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>

GaussianBlurPanel::GaussianBlurPanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("ガウスぼかし"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    auto *row = new QHBoxLayout();
    row->setSpacing(8);

    auto *nameLabel = new QLabel(QStringLiteral("半径"), this);
    nameLabel->setObjectName("panelLabel");
    nameLabel->setFixedWidth(32);

    radiusSlider_ = new QSlider(Qt::Horizontal, this);
    radiusSlider_->setRange(1, 64);
    radiusSlider_->setValue(8);

    radiusValueLabel_ = new QLabel("8px", this);
    radiusValueLabel_->setObjectName("panelLabel");
    radiusValueLabel_->setFixedWidth(36);
    radiusValueLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    row->addWidget(nameLabel);
    row->addWidget(radiusSlider_, 1);
    row->addWidget(radiusValueLabel_);
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

    connect(radiusSlider_, &QSlider::valueChanged, this, [this](int v) {
        radiusValueLabel_->setText(QString("%1px").arg(v));
        emit radiusChanged(v);
    });
    connect(confirmBtn, &QPushButton::clicked, this, &GaussianBlurPanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &GaussianBlurPanel::cancelled);

    setFixedWidth(280);
}

void GaussianBlurPanel::setRadius(int radius)
{
    QSignalBlocker blocker(radiusSlider_);
    radiusSlider_->setValue(radius);
    radiusValueLabel_->setText(QString("%1px").arg(radius));
}
