#pragma once

#include <QtGlobal>

// ---------------------------------------------------------------------------
// BrushShape
// ---------------------------------------------------------------------------
// stroke.comp の sampleBrushAlpha() が実際に塗る「外周半径」をCPU側でも求める。
//
// ブラシの外周は公称の半径ぴったりではない:
//   ・硬い側 … 縁が二値にならないよう最低1px(kEdgeAA)の階調を持たせている
//   ・柔らかい側 … 細いブラシでも硬さが効くよう、フォールオフ幅の基準に
//                  kSoftRefPx の下限を設けており、幅が半径を超えた分の半分は
//                  外側へ広がる
// そのため、ディスパッチ範囲やUndo範囲を公称半径だけで求めると、ブラシの外周が
// 矩形に切り取られてしまう(細いブラシを柔らかくすると顕著)。
//
// 【重要】ここの定数と式は resources/shaders/paint/stroke.comp と一致させること。
// 片方だけ変えると、切り取られたり無駄に広い範囲を処理したりする。
// ---------------------------------------------------------------------------
namespace BrushShape {

inline constexpr float kEdgeAA    = 1.0f; // 縁に必ず確保する遷移幅(px)
inline constexpr float kSoftRefPx = 8.0f; // フォールオフ幅の基準長へ加算する下駄(px)
inline constexpr float kSoftOutwardRatio   = 0.15f; // 超過分を外周へ逃がす割合
inline constexpr float kMaxFalloffVsRadius = 3.0f;  // フォールオフ幅の上限(半径の倍数)

// 硬さ hardness(0=最も柔らかい 〜 1=最も硬い)のときのフォールオフ幅(px)。
inline float falloffPx(float radius, float hardness)
{
    const float w = qMax((1.0f - hardness) * (radius + kSoftRefPx), kEdgeAA);
    // 中心まで薄くなってブラシが消えないよう頭打ちにする
    return qMin(w, qMax(radius * kMaxFalloffVsRadius, kEdgeAA));
}

// 実際にアルファが0より大きくなりうる外周半径(px)。
inline float outerRadius(float radius, float hardness)
{
    return radius + qMax(0.0f, (falloffPx(radius, hardness) - radius) * kSoftOutwardRatio);
}

} // namespace BrushShape
