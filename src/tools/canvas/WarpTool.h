#pragma once

#include "tools/core/Tool.h"
#include "tools/core/ToolConfig.h"
#include "tools/core/CursorUtils.h"

#include <QOpenGLFunctions_4_3_Core>
#include <QOpenGLShaderProgram>
#include <QVector2D>

// ---------------------------------------------------------------------------
// WarpTool
// ---------------------------------------------------------------------------
// ドラッグした方向にブラシ範囲内の色を引きずる、ゆがみ(リキファイ)ツール。
//
// 「累積変位ベクトル場」方式で実装している。ストローク開始時にレイヤーの内容を
// srcTex_ へ丸ごと退避し、ドラッグ中は変位ベクトル場 dispTex_ だけを更新して、
// 表示のたびに srcTex_ を1回引き直す。
//
// 素朴な実装(毎ステップ、前回の結果をバイリニアで引き直して上書きする)にすると、
// 1ストロークで100回以上補間が重なり、そのたびに掛かるローパスが積算されて線が
// どんどんボケる。何回ドラッグしても元画像からの補間を1回に保つのがこの方式の狙い
// (商用ペイントソフトのリキファイが線をぼかさずに引きずれるのも同じ理屈)。
// 詳細は resources/shaders/paint/warp.comp のコメントを参照。
//
// メモリ: ストローク作業用にキャンバスサイズのテクスチャを4枚
// (RGBA8×2 + RG32F×2 = 24バイト/px)持つ。ゆがみツールを一度でも使うまでは
// 確保しない。変位場をRG32Fにしているのは、half floatだと変位が数百pxまで育った
// ときの精度(~0.25px)が足りず、ゆがみに段差が出るため。
// ---------------------------------------------------------------------------
class WarpTool : public Tool, protected QOpenGLFunctions_4_3_Core
{
public:
    void initialize(QOpenGLContext *ctx);

    // MainWindowが所有する唯一のToolConfigへの非所有ポインタ(CanvasWidget経由で渡される)
    void setToolConfig(ToolConfig *cfg) { toolCfg_ = cfg; }
    // CanvasWidget が initializeGL でコンパイルした warp.comp を注入する
    void setProgram(QOpenGLShaderProgram *p) { prog_ = p; }

    void onMousePress(QMouseEvent *event, ToolContext &ctx)   override;
    void onMouseMove(QMouseEvent *event, ToolContext &ctx)    override;
    void onMouseRelease(QMouseEvent *event, ToolContext &ctx) override;
    bool isActive() const override { return dragging_; }

    std::optional<QCursor> cursor(const ToolContext &ctx) const override;

private:
    ToolConfig           *toolCfg_ = nullptr;
    QOpenGLShaderProgram *prog_    = nullptr;

    bool      dragging_ = false;
    bool      ready_    = false; // srcTex_へのスナップショットが成功しているか
    QVector2D lastPos_;

    // 作業テクスチャ(すべてキャンバスサイズ)。サイズが変わったら作り直す。
    GLuint srcTex_      = 0; // RGBA8 ストローク開始時のレイヤー内容(ストローク中は不変)
    GLuint dispTex_     = 0; // RG32F 累積変位ベクトル場
    GLuint dispPrevTex_ = 0; // RG32F 変位合成の読み取り元(自分以外の画素の変位も読むため別枚数が要る)
    GLuint warpTex_     = 0; // RGBA8 ワープ結果(タイルへ書き戻す前段)
    int    texW_ = 0;
    int    texH_ = 0;
    void   ensureTextures(int w, int h);

    // ストローク開始時のスナップショット取得 + 変位場のゼロクリア
    bool beginStroke(ToolContext &ctx);

    // from→to の線分に沿ってブラシ範囲をゆがませる(press/move から呼ぶ)
    void applyWarp(ToolContext &ctx, const QVector2D &from, const QVector2D &to);

    // 変位場を1ステップ分だけ進める(applyWarpが線分を分割して複数回呼ぶ)。
    // タイルへの書き戻しは行わない(applyWarpが最後にまとめて行う)。
    void dispatchWarpStep(ToolContext &ctx, const QVector2D &from, const QVector2D &to,
                          float radius, float hardness, float strength);
};
