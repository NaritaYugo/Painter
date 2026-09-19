#include "tools/actions/GradientMapTool.h"
#include "tools/core/ToolDispatchUtil.h"

#include <algorithm>
#include <cmath>

void GradientMapTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

void GradientMapTool::activate(ToolContext &ctx)
{
    stops_ = { { 0.0f, QColor(0, 0, 0) }, { 1.0f, QColor(255, 255, 255) } };
    rebuildLut();
    uploadLut(ctx);
    engaged_ = true;
}

void GradientMapTool::deactivate()
{
    engaged_ = false;
    stops_ = { { 0.0f, QColor(0, 0, 0) }, { 1.0f, QColor(255, 255, 255) } };
}

void GradientMapTool::setStops(ToolContext &ctx, const QVector<Stop> &stops)
{
    if (stops.size() < 2) return; // GradientStripEditor側で常に2個以上を保証している前提
    stops_ = stops;
    rebuildLut();
    uploadLut(ctx);
}

// ストップ間を線形補間して256段階(輝度0-255入力)のLUTを構築する。
// 先頭より手前・末尾より奥は端の色でクランプする(GradientStripEditorのプレビュー
// (QLinearGradient)と同じ見え方になるよう、補間もsRGB空間の素直な線形補間にする)。
void GradientMapTool::rebuildLut()
{
    const int n = stops_.size();
    for (int i = 0; i < 256; i++) {
        const float pos = float(i) / 255.0f;

        QColor c;
        if (pos <= stops_.front().pos) {
            c = stops_.front().color;
        } else if (pos >= stops_.back().pos) {
            c = stops_.back().color;
        } else {
            c = stops_.back().color;
            for (int k = 0; k + 1 < n; k++) {
                const Stop &a = stops_[k], &b = stops_[k + 1];
                if (pos >= a.pos && pos <= b.pos) {
                    const float span = b.pos - a.pos;
                    const float t = (span > 1e-6f) ? (pos - a.pos) / span : 0.0f;
                    c = QColor::fromRgbF(
                        a.color.redF()   + (b.color.redF()   - a.color.redF())   * t,
                        a.color.greenF() + (b.color.greenF() - a.color.greenF()) * t,
                        a.color.blueF()  + (b.color.blueF()  - a.color.blueF())  * t);
                    break;
                }
            }
        }

        lut_[i * 4 + 0] = (quint8)std::clamp(c.red(),   0, 255);
        lut_[i * 4 + 1] = (quint8)std::clamp(c.green(), 0, 255);
        lut_[i * 4 + 2] = (quint8)std::clamp(c.blue(),  0, 255);
        lut_[i * 4 + 3] = 255;
    }
}

void GradientMapTool::uploadLut(ToolContext &ctx)
{
    if (ctx.gradientMapLUTTex == 0) return;
    glBindTexture(GL_TEXTURE_2D, ctx.gradientMapLUTTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, lut_);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GradientMapTool::confirm(ToolContext &ctx)
{
    if (!engaged_) return;
    applyAdjustment(ctx);
    deactivate();
    ctx.requestRepaint();
}

void GradientMapTool::applyAdjustment(ToolContext &ctx)
{
    if (!ctx.computeGradientMapProgram) return;

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

    // 2. gradientMap.comp で fullLayerTex に直接(読み書き両用で)焼き込む。
    //    LUTは通常のサンプラーとしてテクスチャユニット2へ束縛する(toneCurveと同じ)。
    glBindImageTexture(0, ctx.fullLayerTex,     0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
    glBindImageTexture(1, ctx.selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.gradientMapLUTTex);

    ctx.computeGradientMapProgram->bind();
    ctx.computeGradientMapProgram->setUniformValue("uLut", 2);
    glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    ctx.computeGradientMapProgram->release();

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
