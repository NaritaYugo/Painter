#include "dialogs/ShortcutsDlg.h"
#include "shortcuts/ShortcutRegistry.h"
#include "tools/core/ToolConfig.h"
#include "tools/core/ToolRegistry.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QSettings>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QGuiApplication>
#include <QScreen>

namespace {
// ツリー項目に m_rows のインデックスを埋めておくためのロール
constexpr int kRowIndexRole = Qt::UserRole + 1;
}

// ===========================================================================
QString ShortcutsDlg::keyName(int key)
{
    if (key == 0) return "（未設定）";
    return QKeySequence(key).toString(QKeySequence::NativeText);
}

// ===========================================================================
ShortcutsDlg::ShortcutsDlg(ShortcutRegistry &registry, ToolConfig *toolCfg, QWidget *parent)
    : QDialog(parent), m_registry(registry), m_toolCfg(toolCfg)
{
    setWindowTitle("ショートカットキー設定");
    setModal(true);

    buildRows();

    auto *rootLayout = new QVBoxLayout(this);

    // ---- 説明 ----
    auto *desc = new QLabel(
        "行をクリックしてからキーを押すと割り当てられます。"
        "★の付いた行は長押しで一時的に切り替わります。");
    desc->setWordWrap(true);
    desc->setStyleSheet("color: #AAAAAA; font-size: 11px;");
    rootLayout->addWidget(desc);

    // ---- 左: カテゴリ一覧 / 右: その内容(環境設定と同じ構成) ----
    auto *bodyLayout = new QHBoxLayout();

    m_categoryList = new QListWidget();
    m_categoryList->setFixedWidth(140);
    bodyLayout->addWidget(m_categoryList);

    m_pageStack = new QStackedWidget();
    bodyLayout->addWidget(m_pageStack, 1);

    buildPages(); // m_categoryList / m_pageStack を埋める

    connect(m_categoryList, &QListWidget::currentRowChanged,
            m_pageStack, &QStackedWidget::setCurrentIndex);
    m_categoryList->setCurrentRow(0);

    rootLayout->addLayout(bodyLayout, 1);

    // ---- ヒントラベル ----
    m_hintLabel = new QLabel("");
    m_hintLabel->setAlignment(Qt::AlignCenter);
    m_hintLabel->setStyleSheet("color: #5695dd; font-size: 11px;");
    m_hintLabel->setFixedHeight(20);
    rootLayout->addWidget(m_hintLabel);

    // ---- ボタン行 ----
    auto *btnRow = new QHBoxLayout();

    m_clearBtn = new QPushButton("クリア");
    m_clearBtn->setToolTip("選択中のショートカットを解除");
    m_clearBtn->setEnabled(false);
    connect(m_clearBtn, &QPushButton::clicked, this, [this]() {
        if (m_editingRow < 0) return;
        m_rows[m_editingRow].currentKey = 0;
        setEditingRow(-1);
    });

    m_resetBtn = new QPushButton("デフォルトに戻す");
    connect(m_resetBtn, &QPushButton::clicked, this, &ShortcutsDlg::resetDefaults);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(box, &QDialogButtonBox::accepted, this, &ShortcutsDlg::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    btnRow->addWidget(m_clearBtn);
    btnRow->addWidget(m_resetBtn);
    btnRow->addStretch();
    btnRow->addWidget(box);
    rootLayout->addLayout(btnRow);

    // 高さは画面からはみ出さない範囲に収める(ツリーの推奨サイズをそのまま採用すると
    // 画面より高くなり、OK/キャンセルが画面外に出てしまう)。
    int maxH = 700;
    if (QScreen *scr = QGuiApplication::primaryScreen())
        maxH = qMax(360, scr->availableGeometry().height() - 120);
    resize(720, qMin(620, maxH));
}

// ===========================================================================
// 表示する全行を1本のリストとして作る。
// ページへの振り分けは category を見て buildPages() が行い、
// ツールプリセットの行は同じ category(=そのツールの category)のページ内で
// ツールの行の子として並べられる。
// ===========================================================================
void ShortcutsDlg::buildRows()
{
    m_rows.clear();

    // ---- ツール / コマンド ----
    for (const ActionSpec &spec : m_registry.actions()) {
        ActionSpec row = spec;
        if (row.kind == ActionKind::Tool)
            row.label += "  ★長押し対応";
        m_rows.append(row);
    }

    if (!m_toolCfg) return;

    // ---- ツールプリセット ----
    // プリセットは実行時に増減するため registry には登録されていない。
    // ツール一覧(ToolRegistry)を順に辿り、そのツールが持つプリセットを行にする。
    // 割り当て先はインデックスではなくuidなので、この後プリセットを並び替えたり
    // 改名したりしてもキーは同じプリセットに付いたままになる。
    for (const ToolTypeMeta &meta : toolTypeRegistry()) {
        IToolPresetList *list = m_toolCfg->toolPresetList(meta.type);
        if (!list) continue;

        // 親になるツールの行を探し、その category に合わせる
        // (見つからなければ「ツール」ページ扱いにしておく)。
        QString category = QStringLiteral("ツール");
        for (const ActionSpec &row : m_rows) {
            if (row.kind == ActionKind::Tool && row.tool == meta.type) {
                category = row.category;
                break;
            }
        }

        for (int i = 0; i < list->count(); i++) {
            const QString uid = list->uid(i);
            if (uid.isEmpty()) continue;

            ActionSpec row;
            row.id         = ShortcutRegistry::presetId(meta.type, uid);
            row.label      = list->name(i) + QStringLiteral("  ★長押し対応");
            row.category   = category;
            row.kind       = ActionKind::ToolPreset;
            row.tool       = meta.type;
            row.presetUid  = uid;
            row.defaultKey = 0; // プリセットには既定キーが無い
            row.currentKey = m_registry.presetKeyFor(meta.type, uid);
            m_rows.append(row);
        }
    }
}

// ===========================================================================
// category ごとにページを1枚ずつ作る(出現順)。
// ツールの行にはそのツールのプリセットを子として付け、既定では閉じておく。
// ===========================================================================
void ShortcutsDlg::buildPages()
{
    m_pages.clear();
    m_itemForRow.clear();

    // ---- カテゴリの並び(ツール/コマンドの登録順。プリセットは親に従うので見ない) ----
    QVector<QString> categories;
    for (const ActionSpec &row : m_rows) {
        if (row.kind == ActionKind::ToolPreset) continue;
        if (!categories.contains(row.category)) categories.append(row.category);
    }
    // 親のツールが1つも登録されていないプリセットだけのカテゴリも拾っておく
    for (const ActionSpec &row : m_rows) {
        if (row.kind != ActionKind::ToolPreset) continue;
        if (!categories.contains(row.category)) categories.append(row.category);
    }

    for (const QString &cat : categories) {
        auto *tree = new QTreeWidget();
        tree->setColumnCount(2);
        tree->setHeaderLabels({ "アクション", "キー" });
        tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        tree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
        tree->header()->resizeSection(1, 160);
        tree->setSelectionMode(QAbstractItemView::SingleSelection);
        tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tree->setFocusPolicy(Qt::NoFocus); // フォーカスはダイアログ自体が持つ(キー入力を受けるため)
        tree->setUniformRowHeights(true);
        tree->setExpandsOnDoubleClick(false); // ダブルクリックでの開閉は誤爆しやすいので矢印だけに任せる

        // ---- 親(ツール/コマンド)の行 ----
        // ツール種別 → その項目。プリセットをぶら下げる先を引くのに使う。
        QHash<int, QTreeWidgetItem *> toolItems;
        for (int i = 0; i < m_rows.size(); i++) {
            const ActionSpec &row = m_rows[i];
            if (row.kind == ActionKind::ToolPreset) continue;
            if (row.category != cat) continue;

            auto *item = new QTreeWidgetItem(tree);
            item->setData(0, kRowIndexRole, i);
            m_itemForRow.insert(i, item);
            if (row.kind == ActionKind::Tool)
                toolItems.insert((int)row.tool, item);
        }

        // ---- 子(ツールプリセット)の行 ----
        for (int i = 0; i < m_rows.size(); i++) {
            const ActionSpec &row = m_rows[i];
            if (row.kind != ActionKind::ToolPreset) continue;
            if (row.category != cat) continue;

            QTreeWidgetItem *parent = toolItems.value((int)row.tool, nullptr);
            auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
            item->setData(0, kRowIndexRole, i);
            m_itemForRow.insert(i, item);

            // 既にキーが割り当たっているプリセットは、閉じたままだと気づけないので
            // その親だけ開いておく(それ以外は畳んだまま=既定はツールの行だけ見える)。
            if (parent && row.currentKey != 0)
                parent->setExpanded(true);
        }

        Page page;
        page.title = cat.isEmpty() ? QStringLiteral("その他") : cat;
        page.tree  = tree;
        m_pages.append(page);

        m_categoryList->addItem(page.title);
        m_pageStack->addWidget(tree);

        connect(tree, &QTreeWidget::itemClicked, this,
                [this](QTreeWidgetItem *item, int) {
            if (!item) return;
            const QVariant v = item->data(0, kRowIndexRole);
            if (!v.isValid()) return;
            setEditingRow(v.toInt());
        });
    }

    refreshItems();
}

// ===========================================================================
void ShortcutsDlg::refreshItems()
{
    for (auto it = m_itemForRow.constBegin(); it != m_itemForRow.constEnd(); ++it) {
        const int rowIndex = it.key();
        QTreeWidgetItem *item = it.value();
        if (!item || rowIndex < 0 || rowIndex >= m_rows.size()) continue;

        const ActionSpec &row = m_rows[rowIndex];
        item->setText(0, row.label);
        item->setTextAlignment(1, Qt::AlignVCenter | Qt::AlignCenter);

        if (rowIndex == m_editingRow) {
            const QColor hl(86, 149, 221, 40);
            item->setBackground(0, hl);
            item->setBackground(1, hl);
            item->setText(1, "▶ キーを押してください");
            item->setForeground(1, QColor(86, 149, 221));
        } else {
            item->setBackground(0, QBrush());
            item->setBackground(1, QBrush());
            item->setText(1, keyName(row.currentKey));
            item->setForeground(1, QBrush());
        }
    }
}

// ===========================================================================
void ShortcutsDlg::setEditingRow(int rowIndex)
{
    m_editingRow = rowIndex;
    m_clearBtn->setEnabled(rowIndex >= 0);
    m_hintLabel->setText(rowIndex >= 0
        ? QString("「%1」に割り当てるキーを押してください  /  Esc でキャンセル")
              .arg(m_rows[rowIndex].label)
        : "");
    refreshItems();
}

// ===========================================================================
void ShortcutsDlg::keyPressEvent(QKeyEvent *event)
{
    if (m_editingRow < 0) {
        QDialog::keyPressEvent(event);
        return;
    }

    const int key = event->key();

    if (key == Qt::Key_Escape) {
        setEditingRow(-1);
        return;
    }

    // 修飾キー単体は無視（組み合わせの入力途中なので）
    if (key == Qt::Key_Control || key == Qt::Key_Shift ||
        key == Qt::Key_Alt     || key == Qt::Key_Meta) {
        return;
    }

    // 修飾キーとの組み合わせを含めたキーコードを作る
    const int modifiers = event->modifiers() & ~Qt::KeypadModifier;
    const int fullKey   = key | modifiers;

    assignKey(fullKey);
}

// ===========================================================================
// 重複チェックは m_rows 全体をまたぐので、別カテゴリやツールプリセットとの
// 衝突もここで拾える。
// ===========================================================================
void ShortcutsDlg::assignKey(int key)
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (i == m_editingRow) continue;
        if (m_rows[i].currentKey != key) continue;

        const auto ans = QMessageBox::question(
            this,
            "キーの競合",
            QString("「%1」はすでに「%2」(%3)に割り当てられています。\n上書きしますか？")
                .arg(keyName(key))
                .arg(m_rows[i].label)
                .arg(m_rows[i].category),
            QMessageBox::Yes | QMessageBox::No
        );
        if (ans != QMessageBox::Yes) {
            setEditingRow(-1);
            return;
        }
        // 上書き：競合行をクリア
        m_rows[i].currentKey = 0;
        break;
    }

    m_rows[m_editingRow].currentKey = key;
    setEditingRow(-1); // 入力完了
}

// ===========================================================================
void ShortcutsDlg::resetDefaults()
{
    // ツールプリセットには既定キーが無いので、defaultKey(=0)に戻す＝解除になる。
    for (ActionSpec &row : m_rows)
        row.currentKey = row.defaultKey;
    setEditingRow(-1);
}

// ===========================================================================
// OKが押された時点で初めてregistryへ反映する。setKey()はコマンドなら
// 対応するQActionのshortcutをその場で更新するので、閉じた瞬間から
// 新しいキーが有効になる(再起動不要)。
// ===========================================================================
void ShortcutsDlg::accept()
{
    for (const ActionSpec &row : m_rows) {
        if (row.kind == ActionKind::ToolPreset)
            m_registry.setPresetKey(row.tool, row.presetUid, row.currentKey);
        else
            m_registry.setKey(row.id, row.currentKey);
    }

    QSettings settings;
    m_registry.save(settings);

    QDialog::accept();
}
