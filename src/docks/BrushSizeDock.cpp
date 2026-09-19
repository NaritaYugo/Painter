#include "docks/BrushSizeDock.h"

#include <QGridLayout>
#include <QVBoxLayout>
#include <QButtonGroup>
#include <QPushButton>
#include <QPainter>
#include <QResizeEvent>
#include <QPen>
#include <QtMath>
#include <algorithm>

// ===========================================================================
// SizeButton
// ===========================================================================
class SizeButton : public QPushButton
{
public:
    explicit SizeButton(int px, int row, int col, QWidget *parent = nullptr)
        : QPushButton(parent), m_px(px), m_row(row), m_col(col)
    {
        setCheckable(true);
        setFlat(true);
        setFixedSize(BrushSizeDock::BTN_WIDTH, BrushSizeDock::BTN_HEIGHT);
        setToolTip(QString("%1 px").arg(px));
        setText("");
    }

    int sizePx() const { return m_px; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QRect rc = rect();

        // ── 背景・グリッド線 ──
        // 選択中：1マスハイライト、それ以外：透明＋枠線のみ
        if (isChecked()) {
            p.fillRect(rc.adjusted(-1, -1, -1, -1), QColor("#5695dd"));
        }else if (underMouse()) {
            p.fillRect(rc.adjusted(-1, -1, -1, -1), QColor(255, 255, 255, 60)); 
        } else {
            p.fillRect(rc.adjusted(-1, -1, -1, -1), QColor(255, 255, 255, 30));
        }

        // ── 丸プレビュー ──
        const int labelH    = 6;
        const int dotAreaH  = BrushSizeDock::BTN_WIDTH - 4;
        const int dotAreaW  = BrushSizeDock::BTN_WIDTH - 4;
        const qreal maxR    = qMin(dotAreaW, dotAreaH) / 2.0 - 1.0;
        const qreal actualR = m_px / 2.0;
        const bool isActual = actualR <= maxR;
        const qreal drawR   = isActual ? actualR : maxR;

        const qreal cx = rc.width()  / 2.0;
        const qreal cy = 2.0 + dotAreaH / 2.0;

        p.setBrush(QColor(Qt::white));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(cx, cy), drawR, drawR);

        // ── サイズラベル ──
        QFont f = p.font();
        f.setPixelSize(9);
        p.setFont(f);

        QColor textColor = palette().color(QPalette::WindowText);
        p.setPen(textColor);

        const QString label = QString("%1").arg(m_px);
        p.drawText(QRect(0, dotAreaH + 4, rc.width(), rc.height() - dotAreaH - 8),
                   Qt::AlignHCenter | Qt::AlignVCenter, label);
    }

private:
    int m_px, m_row, m_col;
};

// ===========================================================================
// BrushSizeDock
// ===========================================================================

QList<int> BrushSizeDock::generateSizes(int count)
{
    if (count <= 0) return {};
    if (count == 1) return { MIN_SIZE };

    const double logMin = qLn((double)MIN_SIZE);
    const double logMax = qLn((double)MAX_SIZE);

    // まず重複なしで生成できる最大数を確認
    // MIN_SIZE〜MAX_SIZEの整数の種類数が上限
    // count がそれを超える場合はその上限に丸める
    QList<int> sizes;
    sizes.reserve(count);

    // count 個ぴったり確保されるまでステップを細かくしながら再試行
    for (int tries = count; tries <= count * 10; ++tries) {
        sizes.clear();
        for (int i = 0; i < tries; ++i) {
            const double t  = double(i) / (tries - 1);
            const int    px = qRound(qExp(logMin + t * (logMax - logMin)));
            if (sizes.isEmpty() || sizes.last() != px)
                sizes.append(px);
            if (sizes.size() == count) break;
        }
        if (sizes.size() >= count) break;
    }

    // それでも足りない場合（整数の種類が count に満たない）はそのまま返す
    return sizes;
}

int BrushSizeDock::calcCols(int w)
{
    return qMax(1, w / BTN_WIDTH);
}

int BrushSizeDock::calcRows(int h)
{
    return qMax(1, h / BTN_HEIGHT);
}

BrushSizeDock::BrushSizeDock(QWidget *parent)
    : QWidget(parent)
{
    setMinimumWidth(BTN_WIDTH);
    setMinimumHeight(BTN_HEIGHT);

    auto *vLayout = new QVBoxLayout(this);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(0);

    m_grid = new QGridLayout();
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(0);
    vLayout->addLayout(m_grid);

    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);

    connect(m_group, &QButtonGroup::idClicked, this, [this](int id) {
        m_selectedPx = id;
        emit brushSizeChanged(id);
    });
}

void BrushSizeDock::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    const int cols  = calcCols(event->size().width());
    const int rows  = calcRows(event->size().height());
    const int count = cols * rows;

    if (count == m_lastCount) return;
    rebuildButtons(cols, rows);
}

void BrushSizeDock::rebuildButtons(int cols, int rows)
{
    const int count = cols * rows;
    m_lastCount = count;

    while (m_grid->count() > 0)
        delete m_grid->takeAt(0);

    const auto oldBtns = m_group->buttons();
    for (auto *b : oldBtns) {
        m_group->removeButton(b);
        delete b;
    }

    const QList<int> sizes = generateSizes(count);
    const int nearPx = nearestSize(m_selectedPx);

    for (int i = 0; i < sizes.size(); ++i) {
        const int px  = sizes[i];
        const int row = i / cols;
        const int col = i % cols;
        auto *btn = new SizeButton(px, row, col, this);
        m_group->addButton(btn, px);
        m_grid->addWidget(btn, i / cols, i % cols);
        if (px == nearPx)
            btn->setChecked(true);
    }

    m_grid->setColumnStretch(cols, 10000);

    if (!m_group->checkedButton()) {
        int bestPx = sizes.first(), bestDiff = INT_MAX;
        for (int px : sizes) {
            int diff = qAbs(px - m_selectedPx);
            if (diff < bestDiff) { bestDiff = diff; bestPx = px; }
        }
        if (auto *b = m_group->button(bestPx))
            b->setChecked(true);
    }
}

int BrushSizeDock::nearestSize(int px) const
{
    const auto btns = m_group->buttons();
    if (btns.isEmpty()) return px;

    int bestPx = static_cast<SizeButton *>(btns.first())->sizePx();
    int bestDiff = INT_MAX;
    for (auto *b : btns) {
        const int p    = static_cast<SizeButton *>(b)->sizePx();
        const int diff = qAbs(p - px);
        if (diff < bestDiff) { bestDiff = diff; bestPx = p; }
    }
    return bestPx;
}

void BrushSizeDock::syncSize(int px)
{
    m_selectedPx = px;
    m_group->blockSignals(true);
    const int near = nearestSize(px);
    if (auto *b = m_group->button(near))
        b->setChecked(true);
    m_group->blockSignals(false);
}

void BrushSizeDock::stepUp()
{
    const auto btns = m_group->buttons();
    if (btns.isEmpty()) return;

    // 現在のサイズリストを昇順で取得
    QList<int> sizes;
    for (auto *b : btns)
        sizes.append(static_cast<SizeButton *>(b)->sizePx());
    std::sort(sizes.begin(), sizes.end());

    // 現在選択中より大きい最初のサイズを選ぶ
    for (int px : sizes) {
        if (px > m_selectedPx) {
            m_selectedPx = px;
            if (auto *b = m_group->button(px)) {
                b->setChecked(true);
                emit brushSizeChanged(px);
            }
            return;
        }
    }
    // すでに最大なら何もしない
}

void BrushSizeDock::stepDown()
{
    const auto btns = m_group->buttons();
    if (btns.isEmpty()) return;

    QList<int> sizes;
    for (auto *b : btns)
        sizes.append(static_cast<SizeButton *>(b)->sizePx());
    std::sort(sizes.begin(), sizes.end());

    // 現在選択中より小さい最後のサイズを選ぶ
    for (int i = sizes.size() - 1; i >= 0; --i) {
        if (sizes[i] < m_selectedPx) {
            m_selectedPx = sizes[i];
            if (auto *b = m_group->button(sizes[i])) {
                b->setChecked(true);
                emit brushSizeChanged(sizes[i]);
            }
            return;
        }
    }
    // すでに最小なら何もしない
}