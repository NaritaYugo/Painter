#include "docks/LayerDock.h"
#include "docks/layers/LayerDockLayout.h"
#include "docks/layers/LayerIndicatorZone.h"
#include "docks/layers/LayerRowWidget.h"
#include "docks/layers/MaskOpacityPreview.h"

#include "canvas/CanvasWidget.h"
#include "components/ThemeColors.h"
#include "document/BlendModeList.h"

#include <QAction>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
// LayerDock
LayerDock::LayerDock(CanvasWidget *gl, QWidget *parent)
    : QWidget(parent), glWidget(gl)
{
    setMinimumSize(MIN_WIDTH, MIN_HEIGHT);

    refreshDebounceTimer_ = new QTimer(this);
    refreshDebounceTimer_->setSingleShot(true);
    connect(refreshDebounceTimer_, &QTimer::timeout, this, &LayerDock::refresh);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(6);

    // ---- 上部コントロール(選択中レイヤーのブレンドモード/不透明度、追加/削除) ----。
    auto *ctrlRow = new QHBoxLayout();
    auto *ctrlLeft = new QVBoxLayout();
    blendCombo = new QComboBox(this);
    populateBlendCombo(BlendComboMode::Blend); // 初期状態はブレンドモード一覧
    connect(blendCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        QVariant data = blendCombo->itemData(idx);
        if (!data.isValid()) return; // セパレーター行
        CanvasDocument &doc = glWidget->document();
        int active = doc.activeLayerIndex();
        if (active < 0 || active >= doc.layerCount()) return;
        if (blendComboMode_ == BlendComboMode::Adjustment) {
            // 調整レイヤー選択中は、このコンボは種類。
            doc.layers[active].adjustment.kind = static_cast<AdjustmentKind>(data.toInt());
            refresh(); // メタ表示("色相・彩度・明度: 100%"等)を更新する
        } else if (blendComboMode_ == BlendComboMode::Filter) {
            // フィルターレイヤーでは種類を選択する。
            doc.layers[active].filter.kind = static_cast<FilterKind>(data.toInt());
            glWidget->invalidateFilterChainCache();
            glWidget->update();
            refresh();
        } else {
            BlendMode mode = static_cast<BlendMode>(data.toInt());
            doc.setLayerBlendMode(active, mode);
        }
    });
    ctrlLeft->addWidget(blendCombo, 1);
    
    auto *buttonBox = new QHBoxLayout();
    buttonBox->setSpacing(2);

    addRowBtn = new QPushButton(this);
    addRowBtn->setIcon(QIcon(":/icons/common/add_above"));
    addRowBtn->setToolTip("新規レイヤー");
    connect(addRowBtn, &QPushButton::clicked, this, &LayerDock::addRow);

    addColBtn = new QPushButton(this);
    addColBtn->setIcon(QIcon(":/icons/common/add_right"));
    addColBtn->setToolTip("クリップ追加");
    connect(addColBtn, &QPushButton::clicked, this, &LayerDock::addColumn);

    addSpecialLayerBtn = new QPushButton();
    addSpecialLayerBtn->setIcon(QIcon(":/icons/common/new"));
    addSpecialLayerBtn->setToolTip("追加");
    addSpecialLayerBtn->setStyleSheet(R"(
        QPushButton::menu-indicator {
            subcontrol-position: right;
            subcontrol-origin: padding;
            width: 0;
        }
        )");

    // 親をthisにしておく(setMenu()はメニューの所有権を取らないため、親無しで作るとLayerDock破棄後もQMenuが残る)。
    QMenu *specialLayerMenu = new QMenu(this);
    addSpecialLayerBtn->setMenu(specialLayerMenu);
    QAction *addFolderAction = specialLayerMenu->addAction("新規フォルダー");
    QAction *addSolidColorAction = specialLayerMenu->addAction("新規単色レイヤー");
    QAction *addAdjustmentAction = specialLayerMenu->addAction("新規調整レイヤー");
    QAction *addTextLayerAction = specialLayerMenu->addAction("新規テキストレイヤー");
    QAction *addFilterLayerAction = specialLayerMenu->addAction("新規フィルターレイヤー");
    connect(addFolderAction, &QAction::triggered, this, &LayerDock::addFolder);
    connect(addSolidColorAction, &QAction::triggered, this, &LayerDock::insertSolidColorLayer);
    connect(addAdjustmentAction, &QAction::triggered, this, &LayerDock::insertAdjustmentLayer);
    connect(addTextLayerAction, &QAction::triggered, this, &LayerDock::insertTextLayer);
    connect(addFilterLayerAction, &QAction::triggered, this, &LayerDock::insertFilterLayer);

    duplicateBtn = new QPushButton(this);
    duplicateBtn->setIcon(QIcon(":/icons/common/copy"));
    duplicateBtn->setToolTip("レイヤーを複製");
    connect(duplicateBtn, &QPushButton::clicked, this, &LayerDock::duplicateSelected);

    mergeBtn = new QPushButton(this);
    mergeBtn->setIcon(QIcon(":/icons/common/merge"));
    mergeBtn->setToolTip("レイヤーを結合");
    connect(mergeBtn, &QPushButton::clicked, this, &LayerDock::mergeSelected);

    deleteBtn = new QPushButton(this);
    deleteBtn->setIcon(QIcon(":/icons/common/trash"));
    deleteBtn->setToolTip("レイヤーを削除");
    connect(deleteBtn, &QPushButton::clicked, this, &LayerDock::deleteActiveLayer);

    buttonBox->addWidget(addRowBtn);
    buttonBox->addWidget(addColBtn);
    buttonBox->addWidget(addSpecialLayerBtn);
    buttonBox->addWidget(duplicateBtn);
    buttonBox->addWidget(mergeBtn);
    buttonBox->addWidget(deleteBtn);
    ctrlLeft->addLayout(buttonBox);

    ctrlRow->addLayout(ctrlLeft);

    opacityPreview = new MaskOpacityPreview(this);
    opacityLabel = new QLabel("100%", this);
    opacityLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    opacityLabel->setFixedWidth(opacityPreview->width());
    opacityLabel->setStyleSheet(QString("color: %1; font-size: 10px; background: transparent;")
                                    .arg(Theme::hintText.name()));
    // 左右ドラッグ: 一律の不透明度(scalar)を変更する。
    connect(opacityPreview, &MaskOpacityPreview::opacityChanged, this, [this](float v) {
        CanvasDocument &doc = glWidget->document();
        int a = doc.activeLayerIndex();
        if (a < 0 || a >= doc.layerCount()) return;
        opacityLabel->setText(QString::number(qBound(0, qRound(v * 100.0f), 100)) + "%");
        doc.setLayerOpacity(a, v);
    });
    // クリック: そのレイヤーのマスク編集モードをオン/オフ(CanvasWidget側で、まだマスクが無ければ白マスクを自動生成する)。
    connect(opacityPreview, &MaskOpacityPreview::clicked, this, [this]() {
        CanvasDocument &doc = glWidget->document();
        int a = doc.activeLayerIndex();
        if (a < 0 || a >= doc.layerCount()) return;
        glWidget->setEditingMaskLayer(a);
        updateControlsFromActiveLayer();
    });
    auto *opacityColumn = new QVBoxLayout();
    opacityColumn->setContentsMargins(0, 0, 0, 0);
    opacityColumn->setSpacing(1);
    opacityColumn->addWidget(opacityPreview, 0, Qt::AlignHCenter);
    opacityColumn->addWidget(opacityLabel, 0, Qt::AlignHCenter);
    ctrlRow->addLayout(opacityColumn);

    mainLayout->addLayout(ctrlRow);

    // ---- パンくずリスト(フォルダーの階層を降りているときだけ表示) ----。
    breadcrumbBar_ = new QWidget(this);
    breadcrumbLayout_ = new QHBoxLayout(breadcrumbBar_);
    breadcrumbLayout_->setContentsMargins(2, 0, 2, 0);
    breadcrumbLayout_->setSpacing(2);
    breadcrumbBar_->setVisible(false);
    mainLayout->addWidget(breadcrumbBar_);

    // ---- レイヤーリスト + 常時表示インジケーター ----。
    auto *splitter = new QSplitter();

    scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    listContainer = new QWidget();
    listLayout = new QVBoxLayout(listContainer);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(4);

    emptyFolderHintLabel_ = new QLabel("(空のフォルダーです)", listContainer);
    emptyFolderHintLabel_->setAlignment(Qt::AlignCenter);
    emptyFolderHintLabel_->setStyleSheet(QString("color: %1; padding: 24px 0;").arg(Theme::hintText.name()));
    emptyFolderHintLabel_->setVisible(false);
    listLayout->addWidget(emptyFolderHintLabel_);

    listLayout->addStretch();
    scrollArea->setWidget(listContainer);
    splitter->addWidget(scrollArea);

    indicatorZone = new LayerIndicatorZone(glWidget, this);
    connect(indicatorZone, &LayerIndicatorZone::layerClicked, this, &LayerDock::selectLayer);
    connect(indicatorZone, &LayerIndicatorZone::folderDoubleClicked, this, &LayerDock::enterFolder);
    splitter->addWidget(indicatorZone);

    splitter->setSizes({600, 400});

    mainLayout->addWidget(splitter, 1);

    rebuildRows();
}

void LayerDock::setCanvasWidget(CanvasWidget *gl)
{
    glWidget = gl;
    indicatorZone->setCanvasWidget(gl);
    // LayerRowWidgetは(サムネイルキャッシュを保持するため)自分がどのCanvasWidget向けに作られたかを覚えず、rebuildRows()の使い回しロジックもレイヤー構成の「形状」だけで一致判定している。
    qDeleteAll(rows_);
    rows_.clear();
    folderPath_.clear();
    rebuildBreadcrumb();
    refresh();
}

void LayerDock::rebuildRows()
{
    CanvasDocument &doc = glWidget->document();
    const int scope = currentScope();
    indicatorZone->setScope(scope);

    QVector<QVector<int>> rows = computeRows(doc, scope);
    int activeIndex = doc.activeLayerIndex();
    emptyFolderHintLabel_->setVisible(insideFolder() && rows.isEmpty());

    // レイヤー構成(行数/各行の枚数)が変わるたびに全行を作り直すと、変化していない既存レイヤーぶんまでサムネイルを再生成することになり重い。
    QVector<LayerRowWidget*> oldRows = rows_;
    QVector<bool> reused(oldRows.size(), false);
    rows_.clear();

    // rows は下から上の順で保持している。
    for (int r = rows.size() - 1; r >= 0; --r) {
        LayerRowWidget *row = nullptr;
        for (int i = 0; i < oldRows.size(); i++) {
            if (!reused[i] && oldRows[i]->rowLayers() == rows[r]) {
                row = oldRows[i];
                reused[i] = true;
                break;
            }
        }
        if (row) {
            listLayout->removeWidget(row);
        } else {
            row = new LayerRowWidget(glWidget, rows[r], listContainer);
            connect(row, &LayerRowWidget::clicked, this, &LayerDock::selectLayer);
            connect(row, &LayerRowWidget::adjustmentLayerDoubleClicked, this, &LayerDock::openAdjustmentLayerEditor);
            connect(row, &LayerRowWidget::solidColorLayerDoubleClicked, this, &LayerDock::openSolidColorLayerEditor);
            connect(row, &LayerRowWidget::filterLayerDoubleClicked,     this, &LayerDock::openFilterLayerEditor);
            connect(row, &LayerRowWidget::folderDoubleClicked,          this, &LayerDock::enterFolder);
        }
        row->refreshFromDocument();
        row->setSelected(rows[r].contains(activeIndex));
        listLayout->insertWidget(listLayout->count() - 1, row);
        rows_.append(row);
    }

    for (int i = 0; i < oldRows.size(); i++)
        if (!reused[i]) oldRows[i]->deleteLater();

    updateControlsFromActiveLayer();
    indicatorZone->update();
}

void LayerDock::selectLayer(int layerIndex)
{
    glWidget->document().setActiveLayer(layerIndex);
    for (LayerRowWidget *row : rows_) {
        row->refreshFromDocument();
        row->setSelected(row->rowLayers().contains(layerIndex));
    }
    updateControlsFromActiveLayer();
    indicatorZone->update();
}

void LayerDock::updateControlsFromActiveLayer()
{
    const CanvasDocument &doc = glWidget->document();
    // キャンバスタブが1つも開いていないとき(glWidgetがMainWindowのダミーCanvasWidgetを指している)はlayerCount()が常に0になるため、これをそのまま「レイヤー操作不可」の判定に使う。
    bool hasLayer = doc.layerCount() > 0;
    blendCombo->setEnabled(hasLayer);
    addRowBtn->setEnabled(hasLayer);
    addColBtn->setEnabled(hasLayer);
    duplicateBtn->setEnabled(hasLayer);
    addSpecialLayerBtn->setEnabled(hasLayer);
    deleteBtn->setEnabled(hasLayer);
    if (!hasLayer) {
        opacityPreview->setState(false, 1.0f, false, QImage());
        opacityLabel->setText("--");
        opacityLabel->setEnabled(false);
        mergeBtn->setEnabled(false);
        return;
    }

    const Layer &layer = doc.layers[doc.activeLayerIndex()];
    QSignalBlocker b1(blendCombo);
    if (layer.layerType == LayerType::Adjustment) {
        // 調整レイヤー選択中は、ブレンドモードの代わりに調整の種類を選ぶコンボとして使う。
        populateBlendCombo(BlendComboMode::Adjustment);
        blendCombo->setCurrentIndex(blendCombo->findData((int)layer.adjustment.kind));
        blendCombo->setEnabled(true);
    } else if (layer.layerType == LayerType::Filter) {
        // フィルターレイヤー選択中は、ブレンドモードの代わりに種類(ぼかし/色収差)を選ぶコンボとして使う(調整レイヤーと同じ考え方)。
        populateBlendCombo(BlendComboMode::Filter);
        blendCombo->setCurrentIndex(blendCombo->findData((int)layer.filter.kind));
        blendCombo->setEnabled(true);
    } else {
        populateBlendCombo(BlendComboMode::Blend);
        blendCombo->setCurrentIndex(blendCombo->findData((int)layer.blendMode));
        // 単色レイヤーはブレンドモードを変更できない。
        blendCombo->setEnabled(layer.layerType == LayerType::Normal || layer.layerType == LayerType::Text);
    }
    // 不透明度プレビュー(=旧不透明度スライダー)の状態を反映する。
    {
        int activeIndex = doc.activeLayerIndex();
        bool hasMask  = layer.hasMask;
        QImage maskThumb = hasMask ? glWidget->getMaskPreview(activeIndex, 96) : QImage();
        opacityPreview->setState(true, layer.opacity, hasMask, maskThumb);
        opacityLabel->setEnabled(true);
        opacityLabel->setText(QString::number(qBound(0, qRound(layer.opacity * 100.0f), 100)) + "%");
    }

    // フォルダーへのクリップ追加はまだ対応しない(フォルダーに対するクリッピングは未実装のためスコープ外)。
    addColBtn->setEnabled(layer.layerType != LayerType::Folder);

    // 「結合」: クリップ列(col>=1)なら常に有効(左隣に結合できる)。
    QVector<QVector<int>> rows = computeRows(doc, currentScope());
    int activeIndex = doc.activeLayerIndex();
    bool canMerge = false;
    if (layer.layerType == LayerType::Folder) {
        canMerge = layer.childCount > 0;
    } else {
        for (int r = 0; r < rows.size(); r++) {
            int col = rows[r].indexOf(activeIndex);
            if (col < 0) continue;
            canMerge = (col >= 1) || (rows[r].size() > 1) || (r > 0);
            break;
        }
    }
    mergeBtn->setEnabled(canMerge);
}

// blendComboの中身を、通常のブレンドモード一覧/調整レイヤーの種類一覧/フィルターレイヤーの種類一覧のいずれかに入れ替える。
void LayerDock::populateBlendCombo(BlendComboMode mode)
{
    if (blendComboMode_ == mode) return;
    blendComboMode_ = mode;

    QSignalBlocker guard(blendCombo);
    blendCombo->clear();

    if (mode == BlendComboMode::Adjustment) {
        // 無料版でも常に使えるものを先頭に。
        blendCombo->addItem("色相・彩度・明度",     (int)AdjustmentKind::HueSaturation);
        blendCombo->addItem("明るさ・コントラスト", (int)AdjustmentKind::BrightnessContrast);
        blendCombo->addItem("カラーバランス",       (int)AdjustmentKind::ColorBalance);
        blendCombo->addItem("トーンカーブ",         (int)AdjustmentKind::ToneCurve);
        blendCombo->addItem("グラデーションマップ", (int)AdjustmentKind::GradientMap);
        return;
    }
    if (mode == BlendComboMode::Filter) {
        // 無料版でも常に使えるものを先頭に。
        blendCombo->addItem("ぼかし",     (int)FilterKind::GaussianBlur);
        blendCombo->addItem("移動ぼかし", (int)FilterKind::MotionBlur);
        blendCombo->addItem("モザイク",   (int)FilterKind::Mosaic);
        blendCombo->addItem("ノイズ",     (int)FilterKind::Noise);
        blendCombo->addItem("色収差",     (int)FilterKind::ChromaticAberration);
        blendCombo->addItem("レンズぼかし", (int)FilterKind::LensBlur);
        return;
    }

    // 一覧はブラシの合成モードコンボ(ToolPropertyDock)と共有する(BlendModeList.h)。
    for (const BlendModeItem &it : blendModeItems()) {
        if (it.isSeparator()) blendCombo->insertSeparator(blendCombo->count());
        else                  blendCombo->addItem(it.label, (int)it.mode);
    }
}

// レイヤーリストで調整レイヤーのプレビューをダブルクリックしたときに呼ばれる。
void LayerDock::openAdjustmentLayerEditor(int layerIndex)
{
    glWidget->editAdjustmentLayer(layerIndex);
}

// レイヤーリストで単色レイヤーのプレビューをダブルクリックしたときに呼ばれる。
void LayerDock::openSolidColorLayerEditor(int layerIndex)
{
    glWidget->editSolidColorLayer(layerIndex);
}

// レイヤーリストでフィルターレイヤーのプレビューをダブルクリックしたときに呼ばれる。
void LayerDock::openFilterLayerEditor(int layerIndex)
{
    glWidget->editFilterLayer(layerIndex);
}

// 現在のスコープ(currentScope())内で、選択中レイヤーが属する行の直後に新規レイヤーを挿入する場合のflat挿入位置を返す(該当行が無ければスコープの末尾)。
int LayerDock::computeInsertIndexInScope() const
{
    const CanvasDocument &doc = glWidget->document();
    const int scope = currentScope();
    QVector<QVector<int>> rows = computeRows(doc, scope);
    const int activeIndex = doc.activeLayerIndex();
    for (const auto &r : rows)
        if (r.contains(activeIndex))
            return afterLayerBlock(doc, r.last());
    return scopeEndIndex(doc, scope);
}

void LayerDock::addRow()
{
    CanvasDocument &doc = glWidget->document();
    const int insertIndex = computeInsertIndexInScope();

    // キャンバス作成時に "レイヤー1" が自動追加されるため、ここでは2から始まるように既存の "レイヤーN" の最大値+1を採番する。
    const QString name = nextNumberedName(doc, "レイヤー", [](const Layer &l) {
        return l.layerType == LayerType::Normal;
    });
    glWidget->beginLayerAddUndo();
    if (!glWidget->addLayer(name, insertIndex, /*clipping=*/false, 0, 0, -1, -1, LayerType::Normal, folderPath_)) {
        glWidget->abortLayerAddUndo();
        return;
    }
    doc.setActiveLayer(insertIndex);
    glWidget->commitLayerAddUndo(insertIndex, folderPath_);
    rebuildRows();
}

void LayerDock::addColumn()
{
    CanvasDocument &doc = glWidget->document();
    if (doc.layerCount() == 0) { addRow(); return; }

    const int scope = currentScope();
    QVector<QVector<int>> rows = computeRows(doc, scope);
    int activeIndex = doc.activeLayerIndex();
    int insertIndex = activeIndex + 1;
    QString rootName;
    bool foundRow = false;
    for (const auto &r : rows) {
        int col = r.indexOf(activeIndex);
        if (col >= 0) {
            // フォルダーへのクリップ追加はまだ対応しない(addColBtn側でも無効化済み、ここは誤ってショートカット等から呼ばれた場合の防御)。
            if (doc.layers[r[0]].layerType == LayerType::Folder) return;
            insertIndex = r[col] + 1;
            rootName = doc.layers[r[0]].name;
            foundRow = true;
            break;
        }
    }
    if (!foundRow) return; // アクティブレイヤーが現在のスコープに存在しない

    // クリッピング先のレイヤー名を接頭辞にした連番(例: "レイヤー1" へのクリップなら "レイヤー1_1", "レイヤー1_2", ...)にする。
    const QString name = rootName.isEmpty()
        ? QStringLiteral("クリップレイヤー")
        : nextNumberedName(doc, rootName + "_", [](const Layer &) { return true; });
    glWidget->beginLayerAddUndo();
    if (!glWidget->addLayer(name, insertIndex, /*clipping=*/true, 0, 0, -1, -1, LayerType::Normal, folderPath_)) {
        glWidget->abortLayerAddUndo();
        return;
    }
    doc.setActiveLayer(insertIndex);
    glWidget->commitLayerAddUndo(insertIndex, folderPath_);
    rebuildRows();
}

// 現在のスコープ(ルート、またはフォルダーの中)の、選択中の行の直後にフォルダーを1つ挿入する。
void LayerDock::addFolder()
{
    CanvasDocument &doc = glWidget->document();
    const int insertIndex = computeInsertIndexInScope();

    const QString name = nextNumberedName(doc, "フォルダー", [](const Layer &l) {
        return l.layerType == LayerType::Folder;
    });
    glWidget->beginLayerAddUndo();
    if (!glWidget->addLayer(name, insertIndex, /*clipping=*/false, 0, 0, 0, 0, LayerType::Folder, folderPath_)) {
        glWidget->abortLayerAddUndo();
        return;
    }
    doc.setActiveLayer(insertIndex);
    glWidget->commitLayerAddUndo(insertIndex, folderPath_);
    rebuildRows();
}

// インジケーター上の星形(フォルダー)をダブルクリックしたときに呼ばれる。
void LayerDock::enterFolder(int folderLayerIndex)
{
    const CanvasDocument &doc = glWidget->document();
    if (folderLayerIndex < 0 || folderLayerIndex >= doc.layerCount()) return;
    if (doc.layers[folderLayerIndex].layerType != LayerType::Folder) return;

    folderPath_.append(folderLayerIndex);
    rebuildBreadcrumb();
    rebuildRows();
}

// パンくずリストのセグメントをクリックしたときに呼ばれる。
void LayerDock::goToBreadcrumbLevel(int keepCount)
{
    if (keepCount >= folderPath_.size()) return;
    folderPath_.resize(qMax(0, keepCount));
    rebuildBreadcrumb();
    rebuildRows();
}

// folderPath_の内容から、パンくずリストのボタン列を作り直す。
void LayerDock::rebuildBreadcrumb()
{
    QLayoutItem *item;
    while ((item = breadcrumbLayout_->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    breadcrumbBar_->setVisible(!folderPath_.isEmpty());
    if (folderPath_.isEmpty()) return;

    const CanvasDocument &doc = glWidget->document();

    auto addSegment = [&](const QString &text, int keepCount, bool isCurrent) {
        auto *btn = new QPushButton(text, breadcrumbBar_);
        btn->setFlat(true);
        btn->setCursor(isCurrent ? Qt::ArrowCursor : Qt::PointingHandCursor);
        btn->setEnabled(!isCurrent);
        btn->setStyleSheet(QString("QPushButton { color: %1; border: none; padding: 2px 4px; text-align: left; }")
            .arg((isCurrent ? Theme::textBright : Theme::panelText).name()));
        if (!isCurrent)
            connect(btn, &QPushButton::clicked, this, [this, keepCount]() { goToBreadcrumbLevel(keepCount); });
        breadcrumbLayout_->addWidget(btn);
    };

    addSegment("ルート", 0, false);
    for (int i = 0; i < folderPath_.size(); i++) {
        auto *sep = new QLabel(">", breadcrumbBar_);
        sep->setStyleSheet(QString("color: %1;").arg(Theme::hintText.name()));
        breadcrumbLayout_->addWidget(sep);

        int li = folderPath_[i];
        QString name = (li >= 0 && li < doc.layerCount()) ? doc.layers[li].name : QStringLiteral("?");
        bool isCurrent = (i == folderPath_.size() - 1);
        addSegment(name, i + 1, isCurrent);
    }
    breadcrumbLayout_->addStretch();
}

// 現在のスコープ内、選択中レイヤーのすぐ後ろに、単色(白)レイヤーを挿入する。
void LayerDock::insertSolidColorLayer()
{
    CanvasDocument &doc = glWidget->document();
    const int insertIndex = computeInsertIndexInScope();

    const QString name = nextNumberedName(doc, "単色レイヤー", [](const Layer &l) {
        return l.layerType == LayerType::SolidColor;
    });
    glWidget->beginLayerAddUndo();
    if (!glWidget->addSolidColorLayer(name, insertIndex, QColor(255, 255, 255, 255), folderPath_)) {
        glWidget->abortLayerAddUndo();
        return;
    }
    doc.setActiveLayer(insertIndex);
    glWidget->commitLayerAddUndo(insertIndex, folderPath_);
    rebuildRows();
}

// 空のテキストレイヤーを作成するだけ。
void LayerDock::insertTextLayer()
{
    CanvasDocument &doc = glWidget->document();
    const int insertIndex = computeInsertIndexInScope();

    const QString name = nextNumberedName(doc, "テキストレイヤー", [](const Layer &l) {
        return l.layerType == LayerType::Text;
    });
    glWidget->beginLayerAddUndo();
    if (!glWidget->addLayer(name, insertIndex, /*clipping=*/false, 0, 0, 0, 0, LayerType::Text, folderPath_)) {
        glWidget->abortLayerAddUndo();
        return;
    }
    doc.setActiveLayer(insertIndex);
    glWidget->commitLayerAddUndo(insertIndex, folderPath_);
    rebuildRows();
}

// 調整レイヤーを作成する(デフォルトは色相・彩度・明度、全パラメータ0)。
void LayerDock::insertAdjustmentLayer()
{
    CanvasDocument &doc = glWidget->document();
    const int insertIndex = computeInsertIndexInScope();

    const QString name = nextNumberedName(doc, "調整レイヤー", [](const Layer &l) {
        return l.layerType == LayerType::Adjustment;
    });
    glWidget->beginLayerAddUndo();
    if (!glWidget->addLayer(name, insertIndex, /*clipping=*/false, 0, 0, 0, 0, LayerType::Adjustment, folderPath_)) {
        glWidget->abortLayerAddUndo();
        return;
    }
    doc.layers[insertIndex].adjustment.kind = AdjustmentKind::HueSaturation;
    doc.setActiveLayer(insertIndex);
    // adjustment.kindを入れてからcommitする(Redoで種類を復元するため)。
    glWidget->commitLayerAddUndo(insertIndex, folderPath_);
    rebuildRows();
}

// フィルターレイヤーを作成する(現時点では種類は色収差のみ)。
void LayerDock::insertFilterLayer()
{
    CanvasDocument &doc = glWidget->document();
    const int insertIndex = computeInsertIndexInScope();

    // 種類名を名前に含めない("フィルター1"等)。
    const QString name = nextNumberedName(doc, "フィルター", [](const Layer &l) {
        return l.layerType == LayerType::Filter;
    });
    glWidget->beginLayerAddUndo();
    if (!glWidget->addLayer(name, insertIndex, /*clipping=*/false, 0, 0, 0, 0, LayerType::Filter, folderPath_)) {
        glWidget->abortLayerAddUndo();
        return;
    }
    // 既定はガウスぼかし(無料版でも常に使える種類)。
    FilterParams fp;
    fp.kind = FilterKind::GaussianBlur;
    fp.blurRadiusPx = 8.0f;
    // ノイズの乱数の種はここで決めておく(後から種類をノイズへ切り替えたときのため。FilterLayerEditAction::onActivateはこの値をそのまま使い続けるだけで振り直さない)。
    fp.nsSeed = QRandomGenerator::global()->generate();
    doc.layers[insertIndex].filter = fp;
    doc.setActiveLayer(insertIndex);
    glWidget->commitLayerAddUndo(insertIndex, folderPath_);
    rebuildRows();
}

void LayerDock::deleteActiveLayer()
{
    CanvasDocument &doc = glWidget->document();
    if (doc.layerCount() <= 1) return; // 最後の1枚は消せない
    glWidget->removeLayer(doc.activeLayerIndex(), folderPath_);
    rebuildRows();
}

// 選択中レイヤーの列(root/クリップ)に応じて複製処理を振り分ける。
void LayerDock::duplicateSelected()
{
    CanvasDocument &doc = glWidget->document();
    QVector<QVector<int>> rows = computeRows(doc, currentScope());
    int activeIndex = doc.activeLayerIndex();
    if (activeIndex < 0) return;

    for (const auto &r : rows) {
        int col = r.indexOf(activeIndex);
        if (col < 0) continue;

        if (col == 0 && doc.layers[r[0]].layerType == LayerType::Folder) {
            // フォルダーの「複製」は中身(ネストも含む)ごと丸ごと複製する
            int insertAt = afterLayerBlock(doc, r.last());
            int newFolderIndex = duplicateFolderDeep(r[0], insertAt);
            if (newFolderIndex < 0) return;
            doc.setActiveLayer(newFolderIndex);
        } else if (col == 0) {
            // 元の行の直後(上)へ、列の並び順を保ったまま複製していく。
            int insertAt = afterLayerBlock(doc, r.last());
            int newActiveIndex = insertAt;
            for (int i = 0; i < r.size(); i++) {
                glWidget->beginLayerAddUndo();
                int newIndex = glWidget->duplicateLayer(r[i], insertAt + i, folderPath_);
                if (newIndex < 0) { glWidget->abortLayerAddUndo(); return; }
                glWidget->commitLayerAddUndo(newIndex, folderPath_, /*duplicateSourceIndex=*/r[i]);
                if (i == col) newActiveIndex = newIndex;
            }
            doc.setActiveLayer(newActiveIndex);
        } else {
            glWidget->beginLayerAddUndo();
            int newIndex = glWidget->duplicateLayer(activeIndex, activeIndex + 1, folderPath_);
            if (newIndex < 0) { glWidget->abortLayerAddUndo(); return; }
            doc.setLayerClipping(newIndex, true); // 右隣は必ずクリップ列になる
            doc.setActiveLayer(newIndex);
            // clippingを立ててからcommitする(Redoで復元されるのはcommit時点の状態)。
            glWidget->commitLayerAddUndo(newIndex, folderPath_, /*duplicateSourceIndex=*/activeIndex);
        }
        rebuildRows();
        return;
    }
}

// sourceFolderIndexの中身(ネストしたフォルダーも含む)を丸ごと複製し、insertAtへ挿入する。
int LayerDock::duplicateFolderDeep(int sourceFolderIndex, int insertAt)
{
    CanvasDocument &doc = glWidget->document();
    const int count = 1 + doc.layers[sourceFolderIndex].childCount;

    QHash<int, int> oldToNew;
    // 元配列上での祖先チェーン(範囲判定にのみ使う。複製処理中はsourceFolderIndexより前の実データなので不変)。
    QVector<int> oldStack{ sourceFolderIndex };

    for (int k = 0; k < count; k++) {
        const int srcIdx = sourceFolderIndex + k;

        // oldStackを、srcIdxが実際に含まれる範囲まで閉じる。
        while (oldStack.size() > 1) {
            int top = oldStack.last();
            int topEnd = top + 1 + doc.layers[top].childCount;
            if (srcIdx < topEnd) break;
            oldStack.removeLast();
        }

        // oldStack(祖先の元index列、srcIdx自身は除く)を複製後のindexへ読み替える。
        QVector<int> ancestors = folderPath_;
        for (int old : oldStack) {
            if (old == srcIdx) continue; // k==0(フォルダー自身)のときはoldStack==[srcIdx]
            ancestors.append(oldToNew.value(old));
        }

        glWidget->beginLayerAddUndo();
        int newIdx = glWidget->duplicateLayer(srcIdx, insertAt + k, ancestors);
        if (newIdx < 0) { glWidget->abortLayerAddUndo(); return -1; }
        glWidget->commitLayerAddUndo(newIdx, ancestors, /*duplicateSourceIndex=*/srcIdx);
        oldToNew.insert(srcIdx, newIdx);

        if (doc.layers[srcIdx].layerType == LayerType::Folder && doc.layers[srcIdx].childCount > 0)
            oldStack.append(srcIdx);
    }
    return oldToNew.value(sourceFolderIndex, -1);
}

// 選択中レイヤーの列(root/クリップ)に応じて結合処理を振り分ける。
void LayerDock::mergeSelected()
{
    CanvasDocument &doc = glWidget->document();
    QVector<QVector<int>> rows = computeRows(doc, currentScope());
    int activeIndex = doc.activeLayerIndex();

    for (const auto &r : rows) {
        int col = r.indexOf(activeIndex);
        if (col < 0) continue;

        if (col >= 1) {
            int survivor = r[col - 1];
            int victim   = r[col];
            int newIndex = glWidget->mergeLayers(survivor, victim, folderPath_);
            if (newIndex >= 0) {
                doc.setActiveLayer(newIndex);
                rebuildRows();
            }
        } else if (doc.layers[activeIndex].layerType == LayerType::Folder) {
            // フォルダーの「結合」は行そのものではなく、フォルダーの中身をすべて1枚のレイヤーへまとめる操作としてオーバーライドする。
            mergeFolderContents(activeIndex);
        } else {
            mergeRootRow(activeIndex);
        }
        return;
    }
}

// rootIndexが属する行(一番左の列)を結合する。
void LayerDock::mergeRootRow(int rootIndex)
{
    CanvasDocument &doc = glWidget->document();
    const int scope = currentScope();
    QVector<QVector<int>> rows = computeRows(doc, scope);
    int r = -1;
    for (int i = 0; i < rows.size(); i++) if (!rows[i].isEmpty() && rows[i][0] == rootIndex) { r = i; break; }
    if (r < 0) return;

    if (rows[r].size() > 1) {
        // 同じ行のクリップ列は常にrootIndexより大きいインデックスなので、結合してもrootIndex自体はずれない。
        int activeIndex = rootIndex;
        for (;;) {
            QVector<QVector<int>> rows2 = computeRows(doc, scope);
            int r2 = -1;
            for (int i = 0; i < rows2.size(); i++) if (!rows2[i].isEmpty() && rows2[i][0] == activeIndex) { r2 = i; break; }
            if (r2 < 0 || rows2[r2].size() <= 1) break;
            int victim = rows2[r2][1];
            if (glWidget->mergeLayers(activeIndex, victim, folderPath_) < 0) return;
        }
        doc.setActiveLayer(activeIndex);
        rebuildRows();
        return;
    }

    if (r == 0) return; // 一番下の行(結合先が無い)
    int below = rows[r - 1][0];
    int newIndex = glWidget->mergeLayers(below, rootIndex, folderPath_);
    if (newIndex < 0) return;
    doc.setActiveLayer(newIndex);
    rebuildRows();
}

// folderIndexの直接の中身をすべて1枚のNormalレイヤーへ結合し、フォルダーのマーカー自体は(結合後のレイヤーを残したまま)取り除く。
void LayerDock::mergeFolderContents(int folderIndex)
{
    CanvasDocument &doc = glWidget->document();
    const int childCount = doc.layers[folderIndex].childCount;
    if (childCount <= 0) return; // 空フォルダーは結合対象が無い

    QVector<int> children;
    for (int i = folderIndex + 1; i < folderIndex + 1 + childCount; i++) {
        if (doc.layers[i].layerType != LayerType::Normal) return;
        children.append(i);
    }
    if (children.isEmpty()) return;

    // 下から順に1つ上へ結合していく。
    int survivor = children[0];
    for (int k = 1; k < children.size(); k++) {
        int newIndex = glWidget->mergeLayers(survivor, survivor + 1, folderPath_ + QVector<int>{ folderIndex });
        if (newIndex < 0) return;
        survivor = newIndex;
    }

    // 実レイヤーが1枚だけ残ったfolderIndexのマーカーを、中身(survivor)を巻き込まず取り除く(includeContents=false)。
    if (!glWidget->removeLayer(folderIndex, folderPath_, /*includeContents=*/false)) return;

    doc.setActiveLayer(folderIndex); // マーカー除去でsurvivorがちょうどfolderIndexの位置に来る
    rebuildRows();
}

void LayerDock::scheduleRefresh()
{
    // ストローク確定の連打や不透明度スライダーのドラッグ等でlayersChanged()が短時間に連続発火しても、都度refresh()(フルキャンバス合成を伴いうる重いサムネイル再生成)を実行せず、
    // 一定時間発火が止まってから最後の1回だけまとめて実行する。
    refreshDebounceTimer_->start(80);
}

void LayerDock::refresh()
{
    // ストローク中/ペンのホバー中は、アクティブレイヤーのサムネイル再生成(タイルのGPU→CPU読み戻しを伴う)を先送りする(NavigatorDock::refreshと同じ理由: 文字書きの連打中に次のペンダウンを塞がない)。
    if (glWidget && glWidget->isGLReady() && glWidget->isInkingBusy()) {
        refreshDebounceTimer_->start(150);
        return;
    }

    CanvasDocument &doc = glWidget->document();
    QVector<QVector<int>> rows = computeRows(doc, currentScope());

    // 行の枚数、または行ごとの列数(クリップの追加/削除)が変わっていたら行ウィジェットを作り直す。
    bool shapeChanged = (rows.size() != rows_.size());
    if (!shapeChanged) {
        for (int i = 0; i < rows_.size(); i++) {
            // rows_ は上(最前面)から並んでいるので、対応する rows のインデックスは逆順。
            int dataRow = rows_.size() - 1 - i;
            if (rows_[i]->rowLayers().size() != rows[dataRow].size()) {
                shapeChanged = true;
                break;
            }
        }
    }

    if (shapeChanged) {
        rebuildRows();
        return;
    }

    int activeIndex = doc.activeLayerIndex();
    for (LayerRowWidget *row : rows_) {
        row->refreshFromDocument();
        row->setSelected(row->rowLayers().contains(activeIndex));
    }
    updateControlsFromActiveLayer();
    indicatorZone->update();
}
