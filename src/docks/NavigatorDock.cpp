#include "docks/NavigatorDock.h"
#include "widgets/GLWidget.h"
#include "components/ThemeColors.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSlider>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QVector2D>
#include <QSignalBlocker>
#include <QTimer>

namespace {
constexpr int kZoomSliderH = 20;
constexpr int kFlipButtonH = 22;
}

// ===========================================================================
// NavigatorDock
// ===========================================================================
NavigatorDock::NavigatorDock(GLWidget *gl, QWidget *parent)
    : QWidget(parent), glWidget(gl)
{
    setMinimumSize(100, 160);

    refreshDebounceTimer_ = new QTimer(this);
    refreshDebounceTimer_->setSingleShot(true);
    connect(refreshDebounceTimer_, &QTimer::timeout, this, &NavigatorDock::refresh);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 切り替えボタン行
    QHBoxLayout *btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(2);

    btnCanvas = new QPushButton("全体", this);
    btnLayer  = new QPushButton("単体", this);

    btnCanvas->setCheckable(true);
    btnLayer ->setCheckable(true);
    btnCanvas->setChecked(true);

    btnCanvas->setFixedHeight(22);
    btnLayer ->setFixedHeight(22);

    connect(btnCanvas, &QPushButton::clicked, this, [this]() {
        m_fullCanvas = true;
        applyButtonStyles();
        updatePreview();
        update();
    });
    connect(btnLayer, &QPushButton::clicked, this, [this]() {
        m_fullCanvas = false;
        applyButtonStyles();
        updatePreview();
        update();
    });

    btnLayout->addWidget(btnCanvas);
    btnLayout->addWidget(btnLayer);
    btnLayout->addStretch();

    mainLayout->addLayout(btnLayout);
    mainLayout->addStretch(); // プレビューは paintEvent で描くので stretch で余白を確保

    QHBoxLayout *bottomLayout = new QHBoxLayout();

    // キャンバス表示倍率スライダー(10%-800%、絶対値でGLWidgetの表示倍率を変更する)
    zoomSlider = new QSlider(Qt::Horizontal, this);
    zoomSlider->setRange(10, 800);
    zoomSlider->setValue(qRound(glWidget->viewScale() * 100.0f));
    zoomSlider->setFixedHeight(kZoomSliderH);
    bottomLayout->addWidget(zoomSlider);

    // アイコンだけの正方形ボタン用の共通スタイル。style.qssのQPushButton共通ルール
    // (padding: 6px 16px; 等)がsetFixedSize()の効果を再polish時に上書きし、横長に
    // なってしまうことがある(ToolPropDockの先端画像プレビューと同じ問題)ため、
    // ウィジェット単位でpadding/min・maxサイズを明示的に固定する。
    const QString squareIconBtnStyle = QString(
        "QPushButton { padding: 0px; min-width: %1px; max-width: %1px; "
        "min-height: %1px; max-height: %1px; }").arg(kFlipButtonH);

    // サイズ・回転のリセット(=キャンバスをフィット。編集メニューのfitActionと同じ処理)
    resetButton = new QPushButton(this);
    resetButton->setIcon(QIcon(":/icons/common/reset_canvas.png"));
    resetButton->setToolTip("表示をリセット(キャンバスをフィット)");
    resetButton->setFixedSize(kFlipButtonH, kFlipButtonH);
    resetButton->setStyleSheet(squareIconBtnStyle);
    bottomLayout->addWidget(resetButton);

    // 表示上の左右反転トグル(キャンバスのデータ自体は変更しない)
    flipButton = new QPushButton(this);
    flipButton->setIcon(QIcon(":/icons/common/flip.png"));
    flipButton->setCheckable(true);
    flipButton->setChecked(glWidget->isFlippedX());
    flipButton->setFixedSize(kFlipButtonH, kFlipButtonH);
    flipButton->setStyleSheet(squareIconBtnStyle);
    bottomLayout->addWidget(flipButton);

    mainLayout-> addLayout(bottomLayout);

    connect(zoomSlider, &QSlider::valueChanged, this, [this](int v) {
        glWidget->setViewScaleCentered(v / 100.0f);
    });
    connect(resetButton, &QPushButton::clicked, this, [this]() {
        glWidget->fitCanvasToView();
    });
    connect(flipButton, &QPushButton::toggled, this, [this](bool checked) {
        glWidget->setFlippedX(checked);
    });

    // GLWidget側でパン・ズーム・回転・反転が変わったら、表示範囲枠とスライダー値を
    // 追従させる(プレビュー画像自体は変わらないのでupdatePreview()は呼ばない)。
    viewChangedConn_ = connect(glWidget, &GLWidget::viewChanged, this, [this]() {
        const QSignalBlocker blocker(zoomSlider);
        zoomSlider->setValue(qBound(zoomSlider->minimum(),
                                     qRound(glWidget->viewScale() * 100.0f),
                                     zoomSlider->maximum()));
        update();
    });

    applyButtonStyles();
}

void NavigatorDock::setGLWidget(GLWidget *gl)
{
    QObject::disconnect(viewChangedConn_);
    glWidget = gl;
    viewChangedConn_ = connect(glWidget, &GLWidget::viewChanged, this, [this]() {
        const QSignalBlocker blocker(zoomSlider);
        zoomSlider->setValue(qBound(zoomSlider->minimum(),
                                     qRound(glWidget->viewScale() * 100.0f),
                                     zoomSlider->maximum()));
        update();
    });

    {
        const QSignalBlocker blocker(zoomSlider);
        zoomSlider->setValue(qBound(zoomSlider->minimum(),
                                     qRound(glWidget->viewScale() * 100.0f),
                                     zoomSlider->maximum()));
    }
    {
        const QSignalBlocker blocker(flipButton);
        flipButton->setChecked(glWidget->isFlippedX());
    }
    refresh();
    update();
}

void NavigatorDock::applyButtonStyles()
{
    auto activeStyle = [](bool active) {
        return active
            ? "QPushButton { background:#4a90e2; color:#fff; border:none;"
              " border-radius:3px; padding:0 8px; font-size:11px; }"
            : "QPushButton { background:#3a3a3a; color:#aaa; border:none;"
              " border-radius:3px; padding:0 8px; font-size:11px; }"
              "QPushButton:hover { background:#4a4a4a; color:#fff; }";
    };
    btnCanvas->setStyleSheet(activeStyle(m_fullCanvas));
    btnLayer ->setStyleSheet(activeStyle(!m_fullCanvas));
}

int NavigatorDock::previewTopOffset() const
{
    return 22 + 4 + 4 + 4; // ボタン行 + spacing + 上マージン + 余白(概算。実配置はpreviewAreaRect()参照)
}

int NavigatorDock::previewBottomHeight() const
{
    return kZoomSliderH + 4 + kFlipButtonH + 4 + 4; // スライダー + spacing + 反転ボタン + spacing + 下マージン(概算)
}

QRect NavigatorDock::previewAreaRect() const
{
    // ボタン行(btnCanvas)の下端〜下部コントロール行(zoomSlider)の上端を、実際の
    // ウィジェット配置(QLayoutが確定させた実ジオメトリ)から求める。手計算の
    // previewTopOffset()/previewBottomHeight()だけに頼ると、実際のレイアウト結果と
    // 微妙にズレてスライダー/ボタンにプレビューが重なって見えることがあるため、
    // こちらを正としてプレビュー領域を決める。
    int top    = previewTopOffset();
    int bottom = height() - previewBottomHeight();
    if (btnCanvas && btnCanvas->isVisible())
        top = btnCanvas->geometry().bottom() + 1 + 4; // ボタン行の下端 + 余白
    if (zoomSlider && zoomSlider->isVisible())
        bottom = zoomSlider->geometry().top() - 4; // 下部コントロール行の上端 - 余白
    return QRect(4, top, width() - 8, qMax(0, bottom - top));
}

QRect NavigatorDock::previewDstRect() const
{
    if (m_preview.isNull()) return QRect();
    const QRect area = previewAreaRect();
    if (area.width() <= 0 || area.height() <= 0) return QRect();

    QSize imgSize = m_preview.size().scaled(area.size(), Qt::KeepAspectRatio);
    return QRect(
        area.x() + (area.width()  - imgSize.width())  / 2,
        area.y() + (area.height() - imgSize.height()) / 2,
        imgSize.width(),
        imgSize.height()
    );
}

void NavigatorDock::updatePreview()
{
    // タブが1枚も無い間(ダミーのdeckGLWidget_を指している間)はGLコンテキストが
    // 一度も初期化されていないため、makeCurrent()を伴うgetNavigatorPreview()を
    // 呼ぶとクラッシュする。キャンバスサイズも0のままなので、その場合は空表示にする。
    if (!glWidget->isGLReady()) {
        m_preview = QImage();
        return;
    }

    const QRect area = previewAreaRect();
    int previewW = area.width();
    int previewH = area.height();
    if (previewW <= 0 || previewH <= 0) return;

    // キャンバスのアスペクト比に合わせて収まる最大サイズを渡す
    int canvasW = glWidget->canvasWidth();
    int canvasH = glWidget->canvasHeight();
    if (canvasW <= 0 || canvasH <= 0) { m_preview = QImage(); return; }

    float scaleX = (float)previewW / canvasW;
    float scaleY = (float)previewH / canvasH;
    float scale  = qMin(scaleX, scaleY);

    int sz = qMax(1, (int)(qMax(canvasW, canvasH) * scale));
    m_preview = glWidget->getNavigatorPreview(sz, m_fullCanvas);
}

void NavigatorDock::scheduleRefresh()
{
    // refresh()はフルキャンバス合成(updateCompositedTex)+glReadPixelsを伴うため、
    // layersChanged()の連続発火(ストローク確定の連打、スライダードラッグ等)に
    // そのまま反応すると重い。一定時間発火が止まってから最後の1回だけ実行する。
    refreshDebounceTimer_->start(80);
}

void NavigatorDock::refresh()
{
    // ストローク中/ペンのホバー中(=次のペンダウンが目前)は、フルキャンバス合成+
    // 同期glReadPixelsを伴うプレビュー再生成を先送りする。文字書きのように
    // ストロークを連打していると、デバウンス明けのこの処理がちょうど次の
    // ペンダウンの直前・最中に走ってイベントループを塞ぎ、「レイヤー数が多いと
    // 書き始めが遅い」症状の主因になっていた(合成コストはレイヤー数に比例)。
    if (glWidget && glWidget->isGLReady() && glWidget->isInkingBusy()) {
        refreshDebounceTimer_->start(150);
        return;
    }
    updatePreview();
    update();
}

void NavigatorDock::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updatePreview();
}

void NavigatorDock::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // 全体は他のドックと揃えたUI背景(bgBase)。プレビュー欄はそれより明るい
    // bgPanel。ただし明るい背景を「上端〜プレビュー領域の下端」までに限定して
    // 塗ることで、下の操作行(スライダー/リセット・反転ボタン)には被らないよう
    // にする。上の全体/単体切り替えボタンは従来通りこの明るい背景の上に重ねる
    // (=上側は制限しない)。
    p.fillRect(rect(), Theme::bgBase);
    const QRect area = previewAreaRect();
    // 明るい背景/クリップ領域: 上端(0)からプレビュー領域の下端まで。上のボタン行を
    // 含む一方、下の操作行の直前(area.bottom())で止まる。
    const QRect brightRegion(0, 0, width(), area.isEmpty() ? 0 : area.bottom() + 1);
    if (!brightRegion.isEmpty())
        p.fillRect(brightRegion, Theme::bgPanel);

    if (m_preview.isNull()) return;

    // ボタン行/下部コントロール行を除いた領域の中央にプレビューを表示
    const QRect dst = previewDstRect();
    if (dst.isEmpty()) return;

    // ズームアウト時など、表示範囲枠(下で描く赤枠)がdstの外まではみ出ることが
    // あるため、下の操作行(スライダー/ボタン)にだけ重ならないよう、下端のみ
    // クリップする(上側はボタン行に重ねる従来仕様なので制限しない)。
    p.setClipRect(brightRegion);

    // 市松模様の背景(透明部分の視覚化)。先端画像プレビュー・レイヤープレビュー・
    // キャンバス背景と共通のTheme::checkerDark/Lightを使う。
    const int cs = 6;
    for (int cy = dst.top(); cy < dst.bottom(); cy += cs)
        for (int cx = dst.left(); cx < dst.right(); cx += cs)
            p.fillRect(cx, cy,
                       qMin(cs, dst.right()  - cx),
                       qMin(cs, dst.bottom() - cy),
                       ((cx/cs + cy/cs) % 2 == 0) ? Theme::checkerDark : Theme::checkerLight);

    p.drawImage(dst, m_preview);

    // 枠
    p.setPen(QPen(Theme::hoverBg, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(dst);

    // 現在GLWidgetに表示されている範囲を示す枠(パン・ズーム・回転・左右反転を
    // すべて反映)。プレビュー画像自体はcanvasWidth()xcanvasHeight()に対応するので、
    // キャンバスピクセル座標をそのままdstの比率でプレビュー内座標へ写像するだけでよい。
    const int cw = glWidget->canvasWidth(), ch = glWidget->canvasHeight();
    if (cw > 0 && ch > 0) {
        const QPolygonF canvasPoly = glWidget->visibleCanvasRectPolygon(); // キャンバスピクセル座標(Y下向き)
        QPolygonF screenPoly;
        for (const QPointF &pt : canvasPoly) {
            const double u = pt.x() / cw;
            const double v = pt.y() / ch;
            screenPoly << QPointF(dst.x() + u * dst.width(), dst.y() + v * dst.height());
        }
        p.setPen(QPen(QColor(220, 40, 40), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(screenPoly);
    }
}

void NavigatorDock::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    const QRect dst = previewDstRect();
    if (dst.isEmpty() || !dst.contains(event->pos())) return;
    m_draggingFrame = true;
    m_lastDragPos = event->pos();
}

void NavigatorDock::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_draggingFrame) return;
    const QRect dst = previewDstRect();
    const int cw = glWidget->canvasWidth(), ch = glWidget->canvasHeight();
    if (dst.isEmpty() || cw <= 0 || ch <= 0) return;

    const QPoint delta = event->pos() - m_lastDragPos;
    m_lastDragPos = event->pos();
    if (delta.isNull()) return;

    // プレビュー内の移動量(ウィジェットpx)をキャンバスピクセル座標系(Y下向き)の
    // 移動量に換算する(dstはキャンバス全体をcw x chのアスペクト比で縮小した矩形)。
    const float scale = (float)cw / dst.width();
    const QVector2D canvasDeltaYDown(delta.x() * scale, delta.y() * scale);

    // 枠を動かした方向とは逆方向にビューをパンする(移動ツールでキャンバスを
    // ドラッグする時は内容がドラッグ方向へ動くが、ナビゲーターの枠をドラッグする
    // ときは「もっとそちら側を見たい」という操作なので、内容は逆方向に動く)。
    glWidget->panByCanvasDelta(-canvasDeltaYDown);
}

void NavigatorDock::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    m_draggingFrame = false;
}
