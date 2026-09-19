#pragma once
#include "tools/core/ToolContext.h"
#include "dialogs/GradientStripEditor.h" // Stop の定義
#include <QOpenGLFunctions_4_3_Core>
#include <QVector>

// ---------------------------------------------------------------------------
// GradientMapTool (Pro限定)
// ---------------------------------------------------------------------------
// 処理>色調補正の「グラデーションマップ」アクション本体。ToneCurveTool と全く同じ構造
// (キャンバス上でのドラッグ操作は無く、値は GradientMapPanel が持つ GradientStripEditor
// のストップ列から setStops() 経由で渡される)。
//
// 任意個のストップからなるグラデーションを毎フレームGLSLで組み立てるのは非効率なので、
// CPU側で256エントリのLUT(輝度0-255 -> RGB)を線形補間で構築し、256x1のRGBA8テクスチャ
// (ctx.gradientMapLUTTex)へアップロードする。プレビュー(render.frag)・確定時の
// 焼き込み(gradientMap.comp)ともにこのLUTをサンプルするだけなので、どちらも軽量。
// ---------------------------------------------------------------------------
class GradientMapTool : protected QOpenGLFunctions_4_3_Core
{
public:
    using Stop = GradientStripEditor::Stop;

    void initialize(QOpenGLContext *ctx);

    void activate(ToolContext &ctx);   // アクション開始(黒→白にリセット)
    void deactivate();                 // キャンセル(GPU上のレイヤー本体には一切書き込まない)
    void confirm(ToolContext &ctx);    // 焼き込んで終了する

    bool engaged() const { return engaged_; }

    // ストップ列(pos昇順、2個以上。GradientStripEditor側で保証している前提)
    void setStops(ToolContext &ctx, const QVector<Stop> &stops);

private:
    bool engaged_ = false;
    QVector<Stop> stops_;
    quint8 lut_[256 * 4] = {};

    void rebuildLut();
    void uploadLut(ToolContext &ctx);
    void applyAdjustment(ToolContext &ctx);
};
