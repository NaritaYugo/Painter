#include "tools/actions/NoiseTool.h"
#include <QtMinMax>
#include <QRandomGenerator>

void NoiseTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

void NoiseTool::activate(ToolContext &ctx)
{
    layerIndex_ = ctx.doc->activeLayerIndex();
    const Layer &layer = ctx.doc->layers[layerIndex_];
    layerW_ = layer.tilesX() * ctx.tileSize;
    layerH_ = layer.tilesY() * ctx.tileSize;
    previewOriginPx_ = QVector2D((float)(layer.originTx * ctx.tileSize), (float)(layer.originTy * ctx.tileSize));

    engaged_ = (layerW_ > 0 && layerH_ > 0);
    if (!engaged_) return;

    strength_ = 0.25f;
    monochrome_ = true;
    grainPx_ = 1.0f;
    // 乱数の種はここで1度だけ決める。スライダーを動かすたびに振り直すと粒が
    // チラチラして調整できないため(noiseFilter.compの同趣旨のコメント参照)。
    // 開き直すたびに粒の出方は変わる。
    seed_ = QRandomGenerator::global()->generate();

    ctx.ensureTransformScratchSize(layerW_, layerH_);

    // 1. アクティブレイヤーのタイルを fullLayerTex へ展開する。以後、パラメータを
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

void NoiseTool::deactivate()
{
    engaged_ = false;
    layerIndex_ = -1;
}

void NoiseTool::setStrength(ToolContext &ctx, float strength01)
{
    strength_ = qBound(0.0f, strength01, 1.0f);
    if (engaged_) updatePreview(ctx);
}

void NoiseTool::setMonochrome(ToolContext &ctx, bool mono)
{
    monochrome_ = mono;
    if (engaged_) updatePreview(ctx);
}

void NoiseTool::setGrainPx(ToolContext &ctx, float grainPx)
{
    grainPx_ = qBound(1.0f, grainPx, 16.0f);
    if (engaged_) updatePreview(ctx);
}

void NoiseTool::updatePreview(ToolContext &ctx)
{
    if (!ctx.computeNoiseFilterProgram) return;
    const Layer &layer = ctx.doc->layers[layerIndex_];

    glBindImageTexture(0, ctx.fullLayerTex,     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
    glBindImageTexture(1, ctx.transformSrcTex,  0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(2, ctx.selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);

    ctx.computeNoiseFilterProgram->bind();
    // スライダー100%でストレート色を最大±0.5ずらす。1.0までずらせるようにすると
    // 全面が白飛び/黒潰れして「質感」ではなく砂嵐になってしまうため、この辺で頭打ちにする。
    ctx.computeNoiseFilterProgram->setUniformValue("uAmount", strength_ * 0.5f);
    ctx.computeNoiseFilterProgram->setUniformValue("uMonochrome", monochrome_ ? 1 : 0);
    ctx.computeNoiseFilterProgram->setUniformValue("uGrainPx", grainPx_);
    {
        // uSeedはuint。QOpenGLShaderProgram::setUniformValueにuintのオーバーロードが
        // 無いので、ロケーションを引いてglUniform1uiで直接渡す。
        GLint loc = glGetUniformLocation(ctx.computeNoiseFilterProgram->programId(), "uSeed");
        if (loc >= 0) glUniform1ui(loc, (GLuint)seed_);
    }
    {
        GLint loc = glGetUniformLocation(ctx.computeNoiseFilterProgram->programId(), "uSelMaskOffset");
        if (loc >= 0)
            glUniform2i(loc, layer.originTx * ctx.tileSize, layer.originTy * ctx.tileSize);
    }
    glDispatchCompute((layerW_ + 15) / 16, (layerH_ + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    ctx.computeNoiseFilterProgram->release();
}

void NoiseTool::confirm(ToolContext &ctx)
{
    if (!engaged_) return;
    const Layer &layer = ctx.doc->layers[layerIndex_];
    const GLuint defaultFbo = ctx.defaultFbo();

    // Undoに記録する「変更前」の状態は、まだ書き換えていない今のタイルの中身そのもの
    // (=フィルター開始前の元画像)。この後の書き戻しより前に捉える。
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
