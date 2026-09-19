#pragma once

#include "widgets/GLWidget.h" // ToolType

#include <QString>

class QAction;

// ---------------------------------------------------------------------------
// ActionSpec
// ---------------------------------------------------------------------------
// 1つのショートカット可能な操作(ツール切り替え or コマンド)を表す。
// ShortcutRegistry に登録された全ActionSpecが、設定ダイアログの行と
// 実行時のキー割り当ての唯一の情報源になる。
// ---------------------------------------------------------------------------
enum class ActionKind {
    Tool,       // ツール切り替え(長押しで一時切り替え、離すと元のツールに戻る)
    ToolPreset, // ツールプリセットの直接指定(ツール+プリセットを一度に切り替える。長押し対応)
    Command,    // 単発コマンド(QActionのtriggeredで実行される)
};

struct ActionSpec
{
    QString    id;                     // 保存キー・一意識別子 (例: "tool_pen", "cmd_undo")
    QString    label;                  // 表示名
    QString    category;               // 設定ダイアログでのグループ見出し
    int        defaultKey  = 0;        // Qt::Key | modifiers
    int        currentKey  = 0;        // 現在のキー (0 = 未設定)
    ActionKind kind         = ActionKind::Command;
    ToolType   tool         = ToolType::Pen;  // kind == Tool / ToolPreset のとき有効
    QString    presetUid;                     // kind == ToolPreset のとき有効(ToolPresetList::Entry::uid)
    QAction   *action       = nullptr;        // kind == Command のとき、shortcutを反映する先
};
