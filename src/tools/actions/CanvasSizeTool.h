#pragma once
#include "tools/core/Tool.h"
#include <QVector2D>
#include <QRectF>

// ---------------------------------------------------------------------------
// CanvasSizeTool
// ---------------------------------------------------------------------------
// 編集メニューの「キャンバスサイズ変更」アクション本体。TransformTool同様、
// キャンバス上の枠(8箇所のハンドル)をドラッグして直接トリミング/拡張できるほか、
// CanvasSizePanel(実際のQWidget: 9箇所のアンカー選択 + 幅/高さの数値入力)からも
// setAnchorAndSize() 経由で操作できる。どちらの操作も同じ矩形状態
// (x0_,y0_,w_,h_ : 旧キャンバス座標系での提案矩形)を更新する。
// 実データへの書き込みは無く、確定(confirm)時に GLWidget::resizeCanvasKeepingContent
// を1回呼ぶだけ(ピクセル位置の移動を伴うため、Transform系のようなプレビュー用シェーダーは
// 使わず、QPainterでの矩形オーバーレイのみでプレビューする)。
// ---------------------------------------------------------------------------
class CanvasSizeTool : public Tool
{
public:
    void activate(ToolContext &ctx);   // アクション開始(現在のキャンバスサイズで等倍にリセット)
    void deactivate();                 // キャンセル(GPUには一切書き込まない)
    void confirm(ToolContext &ctx);    // 実際にキャンバスをリサイズして終了する

    void onMousePress(QMouseEvent *event, ToolContext &ctx)   override;
    void onMouseMove(QMouseEvent *event, ToolContext &ctx)    override;
    void onMouseRelease(QMouseEvent *event, ToolContext &ctx) override;
    bool isActive() const override { return activeHandle_ != -1; }

    void paintOverlay(QPainter &painter, const ToolContext &ctx) const override;

    bool engaged() const { return engaged_; }

    // CanvasSizePanel(9箇所アンカー + 幅/高さスピンボックス)からの設定を反映する
    void setAnchorAndSize(int anchorIndex, int newW, int newH);

    int newWidth()  const { return w_; }
    int newHeight() const { return h_; }
    int newX0()     const { return x0_; }
    int newY0()     const { return y0_; }

private:
    bool engaged_ = false;

    int origW_ = 0, origH_ = 0; // 元のキャンバスサイズ(アンカー計算の基準、activate()時に固定)
    int x0_ = 0, y0_ = 0;       // 提案中の新キャンバス矩形の左上(旧キャンバス座標系)
    int w_ = 0, h_ = 0;         // 提案中の新キャンバス矩形のサイズ
    int anchorIndex_ = 4;       // 0..8 (行優先: 0=左上...4=中央...8=右下)

    int activeHandle_ = -1; // -1=非ドラッグ中、0..7でドラッグ中のハンドル
    int dragFixedX0_ = 0, dragFixedY0_ = 0, dragFixedX1_ = 0, dragFixedY1_ = 0; // ドラッグ開始時点の矩形(クランプ基準)

    static QVector2D handlePoint(int index, int x0, int y0, int w, int h);
};
