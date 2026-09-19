#pragma once

#include "tools/core/Tool.h"
#include "tools/core/ToolConfig.h"

#include <QOpenGLFunctions_4_3_Core>
#include <QVector2D>
#include <QRectF>
#include <array>

// ---------------------------------------------------------------------------
// FreeTransformTool
// ---------------------------------------------------------------------------
// 「自由変形」アクション(編集メニュー、Ctrl+Shift+Tで開始する一回限りの操作。
// ツールバーの常設ツールではない)。TransformTool(拡大・縮小・回転)と違い、
// bboxの4頂点を個別にドラッグしてシアー/自由変形できる
// (双一次補間によるクアッド歪み。invBilinear/共通GLSLヘルパーで逆変換する)。
// 移動(内側ドラッグ)/回転(外側ドラッグ)はTransformToolと同じ操作性。
//
// 操作中はGPU上の実データには一切触れず、corners_(4頂点の現在位置)だけを
// CPU側で保持する。render.fragが毎フレームこれを使ってプレビューを合成し、
// paintOverlay()が枠線・ハンドル・確定/キャンセルボタンを描く。確定して
// 初めてfreeTransform.compで実際にlayerTexArray/selectionMaskTexへ焼き込む。
// ---------------------------------------------------------------------------
class FreeTransformTool : public Tool, protected QOpenGLFunctions_4_3_Core
{
public:
    void initialize(QOpenGLContext *ctx);
    void setToolConfig(ToolConfig *cfg) { toolCfg_ = cfg; }

    // アクション開始(GLWidget::startFreeTransformAction)時に呼ばれ、
    // 選択範囲(あれば)/レイヤー全体(無ければ)から初期の4頂点を算出する。
    void activate(ToolContext &ctx);
    // アクションをキャンセルして終了する(GPU側は未変更なので状態を捨てるだけでよい)。
    void deactivate();
    // アクションを確定して終了する: freeTransform.compで実データへ焼き込んでから
    // engaged状態を解除する(恒等変形なら何も焼き込まずに終了する)。
    void confirm(ToolContext &ctx);

    void onMousePress(QMouseEvent *event, ToolContext &ctx)   override;
    void onMouseMove(QMouseEvent *event, ToolContext &ctx)    override;
    void onMouseRelease(QMouseEvent *event, ToolContext &ctx) override;
    bool isActive() const override { return dragMode_ != DragMode::None; }

    void paintOverlay(QPainter &painter, const ToolContext &ctx) const override;

    // キーボードショートカット(Shift+WASD)による平行移動。ドラッグ中のMoveと同じ
    // 4頂点一律の平行移動として扱う。
    void nudge(const QVector2D &delta) { if (engaged_) for (auto &c : corners_) c += delta; }

    // render.frag用のゲッター(GLWidget::paintGLから呼ぶ)
    bool      engaged()  const { return engaged_; }
    QVector2D corner(int i) const { return corners_[i]; } // 0=TL,1=TR,2=BR,3=BL
    QVector2D center0()  const { return c0_; }
    QVector2D halfSize() const { return halfSize_; }

private:
    ToolConfig *toolCfg_ = nullptr;
    bool engaged_ = false;

    // 現在の4頂点(キャンバスpx座標): 0=TL,1=TR,2=BR,3=BL
    std::array<QVector2D, 4> corners_;
    QVector2D c0_;       // 変形開始時の中心(固定)
    QVector2D halfSize_; // 変形開始時のbbox半径

    enum class DragMode { None, Move, Rotate, Corner };
    DragMode  dragMode_     = DragMode::None;
    int       activeCorner_ = -1;             // Cornerドラッグでどの頂点か(0-3)
    QVector2D lastMouseCanvas_;                // Move/Rotateドラッグの直前フレームのマウス位置
    QVector2D rotateCenter_;                   // Rotateドラッグ中の中心(4頂点の重心。ドラッグ開始時に固定)

    QVector2D centroid() const;

    struct ButtonRects { QRectF confirm, cancel; };
    ButtonRects buttonRects(const ToolContext &ctx) const;

    static bool pointInQuad(const QVector2D &p, const std::array<QVector2D, 4> &quad);

    void applyTransform(ToolContext &ctx); // 実際にfreeTransform.compで実データへ焼き込む
    void resetToIdentity();                // corners_をc0_/halfSize_基準の矩形に戻す
};
