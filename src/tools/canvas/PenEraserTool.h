#pragma once

#include "tools/core/ToolConfig.h"
#include "tools/core/Tool.h"

// PenEraserTool
class PenEraserTool : public Tool
{
public:
    // settings: このツールが使うブラシ設定への参照(CanvasWidgetが所有する penSettings[i] or eraserSettings)。
    PenEraserTool(bool isEraser, float *smoothingStrength, ToolConfig *toolCfg)
        : isEraser_(isEraser), smoothingStrength_(smoothingStrength), toolCfg_(toolCfg) {}

    void onMousePress(QMouseEvent *event, ToolContext &ctx)   override;
    void onMouseMove(QMouseEvent *event, ToolContext &ctx)    override;
    void onMouseRelease(QMouseEvent *event, ToolContext &ctx) override;
    bool isActive() const override { return isDrawing_; }
    std::optional<QCursor> cursor(const ToolContext &ctx) const override;
    // フレームレート律速バッチ(Tool.h参照)。
    void flushPendingInput(ToolContext &ctx) override;
    bool hasPendingInput() const override { return !pendingStamps_.isEmpty(); }
    int  flushIntervalMs() const override;

private:
    ToolConfig *toolCfg_ = nullptr;
    bool           isEraser_;
    float         *smoothingStrength_;

    bool      isDrawing_ = false;
    QVector2D lastMousePos_;
    QVector2D drawHead_;
    QVector2D penTip_;

    // 距離ベースのスタンプ方式: 直前にスタンプを置いた位置からまだ「間隔」に満たない移動量(次のスタンプまでの繰越距離)。
    float distToNextStamp_ = 0.0f;
    // 直前のイベント時点での半径。
    float lastRadius_ = 0.0f;
    // 同じ理由で、筆圧→不透明度が有効なときのスタンプ濃度も区間内で補間する。
    float lastStampAlpha_ = 1.0f;

    // 今回のストロークが実際に触れたキャンバスタイル範囲(両端含む)の累積。
    bool strokeTileBoundsValid_ = false;
    int  strokeTxMin_ = 0, strokeTxMax_ = -1;
    int  strokeTyMin_ = 0, strokeTyMax_ = -1;
    // ラップ(周回)により反対側の端タイルにも書き込みが発生した場合、その分はstrokeTxMin_等の範囲に含まれないため、
    // onMouseRelease側の部分bake最適化を諦めて従来通りキャンバス全域を処理する(ラップは比較的まれな機能なので簡略化する)。
    bool strokeUsedWrap_ = false;

    // フレームレート律速バッチ用に貯めておく未処理スタンプ(位置+その時点の半径。筆圧でスタンプごとに半径が変わりうるため、半径もスタンプ単位で持つ)。
    struct PendingStamp {
        QVector2D pos; float radius; float alpha; float angle; float dist; float roundness;
        int pathIndex = -1;
        float r = 0.0f, g = 0.0f, b = 0.0f;
    };
    QVector<PendingStamp> pendingStamps_;

    // 入り抜き・後補正用のストローク再描画
    float strokeLen_ = 0.0f;              // ここまでの経路長[px]
    QVector<PendingStamp> strokeStamps_;  // 引き直しが要るときのみ記録
    // スタンプを乗せた軌道上の点(散布のずれを含まない、手振れ補正後のペン先の通り道)。
    QVector<QVector2D> strokePath_;
    bool  strokeRecording_ = false;
    // 記録するスタンプ数の上限。
    static constexpr int kMaxRecordedStamps = 200000;

    // 入りだけの係数(描画中に使う)。
    float taperInFactor(float dist) const;
    // 入り+抜き両方の係数(全長が確定してから使う)。
    float taperFactor(float dist, float totalLen) const;
    // 抜き・後補正を適用するため、マスクを消してからストローク全体を引き直す。
    void rebuildStroke(ToolContext &ctx);
    // strokePath_をなめらかにしたものを返す(後補正が無効なら空を返す)。
    QVector<QVector2D> smoothStrokePath() const;
    // マスクを、今回のストロークが触れたタイル範囲だけクリアする。
    void clearMaskOverStrokeTiles(ToolContext &ctx);
    // スタンプごとの濃度(vec4のw)を使う必要があるか(筆圧/ランダム/入り抜きのいずれか)。
    bool usesStampAlpha() const;
    // スタンプごとの色(ストローク色バッファ)を使う必要があるか。
    bool usesStrokeColor(ToolContext &ctx) const;
    // 下地混色が有効か(=brushState.compを走らせるか)。
    bool usesColorMixing(ToolContext &ctx) const;

    // アクティブレイヤーのタイルを指定範囲だけ fullLayerTex へ展開する。
    void expandLayerToFullTex(ToolContext &ctx, int txMin, int txMax,
                              int tyMin, int tyMax, bool maskMode);
    // 筆に乗っている絵の具を基準色へ戻す(ストローク開始時と引き直しの前)。
    void resetBrushPaint(ToolContext &ctx);
    // このバッチのスタンプ色を、下地を拾いながら更新する(brushState.comp)。
    void dispatchBrushState(ToolContext &ctx, int stampCount);
    // スタンプ列をディスパッチする(下地混色のときだけ小分けにする)。
    void dispatchStampsChunked(ToolContext &ctx, const QVector<PendingStamp> &stamps);

    // 散布・各種ランダム用の擬似乱数(xorshift32)。
    quint32 rngState_ = 1u;
    float rnd01();     // 0.0〜1.0
    float rndSigned(); // -1.0〜1.0

    // 経路上の1点ぶんのスタンプをpendingStamps_へ積む。
    void appendStamps(const QVector2D &pathPos, float radius, float alpha, int size, float dist);
    // 「進行方向に追従」用に、直前に進んだ向き[rad]を覚えておく。
    float lastDirAngle_ = 0.0f;
    // 進行方向に追従するとき、押した瞬間はまだ方向が分からない。
    bool      pendingFirstStamp_ = false;
    QVector2D firstStampPos_;
    float     firstStampRadius_ = 0.0f;
    float     firstStampAlpha_  = 1.0f;
    // 保留していた最初の1粒を、いまの進行方向で打つ(保留が無ければ何もしない)。
    void flushPendingFirstStamp(int size);
    // 次のスタンプまでの距離(スタンプ間隔にランダムを適用したもの)。
    float nextSpacingPx(int size);

    static QColor toPreMulColor(const QColor &rawColor, float opacity);
    // このストロークが「消す」動作か(消しゴムツール、または透明色)と、その強さ。
    EraseBrush eraseBrush() const;
    // bake.compへ渡す色。
    QColor bakeColor(const ToolContext &ctx) const;
    // 現在の筆圧から、このツールの設定に沿った半径とスタンプ濃度を求める。
    float pressureRadius(int size) const;
    float pressureStampAlpha() const;
    // pendingStamps_に積んだ全スタンプを1回のdispatchでまとめて描く。
    void dispatchStampBatch(ToolContext &ctx, const QVector<PendingStamp> &stamps);
};
