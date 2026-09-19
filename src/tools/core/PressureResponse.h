#pragma once

#include <QtGlobal>

// ===========================================================================
// PressureResponse
// ---------------------------------------------------------------------------
// 筆圧(0..1)を「最小値〜最大値」の範囲へ写す共通の式。
//
// 筆圧をそのまま太さ/濃さへ掛けると、筆圧0付近で線が消えてしまい、入り抜きが
// 効きすぎて実用しづらい。そこで「筆圧0のときに何%残すか」を設定できるように
// する(SAI/CLIP STUDIO PAINT/Procreate の「最小サイズ」「最小濃度」と同じ考え方。
// いずれも最大値に対する比率で指定する)。
//
//   minRatio = 0.0 … 従来どおり筆圧そのまま(筆圧0で消える)
//   minRatio = 0.5 … 筆圧0でも半分残る
//   minRatio = 1.0 … 筆圧を無視して常に最大
//
// カーブ(PressureCurve)は「筆圧そのものの入出力特性」、こちらは「その結果を
// どの範囲へ割り当てるか」で役割が違う。カーブを通した後の値をここへ渡す。
// ===========================================================================
namespace PressureResponse {

inline float scale(float pressure, float minRatio)
{
    const float p = qBound(0.0f, pressure, 1.0f);
    const float m = qBound(0.0f, minRatio, 1.0f);
    return m + (1.0f - m) * p;
}

} // namespace PressureResponse
