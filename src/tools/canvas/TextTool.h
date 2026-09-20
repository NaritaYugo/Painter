#pragma once
#include "tools/core/Tool.h"

#include <QVector2D>
#include <optional>

// ---------------------------------------------------------------------------
// TextTool
// ---------------------------------------------------------------------------
// アクティブレイヤーがテキストレイヤーのときだけ動作する。1つのテキストレイヤーは
// 複数の独立したテキストボックス(layer.textBoxes)を持てる:
//   ・空いている場所をクリック         -> 新規ボックスを作成し、即座に編集パネルを開く
//   ・既存ボックスの内側をクリック     -> そのボックスを選択し、ドラッグで移動
//   ・選択中ボックスの四隅をドラッグ   -> 中心からの距離比で一様スケール(フォント
//                                        サイズを含め見た目全体が比例して伸縮する)
//   ・選択中ボックスの周囲(枠のすぐ外)をドラッグ -> 回転
//   ・ボックスをダブルクリック         -> 編集パネルを開く(再編集)
// 移動中は他ボックスの端/キャンバス中央(x,y各軸)にスナップする。
// 実際のパネル管理・ラスタライズはCanvasWidget側(ToolContext::startOrEditTextBox/
// requestTextRasterize経由)が担当する。
// ---------------------------------------------------------------------------
class TextTool : public Tool
{
public:
    void onMousePress(QMouseEvent *event, ToolContext &ctx)        override;
    void onMouseMove(QMouseEvent *event, ToolContext &ctx)         override;
    void onMouseRelease(QMouseEvent *event, ToolContext &ctx)      override;
    void onMouseDoubleClick(QMouseEvent *event, ToolContext &ctx)  override;
    bool isActive() const override { return dragMode_ != DragMode::None; }

    void paintOverlay(QPainter &painter, const ToolContext &ctx) const override;

private:
    enum class DragMode { None, Move, Corner, Rotate };
    DragMode dragMode_     = DragMode::None;
    int      activeCorner_ = -1; // Cornerドラッグでどの頂点か(0=TL,1=TR,2=BR,3=BL)

    int selectedLayerIndex_ = -1;
    int selectedBox_        = -1;

    // Moveドラッグ用: 開始時のマウス位置とボックス中心(絶対量で毎フレーム再計算する
    // ことで、スナップの吸着/解除を繰り返してもズレが蓄積しないようにする)。
    QVector2D pressMouseCanvas_;
    float dragStartCx_ = 0.0f, dragStartCy_ = 0.0f;

    // Cornerドラッグ用: 開始時のスケールと、中心からドラッグ開始点までの距離。
    float dragStartScale_     = 1.0f;
    float dragStartMouseDist_ = 1.0f;

    // Rotateドラッグ用
    QVector2D rotateCenter_;
    QVector2D lastMouseCanvas_;

    // 移動中にスナップが発生している軸のガイド線位置(キャンバスpx座標)。
    // 発生していなければnullopt(paintOverlay()で描画するかどうかに使う)。
    std::optional<float> snapGuideX_;
    std::optional<float> snapGuideY_;

    // selectedLayerIndex_/selectedBox_が現在のアクティブレイヤー/textBoxesの
    // 範囲と矛盾していないか確認し、矛盾していれば選択解除する。
    void validateSelection(ToolContext &ctx);
};
