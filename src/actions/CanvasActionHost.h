#pragma once

// CanvasActionHost

class QWidget;
class CanvasDocument;
struct ToolContext;

class CanvasActionHost
{
public:
    virtual ~CanvasActionHost() = default;

    // パネルの親・位置計算(width()/mapToGlobal())・makeCurrent の対象となるウィジェット。
    virtual QWidget       *hostWidget()      = 0;
    virtual ToolContext   &hostToolContext() = 0;
    virtual CanvasDocument *hostDocument()   = 0;

    virtual void hostMakeCurrent()  = 0; // CanvasWidget::makeCurrent()
    virtual void hostUpdate()       = 0; // CanvasWidget::update()
    virtual void hostUpdateCursor() = 0; // CanvasWidget::updateCursor()

    // レイヤー編集系アクション(調整/単色/テキスト)専用。
    virtual void hostRasterizeTextLayer(int layerIndex) = 0;
    // LayerDockのサムネイル等を更新するための layersChanged シグナル発行。
    virtual void hostNotifyLayersChanged() = 0;

    // フィルターレイヤーのパラメータを変えたときに呼ぶ(FilterLayerEditAction専用)。
    virtual void hostInvalidateFilterChain() = 0;

    // 移動/回転ツールへの切替だけを素通しする系のアクション(blocksToolInput()==trueかつ blocksAllToolInput()==false)が開始/終了する際に CanvasActionController
    // から呼ばれる。
    virtual void hostNotifyToolBlockingActionStarted() = 0;
    virtual void hostNotifyToolBlockingActionEnded()   = 0;
};
