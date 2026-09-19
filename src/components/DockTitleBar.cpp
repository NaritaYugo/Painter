#include "components/DockTitleBar.h"
#include "components/ThemeColors.h"

#include <QDockWidget>
#include <QToolButton>
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOption>

// ===========================================================================
DockTitleBar::DockTitleBar(const QString &title, QWidget *parent)
    : QWidget(parent), m_title(title)
{
    setMinimumWidth(GRIP_WIDTH + CLOSE_SIZE + 2); // グリップ＋閉じるボタン最小幅

    // 閉じるボタン
    m_closeBtn = new QToolButton(this);
    m_closeBtn->setAutoRaise(true);
    m_closeBtn->setFixedSize(CLOSE_SIZE, CLOSE_SIZE);

    // システムの "close" アイコンを使う（テーマに追従）
    m_closeBtn->setIcon(style()->standardIcon(QStyle::SP_DockWidgetCloseButton));
    m_closeBtn->setIconSize(QSize(CLOSE_SIZE - 4, CLOSE_SIZE - 4));
    m_closeBtn->setToolTip("閉じる");

    connect(m_closeBtn, &QToolButton::clicked, this, [this] {
        if (auto *dock = qobject_cast<QDockWidget *>(parentWidget()))
            dock->close();
    });

    updateLayout();
}

// ---------------------------------------------------------------------------
QSize DockTitleBar::sizeHint()        const { return {120, BAR_HEIGHT}; }
QSize DockTitleBar::minimumSizeHint() const { return {GRIP_WIDTH + CLOSE_SIZE + 2, BAR_HEIGHT}; }

// ---------------------------------------------------------------------------
void DockTitleBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateLayout();
}

// ---------------------------------------------------------------------------
// グリップ＋タイトルを paintEvent で描画する
void DockTitleBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    // ── 背景：QDockWidget のタイトルバーに近い色をパレットから取得 ──
    const QColor bgColor = palette().color(QPalette::Window).darker(110);
    p.fillRect(rect(), bgColor);

    // ── グリップ ──
    const QRect gripRect(0, 0, GRIP_WIDTH, height());
    drawGrip(p, gripRect);

    // ── タイトル文字列（幅が閾値以上のとき表示）──
    if (width() >= TITLE_SHOW_THRESHOLD) {
        const int textX     = GRIP_WIDTH + TITLE_MARGIN;
        const int textRight = m_closeBtn->x() - TITLE_MARGIN;
        if (textRight > textX) {
            const QRect textRect(textX, 0, textRight - textX, height());
            p.setPen(palette().color(QPalette::WindowText));
            p.setFont(font());
            p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(m_title, Qt::ElideRight, textRect.width()));
        }
    }
}

// ---------------------------------------------------------------------------
// グリップ（点線2列）を描画
void DockTitleBar::drawGrip(QPainter &p, const QRect &rect)
{
    const QColor dotColor  = Theme::textDisabled;
    const int dotSize = 2;
    const int step    = 4;
    const int col0    = rect.x() + 2;
    const int col1    = col0 + 4;

    for (int y = rect.top() + 3; y + dotSize <= rect.bottom() - 2; y += step) {
        p.fillRect(col0, y, dotSize, dotSize, dotColor);
        p.fillRect(col1, y, dotSize, dotSize, dotColor);
    }
}

// ---------------------------------------------------------------------------
// 閉じるボタンを右端に配置する
void DockTitleBar::updateLayout()
{
    const int x = width() - CLOSE_SIZE - 2;
    const int y = (height() - CLOSE_SIZE) / 2;
    m_closeBtn->move(x, y);
}