#include "tools/canvas/WarpTool.h"
#include "tools/core/PressureResponse.h"
#include "tools/core/DispatchBounds.h"
#include "tools/core/ToolDispatchUtil.h"

#include <cmath>

void WarpTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

std::optional<QCursor> WarpTool::cursor(const ToolContext &ctx) const
{
    int size = toolCfg_ ? toolCfg_->warp().size() : 40;
    // view->scale() はキャンバスpx→デバイスpx。カーソル画像は論理pxで作るので
    // viewDpr で割って画面上の見た目のサイズに合わせる(ToolContext::viewDpr参照)。
    float viewScale = (ctx.view ? ctx.view->scale() : 1.0f) / (ctx.viewDpr > 0.0f ? ctx.viewDpr : 1.0f);
    return CursorUtils::makeCircleCursor(size * viewScale);
}

void WarpTool::ensureTextures(int w, int h)
{
    if (texW_ == w && texH_ == h && srcTex_ && dispTex_ && dispPrevTex_ && warpTex_) return;

    GLuint old[4] = { srcTex_, dispTex_, dispPrevTex_, warpTex_ };
    for (GLuint t : old) if (t) glDeleteTextures(1, &t);
    srcTex_ = dispTex_ = dispPrevTex_ = warpTex_ = 0;

    auto make = [&](GLenum fmt) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return t;
    };
    srcTex_      = make(GL_RGBA8);
    dispTex_     = make(GL_RG32F);
    dispPrevTex_ = make(GL_RG32F);
    warpTex_     = make(GL_RGBA8);

    texW_ = w;
    texH_ = h;
}

// ストローク開始時の下準備。
//  ・アクティブレイヤーの内容を srcTex_ へ丸ごと退避する(以降ストローク中は不変)
//  ・累積変位場をゼロクリアする
// これ以降、ドラッグ中は「srcTex_ を累積変位で1回引き直す」だけになるので、
// どれだけ長くドラッグしても補間の回数は増えない = ボケが溜まらない。
bool WarpTool::beginStroke(ToolContext &ctx)
{
    const int canvasW = ctx.canvasW, canvasH = ctx.canvasH;
    if (canvasW <= 0 || canvasH <= 0) return false;

    ensureTextures(canvasW, canvasH);
    if (!srcTex_ || !dispTex_) return false;

    const Layer &layer = ctx.doc->activeLayer();

    GLuint srcFbo = 0, dstFbo = 0;
    glGenFramebuffers(1, &srcFbo);
    glGenFramebuffers(1, &dstFbo);

    const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    // レイヤーがタイルを持たない領域(単色/調整レイヤー等)を読んでも未初期化の
    // ゴミが混ざらないよう、退避先を先に透明で埋めておく。
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTex_, 0);
    glClearBufferfv(GL_COLOR, 0, zero);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
    forEachLayerTileInRange(layer, canvasW, canvasH, ctx.tileSize,
        0, ctx.doc->tilesX() - 1, 0, ctx.doc->tilesY() - 1,
        [&](int, int, int si, int dstX, int dstY, int w, int h) {
            glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
            glBlitFramebuffer(0, 0, w, h, dstX, dstY, dstX + w, dstY + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        });

    // 変位場のゼロクリア。GL4.3にはglClearTexImage(4.4)が無いのでFBO経由でクリアする。
    // dispPrevTex_ は毎ステップ必要範囲だけを写して使う(=全面は埋まらない)ので、
    // 未初期化のゴミを変位として読んでしまわないようここで一度だけ均しておく。
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dispTex_, 0);
    glClearBufferfv(GL_COLOR, 0, zero);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dispPrevTex_, 0);
    glClearBufferfv(GL_COLOR, 0, zero);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx.defaultFbo());
    glDeleteFramebuffers(1, &srcFbo);
    glDeleteFramebuffers(1, &dstFbo);

    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    return true;
}

void WarpTool::onMousePress(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    if (!prog_ || !toolCfg_) return;
    if (ctx.doc->layers.isEmpty()) return;

    dragging_ = true;
    ctx.beginStrokeUndo();

    lastPos_ = ctx.widgetToPixel(event->position());
    ready_   = beginStroke(ctx);
    // 押しただけ(ドラッグ量ゼロ)では変位が生まれず、書き戻しても同じ画素を
    // 書き直すだけなのでディスパッチはしない。
}

void WarpTool::onMouseMove(QMouseEvent *event, ToolContext &ctx)
{
    if (!dragging_) return;
    QVector2D p = ctx.widgetToPixel(event->position());
    applyWarp(ctx, lastPos_, p);
    lastPos_ = p;
}

void WarpTool::onMouseRelease(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    if (!dragging_) return;
    dragging_ = false;
    ready_    = false;

    ctx.commitStrokeUndo();
    ctx.requestRepaint();
    ctx.notifyLayersChanged();
}

void WarpTool::applyWarp(ToolContext &ctx, const QVector2D &from, const QVector2D &to)
{
    if (!ready_ || !prog_ || ctx.doc->layers.isEmpty()) return;

    const WarpToolConfig &cfg = toolCfg_->warp();
    float radius = cfg.size() / 2.0f * PressureResponse::scale(pressure_, cfg.minSizeRatio());
    if (radius < 0.5f) radius = 0.5f;
    const float hardness = cfg.hardness();
    const float strength = cfg.strength();

    // 1イベント分の移動量がブラシ半径に対して大きいと、半径より遠くから色を
    // 引っぱってくることになり絵が千切れてしまう。線分を半径の半分以下の長さに
    // 刻んで順に適用することで、素早く振ったときも滑らかに引きずれる。
    // 通常の移動量(半径の半分以下)では分割は起きず、従来どおり1回で済む。
    const QVector2D delta = to - from;
    const int nSub = qBound(1, (int)std::ceil(delta.length() / qMax(radius * 0.5f, 1.0f)), 24);

    for (int i = 0; i < nSub; i++) {
        const QVector2D a = from + delta * (float(i)     / float(nSub));
        const QVector2D b = from + delta * (float(i + 1) / float(nSub));
        dispatchWarpStep(ctx, a, b, radius, hardness, strength);
    }

    ctx.requestRepaint();
}

// 線分1本ぶんのゆがみを適用する(変位場の更新 → 元画像の引き直し → タイルへ書き戻し)。
// 書き戻しはタイル単位なので、分割した線分ごとに完結させる必要がある: 斜めのドラッグでは
// 分割後の各線分のbboxを足しても元の線分のbbox矩形にはならず(階段状に隙間が空く)、
// 書き戻し範囲だけをまとめて広く取ると、一度もディスパッチされていないタイルの
// warpTex_(=前回の残骸)をレイヤーへ書き込んでしまう。
void WarpTool::dispatchWarpStep(ToolContext &ctx, const QVector2D &from, const QVector2D &to,
                                 float radius, float hardness, float strength)
{
    const int canvasW  = ctx.canvasW;
    const int canvasH  = ctx.canvasH;
    const int tileSize = ctx.tileSize;
    const int tilesX   = ctx.doc->tilesX();
    const int tilesY   = ctx.doc->tilesY();
    const Layer &layer = ctx.doc->activeLayer();
    GLuint defaultFbo  = ctx.defaultFbo();

    const QVector2D delta = to - from;

    // 変位場の読み取りが引き戻される最大距離ぶん、退避範囲に余白を足す。
    // 引き戻し量は |delta| * amt で amt <= strength なのでこの見積もりで足りる
    // (バイリニア補間の1px分も含めて安全側に+2)。
    const int margin = qMax(2, (int)std::ceil(delta.length() * strength) + 2);

    const StrokeDispatchBounds b = computeStrokeDispatchBounds(
        canvasW, canvasH, tileSize, tilesX, tilesY, from, to, radius, margin);
    if (b.empty()) return;

    // ラップ有効時、ブラシがキャンバス端をはみ出す分だけ反対側にも書き込みが
    // 発生する。その追加のタイル範囲をここでまとめて求めておく(Undoキャプチャ/
    // 書き戻しで使う)。
    const QVector<WrapTileRange> extraWrapRanges = computeExtraWrapTileRanges(
        canvasW, canvasH, tileSize, tilesX, tilesY,
        ctx.wrapX, ctx.wrapY,
        b.rawPxMinX, b.rawPxMaxX, b.rawPxMinY, b.rawPxMaxY,
        b.wTxMin, b.wTxMax, b.wTyMin, b.wTyMax);

    // Undo: 書き込むタイルの「ゆがめる前」の内容をキャプチャ(書き戻しより前に呼ぶ)
    ctx.expandStrokeUndoRegion(b.wTxMin, b.wTxMax, b.wTyMin, b.wTyMax);
    for (const WrapTileRange &r : extraWrapRanges)
        ctx.expandStrokeUndoRegion(r.txMin, r.txMax, r.tyMin, r.tyMax);

    // 1) このステップ開始時点の変位を dispPrevTex_ へ退避する。
    //    変位の合成は「引き戻した先の画素の変位」を読むため、同じテクスチャを
    //    読みながら書くと他の invocation の書き込みと競合する(結果が不定になる)。
    //    全面コピーは帯域の無駄なので、実際に読まれる範囲(=収集タイル範囲)だけ写す。
    {
        auto copyDisp = [&](int txMin, int txMax, int tyMin, int tyMax) {
            const int x = txMin * tileSize;
            const int y = tyMin * tileSize;
            const int w = qMin(canvasW, (txMax + 1) * tileSize) - x;
            const int h = qMin(canvasH, (tyMax + 1) * tileSize) - y;
            if (w <= 0 || h <= 0) return;
            glCopyImageSubData(dispTex_,     GL_TEXTURE_2D, 0, x, y, 0,
                               dispPrevTex_, GL_TEXTURE_2D, 0, x, y, 0, w, h, 1);
        };
        copyDisp(b.gTxMin, b.gTxMax, b.gTyMin, b.gTyMax);
        // ラップ有効かつ収集範囲がキャンバス端に達している場合、引き戻し先が反対側の端へ
        // 周回するため、その実座標ぶんの変位も退避しておかないと古い値を読んでしまう。
        if (ctx.wrapX && b.gTxMin == 0)          copyDisp(tilesX - 1, tilesX - 1, b.gTyMin, b.gTyMax);
        if (ctx.wrapX && b.gTxMax == tilesX - 1) copyDisp(0, 0, b.gTyMin, b.gTyMax);
        if (ctx.wrapY && b.gTyMin == 0)          copyDisp(b.gTxMin, b.gTxMax, tilesY - 1, tilesY - 1);
        if (ctx.wrapY && b.gTyMax == tilesY - 1) copyDisp(b.gTxMin, b.gTxMax, 0, 0);
        // 四隅(x/y両方がラップする場合)も念のため
        if (ctx.wrapX && ctx.wrapY) {
            if (b.gTxMin == 0 && b.gTyMin == 0)                   copyDisp(tilesX - 1, tilesX - 1, tilesY - 1, tilesY - 1);
            if (b.gTxMin == 0 && b.gTyMax == tilesY - 1)          copyDisp(tilesX - 1, tilesX - 1, 0, 0);
            if (b.gTxMax == tilesX - 1 && b.gTyMin == 0)          copyDisp(0, 0, tilesY - 1, tilesY - 1);
            if (b.gTxMax == tilesX - 1 && b.gTyMax == tilesY - 1) copyDisp(0, 0, 0, 0);
        }
    }

    // コピーの結果を直後の compute shader の imageLoad から確実に見えるようにする
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    // 処理領域 = 書き込みタイルをピクセルに整列した範囲。ラップ有効軸は、
    // ブラシの生bbox(タイル整列前)がキャンバス外まではみ出しているぶんだけ
    // 領域を拡張する(shader側でその拡張ぶんを周回書き込みする)。
    int regionMinX = b.regionMinX;
    int regionMinY = b.regionMinY;
    int regionMaxX = b.regionMaxX;
    int regionMaxY = b.regionMaxY;
    extendRegionForWrap(canvasW, canvasH, tileSize, tilesX, tilesY,
                         ctx.wrapX, ctx.wrapY,
                         b.rawPxMinX, b.rawPxMaxX, b.rawPxMinY, b.rawPxMaxY,
                         b.wTxMin, b.wTxMax, b.wTyMin, b.wTyMax,
                         regionMinX, regionMaxX, regionMinY, regionMaxY);

    // 2) ゆがみ compute: srcTex_(不変の元画像) + dispPrevTex_ -> warpTex_ + dispTex_
    glBindImageTexture(0, srcTex_,             0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
    glBindImageTexture(1, warpTex_,            0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(2, ctx.selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    glBindImageTexture(3, dispPrevTex_,        0, GL_FALSE, 0, GL_READ_ONLY,  GL_RG32F);
    glBindImageTexture(4, dispTex_,            0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG32F);
    prog_->bind();
    prog_->setUniformValue("uPosStart",   from);
    prog_->setUniformValue("uPosEnd",     to);
    prog_->setUniformValue("uDelta",      delta);
    prog_->setUniformValue("uRadius",     radius);
    prog_->setUniformValue("uHardness",   hardness);
    prog_->setUniformValue("uStrength",   strength);
    prog_->setUniformValue("uRegionMinX", regionMinX);
    prog_->setUniformValue("uRegionMinY", regionMinY);
    prog_->setUniformValue("uRegionMaxX", regionMaxX);
    prog_->setUniformValue("uRegionMaxY", regionMaxY);
    prog_->setUniformValue("uWrapX",      ctx.wrapX ? 1 : 0);
    prog_->setUniformValue("uWrapY",      ctx.wrapY ? 1 : 0);
    const int rw = regionMaxX - regionMinX + 1;
    const int rh = regionMaxY - regionMinY + 1;
    glDispatchCompute((rw + 15) / 16, (rh + 15) / 16, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    prog_->release();

    // 3) warpTex_ -> layerTexArray(書き込みタイルのみ)
    {
        GLuint srcFbo = 0, dstFbo = 0;
        glGenFramebuffers(1, &srcFbo);
        glGenFramebuffers(1, &dstFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, warpTex_, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        auto writeBackTiles = [&](int txMin, int txMax, int tyMin, int tyMax) {
            forEachLayerTileInRange(layer, canvasW, canvasH, tileSize, txMin, txMax, tyMin, tyMax,
                [&](int, int, int si, int srcX, int srcY, int w, int h) {
                    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                    glBlitFramebuffer(srcX, srcY, srcX + w, srcY + h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                });
        };
        writeBackTiles(b.wTxMin, b.wTxMax, b.wTyMin, b.wTyMax);
        for (const WrapTileRange &r : extraWrapRanges)
            writeBackTiles(r.txMin, r.txMax, r.tyMin, r.tyMax);
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &srcFbo);
        glDeleteFramebuffers(1, &dstFbo);
    }

    glMemoryBarrier(GL_ALL_BARRIER_BITS);
}
