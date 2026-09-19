#pragma once

#include <QColor>
#include <QImage>
#include <QSize>
#include <QVariantMap>

#include "tools/core/ToolType.h"

// ===========================================================================
// ToolPreview  ―  ツールプリセットの「そのツールで1本描いた見た目」を作る
// ---------------------------------------------------------------------------
// ツールプリセット一覧(ToolPresetDock)の各行に出すプレビュー画像を、保存された
// 設定値そのものから毎回作り直す。あらかじめ用意した画像を貼るのではないので、
// サイズ・硬さ・先端画像・間隔・散布・入り抜き…を変えれば見た目もそのまま変わる。
//
// 描き方はGPU側の実装(stroke.comp の sampleBrushAlpha と、PenEraserTool の
// スタンプの積み方)をCPUでなぞったもの。プレビューは数十pxしかないので
// 実際の描画パイプライン(GLコンテキスト・コンピュートシェーダー)を持ち出さず、
// QImage上で同じ式を回す方が軽くて壊れにくい。完全な一致ではなく
// 「その設定で描いたらこうなる」が伝わることを目標にしている。
//
// 【対応するツール】
//   ペン / エアブラシ / 消しゴム / ぼかし ― いずれも「大きさと形を持つ筆」なので
//   1本のストロークとして描ける。
// 【対応しないツール】
//   移動・回転・塗りつぶし・スポイト・選択・テキスト・ゆがみ ― 1本の線として
//   見せられる形を持たないので、空(透明)の画像を返す。呼び出し側は
//   isNull() を見て何も描かなければよい。
// ===========================================================================
namespace ToolPreview {

// values は ToolPresetList の各エントリの設定値(ConfigT::toMap() と同じ形)。
// ink は「今の描画色」。消しゴムやぼかしのように色を使わないツールでは無視される。
// 戻り値はプリマルチプライドではない通常のARGB32画像。プレビュー不可なら null。
QImage render(ToolType tool, const QVariantMap &values, const QSize &size,
              const QColor &ink, bool inkIsTransparent);

// そのツールがプレビューを作れるか(作れないツールの行でレイアウトを詰めたい場合用)。
bool isSupported(ToolType tool);

} // namespace ToolPreview
