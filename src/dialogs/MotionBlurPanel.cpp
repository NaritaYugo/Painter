#include "dialogs/MotionBlurPanel.h"

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
    label->setFixedWidth(48);

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

MotionBlurPanel::MotionBlurPanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("移動ぼかし"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    // ---- 平行/円形切り替え ----
    auto *modeRow = new QHBoxLayout();
    modeRow->setSpacing(8);
    auto *modeLabel = new QLabel(QStringLiteral("種類"), this);
    modeLabel->setObjectName("panelLabel");
    modeLabel->setFixedWidth(48);
    modeCombo_ = new QComboBox(this);
    modeCombo_->addItem(QStringLiteral("平行"));
    modeCombo_->addItem(QStringLiteral("円形"));
    modeRow->addWidget(modeLabel);
    modeRow->addWidget(modeCombo_, 1);
    layout->addLayout(modeRow);

    // ---- 平行モード: 角度・距離 ----
    angleRow_    = makeSliderRow(this, QStringLiteral("角度"), 0, 359, 0,  QStringLiteral("°"),  &angleSlider_,    &angleValueLabel_);
    distanceRow_ = makeSliderRow(this, QStringLiteral("距離"), 0, 300, 24, QStringLiteral("px"), &distanceSlider_, &distanceValueLabel_);
    layout->addWidget(angleRow_);
    layout->addWidget(distanceRow_);

    // ---- 円形モード: 中心(ハンドル)・振れ角 ----
    centerHintLabel_ = new QLabel(QStringLiteral("中心: キャンバス上のハンドルをドラッグして指定"), this);
    centerHintLabel_->setObjectName("panelLabel");
    centerHintLabel_->setWordWrap(true);
    layout->addWidget(centerHintLabel_);
    spanRow_ = makeSliderRow(this, QStringLiteral("振れ角"), 0, 180, 10, QStringLiteral("°"), &spanSlider_, &spanValueLabel_);
    layout->addWidget(spanRow_);

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

    connect(modeCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        applyModeVisibility(idx);
        emit modeChanged(idx);
    });
    connect(angleSlider_, &QSlider::valueChanged, this, [this](int v) {
        angleValueLabel_->setText(QString("%1°").arg(v));
        emit angleChanged((float)v);
    });
    connect(distanceSlider_, &QSlider::valueChanged, this, [this](int v) {
        distanceValueLabel_->setText(QString("%1px").arg(v));
        emit distanceChanged((float)v);
    });
    connect(spanSlider_, &QSlider::valueChanged, this, [this](int v) {
        spanValueLabel_->setText(QString("%1°").arg(v));
        emit angleSpanChanged((float)v);
    });
    connect(confirmBtn, &QPushButton::clicked, this, &MotionBlurPanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &MotionBlurPanel::cancelled);

    applyModeVisibility(0);
    setFixedWidth(300);
}

void MotionBlurPanel::applyModeVisibility(int mode)
{
    const bool parallel = (mode == 0);
    angleRow_->setVisible(parallel);
    distanceRow_->setVisible(parallel);
    centerHintLabel_->setVisible(!parallel);
    spanRow_->setVisible(!parallel);
    // 行を出し入れするとレイアウトの必要な高さが変わるので、パネル自体を縮め直す
    // (縮まないと下に余白が残る)。幅はsetFixedWidth()で固定したまま。
    adjustSize();
}

void MotionBlurPanel::setMode(int mode)
{
    QSignalBlocker blocker(modeCombo_);
    modeCombo_->setCurrentIndex(mode);
    applyModeVisibility(mode);
}

void MotionBlurPanel::setAngleDeg(float angleDeg)
{
    QSignalBlocker blocker(angleSlider_);
    const int v = qBound(0, (int)qRound(angleDeg), 359);
    angleSlider_->setValue(v);
    angleValueLabel_->setText(QString("%1°").arg(v));
}

void MotionBlurPanel::setDistancePx(float distancePx)
{
    QSignalBlocker blocker(distanceSlider_);
    const int v = qBound(0, (int)qRound(distancePx), 300);
    distanceSlider_->setValue(v);
    distanceValueLabel_->setText(QString("%1px").arg(v));
}

void MotionBlurPanel::setAngleSpanDeg(float spanDeg)
{
    QSignalBlocker blocker(spanSlider_);
    const int v = qBound(0, (int)qRound(spanDeg), 180);
    spanSlider_->setValue(v);
    spanValueLabel_->setText(QString("%1°").arg(v));
}
