#pragma once

#include "tools/core/ToolType.h"

#include <QString>
#include <QVector>

// ===========================================================================
// ToolTypeMeta / toolTypeRegistry()
// ---------------------------------------------------------------------------
// 「存在するツール一覧」を1箇所にまとめたテーブル。ToolDock(ボタン一覧)・
// ToolPresetDock(タイトル表示)・ToolConfig(設定の保存/復元キー)が全員
// このテーブルを参照するので、新しいツールを追加するときはここに1行足せば
// 各UIへの反映漏れが起きにくくなる。
// ---------------------------------------------------------------------------
struct ToolTypeMeta {
    ToolType type;
    QString  iconPath;
    QString  label;       // UIに出す日本語名
    QString  settingsKey; // QSettings上のグループ名(英数字、enum順序に依存しない安定キー)
    // カーソル用に別画像を使いたい場合だけ指定する。空ならiconPathを流用する
    // (ツールバーのボタン絵とカーソルは求められる見た目が違う——ボタンは小さく
    // 潰れないシンプルな図形、カーソルは縁取り等で視認性を確保したい——ため分離)。
    QString  cursorIconPath;
};

const QVector<ToolTypeMeta> &toolTypeRegistry();

QString toolTypeLabel(ToolType type);
QString toolTypeSettingsKey(ToolType type);
