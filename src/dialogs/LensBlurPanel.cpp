#include "dialogs/LensBlurPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>

namespace {
// ラベル+スライダー+数値ラベルの1行を作る(このパネル内で4回使う定型)
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
    label->setFixedWidth(64);

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

LensBlurPanel::LensBlurPanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("レンズぼかし"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    // ---- ぼけ半径 ----
    layout->addWidget(makeSliderRow(this, QStringLiteral("半径"), 0, 64, 16, QStringLiteral("px"),
                                    &radiusSlider_, &radiusValueLabel_));

    // ---- 絞りの形 ----
    auto *bladesRow = new QHBoxLayout();
    bladesRow->setSpacing(8);
    auto *bladesLabel = new QLabel(QStringLiteral("絞りの形"), this);
    bladesLabel->setObjectName("panelLabel");
    bladesLabel->setFixedWidth(64);
    bladesCombo_ = new QComboBox(this);
    // itemData に辺数を持たせる(0=円)。シェーダーの uBlades にそのまま渡す。
    bladesCombo_->addItem(QStringLiteral("円形"),   0);
    bladesCombo_->addItem(QStringLiteral("五角形"), 5);
    bladesCombo_->addItem(QStringLiteral("六角形"), 6);
    bladesCombo_->addItem(QStringLiteral("七角形"), 7);
    bladesCombo_->addItem(QStringLiteral("八角形"), 8);
    bladesRow->addWidget(bladesLabel);
    bladesRow->addWidget(bladesCombo_, 1);
    layout->addLayout(bladesRow);

    // ---- 絞りの回転(多角形のときだけ意味がある) ----
    rotRow_ = makeSliderRow(this, QStringLiteral("回転"), 0, 359, 0, QStringLiteral("°"),
                            &rotSlider_, &rotValueLabel_);
    layout->addWidget(rotRow_);

    // ---- 玉ボケの強さ / しきい値 ----
    layout->addWidget(makeSliderRow(this, QStringLiteral("玉ボケ"), 0, 100, 60, QStringLiteral("%"),
                                    &boostSlider_, &boostValueLabel_));
    layout->addWidget(makeSliderRow(this, QStringLiteral("しきい値"), 0, 100, 70, QStringLiteral("%"),
                                    &thresholdSlider_, &thresholdValueLabel_));

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
        emit radiusChanged((float)v);
    });
    connect(bladesCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        const int blades = bladesCombo_->itemData(idx).toInt();
        rotRow_->setVisible(blades >= 3); // 円形では回転に意味がない
        adjustSize();
        emit bladesChanged(blades);
    });
    connect(rotSlider_, &QSlider::valueChanged, this, [this](int v) {
        rotValueLabel_->setText(QString("%1°").arg(v));
        emit bladeRotChanged((float)v);
    });
    connect(boostSlider_, &QSlider::valueChanged, this, [this](int v) {
        boostValueLabel_->setText(QString("%1%").arg(v));
        emit highlightBoostChanged((float)v / 100.0f);
    });
    connect(thresholdSlider_, &QSlider::valueChanged, this, [this](int v) {
        thresholdValueLabel_->setText(QString("%1%").arg(v));
        emit thresholdChanged((float)v / 100.0f);
    });
    connect(confirmBtn, &QPushButton::clicked, this, &LensBlurPanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &LensBlurPanel::cancelled);

    rotRow_->setVisible(false); // 既定は円形絞り
    setFixedWidth(320);
}

void LensBlurPanel::setRadiusPx(float radiusPx)
{
    QSignalBlocker blocker(radiusSlider_);
    const int v = qBound(0, (int)qRound(radiusPx), 64);
    radiusSlider_->setValue(v);
    radiusValueLabel_->setText(QString("%1px").arg(v));
}

void LensBlurPanel::setBlades(int blades)
{
    QSignalBlocker blocker(bladesCombo_);
    int idx = bladesCombo_->findData(blades);
    if (idx < 0) idx = 0;
    bladesCombo_->setCurrentIndex(idx);
    rotRow_->setVisible(bladesCombo_->itemData(idx).toInt() >= 3);
    adjustSize();
}

void LensBlurPanel::setBladeRotDeg(float rotDeg)
{
    QSignalBlocker blocker(rotSlider_);
    const int v = qBound(0, (int)qRound(rotDeg), 359);
    rotSlider_->setValue(v);
    rotValueLabel_->setText(QString("%1°").arg(v));
}

void LensBlurPanel::setHighlightBoost(float boost)
{
    QSignalBlocker blocker(boostSlider_);
    const int v = qBound(0, (int)qRound(boost * 100.0f), 100);
    boostSlider_->setValue(v);
    boostValueLabel_->setText(QString("%1%").arg(v));
}

void LensBlurPanel::setThreshold(float threshold)
{
    QSignalBlocker blocker(thresholdSlider_);
    const int v = qBound(0, (int)qRound(threshold * 100.0f), 100);
    thresholdSlider_->setValue(v);
    thresholdValueLabel_->setText(QString("%1%").arg(v));
}
