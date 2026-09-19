#pragma once
#include "tools/core/ToolContext.h"
#include <QOpenGLFunctions_4_3_Core>

class QMouseEvent;
class QPainter;

// ---------------------------------------------------------------------------
// MotionBlurTool
// ---------------------------------------------------------------------------
// フィルターメニュー「移動ぼかし」アクション本体。GaussianBlurTool/MosaicTool/
// ChromaticAberrationTool と全く同じ構造(近傍サンプリングが必要で毎フレームの
// シェーダー内計算では重すぎるため、activate()時にアクティブレイヤーを
// ctx.fullLayerTex へ集約した上で、パラメータが変わるたびに motionBlurFilter.comp を
// 1回ディスパッチして ctx.transformSrcTex へプレビュー結果を焼く。render.frag側は
// それをそのままサンプルして表示するだけ。confirm()時はプレビュー結果をタイルへ
// 書き戻すだけで、キャンセルはレイヤー本体に一切触れないので常に安全)。
//
// 円形モードの中心は、ChromaticAberrationTool と同様にキャンバス上へ直接描画した
// ハンドルをドラッグして指定する(数値入力欄は持たない)。
// ---------------------------------------------------------------------------
class MotionBlurTool : protected QOpenGLFunctions_4_3_Core
{
public:
    enum class Mode { Parallel = 0, Radial = 1 };

    void initialize(QOpenGLContext *ctx);

    void activate(ToolContext &ctx);   // アクション開始: レイヤーを集約し、既定パラメータでプレビューを1回作る
    void deactivate();                 // キャンセル(GPU上のレイヤー本体には一切書き込まない)
    void confirm(ToolContext &ctx);    // プレビュー結果をタイルへ焼き込んで終了する

    bool engaged() const { return engaged_; }

    // パラメータ変更 → その場でプレビュー再計算
    void setMode(ToolContext &ctx, Mode mode);
    void setAngleDeg(ToolContext &ctx, float angleDeg);        // 平行モード: ぶれる向き
    void setDistancePx(ToolContext &ctx, float distancePx);    // 平行モード: ぶれる長さ
    void setCenterPx(ToolContext &ctx, QVector2D centerPx);    // 円形モード: 回転中心(レイヤーローカルpx)
    void setAngleSpanDeg(ToolContext &ctx, float spanDeg);     // 円形モード: 振れ角

    Mode      mode()         const { return mode_; }
    float     angleDeg()     const { return angleDeg_; }
    float     distancePx()   const { return distancePx_; }
    QVector2D centerPx()     const { return centerPx_; }
    float     angleSpanDeg() const { return angleSpanDeg_; }

    // render.frag用: プレビュー結果(ctx.transformSrcTex)がキャンバス座標系のどこに
    // 対応するか(レイヤー原点のピクセル座標とサイズ)。
    QVector2D previewOriginPx() const { return previewOriginPx_; }
    QVector2D previewSizePx()   const { return QVector2D((float)layerW_, (float)layerH_); }

    // 円形モードの中心ハンドルをキャンバス上でドラッグするためのマウス処理。
    // onMousePress()はハンドルにヒットして処理した場合のみtrueを返す。
    bool onMousePress(QMouseEvent *event, ToolContext &ctx);
    void onMouseMove(QMouseEvent *event, ToolContext &ctx);
    void onMouseRelease(QMouseEvent *event, ToolContext &ctx);
    bool isDraggingCenter() const { return draggingCenter_; }
    void paintOverlay(QPainter &painter, const ToolContext &ctx) const;

private:
    bool  engaged_ = false;
    Mode  mode_ = Mode::Parallel;
    float angleDeg_ = 0.0f;       // 平行モード: 0=水平右向き
    float distancePx_ = 24.0f;    // 平行モード: ぶれる長さ
    QVector2D centerPx_;          // 円形モード: 回転中心(レイヤーローカルpx)。既定はレイヤー中心。
    float angleSpanDeg_ = 10.0f;  // 円形モード: 振れ角
    int  layerW_ = 0, layerH_ = 0;
    int  layerIndex_ = -1;
    QVector2D previewOriginPx_;

    bool draggingCenter_ = false;

    void updatePreview(ToolContext &ctx);
    // 経路長に見合ったサンプル数(飛び飛びの多重像にならない下限を確保しつつ上限で頭打ち)
    int  sampleCount() const;
    // 中心ハンドルの「キャンバスピクセル座標」(previewOriginPx_ + centerPx_)
    QVector2D centerCanvasPx() const;
};
