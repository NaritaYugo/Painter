#pragma once

#include "tools/core/ToolConfig.h"
#include "tools/core/Tool.h"

// ---------------------------------------------------------------------------
// AirbrushTool
// ---------------------------------------------------------------------------
// ペン(PenEraserTool)は1ストローク全体分のスタンプ形状を「マスク」へ最大値で
// 積み上げ、マウスを離した瞬間に1回だけ焼き込む(このためストローク中に同じ場所を
// 何度往復しても、最終的な濃さは設定した不透明度を超えない)。
//
// エアブラシはこれと異なり、スタンプ1つを打つたびに即座にレイヤーへ焼き込む
// (通常のアルファ合成を繰り返す)。そのため同じ場所へスタンプが重なるほど、
// 1ストローク中であっても際限なく濃くなっていく(実物のエアブラシ/スプレーの
// 挙動に相当する)。
//
// 先端画像は常に手続き的な円(硬さによるフォールオフのみ)固定で、Penのような
// 先端画像テクスチャの差し替えには対応しない。
// ---------------------------------------------------------------------------
class AirbrushTool : public Tool
{
public:
    // toolCfg: MainWindowが所有する唯一のToolConfigへの非所有ポインタ(CanvasWidget経由で渡される)
    explicit AirbrushTool(ToolConfig *toolCfg) : toolCfg_(toolCfg) {}

    void onMousePress(QMouseEvent *event, ToolContext &ctx)   override;
    void onMouseMove(QMouseEvent *event, ToolContext &ctx)    override;
    void onMouseRelease(QMouseEvent *event, ToolContext &ctx) override;
    bool isActive() const override { return isDrawing_; }
    std::optional<QCursor> cursor(const ToolContext &ctx) const override;
    // フレームレート律速バッチ(Tool.h参照)。onMousePress/onMouseMoveが貯めた
    // pendingStamps_を、ここでまとめて(1つずつstampAndBakeへ)流す。
    void flushPendingInput(ToolContext &ctx) override;
    bool hasPendingInput() const override { return !pendingStamps_.isEmpty(); }
    int  flushIntervalMs() const override;

private:
    ToolConfig *toolCfg_ = nullptr;

    bool      isDrawing_ = false;
    QVector2D lastMousePos_;
    QVector2D drawHead_;

    // PenEraserTool::strokeDistCarry_と同じ、距離ベースのスタンプ方式のための繰越距離。
    float strokeDistCarry_ = 0.0f;

    // フレームレート律速バッチ用に貯めておく未処理スタンプ(位置+その時点の半径)。
    // onMousePress/onMouseMoveはここへ積むだけで、即座にはstampAndBake()を呼ばない
    // (実際の処理はflushPendingInput()、CanvasWidgetの定期タイマーが呼ぶ)。
    // 1スタンプごとに即座に焼き込むという挙動自体(同じ場所に止めると際限なく
    // 濃くなる)は変えず、呼ぶタイミングだけをペンタブの生サンプル頻度から
    // 一定間隔(60Hz目安)へ間引く。
    // alphaは筆圧→不透明度が有効なときの、そのスタンプの濃度(0..1)。無効なら常に1.0。
    struct PendingStamp { QVector2D pos; float radius; float alpha; };
    QVector<PendingStamp> pendingStamps_;

    // 1スタンプぶんの形状をマスクへ描き、直後にそのスタンプの影響範囲だけを
    // 即座にレイヤーへ焼き込む(ディスパッチ・タイル読み書きともスタンプの
    // バウンディングボックスに限定し、キャンバス全体を毎回処理しない)。
    // opacityScale: このスタンプの濃度(筆圧→不透明度)。焼き込む色のアルファへ掛ける。
    // ペン(PenEraserTool)と違いスタンプ1つずつ即座に焼き込む方式なので、
    // シェーダーへスタンプごとの濃度を渡す必要はなく、色を作る時点で掛ければよい。
    void stampAndBake(ToolContext &ctx, const QVector2D &pos, float radius, float hardness,
                      float opacityScale);
    // 現在の筆圧から、このツールの設定に沿った半径とスタンプ濃度を求める。
    float pressureRadius(int size) const;
    float pressureStampAlpha() const;
};
