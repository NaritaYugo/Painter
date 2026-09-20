#include "docks/ToolPresetDock.h"
#include "canvas/CanvasWidget.h"
#include "tools/core/ToolConfig.h"
#include "tools/core/ToolRegistry.h"
#include "components/ThemeColors.h"
#include "components/ToolPreview.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QTimer>

// ---------------------------------------------------------------------------
// ToolPresetRowWidget ― ツールプリセット1件分の行(選択/改名/複製/削除)
// ---------------------------------------------------------------------------
// プレビュー画像の大きさ(論理px)。行の高さはこれに合わせて決める。
static constexpr int kPreviewW = 84;
static constexpr int kPreviewH = 30;

class ToolPresetRowWidget : public QWidget
{
    Q_OBJECT
public:
    ToolPresetRowWidget(int index, const QString &name, bool active, bool deletable,
                        const QImage &preview, QWidget *parent = nullptr)
        : QWidget(parent), index_(index)
    {
        setFixedHeight(kPreviewH + 8);
        setStyleSheet(QStringLiteral("background-color: %1; border-radius: 4px;")
                          .arg((active ? Theme::accent : Theme::bgDisabled).name()));

        auto *h = new QHBoxLayout(this);
        h->setContentsMargins(4, 2, 4, 2);
        h->setSpacing(6);

        // プレビュー。作れないツール(移動など)は空の画像が来るので、場所だけ空けて
        // 何も描かない(行の高さと名前の位置を全ツールで揃えるため)。
        auto *previewLabel = new QLabel(this);
        previewLabel->setFixedSize(kPreviewW, kPreviewH);
        previewLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        previewLabel->setStyleSheet("background: transparent;");
        if (!preview.isNull())
            previewLabel->setPixmap(QPixmap::fromImage(preview));
        h->addWidget(previewLabel);

        nameEdit_ = new QLineEdit(name, this);
        nameEdit_->setReadOnly(true);
        nameEdit_->setFrame(false);
        nameEdit_->setAttribute(Qt::WA_TransparentForMouseEvents);
        nameEdit_->setStyleSheet(QStringLiteral("background: transparent; color: %1;")
                                      .arg((active ? Theme::textBright : Theme::text).name()));
        h->addWidget(nameEdit_, 1);

        // グローバルQSSのQPushButtonはpadding:6px 16pxがあり、22x22の正方形ボタンだと
        // 文字が完全に押し出されて見えなくなるため、ここだけpaddingを打ち消す。
        const QString smallBtnStyle = "QPushButton { padding: 0px; min-height: 0px; }";

        auto *dupBtn = new QPushButton(this);
        dupBtn->setIcon(QIcon(":/icons/common/copy"));
        dupBtn->setFixedSize(22, 22);
        dupBtn->setToolTip("複製");
        dupBtn->setStyleSheet(smallBtnStyle);
        h->addWidget(dupBtn);

        auto *delBtn = new QPushButton(this);
        delBtn->setIcon(QIcon(":/icons/common/trash"));
        delBtn->setFixedSize(22, 22);
        delBtn->setToolTip("削除");
        delBtn->setEnabled(deletable);
        delBtn->setStyleSheet(smallBtnStyle);
        h->addWidget(delBtn);

        previewLabel_ = previewLabel;

        connect(dupBtn, &QPushButton::clicked, this, [this] { emit duplicateRequested(index_); });
        connect(delBtn, &QPushButton::clicked, this, [this] { emit deleteRequested(index_); });
        connect(nameEdit_, &QLineEdit::editingFinished, this, [this] {
            nameEdit_->setReadOnly(true);
            nameEdit_->setAttribute(Qt::WA_TransparentForMouseEvents);
            emit renamed(index_, nameEdit_->text());
        });
    }

    // 行を作り直さずに絵だけ差し替える(名前の編集中でも消えないように)
    void setPreview(const QImage &img)
    {
        if (!previewLabel_) return;
        if (img.isNull()) previewLabel_->clear();
        else              previewLabel_->setPixmap(QPixmap::fromImage(img));
    }

signals:
    void selected(int index);
    void renamed(int index, const QString &newName);
    void duplicateRequested(int index);
    void deleteRequested(int index);

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) emit selected(index_);
        QWidget::mousePressEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        Q_UNUSED(event);
        nameEdit_->setReadOnly(false);
        nameEdit_->setAttribute(Qt::WA_TransparentForMouseEvents, false);
        nameEdit_->setFocus();
        nameEdit_->selectAll();
    }

private:
    int index_;
    QLineEdit *nameEdit_ = nullptr;
    QLabel    *previewLabel_ = nullptr;
};

// ===========================================================================
ToolPresetDock::ToolPresetDock(CanvasWidget *gl, ToolConfig *toolCfg, QWidget *parent)
    : QWidget(parent), glWidget(gl), toolCfg_(toolCfg)
{
    auto *vLayout = new QVBoxLayout(this);
    vLayout->setContentsMargins(6, 6, 6, 6);
    vLayout->setSpacing(4);

    auto *headerRow = new QHBoxLayout();
    titleLabel = new QLabel(this);
    titleLabel->setStyleSheet("font-weight: bold;");
    headerRow->addWidget(titleLabel);
    headerRow->addStretch();

    addBtn = new QPushButton("追加", this);
    addBtn->setFixedSize(48, 24);
    addBtn->setStyleSheet("QPushButton { padding: 0px 4px; min-height: 0px; }");
    addBtn->setToolTip("現在の設定からツールプリセットを追加");
    headerRow->addWidget(addBtn);
    vLayout->addLayout(headerRow);

    scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    listContainer = new QWidget();
    listLayout = new QVBoxLayout(listContainer);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(3);
    listLayout->addStretch();
    scrollArea->setWidget(listContainer);

    vLayout->addWidget(scrollArea, 1);

    connect(addBtn, &QPushButton::clicked, this, &ToolPresetDock::addNew);

    currentType_ = glWidget->getActiveTool();
    activeToolChangedConn_ = connect(glWidget, &CanvasWidget::activeToolChanged, this, &ToolPresetDock::setCurrentTool);

    // 設定変更の見張り(理由はヘッダのコメント)。値の比較だけなので負荷は無視できる。
    previewWatchTimer_ = new QTimer(this);
    connect(previewWatchTimer_, &QTimer::timeout, this, &ToolPresetDock::updateActivePreview);
    previewWatchTimer_->start(400);

    rebuildRows();
}

void ToolPresetDock::setCurrentTool(ToolType tool)
{
    currentType_ = tool;
    rebuildRows();
}

void ToolPresetDock::setCanvasWidget(CanvasWidget *gl)
{
    QObject::disconnect(activeToolChangedConn_);
    glWidget = gl;
    activeToolChangedConn_ = connect(glWidget, &CanvasWidget::activeToolChanged, this, &ToolPresetDock::setCurrentTool);
    setCurrentTool(glWidget->getActiveTool());
}

void ToolPresetDock::refresh()
{
    rebuildRows();
}

void ToolPresetDock::rebuildRows()
{
    for (auto *r : rows_) { listLayout->removeWidget(r); r->deleteLater(); }
    rows_.clear();

    titleLabel->setText(toolTypeLabel(currentType_));

    IToolPresetList *list = toolCfg_->toolPresetList(currentType_);
    if (!list) return;

    const int active = list->activeIndex();
    const bool deletable = list->count() > 1;

    for (int i = 0; i < list->count(); ++i) {
        auto *row = new ToolPresetRowWidget(i, list->name(i), i == active, deletable,
                                            renderPreviewFor(i), listContainer);
        connect(row, &ToolPresetRowWidget::selected,          this, &ToolPresetDock::selectIndex);
        connect(row, &ToolPresetRowWidget::renamed,            this, &ToolPresetDock::renameIndex);
        connect(row, &ToolPresetRowWidget::duplicateRequested, this, &ToolPresetDock::duplicateIndex);
        connect(row, &ToolPresetRowWidget::deleteRequested,    this, &ToolPresetDock::deleteIndex);
        listLayout->insertWidget(i, row);
        rows_.append(row);
    }

    // 見張りの基準を今の状態に合わせ直す(作り直した直後は当然「変化なし」)
    watchedValues_         = list->valuesAt(active);
    watchedInk_            = toolCfg_->color().rawRGBA();
    watchedInkTransparent_ = toolCfg_->color().isTransparent();
}

// index番目のツールプリセットのプレビュー画像。プレビューを作れないツールでは
// null画像が返り、行側は場所だけ空けて何も描かない。
QImage ToolPresetDock::renderPreviewFor(int index) const
{
    IToolPresetList *list = toolCfg_->toolPresetList(currentType_);
    if (!list) return {};
    // 高DPIでも滲まないよう実ピクセル数で作り、devicePixelRatioを付けて返す。
    const qreal dpr = devicePixelRatioF();
    QImage img = ToolPreview::render(currentType_, list->valuesAt(index),
                                     QSize(qRound(kPreviewW * dpr), qRound(kPreviewH * dpr)),
                                     toolCfg_->color().rawRGBA(),
                                     toolCfg_->color().isTransparent());
    if (!img.isNull()) img.setDevicePixelRatio(dpr);
    return img;
}

// アクティブなツールプリセットの設定か描画色が変わっていたら、その行の絵だけ描き直す。
void ToolPresetDock::updateActivePreview()
{
    IToolPresetList *list = toolCfg_->toolPresetList(currentType_);
    if (!list) return;
    const int active = list->activeIndex();
    if (active < 0 || active >= rows_.size()) return;

    const QVariantMap values = list->valuesAt(active);
    const QColor ink         = toolCfg_->color().rawRGBA();
    const bool inkTransp     = toolCfg_->color().isTransparent();
    if (values == watchedValues_ && ink == watchedInk_ && inkTransp == watchedInkTransparent_)
        return;

    watchedValues_         = values;
    watchedInk_            = ink;
    watchedInkTransparent_ = inkTransp;
    rows_[active]->setPreview(renderPreviewFor(active));
}

void ToolPresetDock::selectIndex(int index)
{
    glWidget->setActiveToolPreset(currentType_, index);
    rebuildRows();
}

void ToolPresetDock::renameIndex(int index, const QString &name)
{
    IToolPresetList *list = toolCfg_->toolPresetList(currentType_);
    if (!list) return;
    const QString trimmed = name.trimmed();
    if (!trimmed.isEmpty())
        list->setName(index, trimmed);
    rebuildRows();
}

void ToolPresetDock::duplicateIndex(int index)
{
    IToolPresetList *list = toolCfg_->toolPresetList(currentType_);
    if (!list) return;
    const int newIndex = list->duplicate(index);
    if (newIndex < 0) return;
    glWidget->setActiveToolPreset(currentType_, newIndex);
    rebuildRows();
}

void ToolPresetDock::deleteIndex(int index)
{
    IToolPresetList *list = toolCfg_->toolPresetList(currentType_);
    if (!list) return;
    if (!list->remove(index)) return;
    glWidget->setActiveToolPreset(currentType_, list->activeIndex());
    rebuildRows();
}

void ToolPresetDock::addNew()
{
    IToolPresetList *list = toolCfg_->toolPresetList(currentType_);
    if (!list) return;
    const QString name = QString("%1 %2").arg(toolTypeLabel(currentType_)).arg(list->count() + 1);
    const int newIndex = list->addNew(name);
    glWidget->setActiveToolPreset(currentType_, newIndex);
    rebuildRows();
}

#include "ToolPresetDock.moc"
