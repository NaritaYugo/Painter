#pragma once

#include "tools/core/Tool.h"
#include "tools/core/ToolConfig.h"

#include <QOpenGLFunctions_4_3_Core>
#include <QVector2D>
#include <QVector>

// ---------------------------------------------------------------------------
// SelectTool
// ---------------------------------------------------------------------------
// 「選択」ツール。SelectionToolConfig::mode() に応じて2通りの操作を行う、
// 1つのToolクラス(投げ縄選択とペン選択は別クラスではなく同じSelectToolの
// ツールプリセット違いとして表現している。Fillのreference先切り替えと同じ考え方)。
//
// ・投げ縄(Lasso): ドラッグした軌跡を記録し、離した時に始点/終点を直線で結んで
//   閉じたポリゴンとして扱い、CPU側でスキャンライン塗りつぶしして選択範囲マスクへ書き込む。
// ・ペン選択(PenSelect): ドラッグ中、通常のペンと同じ仕組み(ctx.computeDrawProgram/
//   ctx.maskTex)でストローク範囲を蓄積し、離した時にそのまま選択範囲マスクへコピーする。
//
// どちらも最終的に ctx.selectionMaskTex (R8, canvasW x canvasH) を書き換えるだけなので、
// 「選択範囲内にしか塗れないマスク」として他の全ツールから同じように参照できる。
// ---------------------------------------------------------------------------
class SelectTool : public Tool, protected QOpenGLFunctions_4_3_Core
{
public:
    void initialize(QOpenGLContext *ctx);

    // MainWindowが所有する唯一のToolConfigへの非所有ポインタ(CanvasWidget経由で渡される)
    void setToolConfig(ToolConfig *cfg) { toolCfg_ = cfg; }

    void onMousePress(QMouseEvent *event, ToolContext &ctx)   override;
    void onMouseMove(QMouseEvent *event, ToolContext &ctx)    override;
    void onMouseRelease(QMouseEvent *event, ToolContext &ctx) override;
    bool isActive() const override { return dragging_; }

    // ペン選択はドラッグ中キャンバスの見た目が一切変わらない(ストロークはmaskTexへ
    // 溜まるだけで、render.fragのプレビュー分岐は選択ツール中スキップされる)ため、
    // 定期的な再描画は不要。投げ縄は軌跡の点線をQPainterで描くので必要。
    bool needsCanvasRepaintWhileActive() const override
    {
        return !toolCfg_ || toolCfg_->selection().mode() == SelectionMode::Lasso;
    }

    std::optional<QCursor> cursor(const ToolContext &ctx) const override;
    void paintOverlay(QPainter &painter, const ToolContext &ctx) const override;

private:
    // 今回のドラッグで作った形を、既存の選択範囲へどう反映するか。
    // 修飾キーからonMousePress時に決めて、release時の確定まで保持する。
    //   投げ縄  : 修飾なし=置き換え / Ctrl=追加 / Alt=削減
    //   ペン選択: 修飾なし=追加(Ctrl不要) / Alt=削減
    //   共通    : Ctrl+Shift+クリックで選択解除(Escと同じ)
    enum class CombineMode { Replace, Union, Subtract };

    ToolConfig *toolCfg_ = nullptr;

    bool               dragging_ = false;
    CombineMode        combine_  = CombineMode::Replace;
    QVector2D          lastPos_;
    QVector<QVector2D> lassoPoints_; // 投げ縄の軌跡(キャンバスpx座標)

    // ペン選択: from→to の線分ぶんストロークマスク(ctx.maskTex)へ蓄積する。
    // radiusはディスパッチ範囲をブラシが届く矩形へ絞るために使う(全域ディスパッチを避ける)。
    void dispatchStrokeSegment(ToolContext &ctx, const QVector2D &from, const QVector2D &to,
                               float radius);

    // 投げ縄を閉じてポリゴン塗りつぶしを選択範囲マスクへ書き込む(release時)
    void finishLasso(ToolContext &ctx);
    // ペン選択のストロークマスクを選択範囲マスクへ反映する(release時)
    void finishPenSelect(ToolContext &ctx);

    // 今回描いた形(incoming)をcombine_に従って既存の選択範囲と合成し、確定する。
    // Undoのcommitまでここで面倒を見る(finishLasso/finishPenSelect共通の後半)。
    void commitSelection(ToolContext &ctx, QVector<uint8_t> &&incoming);
    // 選択範囲マスクの部分矩形をCPUへ読み戻す / 書き戻す。
    // 確定処理をキャンバス全域ではなく「実際に変わる矩形」だけに絞るために使う
    // (全域だとフルHDで数百msかかっていた。commitSelectionのコメント参照)。
    QVector<uint8_t> readSelectionRect(ToolContext &ctx, int x, int y, int w, int h);
    void uploadSelectionRect(ToolContext &ctx, const uint8_t *data, int x, int y, int w, int h);
};
