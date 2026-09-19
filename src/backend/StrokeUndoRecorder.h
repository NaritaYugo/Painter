#pragma once

#include "tools/core/ToolContext.h"
#include "backend/CanvasDocument.h"
#include <QSet>
#include <functional>

// ---------------------------------------------------------------------------
// StrokeUndoRecorder
// ---------------------------------------------------------------------------
// 旧 GLWidget の以下のメンバ/メソッドをそのまま移したもの:
//   m_pendingUndoTiles, m_strokeTxMin/Max, m_strokeTyMin/Max
//   beginStrokeUndo() / expandStrokeUndoRegion() / commitStrokeUndo()
//   captureAllTiles() / restoreLayer()
//
// マウスイベントを受け取らないので Tool は継承しない。
// GL資源へのアクセスは ToolContext をそのまま再利用する
// (Tool側と同じ layerTexArray/tileSize/defaultFbo が必要なため)。
//
// 「1ストローク分だけ差分キャプチャして1つのUndoEntryにまとめる」until commit、
// という時間軸の状態(pending_, 触れたタイル範囲)だけをこのクラスが持つ。
// UndoStack自体(pushUndo/applyUndo等)は引き続き CanvasDocument が持つ。
// ---------------------------------------------------------------------------
class StrokeUndoRecorder
{
public:
    // ストローク開始: 前回の記録をクリアする(前回取り出し損ねたPBOがあれば後始末する)
    void beginStroke(ToolContext &ctx);

    // 新しく触れるタイル範囲をキャプチャする(すでに触れた範囲は再キャプチャしない)
    void expandRegion(ToolContext &ctx, int txMin, int txMax, int tyMin, int tyMax);

    bool hasPending() const { return !pending_.isEmpty(); }

    // 蓄積したタイル差分を1つのUndoEntryにまとめて返す(内部状態はクリアされる)。
    // expandRegion()でPBOへ発行しておいた転送結果をここで取り出す(GPU→CPU同期はここに集約)。
    UndoEntry takeStrokeEntry(ToolContext &ctx, int layerIndex);

    // 指定レイヤーの全タイルをキャプチャする(Undo/Redo時のスナップショット用)
    UndoEntry captureAllTiles(ToolContext &ctx, int layerIndex);

    // shapeで渡された各TileUndoの(tx,ty)位置についてだけ、現在のピクセル内容を
    // キャプチャする(captureAllTilesの部分版)。GLWidget::undo()/redo()が、
    // これから復元しようとしているエントリが実際に触れるタイルだけを"current"
    // (取り消す直前の状態)としてキャプチャし、レイヤー全体の読み出しを避けるために使う。
    // sliceIndexはshape側のものを使わず、layerの現在のtileSlice(tx,ty)を都度引き直す。
    UndoEntry captureTilesLike(ToolContext &ctx, int layerIndex, const QVector<TileUndo> &shape);
 
    // UndoEntryの内容をlayerTexArrayに書き戻す
    void restoreLayer(ToolContext &ctx, const UndoEntry &entry);
 
    // シリアライザ用: 指定スライスの生ピクセルを読み出す/書き込む
    // (captureAllTiles/restoreLayerと同じ「タイル単位のGL⇔メモリ転送」という関心事なのでここに置く)
    QByteArray readSlicePixels(ToolContext &ctx, int texArraySlice);
    void writeSlicePixels(ToolContext &ctx, int texArraySlice, const QByteArray &raw);

    // readSlicePixels()をタイル数ぶん逐次呼ぶと、glReadPixels(クライアントメモリへの読み出し)
    // が呼び出しのたびにGPUの完了を待ってCPU側をブロックしてしまう(=タイル枚数ぶんの
    // ストールが直列に積み重なる)。PBO(ピクセルバッファオブジェクト)のリングバッファへ
    // glReadPixelsを発行するとGPU→PBOへの転送は非同期に予約されるだけで即座に戻るため、
    // 数枚先までReadPixelsを発行してからまとめてglMapBufferRangeで取り出すことで、
    // GPU側の転送とCPU側の発行をパイプライン化しストールを削減できる。戻り値の
    // 呼び出し自体は同期的(全タイル読み終わるまで戻らない)なままなので、
    // 呼び出し側(save()/captureAllLayerSnapshots())の変更は最小限で済む。
    QVector<QByteArray> readSlicesBatch(ToolContext &ctx, const QVector<int> &slices);

    // 書き込み側のバッチ版(load()のように多数のタイルをまとめて書く箇所向け)。
    // 転送自体は読み出し側のようなPBO非同期化はしていない(理由は実装側のコメント参照)。
    // slicesとdataは同じ長さ・同じ順序であること。onProgressは一定枚数ごとに
    // 「完了した枚数の累計」で呼ばれる(進捗表示が固まらないように)。
    void writeSlicesBatch(ToolContext &ctx, const QVector<int> &slices,
                          const QVector<const QByteArray *> &data,
                          const std::function<void(int)> &onProgress = {});

private:
    // ストローク中に新規タイルへ触れるたびに、そのタイルの「触れる前」の内容を
    // glReadPixels で即座に(同期的に)読み出すと、1タイルごとにGPUの完了待ちで
    // CPU側がストールする(ブラシが大きいほど1スタンプで触れるタイル数が増え、
    // 顕著に重くなる原因だった)。PBO(ピクセルバッファオブジェクト)へ
    // glReadPixels を発行するとGPU→PBOへの転送はキューイングされるだけで
    // 即座に戻る(同一コンテキストのコマンドは発行順に実行されるため、後続の
    // stroke.comp によるmaskTexへの書き込みとは順序が保証され、正しく
    // 「描く前」のlayerTexArrayの内容を読める)ため、ストローク中は転送を
    // 予約するだけにとどめ、実際にCPUへ取り出す(glMapBufferRange、ここで初めて
    // 待ちが発生しうる)のはストローク確定時(takeStrokeEntry)まで遅延させる。
    QVector<TileUndo> pending_;
    QVector<GLuint>   pendingPbos_; // pending_ と同じ並びで対応するPBO(まだ取り出していない転送)
    // 実際にキャプチャ済みの(レイヤーローカル)タイル座標の集合。矩形のジグザグ
    // ストロークなど、触れたタイルが凸包にならない軌跡ではバウンディングボックスだけで
    // 「既にキャプチャ済みか」を判定できない(範囲内だが未キャプチャのタイルが
    // 誤って除外され、Undo時にそのタイルだけ復元されなくなる)ため、タイル単位で管理する。
    QSet<qint64> capturedTiles_;
    static qint64 tileKey(int tx, int ty) { return (qint64(tx) << 32) ^ quint32(ty); }

    // expandRegion()はストローク中(マウス移動のたび)に呼ばれるホットパスなので、
    // 呼び出しのたびにFBOをgen/deleteするドライバ呼び出しのオーバーヘッドを避けるため、
    // 使い回し用のFBOを1つ保持しておく(初回呼び出し時に確保。以後は同じFBOを
    // 使い回す。GLコンテキストが生きている間は破棄しない=アプリ終了までそのまま)。
    GLuint expandRegionFbo_ = 0;
};