#include "dialogs/SolidColorPickerPanel.h"
#include "components/ColorWheelWidget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

SolidColorPickerPanel::SolidColorPickerPanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("レイヤーの色"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    wheel_ = new ColorWheelWidget(this);
    layout->addWidget(wheel_);
    connect(wheel_, &ColorWheelWidget::colorChanged, this, &SolidColorPickerPanel::colorChanged);

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

    connect(confirmBtn, &QPushButton::clicked, this, &SolidColorPickerPanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &SolidColorPickerPanel::cancelled);

    setFixedWidth(280);
}

void SolidColorPickerPanel::setColor(const QColor &color)
{
    wheel_->pickColor(color);
}
