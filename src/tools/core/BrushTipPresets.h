#pragma once

#include "tools/core/ImagePresets.h"

// ===========================================================================
// BrushTipPresets
// ---------------------------------------------------------------------------
// 同梱している先端(スタンプ)画像の一覧。ツール設定の先端画像ピッカー
// (ImagePresetPicker)が並べる。
//
// 画像は 256x256 のRGBA PNGで、形はアルファチャンネルだけで表す(stroke.compは
// .a しか見ない)。stroke.comp はこの画像の「内接円」をスタンプの外周へ写すので、
// 絵は中心から半径128の円の内側に収めること。外へはみ出した部分は
// dist > outer の判定で切り落とされる。
//
// 画像を足すときは resources/resources.qrc にも <file> を1行足すこと。
// ===========================================================================
namespace BrushTipPresets {

inline const ImagePresetList &all()
{
    static const ImagePresetList list = {
        { QStringLiteral(":/textures/penTip/circle.png"),   QStringLiteral("円") },
        { QStringLiteral(":/textures/penTip/square.png"),   QStringLiteral("四角") },
        { QStringLiteral(":/textures/penTip/flat.png"),     QStringLiteral("平筆") },
        { QStringLiteral(":/textures/penTip/pencil.png"),   QStringLiteral("鉛筆") },
        { QStringLiteral(":/textures/penTip/chalk.png"),    QStringLiteral("チョーク") },
        { QStringLiteral(":/textures/penTip/charcoal.png"), QStringLiteral("木炭") },
        { QStringLiteral(":/textures/penTip/bristle.png"),  QStringLiteral("毛筆") },
        { QStringLiteral(":/textures/penTip/spray.png"),    QStringLiteral("スプレー") },
        { QStringLiteral(":/textures/penTip/leaf.png"),     QStringLiteral("葉") },
        { QStringLiteral(":/textures/penTip/star.png"),     QStringLiteral("星") },
    };
    return list;
}

inline QString labelFor(const QString &path) { return imagePresetLabel(all(), path); }

} // namespace BrushTipPresets
