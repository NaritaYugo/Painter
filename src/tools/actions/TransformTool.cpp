#include "tools/actions/TransformTool.h"

#include <QPainter>
#include <QLineF>
#include <QFont>
#include <cmath>
#include <algorithm>
#include <climits>

void TransformTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

// ---------------------------------------------------------------------------
QVector2D TransformTool::localToCanvas(const QVector2D &local) const
{
    const float cosT = std::cos(rotation_), sinT = std::sin(rotation_);
    QVector2D s(local.x() * scale_.x(), local.y() * scale_.y());
    QVector2D r(s.x() * cosT - s.y() * sinT, s.x() * sinT + s.y() * cosT);
    return pivot_ + r;
}

QVector2D TransformTool::canvasToLocal(const QVector2D &canvasPos) const
{
    const float cosT = std::cos(rotation_), sinT = std::sin(rotation_);
    QVector2D d = canvasPos - pivot_;
    QVector2D r(d.x() * cosT + d.y() * sinT, -d.x() * sinT + d.y() * cosT); // R^-1 * d
    return QVector2D(r.x() / scale_.x(), r.y() / scale_.y());
}

QVector2D TransformTool::handleLocal(int index, const QVector2D &halfSize)
{
    const float hw = halfSize.x(), hh = halfSize.y();
    switch (index) {
    case 0: return QVector2D(-hw, -hh); // TL
    case 1: return QVector2D(0,   -hh); // TM
    case 2: return QVector2D(hw,  -hh); // TR
    case 3: return QVector2D(-hw, 0);   // ML
    case 4: return QVector2D(hw,  0);   // MR
    case 5: return QVector2D(-hw, hh);  // BL
    case 6: return QVector2D(0,   hh);  // BM
    default: return QVector2D(hw, hh);  // BR (7)
    }
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
void TransformTool::activate(ToolContext &ctx)
{
    if (!ctx.doc || ctx.doc->layers.isEmpty()) { engaged_ = false; return; }
    // 単色レイヤーは移動変形の対象にできない(実ピクセルデータを持たないため)
    if (ctx.doc->activeLayer().layerType != LayerType::Normal) { engaged_ = false; return; }

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
        if (ctx.wrapX || ctx.wrapY) {
            // ループが有効な場合、選択範囲が無ければ常にキャンバス全体を対象にする
            // (周回はキャンバス全体を基準に定義されるため、レイヤーの実際の描画範囲
            // だけを対象にすると、ループの基準サイズが操作のたびに変わってしまう)
            minX = 0; minY = 0; maxX = ctx.canvasW - 1; maxY = ctx.canvasH - 1;
        } else {
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
    }

    c0_       = QVector2D((minX + maxX + 1) / 2.0f, (minY + maxY + 1) / 2.0f);
    halfSize_ = QVector2D((maxX - minX + 1) / 2.0f, (maxY - minY + 1) / 2.0f);
    resetToIdentity();
    engaged_ = true;
}

void TransformTool::deactivate()
{
    engaged_  = false;
    dragMode_ = DragMode::None;
}

void TransformTool::resetToIdentity()
{
    pivot_        = c0_;
    scale_        = QVector2D(1.0f, 1.0f);
    rotation_     = 0.0f;
    dragMode_     = DragMode::None;
    activeHandle_ = -1;
}

// ---------------------------------------------------------------------------
TransformTool::ButtonRects TransformTool::buttonRects(const ToolContext &ctx) const
{
    if (!ctx.pixelToWidget) return {};

    QPointF tl = ctx.pixelToWidget(localToCanvas(handleLocal(0, halfSize_)));
    QPointF tr = ctx.pixelToWidget(localToCanvas(handleLocal(2, halfSize_)));
    QPointF br = ctx.pixelToWidget(localToCanvas(handleLocal(7, halfSize_)));
    QPointF bl = ctx.pixelToWidget(localToCanvas(handleLocal(5, halfSize_)));

    const double bottom  = std::max({ tl.y(), tr.y(), br.y(), bl.y() });
    const double centerX = (tl.x() + tr.x() + br.x() + bl.x()) / 4.0;

    const double btnW = 76, btnH = 30, gap = 8, marginTop = 14;
    const double top = bottom + marginTop;

    ButtonRects r;
    r.confirm = QRectF(centerX - btnW - gap / 2.0, top, btnW, btnH);
    r.cancel  = QRectF(centerX + gap / 2.0,         top, btnW, btnH);
    return r;
}

// ---------------------------------------------------------------------------
void TransformTool::paintOverlay(QPainter &painter, const ToolContext &ctx) const
{
    if (!engaged_ || !ctx.pixelToWidget) return;

    QPointF tl = ctx.pixelToWidget(localToCanvas(handleLocal(0, halfSize_)));
    QPointF tr = ctx.pixelToWidget(localToCanvas(handleLocal(2, halfSize_)));
    QPointF br = ctx.pixelToWidget(localToCanvas(handleLocal(7, halfSize_)));
    QPointF bl = ctx.pixelToWidget(localToCanvas(handleLocal(5, halfSize_)));

    painter.setRenderHint(QPainter::Antialiasing);

    QPolygonF poly;
    poly << tl << tr << br << bl;
    QPen boxPen(QColor(80, 180, 255, 230));
    boxPen.setWidthF(1.5);
    painter.setPen(boxPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(poly);

    painter.setPen(QPen(QColor(80, 180, 255, 230), 1.2));
    painter.setBrush(QColor(255, 255, 255, 230));
    for (int i = 0; i < 8; i++) {
        QPointF p = ctx.pixelToWidget(localToCanvas(handleLocal(i, halfSize_)));
        painter.drawRect(QRectF(p.x() - 4, p.y() - 4, 8, 8));
    }

    ButtonRects br2 = buttonRects(ctx);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(60, 140, 255, 235));
    painter.drawRoundedRect(br2.confirm, 4, 4);
    painter.setBrush(QColor(90, 90, 90, 235));
    painter.drawRoundedRect(br2.cancel, 4, 4);

    painter.setPen(QColor(255, 255, 255));
    QFont f = painter.font();
    f.setPixelSize(13);
    painter.setFont(f);
    painter.drawText(br2.confirm, Qt::AlignCenter, QStringLiteral("確定"));
    painter.drawText(br2.cancel,  Qt::AlignCenter, QStringLiteral("キャンセル"));
}

// ---------------------------------------------------------------------------
void TransformTool::onMousePress(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    if (!engaged_) return;

    const QPointF widgetPos = event->position();

    ButtonRects br = buttonRects(ctx);
    if (br.confirm.contains(widgetPos)) { confirm(ctx); return; }
    if (br.cancel.contains(widgetPos))  { deactivate(); ctx.requestRepaint(); return; }

    const QVector2D mouseCanvas = ctx.widgetToPixel(widgetPos);

    // ハンドル当たり判定(ウィジェット空間で一定px半径)
    const float hitRadiusWidget = 12.0f;
    for (int i = 0; i < 8; i++) {
        QVector2D hLocal  = handleLocal(i, halfSize_);
        QVector2D hCanvas = localToCanvas(hLocal);
        QPointF   hWidget = ctx.pixelToWidget(hCanvas);
        const double dist = QLineF(hWidget, widgetPos).length();
        if (dist <= hitRadiusWidget) {
            dragMode_      = DragMode::Scale;
            activeHandle_  = i;
            const int anchorIdx = 7 - i;
            dragAnchorLocal_   = handleLocal(anchorIdx, halfSize_);
            dragHandleLocal_   = hLocal;
            dragAnchorCanvas_  = localToCanvas(dragAnchorLocal_);
            dragStartScale_    = scale_;
            dragRotationFixed_ = rotation_;
            return;
        }
    }

    // ハンドル以外: bboxの内側ならMove、外側ならRotate
    QVector2D local = canvasToLocal(mouseCanvas);
    const bool inside = qAbs(local.x()) <= halfSize_.x() && qAbs(local.y()) <= halfSize_.y();
    dragMode_ = inside ? DragMode::Move : DragMode::Rotate;
    lastMouseCanvas_ = mouseCanvas;
}

void TransformTool::onMouseMove(QMouseEvent *event, ToolContext &ctx)
{
    if (dragMode_ == DragMode::None) return;
    const QVector2D mouseCanvas = ctx.widgetToPixel(event->position());

    if (dragMode_ == DragMode::Move) {
        pivot_ += (mouseCanvas - lastMouseCanvas_);
        lastMouseCanvas_ = mouseCanvas;
    } else if (dragMode_ == DragMode::Rotate) {
        QVector2D d0 = lastMouseCanvas_ - pivot_;
        QVector2D d1 = mouseCanvas - pivot_;
        const float a0 = std::atan2(d0.y(), d0.x());
        const float a1 = std::atan2(d1.y(), d1.x());
        rotation_ += (a1 - a0);
        lastMouseCanvas_ = mouseCanvas;
    } else if (dragMode_ == DragMode::Scale) {
        const float cosT = std::cos(dragRotationFixed_), sinT = std::sin(dragRotationFixed_);
        QVector2D delta = mouseCanvas - dragAnchorCanvas_;
        // 固定した回転を除去し、ローカル方向のベクトルに戻す(R^-1 * delta)
        QVector2D v(delta.x() * cosT + delta.y() * sinT, -delta.x() * sinT + delta.y() * cosT);
        QVector2D d = dragHandleLocal_ - dragAnchorLocal_;

        QVector2D newScale = dragStartScale_;
        const float MIN_SCALE = 0.02f;
        if (!qFuzzyIsNull(d.x())) {
            float sx = v.x() / d.x();
            newScale.setX(qAbs(sx) < MIN_SCALE ? (sx < 0 ? -MIN_SCALE : MIN_SCALE) : sx);
        }
        if (!qFuzzyIsNull(d.y())) {
            float sy = v.y() / d.y();
            newScale.setY(qAbs(sy) < MIN_SCALE ? (sy < 0 ? -MIN_SCALE : MIN_SCALE) : sy);
        }

        scale_    = newScale;
        rotation_ = dragRotationFixed_;

        // アンカー(反対側のハンドル)を画面上で固定したまま中心を求め直す:
        // pivot' = anchorCanvas - R * (scale' ⊙ anchorLocal)
        QVector2D sAnchor(scale_.x() * dragAnchorLocal_.x(), scale_.y() * dragAnchorLocal_.y());
        QVector2D rotated(sAnchor.x() * cosT - sAnchor.y() * sinT,
                           sAnchor.x() * sinT + sAnchor.y() * cosT);
        pivot_ = dragAnchorCanvas_ - rotated;
    }

    ctx.requestRepaint();
}

void TransformTool::onMouseRelease(QMouseEvent *event, ToolContext &ctx)
{
    Q_UNUSED(event); Q_UNUSED(ctx);
    dragMode_     = DragMode::None;
    activeHandle_ = -1;
}

// ---------------------------------------------------------------------------
void TransformTool::confirm(ToolContext &ctx)
{
    if (!engaged_) return;
    applyTransform(ctx); // 恒等変形なら内部で何もしない
    deactivate();        // アクション終了(枠を消す)
    ctx.requestRepaint();
}

void TransformTool::applyTransform(ToolContext &ctx)
{
    if (!engaged_) return;

    // 移動も拡縮も回転もしていないなら何もしない
    const bool identity = qFuzzyCompare(scale_.x(), 1.0f) && qFuzzyCompare(scale_.y(), 1.0f)
                        && qFuzzyIsNull(rotation_) && (pivot_ - c0_).lengthSquared() < 0.01f;
    if (identity) return;

    const int tileSize = ctx.tileSize;
    const int activeLayerIndex = ctx.doc->activeLayerIndex();
    const GLuint defaultFbo = ctx.defaultFbo();

    // 0. 変形後の内容がキャンバス外へはみ出す場合に備え、そのぶんレイヤーの矩形を
    //    拡張しておく(既存矩形との和集合。内容を失わないための措置)。
    //    対象の4隅(TL,TR,BL,BR)をキャンバスpx座標で求めてAABBを取る。
    {
        QVector2D corners[4] = {
            localToCanvas(handleLocal(0, halfSize_)), // TL
            localToCanvas(handleLocal(2, halfSize_)), // TR
            localToCanvas(handleLocal(5, halfSize_)), // BL
            localToCanvas(handleLocal(7, halfSize_)), // BR
        };
        float minX = corners[0].x(), maxX = corners[0].x();
        float minY = corners[0].y(), maxY = corners[0].y();
        for (int i = 1; i < 4; i++) {
            minX = qMin(minX, corners[i].x()); maxX = qMax(maxX, corners[i].x());
            minY = qMin(minY, corners[i].y()); maxY = qMax(maxY, corners[i].y());
        }
        int minTx = (int)std::floor(minX / tileSize);
        int minTy = (int)std::floor(minY / tileSize);
        int maxTxEx = (int)std::ceil(maxX / tileSize);
        int maxTyEx = (int)std::ceil(maxY / tileSize);

        // ラップ有効な軸は、内容がキャンバス内で周回するだけでキャンバス外へは
        // 実際にはみ出さないため、その軸は現在のレイヤー矩形を超えて拡張しない
        // (=growLayerBoundsへ渡す範囲を現在の矩形内に収める)。
        const Layer &curLayer = ctx.doc->activeLayer();
        if (ctx.wrapX) {
            minTx   = qMax(minTx,   curLayer.originTx);
            maxTxEx = qMin(maxTxEx, curLayer.originTx + curLayer.tilesX());
        }
        if (ctx.wrapY) {
            minTy   = qMax(minTy,   curLayer.originTy);
            maxTyEx = qMin(maxTyEx, curLayer.originTy + curLayer.tilesY());
        }

        if (ctx.growLayerBounds)
            ctx.growLayerBounds(activeLayerIndex, minTx, minTy, maxTxEx, maxTyEx);
    }

    const Layer &layer = ctx.doc->activeLayer(); // grow後の(拡張されているかもしれない)矩形
    const int layerW = layer.tilesX(), layerH = layer.tilesY();
    const int texW = layerW * tileSize, texH = layerH * tileSize;
    if (ctx.ensureTransformScratchSize) ctx.ensureTransformScratchSize(texW, texH);

    ctx.beginStrokeUndo();
    // レイヤー全体(grow後の矩形全部)を「変更されうる範囲」としてUndoに記録する
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

    // 3. transform.compで fullLayerTex/selectionMaskTex に焼き込む
    const bool hasSel = ctx.getHasSelection && ctx.getHasSelection();
    const float cosT = std::cos(rotation_), sinT = std::sin(rotation_);

    glBindImageTexture(0, ctx.transformSrcTex,        0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
    glBindImageTexture(1, ctx.transformSrcSelMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);
    glBindImageTexture(2, ctx.fullLayerTex,           0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(3, ctx.selectionMaskTex,       0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);

    ctx.computeTransformProgram->bind();
    ctx.computeTransformProgram->setUniformValue("uPivot",        pivot_);
    ctx.computeTransformProgram->setUniformValue("uRotCosSin",    QVector2D(cosT, sinT));
    ctx.computeTransformProgram->setUniformValue("uScale",        scale_);
    ctx.computeTransformProgram->setUniformValue("uC0",           c0_);
    ctx.computeTransformProgram->setUniformValue("uHalfSize",     halfSize_);
    ctx.computeTransformProgram->setUniformValue("uHasSelection", hasSel ? 1 : 0);
    ctx.computeTransformProgram->setUniformValue("uWrapX", ctx.wrapX ? 1 : 0);
    ctx.computeTransformProgram->setUniformValue("uWrapY", ctx.wrapY ? 1 : 0);
    {
        GLint loc = ctx.computeTransformProgram->uniformLocation("uTexOriginPx");
        if (loc >= 0) ctx.gl->glUniform2i(loc, layer.originTx * tileSize, layer.originTy * tileSize);
    }
    glDispatchCompute((texW + 15) / 16, (texH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    ctx.computeTransformProgram->release();

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
    // 選択マスクをtransform.compが動かしたので、そこから作ってある点線
    // (マーチングアンツ)の輪郭キャッシュも作り直させる。これを忘れると、
    // 選択範囲は移動しているのに点線だけ元の位置に残り続ける。
    if (hasSel && ctx.invalidateSelectionOutline) ctx.invalidateSelectionOutline();

    ctx.commitStrokeUndo();
    ctx.requestRepaint();
    ctx.notifyLayersChanged();
}
