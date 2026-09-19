#include "components/CollapsibleSection.h"

#include <QToolButton>
#include <QVBoxLayout>

CollapsibleSection::CollapsibleSection(const QString &title, QWidget *parent)
    : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // 見出し。QToolButton の矢印(setArrowType)と文字を並べて出すことで、
    // 「クリックで開閉できる」ことが見た目で分かるようにする。
    header_ = new QToolButton(this);
    header_->setText(title);
    header_->setCheckable(true);
    header_->setChecked(true);
    // autoRaise にしておくと、ホバー時の強調をスタイル(=テーマ)側が描いてくれる。
    // ここで色を直接指定してしまうとテーマ切り替えに追従しなくなる。
    header_->setAutoRaise(true);
    header_->setArrowType(Qt::DownArrow);
    header_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    header_->setFocusPolicy(Qt::NoFocus);
    header_->setCursor(Qt::PointingHandCursor);
    // style.qss の QPushButton 共通ルールとは別物(QToolButton)なので、
    // ここでは見出しらしい見た目(左寄せ・太字・余白控えめ)だけを指定する。
    header_->setStyleSheet(
        "QToolButton { border: none; padding: 3px 2px; font-weight: bold; text-align: left; }");
    outer->addWidget(header_);

    content_       = new QWidget(this);
    contentLayout_ = new QVBoxLayout(content_);
    // 見出しより一段下げて、どこまでがこのセクションの中身か分かるようにする。
    contentLayout_->setContentsMargins(10, 2, 0, 6);
    contentLayout_->setSpacing(4);
    outer->addWidget(content_);

    connect(header_, &QToolButton::toggled, this, [this](bool on) {
        header_->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        content_->setVisible(on);
        emit expandedChanged(on);
    });
}

void CollapsibleSection::setExpanded(bool expanded)
{
    header_->setChecked(expanded); // 変化があれば toggled 経由で矢印と表示が揃う
}

bool CollapsibleSection::isExpanded() const
{
    return header_->isChecked();
}
