#include "dialogs/CalibrationDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>

CalibrationDialog::CalibrationDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("モニターキャリブレーション"));
    setAttribute(Qt::WA_DeleteOnClose, false);

    auto *layout = new QVBoxLayout(this);

    auto *hint = new QLabel(QStringLiteral(
        "PC側のモニター設定で色味が合わない場合に、キャンバスの見た目だけを補正します。\n"
        "実際のレイヤーデータは変更されません(内部データ→カラーモード→この補正、の順に適用)。"), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    layout->addWidget(makeRow(QStringLiteral("明るさ"),     brightnessSlider_, brightnessValueLabel_));
    layout->addWidget(makeRow(QStringLiteral("コントラスト"), contrastSlider_,   contrastValueLabel_));
    layout->addWidget(makeRow(QStringLiteral("シアン"),      cyanSlider_,       cyanValueLabel_));
    layout->addWidget(makeRow(QStringLiteral("マゼンタ"),    magentaSlider_,    magentaValueLabel_));
    layout->addWidget(makeRow(QStringLiteral("イエロー"),    yellowSlider_,     yellowValueLabel_));

    auto *btnRow = new QHBoxLayout();
    auto *resetBtn = new QPushButton(QStringLiteral("リセット"), this);
    btnRow->addWidget(resetBtn);
    btnRow->addStretch();
    auto *closeBtn = new QPushButton(QStringLiteral("閉じる"), this);
    closeBtn->setDefault(true);
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);

    for (QSlider *s : {brightnessSlider_, contrastSlider_, cyanSlider_, magentaSlider_, yellowSlider_})
        connect(s, &QSlider::valueChanged, this, &CalibrationDialog::emitValuesChanged);
    connect(resetBtn, &QPushButton::clicked, this, &CalibrationDialog::resetValues);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);

    setFixedWidth(340);
}

QWidget *CalibrationDialog::makeRow(const QString &labelText, QSlider *&sliderOut, QLabel *&valueLabelOut)
{
    auto *row = new QWidget(this);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);

    auto *nameLabel = new QLabel(labelText, row);
    nameLabel->setFixedWidth(72);

    sliderOut = new QSlider(Qt::Horizontal, row);
    sliderOut->setRange(-100, 100);
    sliderOut->setValue(0);

    valueLabelOut = new QLabel("0", row);
    valueLabelOut->setFixedWidth(32);
    valueLabelOut->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    h->addWidget(nameLabel);
    h->addWidget(sliderOut, 1);
    h->addWidget(valueLabelOut);
    return row;
}

void CalibrationDialog::emitValuesChanged()
{
    brightnessValueLabel_->setText(QString::number(brightnessSlider_->value()));
    contrastValueLabel_->setText(QString::number(contrastSlider_->value()));
    cyanValueLabel_->setText(QString::number(cyanSlider_->value()));
    magentaValueLabel_->setText(QString::number(magentaSlider_->value()));
    yellowValueLabel_->setText(QString::number(yellowSlider_->value()));

    emit valuesChanged(brightnessSlider_->value(), contrastSlider_->value(),
                        cyanSlider_->value(), magentaSlider_->value(), yellowSlider_->value());
}

void CalibrationDialog::resetValues()
{
    setValues(0, 0, 0, 0, 0);
    emitValuesChanged();
}

void CalibrationDialog::setValues(int brightness, int contrast, int cyan, int magenta, int yellow)
{
    const QSignalBlocker b1(brightnessSlider_), b2(contrastSlider_),
                          b3(cyanSlider_), b4(magentaSlider_), b5(yellowSlider_);
    brightnessSlider_->setValue(brightness);
    contrastSlider_->setValue(contrast);
    cyanSlider_->setValue(cyan);
    magentaSlider_->setValue(magenta);
    yellowSlider_->setValue(yellow);

    brightnessValueLabel_->setText(QString::number(brightness));
    contrastValueLabel_->setText(QString::number(contrast));
    cyanValueLabel_->setText(QString::number(cyan));
    magentaValueLabel_->setText(QString::number(magenta));
    yellowValueLabel_->setText(QString::number(yellow));
}
