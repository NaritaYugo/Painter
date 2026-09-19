#include "dialogs/BrightnessContrastPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>

BrightnessContrastPanel::BrightnessContrastPanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("明るさ・コントラスト"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    layout->addWidget(makeRow(QStringLiteral("明るさ"), brightnessSlider_, brightnessValueLabel_, -100, 100));
    layout->addWidget(makeRow(QStringLiteral("コントラスト"), contrastSlider_, contrastValueLabel_, -100, 100));

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

    connect(brightnessSlider_, &QSlider::valueChanged, this, &BrightnessContrastPanel::emitValuesChanged);
    connect(contrastSlider_,   &QSlider::valueChanged, this, &BrightnessContrastPanel::emitValuesChanged);
    connect(confirmBtn, &QPushButton::clicked, this, &BrightnessContrastPanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &BrightnessContrastPanel::cancelled);

    setFixedWidth(280);
}

QWidget *BrightnessContrastPanel::makeRow(const QString &labelText, QSlider *&sliderOut, QLabel *&valueLabelOut,
                                           int minV, int maxV)
{
    auto *row = new QWidget(this);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);

    auto *nameLabel = new QLabel(labelText, row);
    nameLabel->setObjectName("panelLabel");
    nameLabel->setFixedWidth(72);

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

void BrightnessContrastPanel::emitValuesChanged()
{
    brightnessValueLabel_->setText(QString::number(brightnessSlider_->value()));
    contrastValueLabel_->setText(QString::number(contrastSlider_->value()));
    emit valuesChanged(brightnessSlider_->value(), contrastSlider_->value());
}

void BrightnessContrastPanel::resetValues()
{
    brightnessSlider_->setValue(0);
    contrastSlider_->setValue(0);
}

void BrightnessContrastPanel::setValues(int brightness, int contrast)
{
    QSignalBlocker b1(brightnessSlider_), b2(contrastSlider_);
    brightnessSlider_->setValue(brightness);
    contrastSlider_->setValue(contrast);
    brightnessValueLabel_->setText(QString::number(brightness));
    contrastValueLabel_->setText(QString::number(contrast));
}
