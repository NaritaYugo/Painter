#include "dialogs/GradientMapPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

GradientMapPanel::GradientMapPanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("グラデーションマップ"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    auto *hint = new QLabel(
        QStringLiteral("帯をクリックで色を追加 / つまみをドラッグで位置変更 / ダブルクリックで色を変更"), this);
    hint->setObjectName("panelLabel");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    editor_ = new GradientStripEditor(this);
    layout->addWidget(editor_);

    auto *editRow = new QHBoxLayout();
    editRow->setSpacing(8);
    colorBtn_  = new QPushButton(QStringLiteral("色を変更"), this);
    removeBtn_ = new QPushButton(QStringLiteral("削除"), this);
    auto *resetBtn = new QPushButton(QStringLiteral("リセット"), this);
    colorBtn_->setEnabled(false);
    removeBtn_->setEnabled(false);
    editRow->addWidget(colorBtn_);
    editRow->addWidget(removeBtn_);
    editRow->addWidget(resetBtn);
    editRow->addStretch();
    layout->addLayout(editRow);

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

    connect(editor_, &GradientStripEditor::stopsChanged, this, &GradientMapPanel::stopsChanged);
    connect(editor_, &GradientStripEditor::selectionChanged, this, [this](bool canRemove) {
        // 「削除」は最低2個を割らない場合だけ有効。「色を変更」は選択さえあれば
        // (2個しかなくても)有効なので、canRemoveではなくhasSelection()で判定する。
        removeBtn_->setEnabled(canRemove);
        colorBtn_->setEnabled(editor_->hasSelection());
    });
    connect(colorBtn_,  &QPushButton::clicked, this, [this] { editor_->openColorPickerForSelected(); });
    connect(removeBtn_, &QPushButton::clicked, this, [this] { editor_->removeSelected(); });
    connect(resetBtn,   &QPushButton::clicked, this, [this] {
        editor_->reset();
        emit stopsChanged(editor_->stops());
    });
    connect(confirmBtn, &QPushButton::clicked, this, &GradientMapPanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &GradientMapPanel::cancelled);

    setFixedWidth(320);
}

void GradientMapPanel::resetGradient()
{
    editor_->reset();
    colorBtn_->setEnabled(false);
    removeBtn_->setEnabled(false);
}

void GradientMapPanel::setStops(const QVector<GradientStripEditor::Stop> &stops)
{
    editor_->setStops(stops);
    colorBtn_->setEnabled(false);
    removeBtn_->setEnabled(false);
}
