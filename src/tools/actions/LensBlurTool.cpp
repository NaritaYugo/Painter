#include "tools/actions/LensBlurTool.h"
#include <QtMinMax>
#include <QtMath>

void LensBlurTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

void LensBlurTool::activate(ToolContext &ctx)
{
    layerIndex_ = ctx.doc->activeLayerIndex();
    const Layer &layer = ctx.doc->layers[layerIndex_];
    layerW_ = layer.tilesX() * ctx.tileSize;
    layerH_ = layer.tilesY() * ctx.tileSize;
    previewOriginPx_ = QVector2D((float)(layer.originTx * ctx.tileSize), (float)(layer.originTy * ctx.tileSize));

    engaged_ = (layerW_ > 0 && layerH_ > 0);
    if (!engaged_) return;

    radiusPx_ = 16.0f;
    blades_ = 0;
    bladeRotDeg_ = 0.0f;
    highlightBoost_ = 0.6f;
    threshold_ = 0.7f;

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

void LensBlurTool::deactivate()
{
    engaged_ = false;
    layerIndex_ = -1;
}

void LensBlurTool::setRadiusPx(ToolContext &ctx, float radiusPx)
{
    radiusPx_ = qBound(0.0f, radiusPx, 64.0f);
    if (engaged_) updatePreview(ctx);
}

void LensBlurTool::setBlades(ToolContext &ctx, int blades)
{
    blades_ = (blades < 3) ? 0 : qBound(3, blades, 12);
    if (engaged_) updatePreview(ctx);
}

void LensBlurTool::setBladeRotDeg(ToolContext &ctx, float rotDeg)
{
    bladeRotDeg_ = std::fmod(std::fmod(rotDeg, 360.0f) + 360.0f, 360.0f);
    if (engaged_) updatePreview(ctx);
}

void LensBlurTool::setHighlightBoost(ToolContext &ctx, float boost)
{
    highlightBoost_ = qBound(0.0f, boost, 1.0f);
    if (engaged_) updatePreview(ctx);
}

void LensBlurTool::setThreshold(ToolContext &ctx, float threshold)
{
    threshold_ = qBound(0.0f, threshold, 1.0f);
    if (engaged_) updatePreview(ctx);
}

int LensBlurTool::sampleCount() const
{
    // 円板の面積に比例させると重すぎるので、半径に比例(周長オーダー)させたうえで
    // 上限を設ける。玉ボケは元々滑らかな見た目なので、これで十分きれいに埋まる。
    return qBound(12, (int)std::lround(radiusPx_ * 6.0f), 192);
}

float LensBlurTool::bokehGamma() const
{
    // ハイライト強調スライダーを上げるほど、平均前のγを強くして明るい画素を
    // 際立たせる(1=無変換、4程度でしっかり玉が出る)。
    return 1.0f + highlightBoost_ * 3.0f;
}

void LensBlurTool::updatePreview(ToolContext &ctx)
{
    if (!ctx.computeLensBlurFilterProgram) return;
    const Layer &layer = ctx.doc->layers[layerIndex_];

    glBindImageTexture(0, ctx.fullLayerTex,     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
    glBindImageTexture(1, ctx.transformSrcTex,  0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(2, ctx.selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);

    ctx.computeLensBlurFilterProgram->bind();
    ctx.computeLensBlurFilterProgram->setUniformValue("uRadiusPx", radiusPx_);
    ctx.computeLensBlurFilterProgram->setUniformValue("uSamples", sampleCount());
    ctx.computeLensBlurFilterProgram->setUniformValue("uBlades", blades_);
    ctx.computeLensBlurFilterProgram->setUniformValue("uBladeRotRad", (float)qDegreesToRadians(bladeRotDeg_));
    ctx.computeLensBlurFilterProgram->setUniformValue("uBokehGamma", bokehGamma());
    ctx.computeLensBlurFilterProgram->setUniformValue("uThreshold", threshold_);
    // シェーダー側の重みは 1 + boost * smoothstep(...) なので、スライダー1.0で
    // ハイライトが周囲の数倍の重みになるようスケールしておく。
    ctx.computeLensBlurFilterProgram->setUniformValue("uHighlightBoost", highlightBoost_ * 8.0f);
    {
        GLint loc = glGetUniformLocation(ctx.computeLensBlurFilterProgram->programId(), "uSelMaskOffset");
        if (loc >= 0)
            glUniform2i(loc, layer.originTx * ctx.tileSize, layer.originTy * ctx.tileSize);
    }
    glDispatchCompute((layerW_ + 15) / 16, (layerH_ + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    ctx.computeLensBlurFilterProgram->release();
}

void LensBlurTool::confirm(ToolContext &ctx)
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
