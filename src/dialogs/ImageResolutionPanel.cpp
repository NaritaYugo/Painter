#include "dialogs/ImageResolutionPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSpinBox>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>

ImageResolutionPanel::ImageResolutionPanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("画像解像度変更"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

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

    lockAspectCheck_ = new QCheckBox(QStringLiteral("縦横比を固定"), this);
    lockAspectCheck_->setObjectName("panelCheckbox");
    lockAspectCheck_->setChecked(true);
    layout->addWidget(lockAspectCheck_);

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

    connect(widthSpin_,  QOverload<int>::of(&QSpinBox::valueChanged), this, &ImageResolutionPanel::onWidthChanged);
    connect(heightSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &ImageResolutionPanel::onHeightChanged);
    connect(confirmBtn, &QPushButton::clicked, this, &ImageResolutionPanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &ImageResolutionPanel::cancelled);

    setFixedWidth(280);
}

void ImageResolutionPanel::onWidthChanged(int w)
{
    if (lockAspectCheck_->isChecked() && aspectRatio_ > 0.0) {
        const int newH = qMax(1, qRound(w / aspectRatio_));
        const QSignalBlocker b(heightSpin_);
        heightSpin_->setValue(newH);
    }
    emit sizeChanged(widthSpin_->value(), heightSpin_->value());
}

void ImageResolutionPanel::onHeightChanged(int h)
{
    if (lockAspectCheck_->isChecked() && aspectRatio_ > 0.0) {
        const int newW = qMax(1, qRound(h * aspectRatio_));
        const QSignalBlocker b(widthSpin_);
        widthSpin_->setValue(newW);
    }
    emit sizeChanged(widthSpin_->value(), heightSpin_->value());
}

void ImageResolutionPanel::resetTo(int w, int h)
{
    aspectRatio_ = (h > 0) ? (double)w / (double)h : 1.0;
    const QSignalBlocker b1(widthSpin_), b2(heightSpin_);
    widthSpin_->setValue(w);
    heightSpin_->setValue(h);
}
