#include "tools/actions/ToneCurveTool.h"
#include "tools/core/ToolDispatchUtil.h"
#include "tools/core/ToneCurveMath.h"

#include <algorithm>
#include <cmath>

void ToneCurveTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

void ToneCurveTool::activate(ToolContext &ctx)
{
    points_ = { QPointF(0, 0), QPointF(255, 255) };
    rebuildLut();
    uploadLut(ctx);
    engaged_ = true;
}

void ToneCurveTool::deactivate()
{
    engaged_ = false;
    points_ = { QPointF(0, 0), QPointF(255, 255) };
}

void ToneCurveTool::setControlPoints(ToolContext &ctx, const QVector<QPointF> &points)
{
    points_ = points;
    if (points_.size() < 2) return; // ToneCurveEditor側で常に2点以上を保証している前提
    rebuildLut();
    uploadLut(ctx);
}

bool ToneCurveTool::isIdentity() const
{
    if (points_.size() != 2) return false;
    return std::abs(points_[0].x())         < 0.001 && std::abs(points_[0].y())         < 0.001
        && std::abs(points_[1].x() - 255.0) < 0.001 && std::abs(points_[1].y() - 255.0) < 0.001;
}

// 制御点間を3次エルミートスプラインで補間し、256段階(0-255入力)のLUTを構築する。
// dialogs/ToneCurveEditor.cppのプレビュー曲線と全く同じ補間(tools/core/ToneCurveMath.h
// に集約してある。以前は前者にもここにも別々に実装があり、区間幅を無視した
// 「一様」Catmull-Romのバグ(節点で曲線が折れ曲がる)が生じていた)。
void ToneCurveTool::rebuildLut()
{
    ToneCurveMath::buildLut256(points_, lut_);
}

void ToneCurveTool::uploadLut(ToolContext &ctx)
{
    if (ctx.toneCurveLUTTex == 0) return;
    glBindTexture(GL_TEXTURE_2D, ctx.toneCurveLUTTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RED, GL_UNSIGNED_BYTE, lut_);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ToneCurveTool::confirm(ToolContext &ctx)
{
    if (!engaged_) return;
    applyAdjustment(ctx); // 恒等カーブなら内部で何もしない
    deactivate();
    ctx.requestRepaint();
}

void ToneCurveTool::applyAdjustment(ToolContext &ctx)
{
    if (isIdentity()) return;

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

    // 2. toneCurve.comp で fullLayerTex に直接(読み書き両用で)焼き込む。
    //    LUTは通常のサンプラーとしてテクスチャユニット2へ束縛する。
    glBindImageTexture(0, ctx.fullLayerTex,     0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
    glBindImageTexture(1, ctx.selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.toneCurveLUTTex);

    ctx.computeToneCurveProgram->bind();
    ctx.computeToneCurveProgram->setUniformValue("uLut", 2);
    glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    ctx.computeToneCurveProgram->release();

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
