#include "tools/canvas/PenEraserTool.h"
#include "tools/core/CursorUtils.h"
#include "tools/core/DispatchBounds.h"
#include "tools/core/BrushShape.h"
#include "tools/core/MaskBrush.h"
#include "tools/core/PressureResponse.h"
#include "tools/core/ToolDispatchUtil.h"
#include <cmath>
#include <QDateTime>

// QOpenGLShaderProgram::setUniformValue(name, QPoint)はこの環境ではivec2
// uniformに対して値が反映されないため(型解決の問題。CanvasCompositor.cppの同名関数と同じ理由)、ivec2はglUniform2iで直接設定する。
static void setUniformIVec2(QOpenGLFunctions_4_3_Core *gl, QOpenGLShaderProgram *prog,
                             const char *name, int x, int y)
{
    GLint loc = gl->glGetUniformLocation(prog->programId(), name);
    if (loc >= 0)
        gl->glUniform2i(loc, x, y);
}

std::optional<QCursor> PenEraserTool::cursor(const ToolContext &ctx) const
{
    int size = isEraser_ ? toolCfg_->eraser().size() : toolCfg_->pen().size();
    // view->scale() はキャンバスpx→デバイスpx。
    float viewScale = (ctx.view ? ctx.view->scale() : 1.0f) / (ctx.viewDpr > 0.0f ? ctx.viewDpr : 1.0f);
    return CursorUtils::makeCircleCursor(size * viewScale);
}

// 現在の筆圧(setPressure済み。CanvasWidget::mapPressureで筆圧カーブ適用済み)から半径を求める。
float PenEraserTool::pressureRadius(int size) const
{
    const float minRatio = isEraser_ ? toolCfg_->eraser().minSizeRatio()
                                     : toolCfg_->pen().minSizeRatio();
    float r = (float)size / 2.0f * PressureResponse::scale(pressure_, minRatio);
    // 傾き(ペンを寝かせた量)で太くする。
    if (!isEraser_) {
        const float k = toolCfg_->pen().tiltSize();
        if (k > 0.0f) r *= 1.0f + tiltAmount_ * k;
    }
    return r;
}

// 筆圧→不透明度が有効なときの、このスタンプの濃度(0..1)。
float PenEraserTool::pressureStampAlpha() const
{
    if (isEraser_) return 1.0f;
    const PenToolConfig &c = toolCfg_->pen();
    float a = c.pressureOpacity()
            ? PressureResponse::scale(pressure_, c.minOpacityRatio())
            : 1.0f;
    // 傾きで薄くする。
    if (c.tiltOpacity() > 0.0f) a *= qMax(0.05f, 1.0f - tiltAmount_ * c.tiltOpacity());
    return a;
}

// 散布・各種ランダム
float PenEraserTool::rnd01()
{
    // xorshift32
    rngState_ ^= rngState_ << 13;
    rngState_ ^= rngState_ >> 17;
    rngState_ ^= rngState_ << 5;
    return float(rngState_ & 0x00FFFFFFu) / float(0x01000000u);
}

float PenEraserTool::rndSigned() { return rnd01() * 2.0f - 1.0f; }

// スタンプ間隔[px]。
float PenEraserTool::nextSpacingPx(int size)
{
    const float spacingRatio = isEraser_ ? toolCfg_->eraser().spacing() : toolCfg_->pen().spacing();
    float px = (float)size * spacingRatio;
    if (!isEraser_) {
        const float jitter = toolCfg_->pen().spacingJitter();
        if (jitter > 0.0f) px *= 1.0f + rndSigned() * jitter;
    }
    // 極端に小さい間隔での無限ループを避けるため下限を1pxに丸める。
    return qMax(1.0f, px);
}

// 経路上の1点に対して、実際に打つスタンプ群をpendingStamps_へ積む。
void PenEraserTool::appendStamps(const QVector2D &pathPos, float radius, float alpha,
                                 int size, float dist)
{
    if (isEraser_) { // 消しゴムは散布・入り抜き・先端の形・色の設定を持たない
        pendingStamps_.append({ pathPos, radius, alpha, 0.0f, dist, 1.0f, -1, 0.0f, 0.0f, 0.0f });
        return;
    }
    // 引き直しが要るときは、この呼び出し1回ぶんを軌道上の1点として控える(散布で複数粒に散らばっても、元の軌道点は共通)。
    int pathIndex = -1;
    if (strokeRecording_) {
        pathIndex = strokePath_.size();
        strokePath_.append(pathPos);
    }
    const PenToolConfig &cfg = toolCfg_->pen();
    const int   count        = cfg.particleCount();
    const float scatterPx    = cfg.scatter() * (float)size; // 直径に対する比率
    const float sizeJitter   = cfg.sizeJitter();
    const float opacityJitter= cfg.opacityJitter();
    const float angleJitter  = cfg.angleJitter();
    // 色のランダム。
    const bool  jitterColor  = cfg.usesPerStampColor();
    const QColor baseColor   = toolCfg_->color().rawRGBA();

    for (int i = 0; i < count; i++) {
        QVector2D pos = pathPos;
        if (scatterPx > 0.0f) {
            // 軌道を中心に等方に散らす。
            const float ang = rnd01() * float(M_PI) * 2.0f;
            const float r   = scatterPx * std::sqrt(rnd01());
            pos += QVector2D(std::cos(ang) * r, std::sin(ang) * r);
        }
        // ジッターは基準値から減らす方向に適用する。
        const float r = (sizeJitter    > 0.0f) ? radius * (1.0f - rnd01() * sizeJitter)    : radius;
        const float a = (opacityJitter > 0.0f) ? alpha  * (1.0f - rnd01() * opacityJitter) : alpha;
        // 先端の角度 = 基準角 + (進行方向・傾き方向・ペン回転のうち有効なもの) + ランダム。
        float t = float(cfg.angleDeg()) * float(M_PI) / 180.0f;
        if (cfg.followDirection())   t += lastDirAngle_;
        if (cfg.tiltAngleFollow())   t += tiltAngle_;
        if (cfg.penRotationFollow()) t += penRotation_;
        if (angleJitter > 0.0f)      t += rnd01() * float(M_PI) * 2.0f * angleJitter;

        // 真円率。
        float roundness = cfg.roundness();
        if (cfg.tiltFlatten() > 0.0f)
            roundness = qMax(0.05f, roundness * (1.0f - tiltAmount_ * cfg.tiltFlatten()));

        // このスタンプの色。
        float cr = 0.0f, cg = 0.0f, cb = 0.0f;
        if (jitterColor) {
            float h, s, v;
            baseColor.getHsvF(&h, &s, &v);
            if (h < 0.0f) h = 0.0f; // 無彩色は色相が-1で返る
            h = std::fmod(h + rndSigned() * cfg.hueJitter() * 0.5f + 1.0f, 1.0f);
            v *= 1.0f - rnd01() * cfg.valueJitter();
            const QColor jc = QColor::fromHsvF(h, s, qBound(0.0f, v, 1.0f));
            cr = (float)jc.redF(); cg = (float)jc.greenF(); cb = (float)jc.blueF();
        }

        const PendingStamp stamp{ pos, qMax(0.1f, r), a, t, dist, roundness, pathIndex,
                                  cr, cg, cb };
        // 引き直しが要るときは「入り抜きを掛ける前」の値を控えておく。
        if (strokeRecording_) {
            if (strokeStamps_.size() < kMaxRecordedStamps) strokeStamps_.append(stamp);
            else strokeRecording_ = false; // 上限超過。引き直しは諦める(記録も止める)
        }

        // 「入り」は開始からの距離だけで決まるので、描いている最中にそのまま適用できる。
        PendingStamp live = stamp;
        const float f = taperInFactor(dist);
        if (f < 1.0f) {
            if (cfg.taperSize())    live.radius = qMax(0.1f, live.radius * f);
            if (cfg.taperOpacity()) live.alpha *= f;
        }
        pendingStamps_.append(live);
    }
}

// 保留していた最初の1粒を、そのとき分かっている進行方向で打つ。
void PenEraserTool::flushPendingFirstStamp(int size)
{
    if (!pendingFirstStamp_) return;
    pendingFirstStamp_ = false;
    appendStamps(firstStampPos_, firstStampRadius_, firstStampAlpha_, size, 0.0f);
}

// 入り抜き
float PenEraserTool::taperInFactor(float dist) const
{
    if (isEraser_) return 1.0f;
    const float inPx = (float)toolCfg_->pen().taperInPx();
    if (inPx <= 0.0f) return 1.0f;
    return qBound(0.0f, dist / inPx, 1.0f);
}

float PenEraserTool::taperFactor(float dist, float totalLen) const
{
    if (isEraser_) return 1.0f;
    const PenToolConfig &c = toolCfg_->pen();
    float inPx  = (float)c.taperInPx();
    float outPx = (float)c.taperOutPx();
    if (inPx <= 0.0f && outPx <= 0.0f) return 1.0f;

    const float sum = inPx + outPx;
    if (sum > totalLen && sum > 0.0f) { // 短いストローク: 両方を同率で縮める
        const float k = totalLen / sum;
        inPx  *= k;
        outPx *= k;
    }
    float f = 1.0f;
    if (inPx  > 0.0f) f = qMin(f, dist / inPx);
    if (outPx > 0.0f) f = qMin(f, (totalLen - dist) / outPx);
    return qBound(0.0f, f, 1.0f);
}

// マスクを、今回のストロークが触れたタイル範囲だけクリアする
void PenEraserTool::clearMaskOverStrokeTiles(ToolContext &ctx)
{
    if (!strokeTileBoundsValid_) return;
    const int tileSize = ctx.tileSize;
    int loopTxMin = 0, loopTxMax = ctx.doc->tilesX() - 1;
    int loopTyMin = 0, loopTyMax = ctx.doc->tilesY() - 1;
    if (!strokeUsedWrap_) {
        loopTxMin = strokeTxMin_; loopTxMax = strokeTxMax_;
        loopTyMin = strokeTyMin_; loopTyMax = strokeTyMax_;
    }
    const int regionMinX = loopTxMin * tileSize;
    const int regionMinY = loopTyMin * tileSize;
    const int regionMaxX = qMin(ctx.canvasW - 1, (loopTxMax + 1) * tileSize - 1);
    const int regionMaxY = qMin(ctx.canvasH - 1, (loopTyMax + 1) * tileSize - 1);
    const int groupsX = (regionMaxX - regionMinX + 1 + 15) / 16;
    const int groupsY = (regionMaxY - regionMinY + 1 + 15) / 16;

    ctx.computeMaskClearProgram->bind();
    setUniformIVec2(ctx.gl, ctx.computeMaskClearProgram, "uDispatchOrigin", regionMinX, regionMinY);
    ctx.gl->glDispatchCompute(groupsX, groupsY, 1);
    ctx.gl->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    ctx.computeMaskClearProgram->release();
    ctx.gl->glBindImageTexture(0, ctx.maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);
}

// アクティブレイヤーのタイルを、指定タイル範囲だけ fullLayerTex へ展開する。
void PenEraserTool::expandLayerToFullTex(ToolContext &ctx, int txMin, int txMax,
                                         int tyMin, int tyMax, bool maskMode)
{
    const Layer &layer = ctx.doc->activeLayer();
    GLuint srcFbo = 0, dstFbo = 0;
    ctx.gl->glGenFramebuffers(1, &srcFbo);
    ctx.gl->glGenFramebuffers(1, &dstFbo);
    ctx.gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    ctx.gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.fullLayerTex, 0);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
    forEachLayerTileInRange(layer, ctx.canvasW, ctx.canvasH, ctx.tileSize,
        txMin, txMax, tyMin, tyMax,
        [&](int, int, int si, int dstX, int dstY, int w, int h) {
            ctx.gl->glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
            ctx.gl->glBlitFramebuffer(0, 0, w, h, dstX, dstY, dstX + w, dstY + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }, maskMode);
    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, ctx.defaultFbo());
    ctx.gl->glDeleteFramebuffers(1, &srcFbo);
    ctx.gl->glDeleteFramebuffers(1, &dstFbo);
}

// 下地混色
void PenEraserTool::resetBrushPaint(ToolContext &ctx)
{
    if (!ctx.brushPaintSSBO) return;
    const QColor c = toolCfg_->color().rawRGBA();
    const float load = toolCfg_->pen().pickupOnly() ? 0.0f : 1.0f;
    const float init[4] = { (float)c.redF(), (float)c.greenF(), (float)c.blueF(), load };
    ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.brushPaintSSBO);
    ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(init), init);
    ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// このバッチのスタンプ色を、下地を拾いながら更新する(brushState.comp)。
void PenEraserTool::dispatchBrushState(ToolContext &ctx, int stampCount)
{
    // シェーダーが宣言しているイメージユニットは、実際に読むかどうかに関わらず必ず「宣言と同じ形式の有効なテクスチャ」で埋めておくこと。
    ctx.gl->glBindImageTexture(0, ctx.fullLayerTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    ctx.gl->glBindImageTexture(1, ctx.maskTex,      0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    ctx.gl->glBindImageTexture(2, ctx.ensureStrokeColorTex(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, ctx.brushPaintSSBO);

    ctx.computeBrushStateProgram->bind();
    ctx.computeBrushStateProgram->setUniformValue("uStampCount", stampCount);
    ctx.computeBrushStateProgram->setUniformValue("uMixRate", toolCfg_->pen().mixRate());
    ctx.computeBrushStateProgram->setUniformValue("uOpacity",
                                                  (float)bakeColor(ctx).alphaF());
    setUniformIVec2(ctx.gl, ctx.computeBrushStateProgram, "uCanvasSize",
                    ctx.canvasW, ctx.canvasH);
    ctx.gl->glDispatchCompute(1, 1, 1);
    // 次に走る stroke.comp がSSBOの色を読むので、書き込みの可視性を保証する。
    ctx.gl->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    ctx.computeBrushStateProgram->release();

    // イメージユニット0はmaskTexが入っている前提で使われているので戻す(CanvasWidget::updateBelowCompositeCacheの同種コメント参照)。
    ctx.gl->glBindImageTexture(0, ctx.maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);
}

// 「抜き」を適用するため、マスクを一度消してからストローク全体を引き直す。
void PenEraserTool::rebuildStroke(ToolContext &ctx)
{
    if (strokeStamps_.isEmpty()) return;

    // 後補正: なめらかにした軌道。
    const QVector<QVector2D> smoothed = smoothStrokePath();
    const bool  correcting = !smoothed.isEmpty();
    // 進行方向に追従するときは、軌道を動かしたぶん向きも変わる。
    const bool  followDir = correcting && toolCfg_->pen().followDirection();
    auto dirAngleAt = [](const QVector<QVector2D> &path, int i) {
        if (path.size() < 2) return 0.0f;
        const int a = qMax(0, i - 1);
        const int b = qMin(path.size() - 1, qMax(1, i));
        const QVector2D d = path[b] - path[a];
        if (d.lengthSquared() < 1e-8f) return 0.0f;
        return std::atan2(d.y(), d.x());
    };

    clearMaskOverStrokeTiles(ctx);
    // 下地混色はマスクが消えた状態から同じ順序で拾い直す(拾う先の fullLayerTex はストローク開始時のまま変わっていないので、同じ結果が再現できる)。
    if (usesColorMixing(ctx)) resetBrushPaint(ctx);

    // 引き直しも描画中と同じ粒度で小分けにする。
    constexpr int kRebuildChunk = 32;
    QVector<PendingStamp> chunk;
    chunk.reserve(kRebuildChunk);

    const PenToolConfig &c = toolCfg_->pen();
    for (const PendingStamp &s : strokeStamps_) {
        const float f = taperFactor(s.dist, strokeLen_);
        if (f <= 0.001f) continue; // 完全に消える粒は打たない
        PendingStamp t = s;
        if (correcting && s.pathIndex >= 0 && s.pathIndex < smoothed.size()) {
            // 軌道点の移動量をそのまま足す。
            t.pos += smoothed[s.pathIndex] - strokePath_[s.pathIndex];
            if (followDir)
                t.angle += dirAngleAt(smoothed, s.pathIndex) - dirAngleAt(strokePath_, s.pathIndex);
        }
        if (c.taperSize())    t.radius = qMax(0.1f, t.radius * f);
        if (c.taperOpacity()) t.alpha *= f;
        chunk.append(t);
        if (chunk.size() >= kRebuildChunk) { dispatchStampsChunked(ctx, chunk); chunk.clear(); }
    }
    if (!chunk.isEmpty()) dispatchStampsChunked(ctx, chunk);
}

// 後補正
QVector<QVector2D> PenEraserTool::smoothStrokePath() const
{
    if (isEraser_) return {};
    const float amount = toolCfg_->pen().postCorrection();
    const int n = strokePath_.size();
    if (amount <= 0.0f || n < 3) return {};

    // 補正が効く長さ。
    constexpr float kMaxRadiusPx = 120.0f;
    const float meanStep = strokeLen_ / float(qMax(1, n - 1));
    if (meanStep <= 0.0001f) return {};
    const int radius = qBound(1, int(amount * kMaxRadiusPx / meanStep + 0.5f), n / 2);

    // 移動平均は前置和で O(n)。
    auto boxPass = [n, radius](const QVector<QVector2D> &src) {
        QVector<QVector2D> sum(n + 1);
        sum[0] = QVector2D(0.0f, 0.0f);
        for (int i = 0; i < n; i++) sum[i + 1] = sum[i] + src[i];
        QVector<QVector2D> out(n);
        for (int i = 0; i < n; i++) {
            const int w = qMin(radius, qMin(i, n - 1 - i)); // 端ほど狭める
            const int a = i - w, b = i + w;
            out[i] = (sum[b + 1] - sum[a]) / float(b - a + 1);
        }
        return out;
    };
    return boxPass(boxPass(strokePath_));
}

// スタンプごとの濃度(SSBOのw)を使う必要があるか。
bool PenEraserTool::usesStampAlpha() const
{
    if (isEraser_) return false;
    const PenToolConfig &c = toolCfg_->pen();
    if (c.pressureOpacity()) return true;
    if (c.opacityJitter() > 0.0f) return true;
    if (c.taperOpacity() && (c.taperInPx() > 0 || c.taperOutPx() > 0)) return true;
    if (c.tiltOpacity() > 0.0f) return true;
    // 紙質も「濃度」側で効かせる必要がある。
    if (c.paperEnabled()) return true;
    // 絵の具量(CPU側で濃度を減らす)と「拾った色だけで塗る」(brushState.compが濃度を減らす)。
    if (c.usesPaintDepletion()) return true;
    if (c.pickupOnly()) return true;
    return false;
}

// スタンプごとの色(ストローク色バッファ)を使うか。
bool PenEraserTool::usesStrokeColor(ToolContext &ctx) const
{
    if (isEraser_ || !toolCfg_->pen().usesPerStampColor()) return false;
    if (eraseBrush().active) return false; // 透明色は色ではなく「消す」動作
    return ctx.ensureStrokeColorTex && ctx.ensureStrokeColorTex() != 0;
}

// 下地混色が有効か。
bool PenEraserTool::usesColorMixing(ToolContext &ctx) const
{
    return toolCfg_->pen().mixRate() > 0.0f
        && ctx.computeBrushStateProgram != nullptr
        && ctx.brushPaintSSBO != 0
        && usesStrokeColor(ctx);
}

// このストロークが「消す」動作か(消しゴムツール、または透明色を選んでいる)と、その強さ。
EraseBrush PenEraserTool::eraseBrush() const
{
    return eraseBrushFor(isEraser_, toolCfg_->color(), toolCfg_->pen().opacity());
}

// bake.compへ渡す色。
QColor PenEraserTool::bakeColor(const ToolContext &ctx) const
{
    const EraseBrush erase = eraseBrush();

    // マスク編集中(このレイヤーのマスクサムネイルをクリックして選んでいる)なら、レイヤー本体ではなくマスクへ焼き込む色を使う。
    const Layer &layer = ctx.doc->activeLayer();
    const bool maskMode = layer.hasMask && ctx.editingMaskLayerIndex == ctx.doc->activeLayerIndex();
    if (maskMode) {
        return erase.active
            ? maskBrushColor(true, toolCfg_->color().rawRGBA(), erase.strength)
            : maskBrushColor(false, toolCfg_->color().rawRGBA(), toolCfg_->pen().opacity());
    }
    return erase.active
        ? erase.shaderColor()
        : toPreMulColor(toolCfg_->color().rawRGBA(), toolCfg_->pen().opacity());
}

QColor PenEraserTool::toPreMulColor(const QColor &rawColor, float opacity) {
    float a = rawColor.alphaF() * opacity;
    return QColor::fromRgbF(
        rawColor.redF()   * a,
        rawColor.greenF() * a,
        rawColor.blueF()  * a,
        a
    );
}

// 先端(スタンプ)画像をテクスチャユニット1にバインドし、computeDrawProgramへuTipTex/uUseTipTextureを設定する。
static void bindTipTexture(ToolContext &ctx, bool isEraser)
{
    ctx.gl->glActiveTexture(GL_TEXTURE1);
    ctx.gl->glBindTexture(GL_TEXTURE_2D, ctx.penTipTex);
    ctx.computeDrawProgram->setUniformValue("uTipTex", 1);
    ctx.computeDrawProgram->setUniformValue("uUseTipTexture", (!isEraser && ctx.penTipTex != 0) ? 1 : 0);
}

// 紙質テクスチャをテクスチャユニット2へバインドし、stroke.compのuPaper*を設定する。
static void bindPaperTexture(ToolContext &ctx, const PenToolConfig &cfg, bool isEraser)
{
    const bool on = !isEraser && cfg.paperEnabled() && ctx.paperTex != 0;
    if (on) {
        ctx.gl->glActiveTexture(GL_TEXTURE2);
        ctx.gl->glBindTexture(GL_TEXTURE_2D, ctx.paperTex);
        ctx.computeDrawProgram->setUniformValue("uPaperTex", 2);
    }
    const float scale = qMax(0.01f, cfg.paperScale());
    ctx.computeDrawProgram->setUniformValue("uPaperStrength", on ? cfg.paperStrength() : 0.0f);
    ctx.computeDrawProgram->setUniformValue("uPaperScale", scale);
    // 縮小して貼るときだけミップを下げる(拡大側はLOD0のまま)。
    ctx.computeDrawProgram->setUniformValue("uPaperLod",
                                            scale < 1.0f ? std::log2(1.0f / scale) : 0.0f);
}

void PenEraserTool::dispatchStampBatch(ToolContext &ctx, const QVector<PendingStamp> &stamps)
{
    if (stamps.isEmpty()) return;
    const int tileSize = ctx.tileSize;

    // バッチ全体のUndo範囲とディスパッチ範囲を求める。
    int wTxMin = 0, wTxMax = -1, wTyMin = 0, wTyMax = -1; // 空(wTxMax<wTxMin)から開始
    int rawXMin = 0, rawXMax = -1, rawYMin = 0, rawYMax = -1;
    bool haveRaw = false;
    // 範囲は公称半径ではなく「実際にアルファが乗る外周半径」で取る。
    const float hardnessForBounds =
        isEraser_ ? toolCfg_->eraser().hardness() : toolCfg_->pen().hardness();
    for (const PendingStamp &s : stamps) {
        const StrokeDispatchBounds b = computeStrokeDispatchBounds(
            ctx.canvasW, ctx.canvasH, tileSize, ctx.doc->tilesX(), ctx.doc->tilesY(),
            s.pos, s.pos, BrushShape::outerRadius(s.radius, hardnessForBounds), 0);
        if (!b.empty()) {
            if (wTxMax < wTxMin) { wTxMin = b.wTxMin; wTxMax = b.wTxMax; wTyMin = b.wTyMin; wTyMax = b.wTyMax; }
            else {
                wTxMin = qMin(wTxMin, b.wTxMin); wTxMax = qMax(wTxMax, b.wTxMax);
                wTyMin = qMin(wTyMin, b.wTyMin); wTyMax = qMax(wTyMax, b.wTyMax);
            }
        }
        if (!haveRaw) { rawXMin = b.rawPxMinX; rawXMax = b.rawPxMaxX; rawYMin = b.rawPxMinY; rawYMax = b.rawPxMaxY; haveRaw = true; }
        else {
            rawXMin = qMin(rawXMin, b.rawPxMinX); rawXMax = qMax(rawXMax, b.rawPxMaxX);
            rawYMin = qMin(rawYMin, b.rawPxMinY); rawYMax = qMax(rawYMax, b.rawPxMaxY);
        }
    }

    if (wTxMax >= wTxMin) {
        ctx.expandStrokeUndoRegion(wTxMin, wTxMax, wTyMin, wTyMax);
        // onMouseRelease時のbake/書き戻し/マスククリアをキャンバス全域ではなく実際に触れたタイル範囲だけに限定するため、今回のストローク全体でのタイル範囲の和集合を積み上げておく。
        if (!strokeTileBoundsValid_) {
            strokeTxMin_ = wTxMin; strokeTxMax_ = wTxMax;
            strokeTyMin_ = wTyMin; strokeTyMax_ = wTyMax;
            strokeTileBoundsValid_ = true;
        } else {
            strokeTxMin_ = qMin(strokeTxMin_, wTxMin);
            strokeTxMax_ = qMax(strokeTxMax_, wTxMax);
            strokeTyMin_ = qMin(strokeTyMin_, wTyMin);
            strokeTyMax_ = qMax(strokeTyMax_, wTyMax);
        }
    }

    // stroke.compはキャンバス全体ではなく、今回のバッチの全スタンプが実際に触れうる矩形(半径ぶんのマージンを持たせたバウンディングボックスの和)だけをディスパッチする。
    const int pxMin = ctx.wrapX ? rawXMin : qMax(0, rawXMin);
    const int pxMax = ctx.wrapX ? rawXMax : qMin(ctx.canvasW, rawXMax);
    const int pyMin = ctx.wrapY ? rawYMin : qMax(0, rawYMin);
    const int pyMax = ctx.wrapY ? rawYMax : qMin(ctx.canvasH, rawYMax);
    if (pxMax <= pxMin || pyMax <= pyMin) return;

    // ストローク中の部分再描画用に、このバッチで実際に変わるキャンバス領域を報告する(ToolContext::noteStrokeDirtyRegion参照)。
    if (ctx.noteStrokeDirtyRegion) {
        const bool wrapSpill = (ctx.wrapX && (rawXMin < 0 || rawXMax > ctx.canvasW))
                            || (ctx.wrapY && (rawYMin < 0 || rawYMax > ctx.canvasH));
        if (wrapSpill)
            ctx.noteStrokeDirtyRegion(0, 0, (float)ctx.canvasW, (float)ctx.canvasH);
        else
            ctx.noteStrokeDirtyRegion((float)qMax(0, pxMin), (float)qMax(0, pyMin),
                                      (float)qMin(ctx.canvasW, pxMax), (float)qMin(ctx.canvasH, pyMax));
    }

    // ラップではみ出した先(反対側の端)のタイルもUndoキャプチャしておく(端をまたぐストロークは両端のタイルが変更されるため)。
    auto tileIdxOf = [&](int px, int tiles) { return qBound(0, px / tileSize, tiles - 1); };
    if (ctx.wrapX && rawXMin < 0) {
        int wrappedMinTile = tileIdxOf(ctx.canvasW + rawXMin, ctx.doc->tilesX());
        ctx.expandStrokeUndoRegion(wrappedMinTile, ctx.doc->tilesX() - 1, wTyMin, wTyMax);
        strokeUsedWrap_ = true; // 反対側の端タイルは累積タイル範囲(strokeTxMin_等)に含まれないため、
                                 // onMouseRelease側の部分bake最適化を諦めフルキャンバス処理へフォールバックする。
    }
    if (ctx.wrapX && rawXMax > ctx.canvasW) {
        int wrappedMaxTile = tileIdxOf(rawXMax - 1 - ctx.canvasW, ctx.doc->tilesX());
        ctx.expandStrokeUndoRegion(0, wrappedMaxTile, wTyMin, wTyMax);
        strokeUsedWrap_ = true;
    }
    if (ctx.wrapY && rawYMin < 0) {
        int wrappedMinTile = tileIdxOf(ctx.canvasH + rawYMin, ctx.doc->tilesY());
        ctx.expandStrokeUndoRegion(wTxMin, wTxMax, wrappedMinTile, ctx.doc->tilesY() - 1);
        strokeUsedWrap_ = true;
    }
    if (ctx.wrapY && rawYMax > ctx.canvasH) {
        int wrappedMaxTile = tileIdxOf(rawYMax - 1 - ctx.canvasH, ctx.doc->tilesY());
        ctx.expandStrokeUndoRegion(wTxMin, wTxMax, 0, wrappedMaxTile);
        strokeUsedWrap_ = true;
    }

    // 筆圧→不透明度・不透明度のランダム・入り抜き(不透明度)・傾き・紙質のいずれかが有効ならスタンプごとの濃度を使う。
    const bool useStampAlpha = usesStampAlpha();

    // スタンプ位置+半径+濃度をSSBOへアップロードする(xy=キャンバス座標、z=半径、w=そのスタンプの濃度)。
    const bool  depleting = !isEraser_ && toolCfg_->pen().usesPaintDepletion();
    const float paintCapacityPx = depleting
        ? toolCfg_->pen().paintCapacityPx(toolCfg_->pen().size()) : 0.0f;

    QVector<float> packed;
    packed.reserve(stamps.size() * 12);
    for (const PendingStamp &s : stamps) {
        float alpha = s.alpha;
        if (depleting) {
            // 容量0(絵の具量0%)は「最初から持っていない」なので常に0。
            alpha *= (paintCapacityPx > 0.0f)
                   ? qBound(0.0f, 1.0f - s.dist / paintCapacityPx, 1.0f) : 0.0f;
        }

        packed.append(s.pos.x());
        packed.append(s.pos.y());
        packed.append(s.radius);
        packed.append(alpha);
        // 先端画像の回転。
        packed.append(std::cos(s.angle));
        packed.append(std::sin(s.angle));
        packed.append(s.roundness);
        // ストローク開始からの経路長[px]。
        packed.append(s.dist);
        // スタンプごとの色(uUseStrokeColor!=0のときだけ読まれる)。
        packed.append(s.r);
        packed.append(s.g);
        packed.append(s.b);
        packed.append(0.0f);
    }
    ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.strokeStampSSBO);
    ctx.gl->glBufferData(GL_SHADER_STORAGE_BUFFER,
                         (GLsizeiptr)packed.size() * sizeof(float),
                         packed.constData(), GL_STREAM_DRAW);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ctx.strokeStampSSBO);

    // 下地混色: この直後に走る stroke.comp が読むスタンプ色を、下地を拾いながら1粒ずつ書き換える(brushState.comp)。
    if (usesColorMixing(ctx)) dispatchBrushState(ctx, stamps.size());

    const float hardness = isEraser_ ? toolCfg_->eraser().hardness() : toolCfg_->pen().hardness();

    ctx.computeDrawProgram->bind();
    ctx.computeDrawProgram->setUniformValue("uHardness", hardness);
    bindTipTexture(ctx, isEraser_);
    bindPaperTexture(ctx, toolCfg_->pen(), isEraser_);
    setUniformIVec2(ctx.gl, ctx.computeDrawProgram, "uDispatchOrigin", pxMin, pyMin);
    ctx.computeDrawProgram->setUniformValue("uUseStampBatch", 1);
    ctx.computeDrawProgram->setUniformValue("uStampCount", (int)stamps.size());
    ctx.computeDrawProgram->setUniformValue("uUseStampAlpha", useStampAlpha ? 1 : 0);
    // フロー。
    ctx.computeDrawProgram->setUniformValue("uFlow", isEraser_ ? 1.0f : toolCfg_->pen().flow());
    // スタンプごとの色。
    const bool useStrokeColor = usesStrokeColor(ctx);
    ctx.computeDrawProgram->setUniformValue("uUseStrokeColor", useStrokeColor ? 1 : 0);
    if (useStrokeColor) {
        ctx.gl->glBindImageTexture(1, ctx.ensureStrokeColorTex(), 0, GL_FALSE, 0,
                                   GL_READ_WRITE, GL_RGBA16F);
    }
    ctx.computeDrawProgram->setUniformValue("uWrapX", ctx.wrapX ? 1 : 0);
    ctx.computeDrawProgram->setUniformValue("uWrapY", ctx.wrapY ? 1 : 0);
    ctx.gl->glDispatchCompute((pxMax - pxMin + 15) / 16, (pyMax - pyMin + 15) / 16, 1);
    // maskTexはここでimageStoreで書かれ、直後にrender.fragが texture() でサンプルして読む(ライブプレビュー)。
    ctx.gl->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    ctx.computeDrawProgram->release();
}

void PenEraserTool::onMousePress(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    // Windows Inkが同じ接触のマウスイベントを重ねて送る場合がある。
    if (isDrawing_) return;

    ctx.beginStrokeUndo();
    isDrawing_ = true;
    pendingStamps_.clear(); // 前回ストロークの取りこぼしがあれば念のため捨てる(通常はrelease時に空になっている)

    // ストローク中に変化しない上下の合成結果をキャッシュする。
    if (ctx.updateBelowCompositeCache) ctx.updateBelowCompositeCache(ctx.doc->activeLayerIndex());
    // 「アクティブより上」も、条件を満たせば1枚へ事前合成しておく(下と対。上に多数レイヤーがあってもストローク中の合成コストが一定になる)。
    if (ctx.updateAboveCompositeCache) ctx.updateAboveCompositeCache(ctx.doc->activeLayerIndex());

    penTip_       = ctx.widgetToPixel(event->position());
    drawHead_     = penTip_;
    lastMousePos_ = drawHead_;
    // 散布・ランダムの種はストロークごとに変える(同じ線を引いても毎回違う散らばりに)。
    rngState_ = (quint32)QDateTime::currentMSecsSinceEpoch() | 1u;

    lastDirAngle_ = 0.0f; // 進行方向はまだ無い

    // 抜き・後補正が必要な場合だけストロークを記録する。
    strokeLen_ = 0.0f;
    strokeStamps_.clear();
    strokePath_.clear();
    strokeRecording_ = !isEraser_ && (toolCfg_->pen().taperOutPx() > 0
                                      || toolCfg_->pen().postCorrection() > 0.0f);

    // マスクをクリアしてから今回のストロークを開始する。
    clearMaskOverStrokeTiles(ctx);
    strokeTileBoundsValid_ = false;
    strokeUsedWrap_        = false;

    // 下地混色: 「拾う先」としてアクティブレイヤーの内容を fullLayerTex へ展開し、筆に乗っている絵の具を基準色へ戻しておく。
    if (usesColorMixing(ctx)) {
        const Layer &layer = ctx.doc->activeLayer();
        const bool maskMode = layer.hasMask
                           && ctx.editingMaskLayerIndex == ctx.doc->activeLayerIndex();
        expandLayerToFullTex(ctx, 0, ctx.doc->tilesX() - 1, 0, ctx.doc->tilesY() - 1, maskMode);
        resetBrushPaint(ctx);
    }

    int size = isEraser_? toolCfg_->eraser().size() : toolCfg_->pen().size();
    const float radius = pressureRadius(size);
    const float stampAlpha = pressureStampAlpha();

    // フレームレート律速バッチ: ここでは即座にGPUディスパッチせず、スタンプを貯めておくだけにする(実際のディスパッチはflushPendingInput()で行う。Tool.h/CanvasWidget::paintGL()参照)。
    pendingFirstStamp_ = !isEraser_ && toolCfg_->pen().followDirection();
    if (pendingFirstStamp_) {
        firstStampPos_    = lastMousePos_;
        firstStampRadius_ = radius;
        firstStampAlpha_  = stampAlpha;
    } else {
        appendStamps(lastMousePos_, radius, stampAlpha, size, 0.0f);
    }
    distToNextStamp_ = nextSpacingPx(size); // 押した点に1粒置いたので、次は間隔ぶん先
    lastRadius_      = radius;     // 次のイベントまでの補間の起点
    lastStampAlpha_  = stampAlpha;

    ctx.requestRepaint();
}

void PenEraserTool::onMouseMove(QMouseEvent *event, ToolContext &ctx)
{
    if (!isDrawing_) return;
    int size = isEraser_? toolCfg_->eraser().size() : toolCfg_->pen().size();

    const float radius = pressureRadius(size);
    const float stampAlpha = pressureStampAlpha();
    penTip_ = ctx.widgetToPixel(event->position());

    // drawHead をペン先に引き寄せる(手振れ補正)。
    drawHead_ += (penTip_ - drawHead_) * (*smoothingStrength_);

    // 距離ベースのスタンプ方式: 前回イベントからの移動量ではなく、実際にペン先が移動した経路上で「間隔」ごとに区切った位置にだけスタンプを打つ。
    const QVector2D segVec = drawHead_ - lastMousePos_;
    const float segLen = segVec.length();
    if (segLen > 0.0001f) {
        const QVector2D dir = segVec / segLen;
        // 「進行方向に追従」用。
        lastDirAngle_ = std::atan2(dir.y(), dir.x());
        flushPendingFirstStamp(size); // 保留していた始点の1粒をこの向きで打つ
        constexpr int kMaxStampPointsPerEvent = 1000;
        int stampCount = 0;
        float t = distToNextStamp_;
        while (t <= segLen) {
            // 半径は区間内で前回イベント時の値から補間する。
            const float u = t / segLen; // 0(前回位置) 〜 1(今回位置)
            const float r = lastRadius_ + (radius - lastRadius_) * u;
            const float a = lastStampAlpha_ + (stampAlpha - lastStampAlpha_) * u; // 濃度も同様に補間

            // フレームレート律速バッチ: ここでも即座にはディスパッチせず貯めるだけ。
            appendStamps(lastMousePos_ + dir * t, r, a, size, strokeLen_ + t);
            if (++stampCount >= kMaxStampPointsPerEvent) { t += segLen; break; }
            t += nextSpacingPx(size);
        }
        distToNextStamp_ = t - segLen; // 次のイベントへ持ち越す残り距離
    }
    strokeLen_ += segLen; // 入り抜き用の経路長

    lastRadius_     = radius;
    lastStampAlpha_ = stampAlpha;
    lastMousePos_   = drawHead_;
    ctx.requestRepaint();
}

// スタンプ列をディスパッチする。
void PenEraserTool::dispatchStampsChunked(ToolContext &ctx, const QVector<PendingStamp> &stamps)
{
    if (stamps.isEmpty()) return;
    if (!usesColorMixing(ctx)) { dispatchStampBatch(ctx, stamps); return; } // 小分けは色を拾うときだけ要る

    constexpr int kMixChunk = 8;
    for (int i = 0; i < stamps.size(); i += kMixChunk)
        dispatchStampBatch(ctx, stamps.mid(i, kMixChunk));
}

void PenEraserTool::flushPendingInput(ToolContext &ctx)
{
    if (pendingStamps_.isEmpty()) return;
    dispatchStampsChunked(ctx, pendingStamps_);
    pendingStamps_.clear();
}

int PenEraserTool::flushIntervalMs() const
{
    // 細いブラシ: 追従性最優先で毎イベント即flush(0ms)。
    const int size = isEraser_ ? toolCfg_->eraser().size() : toolCfg_->pen().size();
    return qBound(0, (size - 32) / 6, 16);
}

void PenEraserTool::onMouseRelease(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    if (!isDrawing_) return;
    isDrawing_ = false;

    // フレームレート律速バッチ: 直前のonMouseMove()で貯めたまま、まだ一度もflushPendingInput()が呼ばれていないスタンプが残っている可能性がある(次のpaintGL()を待たずにマウスが離された場合)。
    flushPendingFirstStamp(isEraser_ ? toolCfg_->eraser().size() : toolCfg_->pen().size());

    if (!pendingStamps_.isEmpty()) {
        dispatchStampsChunked(ctx, pendingStamps_);
        pendingStamps_.clear();
    }

    // 「抜き」の終点と「後補正」の軌道全体はここで初めて分かるので、マスクを消してストローク全体を引き直す(rebuildStroke のコメント参照)。
    if (strokeRecording_ && !strokeStamps_.isEmpty())
        rebuildStroke(ctx);
    strokeStamps_.clear();
    strokeStamps_.squeeze(); // 長いストロークのぶんを抱えたままにしない
    strokePath_.clear();
    strokePath_.squeeze();
    strokeRecording_ = false;

    // 事前合成キャッシュ(アクティブより下/上)はここでは破棄しない。
    const int tileSize = ctx.tileSize;
    const Layer &layer = ctx.doc->activeLayer();
    // マスク編集中(このレイヤーのマスクサムネイルをクリックして選んでいる)なら、レイヤー本体ではなくマスクのタイルへ焼き込む。
    const bool maskMode = layer.hasMask && ctx.editingMaskLayerIndex == ctx.doc->activeLayerIndex();

    if (maskMode) {
        // 実際に描き込んだので「マスクは編集済み」と記録する(編集モードを抜けるとき未編集の自動生成マスクを破棄する判定に使う)。
        ctx.doc->layers[ctx.doc->activeLayerIndex()].maskDirty = true;
    }
    const QColor col = bakeColor(ctx);

    // bake/書き戻し/マスククリアの対象範囲。
    const bool usePartial = strokeTileBoundsValid_ && !strokeUsedWrap_;
    const int loopTxMin = usePartial ? strokeTxMin_ : 0;
    const int loopTxMax = usePartial ? strokeTxMax_ : ctx.doc->tilesX() - 1;
    const int loopTyMin = usePartial ? strokeTyMin_ : 0;
    const int loopTyMax = usePartial ? strokeTyMax_ : ctx.doc->tilesY() - 1;

    const int regionMinX = loopTxMin * tileSize;
    const int regionMinY = loopTyMin * tileSize;
    const int regionMaxX = qMin(ctx.canvasW - 1, (loopTxMax + 1) * tileSize - 1);
    const int regionMaxY = qMin(ctx.canvasH - 1, (loopTyMax + 1) * tileSize - 1);
    const int regionW = regionMaxX - regionMinX + 1;
    const int regionH = regionMaxY - regionMinY + 1;
    const int groupsX = (regionW + 15) / 16;
    const int groupsY = (regionH + 15) / 16;

    // 1.
    expandLayerToFullTex(ctx, loopTxMin, loopTxMax, loopTyMin, loopTyMax, maskMode);

    // 2.
    ctx.gl->glBindImageTexture(0, ctx.maskTex,      0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);
    ctx.gl->glBindImageTexture(1, ctx.fullLayerTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
    ctx.gl->glBindImageTexture(2, ctx.selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    const bool bakeStrokeColor = usesStrokeColor(ctx) && !maskMode;
    if (bakeStrokeColor)
        ctx.gl->glBindImageTexture(3, ctx.ensureStrokeColorTex(), 0, GL_FALSE, 0,
                                   GL_READ_ONLY, GL_RGBA16F);
    ctx.computeBakeProgram->bind();
    ctx.computeBakeProgram->setUniformValue("uUseStrokeColor", bakeStrokeColor ? 1 : 0);
    ctx.computeBakeProgram->setUniformValue("uBrushColor", col.redF(), col.greenF(), col.blueF(), col.alphaF());
    // 「消す」モード。
    ctx.computeBakeProgram->setUniformValue("uEraseMode",
                                            (!maskMode && eraseBrush().active) ? 1 : 0);
    // ブラシの合成モード。
    ctx.computeBakeProgram->setUniformValue(
        "uBrushBlendMode",
        (isEraser_ || maskMode) ? 0 : toolCfg_->pen().brushBlendMode());
    // AirbrushToolでは部分bakeの原点が非ゼロになる。
    setUniformIVec2(ctx.gl, ctx.computeBakeProgram, "uDispatchOrigin", regionMinX, regionMinY);
    ctx.gl->glDispatchCompute(groupsX, groupsY, 1);
    ctx.gl->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    ctx.computeBakeProgram->release();

    // 3.
    {
        GLuint srcFbo = 0, dstFbo = 0;
        ctx.gl->glGenFramebuffers(1, &srcFbo);
        ctx.gl->glGenFramebuffers(1, &dstFbo);
        ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        ctx.gl->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.fullLayerTex, 0);
        ctx.gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        forEachLayerTileInRange(layer, ctx.canvasW, ctx.canvasH, tileSize,
            loopTxMin, loopTxMax, loopTyMin, loopTyMax,
            [&](int, int, int si, int srcX, int srcY, int w, int h) {
                ctx.gl->glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                ctx.gl->glBlitFramebuffer(srcX, srcY, srcX + w, srcY + h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }, maskMode);
        ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, ctx.defaultFbo());
        ctx.gl->glDeleteFramebuffers(1, &srcFbo);
        ctx.gl->glDeleteFramebuffers(1, &dstFbo);
    }

    ctx.gl->glMemoryBarrier(GL_ALL_BARRIER_BITS);

    // 4.
    ctx.gl->glBindImageTexture(0, ctx.maskTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    ctx.computeMaskClearProgram->bind();
    setUniformIVec2(ctx.gl, ctx.computeMaskClearProgram, "uDispatchOrigin", regionMinX, regionMinY); // 理由は上のbake呼び出し箇所のコメントを参照
    ctx.gl->glDispatchCompute(groupsX, groupsY, 1);
    ctx.gl->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    ctx.computeMaskClearProgram->release();
    ctx.gl->glBindImageTexture(0, ctx.maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);

    ctx.commitStrokeUndo();
    ctx.requestRepaint();
    ctx.notifyLayersChanged();
}
