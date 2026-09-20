#pragma once

// ---------------------------------------------------------------------------
// CanvasActionHost
// ---------------------------------------------------------------------------
// 各 CanvasAction(色調整/フィルター/変形/レイヤー編集などの一回限りのアクション)が、
// CanvasWidget の内部実装に直接依存せずに必要な資源だけを取得するための狭い
// インターフェース。CanvasWidget がこれを実装する。
//
// アクションは「対象レイヤーへの処理」と「専用パネルの表示」を担うが、そのために
// 必要なのは以下だけ:
//   - パネルの親ウィジェット / 位置計算 / makeCurrent の対象となる QWidget
//   - 共有GL資源・ドキュメントへの参照(ToolContext)
//   - 現在のドキュメント
//   - makeCurrent() / update() / updateCursor() の呼び出し
// これらを介することで、アクションは CanvasWidget の巨大なヘッダを include せずに済み、
// CanvasWidget 側もアクションの中身を知らなくてよくなる。
// ---------------------------------------------------------------------------

class QWidget;
class CanvasDocument;
struct ToolContext;

class CanvasActionHost
{
public:
    virtual ~CanvasActionHost() = default;

    // パネルの親・位置計算(width()/mapToGlobal())・makeCurrent の対象となるウィジェット。
    // 実体は CanvasWidget 自身。
    virtual QWidget       *hostWidget()      = 0;
    virtual ToolContext   &hostToolContext() = 0;
    virtual CanvasDocument *hostDocument()   = 0;

    virtual void hostMakeCurrent()  = 0; // CanvasWidget::makeCurrent()
    virtual void hostUpdate()       = 0; // CanvasWidget::update()
    virtual void hostUpdateCursor() = 0; // CanvasWidget::updateCursor()

    // レイヤー編集系アクション(調整/単色/テキスト)専用。
    // テキストボックスのラスタライズはタイル書き込み・GL資源再確保を伴う低レベル処理
    // (TILE_SIZE・スライス・growLayerBounds等)のため、CanvasWidget側に残したまま委譲する。
    virtual void hostRasterizeTextLayer(int layerIndex) = 0;
    // LayerDockのサムネイル等を更新するための layersChanged シグナル発行。
    virtual void hostNotifyLayersChanged() = 0;

    // フィルターレイヤーのパラメータを変えたときに呼ぶ(FilterLayerEditAction専用)。
    // オフスクリーンで作り置きしてある合成結果(CanvasWidget::rebuildFilterChain)を
    // 破棄して、次の描画で組み直させる。doc_->onChanged を経由しない直接編集なので
    // 自動では無効化されない。
    virtual void hostInvalidateFilterChain() = 0;

    // 移動/回転ツールへの切替だけを素通しする系のアクション(blocksToolInput()==true
    // かつ blocksAllToolInput()==false)が開始/終了する際に CanvasActionController から
    // 呼ばれる。開始時は現在のツールを退避して移動ツールへ切り替え、終了時は退避した
    // ツールへ戻す(パネルを開いたまま視点だけ操作したい、というUXのための自動化)。
    virtual void hostNotifyToolBlockingActionStarted() = 0;
    virtual void hostNotifyToolBlockingActionEnded()   = 0;
};
