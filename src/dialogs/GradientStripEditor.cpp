#include "dialogs/GradientStripEditor.h"
#include "components/ColorWheelWidget.h"
#include "components/ThemeColors.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QLinearGradient>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr int kBandHeight   = 40; // グラデーションの帯の高さ
constexpr int kHandleRadius = 7;  // つまみの半径(当たり判定にも使う)
constexpr int kSideMargin   = kHandleRadius + 2; // 端のつまみが切れないための左右余白
} // namespace

GradientStripEditor::GradientStripEditor(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(74);
    reset();
}

void GradientStripEditor::reset()
{
    stops_ = { { 0.0f, QColor(0, 0, 0) }, { 1.0f, QColor(255, 255, 255) } };
    selectedIndex_ = -1;
    dragging_ = false;
    update();
    emit selectionChanged(false);
}

void GradientStripEditor::setStops(const QVector<Stop> &stops)
{
    stops_ = stops.size() >= 2 ? stops : QVector<Stop>{ { 0.0f, QColor(0, 0, 0) }, { 1.0f, QColor(255, 255, 255) } };
    selectedIndex_ = -1;
    dragging_ = false;
    update();
    emit selectionChanged(false);
}

QRect GradientStripEditor::bandRect() const
{
    return QRect(kSideMargin, 4, qMax(1, width() - kSideMargin * 2), kBandHeight);
}

int GradientStripEditor::handleY() const
{
    return bandRect().bottom() + 2 + kHandleRadius;
}

float GradientStripEditor::toPos(int widgetX) const
{
    const QRect band = bandRect();
    if (band.width() <= 1) return 0.0f;
    return qBound(0.0f, float(widgetX - band.left()) / float(band.width() - 1), 1.0f);
}

int GradientStripEditor::toX(float pos) const
{
    const QRect band = bandRect();
    return band.left() + int(std::lround(qBound(0.0f, pos, 1.0f) * (band.width() - 1)));
}

QColor GradientStripEditor::sampleAt(float pos) const
{
    if (stops_.isEmpty()) return Qt::black;
    if (pos <= stops_.first().pos) return stops_.first().color;
    if (pos >= stops_.last().pos)  return stops_.last().color;
    for (int i = 0; i + 1 < stops_.size(); i++) {
        const Stop &a = stops_[i], &b = stops_[i + 1];
        if (pos >= a.pos && pos <= b.pos) {
            const float span = b.pos - a.pos;
            const float t = (span > 1e-6f) ? (pos - a.pos) / span : 0.0f;
            return QColor::fromRgbF(
                a.color.redF()   + (b.color.redF()   - a.color.redF())   * t,
                a.color.greenF() + (b.color.greenF() - a.color.greenF()) * t,
                a.color.blueF()  + (b.color.blueF()  - a.color.blueF())  * t);
        }
    }
    return stops_.last().color;
}

int GradientStripEditor::hitTestHandle(const QPoint &p) const
{
    const int hy = handleY();
    int best = -1;
    int bestDist = kHandleRadius + 4; // これより遠ければヒットなし
    for (int i = 0; i < stops_.size(); i++) {
        const int hx = toX(stops_[i].pos);
        const int d = (int)std::lround(std::hypot(double(p.x() - hx), double(p.y() - hy)));
        if (d <= bestDist) { bestDist = d; best = i; }
    }
    return best;
}

void GradientStripEditor::sortAndKeepSelection()
{
    // 並べ替えでindexがずれるので、選択中の要素を実体で覚えてから追従させる。
    const Stop selected = (selectedIndex_ >= 0 && selectedIndex_ < stops_.size())
                            ? stops_[selectedIndex_] : Stop{ -1.0f, QColor() };
    std::stable_sort(stops_.begin(), stops_.end(),
                     [](const Stop &a, const Stop &b) { return a.pos < b.pos; });
    if (selected.pos >= 0.0f) {
        for (int i = 0; i < stops_.size(); i++) {
            if (qFuzzyCompare(stops_[i].pos + 1.0f, selected.pos + 1.0f)
                && stops_[i].color == selected.color) {
                selectedIndex_ = i;
                break;
            }
        }
    }
}

void GradientStripEditor::emitChanged()
{
    emit stopsChanged(stops_);
    emit selectionChanged(canRemoveSelected());
    update();
}

bool GradientStripEditor::canRemoveSelected() const
{
    // 最低2個は残す(グラデーションが成立しなくなるため)
    return selectedIndex_ >= 0 && selectedIndex_ < stops_.size() && stops_.size() > 2;
}

void GradientStripEditor::removeSelected()
{
    if (!canRemoveSelected()) return;
    stops_.remove(selectedIndex_);
    selectedIndex_ = -1;
    emitChanged();
}

void GradientStripEditor::openColorPickerForSelected()
{
    if (selectedIndex_ < 0 || selectedIndex_ >= stops_.size()) return;

    // QColorDialogではなくColorWheelWidgetをポップアップで出す
    // (TextLayerPanel/SolidColorPickerPanelと同じ、アプリ内の色選択UI統一のため)。
    auto *popup = new QWidget(this, Qt::Popup);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto *popupLayout = new QVBoxLayout(popup);
    popupLayout->setContentsMargins(8, 8, 8, 8);
    auto *wheel = new ColorWheelWidget(popup);
    wheel->pickColor(stops_[selectedIndex_].color);
    popupLayout->addWidget(wheel);
    connect(wheel, &ColorWheelWidget::colorChanged, this, [this](const QColor &c) {
        if (selectedIndex_ < 0 || selectedIndex_ >= stops_.size()) return;
        // グラデーションの色は不透明として扱う(輝度→色の対応表を作るだけなので、
        // ここでアルファを持たせても最終的な絵のアルファには影響しない)。
        stops_[selectedIndex_].color = QColor(c.red(), c.green(), c.blue());
        emitChanged();
    });
    popup->move(mapToGlobal(QPoint(toX(stops_[selectedIndex_].pos) - 8, handleY() + kHandleRadius + 4)));
    popup->show();
}

void GradientStripEditor::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRect band = bandRect();

    // ---- グラデーションの帯(1次元プレビュー) ----
    QLinearGradient grad(band.topLeft(), band.topRight());
    for (const Stop &s : stops_)
        grad.setColorAt(qBound(0.0, double(s.pos), 1.0), s.color);
    p.fillRect(band, grad);
    // このウィジェットはフローティングパネル(DraggablePanel)の上に載るので、
    // 文字・線の色は text ではなく panelText 系を使う。
    p.setPen(QPen(Theme::panelText, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(band.adjusted(0, 0, -1, -1));

    // ---- つまみ ----
    const int hy = handleY();
    for (int i = 0; i < stops_.size(); i++) {
        const int hx = toX(stops_[i].pos);
        const bool sel = (i == selectedIndex_);

        // 帯とつまみをつなぐ細い線(どの位置の色かを分かりやすくする)
        p.setPen(QPen(Theme::textDisabled, 1));
        p.drawLine(hx, band.bottom() + 1, hx, hy - kHandleRadius);

        // つまみ本体: 中身はそのストップの色、枠は選択中だけアクセント色で太く
        p.setBrush(stops_[i].color);
        p.setPen(sel ? QPen(Theme::accent, 2.5)
                     : QPen(Theme::panelText, 1.2));
        p.drawEllipse(QPoint(hx, hy), kHandleRadius, kHandleRadius);
    }
}

void GradientStripEditor::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;

    const int hit = hitTestHandle(event->pos());
    if (hit >= 0) {
        selectedIndex_ = hit;
        dragging_ = true;
        emit selectionChanged(canRemoveSelected());
        update();
        return;
    }

    // つまみ以外(帯の上やつまみ列の隙間)をクリック: その位置に新しいストップを足す。
    // 色はその時点のグラデーション色を引き継ぐので、追加した瞬間は見た目が変わらず、
    // そこから色を変えていく流れになる。
    const float pos = toPos(event->pos().x());
    stops_.append({ pos, sampleAt(pos) });
    selectedIndex_ = stops_.size() - 1;
    sortAndKeepSelection();
    dragging_ = true;
    emitChanged();
}

void GradientStripEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (!dragging_ || selectedIndex_ < 0 || selectedIndex_ >= stops_.size()) return;
    stops_[selectedIndex_].pos = toPos(event->pos().x());
    sortAndKeepSelection();
    emitChanged();
}

void GradientStripEditor::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    dragging_ = false;
}

void GradientStripEditor::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    const int hit = hitTestHandle(event->pos());
    if (hit < 0) return;
    selectedIndex_ = hit;
    dragging_ = false; // ダブルクリックの前段のpressでtrueになっているので降ろす
    emit selectionChanged(canRemoveSelected());
    update();
    openColorPickerForSelected();
}
