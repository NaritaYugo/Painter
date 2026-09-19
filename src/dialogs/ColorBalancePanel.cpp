#include "dialogs/ColorBalancePanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>

ColorBalancePanel::ColorBalancePanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("カラーバランス"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    layout->addWidget(makeRow(QStringLiteral("C"), cyanSlider_,    cyanValueLabel_,    -100, 100));
    layout->addWidget(makeRow(QStringLiteral("M"), magentaSlider_, magentaValueLabel_, -100, 100));
    layout->addWidget(makeRow(QStringLiteral("Y"), yellowSlider_,  yellowValueLabel_,  -100, 100));

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

    connect(cyanSlider_,    &QSlider::valueChanged, this, &ColorBalancePanel::emitValuesChanged);
    connect(magentaSlider_, &QSlider::valueChanged, this, &ColorBalancePanel::emitValuesChanged);
    connect(yellowSlider_,  &QSlider::valueChanged, this, &ColorBalancePanel::emitValuesChanged);
    connect(confirmBtn, &QPushButton::clicked, this, &ColorBalancePanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &ColorBalancePanel::cancelled);

    setFixedWidth(280);
}

QWidget *ColorBalancePanel::makeRow(const QString &labelText, QSlider *&sliderOut, QLabel *&valueLabelOut,
                                     int minV, int maxV)
{
    auto *row = new QWidget(this);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);

    auto *nameLabel = new QLabel(labelText, row);
    nameLabel->setObjectName("panelLabel");
    nameLabel->setFixedWidth(20);

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

void ColorBalancePanel::emitValuesChanged()
{
    cyanValueLabel_->setText(QString::number(cyanSlider_->value()));
    magentaValueLabel_->setText(QString::number(magentaSlider_->value()));
    yellowValueLabel_->setText(QString::number(yellowSlider_->value()));
    emit valuesChanged(cyanSlider_->value(), magentaSlider_->value(), yellowSlider_->value());
}

void ColorBalancePanel::resetValues()
{
    cyanSlider_->setValue(0);
    magentaSlider_->setValue(0);
    yellowSlider_->setValue(0);
}

void ColorBalancePanel::setValues(int cyan, int magenta, int yellow)
{
    QSignalBlocker b1(cyanSlider_), b2(magentaSlider_), b3(yellowSlider_);
    cyanSlider_->setValue(cyan);
    magentaSlider_->setValue(magenta);
    yellowSlider_->setValue(yellow);
    cyanValueLabel_->setText(QString::number(cyan));
    magentaValueLabel_->setText(QString::number(magenta));
    yellowValueLabel_->setText(QString::number(yellow));
}
