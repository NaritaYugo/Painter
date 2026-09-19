#include "dialogs/ToneCurvePanel.h"
#include "dialogs/ToneCurveEditor.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

ToneCurvePanel::ToneCurvePanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("トーンカーブ"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    editor_ = new ToneCurveEditor(this);
    layout->addWidget(editor_, 0, Qt::AlignHCenter);

    deleteBtn_ = new QPushButton(QStringLiteral("選択中の点を削除"), this);
    deleteBtn_->setEnabled(false);
    deleteBtn_->setObjectName("panelDeleteButton");
    layout->addWidget(deleteBtn_);

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

    connect(editor_, &ToneCurveEditor::pointsChanged, this, &ToneCurvePanel::pointsChanged);
    connect(editor_, &ToneCurveEditor::selectionChanged, deleteBtn_, &QPushButton::setEnabled);
    connect(deleteBtn_, &QPushButton::clicked, editor_, &ToneCurveEditor::removeSelected);
    connect(confirmBtn, &QPushButton::clicked, this, &ToneCurvePanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &ToneCurvePanel::cancelled);

    setFixedWidth(272);
}

void ToneCurvePanel::resetValues()
{
    editor_->reset();
}

void ToneCurvePanel::setPoints(const QVector<QPointF> &points)
{
    editor_->setPoints(points);
}
