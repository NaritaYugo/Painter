#pragma once

#include "tools/core/ImagePresets.h"

// ===========================================================================
// PaperTexPresets
// ---------------------------------------------------------------------------
// 同梱している紙質テクスチャの一覧。ツール設定の紙質ピッカー
// (ImagePresetPicker)が並べる。
//
// 画像は 512x512 のグレースケールPNGで、明るいほどインクが乗る
// (stroke.comp は .r をブラシの濃度へ掛ける)。紙の目はキャンバス座標に
// 貼り付けて繰り返すので、上下左右がシームレスに繋がっていること
// (同梱ぶんはFFTによる周期ノイズで作ってあり、定義上必ず繋がる)。
//
// 画像を足すときは resources/resources.qrc にも <file> を1行足すこと。
// ===========================================================================
namespace PaperTexPresets {

inline const ImagePresetList &all()
{
    static const ImagePresetList list = {
        { QStringLiteral(":/textures/paper/drawing.png"),    QStringLiteral("画用紙") },
        { QStringLiteral(":/textures/paper/watercolor.png"), QStringLiteral("水彩紙") },
        { QStringLiteral(":/textures/paper/rough.png"),      QStringLiteral("ざら紙") },
        { QStringLiteral(":/textures/paper/washi.png"),      QStringLiteral("和紙") },
        { QStringLiteral(":/textures/paper/canvas.png"),     QStringLiteral("キャンバス") },
        { QStringLiteral(":/textures/paper/cloth.png"),      QStringLiteral("布目") },
        { QStringLiteral(":/textures/paper/grain.png"),      QStringLiteral("砂目") },
    };
    return list;
}

// 未設定(=紙質を使わない)を表す表示名。パスは空文字列。
inline QString noneLabel() { return QStringLiteral("なし"); }

inline QString labelFor(const QString &path)
{
    return path.isEmpty() ? noneLabel() : imagePresetLabel(all(), path);
}

} // namespace PaperTexPresets
