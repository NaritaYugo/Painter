#pragma once

#include <QColor>

// ---------------------------------------------------------------------------
// レイヤーマスク編集中に「マスクへ焼き込む色」を決める共通ヘルパー
// ---------------------------------------------------------------------------
// マスクはグレースケール濃度(0=完全に隠す、1=完全に表示)として扱うため、
// ペンは前景色の輝度へ向けて(不透明度ぶんの強さで)塗り、消しゴムは黒(=隠す)
// へ向けて塗る。bake.compの「uBrushColor==vec4(0)なら特別なアルファ消去」分岐は
// 避けたいので、アルファは常に非ゼロにする(alphaが「今回のストロークでどこまで
// 値を寄せるか」の強さを表す。render.frag側の通常ペイントと同じnormalBlend経路)。
//
// 通常ペイントの色(toPreMulColor)と同じく事前乗算済み(rgb = 寄せ先の濃度 × 強さ)
// で返す。bake.comp/render.fragは「uBrushColor × マスク」を一律にsrcとして扱うため、
// ここで乗算しておかないと「寄せる側」だけ強さが掛からず、不透明度を下げても
// 白へ寄せる方向だけは全力で効いてしまう。
//
// この値は2箇所で使われ、両者が一致していないとプレビューと確定結果がずれる:
//   ・焼き込み側 : PenEraserTool::onMouseRelease / AirbrushTool::stampAndBake が
//                  bake.comp の uBrushColor へ渡す
//   ・プレビュー側: CanvasWidget::paintGL が render.frag の uMaskBrushColor へ渡し、
//                  シェーダー側が bake.comp と同じ式でマスク値を先に反映して描く
// そのため定義はここ1箇所だけに置く。
// ---------------------------------------------------------------------------
inline QColor maskBrushColor(bool isEraser, const QColor &foregroundColor, float opacity)
{
    const float luminance = 0.299f * foregroundColor.redF()
                          + 0.587f * foregroundColor.greenF()
                          + 0.114f * foregroundColor.blueF();
    const float v = (isEraser ? 0.0f : luminance) * opacity;
    return QColor::fromRgbF(v, v, v, opacity);
}
