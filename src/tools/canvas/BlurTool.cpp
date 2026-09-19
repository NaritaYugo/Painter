#include "tools/canvas/BlurTool.h"
#include "tools/core/PressureResponse.h"
#include "tools/core/DispatchBounds.h"
#include "tools/core/ToolDispatchUtil.h"

#include <cmath>

void BlurTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

std::optional<QCursor> BlurTool::cursor(const ToolContext &ctx) const
{
    int size = toolCfg_ ? toolCfg_->blur().size() : 20;
    // view->scale() はキャンバスpx→デバイスpx。カーソル画像は論理pxで作るので
    // viewDpr で割って画面上の見た目のサイズに合わせる(ToolContext::viewDpr参照)。
    float viewScale = (ctx.view ? ctx.view->scale() : 1.0f) / (ctx.viewDpr > 0.0f ? ctx.viewDpr : 1.0f);
    return CursorUtils::makeCircleCursor(size * viewScale);
}

void BlurTool::ensureBlurTex(int w, int h)
{
    if (blurTex_ && blurTexW_ == w && blurTexH_ == h) return;
    if (blurTex_) { glDeleteTextures(1, &blurTex_); blurTex_ = 0; }

    glGenTextures(1, &blurTex_);
    glBindTexture(GL_TEXTURE_2D, blurTex_);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    blurTexW_ = w;
    blurTexH_ = h;
}

void BlurTool::onMousePress(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    if (ctx.doc->layers.isEmpty()) return;

    dragging_ = true;
    ctx.beginStrokeUndo();

    lastPos_ = ctx.widgetToPixel(event->position());
    applyBlur(ctx, lastPos_, lastPos_);
}

void BlurTool::onMouseMove(QMouseEvent *event, ToolContext &ctx)
{
    if (!dragging_) return;
    QVector2D p = ctx.widgetToPixel(event->position());
    applyBlur(ctx, lastPos_, p);
    lastPos_ = p;
}

void BlurTool::onMouseRelease(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    if (!dragging_) return;
    dragging_ = false;

    ctx.commitStrokeUndo();
    ctx.requestRepaint();
    ctx.notifyLayersChanged();
}

void BlurTool::applyBlur(ToolContext &ctx, const QVector2D &from, const QVector2D &to)
{
    if (!prog_ || ctx.doc->layers.isEmpty()) return;

    const int canvasW  = ctx.canvasW;
    const int canvasH  = ctx.canvasH;
    const int tileSize = ctx.tileSize;
    const Layer &layer = ctx.doc->activeLayer();
    GLuint defaultFbo  = ctx.defaultFbo();

    const BlurToolConfig &cfg = toolCfg_->blur();
    float radius = cfg.size() / 2.0f * PressureResponse::scale(pressure_, cfg.minSizeRatio());
    if (radius < 0.5f) radius = 0.5f;
    const float hardness = cfg.hardness();
    const float strength = cfg.strength();
    const int   kernelR  = qBound(1, cfg.blurRadius(), 32);

    ensureBlurTex(canvasW, canvasH);

    // 影響ピクセル範囲・タイル範囲(書き込み範囲/カーネル分だけ広げた収集範囲)
    const StrokeDispatchBounds bounds = computeStrokeDispatchBounds(
        canvasW, canvasH, tileSize, ctx.doc->tilesX(), ctx.doc->tilesY(),
        from, to, radius, kernelR);
    if (bounds.empty()) return;

    const int wTxMin = bounds.wTxMin, wTxMax = bounds.wTxMax;
    const int wTyMin = bounds.wTyMin, wTyMax = bounds.wTyMax;
    const int gTxMin = bounds.gTxMin, gTxMax = bounds.gTxMax;
    const int gTyMin = bounds.gTyMin, gTyMax = bounds.gTyMax;

    // ラップ有効時、ブラシがキャンバス端をはみ出す分だけ反対側にも書き込みが
    // 発生する(Pen同様、ブラシの一部が反対側の端にも描かれる)。その追加の
    // タイル範囲をここでまとめて求めておく(Undoキャプチャ/書き戻し/ディスパッチ
    // 領域の拡張で使う)。
    const QVector<WrapTileRange> extraWrapRanges = computeExtraWrapTileRanges(
        canvasW, canvasH, tileSize, ctx.doc->tilesX(), ctx.doc->tilesY(),
        ctx.wrapX, ctx.wrapY,
        bounds.rawPxMinX, bounds.rawPxMaxX, bounds.rawPxMinY, bounds.rawPxMaxY,
        wTxMin, wTxMax, wTyMin, wTyMax);

    // Undo: 書き込むタイルの「ぼかす前」の内容をキャプチャ(書き戻しより前に呼ぶ)
    ctx.expandStrokeUndoRegion(wTxMin, wTxMax, wTyMin, wTyMax);
    for (const WrapTileRange &r : extraWrapRanges)
        ctx.expandStrokeUndoRegion(r.txMin, r.txMax, r.tyMin, r.tyMax);

    // 1) 収集タイルを layerTexArray -> fullLayerTex に展開
    {
        GLuint srcFbo = 0, dstFbo = 0;
        glGenFramebuffers(1, &srcFbo);
        glGenFramebuffers(1, &dstFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.fullLayerTex, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        auto blitTiles = [&](int txMin, int txMax, int tyMin, int tyMax) {
            forEachLayerTileInRange(layer, canvasW, canvasH, tileSize, txMin, txMax, tyMin, tyMax,
                [&](int, int, int si, int dstX, int dstY, int w, int h) {
                    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                    glBlitFramebuffer(0, 0, w, h, dstX, dstY, dstX + w, dstY + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                });
        };
        blitTiles(gTxMin, gTxMax, gTyMin, gTyMax);
        // ラップ有効かつ収集範囲がキャンバス端に達している場合、resolveSampleCoord()が
        // カーネルサンプリング時に反対側の端へ周回するため、その実座標のタイルも
        // fullLayerTexへ展開しておかないと(古いデータのまま=正しくサンプルできない)。
        const int tilesX = ctx.doc->tilesX(), tilesY = ctx.doc->tilesY();
        if (ctx.wrapX && gTxMin == 0)          blitTiles(tilesX - 1, tilesX - 1, gTyMin, gTyMax);
        if (ctx.wrapX && gTxMax == tilesX - 1) blitTiles(0, 0, gTyMin, gTyMax);
        if (ctx.wrapY && gTyMin == 0)          blitTiles(gTxMin, gTxMax, tilesY - 1, tilesY - 1);
        if (ctx.wrapY && gTyMax == tilesY - 1) blitTiles(gTxMin, gTxMax, 0, 0);
        // 四隅(x/y両方がラップする場合)も念のため
        if (ctx.wrapX && ctx.wrapY) {
            if (gTxMin == 0 && gTyMin == 0)                   blitTiles(tilesX - 1, tilesX - 1, tilesY - 1, tilesY - 1);
            if (gTxMin == 0 && gTyMax == tilesY - 1)          blitTiles(tilesX - 1, tilesX - 1, 0, 0);
            if (gTxMax == tilesX - 1 && gTyMin == 0)          blitTiles(0, 0, tilesY - 1, tilesY - 1);
            if (gTxMax == tilesX - 1 && gTyMax == tilesY - 1) blitTiles(0, 0, 0, 0);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &srcFbo);
        glDeleteFramebuffers(1, &dstFbo);
    }

    // glBlitFramebufferによるfullLayerTexへの書き込みを、直後のcompute shaderの
    // imageLoadから確実に見えるようにする(blit/framebuffer書き込みとイメージ読み込みは
    // 別のメモリアクセス経路のため、明示的なバリアが無いと反映前の内容を読む可能性がある)
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    // 処理領域 = 書き込みタイルをピクセルに整列した範囲。ラップ有効軸は、
    // ブラシの生bbox(タイル整列前)がキャンバス外まではみ出しているぶんだけ
    // 領域を拡張する(shader側でその拡張ぶんを周回書き込みする)。
    int regionMinX = bounds.regionMinX;
    int regionMinY = bounds.regionMinY;
    int regionMaxX = bounds.regionMaxX;
    int regionMaxY = bounds.regionMaxY;
    extendRegionForWrap(canvasW, canvasH, tileSize, ctx.doc->tilesX(), ctx.doc->tilesY(),
                         ctx.wrapX, ctx.wrapY,
                         bounds.rawPxMinX, bounds.rawPxMaxX, bounds.rawPxMinY, bounds.rawPxMaxY,
                         wTxMin, wTxMax, wTyMin, wTyMax,
                         regionMinX, regionMaxX, regionMinY, regionMaxY);

    // 2) ぼかし compute: fullLayerTex(read) -> blurTex_(write)
    glBindImageTexture(0, ctx.fullLayerTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
    glBindImageTexture(1, blurTex_,         0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(2, ctx.selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    prog_->bind();
    prog_->setUniformValue("uPosStart",     from);
    prog_->setUniformValue("uPosEnd",       to);
    prog_->setUniformValue("uRadius",       radius);
    prog_->setUniformValue("uHardness",     hardness);
    prog_->setUniformValue("uStrength",     strength);
    prog_->setUniformValue("uKernelRadius", kernelR);
    prog_->setUniformValue("uRegionMinX",   regionMinX);
    prog_->setUniformValue("uRegionMinY",   regionMinY);
    prog_->setUniformValue("uRegionMaxX",   regionMaxX);
    prog_->setUniformValue("uRegionMaxY",   regionMaxY);
    prog_->setUniformValue("uWrapX",        ctx.wrapX ? 1 : 0);
    prog_->setUniformValue("uWrapY",        ctx.wrapY ? 1 : 0);
    const int rw = regionMaxX - regionMinX + 1;
    const int rh = regionMaxY - regionMinY + 1;
    glDispatchCompute((rw + 15) / 16, (rh + 15) / 16, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    prog_->release();

    // 3) blurTex_ -> layerTexArray(書き込みタイルのみ)
    {
        GLuint srcFbo = 0, dstFbo = 0;
        glGenFramebuffers(1, &srcFbo);
        glGenFramebuffers(1, &dstFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blurTex_, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        auto writeBackTiles = [&](int txMin, int txMax, int tyMin, int tyMax) {
            forEachLayerTileInRange(layer, canvasW, canvasH, tileSize, txMin, txMax, tyMin, tyMax,
                [&](int, int, int si, int srcX, int srcY, int w, int h) {
                    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                    glBlitFramebuffer(srcX, srcY, srcX + w, srcY + h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                });
        };
        writeBackTiles(wTxMin, wTxMax, wTyMin, wTyMax);
        // ラップではみ出して反対側にも書き込まれた分(blurTex_内、その実座標に
        // 既に正しい結果が書かれている)も書き戻す
        for (const WrapTileRange &r : extraWrapRanges)
            writeBackTiles(r.txMin, r.txMax, r.tyMin, r.tyMax);
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &srcFbo);
        glDeleteFramebuffers(1, &dstFbo);
    }

    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    ctx.requestRepaint();
}
