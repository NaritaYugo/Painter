#include "tools/actions/GaussianBlurTool.h"
#include <QtMinMax>

void GaussianBlurTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

void GaussianBlurTool::activate(ToolContext &ctx)
{
    layerIndex_ = ctx.doc->activeLayerIndex();
    const Layer &layer = ctx.doc->layers[layerIndex_];
    layerW_ = layer.tilesX() * ctx.tileSize;
    layerH_ = layer.tilesY() * ctx.tileSize;
    previewOriginPx_ = QVector2D((float)(layer.originTx * ctx.tileSize), (float)(layer.originTy * ctx.tileSize));

    engaged_ = (layerW_ > 0 && layerH_ > 0);
    if (!engaged_) return;

    ctx.ensureTransformScratchSize(layerW_, layerH_);

    // 1. アクティブレイヤーのタイルを fullLayerTex へ展開する。以後、半径を
    // 変えて何度プレビューを更新してもここは再展開しない(元画像は不変なので)。
    const GLuint defaultFbo = ctx.defaultFbo();
    {
        GLuint srcFbo = 0, dstFbo = 0;
        glGenFramebuffers(1, &srcFbo);
        glGenFramebuffers(1, &dstFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.fullLayerTex, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        for (int ty = 0; ty < layer.tilesY(); ty++)
            for (int tx = 0; tx < layer.tilesX(); tx++) {
                int si = layer.tiles[ty][tx];
                int dstX = tx * ctx.tileSize, dstY = ty * ctx.tileSize;
                glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                glBlitFramebuffer(0, 0, ctx.tileSize, ctx.tileSize,
                                  dstX, dstY, dstX + ctx.tileSize, dstY + ctx.tileSize,
                                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &srcFbo);
        glDeleteFramebuffers(1, &dstFbo);
    }
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    updatePreview(ctx);
}

void GaussianBlurTool::deactivate()
{
    engaged_ = false;
    layerIndex_ = -1;
}

void GaussianBlurTool::setRadius(ToolContext &ctx, int radius)
{
    radius_ = qBound(1, radius, 64);
    if (engaged_) updatePreview(ctx);
}

void GaussianBlurTool::updatePreview(ToolContext &ctx)
{
    const Layer &layer = ctx.doc->layers[layerIndex_];

    // 2次元ガウスは G(x,y)=G(x)*G(y) と分離できるので、横1D→縦1Dの2パスに分けて
    // (2r+1)^2 回ではなく 2*(2r+1) 回のサンプリングで済ませる
    // (詳細は gaussianBlurFilter.comp のコメント参照)。中間バッファは元画像
    // (fullLayerTex)ともプレビュー結果(transformSrcTex)とも別の1枚が要る。
    // 数学的には1パス版と同じ結果だが、中間バッファがRGBA8なのでそこでの量子化ぶん、
    // 実際には最大1/255だけずれる(全半径・全ラップ設定で最大差1を実測)。
    const GLuint midTex = ctx.ensureFilterScratch(layerW_, layerH_);

    auto *prog = ctx.computeGaussianBlurFilterProgram;
    prog->bind();
    prog->setUniformValue("uKernelRadius", radius_);
    prog->setUniformValue("uSigma", qMax(0.5f, radius_ / 2.0f));
    prog->setUniformValue("uWrapX", ctx.wrapX ? 1 : 0);
    prog->setUniformValue("uWrapY", ctx.wrapY ? 1 : 0);
    {
        GLint loc = glGetUniformLocation(prog->programId(), "uSelMaskOffset");
        if (loc >= 0)
            glUniform2i(loc, layer.originTx * ctx.tileSize, layer.originTy * ctx.tileSize);
    }
    glBindImageTexture(2, ctx.selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    // 元画像は仕上げパスの選択範囲ブレンドで要るので、両パスとも繋いだままにしておく。
    glBindImageTexture(3, ctx.fullLayerTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);

    const GLuint gx = (layerW_ + 15) / 16, gy = (layerH_ + 15) / 16;

    // 1パス目: 元画像を横方向にぼかして中間バッファへ
    glBindImageTexture(0, ctx.fullLayerTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
    glBindImageTexture(1, midTex,           0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    {
        GLint loc = glGetUniformLocation(prog->programId(), "uDir");
        if (loc >= 0) glUniform2i(loc, 1, 0);
    }
    prog->setUniformValue("uFinalPass", 0);
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    // 2パス目: 中間バッファを縦方向にぼかし、元画像と選択範囲でブレンドして仕上げる
    glBindImageTexture(0, midTex,              0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
    glBindImageTexture(1, ctx.transformSrcTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    {
        GLint loc = glGetUniformLocation(prog->programId(), "uDir");
        if (loc >= 0) glUniform2i(loc, 0, 1);
    }
    prog->setUniformValue("uFinalPass", 1);
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    prog->release();
}

void GaussianBlurTool::confirm(ToolContext &ctx)
{
    if (!engaged_) return;
    const Layer &layer = ctx.doc->layers[layerIndex_];
    const GLuint defaultFbo = ctx.defaultFbo();

    // Undoに記録する「変更前」の状態は、まだ書き換えていない今のlayerTexArrayの
    // 中身そのもの(=フィルター開始前の元画像)。この後の書き戻しより前に捉える。
    ctx.beginStrokeUndo();
    ctx.expandStrokeUndoRegion(0, ctx.doc->tilesX() - 1, 0, ctx.doc->tilesY() - 1);

    // transformSrcTex(直近のプレビュー結果) -> タイルへ書き戻す
    {
        GLuint srcFbo = 0, dstFbo = 0;
        glGenFramebuffers(1, &srcFbo);
        glGenFramebuffers(1, &dstFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.transformSrcTex, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        for (int ty = 0; ty < layer.tilesY(); ty++)
            for (int tx = 0; tx < layer.tilesX(); tx++) {
                int si = layer.tiles[ty][tx];
                int srcX = tx * ctx.tileSize, srcY = ty * ctx.tileSize;
                glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                glBlitFramebuffer(srcX, srcY, srcX + ctx.tileSize, srcY + ctx.tileSize,
                                  0, 0, ctx.tileSize, ctx.tileSize,
                                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &srcFbo);
        glDeleteFramebuffers(1, &dstFbo);
    }
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    ctx.commitStrokeUndo();
    deactivate();
    ctx.requestRepaint();
    ctx.notifyLayersChanged();
}
