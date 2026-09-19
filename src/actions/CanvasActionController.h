#pragma once

#include "actions/CanvasAction.h"

#include <QVector2D>
#include <memory>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// CanvasActionController
// ---------------------------------------------------------------------------
// GLWidget が1つ保持する、全 CanvasAction の所有者兼ディスパッチャ。
// 以前 GLWidget に散らばっていた以下を1箇所に集約する:
//   - 相互排他: 新しいアクションを start する前に、今アクティブなアクションを cancel する
//     (各 startXxxAction 冒頭の巨大な cancel リストを置き換える)。
//   - 描画 uniform / オーバーレイ / パネル位置 / マウス横取り / 入力ブロック判定を、
//     「現在アクティブなアクション」へ委譲する。
// アクションは add<T>() で生成・所有され、host と自分自身(ctrl)が注入される。
// ---------------------------------------------------------------------------

class QOpenGLContext;
class QOpenGLShaderProgram;
class QMouseEvent;
class QPainter;
struct ToolContext;
class CanvasActionHost;

class CanvasActionController
{
public:
    void setHost(CanvasActionHost &host) { host_ = &host; }

    // アクションを生成して所有する。host と *this を注入したうえで T を構築する。
    // 追加の構築引数(Pro/Free 差分など)は Args... で渡せる。
    template <class T, class... Args>
    T *add(Args &&...args)
    {
        auto up = std::make_unique<T>(*host_, *this, std::forward<Args>(args)...);
        T *raw = up.get();
        actions_.push_back(std::move(up));
        return raw;
    }

    // アクション開始: 今アクティブなものを cancel してから begin する。
    void start(CanvasAction *action);
    void confirmActive();
    void cancelActive();

    CanvasAction *activeAction() const { return active_; }
    bool isBusy() const { return active_ != nullptr; }

    // 現在のアクションが通常ツール入力をブロックするか。
    bool toolInputBlocked() const { return active_ && active_->blocksToolInput(); }
    // 現在のアクションが移動/回転ツールへの切替すら素通ししない(完全にブロックする)か。
    bool toolInputFullyBlocked() const { return active_ && active_->blocksAllToolInput(); }

    // GL 初期化(全アクションの initialize を呼ぶ)。
    void initializeAll(QOpenGLContext *ctx);
    // GLWidget デストラクタから、GLコンテキストがまだ有効なうちに呼ぶ(全アクションの releaseGL)。
    void releaseAllGL();

    // paintGL / resizeGL からの委譲。
    void applyRenderState(QOpenGLShaderProgram *renderProg);
    bool paintActiveOverlay(QPainter &painter, const ToolContext &ctx); // 描いたら true
    void positionActivePanel();

    // マウスイベントの委譲(消費されたら true)。
    bool routeMousePress      (QMouseEvent *e, ToolContext &ctx);
    bool routeMouseMove       (QMouseEvent *e, ToolContext &ctx);
    bool routeMouseRelease    (QMouseEvent *e, ToolContext &ctx);
    bool routeMouseDoubleClick(QMouseEvent *e, ToolContext &ctx);

    // キーボードショートカット(Shift+WASD)による平行移動。アクティブなアクションが
    // 実際に処理したら true(GLWidget側はこの場合 nudgeActiveLayer 等へフォールバックしない)。
    bool nudgeActive(const QVector2D &delta);

private:
    CanvasActionHost *host_ = nullptr;
    std::vector<std::unique_ptr<CanvasAction>> actions_;
    CanvasAction *active_ = nullptr;
};
