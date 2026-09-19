#pragma once

#include <QCursor>
#include <QColor>
#include <QString>

// ---------------------------------------------------------------------------
// CursorUtils
// ---------------------------------------------------------------------------
// 各Toolのcursor()実装やGLWidgetのアイコンフォールバックから共通で使う
// カーソル生成ヘルパー。
// ---------------------------------------------------------------------------
namespace CursorUtils {

// ブラシサイズを表す円カーソル(ペン/消しゴム用)。diameterPx はウィジェット座標
// (画面ピクセル)換算後の直径。中心が実際にブラシが当たる位置になるよう
// ホットスポットを円の中心に置く。
QCursor makeCircleCursor(float diameterPx);

// アイコン画像をそのままカーソルにする(仮のツールカーソル用)。
// hotspotAtBottomLeft=false(既定)ならホットスポットは中心。
// true なら画像の左下(スポイトのように、絵の中の実際に色を拾う/作用する先端が
// 左下に描かれているアイコン向け)。
QCursor makeIconCursor(const QString &iconPath, int size = 32, bool hotspotAtBottomLeft = false);

// アイコン画像の右隣に、指定色のスウォッチを並べて1枚に合成したカーソル
// (スポイトのドラッグ中プレビュー用)。ホットスポットはアイコンの左下=先端。
//
// スウォッチを「カーソル画像の一部」にしてあるのは追従性のため。QPainterの
// オーバーレイとしてキャンバスへ描くと、表示はGLWidgetの再描画(ストローク中は
// 実測コストに応じて12〜100msへ間引かれる)に律速されてカーソルから明確に遅れる。
// カーソル画像ならOSがポインタと完全に同期して動かすので遅れは原理的に生じない。
QCursor makeIconWithSwatchCursor(const QString &iconPath, QColor color, int iconSize = 24);

} // namespace CursorUtils
