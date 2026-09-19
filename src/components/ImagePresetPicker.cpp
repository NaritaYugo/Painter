#include "components/ImagePresetPicker.h"
#include "components/ThemeColors.h"

#include <QListWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>

namespace {
constexpr int kThumb = 56;   // サムネイルの一辺(px)
constexpr int kCols  = 4;

// 透過部分が背景に埋もれないよう、市松模様の上に描いたサムネイルを作る
// (ツール設定ドックのプレビューボタンと同じ考え方)。
// crop が真なら縮小せず中央を等倍で切り出す(ImagePresetPicker::ThumbMode 参照)。
QPixmap makeThumb(const QString &path, bool crop)
{
    QPixmap out(kThumb, kThumb);
    out.fill(Qt::transparent);

    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, false);
    const int checker = 7;
    for (int y = 0; y < kThumb; y += checker) {
        for (int x = 0; x < kThumb; x += checker) {
            const bool dark = ((x / checker) + (y / checker)) % 2 == 0;
            p.fillRect(QRect(x, y, checker, checker).intersected(QRect(0, 0, kThumb, kThumb)),
                       dark ? Theme::checkerDark : Theme::checkerLight);
        }
    }

    const QPixmap src(path);
    if (!src.isNull()) {
        if (crop && src.width() >= kThumb && src.height() >= kThumb) {
            p.drawPixmap(0, 0, src, (src.width() - kThumb) / 2, (src.height() - kThumb) / 2,
                         kThumb, kThumb);
        } else {
            const QPixmap scaled = src.scaled(QSize(kThumb, kThumb), Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation);
            p.drawPixmap((kThumb - scaled.width()) / 2, (kThumb - scaled.height()) / 2, scaled);
        }
    }
    return out;
}

// 「なし」の項目に使う、斜線を引いた空のサムネイル
QPixmap makeNoneThumb()
{
    QPixmap out(kThumb, kThumb);
    out.fill(QColor(0x2b, 0x2b, 0x2b));
    QPainter p(&out);
    p.setPen(QPen(QColor(0x77, 0x77, 0x77), 1));
    p.drawRect(0, 0, kThumb - 1, kThumb - 1);
    p.drawLine(0, kThumb - 1, kThumb - 1, 0);
    return out;
}
} // namespace

ImagePresetPicker::ImagePresetPicker(const QString &title, const ImagePresetList &presets,
                                     const QString &currentPath, ThumbMode thumbMode,
                                     const QString &noneLabel, QWidget *parent)
    : QDialog(parent), title_(title), thumbMode_(thumbMode)
{
    setWindowTitle(title);

    auto *root = new QVBoxLayout(this);

    grid_ = new QListWidget(this);
    grid_->setViewMode(QListView::IconMode);
    grid_->setIconSize(QSize(kThumb, kThumb));
    grid_->setGridSize(QSize(kThumb + 26, kThumb + 30));
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setSelectionMode(QAbstractItemView::SingleSelection);
    grid_->setWordWrap(true);
    grid_->setUniformItemSizes(true);
    grid_->setFixedWidth((kThumb + 26) * kCols + 28); // 4列ぶん + スクロールバー/余白
    // 同梱ぶんが一度に見える高さにする。将来増やしても際限なく縦長にならないよう
    // 4行で頭打ちにして、それ以上はスクロールさせる。
    {
        const int count = presets.size() + (noneLabel.isEmpty() ? 0 : 1);
        const int rows  = qBound(1, (count + kCols - 1) / kCols, 4);
        grid_->setFixedHeight((kThumb + 30) * rows + 14);
    }
    root->addWidget(grid_);

    const bool crop = (thumbMode == ThumbMode::Crop);

    auto addItem = [this](const QPixmap &thumb, const QString &label, const QString &path,
                          bool select) {
        auto *item = new QListWidgetItem(QIcon(thumb), label, grid_);
        item->setData(Qt::UserRole, path);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        if (select) grid_->setCurrentItem(item);
    };

    if (!noneLabel.isEmpty())
        addItem(makeNoneThumb(), noneLabel, QString(), currentPath.isEmpty());

    for (const ImagePreset &e : presets)
        addItem(makeThumb(e.path, crop), e.label, e.path, e.path == currentPath);

    // 同梱プリセット以外(ユーザーが読み込んだファイル)が現在の選択なら、
    // 末尾に1つだけ足して選択状態にしておく(開いた瞬間に選択が消えないように)。
    if (!currentPath.isEmpty() && grid_->currentItem() == nullptr)
        addItem(makeThumb(currentPath, crop), imagePresetLabel(presets, currentPath),
                currentPath, true);

    // ダブルクリックで即決定できるようにする(グリッドUIとして自然なので)
    connect(grid_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        selected_ = item->data(Qt::UserRole).toString();
        accept();
    });

    auto *browseBtn = new QPushButton(QStringLiteral("ファイルから選択..."), this);
    connect(browseBtn, &QPushButton::clicked, this, &ImagePresetPicker::browseFile);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (auto *item = grid_->currentItem())
            selected_ = item->data(Qt::UserRole).toString();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *bottom = new QHBoxLayout();
    bottom->addWidget(browseBtn);
    bottom->addStretch();
    bottom->addWidget(buttons);
    root->addLayout(bottom);

    adjustSize();
}

void ImagePresetPicker::browseFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, title_, QString(),
        QStringLiteral("画像ファイル (*.png *.jpg *.jpeg *.bmp);;すべてのファイル (*)"));
    if (path.isEmpty()) return;

    if (QPixmap(path).isNull()) {
        QMessageBox::warning(this, QStringLiteral("読み込み失敗"),
                             QStringLiteral("画像を読み込めませんでした。"));
        return;
    }
    selected_ = path;
    accept();
}
