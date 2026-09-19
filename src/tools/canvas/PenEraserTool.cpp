#include "tools/canvas/PenEraserTool.h"
#include "tools/core/CursorUtils.h"
#include "tools/core/DispatchBounds.h"
#include "tools/core/BrushShape.h"
#include "tools/core/MaskBrush.h"
#include "tools/core/PressureResponse.h"
#include "tools/core/ToolDispatchUtil.h"
#include <cmath>
#include <QDateTime>

// QOpenGLShaderProgram::setUniformValue(name, QPoint)はこの環境ではivec2 uniformに
// 対して値が反映されないため(型解決の問題。CanvasCompositor.cppの同名関数と同じ理由)、
// ivec2はglUniform2iで直接設定する。
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
    // view->scale() はキャンバスpx→デバイスpx。カーソル画像は論理pxで作るので
    // viewDpr で割って画面上の見た目のサイズに合わせる(ToolContext::viewDpr参照)。
    float viewScale = (ctx.view ? ctx.view->scale() : 1.0f) / (ctx.viewDpr > 0.0f ? ctx.viewDpr : 1.0f);
    return CursorUtils::makeCircleCursor(size * viewScale);
}

// 現在の筆圧(setPressure済み。GLWidget::mapPressureで筆圧カーブ適用済み)から半径を求める。
// 最小サイズ比率のぶんだけ、筆圧0でも太さを残す(PressureResponse参照)。
float PenEraserTool::pressureRadius(int size) const
{
    const float minRatio = isEraser_ ? toolCfg_->eraser().minSizeRatio()
                                     : toolCfg_->pen().minSizeRatio();
    float r = (float)size / 2.0f * PressureResponse::scale(pressure_, minRatio);
    // 傾き(ペンを寝かせた量)で太くする。マウス操作中は tiltAmount_ が0なので無影響。
    if (!isEraser_) {
        const float k = toolCfg_->pen().tiltSize();
        if (k > 0.0f) r *= 1.0f + tiltAmount_ * k;
    }
    return r;
}

// 筆圧→不透明度が有効なときの、このスタンプの濃度(0..1)。無効なら1.0。
// 消しゴムは不透明度の概念を持たない(常に全力で消す)ため常に1.0。
float PenEraserTool::pressureStampAlpha() const
{
    if (isEraser_) return 1.0f;
    const PenToolConfig &c = toolCfg_->pen();
    float a = c.pressureOpacity()
            ? PressureResponse::scale(pressure_, c.minOpacityRatio())
            : 1.0f;
    // 傾きで薄くする。完全には消さない(消えると設定した意味が分からなくなる)。
    if (c.tiltOpacity() > 0.0f) a *= qMax(0.05f, 1.0f - tiltAmount_ * c.tiltOpacity());
    return a;
}

// ---------------------------------------------------------------------------
// 散布・各種ランダム
// ---------------------------------------------------------------------------
// いずれも消しゴムには無い(ペン専用の設定)。消しゴムのときは既定値と同じ
// 「1粒・散らばりなし・揺らぎなし」で動く。
// ---------------------------------------------------------------------------
float PenEraserTool::rnd01()
{
    // xorshift32
    rngState_ ^= rngState_ << 13;
    rngState_ ^= rngState_ >> 17;
    rngState_ ^= rngState_ << 5;
    return float(rngState_ & 0x00FFFFFFu) / float(0x01000000u);
}

float PenEraserTool::rndSigned() { return rnd01() * 2.0f - 1.0f; }

// スタンプ間隔[px]。間隔のランダムは前後対称に揺らす(片側だけだと平均密度が
// 変わってしまい、揺らぎを上げるほど線が濃く/薄くなってしまう)。
float PenEraserTool::nextSpacingPx(int size)
{
    const float spacingRatio = isEraser_ ? toolCfg_->eraser().spacing() : toolCfg_->pen().spacing();
    float px = (float)size * spacingRatio;
    if (!isEraser_) {
        const float jitter = toolCfg_->pen().spacingJitter();
        if (jitter > 0.0f) px *= 1.0f + rndSigned() * jitter;
    }
    // 極端に小さい間隔での無限ループを避けるため下限を1pxに丸める
    return qMax(1.0f, px);
}

// 経路上の1点に対して、実際に打つスタンプ群をpendingStamps_へ積む。
// dist はストローク開始からこの点までの経路長[px](入り抜きに使う)。
void PenEraserTool::appendStamps(const QVector2D &pathPos, float radius, float alpha,
                                 int size, float dist)
{
    if (isEraser_) { // 消しゴムは散布・入り抜き・先端の形・色の設定を持たない
        pendingStamps_.append({ pathPos, radius, alpha, 0.0f, dist, 1.0f, -1, 0.0f, 0.0f, 0.0f });
        return;
    }
    // 引き直しが要るときは、この呼び出し1回ぶんを軌道上の1点として控える
    // (散布で複数粒に散らばっても、元の軌道点は共通)。
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
    // 色のランダム。基準色をHSVでずらす(色相は前後対称、明度は減らす向き)。
    const bool  jitterColor  = cfg.usesPerStampColor();
    const QColor baseColor   = toolCfg_->color().rawRGBA();

    for (int i = 0; i < count; i++) {
        QVector2D pos = pathPos;
        if (scatterPx > 0.0f) {
            // 軌道を中心に等方に散らす。半径は sqrt を噛ませて円内で一様にする
            // (そのまま一様乱数を半径にすると中心へ密集する)。
            const float ang = rnd01() * float(M_PI) * 2.0f;
            const float r   = scatterPx * std::sqrt(rnd01());
            pos += QVector2D(std::cos(ang) * r, std::sin(ang) * r);
        }
        // ランダム系はいずれも「最大値から減らす」向き。増える向きにすると
        // 設定したサイズ/不透明度を超えてしまい、上限としての意味が壊れる。
        const float r = (sizeJitter    > 0.0f) ? radius * (1.0f - rnd01() * sizeJitter)    : radius;
        const float a = (opacityJitter > 0.0f) ? alpha  * (1.0f - rnd01() * opacityJitter) : alpha;
        // 先端の角度 = 基準角 + (進行方向・傾き方向・ペン回転のうち有効なもの) + ランダム
        float t = float(cfg.angleDeg()) * float(M_PI) / 180.0f;
        if (cfg.followDirection())   t += lastDirAngle_;
        if (cfg.tiltAngleFollow())   t += tiltAngle_;
        if (cfg.penRotationFollow()) t += penRotation_;
        if (angleJitter > 0.0f)      t += rnd01() * float(M_PI) * 2.0f * angleJitter;

        // 真円率。傾けるほど平筆化する設定があるのでスタンプ単位で決める。
        float roundness = cfg.roundness();
        if (cfg.tiltFlatten() > 0.0f)
            roundness = qMax(0.05f, roundness * (1.0f - tiltAmount_ * cfg.tiltFlatten()));

        // このスタンプの色。色相は前後対称にずらし、明度は他のランダムと同じく
        // 「最大から減らす」向きにする(増やす向きだと選んだ色より明るくなってしまう)。
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
        // 引き直しが要るときは「入り抜きを掛ける前」の値を控えておく。ペンを離した
        // 時点でここから全部引き直す(rebuildStroke)。
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

// ---------------------------------------------------------------------------
// 入り抜き
// ---------------------------------------------------------------------------
// 「入り」はストローク開始から taperInPx をかけて 0→1 へ、
// 「抜き」は終端の taperOutPx 手前から 1→0 へ、それぞれ直線的に変化させる。
// ストロークが短くて入りと抜きが重なる場合は、両方を同じ比率で縮めて収める
// (そうしないと短い線が丸ごと消えてしまう)。
// ---------------------------------------------------------------------------
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
// (ラップを使った回はタイル範囲だけでは網羅できないのでキャンバス全域)。
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
// 焼き込み(onMouseRelease)の1段目と、下地混色が「拾う先」を用意するために
// ストローク開始時に呼ぶのとで共有する。
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

// ---------------------------------------------------------------------------
// 下地混色
// ---------------------------------------------------------------------------
// 筆の状態を初期化する(ストローク開始時と、引き直しの前)。
// aは筆の濡れ具合で、「拾った色だけで塗る」なら0(=まだ何も拾っていない)から始める。
// そうでなければ1.0にしておけば、brushState.comp側のmaxで常に1.0のままになり、
// スタンプの濃度に一切影響しない。
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
// stroke.comp が同じSSBOの色を読むので、必ずその直前に走らせる。
void PenEraserTool::dispatchBrushState(ToolContext &ctx, int stampCount)
{
    // シェーダーが宣言しているイメージユニットは、実際に読むかどうかに関わらず
    // 必ず「宣言と同じ形式の有効なテクスチャ」で埋めておくこと。空のまま
    // ディスパッチすると、この環境ではSSBOへの書き込みごと壊れて何も描かれなくなる。
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

    // イメージユニット0はmaskTexが入っている前提で使われているので戻す
    // (GLWidget::updateBelowCompositeCacheの同種コメント参照)。
    ctx.gl->glBindImageTexture(0, ctx.maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);
}

// 「抜き」を適用するため、マスクを一度消してからストローク全体を引き直す。
//
// 抜きは終点が分かって初めて決まるので、描いている最中には適用できない
// (CLIP STUDIO PAINTの「指定距離」の抜きも、ペンを離した瞬間に反映される)。
// 末尾だけ消して描き直す手も考えたが、ストロークが自分の終端付近を横切っていた
// 場合に前半まで消してしまうため、全体を引き直す。コストは描いている間に
// 分割して行った描画の合計と同じで、増えるわけではない。
void PenEraserTool::rebuildStroke(ToolContext &ctx)
{
    if (strokeStamps_.isEmpty()) return;

    // 後補正: なめらかにした軌道。空なら補正なし(=位置は動かさない)。
    const QVector<QVector2D> smoothed = smoothStrokePath();
    const bool  correcting = !smoothed.isEmpty();
    // 進行方向に追従するときは、軌道を動かしたぶん向きも変わる。スタンプの角度は
    // 基準角・傾き・ランダムまで畳み込んだ後の値なので、ここでは「補正前後の
    // 進行方向の差」だけを足して辻褄を合わせる。
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
    // 下地混色はマスクが消えた状態から同じ順序で拾い直す(拾う先の fullLayerTex は
    // ストローク開始時のまま変わっていないので、同じ結果が再現できる)。
    if (usesColorMixing(ctx)) resetBrushPaint(ctx);

    // 引き直しも描画中と同じ粒度で小分けにする。stroke.compのバッチ経路は
    // 「画素ごとにバッチ内の全スタンプを走査」するので、一度に大量に渡すと
    // ディスパッチ範囲×スタンプ数で急激に重くなる。
    // 下地混色のときは dispatchStampsChunked がさらに細かく割る。
    constexpr int kRebuildChunk = 32;
    QVector<PendingStamp> chunk;
    chunk.reserve(kRebuildChunk);

    const PenToolConfig &c = toolCfg_->pen();
    for (const PendingStamp &s : strokeStamps_) {
        const float f = taperFactor(s.dist, strokeLen_);
        if (f <= 0.001f) continue; // 完全に消える粒は打たない
        PendingStamp t = s;
        if (correcting && s.pathIndex >= 0 && s.pathIndex < smoothed.size()) {
            // 軌道点の移動量をそのまま足す。posは「軌道点+散布のずれ」なので、
            // これで散らばり方を変えずに軌道だけを引き直せる。
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

// ---------------------------------------------------------------------------
// 後補正
// ---------------------------------------------------------------------------
// 引いた軌道(strokePath_)を移動平均でならす。
//
// 手振れ補正が「ペン先へ遅れて追従する」ことで揺れを抑えるのに対し、こちらは
// 描き終わってから軌道そのものを整えるので、描いている間の追従の遅れが無い。
//
// 窓は端に近づくほど狭める(始点・終点では幅0)。こうすると両端が動かないので、
// 補正してもストロークが縮んだり、狙った位置から始点/終点がずれたりしない。
// 単純な移動平均を2回かけると三角形の重みになり、1回だけよりも滑らかに繋がる。
//
// 窓幅は「経路上の距離[px]」で決める。添字で決めるとスタンプ間隔やブラシサイズで
// 効き方が変わってしまうため。
QVector<QVector2D> PenEraserTool::smoothStrokePath() const
{
    if (isEraser_) return {};
    const float amount = toolCfg_->pen().postCorrection();
    const int n = strokePath_.size();
    if (amount <= 0.0f || n < 3) return {};

    // 補正が効く長さ。100%で経路上120px ぶんを平均する(ゆっくり大きく曲げた線は
    // 保ちつつ、手の震え程度の細かい揺れは消える程度の目安)。
    constexpr float kMaxRadiusPx = 120.0f;
    const float meanStep = strokeLen_ / float(qMax(1, n - 1));
    if (meanStep <= 0.0001f) return {};
    const int radius = qBound(1, int(amount * kMaxRadiusPx / meanStep + 0.5f), n / 2);

    // 移動平均は前置和で O(n)。2回かけるので前置和も2回作る。
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
// どれも無効なら stroke.comp を従来と完全に同じ経路へ通す。
bool PenEraserTool::usesStampAlpha() const
{
    if (isEraser_) return false;
    const PenToolConfig &c = toolCfg_->pen();
    if (c.pressureOpacity()) return true;
    if (c.opacityJitter() > 0.0f) return true;
    if (c.taperOpacity() && (c.taperInPx() > 0 || c.taperOutPx() > 0)) return true;
    if (c.tiltOpacity() > 0.0f) return true;
    // 紙質も「濃度」側で効かせる必要がある。この経路を通さないとマスクは
    // カバレッジのままになり、bake.comp の clamp(mask, 0, 不透明度) が
    // 紙の明るい部分を不透明度で頭打ちにしてしまう(不透明度を下げるほど
    // 紙の目が消えていく、という挙動になる)。
    if (c.paperEnabled()) return true;
    // 絵の具量(CPU側で濃度を減らす)と「拾った色だけで塗る」(brushState.compが
    // 濃度を減らす)。どちらもstroke.comp側がSSBOのwを読む経路を通す必要がある。
    if (c.usesPaintDepletion()) return true;
    if (c.pickupOnly()) return true;
    return false;
}

// スタンプごとの色(ストローク色バッファ)を使うか。
// 消しゴムは色を持たない。バッファが確保できない環境では従来の1色経路へ落とす。
bool PenEraserTool::usesStrokeColor(ToolContext &ctx) const
{
    if (isEraser_ || !toolCfg_->pen().usesPerStampColor()) return false;
    if (eraseBrush().active) return false; // 透明色は色ではなく「消す」動作
    return ctx.ensureStrokeColorTex && ctx.ensureStrokeColorTex() != 0;
}

// 下地混色が有効か。色バッファが要る点は色のランダムと同じなので、
// そちらが使えない環境では混色も無効になる。
bool PenEraserTool::usesColorMixing(ToolContext &ctx) const
{
    return toolCfg_->pen().mixRate() > 0.0f
        && ctx.computeBrushStateProgram != nullptr
        && ctx.brushPaintSSBO != 0
        && usesStrokeColor(ctx);
}

// このストロークが「消す」動作か(消しゴムツール、または透明色を選んでいる)と、
// その強さ。ToolConfig.h の EraseBrush 参照。
EraseBrush PenEraserTool::eraseBrush() const
{
    return eraseBrushFor(isEraser_, toolCfg_->color(), toolCfg_->pen().opacity());
}

// bake.compへ渡す色。「消す」ときは rgb は使われず、a だけが消す強さになる
// (uEraseMode とセットで渡すこと)。
QColor PenEraserTool::bakeColor(const ToolContext &ctx) const
{
    const EraseBrush erase = eraseBrush();

    // マスク編集中(このレイヤーのマスクサムネイルをクリックして選んでいる)なら、
    // レイヤー本体ではなくマスクへ焼き込む色を使う。マスクは色ではなく濃淡なので、
    // 「消す」ときは黒(=隠す)へその強さで寄せる。
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

// 先端(スタンプ)画像をテクスチャユニット1にバインドし、computeDrawProgramへ
// uTipTex/uUseTipTextureを設定する。消しゴムは先端画像を持たないため、常に
// 従来通りの手続き的な円(uUseTipTexture=0)を使う。
static void bindTipTexture(ToolContext &ctx, bool isEraser)
{
    ctx.gl->glActiveTexture(GL_TEXTURE1);
    ctx.gl->glBindTexture(GL_TEXTURE_2D, ctx.penTipTex);
    ctx.computeDrawProgram->setUniformValue("uTipTex", 1);
    ctx.computeDrawProgram->setUniformValue("uUseTipTexture", (!isEraser && ctx.penTipTex != 0) ? 1 : 0);
}

// 紙質テクスチャをテクスチャユニット2へバインドし、stroke.compのuPaper*を設定する。
// 消しゴムは紙質の設定を持たないので常に無効(uPaperStrength=0)。
//
// stroke.compのバッチ経路は uPaperStrength を無条件に読むため、無効なときも必ず
// 0を書き込むこと(前回のペンの値が残っていると消しゴムまで紙の目で消える)。
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

    // このバッチ内の全スタンプについて、Undo対象タイル範囲(および
    // ラップではみ出した反対側の追加範囲)をCPU側で先に統合してから、
    // ctx.expandStrokeUndoRegion()の呼び出しをバッチ全体で数回にまとめる
    // (スタンプごとに毎回呼ぶと、その中のFBO生成/破棄がスタンプ数ぶん発生して
    // しまうため。GPU→CPU読み戻し自体はitem1の変更でPBO経由の非同期に
    // なっているが、FBOのgen/deleteはドライバ呼び出しとして毎回コストがかかる)。
    // スタンプごとに半径が異なりうる(筆圧連動)ため、バウンディングボックスも
    // スタンプ自身の半径で計算する。
    int wTxMin = 0, wTxMax = -1, wTyMin = 0, wTyMax = -1; // 空(wTxMax<wTxMin)から開始
    int rawXMin = 0, rawXMax = -1, rawYMin = 0, rawYMax = -1;
    bool haveRaw = false;
    // 範囲は公称半径ではなく「実際にアルファが乗る外周半径」で取る。
    // 柔らかいブラシは公称半径より外へ広がるため(BrushShape.h参照)、
    // 公称半径のままだとブラシの外周が矩形に切り取られる。
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
        // onMouseRelease時のbake/書き戻し/マスククリアをキャンバス全域ではなく
        // 実際に触れたタイル範囲だけに限定するため、今回のストローク全体での
        // タイル範囲の和集合を積み上げておく。
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

    // stroke.compはキャンバス全体ではなく、今回のバッチの全スタンプが実際に触れうる
    // 矩形(半径ぶんのマージンを持たせたバウンディングボックスの和)だけをディスパッチする。
    // ラップ有効な軸は、キャンバス範囲外(負値・canvasW/H超)までそのままディスパッチし、
    // stroke.comp側で書き込み先座標をラップする(=端をまたぐスタンプが反対側にも描かれる)。
    const int pxMin = ctx.wrapX ? rawXMin : qMax(0, rawXMin);
    const int pxMax = ctx.wrapX ? rawXMax : qMin(ctx.canvasW, rawXMax);
    const int pyMin = ctx.wrapY ? rawYMin : qMax(0, rawYMin);
    const int pyMax = ctx.wrapY ? rawYMax : qMin(ctx.canvasH, rawYMax);
    if (pxMax <= pxMin || pyMax <= pyMin) return;

    // ストローク中の部分再描画用に、このバッチで実際に変わるキャンバス領域を報告する
    // (ToolContext::noteStrokeDirtyRegion参照)。ラップで反対側の端にも書き込みが
    // 発生しうる場合は、範囲を追わずキャンバス全域を報告して確実に覆う。
    if (ctx.noteStrokeDirtyRegion) {
        const bool wrapSpill = (ctx.wrapX && (rawXMin < 0 || rawXMax > ctx.canvasW))
                            || (ctx.wrapY && (rawYMin < 0 || rawYMax > ctx.canvasH));
        if (wrapSpill)
            ctx.noteStrokeDirtyRegion(0, 0, (float)ctx.canvasW, (float)ctx.canvasH);
        else
            ctx.noteStrokeDirtyRegion((float)qMax(0, pxMin), (float)qMax(0, pyMin),
                                      (float)qMin(ctx.canvasW, pxMax), (float)qMin(ctx.canvasH, pyMax));
    }

    // ラップではみ出した先(反対側の端)のタイルもUndoキャプチャしておく
    // (端をまたぐストロークは両端のタイルが変更されるため)。x/y両方が同時に
    // はみ出す対角コーナー部分の1タイルはこの単純化では捉え漏れることがあるが、
    // 実用上ブラシ半径はキャンバスよりずっと小さいため問題にならない想定。
    auto tileIdxOf = [&](int px, int tiles) { return qBound(0, px / tileSize, tiles - 1); };
    if (ctx.wrapX && rawXMin < 0) {
        int wrappedMinTile = tileIdxOf(ctx.canvasW + rawXMin, ctx.doc->tilesX());
        ctx.expandStrokeUndoRegion(wrappedMinTile, ctx.doc->tilesX() - 1, wTyMin, wTyMax);
        strokeUsedWrap_ = true; // 反対側の端タイルは累積タイル範囲(strokeTxMin_等)に含まれないため、
                                 // onMouseRelease側の部分bake最適化を諦めフルキャンバス処理へフォールバックする
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

    // 筆圧→不透明度・不透明度のランダム・入り抜き(不透明度)・傾き・紙質のいずれかが
    // 有効ならスタンプごとの濃度を使う。全部切ってあるときは stroke.comp を従来と
    // 完全に同じ経路へ通す(usesStampAlpha参照)。
    const bool useStampAlpha = usesStampAlpha();

    // スタンプ位置+半径+濃度をSSBOへアップロードする(xy=キャンバス座標、
    // z=半径、w=そのスタンプの濃度)。vec4で確保しているのはstd430でのvec3配列の
    // 16バイト境界切り上げを避けるためで、wは元々詰め物だったところを
    // 筆圧→不透明度に使っている(stroke.compのコメント参照)。
    //
    // 濃度は「このブラシの全力に対する割合」(0〜1)であって、ストロークの不透明度は
    // 含めない。不透明度はbake.comp/render.fragがuBrushColor側で一度だけ掛ける。
    // 以前はここで不透明度を畳み込んでいたが、焼き込み側でもう一度掛かるため
    // 不透明度が二乗になっていた(bake.compのコメント参照)。マスクが常に
    // 0〜1の全域を使えるようになるので、不透明度が低いときの階調も細かくなる
    // (マスクはR8なので、上限が不透明度だと段数もそのぶん減っていた)。
    // 絵の具量。筆に乗る絵の具を有限にして、進むほど濃度を落とし、尽きたら
    // 何も置かなくなる(線がかすれて消える)。消費量は「ストローク開始からの経路長」
    // だけで決まる純粋な関数なので、GPUの逐次パスを通さずここで掛けてしまえる
    // (入り抜き・後補正で引き直しても dist は変わらないため同じ結果が再現される)。
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
        // 先端画像の回転。画素ごとのループ内で三角関数を回さずに済むよう、
        // cos/sinまでここで計算して渡す(stroke.compのuStampDataのコメント参照)。
        packed.append(std::cos(s.angle));
        packed.append(std::sin(s.angle));
        packed.append(s.roundness);
        // ストローク開始からの経路長[px]。絵の具の消費量を求めるのに使う
        // (brushState.comp。stroke.comp側では読まない)。
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

    // 下地混色: この直後に走る stroke.comp が読むスタンプ色を、下地を拾いながら
    // 1粒ずつ書き換える(brushState.comp)。SSBOをアップロードした後・描く前に置く。
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
    // フロー。消しゴムはフローの設定を持たないので常に全力(=従来どおり)。
    ctx.computeDrawProgram->setUniformValue("uFlow", isEraser_ ? 1.0f : toolCfg_->pen().flow());
    // スタンプごとの色。使うときだけ色バッファをイメージユニット1へ繋ぐ
    // (使わないときはシェーダー側が触らないので未バインドで構わない)。
    const bool useStrokeColor = usesStrokeColor(ctx);
    ctx.computeDrawProgram->setUniformValue("uUseStrokeColor", useStrokeColor ? 1 : 0);
    if (useStrokeColor) {
        ctx.gl->glBindImageTexture(1, ctx.ensureStrokeColorTex(), 0, GL_FALSE, 0,
                                   GL_READ_WRITE, GL_RGBA16F);
    }
    ctx.computeDrawProgram->setUniformValue("uWrapX", ctx.wrapX ? 1 : 0);
    ctx.computeDrawProgram->setUniformValue("uWrapY", ctx.wrapY ? 1 : 0);
    ctx.gl->glDispatchCompute((pxMax - pxMin + 15) / 16, (pyMax - pyMin + 15) / 16, 1);
    // maskTexはここでimageStoreで書かれ、直後にrender.fragが texture() で
    // サンプルして読む(ライブプレビュー)。テクスチャフェッチへの可視性を保証する
    // GL_TEXTURE_FETCH_BARRIER_BIT を必ず含める。これが無いと、この12msスロットで
    // 描いた分をrender.fragが古いmaskTexのまま読み飛ばし、後のフレームでやっと
    // 反映される(=「ストローク前半が後からポンと現れる」)原因になる。
    ctx.gl->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    ctx.computeDrawProgram->release();
}

// ===========================================================================
void PenEraserTool::onMousePress(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    // ペンタブ使用時、OSが同じ物理接触に対して(Qtが合成する正しいマウスイベントとは
    // 別に)本物のマウスダウンメッセージを重複して送ってくることがある(Windows Ink
    // の互換レイヤー由来。event->source()では見分けが付かない)。既にストローク中なら
    // これは常に取りこぼした重複イベントなので無視する。ここを無視しないと、
    // ストローク中にもかかわらずここでctx.beginStrokeUndo()が再度呼ばれてUndo境界が
    // 壊れ、pendingStamps_がクリアされて直前の未フラッシュ分が消え、さらに
    // (下のGLWidget::mousePressEventが素のマウスと誤認して筆圧を1.0にリセットする
    // ため)フル筆圧の点が現在位置に打たれてしまう(タブレットで書き始め・
    // ストローク中に太い点が混じる不具合の原因)。
    if (isDrawing_) return;

    ctx.beginStrokeUndo();
    isDrawing_ = true;
    pendingStamps_.clear(); // 前回ストロークの取りこぼしがあれば念のため捨てる(通常はrelease時に空になっている)

    // item4: ストローク中は自レイヤー以外のピクセルが変わらないことを利用し、
    // 「アクティブレイヤーより下」の合成結果を一度だけキャッシュしておく
    // (render.frag はこれ以降、ストローク確定までこのキャッシュを使い、
    // z=アクティブレイヤーとその上だけを毎フレーム合成し直す)。
    if (ctx.updateBelowCompositeCache) ctx.updateBelowCompositeCache(ctx.doc->activeLayerIndex());
    // 「アクティブより上」も、条件を満たせば1枚へ事前合成しておく(下と対。上に
    // 多数レイヤーがあってもストローク中の合成コストが一定になる)。
    if (ctx.updateAboveCompositeCache) ctx.updateAboveCompositeCache(ctx.doc->activeLayerIndex());

    penTip_       = ctx.widgetToPixel(event->position());
    drawHead_     = penTip_;
    lastMousePos_ = drawHead_;
    // 散布・ランダムの種はストロークごとに変える(同じ線を引いても毎回違う散らばりに)
    rngState_ = (quint32)QDateTime::currentMSecsSinceEpoch() | 1u;

    lastDirAngle_ = 0.0f; // 進行方向はまだ無い

    // 抜き・後補正はどちらもペンを離してから決まるので、有効なときだけ
    // 引き直し用に全スタンプと軌道を控える。
    strokeLen_ = 0.0f;
    strokeStamps_.clear();
    strokePath_.clear();
    strokeRecording_ = !isEraser_ && (toolCfg_->pen().taperOutPx() > 0
                                      || toolCfg_->pen().postCorrection() > 0.0f);

    // マスクをクリアしてから今回のストロークを開始する。onMouseRelease()は
    // 前回のストロークが実際に触れた範囲だけを既にクリア済みのはずなので、
    // 通常はここでのクリアは完全に冗長(ここが以前はキャンバス全域を毎回
    // dispatchしていて、大きいキャンバスほど描き始めのたびに無駄なコストが
    // かかっていた)。ただし、前回のストロークがonMouseRelease()まで届かずに
    // 終わった(ウィジェット外でのマウスリリース等の異常系)場合はマスクに
    // ゴミが残ったままになるため、前回ストロークが実際に触れたタイル範囲
    // (strokeTxMin_等。onMouseRelease()はここをリセットしないので、次の
    // onMousePress()に来るこの時点でもまだ「前回の」値が読める)だけを対象に
    // 保険としてクリアしておく(ラップを使った回はタイル範囲だけでは
    // 網羅できないため、その場合のみ従来通りキャンバス全域をクリアする)。
    clearMaskOverStrokeTiles(ctx);
    strokeTileBoundsValid_ = false;
    strokeUsedWrap_        = false;

    // 下地混色: 「拾う先」としてアクティブレイヤーの内容を fullLayerTex へ展開し、
    // 筆に乗っている絵の具を基準色へ戻しておく。ストローク中 fullLayerTex は
    // 他の誰も触らないので、ここで1回展開すれば最後まで使える
    // (焼き込み時にもう一度展開されるが、そちらは対象範囲を絞るための別処理)。
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

    // フレームレート律速バッチ: ここでは即座にGPUディスパッチせず、スタンプを
    // 貯めておくだけにする(実際のディスパッチはflushPendingInput()で行う。
    // Tool.h/GLWidget::paintGL()参照)。
    // 進行方向に追従するときだけ、最初の1粒は方向が分かるまで保留する
    // (PenEraserTool.h の pendingFirstStamp_ のコメント参照)。
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

    // drawHead をペン先に引き寄せる(手振れ補正)。筆圧同様、実イベントごとに
    // 正しい順序でここを計算する(フレームレート律速で遅延させるのはこの後の
    // GPUディスパッチだけなので、手振れ補正やスタンプ間隔の精度は落ちない)。
    drawHead_ += (penTip_ - drawHead_) * (*smoothingStrength_);

    // 距離ベースのスタンプ方式: 前回イベントからの移動量ではなく、実際にペン先が
    // 移動した経路上で「間隔」ごとに区切った位置にだけスタンプを打つ。マウスの
    // イベント頻度(=速度)に関係なく、常に同じ間隔でスタンプが並ぶ。移動区間の
    // 途中で複数回間隔をまたぐ場合(速く動かした場合)は、その分だけこの1回の
    // イベントで複数スタンプを打つ(暴走を避けるため上限を設ける)。
    const QVector2D segVec = drawHead_ - lastMousePos_;
    const float segLen = segVec.length();
    if (segLen > 0.0001f) {
        const QVector2D dir = segVec / segLen;
        // 「進行方向に追従」用。キャンバス座標のYは上向きなので、画面で見た向きと
        // 合うように atan2 の符号をそのまま使う(先端画像の回転と同じ座標系)。
        lastDirAngle_ = std::atan2(dir.y(), dir.x());
        flushPendingFirstStamp(size); // 保留していた始点の1粒をこの向きで打つ
        constexpr int kMaxStampPointsPerEvent = 1000;
        int stampCount = 0;
        float t = distToNextStamp_;
        while (t <= segLen) {
            // 半径は区間内で前回イベント時の値から補間する。
            //
            // 全スタンプに今回の半径をそのまま使うと、筆圧の変化が「イベントが来た
            // 位置」で階段状に現れる。ペンを速く動かすほど1イベントあたりのスタンプ数が
            // 増えるので段差も大きくなり、入り抜きで太さがガクッと変わって見えていた。
            // 位置を経路上で補間しているのと同じ考え方で、太さも滑らかに繋ぐ。
            const float u = t / segLen; // 0(前回位置) 〜 1(今回位置)
            const float r = lastRadius_ + (radius - lastRadius_) * u;
            const float a = lastStampAlpha_ + (stampAlpha - lastStampAlpha_) * u; // 濃度も同様に補間

            // フレームレート律速バッチ: ここでも即座にはディスパッチせず貯めるだけ。
            // ペンタブの高頻度サンプルでも、実際のGPU処理は表示フレームごとに
            // まとまった1回のdispatchで済む(flushPendingInput()参照)。
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

// スタンプ列をディスパッチする。下地混色のときだけ小分けにする。
//
// brushState.comp は「そのバッチを描く前のキャンバス」を読むので、1バッチが長いと
// バッチの後ろのほうのスタンプが「自分がいま置いた色」を拾えず、バッチの継ぎ目で
// 混ざり方が飛んで見える。小分けにすると遅れがその粒数ぶんに抑えられる。
// ディスパッチ範囲もそのぶん小さくなるので、画素あたりの処理量はほぼ変わらない。
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
    // 細いブラシ: 追従性最優先で毎イベント即flush(0ms)。太いブラシ: 1回のdispatchが
    // ブラシ面積(∝size^2)ぶん重くなるので、間隔を空けてdispatch回数を抑える
    // (太いときはペン先とのわずかなずれより処理量削減が支配的、というユーザー方針)。
    // size<=32は0ms、size>=128で上限16ms(≒60fps)まで線形に増やす。現状は太くても
    // 十分軽いので、この境界はかなり大きめ(=積極的に即flush寄り)にしてある。
    const int size = isEraser_ ? toolCfg_->eraser().size() : toolCfg_->pen().size();
    return qBound(0, (size - 32) / 6, 16);
}

void PenEraserTool::onMouseRelease(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    if (!isDrawing_) return;
    isDrawing_ = false;

    // フレームレート律速バッチ: 直前のonMouseMove()で貯めたまま、まだ
    // 一度もflushPendingInput()が呼ばれていないスタンプが残っている可能性がある
    // (次のpaintGL()を待たずにマウスが離された場合)。以降のbake処理は
    // strokeTxMin_等(dispatchStampBatch内で更新される)を前提にするため、
    // ここで確実に流し切ってから先へ進む。
    // 一度も動かさずに離された(点を打っただけ)場合、保留したままの始点の1粒を
    // ここで打つ。方向が無いので基準角のまま。
    flushPendingFirstStamp(isEraser_ ? toolCfg_->eraser().size() : toolCfg_->pen().size());

    if (!pendingStamps_.isEmpty()) {
        dispatchStampsChunked(ctx, pendingStamps_);
        pendingStamps_.clear();
    }

    // 「抜き」の終点と「後補正」の軌道全体はここで初めて分かるので、マスクを消して
    // ストローク全体を引き直す(rebuildStroke のコメント参照)。この後の bake は
    // 引き直したマスクを読む。
    if (strokeRecording_ && !strokeStamps_.isEmpty())
        rebuildStroke(ctx);
    strokeStamps_.clear();
    strokeStamps_.squeeze(); // 長いストロークのぶんを抱えたままにしない
    strokePath_.clear();
    strokePath_.squeeze();
    strokeRecording_ = false;

    // 事前合成キャッシュ(アクティブより下/上)はここでは破棄しない。bakeで変わるのは
    // アクティブレイヤー自身の実データだけで、キャッシュはアクティブを含まない(下: 0..active-1、
    // 上: active+1..n-1)ため、このストローク後もそのまま正しい。破棄せず保持しておくことで、
    // 同じレイヤーへ続けて短い線を引く(文字を書く等)ときに全画面の事前合成をやり直さずに
    // 済む。キャッシュはレイヤー切り替え・構成変更・Undo等(doc_->onChanged)で無効化される。
    const int tileSize = ctx.tileSize;
    const Layer &layer = ctx.doc->activeLayer();
    // マスク編集中(このレイヤーのマスクサムネイルをクリックして選んでいる)なら、
    // レイヤー本体ではなくマスクのタイルへ焼き込む。
    const bool maskMode = layer.hasMask && ctx.editingMaskLayerIndex == ctx.doc->activeLayerIndex();

    if (maskMode) {
        // 実際に描き込んだので「マスクは編集済み」と記録する(編集モードを抜けるとき
        // 未編集の自動生成マスクを破棄する判定に使う)。
        ctx.doc->layers[ctx.doc->activeLayerIndex()].maskDirty = true;
    }
    const QColor col = bakeColor(ctx);

    // bake/書き戻し/マスククリアの対象範囲。ストローク中に実際に触れたタイル範囲だけに
    // 限定できる場合(ラップが絡んでいない場合)はそこだけを処理し、キャンバス全域の
    // blit/dispatchを避ける(キャンバスが大きいほど効果が大きい)。ラップで反対側の端にも
    // 書き込みが発生した場合は、その分の範囲を追わずに済ませるため従来通り全域を処理する。
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

    // 1. タイル -> fullLayerTex に展開(対象範囲のみ)
    expandLayerToFullTex(ctx, loopTxMin, loopTxMax, loopTyMin, loopTyMax, maskMode);

    // 2. bake(対象範囲のみ)
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
    // 「消す」モード。マスク編集中は色(濃淡)を塗るので常に通常モード。
    ctx.computeBakeProgram->setUniformValue("uEraseMode",
                                            (!maskMode && eraseBrush().active) ? 1 : 0);
    // ブラシの合成モード。消しゴム(消す動作)とマスク編集(濃淡を塗る)では意味を
    // 持たないので普通に固定する。
    ctx.computeBakeProgram->setUniformValue(
        "uBrushBlendMode",
        (isEraser_ || maskMode) ? 0 : toolCfg_->pen().brushBlendMode());
    // uDispatchOriginはAirbrushTool(スタンプごとの部分bakeで非ゼロ値を設定する)が
    // 最後にこのプログラムを使った際の値が残っていることがあるため、常に明示的に設定する
    // (放置すると、bakeが対象範囲とは無関係にずれた範囲にしか効かず、maskTexに描いた
    // ストロークの一部が焼き込まれないまま残る。その後のマスククリアも同じオフセットの
    // 影響を受けるため、焼き込まれなかった部分のマスクだけがクリアされずに残り、
    // プレビュー上は描かれているように見えるのにUndoしても一部が消えない・タイル単位で
    // 消え残る、という症状の原因だった)。
    setUniformIVec2(ctx.gl, ctx.computeBakeProgram, "uDispatchOrigin", regionMinX, regionMinY);
    ctx.gl->glDispatchCompute(groupsX, groupsY, 1);
    ctx.gl->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    ctx.computeBakeProgram->release();

    // 3. fullLayerTex -> タイルに書き戻す(対象範囲のみ)
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

    // 4. マスクをクリア(対象範囲のみ。他の範囲は元々ストロークで触れていないので
    // ゼロのままのはず)
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