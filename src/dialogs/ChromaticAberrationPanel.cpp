#include "dialogs/ChromaticAberrationPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>

ChromaticAberrationPanel::ChromaticAberrationPanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("色収差"), this);
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

    // ---- 角度(平行のときのみ表示) ----
    angleRow_ = new QWidget(this);
    auto *angleRowLayout = new QHBoxLayout(angleRow_);
    angleRowLayout->setContentsMargins(0, 0, 0, 0);
    angleRowLayout->setSpacing(8);
    auto *angleLabel = new QLabel(QStringLiteral("角度"), this);
    angleLabel->setObjectName("panelLabel");
    angleLabel->setFixedWidth(48);
    angleSlider_ = new QSlider(Qt::Horizontal, angleRow_);
    angleSlider_->setRange(0, 359);
    angleSlider_->setValue(0);
    angleValueLabel_ = new QLabel(QStringLiteral("0°"), angleRow_);
    angleValueLabel_->setObjectName("panelLabel");
    angleValueLabel_->setFixedWidth(36);
    angleValueLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    angleRowLayout->addWidget(angleLabel);
    angleRowLayout->addWidget(angleSlider_, 1);
    angleRowLayout->addWidget(angleValueLabel_);
    layout->addWidget(angleRow_);

    // ---- 中心(円形のときのみ表示。数値入力ではなくキャンバス上のハンドルで指定) ----
    centerHintLabel_ = new QLabel(QStringLiteral("中心: キャンバス上のハンドルをドラッグして指定"), this);
    centerHintLabel_->setObjectName("panelLabel");
    centerHintLabel_->setWordWrap(true);
    centerHintLabel_->setVisible(false);
    layout->addWidget(centerHintLabel_);

    // ---- ずらす距離 ----
    auto *distRow = new QHBoxLayout();
    distRow->setSpacing(8);
    auto *distLabel = new QLabel(QStringLiteral("距離"), this);
    distLabel->setObjectName("panelLabel");
    distLabel->setFixedWidth(48);
    distanceSlider_ = new QSlider(Qt::Horizontal, this);
    distanceSlider_->setRange(0, 200);
    distanceSlider_->setValue(8);
    distanceValueLabel_ = new QLabel(QStringLiteral("8px"), this);
    distanceValueLabel_->setObjectName("panelLabel");
    distanceValueLabel_->setFixedWidth(36);
    distanceValueLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    distRow->addWidget(distLabel);
    distRow->addWidget(distanceSlider_, 1);
    distRow->addWidget(distanceValueLabel_);
    layout->addLayout(distRow);

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
        angleRow_->setVisible(idx == 0);
        centerHintLabel_->setVisible(idx == 1);
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
    connect(confirmBtn, &QPushButton::clicked, this, &ChromaticAberrationPanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &ChromaticAberrationPanel::cancelled);

    setFixedWidth(300);
}

void ChromaticAberrationPanel::setMode(int mode)
{
    QSignalBlocker blocker(modeCombo_);
    modeCombo_->setCurrentIndex(mode);
    angleRow_->setVisible(mode == 0);
    centerHintLabel_->setVisible(mode == 1);
}

void ChromaticAberrationPanel::setAngleDeg(float angleDeg)
{
    QSignalBlocker blocker(angleSlider_);
    const int v = qBound(0, (int)qRound(angleDeg), 359);
    angleSlider_->setValue(v);
    angleValueLabel_->setText(QString("%1°").arg(v));
}

void ChromaticAberrationPanel::setDistancePx(float distancePx)
{
    QSignalBlocker blocker(distanceSlider_);
    const int v = qBound(0, (int)qRound(distancePx), 200);
    distanceSlider_->setValue(v);
    distanceValueLabel_->setText(QString("%1px").arg(v));
}
