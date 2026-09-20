#include "rendering/CanvasCompositor.h"
#include "tools/core/ToneCurveMath.h"
#include <algorithm>

// 小さな2Dテクスチャを作るための内部ヘルパー。
static GLuint makeTexture2D(QOpenGLFunctions_4_3_Core *gl, GLenum internalFormat,
                            int w, int h, GLenum filter = GL_NEAREST)
{
    GLuint tex = 0;
    gl->glGenTextures(1, &tex);
    gl->glBindTexture(GL_TEXTURE_2D, tex);
    gl->glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, w, h);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    return tex;
}

// QOpenGLShaderProgram::setUniformValue(name, QPoint) はこの環境では ivec2 uniform に対して値が反映されないため(型解決の問題)、
// ivec2 はglUniform2iで直接設定する。
static void setUniformIVec2(QOpenGLFunctions_4_3_Core *gl, QOpenGLShaderProgram *prog,
                             const char *name, int x, int y)
{
    GLint loc = gl->glGetUniformLocation(prog->programId(), name);
    if (loc >= 0)
        gl->glUniform2i(loc, x, y);
}

// 調整レイヤー(ToneCurve/GradientMap)のLUTを、確保済みのタイル配列スライス(1枚、256x256)のy=0行(256エントリ)へ書き込む。
static void uploadAdjustmentLut(ToolContext &ctx, int slice, const quint8 *lutRgba /* [256*4] */)
{
    ctx.gl->glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.bankTexOf(slice));
    ctx.gl->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, ctx.localSliceOf(slice),
                             256, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, lutRgba);
    ctx.gl->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

// GradientMapTool::rebuildLut()と全く同じ補間(ストップ間の素直な線形補間、端の外側は端の色でクランプ)で256段階(輝度0-255入力)のLUTを構築する。
static void buildGradientMapLut256(const QVector<AdjustmentGradientStop> &stops, quint8 *outLut /* [256*4] */)
{
    const int n = stops.size();
    for (int i = 0; i < 256; i++) {
        const float pos = float(i) / 255.0f;

        QColor c;
        if (pos <= stops.front().pos) {
            c = stops.front().color;
        } else if (pos >= stops.back().pos) {
            c = stops.back().color;
        } else {
            c = stops.back().color;
            for (int k = 0; k + 1 < n; k++) {
                const AdjustmentGradientStop &a = stops[k], &b = stops[k + 1];
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

        outLut[i * 4 + 0] = (quint8)std::clamp(c.red(),   0, 255);
        outLut[i * 4 + 1] = (quint8)std::clamp(c.green(), 0, 255);
        outLut[i * 4 + 2] = (quint8)std::clamp(c.blue(),  0, 255);
        outLut[i * 4 + 3] = 255;
    }
}

// タイル走査の共通化。
void CanvasCompositor::forEachCanvasTile(const CanvasDocument &doc, const TileVisitFn &fn)
{
    for (int ty = 0; ty < doc.tilesY(); ty++)
        for (int tx = 0; tx < doc.tilesX(); tx++)
            fn(tx, ty);
}

// レイヤーuniform / SSBO。
void CanvasCompositor::setLayerUniforms(ToolContext &ctx, QOpenGLShaderProgram *prog, int tx, int ty)
{
    CanvasDocument &doc = *ctx.doc;
    const int tileSize = ctx.tileSize;

    prog->setUniformValue("uLayerCount", (int)doc.layers.size());
    prog->setUniformValue("uCanvasTilesX", doc.tilesX()); // レイヤーマスクのタイル参照(常にキャンバス全体)用
    setUniformIVec2(ctx.gl, prog, "uTileOffset", tx * tileSize, ty * tileSize);

    // タイル配列(旧 layerTexArray)は複数バンクへ分割されたので、全バンクをユニット LAYER_BANK_TEXUNIT_BASE..
    GLint bankUnits[MAX_TILE_BANKS];
    for (int i = 0; i < MAX_TILE_BANKS; i++) {
        ctx.gl->glActiveTexture(GL_TEXTURE0 + LAYER_BANK_TEXUNIT_BASE + i);
        ctx.gl->glBindTexture(GL_TEXTURE_2D_ARRAY,
                              ctx.layerTexBanks[i] ? ctx.layerTexBanks[i] : ctx.layerTexBanks[0]);
        bankUnits[i] = LAYER_BANK_TEXUNIT_BASE + i;
    }
    prog->setUniformValueArray("layerTexBanks", bankUnits, MAX_TILE_BANKS);
    prog->setUniformValue("uSlicesPerBank", ctx.slicesPerBank);
    ctx.gl->glActiveTexture(GL_TEXTURE0);
}

void CanvasCompositor::setLayerUniformsForRender(ToolContext &ctx, QOpenGLShaderProgram *prog)
{
    CanvasDocument &doc = *ctx.doc;
    prog->setUniformValue("uLayerCount", (int)doc.layers.size());
    updateLayerSSBOs(ctx);
}

void CanvasCompositor::updateLayerSSBOs(ToolContext &ctx)
{
    CanvasDocument &doc = *ctx.doc;
    const int maxLayers = ctx.maxLayers;
    const int n = qMin(doc.layers.size(), maxLayers);

    // フォルダー(祖先)ぶんの表示・不透明度・マスクをレイヤーへカスケードさせるための一覧(外側→内側の順、各要素は祖先フォルダーのlayers index)。
    const QVector<QVector<int>> ancestorsPerLayer = doc.computeAncestorFolders();
    static constexpr int MAX_ANCESTOR_MASKS = 4; // レイヤーあたりカスケードするフォルダーマスクの最大階層数

    // 配列/アップロードは maxLayers(=MAX_LAYERS、上限8192)ではなく実レイヤー数 n 個ぶんだけ確保・転送する。
    const int nn = qMax(0, n);
    QVector<float> opacity(nn, 1.0f);
    QVector<int>   visible(nn, 1);
    QVector<int>   baseSlice(nn, -1);
    QVector<int>   maskBaseSlice(nn, -1);
    QVector<int>   ancestorMaskSlices(nn * MAX_ANCESTOR_MASKS, -1); // ivec4配列(std430)
    QVector<int>   blendMode(nn, 0);
    QVector<int>   clipping(nn, 0);
    QVector<int>   originTx(nn, 0);
    QVector<int>   originTy(nn, 0);
    QVector<int>   tilesX(nn, 0);
    QVector<int>   tilesY(nn, 0);
    QVector<int>   isSolidColor(nn, 0);
    QVector<float> solidColor(nn * 4, 1.0f); // vec4配列(std430で要素ストライド16バイト)
    QVector<int>   adjKind(nn, 0); // 0=調整レイヤーでない, 1=明るさ・コントラスト, 2=色相・彩度・明度
    QVector<float> adjParams(nn * 4, 0.0f); // vec3配列(std430で要素ストライド16バイト=float4)

    for (int z = 0; z < n; z++) {
        const Layer &layer = doc.layers[z];

        // フォルダー単位の表示・不透明度・マスクをこのレイヤーへ乗算でカスケードする(Photoshopの「通過(パススルー)」グループと同じ考え方: グループ自身はブレンドモードに影響しないが、表示/不透明度/マスクは中身へそのまま掛かる)。
        float effOpacity = layer.opacity;
        bool  effVisible = layer.visible;
        int   maskSlot = 0;
        const QVector<int> &ancestors = ancestorsPerLayer[z];
        for (int f : ancestors) {
            if (f < 0 || f >= doc.layers.size()) continue;
            const Layer &folder = doc.layers[f];
            effOpacity *= folder.opacity;
            effVisible = effVisible && folder.visible;
            // hasMaskが立っているのにmaskTilesが空、という不整合はここで弾く。
            if (folder.hasMask && !folder.maskTiles.isEmpty() && maskSlot < MAX_ANCESTOR_MASKS) {
                ancestorMaskSlices[z * MAX_ANCESTOR_MASKS + maskSlot] = folder.maskTiles[0][0];
                maskSlot++;
            }
        }

        opacity[z]   = effOpacity;
        visible[z]   = effVisible ? 1 : 0;
        baseSlice[z] = layer.tiles.isEmpty() ? -1 : layer.tiles[0][0];
        maskBaseSlice[z] = (layer.hasMask && !layer.maskTiles.isEmpty()) ? layer.maskTiles[0][0] : -1;
        blendMode[z] = (int)layer.blendMode;
        clipping[z]  = layer.clipping ? 1 : 0;
        originTx[z]  = layer.originTx;
        originTy[z]  = layer.originTy;
        tilesX[z]    = layer.tilesX();
        tilesY[z]    = layer.tilesY();
        isSolidColor[z] = (layer.layerType == LayerType::SolidColor) ? 1 : 0;
        if (layer.layerType == LayerType::SolidColor) {
            solidColor[z * 4 + 0] = (float)layer.solidColor.redF();
            solidColor[z * 4 + 1] = (float)layer.solidColor.greenF();
            solidColor[z * 4 + 2] = (float)layer.solidColor.blueF();
            solidColor[z * 4 + 3] = (float)layer.solidColor.alphaF();
        }

        // フィルターレイヤーは合成ループでは何もしない印(3)を立てるだけ。
        if (layer.layerType == LayerType::Filter)
            adjKind[z] = 3;

        if (layer.layerType == LayerType::Adjustment) {
            const AdjustmentKind kind = layer.adjustment.kind;
            const bool needsLut = (kind == AdjustmentKind::ToneCurve || kind == AdjustmentKind::GradientMap);

            // 種別(kind)がLUT不要なものへ変わっていたら、ここで気づいたタイミングで確保済みのLUTスライスを解放する(セルフヒーリング。CanvasDocument.h Layer::lutSliceのコメント参照)。
            if (!needsLut && layer.lutSlice >= 0)
                doc.freeAdjustmentLutSlice(z);

            // BrightnessContrastTool/HueSatLightTool/ColorBalanceTool(いずれも破壊的な編集アクション)と同じ単位変換で揃える(見た目が一致するように)。
            if (kind == AdjustmentKind::BrightnessContrast) {
                adjKind[z] = 1;
                adjParams[z * 4 + 0] = layer.adjustment.brightness / 200.0f;
                adjParams[z * 4 + 1] = 1.0f + layer.adjustment.contrast / 100.0f;
                adjParams[z * 4 + 2] = 0.0f;
            } else if (kind == AdjustmentKind::HueSaturation) {
                adjKind[z] = 2;
                adjParams[z * 4 + 0] = (float)layer.adjustment.hue;
                adjParams[z * 4 + 1] = layer.adjustment.saturation / 100.0f;
                adjParams[z * 4 + 2] = layer.adjustment.lightness / 100.0f;
            } else if (kind == AdjustmentKind::ColorBalance) {
                adjKind[z] = 4;
                adjParams[z * 4 + 0] = layer.adjustment.cyan    / 100.0f;
                adjParams[z * 4 + 1] = layer.adjustment.magenta / 100.0f;
                adjParams[z * 4 + 2] = layer.adjustment.yellow  / 100.0f;
            } else if (kind == AdjustmentKind::ToneCurve) {
                adjKind[z] = 5;
                // LUTスライスは遅延確保(初回のみ実際に確保が走る)、中身は毎フレーム現在のcurvePointsから再構築する(パネルでの編集をそのまま反映するため)。
                if (doc.ensureAdjustmentLutSlice(z)) {
                    quint8 lut[256];
                    ToneCurveMath::buildLut256(layer.adjustment.curvePoints, lut);
                    quint8 lutRgba[256 * 4];
                    for (int i = 0; i < 256; i++) {
                        lutRgba[i * 4 + 0] = lut[i];
                        lutRgba[i * 4 + 1] = lut[i];
                        lutRgba[i * 4 + 2] = lut[i];
                        lutRgba[i * 4 + 3] = 255;
                    }
                    uploadAdjustmentLut(ctx, layer.lutSlice, lutRgba);
                    baseSlice[z] = layer.lutSlice;
                }
            } else if (kind == AdjustmentKind::GradientMap) {
                adjKind[z] = 6;
                if (doc.ensureAdjustmentLutSlice(z)) {
                    quint8 lutRgba[256 * 4];
                    buildGradientMapLut256(layer.adjustment.gradientStops, lutRgba);
                    uploadAdjustmentLut(ctx, layer.lutSlice, lutRgba);
                    baseSlice[z] = layer.lutSlice;
                }
            }
        }
    }

    // アップロードは実レイヤー数 nn 個ぶんだけ(上のQVector確保コメント参照)。
    if (nn > 0) {
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerOpacity);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * sizeof(float), opacity.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerVisible);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * sizeof(int), visible.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerBaseSlice);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * sizeof(int), baseSlice.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerMaskBaseSlice);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * sizeof(int), maskBaseSlice.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerAncestorMaskSlices);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * MAX_ANCESTOR_MASKS * sizeof(int), ancestorMaskSlices.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerBlendMode);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * sizeof(int), blendMode.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerClipping);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * sizeof(int), clipping.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerOriginTx);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * sizeof(int), originTx.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerOriginTy);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * sizeof(int), originTy.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerTilesX);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * sizeof(int), tilesX.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerTilesY);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * sizeof(int), tilesY.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerIsSolidColor);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * sizeof(int), isSolidColor.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerSolidColor);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * 4 * sizeof(float), solidColor.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerAdjKind);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * sizeof(int), adjKind.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx.ssboLayerAdjParams);
        ctx.gl->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nn * 4 * sizeof(float), adjParams.constData());
        ctx.gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, ctx.ssboLayerOpacity);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, ctx.ssboLayerVisible);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, ctx.ssboLayerBaseSlice);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ctx.ssboLayerMaskBaseSlice);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ctx.ssboLayerAncestorMaskSlices);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, ctx.ssboLayerBlendMode);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, ctx.ssboLayerClipping);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9,  ctx.ssboLayerOriginTx);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, ctx.ssboLayerOriginTy);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 11, ctx.ssboLayerTilesX);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 12, ctx.ssboLayerTilesY);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 13, ctx.ssboLayerIsSolidColor);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, ctx.ssboLayerAdjKind);
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, ctx.ssboLayerAdjParams);
    // binding 0はcomposite.comp/render.frag側でテクスチャ/画像用に使われておらず(SSBOとテクスチャ/画像はバインディング名前空間が別)、4〜15が埋まっているため、
    // 単色レイヤーの色だけこの空きスロットに割り当てる。
    ctx.gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ctx.ssboLayerSolidColor);
}

// ナビゲーター用: 全レイヤー合成。
void CanvasCompositor::updateCompositedTex(ToolContext &ctx)
{
    CanvasDocument &doc = *ctx.doc;
    if (doc.layers.isEmpty()) return;

    const int tileSize = ctx.tileSize;

    // タイル配列(全バンク)は setLayerUniforms() 内でユニット8..へバインドする(旧: unit1へ単一配列)。

    // compositedTileArr は initTextures() で事前に確保されている前提。
    Q_ASSERT(ctx.compositedTileArr != 0);

    updateLayerSSBOs(ctx);

    QOpenGLShaderProgram *prog = ctx.computeCompositeProgram;
    prog->bind();
    forEachCanvasTile(doc, [&](int tx, int ty) {
        int tileSlice = ty * doc.tilesX() + tx;
        ctx.gl->glBindImageTexture(2, ctx.compositedTileArr, 0, GL_FALSE,
                                   tileSlice, GL_WRITE_ONLY, GL_RGBA8);
        setLayerUniforms(ctx, prog, tx, ty);
        prog->setUniformValue("uTargetStart", 0);
        prog->setUniformValue("uTargetEnd", (int)doc.layers.size() - 1);
        ctx.gl->glDispatchCompute((tileSize+15)/16, (tileSize+15)/16, 1);
        ctx.gl->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    });
    prog->release();

    GLuint srcFbo = 0, dstFbo = 0;
    ctx.gl->glGenFramebuffers(1, &srcFbo);
    ctx.gl->glGenFramebuffers(1, &dstFbo);
    ctx.gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    ctx.gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, ctx.compositedTex, 0);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);

    forEachCanvasTile(doc, [&](int tx, int ty) {
        int tileSlice = ty * doc.tilesX() + tx;
        int dstX = tx * tileSize;
        int dstY = ty * tileSize;
        int w = qMin(tileSize, ctx.canvasW - dstX);
        int h = qMin(tileSize, ctx.canvasH - dstY);
        ctx.gl->glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          ctx.compositedTileArr, 0, tileSlice);
        ctx.gl->glBlitFramebuffer(0, 0, w, h,
                                  dstX, dstY, dstX + w, dstY + h,
                                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
    });

    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, ctx.defaultFbo());
    ctx.gl->glDeleteFramebuffers(1, &srcFbo);
    ctx.gl->glDeleteFramebuffers(1, &dstFbo);

    ctx.gl->glBindTexture(GL_TEXTURE_2D, ctx.compositedTex);
    ctx.gl->glGenerateMipmap(GL_TEXTURE_2D);
    ctx.gl->glBindTexture(GL_TEXTURE_2D, 0);
}

// プレビュー / 書き出し。
QImage CanvasCompositor::renderLayerPreview(ToolContext &ctx, int zStart, int zEnd, int size)
{
    CanvasDocument &doc = *ctx.doc;
    const int tileSize = ctx.tileSize;

    QImage blank(size, size, QImage::Format_RGBA8888);
    blank.fill(Qt::transparent);

    int count = doc.layers.size();
    if (zStart < 0 || zStart >= count) return blank;
    zEnd = qBound(zStart, zEnd, count - 1);

    // 単一レイヤーのプレビュー(呼び出し元は現状すべてこれ)は、他レイヤーとの合成が絡まないため軽量パスで済ませる(下記参照)。
    if (zStart == zEnd)
        return renderSingleLayerPreviewLight(ctx, zStart, size);

    updateLayerSSBOs(ctx);

    int tileCount = doc.tilesX() * doc.tilesY();
    GLuint tmpTileArr = 0;
    ctx.gl->glGenTextures(1, &tmpTileArr);
    ctx.gl->glBindTexture(GL_TEXTURE_2D_ARRAY, tmpTileArr);
    ctx.gl->glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, tileSize, tileSize, tileCount);
    ctx.gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    ctx.gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // タイル配列(全バンク)は setLayerUniforms() 内でユニット8..へバインドする(旧: unit1へ単一配列)。

    QOpenGLShaderProgram *prog = ctx.computeCompositeProgram;
    prog->bind();
    forEachCanvasTile(doc, [&](int tx, int ty) {
        int tileSlice = ty * doc.tilesX() + tx;
        ctx.gl->glBindImageTexture(2, tmpTileArr, 0, GL_FALSE, tileSlice, GL_WRITE_ONLY, GL_RGBA8);
        setLayerUniforms(ctx, prog, tx, ty);
        prog->setUniformValue("uTargetStart", zStart);
        prog->setUniformValue("uTargetEnd",   zEnd);
        ctx.gl->glDispatchCompute((tileSize+15)/16, (tileSize+15)/16, 1);
        ctx.gl->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    });
    prog->release();

    GLuint tmpTex = makeTexture2D(ctx.gl, GL_RGBA8, ctx.canvasW, ctx.canvasH);

    GLuint srcFbo = 0, dstFbo = 0;
    ctx.gl->glGenFramebuffers(1, &srcFbo);
    ctx.gl->glGenFramebuffers(1, &dstFbo);
    ctx.gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    ctx.gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, tmpTex, 0);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);

    forEachCanvasTile(doc, [&](int tx, int ty) {
        int tileSlice = ty * doc.tilesX() + tx;
        int dstX = tx * tileSize, dstY = ty * tileSize;
        int w = qMin(tileSize, ctx.canvasW - dstX);
        int h = qMin(tileSize, ctx.canvasH - dstY);
        ctx.gl->glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          tmpTileArr, 0, tileSlice);
        ctx.gl->glBlitFramebuffer(0, 0, w, h,
                                  dstX, dstY, dstX+w, dstY+h,
                                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
    });

    GLuint dstTex = makeTexture2D(ctx.gl, GL_RGBA8, size, size);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
    ctx.gl->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, tmpTex, 0);
    ctx.gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    ctx.gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, dstTex, 0);
    ctx.gl->glBlitFramebuffer(0, 0, ctx.canvasW, ctx.canvasH, 0, 0, size, size,
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);

    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, dstFbo);
    QVector<uint8_t> buf(size * size * 4);
    ctx.gl->glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());

    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, ctx.defaultFbo());
    ctx.gl->glDeleteFramebuffers(1, &srcFbo);
    ctx.gl->glDeleteFramebuffers(1, &dstFbo);
    ctx.gl->glDeleteTextures(1, &tmpTileArr);
    ctx.gl->glDeleteTextures(1, &tmpTex);
    ctx.gl->glDeleteTextures(1, &dstTex);

    // layerTexArray/合成結果は事前乗算アルファで格納されているため、まずFormat_RGBA8888_Premultipliedとして正しくタグ付けしてからconvertToFormatで非事前乗算(Format_RGBA8888)
    // へ変換する。
    QImage img(buf.constData(), size, size, size * 4, QImage::Format_RGBA8888_Premultiplied);
    return img.copy().convertToFormat(QImage::Format_RGBA8888).mirrored(false, true);
}

QImage CanvasCompositor::renderNavigatorPreview(ToolContext &ctx, int size, bool fullCanvas)
{
    CanvasDocument &doc = *ctx.doc;
    const int tileSize = ctx.tileSize;

    if (fullCanvas)
        updateCompositedTex(ctx);

    float scaleX = (float)size / ctx.canvasW;
    float scaleY = (float)size / ctx.canvasH;
    float scale  = qMin(scaleX, scaleY);
    int dstW = qMax(1, (int)(ctx.canvasW * scale));
    int dstH = qMax(1, (int)(ctx.canvasH * scale));

    GLuint dstTex = makeTexture2D(ctx.gl, GL_RGBA8, dstW, dstH);

    GLuint srcFbo = 0, dstFbo = 0;
    ctx.gl->glGenFramebuffers(1, &srcFbo);
    ctx.gl->glGenFramebuffers(1, &dstFbo);

    ctx.gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    ctx.gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, dstTex, 0);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);

    if (fullCanvas) {
        ctx.gl->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, ctx.compositedTex, 0);
        ctx.gl->glBlitFramebuffer(0, 0, ctx.canvasW, ctx.canvasH,
                                  0, 0, dstW, dstH,
                                  GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else {
        if (doc.layers.isEmpty()) {
            ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, ctx.defaultFbo());
            ctx.gl->glDeleteFramebuffers(1, &srcFbo);
            ctx.gl->glDeleteFramebuffers(1, &dstFbo);
            ctx.gl->glDeleteTextures(1, &dstTex);
            QImage blank(dstW, dstH, QImage::Format_RGBA8888);
            blank.fill(Qt::transparent);
            return blank;
        }
        GLuint midTex = makeTexture2D(ctx.gl, GL_RGBA8, ctx.canvasW, ctx.canvasH);
        GLuint midFbo = 0;
        ctx.gl->glGenFramebuffers(1, &midFbo);

        ctx.gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, midFbo);
        ctx.gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, midTex, 0);
        ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);

        const Layer &layer = doc.activeLayer();
        if (layer.layerType == LayerType::SolidColor) {
            // 単色レイヤーはタイルを持たないので、そのレイヤーの色で塗りつぶすだけでよい。
            ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, midFbo);
            ctx.gl->glClearColor((float)layer.solidColor.redF(), (float)layer.solidColor.greenF(),
                                  (float)layer.solidColor.blueF(), (float)layer.solidColor.alphaF());
            ctx.gl->glClear(GL_COLOR_BUFFER_BIT);
        } else {
            forEachCanvasTile(doc, [&](int tx, int ty) {
                int si = layer.tileSliceAtCanvasTile(tx, ty);
                if (si < 0) return; // 通常起こらない(レイヤーは常にキャンバス全体を覆う)
                int dstX = tx * tileSize, dstY = ty * tileSize;
                int w = qMin(tileSize, ctx.canvasW - dstX);
                int h = qMin(tileSize, ctx.canvasH - dstY);
                ctx.gl->glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                  ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                ctx.gl->glBlitFramebuffer(0, 0, w, h,
                                          dstX, dstY, dstX+w, dstY+h,
                                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
            });
        }

        ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, midFbo);
        ctx.gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        ctx.gl->glBlitFramebuffer(0, 0, ctx.canvasW, ctx.canvasH,
                                  0, 0, dstW, dstH,
                                  GL_COLOR_BUFFER_BIT, GL_LINEAR);

        ctx.gl->glDeleteFramebuffers(1, &midFbo);
        ctx.gl->glDeleteTextures(1, &midTex);
    }

    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, dstFbo);
    QVector<uint8_t> buf(dstW * dstH * 4);
    ctx.gl->glReadPixels(0, 0, dstW, dstH, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());

    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, ctx.defaultFbo());
    ctx.gl->glDeleteFramebuffers(1, &srcFbo);
    ctx.gl->glDeleteFramebuffers(1, &dstFbo);
    ctx.gl->glDeleteTextures(1, &dstTex);

    QImage img(buf.constData(), dstW, dstH, dstW * 4, QImage::Format_RGBA8888_Premultiplied);
    return img.copy().convertToFormat(QImage::Format_RGBA8888).mirrored(false, true);
}

QImage CanvasCompositor::renderMaskPreview(ToolContext &ctx, int layerIndex, int size)
{
    CanvasDocument &doc = *ctx.doc;
    const int tileSize = ctx.tileSize;
    if (layerIndex < 0 || layerIndex >= doc.layers.size()) return QImage();
    const Layer &layer = doc.layers[layerIndex];
    if (!layer.hasMask) return QImage();

    float scale = qMin((float)size / ctx.canvasW, (float)size / ctx.canvasH);
    int dstW = qMax(1, (int)(ctx.canvasW * scale));
    int dstH = qMax(1, (int)(ctx.canvasH * scale));

    // マスクタイル(キャンバス全体を覆う連続スライス)を一旦キャンバスサイズのFBOへ並べ、そこから縮小してdstTexへブリットする(renderNavigatorPreviewの単体レイヤーパスと同じ手順。マスクはR=G=B=濃度・A=255な
    // ので結果はそのままグレースケール)。
    GLuint dstTex = makeTexture2D(ctx.gl, GL_RGBA8, dstW, dstH);
    GLuint midTex = makeTexture2D(ctx.gl, GL_RGBA8, ctx.canvasW, ctx.canvasH);
    GLuint srcFbo = 0, midFbo = 0, dstFbo = 0;
    ctx.gl->glGenFramebuffers(1, &srcFbo);
    ctx.gl->glGenFramebuffers(1, &midFbo);
    ctx.gl->glGenFramebuffers(1, &dstFbo);

    ctx.gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, midFbo);
    ctx.gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, midTex, 0);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
    forEachCanvasTile(doc, [&](int tx, int ty) {
        int si = layer.maskTileSlice(tx, ty);
        if (si < 0) return;
        int dstX = tx * tileSize, dstY = ty * tileSize;
        int w = qMin(tileSize, ctx.canvasW - dstX);
        int h = qMin(tileSize, ctx.canvasH - dstY);
        ctx.gl->glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
        ctx.gl->glBlitFramebuffer(0, 0, w, h, dstX, dstY, dstX+w, dstY+h,
                                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
    });

    ctx.gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    ctx.gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, dstTex, 0);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, midFbo);
    ctx.gl->glBlitFramebuffer(0, 0, ctx.canvasW, ctx.canvasH, 0, 0, dstW, dstH,
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);

    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, dstFbo);
    QVector<uint8_t> buf(dstW * dstH * 4);
    ctx.gl->glReadPixels(0, 0, dstW, dstH, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());

    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, ctx.defaultFbo());
    ctx.gl->glDeleteFramebuffers(1, &srcFbo);
    ctx.gl->glDeleteFramebuffers(1, &midFbo);
    ctx.gl->glDeleteFramebuffers(1, &dstFbo);
    ctx.gl->glDeleteTextures(1, &dstTex);
    ctx.gl->glDeleteTextures(1, &midTex);

    QImage img(buf.constData(), dstW, dstH, dstW * 4, QImage::Format_RGBA8888);
    return img.copy().mirrored(false, true);
}

QImage CanvasCompositor::renderExport(ToolContext &ctx)
{
    CanvasDocument &doc = *ctx.doc;
    const int tileSize = ctx.tileSize;

    updateLayerSSBOs(ctx);

    int tileCount = doc.tilesX() * doc.tilesY();
    GLuint tmpTileArr = 0;
    ctx.gl->glGenTextures(1, &tmpTileArr);
    ctx.gl->glBindTexture(GL_TEXTURE_2D_ARRAY, tmpTileArr);
    ctx.gl->glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, tileSize, tileSize, tileCount);
    ctx.gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    ctx.gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // タイル配列(全バンク)は setLayerUniforms() 内でユニット8..へバインドする(旧: unit1へ単一配列)。

    QOpenGLShaderProgram *prog = ctx.computeCompositeProgram;
    prog->bind();
    forEachCanvasTile(doc, [&](int tx, int ty) {
        int tileSlice = ty * doc.tilesX() + tx;
        ctx.gl->glBindImageTexture(2, tmpTileArr, 0, GL_FALSE, tileSlice, GL_WRITE_ONLY, GL_RGBA8);
        setLayerUniforms(ctx, prog, tx, ty);
        prog->setUniformValue("uTargetStart", 0);
        prog->setUniformValue("uTargetEnd", (int)doc.layers.size() - 1);
        ctx.gl->glDispatchCompute((tileSize+15)/16, (tileSize+15)/16, 1);
        ctx.gl->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    });
    prog->release();

    GLuint exportTex = makeTexture2D(ctx.gl, GL_RGBA8, ctx.canvasW, ctx.canvasH);
    GLuint srcFbo = 0, dstFbo = 0;
    ctx.gl->glGenFramebuffers(1, &srcFbo);
    ctx.gl->glGenFramebuffers(1, &dstFbo);
    ctx.gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    ctx.gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, exportTex, 0);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);

    forEachCanvasTile(doc, [&](int tx, int ty) {
        int tileSlice = ty * doc.tilesX() + tx;
        int dstX = tx * tileSize, dstY = ty * tileSize;
        int w = qMin(tileSize, ctx.canvasW - dstX);
        int h = qMin(tileSize, ctx.canvasH - dstY);
        ctx.gl->glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          tmpTileArr, 0, tileSlice);
        ctx.gl->glBlitFramebuffer(0, 0, w, h,
                                  dstX, dstY, dstX+w, dstY+h,
                                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
    });

    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, dstFbo);
    QVector<uint8_t> buf(ctx.canvasW * ctx.canvasH * 4);
    ctx.gl->glReadPixels(0, 0, ctx.canvasW, ctx.canvasH, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());

    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, ctx.defaultFbo());
    ctx.gl->glDeleteFramebuffers(1, &srcFbo);
    ctx.gl->glDeleteFramebuffers(1, &dstFbo);
    ctx.gl->glDeleteTextures(1, &tmpTileArr);
    ctx.gl->glDeleteTextures(1, &exportTex);

    QImage img(buf.constData(), ctx.canvasW, ctx.canvasH,
               ctx.canvasW * 4, QImage::Format_RGBA8888_Premultiplied);
    return img.copy().convertToFormat(QImage::Format_RGBA8888).mirrored(false, true);
}

// 単一レイヤープレビュー(軽量パス)
QImage CanvasCompositor::renderSingleLayerPreviewLight(ToolContext &ctx, int z, int size)
{
    CanvasDocument &doc = *ctx.doc;
    const int tileSize = ctx.tileSize;
    const Layer &layer = doc.layers[z];

    QImage blank(size, size, QImage::Format_RGBA8888);
    blank.fill(Qt::transparent);

    // 単色/調整レイヤー等、実ピクセルを持たないレイヤーは対象外
    if (layer.tiles.isEmpty() || ctx.canvasW <= 0 || ctx.canvasH <= 0) return blank;

    GLuint dstTex = makeTexture2D(ctx.gl, GL_RGBA8, size, size, GL_LINEAR);
    GLuint dstFbo = 0, srcFbo = 0;
    ctx.gl->glGenFramebuffers(1, &dstFbo);
    ctx.gl->glGenFramebuffers(1, &srcFbo);

    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    ctx.gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
    // レイヤーがキャンバスの一部しか覆っていない場合、覆っていない部分は透明のままにする。
    ctx.gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    ctx.gl->glClear(GL_COLOR_BUFFER_BIT);

    ctx.gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);

    // キャンバス座標系(0..canvasW/H)を出力座標系(0..size)へ写す倍率。
    const float scaleX = (float)size / (float)ctx.canvasW;
    const float scaleY = (float)size / (float)ctx.canvasH;

    for (int ty = 0; ty < layer.tilesY(); ty++) {
        for (int tx = 0; tx < layer.tilesX(); tx++) {
            const int si = layer.tiles[ty][tx];
            const int canvasPxX = (layer.originTx + tx) * tileSize;
            const int canvasPxY = (layer.originTy + ty) * tileSize;

            // floor/ceilで隣接タイルとの間に隙間ができないよう外側に広げて丸める(境界での1px程度の重なりは軽量プレビューでは無視できる)。
            const int dstX0 = qFloor(canvasPxX * scaleX);
            const int dstY0 = qFloor(canvasPxY * scaleY);
            const int dstX1 = qCeil((canvasPxX + tileSize) * scaleX);
            const int dstY1 = qCeil((canvasPxY + tileSize) * scaleY);
            if (dstX1 <= dstX0 || dstY1 <= dstY0) continue;

            ctx.gl->glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                              ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
            ctx.gl->glBlitFramebuffer(0, 0, tileSize, tileSize,
                                      dstX0, dstY0, dstX1, dstY1,
                                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
    }

    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, dstFbo);
    QVector<uint8_t> buf(size * size * 4);
    ctx.gl->glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());

    // レイヤーのopacityは合成シェーダーを経由していないのでここでCPU側から適用する。
    if (layer.opacity < 0.999f) {
        const float op = qBound(0.0f, layer.opacity, 1.0f);
        for (uint8_t &b : buf) b = (uint8_t)(b * op + 0.5f);
    }

    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, ctx.defaultFbo());
    ctx.gl->glDeleteFramebuffers(1, &srcFbo);
    ctx.gl->glDeleteFramebuffers(1, &dstFbo);
    ctx.gl->glDeleteTextures(1, &dstTex);

    QImage img(buf.constData(), size, size, size * 4, QImage::Format_RGBA8888_Premultiplied);
    return img.copy().convertToFormat(QImage::Format_RGBA8888).mirrored(false, true);
}
