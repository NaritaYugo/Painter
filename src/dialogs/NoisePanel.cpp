#include "dialogs/NoisePanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>

namespace {
// ラベル+スライダー+数値ラベルの1行を作る(このパネル内で2回使う定型)
QWidget *makeSliderRow(QWidget *parent, const QString &labelText,
                       int minV, int maxV, int initV, const QString &suffix,
                       QSlider **outSlider, QLabel **outValueLabel)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *label = new QLabel(labelText, row);
    label->setObjectName("panelLabel");
    label->setFixedWidth(56);

    auto *slider = new QSlider(Qt::Horizontal, row);
    slider->setRange(minV, maxV);
    slider->setValue(initV);

    auto *valueLabel = new QLabel(QString("%1%2").arg(initV).arg(suffix), row);
    valueLabel->setObjectName("panelLabel");
    valueLabel->setFixedWidth(40);
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    layout->addWidget(label);
    layout->addWidget(slider, 1);
    layout->addWidget(valueLabel);

    *outSlider = slider;
    *outValueLabel = valueLabel;
    return row;
}
} // namespace

NoisePanel::NoisePanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("ノイズ"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    // ---- 強さ ----
    layout->addWidget(makeSliderRow(this, QStringLiteral("強さ"), 0, 100, 25, QStringLiteral("%"),
                                    &strengthSlider_, &strengthValueLabel_));

    // ---- 種類(モノクロ/カラー) ----
    auto *typeRow = new QHBoxLayout();
    typeRow->setSpacing(8);
    auto *typeLabel = new QLabel(QStringLiteral("種類"), this);
    typeLabel->setObjectName("panelLabel");
    typeLabel->setFixedWidth(56);
    typeCombo_ = new QComboBox(this);
    typeCombo_->addItem(QStringLiteral("モノクロ")); // index 0
    typeCombo_->addItem(QStringLiteral("カラー"));   // index 1
    typeRow->addWidget(typeLabel);
    typeRow->addWidget(typeCombo_, 1);
    layout->addLayout(typeRow);

    // ---- 粒の大きさ ----
    layout->addWidget(makeSliderRow(this, QStringLiteral("粒の大きさ"), 1, 16, 1, QStringLiteral("px"),
                                    &grainSlider_, &grainValueLabel_));

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

    connect(strengthSlider_, &QSlider::valueChanged, this, [this](int v) {
        strengthValueLabel_->setText(QString("%1%").arg(v));
        emit strengthChanged((float)v / 100.0f);
    });
    connect(typeCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        emit monochromeChanged(idx == 0);
    });
    connect(grainSlider_, &QSlider::valueChanged, this, [this](int v) {
        grainValueLabel_->setText(QString("%1px").arg(v));
        emit grainChanged((float)v);
    });
    connect(confirmBtn, &QPushButton::clicked, this, &NoisePanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &NoisePanel::cancelled);

    setFixedWidth(300);
}

void NoisePanel::setStrength(float strength01)
{
    QSignalBlocker blocker(strengthSlider_);
    const int v = qBound(0, (int)qRound(strength01 * 100.0f), 100);
    strengthSlider_->setValue(v);
    strengthValueLabel_->setText(QString("%1%").arg(v));
}

void NoisePanel::setMonochrome(bool mono)
{
    QSignalBlocker blocker(typeCombo_);
    typeCombo_->setCurrentIndex(mono ? 0 : 1);
}

void NoisePanel::setGrainPx(float grainPx)
{
    QSignalBlocker blocker(grainSlider_);
    const int v = qBound(1, (int)qRound(grainPx), 16);
    grainSlider_->setValue(v);
    grainValueLabel_->setText(QString("%1px").arg(v));
}
