#pragma once

#include <QDialog>
#include <QList>
#include <QVector>
#include <QHash>
#include "shortcuts/ActionSpec.h"

class QListWidget;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QLabel;
class ShortcutRegistry;
class ToolConfig;

// ===========================================================================
// ShortcutsDialog  ―  ショートカットキー設定ダイアログ
//
// 画面構成は環境設定(SettingsDialog)に合わせてある: 左にカテゴリ一覧、右にその
// カテゴリの項目。カテゴリは ShortcutRegistry に登録された category から
// 自動生成されるので、新しいショートカットが増えても(新しいカテゴリを
// 使い始めても)このダイアログ側は一切変更不要。
//
// 「ツール」のページだけは2階層になっていて、ツールの行を開くと、そのツールの
// ツールプリセットが子として現れる(既定は閉じた状態)。
//   ・ツールにキーを割り当てた場合  … そのツールで最後に選ばれていたプリセットが選ばれる
//   ・プリセットにキーを割り当てた場合… 押した瞬間にそのツールのそのプリセットへ直接切り替わる
// どちらも長押しで一時的な切り替えになり、離すと元に戻る。
//
// 行をクリック → 「キーを押してください」状態になり、キーを押すと
// ローカルコピー(m_rows)に反映される。OKを押した時点で初めて registry へ
// 書き戻して確定・即時反映し、設定を保存する。キャンセル時はローカルコピーを
// 破棄するだけなので、registryには一切影響しない。
// 重複チェックは m_rows 全体(=全カテゴリ+全プリセット)をまたいで行う。
// ===========================================================================
class ShortcutsDialog : public QDialog
{
    Q_OBJECT
public:
    // toolCfg はツールプリセットの行を作るために使う。nullptr を渡した場合は
    // プリセットの行を出さない(ツール設定がまだ無い状況でも開けるように)。
    explicit ShortcutsDialog(ShortcutRegistry &registry, ToolConfig *toolCfg,
                          QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    ShortcutRegistry &m_registry;
    ToolConfig       *m_toolCfg = nullptr;

    QListWidget    *m_categoryList = nullptr;
    QStackedWidget *m_pageStack    = nullptr;
    QLabel         *m_hintLabel    = nullptr;
    QPushButton    *m_clearBtn     = nullptr;
    QPushButton    *m_resetBtn     = nullptr;

    struct Page {
        QString      title;
        QTreeWidget *tree = nullptr;
    };
    QVector<Page> m_pages;

    // 編集中のローカルコピー(全カテゴリぶんを1本で持つ)。
    QList<ActionSpec> m_rows;
    int m_editingRow = -1; // 入力待ち行 (-1=なし、値は m_rows のインデックス)

    // m_rows のインデックス → 対応するツリー項目(表示更新と所属ページの特定に使う)
    QHash<int, QTreeWidgetItem *> m_itemForRow;

    void buildRows();
    void buildPages();
    void refreshItems();
    void setEditingRow(int rowIndex);
    void assignKey(int key);
    void resetDefaults();
    void accept() override;

    static QString keyName(int key);
};
