#include "tools/actions/MosaicTool.h"
#include <QtMinMax>

void MosaicTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

void MosaicTool::activate(ToolContext &ctx)
{
    layerIndex_ = ctx.doc->activeLayerIndex();
    const Layer &layer = ctx.doc->layers[layerIndex_];
    layerW_ = layer.tilesX() * ctx.tileSize;
    layerH_ = layer.tilesY() * ctx.tileSize;
    previewOriginPx_ = QVector2D((float)(layer.originTx * ctx.tileSize), (float)(layer.originTy * ctx.tileSize));

    engaged_ = (layerW_ > 0 && layerH_ > 0);
    if (!engaged_) return;

    ctx.ensureTransformScratchSize(layerW_, layerH_);

    // 1. アクティブレイヤーのタイルを fullLayerTex へ展開する。以後、ブロックサイズを
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

void MosaicTool::deactivate()
{
    engaged_ = false;
    layerIndex_ = -1;
}

void MosaicTool::setBlockSize(ToolContext &ctx, int blockSize)
{
    blockSize_ = qBound(2, blockSize, 256);
    if (engaged_) updatePreview(ctx);
}

void MosaicTool::updatePreview(ToolContext &ctx)
{
    const Layer &layer = ctx.doc->layers[layerIndex_];

    // ブロック平均は「1ブロックにつき1回」だけ集約パスで出し、本体パスはそれを
    // 引くだけにする。全画素がそれぞれブロック内を舐めていた頃はブロックサイズの
    // 2乗で重くなり、上限の256では実測62.5秒かかっていた
    // (詳細は mosaicReduce.comp のコメント参照)。
    const GLuint blocksTex = ctx.ensureFilterScratch(layerW_, layerH_);

    // 1パス目: ブロックごとの平均色を blocksTex の左上へ集約する
    ctx.computeMosaicReduceProgram->bind();
    glBindImageTexture(0, ctx.fullLayerTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
    glBindImageTexture(1, blocksTex,        0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    ctx.computeMosaicReduceProgram->setUniformValue("uBlockSize", blockSize_);
    {
        GLint loc = glGetUniformLocation(ctx.computeMosaicReduceProgram->programId(), "uBlockOriginPx");
        if (loc >= 0) glUniform2i(loc, 0, 0);
    }
    // ディスパッチはブロック格子ぶんだけ(全画素ぶんではない)。
    // 除数の8は mosaicReduce.comp の local_size(8x8)に合わせてある。
    const int gridX = (layerW_ + blockSize_ - 1) / blockSize_;
    const int gridY = (layerH_ + blockSize_ - 1) / blockSize_;
    glDispatchCompute((gridX + 7) / 8, (gridY + 7) / 8, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    ctx.computeMosaicReduceProgram->release();

    // 2パス目: 集約済みの平均色を引いて、選択範囲で元画像とブレンドする
    glBindImageTexture(0, ctx.fullLayerTex,     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
    glBindImageTexture(1, ctx.transformSrcTex,  0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(2, ctx.selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);
    glBindImageTexture(3, blocksTex,            0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);

    ctx.computeMosaicFilterProgram->bind();
    ctx.computeMosaicFilterProgram->setUniformValue("uBlockSize", blockSize_);
    {
        GLint loc = glGetUniformLocation(ctx.computeMosaicFilterProgram->programId(), "uBlockOriginPx");
        if (loc >= 0) glUniform2i(loc, 0, 0);
    }
    {
        GLint loc = glGetUniformLocation(ctx.computeMosaicFilterProgram->programId(), "uSelMaskOffset");
        if (loc >= 0)
            glUniform2i(loc, layer.originTx * ctx.tileSize, layer.originTy * ctx.tileSize);
    }
    glDispatchCompute((layerW_ + 15) / 16, (layerH_ + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    ctx.computeMosaicFilterProgram->release();
}

void MosaicTool::confirm(ToolContext &ctx)
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
