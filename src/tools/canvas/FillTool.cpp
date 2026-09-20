#include "tools/canvas/FillTool.h"

#include <QDebug>
#include <QColor>
#include <cmath>
#include <vector>

// QOpenGLShaderProgram::setUniformValue(name, QPoint)はこの環境ではivec2
// uniformに対して値が反映されないため(型解決の問題。PenEraserTool.cppの同名関数と同じ理由)、ivec2はglUniform2iで直接設定する。
static void setUniformIVec2(QOpenGLFunctions_4_3_Core *gl, QOpenGLShaderProgram *prog,
                             const char *name, int x, int y)
{
    GLint loc = gl->glGetUniformLocation(prog->programId(), name);
    if (loc >= 0)
        gl->glUniform2i(loc, x, y);
}


void FillTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

// 内部ヘルパー: JFA パスを繰り返す。
void FillTool::runJfaPasses(GLuint tex, GLenum fmt,
                               int maxStep, int canvasW, int canvasH)
{
    for (int step = maxStep; step >= 1; step >>= 1) {
        glBindImageTexture(3, tex, 0, GL_FALSE, 0, GL_READ_WRITE, fmt);
        prog_.jfa->bind();
        prog_.jfa->setUniformValue("uStep", step);
        glDispatchCompute((canvasW+15)/16, (canvasH+15)/16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        prog_.jfa->release();
    }
}

// 内部ヘルパー: SDF を CPU に読み出す。
QVector<float> FillTool::buildSdf(int canvasW, int canvasH, GLuint defaultFbo)
{
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex_.sdfTex, 0);

    QVector<float> distField(canvasW * canvasH);
    glReadPixels(0, 0, canvasW, canvasH, GL_RED, GL_FLOAT, distField.data());

    glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
    glDeleteFramebuffers(1, &fbo);
    return distField;
}

// 内部ヘルパー: 塗り領域決定(SDFベースの等方的な隙間閉じ)
FillTool::FillRegion FillTool::computeFillRegion(int canvasW, int canvasH,
                                                 int sx, int sy,
                                                 const QVector<float> &distField,
                                                 bool wrapX, bool wrapY)
{
    const int N = canvasW * canvasH;
    const float wallD   = -toolCfg_->fill().fillExtension();
    const int   gapR    = toolCfg_->fill().fillGapSize();
    const int   deepR   = qMax(gapR + 1, toolCfg_->fill().protectRayLength());
    const int   seedK   = deepR + gapR; // シードから自分の部屋を探す測地予算
    const int   sealM   = deepR;        // 封印が禁止deepから届く測地距離
    const bool  sealing = gapR > 0;     // 隙間サイズ0なら隙間閉じ自体が無効

    enum : uint8_t {
        WALL   = 1,   // 壁(d < -fillExtension)
        NARROW = 2,   // d < gapR
        DEEP   = 4,   // d >= deepR
        WIDE   = 8,   // A2: d >= gapR のみを通ってシードの部屋から到達できる
        SEALV  = 16,  // B: 封印逆流の訪問済み
        SEAL   = 32,  // B: 封印(壁と同様に塗りを止める)
        VIS    = 64,  // A1/C/Dで使い回す訪問済み(A1後にクリアする)
    };

    std::vector<uint8_t> st(N, 0);
    {
        const float *d = distField.constData();
        const float gapF = (float)gapR, deepF = (float)deepR;
        for (int i = 0; i < N; i++) {
            const float v = d[i];
            if (v < wallD)       st[i] = WALL;
            else if (v < gapF)   st[i] = NARROW;
            else if (v >= deepF) st[i] = DEEP;
        }
    }

    // 4近傍を列挙する(ラップ有効な軸は反対端へ周回)。
    auto forEachN = [&](int i, auto &&fn) {
        const int y = i / canvasW, x = i - y * canvasW;
        if (x > 0)            fn(i - 1);
        else if (wrapX)       fn(i + canvasW - 1);
        if (x < canvasW - 1)  fn(i + 1);
        else if (wrapX)       fn(i - canvasW + 1);
        if (y > 0)            fn(i - canvasW);
        else if (wrapY)       fn(i + N - canvasW);
        if (y < canvasH - 1)  fn(i + canvasW);
        else if (wrapY)       fn(i - (N - canvasW));
    };

    const int seedIdx = sy * canvasW + sx;
    std::vector<int> cur, nxt;
    cur.reserve(1 << 12);
    nxt.reserve(1 << 12);

    if (sealing) {
        // A1: シード近傍のdeepピクセルを探す。
        std::vector<int> deepSeeds;
        cur.clear();
        cur.push_back(seedIdx);
        st[seedIdx] |= VIS;
        for (int depth = 0; depth <= seedK && !cur.empty(); depth++) {
            for (int i : cur)
                if (st[i] & DEEP) deepSeeds.push_back(i);
            if (depth == seedK) break;
            nxt.clear();
            for (int i : cur) forEachN(i, [&](int n) {
                if (st[n] & (WALL | VIS)) return;
                st[n] |= VIS;
                nxt.push_back(n);
            });
            cur.swap(nxt);
        }
        for (int i = 0; i < N; i++) st[i] &= ~VIS; // パスCで再利用する

        // A2: d >= gapRの領域をWIDEとして塗る。
        std::vector<int> stack;
        for (int i : deepSeeds)
            if (!(st[i] & WIDE)) { st[i] |= WIDE; stack.push_back(i); }
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            forEachN(i, [&](int n) {
                if (st[n] & (WALL | NARROW | WIDE)) return;
                st[n] |= WIDE;
                stack.push_back(n);
            });
        }

        // B: WIDE外のdeep領域からsealMまで封鎖する。
        cur.clear();
        for (int i = 0; i < N; i++)
            if ((st[i] & (DEEP | WIDE)) == DEEP) { st[i] |= SEALV; cur.push_back(i); }
        for (int depth = 0; depth < sealM && !cur.empty(); depth++) {
            nxt.clear();
            for (int i : cur) forEachN(i, [&](int n) {
                if (st[n] & (WALL | WIDE | SEALV)) return;
                st[n] |= SEALV;
                if (st[n] & NARROW) st[n] |= SEAL;
                nxt.push_back(n);
            });
            cur.swap(nxt);
        }

        // ユーザーが明示的にクリックした地点は(封印帯に入っていても)必ず塗り始める。
        st[seedIdx] &= ~SEAL;
    }

    // C: 壁と封鎖領域を避けてBFSする。
    FillRegion out;
    out.mask = QVector<uint8_t>(N, 0);
    out.minX = canvasW; out.maxX = -1;
    out.minY = canvasH; out.maxY = -1;
    uint8_t *mask = out.mask.data();

    auto fillPx = [&](int i) {
        mask[i] = 255;
        const int y = i / canvasW, x = i - y * canvasW;
        out.minX = qMin(out.minX, x); out.maxX = qMax(out.maxX, x);
        out.minY = qMin(out.minY, y); out.maxY = qMax(out.maxY, y);
    };

    // FIFOはヘッド添字方式のフラット配列(1件4バイト、各ピクセル最大1回enqueue)。
    std::vector<int> fifo;
    fifo.reserve(1 << 16);
    st[seedIdx] |= VIS;
    fifo.push_back(seedIdx);
    fillPx(seedIdx);
    for (size_t head = 0; head < fifo.size(); head++) {
        const int i = fifo[head];
        forEachN(i, [&](int n) {
            if (st[n] & (WALL | SEAL | VIS)) return;
            st[n] |= VIS;
            fillPx(n);
            fifo.push_back(n);
        });
    }

    // D: 結果を3px膨張して隙間を塞ぐ。
    if (sealing) {
        const std::vector<int> *frontier = &fifo;
        for (int depth = 0; depth < 3 && !frontier->empty(); depth++) {
            nxt.clear();
            for (int i : *frontier) forEachN(i, [&](int n) {
                if (st[n] & (WALL | VIS)) return;
                st[n] |= VIS;
                fillPx(n);
                nxt.push_back(n);
            });
            cur.swap(nxt);
            frontier = &cur;
        }
    }

    return out;
}

QColor FillTool::readPixel(GLuint tex, int x, int y, GLuint defaultFbo)
{
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    uint8_t px[4] = {};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
    glDeleteFramebuffers(1, &fbo);
    return QColor(px[0], px[1], px[2], px[3]);
}

bool FillTool::execute(ToolContext &ctx, const QPointF &widgetPos)
{
    if (ctx.doc->layers.isEmpty()) return false;

    QVector2D startPx = ctx.widgetToPixel(widgetPos);
    int sx = qRound(startPx.x());
    int sy = qRound(startPx.y());

    // 透明色を選んでいるときは、色で塗るのではなく塗りつぶし範囲を消す
    const EraseBrush erase = toolCfg_
        ? eraseBrushFor(false, toolCfg_->color(), toolCfg_->pen().opacity())
        : EraseBrush{};
    QColor brushColor = erase.active ? erase.shaderColor() : ctx.activeBrushPreMulColor();

    int canvasW = ctx.canvasW;
    int canvasH = ctx.canvasH;
    const Layer &layer = ctx.doc->activeLayer();
    const int tileSize = ctx.tileSize;
    float wallThreshold = wallThreshold_;
    GLuint defaultFbo = ctx.defaultFbo();

    if (sx < 0 || sx >= canvasW || sy < 0 || sy >= canvasH) return false;

    // Undo記録開始(実際に塗ったタイルの範囲はfilled確定後にexpandStrokeUndoRegionで確定する)。
    ctx.beginStrokeUndo();

    const bool referenceCanvas = toolCfg_->fill().referenceCanvas();

    // Step 0: アクティブレイヤーのタイルをfullLayerTexへ展開する。
    {
        GLuint srcFbo = 0, dstFbo = 0;
        glGenFramebuffers(1, &srcFbo);
        glGenFramebuffers(1, &dstFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.fullLayerTex, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        for (int ty = 0; ty < ctx.doc->tilesY(); ty++)
            for (int tx = 0; tx < ctx.doc->tilesX(); tx++) {
                int si = layer.tileSliceAtCanvasTile(tx, ty);
                if (si < 0) continue; // 通常起こらない(レイヤーは常にキャンバス全体を覆う)
                int dstX = tx * tileSize, dstY = ty * tileSize;
                int w = qMin(tileSize, canvasW - dstX), h = qMin(tileSize, canvasH - dstY);
                glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                glBlitFramebuffer(0, 0, w, h, dstX, dstY, dstX + w, dstY + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &srcFbo);
        glDeleteFramebuffers(1, &dstFbo);
    }

    // Step 0.5: 境界判定の参照先テクスチャと開始色を決定する。
    
    if (referenceCanvas)
        ctx.updateCompositedTex(); // 全レイヤーを合成してcompositedTexを最新化
    GLuint referenceTex = referenceCanvas ? ctx.compositedTex : ctx.fullLayerTex;
    QColor startColor = readPixel(referenceTex, sx, sy, defaultFbo);


    // Step 1: 壁マスク生成(参照先テクスチャを走査)。
    glBindImageTexture(1, referenceTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(3, tex_.wallTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    prog_.wall->bind();
    prog_.wall->setUniformValue("uWallThreshold", wallThreshold);
    prog_.wall->setUniformValue("uStartColor",
        (float)startColor.redF(), (float)startColor.greenF(),
        (float)startColor.blueF(), (float)startColor.alphaF());
    glDispatchCompute((canvasW+15)/16, (canvasH+15)/16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    prog_.wall->release();

    // Step 2/3: 外側JFA。
    glBindImageTexture(3, tex_.wallTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);
    glBindImageTexture(4, tex_.outerJfaTex,  0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    prog_.jfaInit->bind();
    glDispatchCompute((canvasW+15)/16, (canvasH+15)/16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    prog_.jfaInit->release();

    int maxStep = 1;
    while (maxStep < qMax(canvasW, canvasH)) maxStep <<= 1;
    maxStep >>= 1;

    runJfaPasses(tex_.outerJfaTex, GL_RG16F, maxStep, canvasW, canvasH);

    // Step 4/5: 内側JFA。
    glBindImageTexture(3, tex_.wallTex,      0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);
    glBindImageTexture(4, tex_.innerJfaTex,  0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    prog_.jfaInitInner->bind();
    glDispatchCompute((canvasW+15)/16, (canvasH+15)/16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    prog_.jfaInitInner->release();

    runJfaPasses(tex_.innerJfaTex, GL_RG16F, maxStep, canvasW, canvasH);

    // Step 6: finalize → sdfTex
    glBindImageTexture(3, tex_.wallTex,     0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);
    glBindImageTexture(4, tex_.outerJfaTex,      0, GL_FALSE, 0, GL_READ_ONLY,  GL_RG16F);
    glBindImageTexture(5, tex_.innerJfaTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RG16F);
    glBindImageTexture(6, tex_.sdfTex,      0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    prog_.jfaFinalize->bind();
    glDispatchCompute((canvasW+15)/16, (canvasH+15)/16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    prog_.jfaFinalize->release();

    // Step 7: SDF を CPU に読み出し。
    QVector<float> distField = buildSdf(canvasW, canvasH, defaultFbo);

    // 開始点が壁ならキャンセル。
    {
        GLuint wFbo = 0;
        glGenFramebuffers(1, &wFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, wFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex_.wallTex, 0);
        uint8_t wallVal = 0;
        glReadPixels(sx, sy, 1, 1, GL_RED, GL_UNSIGNED_BYTE, &wallVal);
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &wFbo);
        if (wallVal > 127) return false;
    }

    // Step 8: 塗り領域決定(SDFベースの隙間閉じ+BFS。computeFillRegion参照)。
    FillRegion region = computeFillRegion(canvasW, canvasH, sx, sy, distField,
                                          ctx.wrapX, ctx.wrapY);

    // Step 9: マスクを GPU に転送して bake
    if (region.maxX >= 0) {
        const int txMinT = qBound(0, region.minX / tileSize, ctx.doc->tilesX() - 1);
        const int txMaxT = qBound(0, region.maxX / tileSize, ctx.doc->tilesX() - 1);
        const int tyMinT = qBound(0, region.minY / tileSize, ctx.doc->tilesY() - 1);
        const int tyMaxT = qBound(0, region.maxY / tileSize, ctx.doc->tilesY() - 1);
        ctx.expandStrokeUndoRegion(txMinT, txMaxT, tyMinT, tyMaxT);
    }

    glBindTexture(GL_TEXTURE_2D, tex_.maskTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvasW, canvasH,
                    GL_RED, GL_UNSIGNED_BYTE, region.mask.constData());
    glBindImageTexture(0, tex_.maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);

    // fullLayerTexはStep 0で既にアクティブレイヤーの内容へ展開済み。
    glBindImageTexture(1, ctx.fullLayerTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
    glBindImageTexture(2, ctx.selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);

    prog_.bake->bind();
    prog_.bake->setUniformValue("uBrushColor",
        (float)brushColor.redF(), (float)brushColor.greenF(),
        (float)brushColor.blueF(), (float)brushColor.alphaF());
    // bake.compはペン/エアブラシと共有しているため、消さない場合も必ず明示する。
    prog_.bake->setUniformValue("uEraseMode", erase.active ? 1 : 0);
    // 合成モード・スタンプごとの色はペン専用の設定なので、ここでは明示的に既定へ戻す。
    prog_.bake->setUniformValue("uBrushBlendMode", 0);
    prog_.bake->setUniformValue("uUseStrokeColor", 0);
    // uDispatchOriginはAirbrushToolが最後にこのプログラム(CanvasWidgetと共有しているcomputeBakeProgram)を使った際の非ゼロ値が残っていることがあるため、
    // キャンバス全域を対象とするここでは明示的に(0,0)へ戻す(PenEraserTool.cppの同種の修正と同じ理由)。
    setUniformIVec2(this, prog_.bake, "uDispatchOrigin", 0, 0);
    glDispatchCompute((canvasW+15)/16, (canvasH+15)/16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    prog_.bake->release();

    // fullLayerTex -> タイルに書き戻す。
    {
        GLuint srcFbo = 0, dstFbo = 0;
        glGenFramebuffers(1, &srcFbo);
        glGenFramebuffers(1, &dstFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.fullLayerTex, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        for (int ty = 0; ty < ctx.doc->tilesY(); ty++)
            for (int tx = 0; tx < ctx.doc->tilesX(); tx++) {
                int si = layer.tileSliceAtCanvasTile(tx, ty);
                if (si < 0) continue; // 通常起こらない(レイヤーは常にキャンバス全体を覆う)
                int srcX = tx * tileSize, srcY = ty * tileSize;
                int w = qMin(tileSize, canvasW - srcX), h = qMin(tileSize, canvasH - srcY);
                glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                glBlitFramebuffer(srcX, srcY, srcX + w, srcY + h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &srcFbo);
        glDeleteFramebuffers(1, &dstFbo);
    }

    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    // マスクをクリア。
    prog_.maskClear->bind();
    // uDispatchOriginはAirbrushToolが最後にこのプログラム(CanvasWidgetと共有しているcomputeMaskClearProgram)を使った際の非ゼロ値が残っていることがあるため、
    // キャンバス全域を対象とするここでは明示的に(0,0)へ戻す(PenEraserTool.cppの同種の修正と同じ理由)。
    setUniformIVec2(this, prog_.maskClear, "uDispatchOrigin", 0, 0);
    glDispatchCompute((canvasW+15)/16, (canvasH+15)/16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    prog_.maskClear->release();

    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR)
        qWarning() << "FillEngine GL error:" << err;

    ctx.commitStrokeUndo();
    ctx.requestRepaint();
    ctx.notifyLayersChanged();

    return true;
}
