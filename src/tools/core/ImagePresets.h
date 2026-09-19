#pragma once

#include <QString>
#include <QVector>

// ===========================================================================
// ImagePresets
// ---------------------------------------------------------------------------
// 「同梱画像の一覧から1枚選ぶ」設定(ブラシ先端画像・紙質テクスチャ)が共通で
// 使う型。一覧そのものは用途ごとのヘッダ(BrushTipPresets / PaperTexPresets)が
// 持ち、ダイアログ(ImagePresetPicker)はこの型だけを見る。
// ===========================================================================

struct ImagePreset {
    QString path;  // リソースパス(:/textures/...)またはファイルシステム上のパス
    QString label; // ピッカーに出す名前
};

using ImagePresetList = QVector<ImagePreset>;

// 同梱プリセットならその名前、そうでなければファイル名を返す(現在の選択の表示用)。
inline QString imagePresetLabel(const ImagePresetList &list, const QString &path)
{
    for (const ImagePreset &e : list) {
        if (e.path == path) return e.label;
    }
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash >= 0 ? path.mid(slash + 1) : path;
}
