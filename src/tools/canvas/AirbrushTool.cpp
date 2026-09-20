#include "tools/canvas/AirbrushTool.h"
#include "tools/core/CursorUtils.h"
#include "tools/core/DispatchBounds.h"
#include "tools/core/BrushShape.h"
#include "tools/core/PressureResponse.h"
#include "tools/core/MaskBrush.h"
#include "tools/core/ToolDispatchUtil.h"
#include <cmath>

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

std::optional<QCursor> AirbrushTool::cursor(const ToolContext &ctx) const
{
    int size = toolCfg_->airbrush().size();
    // view->scale() はキャンバスpx→デバイスpx。カーソル画像は論理pxで作るので
    // viewDpr で割って画面上の見た目のサイズに合わせる(ToolContext::viewDpr参照)。
    float viewScale = (ctx.view ? ctx.view->scale() : 1.0f) / (ctx.viewDpr > 0.0f ? ctx.viewDpr : 1.0f);
    return CursorUtils::makeCircleCursor(size * viewScale);
}

static QColor toPreMulColor(const QColor &rawColor, float opacity)
{
    float a = rawColor.alphaF() * opacity;
    return QColor::fromRgbF(
        rawColor.redF()   * a,
        rawColor.greenF() * a,
        rawColor.blueF()  * a,
        a
    );
}

// ===========================================================================
// 現在の筆圧(setPressure済み。CanvasWidget::mapPressureで筆圧カーブ適用済み)から半径を求める。
float AirbrushTool::pressureRadius(int size) const
{
    return (float)size / 2.0f
         * PressureResponse::scale(pressure_, toolCfg_->airbrush().minSizeRatio());
}

// 筆圧→不透明度が有効なときの、このスタンプの濃度(0..1)。無効なら1.0。
float AirbrushTool::pressureStampAlpha() const
{
    if (!toolCfg_->airbrush().pressureOpacity()) return 1.0f;
    return PressureResponse::scale(pressure_, toolCfg_->airbrush().minOpacityRatio());
}

void AirbrushTool::stampAndBake(ToolContext &ctx, const QVector2D &pos, float radius, float hardness,
                                float opacityScale)
{
    const int tileSize = ctx.tileSize;

    // ラップ(キャンバス周回)は現状未対応(margin/wrap=0固定)。ブラシ影響範囲を
    // キャンバス内にクランプしたバウンディングボックス/タイル範囲を求める
    // (PenEraserTool::dispatchStrokeSegmentのUndo範囲計算と同じ考え方)。
    const StrokeDispatchBounds b = computeStrokeDispatchBounds(
        ctx.canvasW, ctx.canvasH, tileSize, ctx.doc->tilesX(), ctx.doc->tilesY(),
        pos, pos, BrushShape::outerRadius(radius, hardness), 0); // 外周はBrushShape.h参照
    if (b.empty()) return;

    ctx.expandStrokeUndoRegion(b.wTxMin, b.wTxMax, b.wTyMin, b.wTyMax);

    const int regionMinX = b.regionMinX;
    const int regionMinY = b.regionMinY;
    const int regionW    = b.regionMaxX - b.regionMinX + 1;
    const int regionH    = b.regionMaxY - b.regionMinY + 1;
    const int groupsX    = (regionW + 15) / 16;
    const int groupsY    = (regionH + 15) / 16;

    // ストローク中の部分再描画用(PenEraserTool::dispatchStampBatchと同じ)
    if (ctx.noteStrokeDirtyRegion)
        ctx.noteStrokeDirtyRegion((float)b.regionMinX, (float)b.regionMinY,
                                  (float)b.regionMaxX, (float)b.regionMaxY);

    const Layer &layer = ctx.doc->activeLayer();
    const bool maskMode = layer.hasMask && ctx.editingMaskLayerIndex == ctx.doc->activeLayerIndex();
    if (maskMode)
        ctx.doc->layers[ctx.doc->activeLayerIndex()].maskDirty = true; // 実際に描いたのでマスクは編集済み
    // 筆圧→不透明度は、このスタンプの色のアルファへ直接掛ける
    // (opacityScaleは無効時1.0なので、そのときは従来と完全に同じ色になる)。
    const float opacity = toolCfg_->airbrush().opacity() * opacityScale;
    // エアブラシ自体に「消しゴム」モードは無いが、透明色を選べば消す動作になる
    // (重ねるほど消えていく。ToolConfig.h の EraseBrush 参照)。
    const EraseBrush erase = eraseBrushFor(false, toolCfg_->color(), opacity);
    const QColor col = maskMode
        ? maskBrushColor(erase.active, toolCfg_->color().rawRGBA(),
                         erase.active ? erase.strength : opacity)
        : (erase.active ? erase.shaderColor()
                        : toPreMulColor(toolCfg_->color().rawRGBA(), opacity));

    // 1. マスクをこのスタンプの影響範囲だけクリアする
    ctx.gl->glBindImageTexture(0, ctx.maskTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    ctx.computeMaskClearProgram->bind();
    setUniformIVec2(ctx.gl, ctx.computeMaskClearProgram, "uDispatchOrigin", regionMinX, regionMinY);
    ctx.gl->glDispatchCompute(groupsX, groupsY, 1);
    ctx.gl->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    ctx.computeMaskClearProgram->release();

    // 2. スタンプ形状(円、硬さフォールオフ)をマスクへ描く
    ctx.gl->glBindImageTexture(0, ctx.maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);
    ctx.computeDrawProgram->bind();
    ctx.computeDrawProgram->setUniformValue("uRadius",   radius);
    ctx.computeDrawProgram->setUniformValue("uHardness", hardness);
    ctx.computeDrawProgram->setUniformValue("uUseTipTexture", 0); // 先端画像は使わず常に手続き円
    // PenEraserToolがこのプログラムをバッチスタンプモード(uUseStampBatch=1)で
    // 使った後にここへ来ることがあるため、線分モードへ明示的に戻す。
    ctx.computeDrawProgram->setUniformValue("uUseStampBatch", 0);
    ctx.computeDrawProgram->setUniformValue("uPosStart", pos);
    ctx.computeDrawProgram->setUniformValue("uPosEnd",   pos);
    ctx.computeDrawProgram->setUniformValue("uWrapX", 0);
    ctx.computeDrawProgram->setUniformValue("uWrapY", 0);
    setUniformIVec2(ctx.gl, ctx.computeDrawProgram, "uDispatchOrigin", regionMinX, regionMinY);
    ctx.gl->glDispatchCompute(groupsX, groupsY, 1);
    // maskTexへのimageStore→render.fragのtexture()フェッチの可視性を保証する
    // (PenEraserTool::dispatchStampBatchの同名バリアと同じ理由)。
    ctx.gl->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    ctx.computeDrawProgram->release();

    // 3. タイル -> fullLayerTex(このスタンプが触れるタイル範囲だけ)
    GLuint srcFbo = 0, dstFbo = 0;
    ctx.gl->glGenFramebuffers(1, &srcFbo);
    ctx.gl->glGenFramebuffers(1, &dstFbo);
    ctx.gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    ctx.gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.fullLayerTex, 0);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
    forEachLayerTileInRange(layer, ctx.canvasW, ctx.canvasH, tileSize,
        b.wTxMin, b.wTxMax, b.wTyMin, b.wTyMax,
        [&](int, int, int si, int dstX, int dstY, int w, int h) {
            ctx.gl->glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
            ctx.gl->glBlitFramebuffer(0, 0, w, h, dstX, dstY, dstX + w, dstY + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }, maskMode);
    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, ctx.defaultFbo());

    // 4. bake(このスタンプの影響範囲だけ)
    ctx.gl->glBindImageTexture(0, ctx.maskTex,      0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);
    ctx.gl->glBindImageTexture(1, ctx.fullLayerTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
    ctx.gl->glBindImageTexture(2, ctx.selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    ctx.computeBakeProgram->bind();
    ctx.computeBakeProgram->setUniformValue("uBrushColor", col.redF(), col.greenF(), col.blueF(), col.alphaF());
    // マスク編集中は色(濃淡)を塗るので常に通常モード(PenEraserToolと同じ)。
    ctx.computeBakeProgram->setUniformValue("uEraseMode", (!maskMode && erase.active) ? 1 : 0);
    // 合成モード・スタンプごとの色はペン専用の設定。プログラムを共有しているので
    // 明示的に既定へ戻す。
    ctx.computeBakeProgram->setUniformValue("uBrushBlendMode", 0);
    ctx.computeBakeProgram->setUniformValue("uUseStrokeColor", 0);
    setUniformIVec2(ctx.gl, ctx.computeBakeProgram, "uDispatchOrigin", regionMinX, regionMinY);
    ctx.gl->glDispatchCompute(groupsX, groupsY, 1);
    ctx.gl->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    ctx.computeBakeProgram->release();

    // 5. fullLayerTex -> タイルへ書き戻す(同じ範囲だけ)
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
    ctx.gl->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.fullLayerTex, 0);
    ctx.gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    forEachLayerTileInRange(layer, ctx.canvasW, ctx.canvasH, tileSize,
        b.wTxMin, b.wTxMax, b.wTyMin, b.wTyMax,
        [&](int, int, int si, int srcX, int srcY, int w, int h) {
            ctx.gl->glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
            ctx.gl->glBlitFramebuffer(srcX, srcY, srcX + w, srcY + h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }, maskMode);
    // 【注意】ctx.defaultFbo()は0固定。この関数はflushPendingInput()経由で
    // CanvasWidget::paintGL()の中からも呼ばれるため、ここで0に外したままだとその
    // フレームの描画がフレームバッファ0へ行って捨てられる。paintGL()側が
    // 入り口でバインドを控えて戻している(CanvasWidget::paintGL()のprevDrawFbo参照)。
    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, ctx.defaultFbo());
    ctx.gl->glDeleteFramebuffers(1, &srcFbo);
    ctx.gl->glDeleteFramebuffers(1, &dstFbo);

    ctx.gl->glMemoryBarrier(GL_ALL_BARRIER_BITS);

    // 6. マスクをこの範囲だけ再クリアする。render.frag は毎フレーム maskTex を
    // サンプリングして、アクティブレイヤーに「未確定ストロークのライブプレビュー」
    // (現在のブラシ色で即座に上書き表示するだけの見た目上のオーバーレイ。実データは
    // 焼き込み時にしか変わらない)を重ねて描画している。焼き込み後もここをクリア
    // しないままだと、実際のレイヤーには正しく焼き込まれているのに、直前のスタンプの
    // 形がプレビュー用の残像として画面に残り続けてしまう(Undoで消えない・現在の
    // ブラシ色に追従して変化する・タイル境界で妙に途切れる、といった症状の原因)。
    ctx.gl->glBindImageTexture(0, ctx.maskTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    ctx.computeMaskClearProgram->bind();
    setUniformIVec2(ctx.gl, ctx.computeMaskClearProgram, "uDispatchOrigin", regionMinX, regionMinY);
    ctx.gl->glDispatchCompute(groupsX, groupsY, 1);
    ctx.gl->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    ctx.computeMaskClearProgram->release();
    ctx.gl->glBindImageTexture(0, ctx.maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);
}

// ===========================================================================
void AirbrushTool::onMousePress(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    // PenEraserTool::onMousePressと同じ理由(ペンタブのOS互換レイヤーが同じ接触に
    // 対して重複したマウスダウンを送ってくることがある)で、ストローク中の
    // 再入は無視する。
    if (isDrawing_) return;

    ctx.beginStrokeUndo();
    isDrawing_ = true;
    pendingStamps_.clear(); // 前回ストロークの取りこぼしがあれば念のため捨てる

    drawHead_        = ctx.widgetToPixel(event->position());
    lastMousePos_    = drawHead_;
    strokeDistCarry_ = 0.0f;

    const int size = toolCfg_->airbrush().size();
    const float radius = pressureRadius(size);
    const float stampAlpha = pressureStampAlpha();

    // フレームレート律速バッチ: 即座にstampAndBake()を呼ばず貯めておくだけにする
    // (実際の処理はflushPendingInput()、CanvasWidgetの定期タイマーが呼ぶ)。
    pendingStamps_.append({ drawHead_, radius, stampAlpha });

    ctx.requestRepaint();
}

void AirbrushTool::onMouseMove(QMouseEvent *event, ToolContext &ctx)
{
    if (!isDrawing_) return;

    const int size = toolCfg_->airbrush().size();
    const float radius = pressureRadius(size);
    const float stampAlpha = pressureStampAlpha();
    const float smoothing = toolCfg_->airbrush().smoothing();

    const QVector2D penTip = ctx.widgetToPixel(event->position());
    // drawHead をペン先に引き寄せる(手振れ補正)。筆圧同様、実イベントごとに
    // 正しい順序でここを計算する(フレームレート律速で遅延させるのはこの後の
    // stampAndBake呼び出しだけなので、手振れ補正やスタンプ間隔の精度は落ちない)。
    drawHead_ += (penTip - drawHead_) * smoothing;

    const float spacingRatio = toolCfg_->airbrush().spacing();
    const float spacingPx = qMax(1.0f, (float)size * spacingRatio);

    // PenEraserTool::onMouseMoveと同じ、距離ベースのスタンプ方式。差分は、各スタンプを
    // まとめてマスクへ積み上げるのではなく、1つずつ即座に焼き込む点(stampAndBake)。
    // ここでは焼き込み自体はflushPendingInput()まで遅延させ、貯めるだけにする。
    const QVector2D segVec = drawHead_ - lastMousePos_;
    const float segLen = segVec.length();
    if (segLen > 0.0001f) {
        const QVector2D dir = segVec / segLen;
        constexpr int kMaxStampsPerEvent = 1000;
        int stampCount = 0;
        for (float t = spacingPx - strokeDistCarry_; t <= segLen; t += spacingPx) {
            pendingStamps_.append({ lastMousePos_ + dir * t, radius, stampAlpha });
            if (++stampCount >= kMaxStampsPerEvent) break;
        }
        strokeDistCarry_ = std::fmod(strokeDistCarry_ + segLen, spacingPx);
    }

    lastMousePos_ = drawHead_;
    ctx.requestRepaint();
}

void AirbrushTool::flushPendingInput(ToolContext &ctx)
{
    if (pendingStamps_.isEmpty()) return;
    const float hardness = toolCfg_->airbrush().hardness();
    for (const PendingStamp &s : pendingStamps_)
        stampAndBake(ctx, s.pos, s.radius, hardness, s.alpha);
    pendingStamps_.clear();
}

int AirbrushTool::flushIntervalMs() const
{
    // PenEraserTool::flushIntervalMs()と同じ方針(細い=即flushで追従性最優先、
    // 太い=間隔を空けてdispatch回数を抑える)。
    const int size = toolCfg_->airbrush().size();
    return qBound(0, (size - 32) / 6, 16);
}

void AirbrushTool::onMouseRelease(QMouseEvent *event, ToolContext &ctx)
{
    if (event->button() != Qt::LeftButton) return;
    if (!isDrawing_) return;
    isDrawing_ = false;

    // フレームレート律速バッチ: 直前のonMouseMove()で貯めたまま、まだ
    // 一度もflushPendingInput()が呼ばれていないスタンプが残っている可能性がある
    // ため、確定前に流し切る。
    flushPendingInput(ctx);

    ctx.commitStrokeUndo();
    ctx.requestRepaint();
    ctx.notifyLayersChanged();
}
