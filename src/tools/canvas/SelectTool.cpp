#include "tools/canvas/SelectTool.h"
#include "tools/core/CursorUtils.h"
#include "tools/core/DispatchBounds.h"
#include "tools/core/BrushShape.h"

#include <QPainter>
#include <QPolygonF>
#include <algorithm>
#include <cmath>
#include <utility>

// QOpenGLShaderProgram::setUniformValue(name, QPoint)はこの環境ではivec2 uniformに
// 対して値が反映されないため(型解決の問題。PenEraserTool.cppの同名関数と同じ理由)、
// ivec2はglUniform2iで直接設定する。
static void setUniformIVec2(QOpenGLFunctions_4_3_Core *gl, QOpenGLShaderProgram *prog,
                             const char *name, int x, int y)
{
    GLint loc = gl->glGetUniformLocation(prog->programId(), name);
    if (loc >= 0)
        gl->glUniform2i(loc, x, y);
}

void SelectTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

std::optional<QCursor> SelectTool::cursor(const ToolContext &ctx) const
{
    if (!toolCfg_ || toolCfg_->selection().mode() != SelectionMode::PenSelect)
        return std::nullopt; // 投げ縄はアイコンにフォールバック

    // view->scale() はキャンバスpx→デバイスpx。カーソル画像は論理pxで作るので
    // viewDpr で割って画面上の見た目のサイズに合わせる(ToolContext::viewDpr参照)。
    float viewScale = (ctx.view ? ctx.view->scale() : 1.0f) / (ctx.viewDpr > 0.0f ? ctx.viewDpr : 1.0f);
    return CursorUtils::makeCircleCursor(toolCfg_->selection().size() * viewScale);
}

void SelectTool::paintOverlay(QPainter &painter, const ToolContext &ctx) const
{
    if (!dragging_ || !toolCfg_) return;
    if (toolCfg_->selection().mode() != SelectionMode::Lasso) return;
    if (lassoPoints_.size() < 2 || !ctx.pixelToWidget) return;

    QPolygonF poly;
    for (const QVector2D &p : lassoPoints_)
        poly << ctx.pixelToWidget(p);

    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);

    // 白一色の破線だと、キャンバスの背景が白いときに軌跡が全く見えなくなってしまう
    // ため、確定後のマーチングアンツ(paintSelectionOutline)と同じく白黒交互の破線
    // (半周期ずらした2本を重ね描き)にして、どんな背景色でも視認できるようにする。
    QPen whitePen(QColor(255, 255, 255, 230));
    whitePen.setWidthF(1.2);
    whitePen.setStyle(Qt::DashLine);
    painter.setPen(whitePen);
    painter.drawPolyline(poly);

    QPen blackPen(QColor(0, 0, 0, 230));
    blackPen.setWidthF(1.2);
    blackPen.setStyle(Qt::DashLine);
    blackPen.setDashOffset(4.0);
    painter.setPen(blackPen);
    painter.drawPolyline(poly);

    // 離した時に始点と結ばれることが分かるよう、閉じる辺も同様に白黒交互の破線で
    // 薄く見せておく
    QPen closeWhitePen(QColor(255, 255, 255, 120));
    closeWhitePen.setWidthF(1.0);
    closeWhitePen.setStyle(Qt::DotLine);
    painter.setPen(closeWhitePen);
    painter.drawLine(poly.last(), poly.first());

    QPen closeBlackPen(QColor(0, 0, 0, 120));
    closeBlackPen.setWidthF(1.0);
    closeBlackPen.setStyle(Qt::DotLine);
    closeBlackPen.setDashOffset(2.0);
    painter.setPen(closeBlackPen);
    painter.drawLine(poly.last(), poly.first());
}

void SelectTool::onMousePress(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    if (ctx.doc->layers.isEmpty()) return;

    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool lasso = (toolCfg_->selection().mode() == SelectionMode::Lasso);

    // Ctrl+Shift+クリックは「選択解除」(Escと同じ)。ドラッグは開始しない。
    // 投げ縄/ペン選択どちらでも同じ。
    if ((mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        dragging_ = false;
        lassoPoints_.clear();
        if (ctx.clearSelection) ctx.clearSelection(); // Undo記録はclearSelection側で行う
        return;
    }

    // 合成方法を修飾キーから決める(SelectTool.hのCombineModeコメント参照)
    if (mods & Qt::AltModifier)
        combine_ = CombineMode::Subtract;
    else if (!lasso)
        combine_ = CombineMode::Union;   // ペン選択は既定で追加(Ctrl不要)
    else
        combine_ = (mods & Qt::ControlModifier) ? CombineMode::Union : CombineMode::Replace;

    // 選択範囲が変わる操作の開始。実際に変化しなければcommit側で何も積まれない。
    if (ctx.beginSelectionUndo) ctx.beginSelectionUndo();

    // 【軽量化】ドラッグ中は毎フレーム再描画が走るが、選択操作はレイヤーの中身を
    // 一切変えない。にもかかわらず、これまでは render.frag が毎フレーム全レイヤーを
    // 合成し直していた(ペン描画には入れてある事前合成キャッシュが選択ツールには
    // 入っていなかった)。レイヤー枚数がそのまま1フレームのコストに乗るので、
    // 「投げ縄で囲っている間が重い」「ペン選択が普通の描画より相当重い」の主因。
    //
    // アクティブより下/上を1枚ずつのキャッシュへ事前合成しておけば、ドラッグ中の
    // 合成コストはレイヤー枚数にほぼ依存しなくなる。選択操作中はどのレイヤーの中身も
    // 変わらないのでキャッシュは常に正しい(キャッシュはレイヤー構成の変更や
    // Undo/Redoで自動的に無効化される)。
    if (ctx.updateBelowCompositeCache) ctx.updateBelowCompositeCache(ctx.doc->activeLayerIndex());
    if (ctx.updateAboveCompositeCache) ctx.updateAboveCompositeCache(ctx.doc->activeLayerIndex());

    dragging_ = true;

    if (lasso) {
        lassoPoints_.clear();
        lassoPoints_.append(ctx.widgetToPixel(event->position()));
        ctx.requestRepaint();
        return;
    }

    // ---- ペン選択: 新しい選択のため、作業用ストロークマスクをクリアしてから使う ----
    lastPos_ = ctx.widgetToPixel(event->position());

    glBindImageTexture(0, ctx.maskTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    ctx.computeMaskClearProgram->bind();
    // uDispatchOriginはAirbrushToolが最後にこのプログラムを使った際の非ゼロ値が
    // 残っていることがあるため、キャンバス全域を対象とするここでは明示的に(0,0)へ戻す
    // (PenEraserTool.cppの同種の修正と同じ理由)。
    setUniformIVec2(ctx.gl, ctx.computeMaskClearProgram, "uDispatchOrigin", 0, 0);
    glDispatchCompute((ctx.canvasW + 15) / 16, (ctx.canvasH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    ctx.computeMaskClearProgram->release();
    glBindImageTexture(0, ctx.maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);

    const float radius   = toolCfg_->selection().size() / 2.0f;
    const float hardness = toolCfg_->selection().hardness();
    ctx.computeDrawProgram->bind();
    ctx.computeDrawProgram->setUniformValue("uRadius", radius);
    ctx.computeDrawProgram->setUniformValue("uHardness", hardness);
    dispatchStrokeSegment(ctx, lastPos_, lastPos_, radius);
    ctx.computeDrawProgram->release();

    ctx.requestRepaint();
}

void SelectTool::onMouseMove(QMouseEvent *event, ToolContext &ctx)
{
    if (!dragging_) return;

    if (toolCfg_->selection().mode() == SelectionMode::Lasso) {
        QVector2D p = ctx.widgetToPixel(event->position());
        // 点が多くなりすぎないよう、一定距離動いた時だけ追加する
        if (lassoPoints_.isEmpty() || (p - lassoPoints_.last()).length() > 3.0f)
            lassoPoints_.append(p);
        ctx.requestRepaint();
        return;
    }

    QVector2D p = ctx.widgetToPixel(event->position());
    const float radius   = toolCfg_->selection().size() / 2.0f;
    const float hardness = toolCfg_->selection().hardness();
    ctx.computeDrawProgram->bind();
    ctx.computeDrawProgram->setUniformValue("uRadius", radius);
    ctx.computeDrawProgram->setUniformValue("uHardness", hardness);
    dispatchStrokeSegment(ctx, lastPos_, p, radius);
    ctx.computeDrawProgram->release();
    lastPos_ = p;

    // 【軽量化】ここでは再描画を要求しない。
    //
    // ペン選択のストロークはctx.maskTexへ溜まるだけで、画面には一切出ない
    // (render.fragのプレビュー分岐は uIsSelectionTool != 0 のとき丸ごとスキップされる)。
    // つまりドラッグ中はキャンバスの見た目が1ピクセルも変わらないのに、以前は
    // 入力イベントごとに全レイヤーの再合成が走っていた。完全に無駄なコストなので省く。
    // マーチングアンツは離した時(finishPenSelect)に一度描けばよい。
}

void SelectTool::onMouseRelease(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    if (!dragging_) return;
    dragging_ = false;

    if (toolCfg_->selection().mode() == SelectionMode::Lasso)
        finishLasso(ctx);
    else
        finishPenSelect(ctx);
}

void SelectTool::dispatchStrokeSegment(ToolContext &ctx, const QVector2D &from, const QVector2D &to,
                                        float radius)
{
    // PenEraserToolがこのプログラムをバッチスタンプモード(uUseStampBatch=1)で
    // 使った後にここへ来ることがあるため、線分モードへ明示的に戻す。
    ctx.computeDrawProgram->setUniformValue("uUseStampBatch", 0);
    ctx.computeDrawProgram->setUniformValue("uPosStart", from);
    ctx.computeDrawProgram->setUniformValue("uPosEnd",   to);

    // 【軽量化】ブラシが実際に届く範囲だけをディスパッチする。
    // 以前はマウス移動イベントごとにキャンバス全域をディスパッチしていて、
    // フルHDなら1イベントあたり8000ワークグループ超。これがペン選択が重い主因だった。
    // ペン/ぼかし/ゆがみが使っているのと同じ範囲計算をそのまま流用する
    // (選択マスクはタイル分割されていないので、タイル整列は不要でピクセルbboxが最小)。
    // 半径は「実際にアルファが乗る外周半径」を使う(柔らかいブラシは公称半径より
    // 外へ広がるため。BrushShape.h参照)。
    const StrokeDispatchBounds b = computeStrokeDispatchBounds(
        ctx.canvasW, ctx.canvasH, ctx.tileSize, ctx.doc->tilesX(), ctx.doc->tilesY(),
        from, to, BrushShape::outerRadius(radius, toolCfg_->selection().hardness()), 0);
    if (b.empty()) return;

    setUniformIVec2(ctx.gl, ctx.computeDrawProgram, "uDispatchOrigin", b.pxMinX, b.pxMinY);
    const int w = b.pxMaxX - b.pxMinX + 1;
    const int h = b.pxMaxY - b.pxMinY + 1;
    glDispatchCompute((w + 15) / 16, (h + 15) / 16, 1);

    // このバリアは省けない。stroke.compはマスクを読み書き両方する
    // (imageLoadした現在値と合成してimageStoreする)ため、区間が重なる部分では
    // 「前の区間の書き込みを次の区間が読む」依存があり、バリアを外すと重なり部分の
    // 塗りが抜け落ちる。
    //
    // なお、通常のペン描画がこのコストを避けているのはバリアを省いているからではなく、
    // 1フレーム分のスタンプをSSBOへ溜めて「1回のディスパッチ」にまとめているため
    // (PenEraserTool::dispatchStampBatch / uUseStampBatch=1)。区間ごとにディスパッチ+
    // バリアを繰り返す今の作りが、ペン選択が通常の描画より重い残りの要因。
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

// ---------------------------------------------------------------------------
// 偶奇規則によるポリゴンのスキャンライン塗りつぶし。
// キャンバス全域サイズのバッファを返す(0=選択外, 255=選択内)。
// ---------------------------------------------------------------------------
static QVector<uint8_t> rasterizePolygon(const QVector<QVector2D> &pts, int w, int h)
{
    QVector<uint8_t> buf(w * h, 0);
    if (pts.size() < 3) return buf;

    float minY = pts[0].y(), maxY = pts[0].y();
    for (const QVector2D &p : pts) {
        minY = qMin(minY, p.y());
        maxY = qMax(maxY, p.y());
    }
    const int y0 = qMax(0, (int)std::floor(minY));
    const int y1 = qMin(h - 1, (int)std::ceil(maxY));
    const int n  = pts.size();

    QVector<float> xs;
    for (int y = y0; y <= y1; y++) {
        const float sy = y + 0.5f;
        xs.clear();
        for (int i = 0; i < n; i++) {
            const QVector2D &a = pts[i];
            const QVector2D &b = pts[(i + 1) % n];
            if ((a.y() <= sy && b.y() > sy) || (b.y() <= sy && a.y() > sy)) {
                const float t = (sy - a.y()) / (b.y() - a.y());
                xs.append(a.x() + t * (b.x() - a.x()));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (int i = 0; i + 1 < xs.size(); i += 2) {
            const int xa = qBound(0, (int)std::ceil(xs[i] - 0.5f), w);
            const int xb = qBound(0, (int)std::floor(xs[i + 1] - 0.5f), w - 1);
            for (int x = xa; x <= xb; x++)
                buf[y * w + x] = 255;
        }
    }
    return buf;
}


// 今回描いた形(incoming)を既存の選択範囲と合成して確定する。
// 合成はCPUで行う(選択の確定はドラッグ1回につき1度しか起きないので、キャンバス全域の
// 読み戻し1回ぶんのコストは問題にならない。専用のcompute shaderを増やすより素直)。
void SelectTool::commitSelection(ToolContext &ctx, QVector<uint8_t> &&incoming)
{
    const int W = ctx.canvasW, H = ctx.canvasH;
    const int n = W * H;
    const bool hadSel = ctx.getHasSelection ? ctx.getHasSelection() : false;

    // 今回描いた形の外接矩形を求める。ここで矩形が分かれば、以降の読み戻し・合成・
    // アップロード・Undo記録を「実際に変わる範囲」だけに絞れる(全域を触ると
    // フルHDで数百msかかっていた ―― 実測でrelease=82〜271ms)。
    // 全ゼロ判定もこの1パスで同時に済ませる。
    int bx0 = W, bx1 = -1, by0 = H, by1 = -1;
    for (int y = 0; y < H; y++) {
        const uint8_t *row = incoming.constData() + (qsizetype)y * W;
        int rx0 = -1, rx1 = -1;
        for (int x = 0; x < W; x++) {
            if (row[x] != 0) { if (rx0 < 0) rx0 = x; rx1 = x; }
        }
        if (rx0 >= 0) {
            bx0 = qMin(bx0, rx0); bx1 = qMax(bx1, rx1);
            by0 = qMin(by0, y);   by1 = qMax(by1, y);
        }
    }

    // 【重要】何も描けていない(全ゼロ)なら選択範囲には一切触らない。
    // キャンバス外だけをドラッグした場合や、ごく短いタッチで1画素も塗られなかった
    // 場合がこれにあたる。下の「空なら選択解除」規則は本来「削減で削り切ったとき」用
    // なので、何も描いていないケースはその手前で弾く(これを通すと全域255へ戻され、
    // 見た目には「全選択された」ように見えてしまう)。
    if (bx1 < bx0) {
        ctx.requestRepaint();
        if (ctx.commitSelectionUndo) ctx.commitSelectionUndo({}, 0, 0, 0, 0); // 変化なし: 積まれない
        return;
    }

    // 「選択なし」はマスク全域255(=制限なし)で表現しているため、その状態での
    // 合成はそのままだと意味が変わってしまう。次のように読み替える:
    //   ・追加 … 「全部」に足しても全部のままなので、新規作成(置き換え)として扱う
    //   ・削減 … 「全部」から削ると予期しない反転選択になるので、何もしない
    CombineMode mode = combine_;
    if (!hadSel) {
        if (mode == CombineMode::Union) {
            mode = CombineMode::Replace;
        } else if (mode == CombineMode::Subtract) {
            ctx.requestRepaint();
            if (ctx.commitSelectionUndo) ctx.commitSelectionUndo({}, 0, 0, 0, 0);
            return;
        }
    }

    if (mode == CombineMode::Replace) {
        // 置き換えは矩形外も0にする必要があるので全域を書く(読み戻しは不要)。
        bool anySelected = (bx1 >= bx0);
        if (!anySelected) incoming.fill(255);
        uploadSelectionRect(ctx, incoming.constData(), 0, 0, W, H);
        if (ctx.invalidateSelectionOutline) ctx.invalidateSelectionOutline();
        if (ctx.setHasSelection) ctx.setHasSelection(anySelected);
        ctx.requestRepaint();
        if (ctx.commitSelectionUndo)
            ctx.commitSelectionUndo(QByteArray(reinterpret_cast<const char *>(incoming.constData()), n),
                                     0, 0, W, H);
        return;
    }

    // ---- 追加/削減: 変わるのは外接矩形の中だけ ----
    const int rw = bx1 - bx0 + 1, rh = by1 - by0 + 1;
    QVector<uint8_t> rect = readSelectionRect(ctx, bx0, by0, rw, rh);
    for (int y = 0; y < rh; y++) {
        uint8_t       *dst = rect.data() + (qsizetype)y * rw;
        const uint8_t *inc = incoming.constData() + (qsizetype)(by0 + y) * W + bx0;
        if (mode == CombineMode::Union) {
            for (int x = 0; x < rw; x++) dst[x] = qMax(dst[x], inc[x]);
        } else {
            // 差集合: 描いた濃さぶんだけ削る(アンチエイリアスされたペン選択でも自然に減る)
            for (int x = 0; x < rw; x++) dst[x] = qMin(dst[x], (uint8_t)(255 - inc[x]));
        }
    }

    // 削減の結果、選択が完全に空になったら「選択解除」として扱う(全域255へ戻す)。
    // 空のマスクを選択ありのまま残すと、以後どのツールも一切描けなくなってしまう。
    // 矩形の中が空になったときだけ、矩形の外に何か残っているかを全域で確かめる
    // (通常は矩形内に何か残るので、この全域スキャンはほとんど走らない)。
    bool anyInRect = false;
    for (int i = 0, m = rw * rh; i < m; i++) {
        if (rect[i] != 0) { anyInRect = true; break; }
    }
    bool anySelected = anyInRect;
    if (!anyInRect && mode == CombineMode::Subtract) {
        QVector<uint8_t> full = readSelectionRect(ctx, 0, 0, W, H);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const bool insideRect = (x >= bx0 && x <= bx1 && y >= by0 && y <= by1);
                if (!insideRect && full[(qsizetype)y * W + x] != 0) { anySelected = true; break; }
            }
            if (anySelected) break;
        }
    }

    if (!anySelected) {
        // 選択解除: 全域255へ戻す
        QVector<uint8_t> fullSel(n, 255);
        uploadSelectionRect(ctx, fullSel.constData(), 0, 0, W, H);
        if (ctx.invalidateSelectionOutline) ctx.invalidateSelectionOutline();
        if (ctx.setHasSelection) ctx.setHasSelection(false);
        ctx.requestRepaint();
        if (ctx.commitSelectionUndo)
            ctx.commitSelectionUndo(QByteArray(reinterpret_cast<const char *>(fullSel.constData()), n),
                                     0, 0, W, H);
        return;
    }

    uploadSelectionRect(ctx, rect.constData(), bx0, by0, rw, rh);
    if (ctx.invalidateSelectionOutline) ctx.invalidateSelectionOutline();
    if (ctx.setHasSelection) ctx.setHasSelection(true);
    ctx.requestRepaint();
    if (ctx.commitSelectionUndo)
        ctx.commitSelectionUndo(QByteArray(reinterpret_cast<const char *>(rect.constData()), rw * rh),
                                 bx0, by0, rw, rh);
}

// 選択マスクの部分矩形をCPUへ読み戻す。glGetTexImageは部分読み出しができないので
// (glGetTextureSubImageはGL4.5)、FBOへ付けてglReadPixelsで矩形だけ読む。
QVector<uint8_t> SelectTool::readSelectionRect(ToolContext &ctx, int x, int y, int w, int h)
{
    QVector<uint8_t> buf((qsizetype)w * h, 0);

    // compute shaderのimageStore結果を読むためのバリア+完了待ち
    // (finishPenSelect側の詳しいコメント参照)
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glFinish();

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, ctx.selectionMaskTex, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, w, h, GL_RED, GL_UNSIGNED_BYTE, buf.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx.defaultFbo());
    glDeleteFramebuffers(1, &fbo);
    return buf;
}

void SelectTool::uploadSelectionRect(ToolContext &ctx, const uint8_t *data, int x, int y, int w, int h)
{
    glBindTexture(GL_TEXTURE_2D, ctx.selectionMaskTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RED, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    // 以降のcompute shaderのimageLoad(bake.comp等のselMask読み取り)から見えるようにする
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
}

void SelectTool::finishLasso(ToolContext &ctx)
{
    if (lassoPoints_.size() < 3) {
        lassoPoints_.clear();
        ctx.requestRepaint();
        // 何も変わっていないので、commit側では履歴に積まれない(保留状態だけ降りる)
        if (ctx.commitSelectionUndo) ctx.commitSelectionUndo({}, 0, 0, 0, 0);
        return;
    }

    QVector<uint8_t> buf = rasterizePolygon(lassoPoints_, ctx.canvasW, ctx.canvasH);
    lassoPoints_.clear();
    commitSelection(ctx, std::move(buf));
}

void SelectTool::finishPenSelect(ToolContext &ctx)
{
    // 直前に蓄積したストロークマスクを読み戻し、合成方法に従って選択範囲へ反映する。
    //
    // 【重要】maskTexはcompute shader(stroke.comp)のimageStoreで書かれている。
    // その結果をglGetTexImageで読み出すには GL_TEXTURE_UPDATE_BARRIER_BIT が必要
    // (dispatchStrokeSegment()が出しているGL_SHADER_IMAGE_ACCESS_BARRIER_BITは
    //  「シェーダーからのimageLoad/Store」に対する可視性で、glGetTexImageは別枠)。
    // これが無いと、短いストロークほど「クリア直後の全ゼロ」が返ってくることがあり、
    // 選択範囲が空 → 下の「空なら選択解除」判定に落ちて全域255 = 一見「全選択」に
    // なってしまう。「ちょっと触っただけで全選択される」の原因はこれ。
    // バリアは「見え方の順序」しか保証しない。ディスパッチが実際に完了している保証は
    // 別で、以前は定期repaint()のバッファスワップが偶然その役割を果たしていた。
    // その再描画を(不要なので)止めた結果、読み戻しが空になり
    // 「選択が全く変わらない=全選択に見える + Undoにも積まれない」が再発した。
    // 副作用に頼らず、ここで明示的に完了を待つ。選択の確定はドラッグ1回につき
    // 1度しか起きないので、この同期のコストは問題にならない。
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glFinish();

    QVector<uint8_t> stroke(ctx.canvasW * ctx.canvasH, 0);
    glBindTexture(GL_TEXTURE_2D, ctx.maskTex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, stroke.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);

    commitSelection(ctx, std::move(stroke));

    // 作業用ストロークマスクは元に戻しておく(他ツールが古い内容を見ないように)
    glBindImageTexture(0, ctx.maskTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    ctx.computeMaskClearProgram->bind();
    // uDispatchOriginはAirbrushToolが最後にこのプログラムを使った際の非ゼロ値が
    // 残っていることがあるため、キャンバス全域を対象とするここでは明示的に(0,0)へ戻す
    // (PenEraserTool.cppの同種の修正と同じ理由)。
    setUniformIVec2(ctx.gl, ctx.computeMaskClearProgram, "uDispatchOrigin", 0, 0);
    glDispatchCompute((ctx.canvasW + 15) / 16, (ctx.canvasH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    ctx.computeMaskClearProgram->release();
    glBindImageTexture(0, ctx.maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);

    if (ctx.invalidateSelectionOutline) ctx.invalidateSelectionOutline();
    if (ctx.setHasSelection) ctx.setHasSelection(true);
    ctx.requestRepaint();
}
