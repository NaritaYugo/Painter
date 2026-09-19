#pragma once

#include "tools/core/Tool.h"
#include "widgets/NativeWindowLog.h"

#include <QElapsedTimer>
#include <QtMath>

// ---------------------------------------------------------------------------
// MoveTool
// ---------------------------------------------------------------------------
// 旧 GLWidget::mousePressEvent/mouseMoveEvent の ToolType::Move 分岐を移したもの。
// ViewTransform::pan() を呼ぶだけの薄いツール。
// ---------------------------------------------------------------------------
class MoveTool : public Tool
{
public:
    void onMousePress(QMouseEvent *event, ToolContext &ctx) override
    {
        Q_UNUSED(ctx);
        lastMousePos_ = QVector2D(event->position().x(), event->position().y());
        dragging_ = true;
        // 計測用(TIEPOLO_WINLOG=1のとき): 1ドラッグぶんの集計。
        // 「ポインタが動いた距離」と「実際にビューを動かした距離」を突き合わせるため。
        startPos_   = lastMousePos_;
        pannedPx_   = QVector2D(0, 0);
        moveEvents_ = 0;
        maxBacklogMs_ = 0;
        firstTimestamp_ = event->timestamp();
        if (WinLog::enabled()) dragClock_.start();
    }

    void onMouseMove(QMouseEvent *event, ToolContext &ctx) override
    {
        if (!dragging_ || !(event->buttons() & Qt::LeftButton)) return;

        QVector2D cur(event->position().x(), event->position().y());
        QVector2D delta = cur - lastMousePos_;
        delta.setY(-delta.y()); // ウィジェット座標(Y下向き) → キャンバス座標(Y上向き)
        // ビューの画面座標はデバイスpx、マウスは論理px(ToolContext::viewDpr参照)
        const QVector2D applied = delta * ctx.viewDpr;
        ctx.view->pan(applied);
        pannedPx_ += applied;
        moveEvents_++;

        // 入力の滞留量(計測用)。
        //
        // イベントが「発生した時刻」(OSが付けたタイムスタンプ)と「処理している時刻」の
        // 差が広がっていくなら、入力キューに溜まっている＝画面がポインタから遅れて
        // 追いかけている、ということ。ドラッグ開始時点を基準に、そこからどれだけ
        // 余分に遅れたかを見る(両者は時計が違うので、差の“増え方”だけを見る)。
        if (WinLog::enabled() && dragClock_.isValid()) {
            const qint64 osElapsed    = (qint64)event->timestamp() - (qint64)firstTimestamp_;
            const qint64 localElapsed = dragClock_.elapsed();
            maxBacklogMs_ = qMax(maxBacklogMs_, localElapsed - osElapsed);
        }

        lastMousePos_ = cur;
        ctx.requestRepaint();
    }

    void onMouseRelease(QMouseEvent *event, ToolContext &ctx) override
    {
        Q_UNUSED(event);
        // ドラッグ1回ぶんの集計を1行だけ出す。ポインタの移動量(論理px)と、
        // それに対して実際にビューが動いた量(デバイスpx)を並べて出すので、
        // 「付いてくるのが遅い」が“遅延”なのか“移動量が足りない”のかを切り分けられる。
        // 期待値は panned == pointer * viewDpr。
        if (dragging_ && WinLog::enabled()) {
            const QVector2D span = lastMousePos_ - startPos_;
            WINLOG(QStringLiteral("MOVE drag: %10ms events=%1 遅れ最大=%11ms "
                                   "pointer=(%2,%3)論理px 距離=%4 "
                                   "panned=(%5,%6)デバイスpx 距離=%7 viewDpr=%8 比=%9")
                       .arg(moveEvents_)
                       .arg(span.x(), 0, 'f', 1).arg(span.y(), 0, 'f', 1)
                       .arg(span.length(), 0, 'f', 1)
                       .arg(pannedPx_.x(), 0, 'f', 1).arg(pannedPx_.y(), 0, 'f', 1)
                       .arg(pannedPx_.length(), 0, 'f', 1)
                       .arg(ctx.viewDpr, 0, 'f', 3)
                       .arg(span.length() > 0.01f ? pannedPx_.length() / span.length() : 0.0f,
                            0, 'f', 3)
                       .arg(dragClock_.isValid() ? dragClock_.elapsed() : 0)
                       .arg(maxBacklogMs_));
        }
        dragging_ = false;
    }

    bool isActive() const override { return dragging_; }
    bool transformsViewWhileActive() const override { return true; }

private:
    bool      dragging_ = false;
    QVector2D lastMousePos_;
    // 計測用(上のonMouseReleaseのコメント参照)
    QVector2D     startPos_;
    QVector2D     pannedPx_;
    int           moveEvents_ = 0;
    QElapsedTimer dragClock_;
    quint64       firstTimestamp_ = 0;
    qint64        maxBacklogMs_   = 0;
};