#include "tools/core/ToolRegistry.h"

const QVector<ToolTypeMeta> &toolTypeRegistry()
{
    static const QVector<ToolTypeMeta> table = {
        // { type, iconPath(ツールバー用), label, settingsKey, cursorIconPath(カーソル用。空ならiconPathを流用) }
        { ToolType::Pen,     ":/icons/tool/quill.png",   "ペン",     "pen",     "" },
        { ToolType::Eraser,  ":/icons/tool/eraser.png",  "消しゴム", "eraser",  "" },
        // 検証用: 1px縁取り版と2px縁取り版のどちらが見やすいか比較するため、
        // 一時的にスポイトと塗りつぶしへそれぞれ割り当てている
        { ToolType::Fill,    ":/icons/tool/fill.png",    "塗りつぶし", "fill",  ":/icons/cursor/fill.png" },
        { ToolType::Dropper, ":/icons/tool/dropper.png", "スポイト", "dropper", ":/icons/cursor/dropper.png" },
        { ToolType::Move,    ":/icons/tool/move.png",    "移動",     "move",    ":/icons/cursor/move.png" },
        { ToolType::Rotate,  ":/icons/tool/rotate.png",  "回転",     "rotate",  ":/icons/cursor/rotate.png" },
        { ToolType::Blur,    ":/icons/tool/blur.png",   "ぼかし",   "blur",    "" },
        { ToolType::Warp,    ":/icons/tool/warp.png",  "ゆがみ",   "warp",    "" },
        { ToolType::Selection, ":/icons/tool/lasso.png",  "選択",     "selection", ":/icons/cursor/lasso.png" },
        { ToolType::Text,    ":/icons/tool/text.png",    "テキスト", "text",    ":/icons/cursor/text.png" },
        // カーソルはペンと同じ円形(AirbrushTool::cursor()がCursorUtils::makeCircleCursorで
        // 独自に返すため、ここは未使用のフォールバック用に空にしておく)。
        { ToolType::Airbrush, ":/icons/tool/airBrush.png", "エアブラシ", "airbrush", "" },
    };
    return table;
}

QString toolTypeLabel(ToolType type)
{
    for (const ToolTypeMeta &m : toolTypeRegistry())
        if (m.type == type) return m.label;
    return QString();
}

QString toolTypeSettingsKey(ToolType type)
{
    for (const ToolTypeMeta &m : toolTypeRegistry())
        if (m.type == type) return m.settingsKey;
    return QString();
}
