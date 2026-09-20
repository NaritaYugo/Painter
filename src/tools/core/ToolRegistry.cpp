#include "tools/core/ToolRegistry.h"

const QVector<ToolTypeMeta> &toolTypeRegistry()
{
    static const QVector<ToolTypeMeta> table = {
        // type, icon, label, settings key, cursor icon
        { ToolType::Pen,     ":/icons/tool/quill.png",   "ペン",     "pen",     "" },
        { ToolType::Eraser,  ":/icons/tool/eraser.png",  "消しゴム", "eraser",  "" },
        { ToolType::Fill,    ":/icons/tool/fill.png",    "塗りつぶし", "fill",  ":/icons/cursor/fill.png" },
        { ToolType::Dropper, ":/icons/tool/dropper.png", "スポイト", "dropper", ":/icons/cursor/dropper.png" },
        { ToolType::Move,    ":/icons/tool/move.png",    "移動",     "move",    ":/icons/cursor/move.png" },
        { ToolType::Rotate,  ":/icons/tool/rotate.png",  "回転",     "rotate",  ":/icons/cursor/rotate.png" },
        { ToolType::Blur,    ":/icons/tool/blur.png",   "ぼかし",   "blur",    "" },
        { ToolType::Warp,    ":/icons/tool/warp.png",  "ゆがみ",   "warp",    "" },
        { ToolType::Selection, ":/icons/tool/lasso.png",  "選択",     "selection", ":/icons/cursor/lasso.png" },
        { ToolType::Text,    ":/icons/tool/text.png",    "テキスト", "text",    ":/icons/cursor/text.png" },
        // AirbrushToolが円形カーソルを生成する。
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
