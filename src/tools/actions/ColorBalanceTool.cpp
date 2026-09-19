#include "tools/actions/ColorBalanceTool.h"
#include "tools/core/ToolDispatchUtil.h"

void ColorBalanceTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

void ColorBalanceTool::activate(ToolContext &ctx)
{
    Q_UNUSED(ctx);
    cyan_ = magenta_ = yellow_ = 0;
    engaged_ = true;
}

void ColorBalanceTool::deactivate()
{
    engaged_ = false;
    cyan_ = magenta_ = yellow_ = 0;
}

void ColorBalanceTool::confirm(ToolContext &ctx)
{
    if (!engaged_) return;
    applyAdjustment(ctx); // 値が全て0(恒等調整)なら内部で何もしない
    deactivate();
    ctx.requestRepaint();
}

void ColorBalanceTool::applyAdjustment(ToolContext &ctx)
{
    if (cyan_ == 0 && magenta_ == 0 && yellow_ == 0) return;

    const int canvasW = ctx.canvasW, canvasH = ctx.canvasH, tileSize = ctx.tileSize;
    const Layer &layer = ctx.doc->activeLayer();
    const GLuint defaultFbo = ctx.defaultFbo();

    ctx.beginStrokeUndo();
    ctx.expandStrokeUndoRegion(0, ctx.doc->tilesX() - 1, 0, ctx.doc->tilesY() - 1);

    // 1. アクティブレイヤーのタイルをfullLayerTexへ展開する
    {
        GLuint srcFbo = 0, dstFbo = 0;
        glGenFramebuffers(1, &srcFbo);
        glGenFramebuffers(1, &dstFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.fullLayerTex, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        forEachActiveLayerTile(*ctx.doc, layer, canvasW, canvasH, tileSize,
            [&](int, int, int si, int dstX, int dstY, int w, int h) {
                glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                glBlitFramebuffer(0, 0, w, h, dstX, dstY, dstX + w, dstY + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            });
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &srcFbo);
        glDeleteFramebuffers(1, &dstFbo);
    }
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    // 2. colorBalance.comp で fullLayerTex に直接(読み書き両用で)焼き込む
    glBindImageTexture(0, ctx.fullLayerTex,     0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
    glBindImageTexture(1, ctx.selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);

    ctx.computeColorBalanceProgram->bind();
    ctx.computeColorBalanceProgram->setUniformValue("uCyanShift",    cyan_    / 100.0f);
    ctx.computeColorBalanceProgram->setUniformValue("uMagentaShift", magenta_ / 100.0f);
    ctx.computeColorBalanceProgram->setUniformValue("uYellowShift",  yellow_  / 100.0f);
    glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    ctx.computeColorBalanceProgram->release();

    // 3. fullLayerTex -> タイルに書き戻す
    {
        GLuint srcFbo = 0, dstFbo = 0;
        glGenFramebuffers(1, &srcFbo);
        glGenFramebuffers(1, &dstFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.fullLayerTex, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        forEachActiveLayerTile(*ctx.doc, layer, canvasW, canvasH, tileSize,
            [&](int, int, int si, int srcX, int srcY, int w, int h) {
                glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                glBlitFramebuffer(srcX, srcY, srcX + w, srcY + h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            });
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &srcFbo);
        glDeleteFramebuffers(1, &dstFbo);
    }
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    ctx.commitStrokeUndo();
    ctx.requestRepaint();
    ctx.notifyLayersChanged();
}
