#pragma once

#include <QStyledItemDelegate>
#include <QAbstractItemView>
#include <QFontMetrics>
#include <QPainter>
#include <QApplication>
#include <QDebug> // ターミナル出力用

class MultiColumnDelegate : public QStyledItemDelegate
{
public:
    explicit MultiColumnDelegate(
        int columnCount,
        QObject* parent = nullptr)
        : QStyledItemDelegate(parent)
        , m_columnCount(columnCount)
        , m_alignments(columnCount, Qt::AlignLeft)
        , m_fixedWidths(columnCount, -1) // 初期値 -1 (未設定)
    {
    }

    // 引数に fixedWidth を追加（デフォルトは -1 で自動計算）
    void setAlignment(
        int column,
        Qt::Alignment alignment,
        int fixedWidth = -1)
    {
        if (column >= 0 && column < m_columnCount) {
            m_alignments[column] = alignment;
            m_fixedWidths[column] = fixedWidth;
        }
    }

protected:

    // 描画およびサイズ計算用の幅を取得・計算する関数
    QVector<int> getColumnWidths(
        const QAbstractItemModel* model,
        const QFontMetrics& fm) const
    {
        QVector<int> widths = m_fixedWidths;

        for (int col = 0; col < m_columnCount; ++col)
        {
            // 設定済み（0以上）の場合は計算をスキップ
            if (widths[col] >= 0) continue;

            int maxWidth = 0;
            for (int row = 0; row < model->rowCount(); ++row)
            {
                QModelIndex idx = model->index(row, 0);
                QString text = idx.data(Qt::UserRole + 1 + col).toString();

                int w = fm.horizontalAdvance(text);

                if (w > maxWidth) {
                    maxWidth = w;
                }
            }

            // キャッシュに保存して次回以降の計算をスキップ
            m_fixedWidths[col] = maxWidth;
            widths[col] = maxWidth;

            // 次回から手動設定できるよう、ターミナルに数値を出力
            qDebug() << "[MultiColumnDelegate] Auto-calculated Column" << col << "width:" << maxWidth;
        }

        return widths;
    }

    void paint(
        QPainter* painter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();

        painter->save();

        QApplication::style()->drawControl(
            QStyle::CE_ItemViewItem,
            &opt,
            painter,
            opt.widget);

        // 重い calculateColumnWidths ではなく、キャッシュ対応の getColumnWidths を使用
        auto widths = getColumnWidths(index.model(), option.fontMetrics);

        constexpr int margin = 8;
        int x = option.rect.left() + margin;

        for (int col = 0; col < m_columnCount; ++col)
        {
            QString text = index.data(Qt::UserRole + 1 + col).toString();

            QRect r(
                x,
                option.rect.top(),
                widths[col],
                option.rect.height());

            painter->drawText(
                r,
                m_alignments[col] | Qt::AlignVCenter,
                text);

            x += widths[col] + margin;
        }

        painter->restore();
    }

private:
    int m_columnCount;
    QVector<Qt::Alignment> m_alignments;
    mutable QVector<int> m_fixedWidths; // const 関数内でも更新できるよう mutable に設定
};