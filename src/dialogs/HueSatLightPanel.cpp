#include "dialogs/HueSatLightPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>

HueSatLightPanel::HueSatLightPanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("色相・彩度・明度"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    layout->addWidget(makeRow(QStringLiteral("色相"),   hueSlider_,   hueValueLabel_,   -180, 180));
    layout->addWidget(makeRow(QStringLiteral("彩度"),   satSlider_,   satValueLabel_,   -100, 100));
    layout->addWidget(makeRow(QStringLiteral("明度"),   lightSlider_, lightValueLabel_, -100, 100));

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

    connect(hueSlider_,   &QSlider::valueChanged, this, &HueSatLightPanel::emitValuesChanged);
    connect(satSlider_,   &QSlider::valueChanged, this, &HueSatLightPanel::emitValuesChanged);
    connect(lightSlider_, &QSlider::valueChanged, this, &HueSatLightPanel::emitValuesChanged);
    connect(confirmBtn, &QPushButton::clicked, this, &HueSatLightPanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &HueSatLightPanel::cancelled);

    setFixedWidth(280);
}

QWidget *HueSatLightPanel::makeRow(const QString &labelText, QSlider *&sliderOut, QLabel *&valueLabelOut,
                                    int minV, int maxV)
{
    auto *row = new QWidget(this);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);

    auto *nameLabel = new QLabel(labelText, row);
    nameLabel->setObjectName("panelLabel");
    nameLabel->setFixedWidth(36);

    sliderOut = new QSlider(Qt::Horizontal, row);
    sliderOut->setRange(minV, maxV);
    sliderOut->setValue(0);

    valueLabelOut = new QLabel("0", row);
    valueLabelOut->setObjectName("panelLabel");
    valueLabelOut->setFixedWidth(32);
    valueLabelOut->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    h->addWidget(nameLabel);
    h->addWidget(sliderOut, 1);
    h->addWidget(valueLabelOut);
    return row;
}

void HueSatLightPanel::emitValuesChanged()
{
    hueValueLabel_->setText(QString::number(hueSlider_->value()));
    satValueLabel_->setText(QString::number(satSlider_->value()));
    lightValueLabel_->setText(QString::number(lightSlider_->value()));
    emit valuesChanged(hueSlider_->value(), satSlider_->value(), lightSlider_->value());
}

void HueSatLightPanel::resetValues()
{
    hueSlider_->setValue(0);
    satSlider_->setValue(0);
    lightSlider_->setValue(0);
}

void HueSatLightPanel::setValues(int hue, int saturation, int lightness)
{
    QSignalBlocker b1(hueSlider_), b2(satSlider_), b3(lightSlider_);
    hueSlider_->setValue(hue);
    satSlider_->setValue(saturation);
    lightSlider_->setValue(lightness);
    hueValueLabel_->setText(QString::number(hue));
    satValueLabel_->setText(QString::number(saturation));
    lightValueLabel_->setText(QString::number(lightness));
}
