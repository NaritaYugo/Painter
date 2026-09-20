#pragma once

#include "document/CanvasDocument.h"

#include <QString>
#include <QVector>

// ===========================================================================
// BlendModeList
// ---------------------------------------------------------------------------
// 合成モードの表示名と並び順。レイヤーの合成モードコンボ(LayerDock)と
// ブラシの合成モードコンボ(ToolPropDock)が同じ一覧を使うため、1箇所にまとめる。
//
// 並びはPhotoshopのメニュー順で、グループの区切りにセパレーター
// (labelが空のエントリ)を挟む。
// ===========================================================================

struct BlendModeItem {
    QString   label;                    // 空文字列ならセパレーター
    BlendMode mode = BlendMode::Normal; // セパレーターのときは未使用

    bool isSeparator() const { return label.isEmpty(); }
};

inline const QVector<BlendModeItem> &blendModeItems()
{
    static const QVector<BlendModeItem> list = {
        { QStringLiteral("普通"),                  BlendMode::Normal },
        { QStringLiteral("ディザ合成"),             BlendMode::Dissolve },
        { QString(),                               BlendMode::Normal },
        { QStringLiteral("比較(暗)"),              BlendMode::Darken },
        { QStringLiteral("乗算"),                  BlendMode::Multiply },
        { QStringLiteral("焼き込みカラー"),         BlendMode::ColorBurn },
        { QStringLiteral("焼き込み(リニア)"),       BlendMode::LinearBurn },
        { QStringLiteral("暗さの比較"),             BlendMode::DarkerColor },
        { QString(),                               BlendMode::Normal },
        { QStringLiteral("比較(明)"),              BlendMode::Lighten },
        { QStringLiteral("スクリーン"),             BlendMode::Screen },
        { QStringLiteral("覆い焼きカラー"),         BlendMode::ColorDodge },
        { QStringLiteral("覆い焼き(リニア)-加算"),  BlendMode::LinearDodge },
        { QStringLiteral("明るさの比較"),           BlendMode::LighterColor },
        { QString(),                               BlendMode::Normal },
        { QStringLiteral("オーバーレイ"),           BlendMode::Overlay },
        { QStringLiteral("ソフトライト"),           BlendMode::SoftLight },
        { QStringLiteral("ハードライト"),           BlendMode::HardLight },
        { QStringLiteral("ビビッドライト"),         BlendMode::VividLight },
        { QStringLiteral("リニアライト"),           BlendMode::LinearLight },
        { QStringLiteral("ピンライト"),             BlendMode::PinLight },
        { QStringLiteral("ハードミックス"),         BlendMode::HardMix },
        { QString(),                               BlendMode::Normal },
        { QStringLiteral("差の絶対値"),             BlendMode::Difference },
        { QStringLiteral("除外"),                  BlendMode::Exclusion },
        { QStringLiteral("減算"),                  BlendMode::Subtract },
        { QStringLiteral("除算"),                  BlendMode::Divide },
        { QString(),                               BlendMode::Normal },
        { QStringLiteral("色相"),                  BlendMode::Hue },
        { QStringLiteral("彩度"),                  BlendMode::Saturation },
        { QStringLiteral("カラー"),                 BlendMode::Color },
        { QStringLiteral("輝度"),                  BlendMode::Luminosity },
    };
    return list;
}
