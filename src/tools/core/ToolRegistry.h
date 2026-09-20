#pragma once

#include "tools/core/ToolType.h"

#include <QString>
#include <QVector>

// ToolTypeMeta / toolTypeRegistry()「存在するツール一覧」を1箇所にまとめたテーブル。
struct ToolTypeMeta {
    ToolType type;
    QString  iconPath;
    QString  label;       // UIに出す日本語名
    QString  settingsKey; // QSettings上のグループ名(英数字、enum順序に依存しない安定キー)
    // カーソル用に別画像を使いたい場合だけ指定する。
    QString  cursorIconPath;
};

const QVector<ToolTypeMeta> &toolTypeRegistry();

QString toolTypeLabel(ToolType type);
QString toolTypeSettingsKey(ToolType type);
