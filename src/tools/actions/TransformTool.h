#pragma once

#include "tools/core/Tool.h"
#include "tools/core/ToolConfig.h"

#include <QOpenGLFunctions_4_3_Core>
#include <QVector2D>
#include <QRectF>

// ---------------------------------------------------------------------------
// TransformTool
// ---------------------------------------------------------------------------
// 「拡大・縮小・回転」アクション(編集メニュー、Ctrl+Tで開始する一回限りの操作。
// ツールバーの常設ツールではない)。選択範囲があればその範囲を、無ければ
// レイヤー全体を対象に、ハンドル付きのbbox型の枠を表示して移動/拡大縮小/回転を行う。
//
// 操作中はGPU上の実データ(layerTexArray/selectionMaskTex)には一切触れず、
// pivot_/scale_/rotation_という現在の変換パラメータだけをCPU側で保持する。
// render.fragが毎フレームこれを使ってプレビューを合成し、paintOverlay()が
// 枠線・ハンドル・確定/キャンセルボタンを描く。確定(confirm)して初めて
// transform.compで実際にlayerTexArray/selectionMaskTexへ焼き込み、アクションを終える。
//
// マウスイベントの委譲を再利用するためTool派生のままにしてあるが、
// GLWidgetはToolType経由ではなく、変形アクション実行中かどうかのフラグで
// 直接このインスタンスへイベントを回す(ToolRegistry/ToolConfigには登録しない)。
// ---------------------------------------------------------------------------
class TransformTool : public Tool, protected QOpenGLFunctions_4_3_Core
{
public:
    void initialize(QOpenGLContext *ctx);
    void setToolConfig(ToolConfig *cfg) { toolCfg_ = cfg; }

    // アクション開始(GLWidget::startTransformAction)時に呼ばれ、
    // 選択範囲(あれば)/レイヤー全体(無ければ)からbboxを算出する。
    void activate(ToolContext &ctx);
    // アクションをキャンセルして終了する(GPU側は未変更なので状態を捨てるだけでよい)。
    void deactivate();
    // アクションを確定して終了する: 変形をtransform.compで実データへ焼き込んでから
    // engaged状態を解除する(恒等変形なら何も焼き込まずに終了する)。
    void confirm(ToolContext &ctx);

    void onMousePress(QMouseEvent *event, ToolContext &ctx)   override;
    void onMouseMove(QMouseEvent *event, ToolContext &ctx)    override;
    void onMouseRelease(QMouseEvent *event, ToolContext &ctx) override;
    bool isActive() const override { return dragMode_ != DragMode::None; }

    void paintOverlay(QPainter &painter, const ToolContext &ctx) const override;

    // キーボードショートカット(Shift+WASD)による平行移動。ドラッグ中のMoveと同じ
    // pivot_の平行移動として扱う。
    void nudge(const QVector2D &delta) { if (engaged_) pivot_ += delta; }

    // render.frag用に現在の変換パラメータを渡すためのゲッター(GLWidget::paintGLから呼ぶ)
    bool      engaged()  const { return engaged_; }
    QVector2D pivot()    const { return pivot_; }
    QVector2D scale()    const { return scale_; }
    float     rotation() const { return rotation_; }
    QVector2D center0()  const { return c0_; }
    QVector2D halfSize() const { return halfSize_; }

private:
    ToolConfig *toolCfg_ = nullptr;

    bool engaged_ = false; // このツールがアクティブでbboxが表示されている間true

    // 変換パラメータ(いずれもキャンバスpx座標系)
    QVector2D pivot_;                        // 現在の中心位置
    QVector2D scale_    = QVector2D(1.0f, 1.0f);
    float     rotation_ = 0.0f;              // ラジアン
    QVector2D c0_;                           // 変形開始時の中心(固定、ローカル座標の原点)
    QVector2D halfSize_;                     // 変形開始時のbbox半径(ローカル座標系。回転/拡縮の影響を受けない定数)

    enum class DragMode { None, Move, Rotate, Scale };
    DragMode  dragMode_     = DragMode::None;
    int       activeHandle_ = -1;            // Scaleドラッグでどのハンドルか(0-7)
    QVector2D dragAnchorCanvas_;              // Scaleドラッグ時、画面上で固定される反対側ハンドルの位置
    QVector2D dragAnchorLocal_;
    QVector2D dragHandleLocal_;
    QVector2D dragStartScale_;
    float     dragRotationFixed_ = 0.0f;      // Scaleドラッグ中は回転を固定して使う
    QVector2D lastMouseCanvas_;               // Move/Rotateドラッグの直前フレームのマウス位置(キャンバスpx)

    // ローカル座標(±halfSize_内) -> 現在のキャンバスpx座標
    QVector2D localToCanvas(const QVector2D &local) const;
    // キャンバスpx座標 -> ローカル座標(回転・拡縮を除去したもの)
    QVector2D canvasToLocal(const QVector2D &canvasPos) const;

    // 8ハンドルのローカル座標(定数テーブル)。7-indexが常に対角の反対側ハンドルになる。
    static QVector2D handleLocal(int index, const QVector2D &halfSize);

    // 確定/キャンセルボタンの矩形(ウィジェット座標)。paintOverlay()とonMousePress()の
    // 両方から同じ計算を使うことで、見た目と当たり判定を一致させる。
    struct ButtonRects { QRectF confirm, cancel; };
    ButtonRects buttonRects(const ToolContext &ctx) const;

    void applyTransform(ToolContext &ctx); // 実際にtransform.compで実データへ焼き込む(恒等変形なら何もしない)
    void resetToIdentity();                // c0_/halfSize_はそのままに scale=1,rotation=0,pivot=c0_ に戻す
};
