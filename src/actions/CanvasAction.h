#pragma once

#include <QtGlobal>
#include <QVector2D>

#include <QPointF>

class QOpenGLContext;
class QOpenGLShaderProgram;
class QMouseEvent;
class QWidget;
class QPainter;
struct ToolContext;
class CanvasActionHost;
class CanvasActionController;

class CanvasAction
{
public:
    CanvasAction(CanvasActionHost &host, CanvasActionController &ctrl)
        : host_(host), ctrl_(ctrl) {}
    virtual ~CanvasAction() = default;

    bool isActive() const { return active_; }

    // コントローラから呼ばれるライフサイクル。
    bool begin();   // 成功して active になったら true
    void confirm();
    void cancel();

    // CanvasWidgetとの結合点。既定では何もしない。
    virtual void initialize(QOpenGLContext * /*ctx*/) {}
    // CanvasWidget デストラクタから、GLコンテキストがまだ有効なうちに一度だけ呼ばれる。
    virtual void releaseGL() {}
    // ウィジェットのリサイズ時、アクティブなアクションに対してだけ呼ばれる。
    virtual void positionPanel() {}
    // このアクションが表示中、通常のペン等ツール入力をブロックするか
    virtual bool blocksToolInput() const { return true; }
    // blocksToolInput()==true のアクションのうち、移動/回転ツールへの切り替えだけは素通しする(パネルを開いたままキャンバスを見る位置調整ができる)か。
    virtual bool blocksAllToolInput() const { return false; }
    // render.frag 用の per-tool uniform(uIsXxxTool + origin/size 等)を設定する。
    virtual void applyRenderState(QOpenGLShaderProgram * /*renderProg*/) {}
    // GL 描画後の QPainter オーバーレイ(変形枠・ハンドル等)。
    virtual bool paintOverlay(QPainter & /*painter*/, const ToolContext & /*ctx*/) { return false; }

    // 操作を消費した場合はtrueを返す。
    virtual bool handleMousePress      (QMouseEvent * /*e*/, ToolContext & /*ctx*/) { return false; }
    virtual bool handleMouseMove       (QMouseEvent * /*e*/, ToolContext & /*ctx*/) { return false; }
    virtual bool handleMouseRelease    (QMouseEvent * /*e*/, ToolContext & /*ctx*/) { return false; }
    virtual bool handleMouseDoubleClick(QMouseEvent * /*e*/, ToolContext & /*ctx*/) { return false; }

    // 選択範囲の点線(マーチングアンツ)を、このアクションのプレビュー変換へ追従させる。
    virtual bool mapSelectionOutlinePoint(QPointF & /*canvasPx*/) const { return false; }

    // Shift+WASD 等のキーボードによる平行移動(変形/自由変形アクションのみ対応)。
    virtual bool nudge(const QVector2D & /*delta*/) { return false; }

protected:
    // 多くのパネルで共通の「ウィジェット上端中央(y=20)に sizeHint で配置」処理。
    void centerPanelTop(QWidget *panel);

    // レイヤー種別などの事前ガード(既定は常に許可)。
    virtual bool canActivate() const { return true; }
    // ツールを activate しパネルを生成/配線/表示する。
    virtual bool onActivate() = 0;
    // 確定(タイルへ焼き込む等)。
    virtual void onConfirm() = 0;
    // キャンセル(レイヤー本体には触れず破棄)。
    virtual void onCancel() = 0;

    CanvasActionHost       &host_;
    CanvasActionController  &ctrl_;
    bool active_ = false;
};
