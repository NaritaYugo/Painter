#include "dialogs/TextLayerPanel.h"
#include "components/ColorWheelWidget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QFontComboBox>
#include <QSpinBox>
#include <QToolButton>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>

TextLayerPanel::TextLayerPanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("テキスト"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    textEdit_ = new QPlainTextEdit(this);
    textEdit_->setFixedHeight(80);
    textEdit_->setPlaceholderText(QStringLiteral("文字を入力..."));
    layout->addWidget(textEdit_);

    auto *fontRow = new QHBoxLayout();
    fontCombo_ = new QFontComboBox(this);
    sizeSpin_  = new QSpinBox(this);
    sizeSpin_->setRange(1, 999);
    sizeSpin_->setValue(48);
    fontRow->addWidget(fontCombo_, 1);
    fontRow->addWidget(sizeSpin_);
    layout->addLayout(fontRow);

    auto *styleRow = new QHBoxLayout();
    boldCheck_   = new QCheckBox(QStringLiteral("B"), this);
    italicCheck_ = new QCheckBox(QStringLiteral("I"), this);
    boldCheck_->setObjectName("panelCheckbox");
    italicCheck_->setObjectName("panelCheckbox");
    colorBtn_ = new QToolButton(this);
    colorBtn_->setFixedSize(28, 22);
    styleRow->addWidget(boldCheck_);
    styleRow->addWidget(italicCheck_);
    styleRow->addStretch();
    styleRow->addWidget(new QLabel(QStringLiteral("色"), this));
    styleRow->addWidget(colorBtn_);
    layout->addLayout(styleRow);

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

    updateColorButton();

    connect(textEdit_,    &QPlainTextEdit::textChanged, this, &TextLayerPanel::emitValuesChanged);
    connect(fontCombo_,   &QFontComboBox::currentFontChanged, this, &TextLayerPanel::emitValuesChanged);
    connect(sizeSpin_,    QOverload<int>::of(&QSpinBox::valueChanged), this, &TextLayerPanel::emitValuesChanged);
    connect(boldCheck_,   &QCheckBox::toggled, this, &TextLayerPanel::emitValuesChanged);
    connect(italicCheck_, &QCheckBox::toggled, this, &TextLayerPanel::emitValuesChanged);
    connect(colorBtn_, &QToolButton::clicked, this, [this]() {
        // ColorCircleDock/SolidColorPickerPanelと同じColorWheelWidgetをポップアップで
        // 出す(QColorDialogは使わない、アプリ内で色選択UIを統一するため)。
        auto *popup = new QWidget(this, Qt::Popup);
        popup->setAttribute(Qt::WA_DeleteOnClose);
        auto *popupLayout = new QVBoxLayout(popup);
        popupLayout->setContentsMargins(8, 8, 8, 8);
        auto *wheel = new ColorWheelWidget(popup);
        wheel->pickColor(currentColor_);
        popupLayout->addWidget(wheel);
        connect(wheel, &ColorWheelWidget::colorChanged, this, [this](const QColor &c) {
            currentColor_ = c;
            updateColorButton();
            emitValuesChanged();
        });
        popup->move(colorBtn_->mapToGlobal(QPoint(0, colorBtn_->height())));
        popup->show();
    });
    connect(confirmBtn, &QPushButton::clicked, this, &TextLayerPanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &TextLayerPanel::cancelled);

    setFixedWidth(280);
}

void TextLayerPanel::updateColorButton()
{
    QPixmap pm(20, 14);
    pm.fill(currentColor_);
    colorBtn_->setIcon(QIcon(pm));
}

void TextLayerPanel::setValues(const TextParams &params)
{
    const QSignalBlocker b1(textEdit_), b2(fontCombo_), b3(sizeSpin_), b4(boldCheck_), b5(italicCheck_);
    textEdit_->setPlainText(params.text);
    fontCombo_->setCurrentFont(QFont(params.fontFamily));
    sizeSpin_->setValue(params.fontSize);
    boldCheck_->setChecked(params.bold);
    italicCheck_->setChecked(params.italic);
    currentColor_ = params.color;
    updateColorButton();
}

void TextLayerPanel::emitValuesChanged()
{
    TextParams p;
    p.text       = textEdit_->toPlainText();
    p.fontFamily = fontCombo_->currentFont().family();
    p.fontSize   = sizeSpin_->value();
    p.bold       = boldCheck_->isChecked();
    p.italic     = italicCheck_->isChecked();
    p.color      = currentColor_;
    emit valuesChanged(p);
}
