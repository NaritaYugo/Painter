#pragma once
#include "tools/core/ToolContext.h"
#include <QOpenGLFunctions_4_3_Core>

class QMouseEvent;
class QPainter;

// ---------------------------------------------------------------------------
// ChromaticAberrationTool (Pro限定)
// ---------------------------------------------------------------------------
// フィルターメニュー「色収差」アクション本体。MosaicTool/GaussianBlurToolと全く
// 同じ構造(近傍サンプリングが必要で毎フレームのシェーダー内計算では重すぎるため、
// activate()時にアクティブレイヤーを ctx.fullLayerTex へ集約した上で、パラメータが
// 変わるたびに chromaticAberrationFilter.comp を1回ディスパッチして
// ctx.transformSrcTex へプレビュー結果を焼く。render.frag側はそれをそのまま
// サンプルして表示するだけ。confirm()時はプレビュー結果をタイルへ書き戻すだけで、
// キャンセルはレイヤー本体に一切触れないので常に安全)。
//
// 円形モードの中心は、CanvasSizeToolの矩形ハンドルと同様にキャンバス上へ直接
// 描画したハンドルをドラッグして指定する(数値入力欄は持たない)。CanvasWidget側は
// canvasSizeActionActive_と同じ扱いで、chromaticAberrationActionActive_中は
// onMousePress()がハンドルにヒットした場合だけ処理を横取りする(ヒットしなければ
// false を返し、CanvasWidget側は通常のツール排他ガード(Move/Rotateのみ通す)に委ねる)。
// ---------------------------------------------------------------------------
class ChromaticAberrationTool : protected QOpenGLFunctions_4_3_Core
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
    void setAngleDeg(ToolContext &ctx, float angleDeg);      // 平行モード時のずらす向き
    void setCenterPx(ToolContext &ctx, QVector2D centerPx); // レイヤーローカルpx座標
    void setDistancePx(ToolContext &ctx, float distancePx);

    Mode      mode()       const { return mode_; }
    float     angleDeg()   const { return angleDeg_; }
    QVector2D centerPx()   const { return centerPx_; }
    float     distancePx() const { return distancePx_; }

    // render.frag用: プレビュー結果(ctx.transformSrcTex)がキャンバス座標系のどこに
    // 対応するか(レイヤー原点のピクセル座標とサイズ)。
    QVector2D previewOriginPx() const { return previewOriginPx_; }
    QVector2D previewSizePx()   const { return QVector2D((float)layerW_, (float)layerH_); }

    // 円形モードの中心ハンドルをキャンバス上でドラッグするためのマウス処理。
    // onMousePress()はハンドルにヒットして処理した場合のみtrueを返す(呼び出し側は
    // trueが返った時だけこの後のonMouseMove/onMouseReleaseも横流しし続ければよい)。
    bool onMousePress(QMouseEvent *event, ToolContext &ctx);
    void onMouseMove(QMouseEvent *event, ToolContext &ctx);
    void onMouseRelease(QMouseEvent *event, ToolContext &ctx);
    bool isDraggingCenter() const { return draggingCenter_; }
    void paintOverlay(QPainter &painter, const ToolContext &ctx) const;

private:
    bool engaged_ = false;
    Mode  mode_ = Mode::Parallel;
    float angleDeg_ = 0.0f;      // 平行モード時のずらす向き(度、0=水平右向き)
    QVector2D centerPx_;         // 円形モード時の中心(レイヤーローカルpx座標)。既定はレイヤー中心。
    float distancePx_ = 8.0f;
    int  layerW_ = 0, layerH_ = 0; // activate()時に集約したレイヤーのピクセルサイズ
    int  layerIndex_ = -1;
    QVector2D previewOriginPx_;

    bool draggingCenter_ = false;

    void updatePreview(ToolContext &ctx);
    // 中心ハンドルの「キャンバスピクセル座標」(previewOriginPx_ + centerPx_)
    QVector2D centerCanvasPx() const;
};
