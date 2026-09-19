#include "tools/actions/FreeTransformTool.h"

#include <QPainter>
#include <QLineF>
#include <QFont>
#include <cmath>
#include <algorithm>
#include <climits>

void FreeTransformTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

// ---------------------------------------------------------------------------
// レイヤー自身の全タイル(originTx/originTy起点、tilesX x tilesY、キャンバスより
// 大きい/はみ出している場合を含む)を読み出し、透明でない(alpha>0)ピクセルの
// bboxをキャンバスpx座標で求める。1ピクセルも見つからなければfalseを返す。
//
// タイル確保範囲(レイヤーの矩形)そのものではなく実際に描かれているピクセルの
// 範囲を使うことで、新たに描き込んでいない限り、平行移動を繰り返しても
// 移動対象の範囲が広がらないようにする(タイル確保範囲はgrowLayerBoundsで
// 一方的に広がっていく履歴のため、それをそのまま対象にすると移動のたびに
// 対象範囲が実際の内容より大きくなってしまう)。
static bool computeLayerContentBBox(ToolContext &ctx, const Layer &layer,
                                     int &outMinX, int &outMinY, int &outMaxX, int &outMaxY)
{
    const int tileSize = ctx.tileSize;
    const int layerW = layer.tilesX(), layerH = layer.tilesY();

    int minX = INT_MAX, maxX = INT_MIN, minY = INT_MAX, maxY = INT_MIN;

    GLuint fbo = 0;
    ctx.gl->glGenFramebuffers(1, &fbo);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    ctx.gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);

    QVector<uint8_t> pixels(tileSize * tileSize * 4);
    for (int ty = 0; ty < layerH; ty++) {
        for (int tx = 0; tx < layerW; tx++) {
            int si = layer.tiles[ty][tx];
            ctx.gl->glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
            ctx.gl->glReadPixels(0, 0, tileSize, tileSize, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

            const int tileCanvasX0 = (layer.originTx + tx) * tileSize;
            const int tileCanvasY0 = (layer.originTy + ty) * tileSize;

            for (int py = 0; py < tileSize; py++) {
                const int rowBase = py * tileSize * 4;
                for (int px = 0; px < tileSize; px++) {
                    if (pixels[rowBase + px * 4 + 3] == 0) continue;
                    const int cx = tileCanvasX0 + px, cy = tileCanvasY0 + py;
                    minX = qMin(minX, cx); maxX = qMax(maxX, cx);
                    minY = qMin(minY, cy); maxY = qMax(maxY, cy);
                }
            }
        }
    }

    ctx.gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, ctx.defaultFbo());
    ctx.gl->glDeleteFramebuffers(1, &fbo);

    if (maxX < minX) return false;
    outMinX = minX; outMinY = minY; outMaxX = maxX; outMaxY = maxY;
    return true;
}

// ---------------------------------------------------------------------------
QVector2D FreeTransformTool::centroid() const
{
    return (corners_[0] + corners_[1] + corners_[2] + corners_[3]) * 0.25f;
}

bool FreeTransformTool::pointInQuad(const QVector2D &p, const std::array<QVector2D, 4> &quad)
{
    int sign = 0;
    for (int i = 0; i < 4; i++) {
        const QVector2D &a = quad[i];
        const QVector2D &b = quad[(i + 1) % 4];
        QVector2D edge = b - a;
        QVector2D toP  = p - a;
        const float cross = edge.x() * toP.y() - edge.y() * toP.x();
        if (cross > 1e-4f) {
            if (sign < 0) return false;
            sign = 1;
        } else if (cross < -1e-4f) {
            if (sign > 0) return false;
            sign = -1;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
void FreeTransformTool::activate(ToolContext &ctx)
{
    if (!ctx.doc || ctx.doc->layers.isEmpty()) { engaged_ = false; return; }
    // 単色レイヤーは自由変形の対象にできない(実ピクセルデータを持たないため)
    if (ctx.doc->activeLayer().layerType != LayerType::Normal) { engaged_ = false; return; }
    // キャンバスのループが有効な場合、自由変形(4隅の非線形なシアー変形)は
    // ラップ時の周回定義が難しいため使用不可にする(Transformツールの平行移動・
    // 拡大縮小・回転のみラップ対応)
    if (ctx.wrapX || ctx.wrapY) { engaged_ = false; return; }

    int minX = ctx.canvasW, maxX = -1, minY = ctx.canvasH, maxY = -1;
    const bool hasSel = ctx.getHasSelection && ctx.getHasSelection();

    if (hasSel) {
        QVector<uint8_t> buf(ctx.canvasW * ctx.canvasH);
        glBindTexture(GL_TEXTURE_2D, ctx.selectionMaskTex);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, buf.data());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, 0);
        for (int y = 0; y < ctx.canvasH; y++) {
            for (int x = 0; x < ctx.canvasW; x++) {
                if (buf[y * ctx.canvasW + x] == 0) continue;
                minX = qMin(minX, x); maxX = qMax(maxX, x);
                minY = qMin(minY, y); maxY = qMax(maxY, y);
            }
        }
    }
    if (maxX < minX || maxY < minY) {
        // 選択なし、または空の選択 → レイヤーの「透明でない部分」の実際のbboxを対象にする
        const Layer &layer = ctx.doc->activeLayer();
        int cMinX, cMinY, cMaxX, cMaxY;
        if (computeLayerContentBBox(ctx, layer, cMinX, cMinY, cMaxX, cMaxY)) {
            minX = cMinX; minY = cMinY; maxX = cMaxX; maxY = cMaxY;
        } else {
            // 完全に透明な(空の)レイヤー → キャンバス全体を対象にする
            minX = 0; minY = 0; maxX = ctx.canvasW - 1; maxY = ctx.canvasH - 1;
        }
    }

    c0_       = QVector2D((minX + maxX + 1) / 2.0f, (minY + maxY + 1) / 2.0f);
    halfSize_ = QVector2D((maxX - minX + 1) / 2.0f, (maxY - minY + 1) / 2.0f);
    resetToIdentity();
    engaged_ = true;
}

void FreeTransformTool::deactivate()
{
    engaged_  = false;
    dragMode_ = DragMode::None;
}

void FreeTransformTool::resetToIdentity()
{
    corners_[0] = c0_ + QVector2D(-halfSize_.x(), -halfSize_.y()); // TL
    corners_[1] = c0_ + QVector2D( halfSize_.x(), -halfSize_.y()); // TR
    corners_[2] = c0_ + QVector2D( halfSize_.x(),  halfSize_.y()); // BR
    corners_[3] = c0_ + QVector2D(-halfSize_.x(),  halfSize_.y()); // BL
    dragMode_     = DragMode::None;
    activeCorner_ = -1;
}

// ---------------------------------------------------------------------------
FreeTransformTool::ButtonRects FreeTransformTool::buttonRects(const ToolContext &ctx) const
{
    if (!ctx.pixelToWidget) return {};

    QPointF w[4];
    for (int i = 0; i < 4; i++) w[i] = ctx.pixelToWidget(corners_[i]);

    const double bottom  = std::max({ w[0].y(), w[1].y(), w[2].y(), w[3].y() });
    const double centerX = (w[0].x() + w[1].x() + w[2].x() + w[3].x()) / 4.0;

    const double btnW = 76, btnH = 30, gap = 8, marginTop = 14;
    const double top = bottom + marginTop;

    ButtonRects r;
    r.confirm = QRectF(centerX - btnW - gap / 2.0, top, btnW, btnH);
    r.cancel  = QRectF(centerX + gap / 2.0,         top, btnW, btnH);
    return r;
}

// ---------------------------------------------------------------------------
void FreeTransformTool::paintOverlay(QPainter &painter, const ToolContext &ctx) const
{
    if (!engaged_ || !ctx.pixelToWidget) return;

    QPointF w[4];
    for (int i = 0; i < 4; i++) w[i] = ctx.pixelToWidget(corners_[i]);

    painter.setRenderHint(QPainter::Antialiasing);

    QPolygonF poly;
    for (int i = 0; i < 4; i++) poly << w[i];
    QPen boxPen(QColor(90, 220, 140, 230));
    boxPen.setWidthF(1.5);
    painter.setPen(boxPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(poly);

    painter.setPen(QPen(QColor(90, 220, 140, 230), 1.2));
    painter.setBrush(QColor(255, 255, 255, 230));
    for (int i = 0; i < 4; i++)
        painter.drawRect(QRectF(w[i].x() - 4, w[i].y() - 4, 8, 8));

    ButtonRects br = buttonRects(ctx);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(60, 140, 255, 235));
    painter.drawRoundedRect(br.confirm, 4, 4);
    painter.setBrush(QColor(90, 90, 90, 235));
    painter.drawRoundedRect(br.cancel, 4, 4);

    painter.setPen(QColor(255, 255, 255));
    QFont f = painter.font();
    f.setPixelSize(13);
    painter.setFont(f);
    painter.drawText(br.confirm, Qt::AlignCenter, QStringLiteral("確定"));
    painter.drawText(br.cancel,  Qt::AlignCenter, QStringLiteral("キャンセル"));
}

// ---------------------------------------------------------------------------
void FreeTransformTool::onMousePress(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    if (!engaged_) return;

    const QPointF widgetPos = event->position();

    ButtonRects br = buttonRects(ctx);
    if (br.confirm.contains(widgetPos)) { confirm(ctx); return; }
    if (br.cancel.contains(widgetPos))  { deactivate(); ctx.requestRepaint(); return; }

    // 頂点当たり判定(ウィジェット空間で一定px半径)
    const float hitRadiusWidget = 12.0f;
    for (int i = 0; i < 4; i++) {
        QPointF cWidget = ctx.pixelToWidget(corners_[i]);
        if (QLineF(cWidget, widgetPos).length() <= hitRadiusWidget) {
            dragMode_     = DragMode::Corner;
            activeCorner_ = i;
            return;
        }
    }

    const QVector2D mouseCanvas = ctx.widgetToPixel(widgetPos);

    // 頂点以外: 四角形の内側ならMove、外側ならRotate
    if (pointInQuad(mouseCanvas, corners_)) {
        dragMode_ = DragMode::Move;
    } else {
        dragMode_     = DragMode::Rotate;
        rotateCenter_ = centroid();
    }
    lastMouseCanvas_ = mouseCanvas;
}

void FreeTransformTool::onMouseMove(QMouseEvent *event, ToolContext &ctx)
{
    if (dragMode_ == DragMode::None) return;
    const QVector2D mouseCanvas = ctx.widgetToPixel(event->position());

    if (dragMode_ == DragMode::Corner) {
        corners_[activeCorner_] = mouseCanvas;
    } else if (dragMode_ == DragMode::Move) {
        const QVector2D delta = mouseCanvas - lastMouseCanvas_;
        for (auto &c : corners_) c += delta;
        lastMouseCanvas_ = mouseCanvas;
    } else if (dragMode_ == DragMode::Rotate) {
        QVector2D d0 = lastMouseCanvas_ - rotateCenter_;
        QVector2D d1 = mouseCanvas - rotateCenter_;
        const float a0 = std::atan2(d0.y(), d0.x());
        const float a1 = std::atan2(d1.y(), d1.x());
        const float cosT = std::cos(a1 - a0), sinT = std::sin(a1 - a0);
        for (auto &c : corners_) {
            QVector2D v = c - rotateCenter_;
            c = rotateCenter_ + QVector2D(v.x() * cosT - v.y() * sinT, v.x() * sinT + v.y() * cosT);
        }
        lastMouseCanvas_ = mouseCanvas;
    }

    ctx.requestRepaint();
}

void FreeTransformTool::onMouseRelease(QMouseEvent *event, ToolContext &ctx)
{
    Q_UNUSED(event); Q_UNUSED(ctx);
    dragMode_     = DragMode::None;
    activeCorner_ = -1;
}

// ---------------------------------------------------------------------------
void FreeTransformTool::confirm(ToolContext &ctx)
{
    if (!engaged_) return;
    applyTransform(ctx); // 恒等変形なら内部で何もしない
    deactivate();        // アクション終了(枠を消す)
    ctx.requestRepaint();
}

void FreeTransformTool::applyTransform(ToolContext &ctx)
{
    if (!engaged_) return;

    // どの頂点も動いていないなら何もしない
    const QVector2D idTL = c0_ + QVector2D(-halfSize_.x(), -halfSize_.y());
    const QVector2D idTR = c0_ + QVector2D( halfSize_.x(), -halfSize_.y());
    const QVector2D idBR = c0_ + QVector2D( halfSize_.x(),  halfSize_.y());
    const QVector2D idBL = c0_ + QVector2D(-halfSize_.x(),  halfSize_.y());
    const bool identity = (corners_[0] - idTL).lengthSquared() < 0.01f
                        && (corners_[1] - idTR).lengthSquared() < 0.01f
                        && (corners_[2] - idBR).lengthSquared() < 0.01f
                        && (corners_[3] - idBL).lengthSquared() < 0.01f;
    if (identity) return;

    const int tileSize = ctx.tileSize;
    const int activeLayerIndex = ctx.doc->activeLayerIndex();
    const GLuint defaultFbo = ctx.defaultFbo();

    // 0. 変形後の内容(4頂点のAABB)がキャンバス外へはみ出す場合に備え、
    //    そのぶんレイヤーの矩形を拡張しておく(既存矩形との和集合)。
    {
        float minX = corners_[0].x(), maxX = corners_[0].x();
        float minY = corners_[0].y(), maxY = corners_[0].y();
        for (int i = 1; i < 4; i++) {
            minX = qMin(minX, corners_[i].x()); maxX = qMax(maxX, corners_[i].x());
            minY = qMin(minY, corners_[i].y()); maxY = qMax(maxY, corners_[i].y());
        }
        const int minTx = (int)std::floor(minX / tileSize);
        const int minTy = (int)std::floor(minY / tileSize);
        const int maxTxEx = (int)std::ceil(maxX / tileSize);
        const int maxTyEx = (int)std::ceil(maxY / tileSize);
        if (ctx.growLayerBounds)
            ctx.growLayerBounds(activeLayerIndex, minTx, minTy, maxTxEx, maxTyEx);
    }

    const Layer &layer = ctx.doc->activeLayer(); // grow後の(拡張されているかもしれない)矩形
    const int layerW = layer.tilesX(), layerH = layer.tilesY();
    const int texW = layerW * tileSize, texH = layerH * tileSize;
    if (ctx.ensureTransformScratchSize) ctx.ensureTransformScratchSize(texW, texH);

    ctx.beginStrokeUndo();
    ctx.expandStrokeUndoRegion(layer.originTx, layer.originTx + layerW - 1,
                                layer.originTy, layer.originTy + layerH - 1);

    // 1. アクティブレイヤーの全タイル(レイヤー自身のローカル座標系)をfullLayerTexへ展開する
    {
        GLuint srcFbo = 0, dstFbo = 0;
        glGenFramebuffers(1, &srcFbo);
        glGenFramebuffers(1, &dstFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.fullLayerTex, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        for (int ty = 0; ty < layerH; ty++)
            for (int tx = 0; tx < layerW; tx++) {
                int si = layer.tiles[ty][tx];
                int dstX = tx * tileSize, dstY = ty * tileSize;
                glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                glBlitFramebuffer(0, 0, tileSize, tileSize, dstX, dstY, dstX + tileSize, dstY + tileSize, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &srcFbo);
        glDeleteFramebuffers(1, &dstFbo);
    }

    // 2. 変形元として読むためのスナップショットを撮る
    //    (fullLayerTex/selectionMaskTexそのものはこれから書き込み先になるため、
    //    読み込み元と書き込み先を別テクスチャに分離する必要がある)
    //    selectionMaskTex/transformSrcSelMaskTexはキャンバスサイズ固定。
    glCopyImageSubData(ctx.fullLayerTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                        ctx.transformSrcTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                        texW, texH, 1);
    glCopyImageSubData(ctx.selectionMaskTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                        ctx.transformSrcSelMaskTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                        ctx.canvasW, ctx.canvasH, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    // 3. freeTransform.compで fullLayerTex/selectionMaskTex に焼き込む
    const bool hasSel = ctx.getHasSelection && ctx.getHasSelection();

    glBindImageTexture(0, ctx.transformSrcTex,        0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
    glBindImageTexture(1, ctx.transformSrcSelMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);
    glBindImageTexture(2, ctx.fullLayerTex,           0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(3, ctx.selectionMaskTex,       0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);

    ctx.computeFreeTransformProgram->bind();
    ctx.computeFreeTransformProgram->setUniformValue("uP0", corners_[0]);
    ctx.computeFreeTransformProgram->setUniformValue("uP1", corners_[1]);
    ctx.computeFreeTransformProgram->setUniformValue("uP2", corners_[2]);
    ctx.computeFreeTransformProgram->setUniformValue("uP3", corners_[3]);
    ctx.computeFreeTransformProgram->setUniformValue("uC0",           c0_);
    ctx.computeFreeTransformProgram->setUniformValue("uHalfSize",     halfSize_);
    ctx.computeFreeTransformProgram->setUniformValue("uHasSelection", hasSel ? 1 : 0);
    {
        GLint loc = ctx.computeFreeTransformProgram->uniformLocation("uTexOriginPx");
        if (loc >= 0) ctx.gl->glUniform2i(loc, layer.originTx * tileSize, layer.originTy * tileSize);
    }
    glDispatchCompute((texW + 15) / 16, (texH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    ctx.computeFreeTransformProgram->release();

    // 4. fullLayerTex -> タイルに書き戻す
    {
        GLuint srcFbo = 0, dstFbo = 0;
        glGenFramebuffers(1, &srcFbo);
        glGenFramebuffers(1, &dstFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.fullLayerTex, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        for (int ty = 0; ty < layerH; ty++)
            for (int tx = 0; tx < layerW; tx++) {
                int si = layer.tiles[ty][tx];
                int srcX = tx * tileSize, srcY = ty * tileSize;
                glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                glBlitFramebuffer(srcX, srcY, srcX + tileSize, srcY + tileSize, 0, 0, tileSize, tileSize, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &srcFbo);
        glDeleteFramebuffers(1, &dstFbo);
    }

    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    if (hasSel && ctx.setHasSelection) ctx.setHasSelection(true); // マスクは移動済みなので有効のまま
    // 選択マスクをfreeTransform.compが動かしたので、点線(マーチングアンツ)の
    // 輪郭キャッシュも作り直させる(TransformTool::confirmの同じ箇所のコメント参照)。
    if (hasSel && ctx.invalidateSelectionOutline) ctx.invalidateSelectionOutline();

    ctx.commitStrokeUndo();
    ctx.requestRepaint();
    ctx.notifyLayersChanged();
}
