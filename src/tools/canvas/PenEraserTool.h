#pragma once

#include "tools/core/ToolConfig.h"
#include "tools/core/Tool.h"

// ---------------------------------------------------------------------------
// PenEraserTool
// ---------------------------------------------------------------------------
// 旧 GLWidget::mousePressEvent/mouseMoveEvent/mouseReleaseEvent のうち、
// 「ペンで描く/消しゴムで消す」ロジックだけを取り出したもの。
// Pen0/Pen1/Pen2/Eraser はすべてこのクラスのインスタンスで、
// コンストラクタに渡す BrushSettings* と isEraser フラグだけが異なる。
//
// GLWidget 側の責務は「どのインスタンスが activeTool か」を切り替えることだけになる。
// ---------------------------------------------------------------------------
class PenEraserTool : public Tool
{
public:
    // settings: このツールが使うブラシ設定への参照(GLWidgetが所有する penSettings[i] or eraserSettings)
    // isEraser: 消しゴムとして振る舞うか(baker への色渡しが透明になる)
    // toolCfg: MainWindowが所有する唯一のToolConfigへの非所有ポインタ(GLWidget経由で渡される)
    PenEraserTool(bool isEraser, float *smoothingStrength, ToolConfig *toolCfg)
        : isEraser_(isEraser), smoothingStrength_(smoothingStrength), toolCfg_(toolCfg) {}

    void onMousePress(QMouseEvent *event, ToolContext &ctx)   override;
    void onMouseMove(QMouseEvent *event, ToolContext &ctx)    override;
    void onMouseRelease(QMouseEvent *event, ToolContext &ctx) override;
    bool isActive() const override { return isDrawing_; }
    std::optional<QCursor> cursor(const ToolContext &ctx) const override;
    // フレームレート律速バッチ(Tool.h参照)。onMousePress/onMouseMoveが貯めた
    // pendingStamps_をここでまとめて1回のdispatchにする。GLWidget::paintGL()が
    // 毎フレーム呼ぶ。
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

    // 距離ベースのスタンプ方式: 直前にスタンプを置いた位置からまだ「間隔」に満たない
    // 移動量(次のスタンプまでの繰越距離)。マウスイベントの間隔に関係なく、実際に
    // ペン先が移動した経路上に一定間隔でスタンプが並ぶようにするための状態。
    // 次のスタンプを打つまでに、経路上をあと何px進む必要があるか。
    // 間隔をランダムに揺らせるようにするため、「経過距離の剰余」ではなく
    // 「残り距離」で持つ(揺らぎ0なら従来の等間隔と完全に同じ並びになる)。
    float distToNextStamp_ = 0.0f;
    // 直前のイベント時点での半径。1回のマウス/ペンイベントで複数スタンプを打つとき、
    // 全部に今回の半径を使うと筆圧の変化がイベント単位の段差になって現れるため、
    // 区間内で前回値から補間する(PenEraserTool.cppのonMouseMove参照)。
    float lastRadius_ = 0.0f;
    // 同じ理由で、筆圧→不透明度が有効なときのスタンプ濃度も区間内で補間する。
    float lastStampAlpha_ = 1.0f;

    // 今回のストロークが実際に触れたキャンバスタイル範囲(両端含む)の累積。
    // onMouseRelease時のbake/書き戻し/マスククリアを、キャンバス全域ではなく
    // この範囲だけに限定するために使う(dispatchStrokeSegmentのたびに和集合をとる)。
    bool strokeTileBoundsValid_ = false;
    int  strokeTxMin_ = 0, strokeTxMax_ = -1;
    int  strokeTyMin_ = 0, strokeTyMax_ = -1;
    // ラップ(周回)により反対側の端タイルにも書き込みが発生した場合、その分は
    // strokeTxMin_等の範囲に含まれないため、onMouseRelease側の部分bake最適化を諦めて
    // 従来通りキャンバス全域を処理する(ラップは比較的まれな機能なので簡略化する)。
    bool strokeUsedWrap_ = false;

    // フレームレート律速バッチ用に貯めておく未処理スタンプ(位置+その時点の半径。
    // 筆圧でスタンプごとに半径が変わりうるため、半径もスタンプ単位で持つ)。
    // onMousePress/onMouseMoveはここへ積むだけで即座にはGPUディスパッチしない。
    // 実際のディスパッチはflushPendingInput()(GLWidget::paintGL()が毎フレーム
    // 呼ぶ)で1回にまとめて行う。ペンタブの高頻度サンプルでもGPU処理回数を
    // 表示フレーム数まで抑えられる(位置・筆圧そのものはonMouseMove()側で
    // 実イベントごとに正しい順序で処理されるため、精度は落ちない)。
    // alphaは筆圧→不透明度が有効なときの、そのスタンプの濃度(0..1)。無効なら常に1.0。
    // angleは先端画像を回す角度[rad](角度のランダムが0なら常に0)。
    // distはストローク開始からこのスタンプまでの経路上の距離[px](入り抜き用)。
    // roundnessは先端の真円率(1.0=真円)。傾きで潰す設定があるためスタンプ単位で持つ。
    // pathIndexは、このスタンプが乗っている軌道上の点(strokePath_の添字)。
    // 後補正で軌道を引き直したとき、散布のずれを保ったままスタンプを動かすのに使う
    // (posは軌道点+散布のずれなので、軌道点の移動量をそのまま足せばよい)。
    // colorはこのスタンプの色(0〜1、不透明度を含まない素の色)。色のランダム等で
    // スタンプごとに色が変わるとき(usesStrokeColor)だけ意味を持つ。
    struct PendingStamp {
        QVector2D pos; float radius; float alpha; float angle; float dist; float roundness;
        int pathIndex = -1;
        float r = 0.0f, g = 0.0f, b = 0.0f;
    };
    QVector<PendingStamp> pendingStamps_;

    // ---- ストローク全体の引き直し(入り抜き / 後補正) ------------------------
    // 「入り」はストローク開始からの距離だけで決まるので、描きながらそのまま適用できる。
    // 一方
    //   ・「抜き」は終点が分かって初めて決まる
    //   ・「後補正」は軌道全体が分かって初めて決まる
    // ため、どちらもペンを離した時点でストローク全体を引き直して適用する
    // (rebuildStroke)。そのために、どちらかが有効なときだけ全スタンプを
    // (入り抜きを掛ける前の素の値で)控えておく。
    float strokeLen_ = 0.0f;              // ここまでの経路長[px]
    QVector<PendingStamp> strokeStamps_;  // 引き直しが要るときのみ記録
    // スタンプを乗せた軌道上の点(散布のずれを含まない、手振れ補正後のペン先の通り道)。
    // 後補正はこの配列をなめらかにしてから、その移動量を各スタンプへ反映する。
    QVector<QVector2D> strokePath_;
    bool  strokeRecording_ = false;
    // 記録するスタンプ数の上限。これを超えたら記録をやめ、引き直しも諦める
    // (極端に長いストロークで引き直しのコストとメモリが読めなくなるのを避ける保険)。
    static constexpr int kMaxRecordedStamps = 200000;

    // 入りだけの係数(描画中に使う)。0..1。
    float taperInFactor(float dist) const;
    // 入り+抜き両方の係数(全長が確定してから使う)。0..1。
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
    // 使うときだけバッファを確保するので、確保に失敗する環境ではfalseになる。
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

    // 散布・各種ランダム用の擬似乱数(xorshift32)。ストローク開始のたびに種を変える。
    // 描くたびに違う散らばりになればよいだけなので品質は問わないが、
    // 1スタンプあたり数回引くため軽さは要る(QRandomGeneratorのグローバル
    // インスタンスを毎回叩くより素直)。
    quint32 rngState_ = 1u;
    float rnd01();     // 0.0〜1.0
    float rndSigned(); // -1.0〜1.0

    // 経路上の1点ぶんのスタンプをpendingStamps_へ積む。散布・粒子数・
    // サイズ/不透明度/角度のランダムはここで一括して適用する。
    void appendStamps(const QVector2D &pathPos, float radius, float alpha, int size, float dist);
    // 「進行方向に追従」用に、直前に進んだ向き[rad]を覚えておく。
    float lastDirAngle_ = 0.0f;
    // 進行方向に追従するとき、押した瞬間はまだ方向が分からない。そのまま基準角で
    // 1粒打つと、平筆では線の始点に進行方向と無関係な向きの粒が残ってしまう
    // (縦線の先端に横棒が付いて十字に見える)。最初の1粒だけは方向が判明する
    // (=最初に動いた)まで保留し、そこで正しい向きにして打つ。
    // 動かずに離された場合(点を打っただけ)は、離すときに基準角のまま打つ。
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
    // bake.compへ渡す色。「消す」ときは a だけが消す強さとして使われる。
    QColor bakeColor(const ToolContext &ctx) const;
    // 現在の筆圧から、このツールの設定に沿った半径とスタンプ濃度を求める。
    float pressureRadius(int size) const;
    float pressureStampAlpha() const;
    // pendingStamps_に積んだ全スタンプを1回のdispatchでまとめて描く。
    void dispatchStampBatch(ToolContext &ctx, const QVector<PendingStamp> &stamps);
};