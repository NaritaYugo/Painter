#include "dialogs/NewCanvasDialog.h"
#include "dialogs/SettingsDialog.h"
#include "components/MultiColumnDelegate.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QComboBox>
#include <QTableView>
#include <QApplication>
#include <QPainter>
#include <QMouseEvent>
#include <QCheckBox>
#include <QTimer>
#include <QScreen>
#include <QGuiApplication>
#include <cmath>
#include <functional>
#include <utility>
#include <QPushButton>

// ===========================================================================
// サイズプレビューを表示するためのカスタムウィジェット
//
// 左上を原点に固定し、右下の点をドラッグすることでピクセルサイズ(W/H)を
// 決められるグラフ状のウィジェット。X軸=幅、Y軸=高さは常に同じ線形スケール
// (m_currentMax pxが正方形の一辺)を共有するため、実際のキャンバス縦横比が
// そのままプレビューの見た目に反映される。ドラッグで表示レンジの端に
// 近づくと自動でスケール(m_currentMax)が伸縮し、常に使いやすい精度で
// 1px〜kAbsMax pxまで選べるようにする。ドラッグ位置は常にcommonSizes()内の
// よく使う値へスナップする。
// ===========================================================================
class CanvasPreviewWidget : public QWidget
{
public:
    explicit CanvasPreviewWidget(QWidget *parent = nullptr)
        : QWidget(parent), m_w(1), m_h(1)
    {
        setMinimumSize(300, 300);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setCursor(Qt::SizeFDiagCursor);
        setMouseTracking(true);
    }

    // 常に正方形を維持する(横幅が決まれば高さもそれに合わせる)
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w; }

    void updateSize(int w, int h)
    {
        if (m_w != w || m_h != h) {
            m_w = w;
            m_h = h;
            rescaleIfNeeded(qMax(w, h));
            update();
        }
    }

    // 左右/上下にループする設定の場合、該当する辺を赤く強調表示する
    void setWrapX(bool wrap) { if (m_wrapX != wrap) { m_wrapX = wrap; update(); } }
    void setWrapY(bool wrap) { if (m_wrapY != wrap) { m_wrapY = wrap; update(); } }

    // ドラッグでスナップされた値が確定するたびに呼ばれる(連続的に呼ばれる)
    std::function<void(int, int)> onSizeDragged;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRect plot = plotRect();

        // スナップ候補のグリッド線(薄いガイド)。現在の表示レンジ内に収まるものだけ描く。
        // ルーラーに数値が出ている(=majorTicks()に含まれる)位置は濃く(白く)強調する。
        for (int v : commonSizes()) {
            if (v > m_currentMax || majorTicks().contains(v)) continue;
            painter.setPen(QPen(QColor(75, 75, 75), 1));
            double t = valueToT(v);
            int x = plot.left() + qRound(t * plot.width());
            int y = plot.top()  + qRound(t * plot.height());
            painter.drawLine(x, plot.top(), x, plot.bottom());
            painter.drawLine(plot.left(), y, plot.right(), y);
        }

        for (int v : commonSizes()) {
            if (v > m_currentMax || !majorTicks().contains(v)) continue;
            painter.setPen(QPen(QColor(100, 100, 100), 1));
            double t = valueToT(v);
            int x = plot.left() + qRound(t * plot.width());
            int y = plot.top()  + qRound(t * plot.height());
            painter.drawLine(x, plot.top(), x, plot.bottom());
            painter.drawLine(plot.left(), y, plot.right(), y);
        }

        painter.setPen(QPen(QColor(100, 100, 100), 1, Qt::DashLine));
        painter.drawRect(plot);

        // ルーラー(主要目盛りの数値ラベル): 上=幅(px)、左=高さ(px)
        QFont rulerFont = painter.font();
        rulerFont.setPointSize(qMax(7, rulerFont.pointSize() - 2));
        painter.setFont(rulerFont);
        painter.setPen(QColor(180, 180, 180));
        const QFontMetrics fm(rulerFont);
        for (int v : majorTicks()) {
            if (v > m_currentMax) continue;
            double t = valueToT(v);
            const QString label = QString::number(v);

            int x = plot.left() + qRound(t * plot.width());
            QRect topLabelRect(x - 20, 2, 40, rulerTopH_ - 4);
            painter.drawText(topLabelRect, Qt::AlignHCenter | Qt::AlignBottom, label);
            painter.drawLine(x, rulerTopH_ - 3, x, rulerTopH_);

            int y = plot.top() + qRound(t * plot.height());
            QRect leftLabelRect(0, y - fm.height() / 2, rulerLeftW_ - 4, fm.height());
            painter.drawText(leftLabelRect, Qt::AlignRight | Qt::AlignVCenter, label);
            painter.drawLine(rulerLeftW_ - 3, y, rulerLeftW_, y);
        }

        if (m_w <= 0 || m_h <= 0) return;

        const QPoint origin = plot.topLeft();
        const QPoint corner = valueToPoint(m_w, m_h);
        QRect canvasRect(origin, corner);
        canvasRect = canvasRect.normalized();
        if (canvasRect.width()  < 1) canvasRect.setWidth(1);
        if (canvasRect.height() < 1) canvasRect.setHeight(1);

        painter.fillRect(canvasRect, Qt::white);
        painter.setPen(QPen(QColor(160, 160, 160), 1));
        painter.drawRect(canvasRect);

        // ループ設定されている辺を赤く強調表示する
        if (m_wrapX || m_wrapY) {
            QPen wrapPen(QColor(220, 40, 40), 3);
            painter.setPen(wrapPen);
            if (m_wrapX) {
                painter.drawLine(canvasRect.topLeft(), canvasRect.bottomLeft());
                painter.drawLine(canvasRect.topRight(), canvasRect.bottomRight());
            }
            if (m_wrapY) {
                painter.drawLine(canvasRect.topLeft(), canvasRect.topRight());
                painter.drawLine(canvasRect.bottomLeft(), canvasRect.bottomRight());
            }
        }

        // ドラッグ用ハンドル(右下角)
        const int handleR = 5;
        painter.setBrush(QColor(240, 240, 240));
        painter.setPen(QPen(QColor(90, 90, 90), 1));
        painter.drawEllipse(corner, handleR, handleR);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        m_dragging = true;
        updateFromMouse(event->pos());
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragging)
            updateFromMouse(event->pos());
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        Q_UNUSED(event);
        m_dragging = false;
    }

private:
    static constexpr double kAbsMax   = 10000.0; // 選択できる最大値
    static constexpr double kMinRange = 64.0;    // 表示レンジの最小値(これ以上は縮小しない)

    int    m_w;
    int    m_h;
    bool   m_dragging = false;
    bool   m_wrapX = false;
    bool   m_wrapY = false;
    double m_currentMax = 2000.0; // 現在の表示レンジ(プレビュー正方形の一辺に相当するpx値)
    double m_targetMax  = 2000.0; // rescaleIfNeeded()が決める、アニメーションの目標レンジ
    QTimer *m_rescaleTimer = nullptr;

    static const QVector<int> &commonSizes()
    {
        static const QVector<int> sizes = {
            1, 2, 4, 8, 16, 24, 32, 48, 64, 96, 128, 150, 192, 256, 300, 384,
            500, 512, 600, 640, 720, 768, 800, 900, 1000, 1024, 1080, 1200,
            1280, 1366, 1440, 1500, 1600, 1920, 2000, 2048, 2160, 2400, 2560,
            3000, 3200, 3840, 4000, 4096, 4500, 5000, 6000, 7000, 8000, 9000,
            10000
        };
        return sizes;
    }

    // ルーラーに数値を表示する主要目盛り(commonSizes()の一部、密集しすぎないよう間引いたもの)
    static const QVector<int> &majorTicks()
    {
        static const QVector<int> ticks = {
            32, 64, 128, 256, 512, 1080, 1600, 1920, 2560, 3840, 5000, 10000
        };
        return ticks;
    }

    static constexpr int rulerLeftW_ = 40;
    static constexpr int rulerTopH_  = 20;

    // プレビューは常に正方形(=X軸/Y軸のスケールが揃う=ドラッグ時の縦横比が
    // 見た目通りになる)にする。左と上はルーラー数値表示用の余白として確保する。
    QRect plotRect() const
    {
        QRect area = rect().adjusted(rulerLeftW_, rulerTopH_, -15, -15);
        int side = qMax(1, qMin(area.width(), area.height()));
        return QRect(area.left(), area.top(), side, side);
    }

    // 線形スケール: X軸(幅)/Y軸(高さ)とも同じm_currentMaxを共有するため、
    // 実際のW:H比がそのままプレビュー上の見た目の比率になる。
    double valueToT(double v) const
    {
        return qBound(0.0, v / m_currentMax, 1.0);
    }

    double tToValue(double t) const
    {
        return qBound(0.0, t, 1.0) * m_currentMax;
    }

    static int snapToNearest(double rawValue, const QVector<int> &table)
    {
        int best = table.first();
        double bestDist = std::abs(rawValue - best);
        for (int v : table) {
            double d = std::abs(rawValue - v);
            if (d < bestDist) { bestDist = d; best = v; }
        }
        return best;
    }

    // ドラッグ位置(または現在値)が表示レンジの端に近づいたら拡大し、
    // 逆に小さい値へ戻ったら精度を上げるため縮小する(kMinRangeが下限)。
    // m_currentMaxを即座に書き換えるのではなく目標値(m_targetMax)だけ更新し、
    // 実際の値はタイマーでゆっくりイージングさせる(閾値をまたいだ瞬間に
    // 最大/最小へ飛ぶのを防ぐため)。
    void rescaleIfNeeded(double farthest)
    {
        constexpr double growTrigger   = 0.85; // レンジの85%を超えたら拡大
        constexpr double growTarget    = 0.70; // 拡大後、farthestがレンジの65%に来るようにする
        constexpr double shrinkTrigger = 0.20; // レンジの35%を下回ったら縮小
        constexpr double shrinkTarget  = 0.40; // 縮小後、farthestがレンジの55%に来るようにする

        double newTarget = m_targetMax;
        if (farthest > m_currentMax * growTrigger) {
            newTarget = qBound(kMinRange, farthest / growTarget, kAbsMax);
        } else if (farthest < m_currentMax * shrinkTrigger && m_currentMax > kMinRange) {
            newTarget = qBound(kMinRange, farthest / shrinkTarget, kAbsMax);
        }

        if (!qFuzzyCompare(newTarget, m_targetMax)) {
            m_targetMax = newTarget;
            startRescaleAnimation();
        }
    }

    void startRescaleAnimation()
    {
        if (!m_rescaleTimer) {
            m_rescaleTimer = new QTimer(this);
            m_rescaleTimer->setInterval(16);
            connect(m_rescaleTimer, &QTimer::timeout, this, [this]() {
                const double diff = m_targetMax - m_currentMax;
                if (std::abs(diff) < 0.5) {
                    m_currentMax = m_targetMax;
                    m_rescaleTimer->stop();
                } else {
                    m_currentMax += diff * 0.05; // イージング係数(小さいほどゆっくり)
                }
                update();
            });
        }
        if (!m_rescaleTimer->isActive())
            m_rescaleTimer->start();
    }

    QPoint valueToPoint(int w, int h) const
    {
        const QRect plot = plotRect();
        int x = plot.left() + qRound(valueToT(w) * plot.width());
        int y = plot.top()  + qRound(valueToT(h) * plot.height());
        return QPoint(x, y);
    }

    void updateFromMouse(const QPoint &pos)
    {
        const QRect plot = plotRect();
        if (plot.width() <= 0 || plot.height() <= 0) return;

        // 現在のスケールでマウス位置が指している生の値(現在のm_currentMax基準)
        double tx = double(pos.x() - plot.left()) / plot.width();
        double ty = double(pos.y() - plot.top())  / plot.height();
        double rawW = qBound(0.0, tToValue(tx), kAbsMax);
        double rawH = qBound(0.0, tToValue(ty), kAbsMax);
        rawW = qMax(1.0, rawW);
        rawH = qMax(1.0, rawH);

        rescaleIfNeeded(qMax(rawW, rawH));

        int newW = snapToNearest(rawW, commonSizes());
        int newH = snapToNearest(rawH, commonSizes());

        if (newW != m_w || newH != m_h) {
            m_w = newW;
            m_h = newH;
            update();
            if (onSizeDragged) onSizeDragged(m_w, m_h);
        } else {
            update(); // スケールだけ変わった場合も再描画する
        }
    }
};


// ===========================================================================
// コンストラクタ
// ===========================================================================
NewCanvasDialog::NewCanvasDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("新規作成");

    QVBoxLayout *main = new QVBoxLayout(this);

    QHBoxLayout *content = new QHBoxLayout();

    m_previewWidget = new CanvasPreviewWidget(this);
    content->addWidget(m_previewWidget, /*stretch=*/1);

    QVBoxLayout *rightCol = new QVBoxLayout();
    content->addLayout(rightCol, /*stretch=*/1);

    QGridLayout *grid = new QGridLayout(); {
        // 列: 0=ラベル, 1=プリセット, 2=幅, 3=高さ, 4=単位(印刷サイズのみ)

        // 0行目: 列ヘッダー(幅/高さ)
        QLabel *wHeader = new QLabel("幅");{ wHeader->setAlignment(Qt::AlignCenter); }
        QLabel *hHeader = new QLabel("高さ");{ hHeader->setAlignment(Qt::AlignCenter); }
        grid->addWidget(wHeader, 0, 2);
        grid->addWidget(hHeader, 0, 3);

        // 1行目: 解像度(dpi)
        grid->addWidget(new QLabel("解像度:"), 1, 0);

        m_dpiCb = new QComboBox(); {
            auto* delegate = new MultiColumnDelegate(4, m_dpiCb); {
                delegate->setAlignment( 0, Qt::AlignLeft, 58);
                delegate->setAlignment( 1, Qt::AlignCenter, 3);
                delegate->setAlignment( 2, Qt::AlignRight, 19);
                delegate->setAlignment( 3, Qt::AlignLeft, 17);
            }
            m_dpiCb->setItemDelegate(delegate);

            auto addAlignItem = [&](QString name, int dpi) {
                m_dpiCb->addItem(name);
                int row = m_dpiCb->count() - 1;
                m_dpiCb->setItemData(row, name, Qt::UserRole + 1);
                m_dpiCb->setItemData(row, QString("|"), Qt::UserRole + 2);
                m_dpiCb->setItemData(row, QString::number(dpi), Qt::UserRole + 3);
                m_dpiCb->setItemData(row, QString("dpi"), Qt::UserRole + 4);
            };

            m_dpiCb->addItem("カスタム");
            addAlignItem("Web", 72);
            addAlignItem("Web", 144);
            addAlignItem("カラー印刷", 300);
            addAlignItem("カラー印刷", 350);
            addAlignItem("線画印刷", 600);
            m_dpiCb->setFixedWidth(160);
        }
        grid->addWidget(m_dpiCb, 1, 1);

        m_dpiWSpin = new QSpinBox();
        m_dpiWSpin->setRange(1, 4800);
        grid->addWidget(m_dpiWSpin, 1, 2);

        m_dpiHSpin = new QSpinBox();
        m_dpiHSpin->setRange(1, 4800);
        grid->addWidget(m_dpiHSpin, 1, 3);

        grid->addWidget(new QLabel("dpi"), 1, 4);

        // 2行目: ×区切り
        QLabel *mul1 = new QLabel("×");{
            mul1->setAlignment(Qt::AlignCenter);
            mul1->setStyleSheet("font-size: 15px;");
        }
        grid->addWidget(mul1, 2, 2);

        QLabel *mul2 = new QLabel("×");{
            mul2->setAlignment(Qt::AlignCenter);
            mul2->setStyleSheet("font-size: 15px;");
        }
        grid->addWidget(mul2, 2, 3);

        // 3行目: 印刷サイズ
        grid->addWidget(new QLabel("印刷サイズ:"), 3, 0);

        m_printCb = new QComboBox(); {
            auto* delegate = new MultiColumnDelegate(6, m_printCb); {
                delegate->setAlignment( 0, Qt::AlignLeft, 42);
                delegate->setAlignment( 1, Qt::AlignCenter, 3);
                delegate->setAlignment( 2, Qt::AlignRight, 23);
                delegate->setAlignment( 3, Qt::AlignCenter, 8);
                delegate->setAlignment( 4, Qt::AlignRight, 21);
                delegate->setAlignment( 5, Qt::AlignLeft, 21);
            }
            m_printCb->setItemDelegate(delegate);
            m_printCb->setFixedWidth(160);
        }
        grid->addWidget(m_printCb, 3, 1);

        m_printWSpin = new QDoubleSpinBox();
        m_printWSpin->setRange(0.1, 9999.0);
        m_printWSpin->setDecimals(2);
        grid->addWidget(m_printWSpin, 3, 2);

        m_printHSpin = new QDoubleSpinBox();
        m_printHSpin->setRange(0.1, 9999.0);
        m_printHSpin->setDecimals(2);
        grid->addWidget(m_printHSpin, 3, 3);

        m_printUnitCb = new QComboBox(); {
            m_printUnitCb->addItem("mm");
            m_printUnitCb->addItem("cm");
            m_printUnitCb->addItem("in");
        }
        grid->addWidget(m_printUnitCb, 3, 4);

        // 4行目: ↓区切り
        QLabel *arrow1 = new QLabel("↓");{
            arrow1->setAlignment(Qt::AlignCenter);
            arrow1->setStyleSheet("font-size: 15px;");
        }
        grid->addWidget(arrow1, 4, 2);

        QLabel *arrow2 = new QLabel("↓");{
            arrow2->setAlignment(Qt::AlignCenter);
            arrow2->setStyleSheet("font-size: 15px;");
        }
        grid->addWidget(arrow2, 4, 3);

        // 5行目: 画像サイズ(px)
        grid->addWidget(new QLabel("画像サイズ:"), 5, 0);

        m_pxCb = new QComboBox(); {
            auto* delegate = new MultiColumnDelegate(6, m_pxCb); {
                delegate->setAlignment( 0, Qt::AlignLeft, 37);
                delegate->setAlignment( 1, Qt::AlignCenter, 3);
                delegate->setAlignment( 2, Qt::AlignRight, 26);
                delegate->setAlignment( 3, Qt::AlignCenter, 8);
                delegate->setAlignment( 4, Qt::AlignRight, 26);
                delegate->setAlignment( 5, Qt::AlignLeft, 13);
            }
            m_pxCb->setItemDelegate(delegate);

            auto addAlignItem = [&](QString name, int w, int h) {
                m_pxCb->addItem(name);
                int row = m_pxCb->count() - 1;
                m_pxCb->setItemData(row, name, Qt::UserRole + 1);
                m_pxCb->setItemData(row, "|", Qt::UserRole + 2);
                m_pxCb->setItemData(row, QString::number(w), Qt::UserRole + 3);
                m_pxCb->setItemData(row, QString("×"), Qt::UserRole + 4);
                m_pxCb->setItemData(row, QString::number(h), Qt::UserRole + 5);
                m_pxCb->setItemData( row, "px", Qt::UserRole + 6);
            };

            m_pxCb->addItem("カスタム");
            addAlignItem("HD", 1280, 720);
            addAlignItem("フルHD", 1920, 1080);
            addAlignItem("WQHD", 2560, 1440);
            addAlignItem("4K", 3840, 2160);

            m_pxCb->setFixedWidth(160);
        }
        grid->addWidget(m_pxCb, 5, 1);

        m_pxWSpin = new QSpinBox();
        m_pxWSpin->setRange(1, 99999);
        grid->addWidget(m_pxWSpin, 5, 2);

        m_pxHSpin = new QSpinBox();
        m_pxHSpin->setRange(1, 99999);
        grid->addWidget(m_pxHSpin, 5, 3);

        grid->addWidget(new QLabel("px"), 5, 4);
    }

    rightCol->addLayout(grid);

    m_swapToggle = new QCheckBox("縦横を入れ替える");
    rightCol->addWidget(m_swapToggle);

    QHBoxLayout *wrapLayout = new QHBoxLayout(); {
        m_wrapXToggle = new QCheckBox("左右にループ");
        m_wrapYToggle = new QCheckBox("上下にループ");
        wrapLayout->addWidget(m_wrapXToggle);
        wrapLayout->addWidget(m_wrapYToggle);
        wrapLayout->addStretch();
    }
    rightCol->addLayout(wrapLayout);
    rightCol->addStretch();

    connect(m_wrapXToggle, &QCheckBox::toggled, this, [this](bool checked) {
        m_previewWidget->setWrapX(checked);
    });
    connect(m_wrapYToggle, &QCheckBox::toggled, this, [this](bool checked) {
        m_previewWidget->setWrapY(checked);
    });

    main->addLayout(content);

    // プレビューのドラッグでピクセルサイズが決まったら、ピクセル入力欄に反映する
    // (以降はrecalculate()経由でdpi/印刷サイズ/プレビュー自体も連動して更新される)
    m_previewWidget->onSizeDragged = [this](int w, int h) {
        if (m_isUpdating) return;
        m_isUpdating = true;
        if (m_pxCb->currentIndex() != 0) m_pxCb->setCurrentIndex(0);
        m_pxWSpin->setValue(w);
        m_pxHSpin->setValue(h);
        m_isUpdating = false;
        recalculate(EditSource::Pixel);
    };

    QHBoxLayout *btnBox = new QHBoxLayout(); {
        btnBox->addStretch();

        QPushButton *createBtn = new QPushButton("作成"); 
        createBtn->setFixedWidth(100);
        connect(createBtn, &QPushButton::clicked, this, &QDialog::accept);
        btnBox->addWidget(createBtn);

        btnBox->addStretch();
    }
    main->addLayout(btnBox);


    // 印刷プリセットデータの登録
    m_printPresets = {
        {"名刺", 55, 91, "mm"},
        {"L判写真", 89, 127, "mm"},
        {"はがき", 100, 148, "mm"},
        {"A5", 148, 210, "mm"},
        {"B5", 182, 257, "mm"},
        {"A4", 210, 297, "mm"},
        {"B4", 257, 364, "mm"},
        {"A3", 297, 420, "mm"},
        {"B3", 364, 515, "mm"}
    };
    updatePrintComboBox(m_printUnitCb->currentText());


    // ===========================================================================
    // シグナルとスロットの設定
    // ===========================================================================

    // ★追加：縦横入れ替えトグル操作時
    connect(m_swapToggle, &QCheckBox::toggled, this, [=](bool checked) {
        Q_UNUSED(checked);
        if (m_isUpdating) return;
        m_isUpdating = true;

        // 現在のWとHの値をスワップ（画像・解像度・印刷すべて）
        int pxW = m_pxWSpin->value();
        m_pxWSpin->setValue(m_pxHSpin->value());
        m_pxHSpin->setValue(pxW);

        int dpiW = m_dpiWSpin->value();
        m_dpiWSpin->setValue(m_dpiHSpin->value());
        m_dpiHSpin->setValue(dpiW);

        double printW = m_printWSpin->value();
        m_printWSpin->setValue(m_printHSpin->value());
        m_printHSpin->setValue(printW);

        m_isUpdating = false;

        // プレビューの更新
        if (m_previewWidget) {
            m_previewWidget->updateSize(m_pxWSpin->value(), m_pxHSpin->value());
        }
    });

    // コンボボックス（プリセット）の変更時
    connect(m_pxCb, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int index) {
        if (index == 0 || m_isUpdating) return;
        m_isUpdating = true;

        int w = m_pxCb->itemData(index, Qt::UserRole + 3).toInt();
        int h = m_pxCb->itemData(index, Qt::UserRole + 5).toInt();
        
        // ★入れ替えトグルがオンならWとHを反転させる
        if (m_swapToggle->isChecked()) {
            std::swap(w, h);
        }

        m_pxWSpin->setValue(w);
        m_pxHSpin->setValue(h);
        m_isUpdating = false;
        recalculate(EditSource::Pixel);
    });

    connect(m_dpiCb, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int index) {
        if (index == 0 || m_isUpdating) return;
        m_isUpdating = true;
        int dpi = m_dpiCb->itemData(index, Qt::UserRole + 3).toInt();
        m_dpiWSpin->setValue(dpi);
        m_dpiHSpin->setValue(dpi);
        m_isUpdating = false;
        recalculate(EditSource::Dpi);
    });

    connect(m_printCb, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int index) {
        if (index == 0 || m_isUpdating) return;
        m_isUpdating = true;

        double w = m_printCb->itemData(index, Qt::UserRole + 3).toDouble();
        double h = m_printCb->itemData(index, Qt::UserRole + 5).toDouble();

        if (m_swapToggle->isChecked()) {
            std::swap(w, h);
        }

        m_printWSpin->setValue(w);
        m_printHSpin->setValue(h);
        m_printUnitCb->setCurrentText(m_printCb->itemData(index, Qt::UserRole + 6).toString());
        m_isUpdating = false;
        recalculate(EditSource::PrintSize);
    });

    // 印刷サイズの単位変更時
    connect(m_printUnitCb, &QComboBox::currentTextChanged, this, [=](const QString& newUnit) {
        if (m_isUpdating) return;
        m_isUpdating = true;
        m_printWSpin->setValue(convertUnit(m_printWSpin->value(), m_currentUnit, newUnit));
        m_printHSpin->setValue(convertUnit(m_printHSpin->value(), m_currentUnit, newUnit));
        m_currentUnit = newUnit;
        m_isUpdating = false;

        updatePrintComboBox(newUnit);
    });

    // スピンボックスの変更時イベントの共通設定
    auto setupSpinBox = [&](QSpinBox* spin, QComboBox* combo, EditSource source) {
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int val) {
            if (m_isUpdating) return;
            
            m_isUpdating = true;
            if (combo && combo->currentIndex() != 0) {
                combo->setCurrentIndex(0);
            }
            
            if (source == EditSource::Dpi) {
                if (spin == m_dpiWSpin) m_dpiHSpin->setValue(val);
                else if (spin == m_dpiHSpin) m_dpiWSpin->setValue(val);
            }
            m_isUpdating = false;
            
            recalculate(source);
        });
    };

    auto setupDoubleSpinBox = [&](QDoubleSpinBox* spin, QComboBox* combo, EditSource source) {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double) {
            if (m_isUpdating) return;
            
            m_isUpdating = true;
            if (combo && combo->currentIndex() != 0) {
                combo->setCurrentIndex(0);
            }
            m_isUpdating = false;

            recalculate(source);
        });
    };

    setupSpinBox(m_pxWSpin, m_pxCb, EditSource::Pixel);
    setupSpinBox(m_pxHSpin, m_pxCb, EditSource::Pixel);
    setupSpinBox(m_dpiWSpin, m_dpiCb, EditSource::Dpi);
    setupSpinBox(m_dpiHSpin, m_dpiCb, EditSource::Dpi);
    setupDoubleSpinBox(m_printWSpin, m_printCb, EditSource::PrintSize);
    setupDoubleSpinBox(m_printHSpin, m_printCb, EditSource::PrintSize);

    // ===========================================================================
    // 初期値の設定(設定ダイアログの「新規キャンバス幅/高さ」を初期値として使う)
    // ===========================================================================
    const SettingsDialog::Values prefs = SettingsDialog::loadValues();
    m_isUpdating = true;
    m_pxWSpin->setValue(prefs.defaultCanvasW);
    m_pxHSpin->setValue(prefs.defaultCanvasH);
    m_dpiWSpin->setValue(72);
    m_dpiHSpin->setValue(72);
    m_printWSpin->setValue(convertUnit(prefs.defaultCanvasW / 72.0, "in", "mm"));
    m_printHSpin->setValue(convertUnit(prefs.defaultCanvasH / 72.0, "in", "mm"));
    m_swapToggle->setChecked(false);
    m_isUpdating = false;

    m_previewWidget->updateSize(m_pxWSpin->value(), m_pxHSpin->value());

    // 横幅を広く取りたいのでsizeHint()を上書きしている(下記参照)。resize()や
    // adjustSize()をここで直接呼ぶと、ネイティブウィンドウがまだ存在しない
    // 段階での複数回のジオメトリ変更がWindows側のWM_GETMINMAXINFOと競合し、
    // 「QWindowsWindow::setGeometry: Unable to set geometry ...」という警告が
    // 出ることがあるため、サイズ指定はsizeHint()に任せ、中央配置だけを
    // showEvent()で(初回表示後に一度だけ)行う。
}

void NewCanvasDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (m_centeredOnce) return;
    m_centeredOnce = true;

    if (QWidget *p = parentWidget()) {
        move(p->geometry().center() - rect().center());
    } else if (QScreen *screen = QGuiApplication::primaryScreen()) {
        move(screen->geometry().center() - rect().center());
    }
}

// ===========================================================================
// ヘルパーメソッド群
// ===========================================================================

void NewCanvasDialog::recalculate(EditSource trigger)
{
    // (recalculateの中身はそのままです)
    if (m_isUpdating) return;
    m_isUpdating = true;

    QString unit = m_printUnitCb->currentText();

    if (trigger == EditSource::Pixel || trigger == EditSource::Dpi) {
        if (m_dpiWSpin->value() > 0) {
            double inW = (double)m_pxWSpin->value() / m_dpiWSpin->value();
            m_printWSpin->setValue(fromInches(inW, unit));
        }
        if (m_dpiHSpin->value() > 0) {
            double inH = (double)m_pxHSpin->value() / m_dpiHSpin->value();
            m_printHSpin->setValue(fromInches(inH, unit));
        }
        m_printCb->setCurrentIndex(0);
    } 
    else if (trigger == EditSource::PrintSize) {
        double inW = getInches(m_printWSpin->value(), unit);
        double inH = getInches(m_printHSpin->value(), unit);
        m_pxWSpin->setValue(qRound(inW * m_dpiWSpin->value()));
        m_pxHSpin->setValue(qRound(inH * m_dpiHSpin->value()));
        m_pxCb->setCurrentIndex(0);
    }
    
    m_isUpdating = false;

    if (m_previewWidget) {
        m_previewWidget->updateSize(m_pxWSpin->value(), m_pxHSpin->value());
    }
}

void NewCanvasDialog::updatePrintComboBox(const QString& unit) {
    bool wasUpdating = m_isUpdating;
    m_isUpdating = true;

    QString lastSelectedPreset = m_printCb->currentText();

    m_printCb->clear();
    m_printCb->addItem("カスタム");

    int decimals = 1;
    if (unit == "mm") decimals = 0;
    else if (unit == "in") decimals = 2;

    for (const auto& preset : m_printPresets) {
        double convertedW = convertUnit(preset.w, preset.unit, unit);
        double convertedH = convertUnit(preset.h, preset.unit, unit);

        QString wStr = QString::number(convertedW, 'f', decimals);
        QString hStr = QString::number(convertedH, 'f', decimals);

        m_printCb->addItem(preset.name);
        int row = m_printCb->count() - 1;
        m_printCb->setItemData(row, preset.name, Qt::UserRole + 1);
        m_printCb->setItemData(row, "|", Qt::UserRole + 2);
        m_printCb->setItemData(row, wStr, Qt::UserRole + 3);
        m_printCb->setItemData(row, "×", Qt::UserRole + 4);
        m_printCb->setItemData(row, hStr, Qt::UserRole + 5);
        m_printCb->setItemData(row, unit, Qt::UserRole + 6);
    }

    int index = m_printCb->findText(lastSelectedPreset);
    if (index >= 0) {
        m_printCb->setCurrentIndex(index);
    } else {
        m_printCb->setCurrentIndex(0);
    }

    m_isUpdating = wasUpdating;
}

double NewCanvasDialog::getInches(double value, const QString& unit) {
    if (unit == "mm") return value / 25.4;
    if (unit == "cm") return value / 2.54;
    return value;
}

double NewCanvasDialog::fromInches(double inches, const QString& unit) {
    if (unit == "mm") return inches * 25.4;
    if (unit == "cm") return inches * 2.54;
    return inches;
}

double NewCanvasDialog::convertUnit(double value, const QString& from, const QString& to) {
    if (from == to) return value;
    return fromInches(getInches(value, from), to);
}

int NewCanvasDialog::canvasWidth() const
{
    return m_pxWSpin->value();
}

int NewCanvasDialog::canvasHeight() const
{
    return m_pxHSpin->value();
}

bool NewCanvasDialog::wrapX() const
{
    return m_wrapXToggle->isChecked();
}

bool NewCanvasDialog::wrapY() const
{
    return m_wrapYToggle->isChecked();
}
