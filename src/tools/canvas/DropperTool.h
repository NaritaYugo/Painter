#pragma once

#include "tools/core/Tool.h"
#include "tools/core/ToolConfig.h"
#include "tools/core/CursorUtils.h"

// ---------------------------------------------------------------------------
// DropperTool
// ---------------------------------------------------------------------------
// 押している間(ドラッグ中含む)は常に現在位置の色をサンプリングし、離した時点の
// 色を実際の描画色として通知する。
//
// カーソルは常にスポイトのアイコンで、ホットスポット(=実際に色を拾う点)は
// アイコンの左下に描かれた先端に合わせてある。ドラッグ中は、そのアイコンの右隣に
// 現在拾っている色のスウォッチを並べた画像へ差し替える(キャンバスへのオーバーレイ
// 描画ではなくカーソル画像に含めることで、スウォッチがカーソルと完全に同じ速さで
// 動く。CursorUtils::makeIconWithSwatchCursorのコメント参照)。
// ---------------------------------------------------------------------------
class DropperTool : public Tool
{
public:
    // MainWindowが所有する唯一のToolConfigへの非所有ポインタ(GLWidget経由で渡される)
    void setToolConfig(ToolConfig *cfg) { toolCfg_ = cfg; }

    void onMousePress(QMouseEvent *event, ToolContext &ctx) override
    {
        if (event->button() != Qt::LeftButton) return;
        dragging_ = true;
        sample(event->position(), ctx);
    }

    void onMouseMove(QMouseEvent *event, ToolContext &ctx) override
    {
        if (!dragging_) return;
        sample(event->position(), ctx);
    }

    void onMouseRelease(QMouseEvent *event, ToolContext &ctx) override
    {
        if (event->button() != Qt::LeftButton) return;
        if (!dragging_) return;
        sample(event->position(), ctx);
        dragging_ = false;
        ctx.notifyColorDropped(lastColor_, pickedTransparent_);
    }

    // 押している間(=ドラッグでプレビュー中)かどうか
    bool isActive() const override { return dragging_; }

    // ドラッグ中にキャンバスの見た目が変わる要素は何も無い(色を読むだけで、
    // プレビューはカーソル画像側に出る)ので、定期的な再描画は不要。
    // これを止めないと、ドラッグ中ずっと全レイヤーの再合成(部分再描画も効かない)が
    // 走り続け、重いだけでなくGLWidget側の適応間隔(最大100ms)が伸びてしまう。
    bool needsCanvasRepaintWhileActive() const override { return false; }

    std::optional<QCursor> cursor(const ToolContext &ctx) const override
    {
        Q_UNUSED(ctx);
        static const QString kIcon = QStringLiteral(":/icons/cursor/dropper.png");
        if (dragging_)
            return CursorUtils::makeIconWithSwatchCursor(kIcon, lastColor_, 24);
        return CursorUtils::makeIconCursor(kIcon, 24, /*hotspotAtBottomLeft=*/true);
    }

private:
    ToolConfig *toolCfg_ = nullptr;
    bool        dragging_ = false;
    QColor      lastColor_ = Qt::transparent;
    bool        pickedTransparent_ = false;

    // スポイトが拾うのは「RGB、または透明色」だけで、不透明度(アルファ)には触れない。
    // アルファは通常色では濃さ、透明色では消す強さを表す“ブラシ側の設定”であって、
    // 画面から拾ってくる性質の値ではないため(受け取る MainWindow 側で現在値を維持する)。
    void sample(const QPointF &pos, ToolContext &ctx)
    {
        const bool referenceCanvas = toolCfg_ && toolCfg_->dropper().referenceCanvas();
        const QColor picked = ctx.getPixelColor(pos, referenceCanvas);

        // 完全に透明な画素は「透明色」として拾う。
        pickedTransparent_ = (picked.alpha() == 0);
        lastColor_ = pickedTransparent_
            ? QColor(Qt::transparent)
            : QColor(picked.red(), picked.green(), picked.blue(), 255);
    }
};
