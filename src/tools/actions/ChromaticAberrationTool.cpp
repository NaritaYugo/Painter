#include "tools/actions/ChromaticAberrationTool.h"
#include <QtMinMax>
#include <QtMath>
#include <QMouseEvent>
#include <QPainter>
#include <QLineF>

void ChromaticAberrationTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

void ChromaticAberrationTool::activate(ToolContext &ctx)
{
    layerIndex_ = ctx.doc->activeLayerIndex();
    const Layer &layer = ctx.doc->layers[layerIndex_];
    layerW_ = layer.tilesX() * ctx.tileSize;
    layerH_ = layer.tilesY() * ctx.tileSize;
    previewOriginPx_ = QVector2D((float)(layer.originTx * ctx.tileSize), (float)(layer.originTy * ctx.tileSize));

    engaged_ = (layerW_ > 0 && layerH_ > 0);
    if (!engaged_) return;

    centerPx_ = QVector2D((float)layerW_ * 0.5f, (float)layerH_ * 0.5f); // 既定はレイヤー中心
    mode_ = Mode::Parallel;
    angleDeg_ = 0.0f;
    distancePx_ = 8.0f;
    draggingCenter_ = false;

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

void ChromaticAberrationTool::deactivate()
{
    engaged_ = false;
    layerIndex_ = -1;
}

void ChromaticAberrationTool::setMode(ToolContext &ctx, Mode mode)
{
    mode_ = mode;
    if (engaged_) updatePreview(ctx);
}

void ChromaticAberrationTool::setAngleDeg(ToolContext &ctx, float angleDeg)
{
    // 0〜360の範囲に正規化する(スライダーからの入力は既に範囲内のはずだが念のため)
    angleDeg_ = std::fmod(std::fmod(angleDeg, 360.0f) + 360.0f, 360.0f);
    if (engaged_) updatePreview(ctx);
}

void ChromaticAberrationTool::setCenterPx(ToolContext &ctx, QVector2D centerPx)
{
    centerPx_ = centerPx;
    if (engaged_) updatePreview(ctx);
}

void ChromaticAberrationTool::setDistancePx(ToolContext &ctx, float distancePx)
{
    distancePx_ = qBound(0.0f, distancePx, 512.0f);
    if (engaged_) updatePreview(ctx);
}

void ChromaticAberrationTool::updatePreview(ToolContext &ctx)
{
    const Layer &layer = ctx.doc->layers[layerIndex_];

    glBindImageTexture(0, ctx.fullLayerTex,     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
    glBindImageTexture(1, ctx.transformSrcTex,  0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(2, ctx.selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);

    ctx.computeChromaticAberrationFilterProgram->bind();
    ctx.computeChromaticAberrationFilterProgram->setUniformValue("uMode", (int)mode_);
    ctx.computeChromaticAberrationFilterProgram->setUniformValue("uAngleRad", qDegreesToRadians(angleDeg_));
    ctx.computeChromaticAberrationFilterProgram->setUniformValue("uCenterPx", centerPx_);
    ctx.computeChromaticAberrationFilterProgram->setUniformValue("uDistancePx", distancePx_);
    {
        GLint loc = glGetUniformLocation(ctx.computeChromaticAberrationFilterProgram->programId(), "uSelMaskOffset");
        if (loc >= 0)
            glUniform2i(loc, layer.originTx * ctx.tileSize, layer.originTy * ctx.tileSize);
    }
    glDispatchCompute((layerW_ + 15) / 16, (layerH_ + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    ctx.computeChromaticAberrationFilterProgram->release();
}

void ChromaticAberrationTool::confirm(ToolContext &ctx)
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

QVector2D ChromaticAberrationTool::centerCanvasPx() const
{
    return previewOriginPx_ + centerPx_;
}

bool ChromaticAberrationTool::onMousePress(QMouseEvent *event, ToolContext &ctx)
{
    if (!engaged_ || mode_ != Mode::Radial) return false;
    if (event->button() != Qt::LeftButton) return false;
    if (!ctx.pixelToWidget) return false;

    const QPointF handleWidget = ctx.pixelToWidget(centerCanvasPx());
    const double dist = QLineF(handleWidget, event->position()).length();
    constexpr double kHitRadiusWidget = 12.0;
    if (dist > kHitRadiusWidget) return false;

    draggingCenter_ = true;
    return true;
}

void ChromaticAberrationTool::onMouseMove(QMouseEvent *event, ToolContext &ctx)
{
    if (!draggingCenter_ || !ctx.widgetToPixel) return;
    const QVector2D canvasPx = ctx.widgetToPixel(event->position());
    setCenterPx(ctx, canvasPx - previewOriginPx_);
}

void ChromaticAberrationTool::onMouseRelease(QMouseEvent *event, ToolContext &ctx)
{
    Q_UNUSED(event);
    Q_UNUSED(ctx);
    draggingCenter_ = false;
}

void ChromaticAberrationTool::paintOverlay(QPainter &painter, const ToolContext &ctx) const
{
    if (!engaged_ || mode_ != Mode::Radial || !ctx.pixelToWidget) return;

    const QPointF p = ctx.pixelToWidget(centerCanvasPx());
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(255, 170, 40, 230), 1.5));
    painter.setBrush(QColor(255, 255, 255, 230));
    painter.drawEllipse(p, 7, 7);
    painter.drawLine(QPointF(p.x() - 11, p.y()), QPointF(p.x() + 11, p.y()));
    painter.drawLine(QPointF(p.x(), p.y() - 11), QPointF(p.x(), p.y() + 11));
}
