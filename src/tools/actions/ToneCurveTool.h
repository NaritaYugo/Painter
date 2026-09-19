#pragma once
#include "tools/core/ToolContext.h"
#include <QOpenGLFunctions_4_3_Core>
#include <QVector>
#include <QPointF>

// ---------------------------------------------------------------------------
// ToneCurveTool
// ---------------------------------------------------------------------------
// 処理>色調補正の「トーンカーブ」アクション本体。ColorBalanceTool/HueSatLightTool
// と同様、キャンバス上でのドラッグ操作は無く、値はToneCurvePanel(ToneCurveEditor
// が持つ制御点列)からsetControlPoints()経由で渡される。
//
// 他の色調整系ツールと違い、任意形状の曲線を毎フレームGLSLで計算し直すのは
// 非効率かつ煩雑なため、CPU側で256エントリのLUT(0-255入力 -> 0-255出力)を
// 制御点からスプライン補間で構築し、小さな256x1テクスチャ(ctx.toneCurveLUTTex)
// へアップロードする。プレビュー(render.frag)・確定時の焼き込み(toneCurve.comp)
// ともにこのLUTテクスチャをサンプルするだけなので、どちらも軽量。
// ---------------------------------------------------------------------------
class ToneCurveTool : protected QOpenGLFunctions_4_3_Core
{
public:
    void initialize(QOpenGLContext *ctx);

    void activate(ToolContext &ctx);   // アクション開始(恒等カーブにリセット)
    void deactivate();                 // キャンセル(GPUには一切書き込まない)
    void confirm(ToolContext &ctx);    // 焼き込んで終了する

    bool engaged() const { return engaged_; }

    // 制御点(x,yともに0..255)。呼び出し側(ToneCurveEditor)がx昇順・重複無しで
    // 渡す前提(先頭がx=0、末尾がx=255である必要は無い。範囲外は端の値でクランプする)。
    void setControlPoints(ToolContext &ctx, const QVector<QPointF> &points);

private:
    bool engaged_ = false;
    QVector<QPointF> points_;
    quint8 lut_[256] = {};

    void rebuildLut();
    void uploadLut(ToolContext &ctx);
    bool isIdentity() const;
    void applyAdjustment(ToolContext &ctx);
};
