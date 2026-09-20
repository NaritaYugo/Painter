#pragma once

#include "shortcuts/ActionSpec.h"

#include <QList>
#include <QMap>
#include <QSettings>
#include <functional>
#include <optional>

// ---------------------------------------------------------------------------
// ShortcutRegistry
// ---------------------------------------------------------------------------
// アプリ内の「ツール切り替え」「コマンド」ショートカットを一元管理する。
//
// 新しいショートカットを増やしたい場合は、MainWindow::registerShortcuts() に
// registerTool()/registerCommand() を1行足すだけでよい
// (デフォルトキー・設定の保存/読み込み・ダイアログへの表示・重複チェックが
//  自動的についてくるので、MainWindow本体やShortcutsDialogを個別に触る必要はない)。
//
// setKey() は即座に反映される: コマンドなら対応するQActionのshortcutをその場で
// 更新するため、設定ダイアログでOKを押した瞬間から有効になる
// (アプリ再起動が必要だったバグの修正)。
// ---------------------------------------------------------------------------
class ShortcutRegistry
{
public:
    // ツールショートカットを登録する。切り替え自体(長押し判定等)は
    // 呼び出し側(MainWindowのeventFilter)が toolForKey() を使って行う。
    void registerTool(ToolType tool, const QString &id, const QString &label,
                       const QString &category, int defaultKey);

    // コマンドを登録する。QActionはこのメソッドが生成し(親はactionParent)、
    // triggered → onTriggered の接続・shortcutの管理までまとめて面倒を見る。
    // 呼び出し側はメニューに積むためのQAction*だけ受け取ればよい。
    // (キーと振る舞いを1呼び出しに並べて書けるのがポイント。詳しくは
    //  MainWindow::setupActions() を参照)
    QAction *registerCommand(QObject *actionParent, const QString &id, const QString &label,
                              const QString &category, int defaultKey,
                              std::function<void()> onTriggered);

    // 登録順の全アクション(設定ダイアログ表示用)
    const QList<ActionSpec> &actions() const { return actions_; }

    // id からキーを取得/変更する。setKey は即座にQActionへ反映される。
    int  keyFor(const QString &id) const;
    void setKey(const QString &id, int newKey);

    // ツール切り替えに現在割り当てられているキーを取得する(ToolDockのツールチップ表示用)。
    // 未割り当てなら0。
    int keyForTool(ToolType tool) const;

    // 現在そのキーを使っているアクションのidを返す(競合検出用)。無ければ空文字。
    QString idForKey(int key) const;

    // キーからツールを引く(eventFilter用)。ツール以外/未割り当てならnullopt。
    std::optional<ToolType> toolForKey(int key) const;

    // ------------------------------------------------------------------
    // ツールプリセットのショートカット
    // ------------------------------------------------------------------
    // ツールやコマンドと違い、プリセットは実行時に追加・複製・削除・改名される
    // ため、起動時に固定のActionSpecとして登録できない。そこで「どのツールの
    // どのプリセットか」を指す文字列(presetId)とキーの対応表だけをここで持ち、
    // 設定ダイアログに並べる一覧は ToolConfig から都度組み立てる。
    // 割り当て先はインデックスではなくプリセットのuid(不変)なので、
    // 並び替え・削除・改名をしてもショートカットは同じプリセットを指し続ける。
    struct PresetRef { ToolType tool = ToolType::Pen; QString uid; };

    static QString presetId(ToolType tool, const QString &uid);
    static bool    parsePresetId(const QString &id, PresetRef &out);

    void setPresetKey(ToolType tool, const QString &uid, int newKey);
    int  presetKeyFor(ToolType tool, const QString &uid) const;
    void clearPresetKeys();

    // キーからプリセットを引く(eventFilter用)。プリセット以外/未割り当てならnullopt。
    std::optional<PresetRef> presetForKey(int key) const;

    // 全アクションをデフォルトキーに戻す。
    void resetToDefaults();

    // QSettings の "shortcuts" グループへ保存/から読み込む。
    // save() は書き込み前にグループを一旦丸ごとクリアしてから書き直すので、
    // ツール構成が違った時期の残留キーが溜まり続けることはない。
    void load(QSettings &settings);
    void save(QSettings &settings) const;

private:
    QList<ActionSpec>  actions_;
    QMap<QString, int> idToIndex_;
    // キー → id。ツール/コマンドのidに加えてプリセットのpresetIdも入れてあるので、
    // idForKey() による重複検出は3種類すべてをまたいで効く。
    QMap<int, QString> keyToId_;
    QMap<QString, int> presetToKey_; // presetId → キー

    static void applyKeyToAction(ActionSpec &spec);
};
