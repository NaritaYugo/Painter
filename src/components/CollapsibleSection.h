#pragma once

#include <QWidget>

class QToolButton;
class QVBoxLayout;

// ===========================================================================
// CollapsibleSection  ―  見出しをクリックして開閉できるセクション
// ---------------------------------------------------------------------------
// 設定項目が増えてくると1枚のパネルに縦一列で並べるのは辛いので、ジャンルごとに
// このセクションでまとめて折り畳めるようにする(ツール設定ドックで使用)。
//
// 使い方:
//   auto *sec = new CollapsibleSection("基本", parent);
//   sec->contentLayout()->addWidget(...);   // 中身は contentLayout() へ足す
//   parentLayout->addWidget(sec);
//
// 開閉状態の永続化はこのクラスの責務ではない(どのキーで保存するかは利用側の
// 都合なので)。expandedChanged() を拾って利用側で保存する。
// ===========================================================================
class CollapsibleSection : public QWidget
{
    Q_OBJECT
public:
    explicit CollapsibleSection(const QString &title, QWidget *parent = nullptr);

    // 中身を足す先。セクションのウィジェットはすべてここへ addWidget/addLayout する。
    QVBoxLayout *contentLayout() const { return contentLayout_; }

    void setExpanded(bool expanded);
    bool isExpanded() const;

signals:
    void expandedChanged(bool expanded);

private:
    QToolButton *header_        = nullptr;
    QWidget     *content_       = nullptr;
    QVBoxLayout *contentLayout_ = nullptr;
};
