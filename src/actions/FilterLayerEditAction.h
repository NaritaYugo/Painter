#pragma once

#include "actions/CanvasAction.h"
#include "document/CanvasDocument.h"

class GaussianBlurPanel;
class MotionBlurPanel;
class MosaicPanel;
class NoisePanel;
#ifdef TIEPOLO_PRO_BUILD
class ChromaticAberrationPanel;
class LensBlurPanel;
#endif

// ---------------------------------------------------------------------------
// FilterLayerEditAction
// ---------------------------------------------------------------------------
// フィルターレイヤー(LayerType::Filter)のパラメータをその場で編集するアクション。
// レイヤー自体はLayerDockの「新規フィルターレイヤー」で先に作成済みのものを使い、
// レイヤーリストでそのプレビューをダブルクリックすると開く
// (AdjustmentLayerEditAction / SolidColorLayerEditAction と同じ構造)。
//
// 破壊的フィルターのアクション(GaussianBlurAction/ChromaticAberrationAction)と
// 違い、確定してもピクセルへは一切焼き込まない ―― パラメータを残すだけで、効果は
// 表示のたびに CanvasWidget::rebuildFilterChain() がかけ直す。キャンセルは編集開始
// 時点のパラメータへ戻すだけ(レイヤー自体は消さない)。
//
// AdjustmentLayerEditAction(種類ごとに bcPanel_/hslPanel_ を使い分ける)と同じ
// 構造で、種類(FilterKind)ごとにパネルを使い分ける:
//   ・GaussianBlur … GaussianBlurPanel(半径スライダー1本、無料版でも常に使える)
//   ・MotionBlur … フィルターメニュー版と同じ MotionBlurPanel(無料版でも常に使える。
//     円形モードの中心は数値入力を持たず、キャンバス上のハンドルをドラッグして
//     決める。保持するのはキャンバスに対する比率 mbCenterU/V)。
//   ・ChromaticAberration … フィルターメニュー版と同じ ChromaticAberrationPanel
//     (Pro限定。円形モードの中心は数値入力を持たず、キャンバス上のハンドルを
//     ドラッグして決める。フィルターレイヤーは特定のレイヤー矩形に紐付かないので、
//     保持するのはキャンバスに対する比率 caCenterU/V)。
//   ・LensBlur … フィルターメニュー版と同じ LensBlurPanel(Pro限定。中心ハンドルは
//     持たずパネルのみで完結する)。
//   ・Mosaic … フィルターメニュー版と同じ MosaicPanel(無料版でも常に使える。
//     ブロックサイズスライダー1本)。
//   ・Noise … フィルターメニュー版と同じ NoisePanel(無料版でも常に使える。
//     強さ・種類・粒の大きさ)。乱数の種(nsSeed)はレイヤー生成時に決めたものを
//     そのまま使い続けるので、このパネル自体は種の管理をしない。
// このクラス自体は無料版・Pro版どちらでも常に登録される(GaussianBlur/MotionBlur/
// Mosaic/Noiseは無料版機能のため)。ChromaticAberration/LensBlur関連のメンバ・
// 処理だけを #ifdef TIEPOLO_PRO_BUILD で囲み、それらのライセンス確認は呼び出し側
// (CanvasWidget::editFilterLayer)がアクション開始前に行う。
// ---------------------------------------------------------------------------
class FilterLayerEditAction : public CanvasAction
{
public:
    FilterLayerEditAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    bool blocksAllToolInput() const override { return true; }
    void setTarget(int layerIndex) { targetLayerIndex_ = layerIndex; }
    void positionPanel() override;
    bool paintOverlay(QPainter &painter, const ToolContext &ctx) override;
    bool handleMousePress  (QMouseEvent *e, ToolContext &ctx) override;
    bool handleMouseMove   (QMouseEvent *e, ToolContext &ctx) override;
    bool handleMouseRelease(QMouseEvent *e, ToolContext &ctx) override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    // 円形モードの中心(キャンバスpx座標)。現在の種類(ChromaticAberrationなら
    // caCenterU/V、MotionBlurならmbCenterU/V)とキャンバスサイズから求める。
    // (種類ごとの専用ハンドルだが、宣言自体はPro限定にする必要が無いので常に持つ)
    QVector2D centerCanvasPx(const ToolContext &ctx) const;

    int targetLayerIndex_ = -1;
    FilterParams backup_;
    bool draggingCenter_ = false;
    GaussianBlurPanel *blurPanel_ = nullptr;
    MotionBlurPanel   *motionBlurPanel_ = nullptr;
    MosaicPanel       *mosaicPanel_ = nullptr;
    NoisePanel        *noisePanel_ = nullptr;
#ifdef TIEPOLO_PRO_BUILD
    ChromaticAberrationPanel *caPanel_ = nullptr;
    LensBlurPanel             *lensBlurPanel_ = nullptr;
#endif
};
