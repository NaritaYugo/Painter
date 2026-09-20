#include "app/StartPage.h"
#include "document/RecentFiles.h"
#include "document/CanvasSerializer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QLabel>
#include <QFrame>
#include <QScrollArea>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QPixmap>

// ---------------------------------------------------------------------------
// RecentFileCard  ―  StartPageのプレビュー一覧の1枚分
//
// 画像(プレビュー)を上部、ファイル名を常に下寄せで表示する。画像はカードの
// 縦横比に関わらずレイヤー部分の中央に収まるよう、アスペクト比を保って
// 縮小して配置する(QToolButtonのToolButtonTextUnderIconだと、画像の縦横比に
// よってファイル名の位置がずれて見えることがあったための代替実装)。
// ---------------------------------------------------------------------------
class RecentFileCard : public QFrame
{
    Q_OBJECT
public:
    explicit RecentFileCard(const QString &path, QWidget *parent = nullptr)
        : QFrame(parent), path_(path)
    {
        setFixedSize(StartPage::CARD_W, StartPage::CARD_H);
        setCursor(Qt::PointingHandCursor);
        setToolTip(path);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 4);
        layout->setSpacing(4);

        imageLabel_ = new QLabel(this);
        imageLabel_->setAlignment(Qt::AlignCenter);
        layout->addWidget(imageLabel_, 1);

        nameLabel_ = new QLabel(this);
        nameLabel_->setAlignment(Qt::AlignHCenter | Qt::AlignBottom);
        layout->addWidget(nameLabel_, 0);

        const QFontMetrics fm(nameLabel_->font());
        nameLabel_->setText(fm.elidedText(QFileInfo(path).completeBaseName(),
                                           Qt::ElideMiddle, StartPage::CARD_W - 12));

        const QImage preview = CanvasSerializer::readPreview(path);
        if (!preview.isNull())
            originalPixmap_ = QPixmap::fromImage(preview);
    }

signals:
    void clicked(const QString &path);

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) emit clicked(path_);
        QFrame::mousePressEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QFrame::resizeEvent(event);
        updateScaledPixmap();
    }

private:
    void updateScaledPixmap()
    {
        if (originalPixmap_.isNull()) return;
        const QSize target = imageLabel_->size();
        if (target.width() <= 0 || target.height() <= 0) return;
        imageLabel_->setPixmap(originalPixmap_.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    QString path_;
    QLabel  *imageLabel_ = nullptr;
    QLabel  *nameLabel_  = nullptr;
    QPixmap  originalPixmap_;
};

// グリッドから全ウィジェットを取り外す(削除はしない。呼び出し側が別途破棄する)
static void clearGrid(QGridLayout *grid)
{
    while (grid->count() > 0) {
        QLayoutItem *item = grid->takeAt(0);
        delete item;
    }
}

// カード幅(マージン込み)から適切な列数を計算する
static int calcCols(int widgetWidth)
{
    return qMax(1, widgetWidth / StartPage::CARD_W);
}

StartPage::StartPage(QWidget *parent)
    : QWidget(parent)
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(16);

    auto *topRow = new QHBoxLayout();
    topRow->setSpacing(12);

    auto *newBtn = new QPushButton("新規作成");
    newBtn->setFixedHeight(40);
    connect(newBtn, &QPushButton::clicked, this, &StartPage::newCanvasRequested);

    auto *openBtn = new QPushButton("ファイルを開く");
    openBtn->setFixedHeight(40);
    connect(openBtn, &QPushButton::clicked, this, &StartPage::openFileRequested);

    auto *transparentBtn = new QPushButton("透過タブにする");
    transparentBtn->setFixedHeight(40);
    transparentBtn->setToolTip("このタブの表示領域ぶんだけウィンドウに穴を開けて、"
                                "後ろにあるウィンドウ(資料や動画など)をそのまま見られるようにします。\n"
                                "穴の部分はクリックも後ろのウィンドウへ抜けます。");
    connect(transparentBtn, &QPushButton::clicked, this, &StartPage::transparentTabRequested);

    topRow->addWidget(newBtn);
    topRow->addWidget(openBtn);
    topRow->addWidget(transparentBtn);
    topRow->addStretch();
    mainLayout->addLayout(topRow);

    auto *recentLabel = new QLabel("最近使ったファイル");
    mainLayout->addWidget(recentLabel);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *recentContainer = new QWidget();
    recentGrid_ = new QGridLayout(recentContainer);
    recentGrid_->setSpacing(8);
    recentGrid_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    scrollArea->setWidget(recentContainer);

    mainLayout->addWidget(scrollArea, 1);

    refresh();
}

void StartPage::refresh()
{
    clearGrid(recentGrid_);
    qDeleteAll(recentCards_);
    recentCards_.clear();

    const QStringList paths = RecentFiles::list();

    for (int i = 0; i < paths.size() && i < MAX_RECENT_SHOWN; ++i) {
        const QString &path = paths[i];

        auto *card = new RecentFileCard(path);
        connect(card, &RecentFileCard::clicked, this, &StartPage::openRecentFileRequested);

        recentCards_.append(card);
    }

    lastCols_ = -1; // 強制的に組み直す
    reflowGrid(calcCols(width()));
}

void StartPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const int cols = calcCols(event->size().width());
    if (cols == lastCols_) return;
    reflowGrid(cols);
}

void StartPage::reflowGrid(int cols)
{
    lastCols_ = cols;
    clearGrid(recentGrid_);
    for (int i = 0; i < recentCards_.size(); ++i)
        recentGrid_->addWidget(recentCards_[i], i / cols, i % cols);
}

#include "StartPage.moc"
