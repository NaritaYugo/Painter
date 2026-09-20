#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>

#include "canvas/CanvasWidget.h"
#include "app/NativeWindowLog.h" // 診断ログ(TIEPOLO_WINLOG)。初期化時間の計測に使う
#include "rendering/ShaderCache.h"
#include "tools/core/ToolRegistry.h"
#include "tools/core/CursorUtils.h"
#include "tools/core/MaskBrush.h"
#include "dialogs/ProFeatureDialog.h"
#include "components/ThemeColors.h"
#include "actions/AdjustmentActions.h"
#include "actions/FilterActions.h"
#include "actions/MotionBlurAction.h"
#include "actions/TransformActions.h"
#include "actions/CanvasSizeActions.h"
#include "actions/LayerEditActions.h"
#include "actions/FilterLayerEditAction.h"
#ifdef TIEPOLO_PRO_BUILD
#include "licensing/LicenseManager.h"
#include "actions/ChromaticAberrationAction.h"
#include "actions/LensBlurAction.h"
#include "actions/GradientMapAction.h"
#endif
#include <QTimer>
#include <QMouseEvent>
#include <QDebug>
#include <QFile>
#include <QVector3D>
#include <QQueue>
#include <QStack>
#include <QPainter>
#include <QPainterPath>
#include <QBitmap>
#include <QGuiApplication>
#include <QSurfaceFormat>
#include <QWindow>
#include <QClipboard>
#include <QMimeData>
#include <cmath>

// OpenGL resource setup, view transforms, and frame rendering.

namespace {
// render.frag の layout(binding = 16) と一致させる。
constexpr int kStrokeColorTexUnit = 16;
}

void CanvasWidget::initializeGL() {
    // 計測用。
    static int initCount = 0;
    ++initCount;
    QElapsedTimer initTimer;
    initTimer.start();

    initializeOpenGLFunctions();
    glFunctionsReady_ = true;

    // シェーダープログラムはプロセス全体で1組だけ作り、全タブ(CanvasWidget)で共有する。
    computeDrawProgram         = ShaderCache::compute(":/shaders/paint/stroke.comp");
    computeBakeProgram         = ShaderCache::compute(":/shaders/paint/bake.comp");
    computeBrushStateProgram   = ShaderCache::compute(":/shaders/paint/brushState.comp");
    computeMaskClearProgram    = ShaderCache::compute(":/shaders/paint/maskclear.comp");
    computeLayerClearProgram   = ShaderCache::compute(":/shaders/paint/layerclear.comp");
    computeCompositeProgram    = ShaderCache::compute(":/shaders/render/composite.comp");
    computeWallProgram         = ShaderCache::compute(":/shaders/fill/wall.comp");
    computeJfaInitOuterProgram = ShaderCache::compute(":/shaders/fill/jfaInitOuter.comp");
    computeJfaInitInnerProgram = ShaderCache::compute(":/shaders/fill/jfaInitInner.comp");
    computeJfaProgram          = ShaderCache::compute(":/shaders/fill/jfa.comp");
    computeJfaFinalizeProgram  = ShaderCache::compute(":/shaders/fill/jfaFinalize.comp");
    computeBlurProgram         = ShaderCache::compute(":/shaders/paint/blur.comp");
    computeGaussianBlurFilterProgram = ShaderCache::compute(":/shaders/paint/gaussianBlurFilter.comp");
    computeMosaicReduceProgram = ShaderCache::compute(":/shaders/paint/mosaicReduce.comp");
    computeMosaicFilterProgram = ShaderCache::compute(":/shaders/paint/mosaicFilter.comp");
    computeMotionBlurFilterProgram = ShaderCache::compute(":/shaders/paint/motionBlurFilter.comp");
    computeNoiseFilterProgram      = ShaderCache::compute(":/shaders/paint/noiseFilter.comp");
    computeGaussianBlurLayerProgram = ShaderCache::compute(":/shaders/render/gaussianBlurLayer.comp"); // 無料版でも使える
    computeMotionBlurLayerProgram   = ShaderCache::compute(":/shaders/render/motionBlurLayer.comp");   // 無料版でも使える
    computeMosaicReduceLayerProgram = ShaderCache::compute(":/shaders/render/mosaicReduceLayer.comp"); // 無料版でも使える
    computeMosaicLayerProgram       = ShaderCache::compute(":/shaders/render/mosaicLayer.comp");       // 無料版でも使える
    computeNoiseLayerProgram        = ShaderCache::compute(":/shaders/render/noiseLayer.comp");        // 無料版でも使える
#ifdef TIEPOLO_PRO_BUILD
    computeChromaticAberrationFilterProgram = ShaderCache::compute(":/shaders/paint/chromaticAberrationFilter.comp");
    computeChromaticAberrationLayerProgram  = ShaderCache::compute(":/shaders/render/chromaticAberrationLayer.comp");
    computeLensBlurFilterProgram            = ShaderCache::compute(":/shaders/paint/lensBlurFilter.comp");
    computeLensBlurLayerProgram             = ShaderCache::compute(":/shaders/render/lensBlurLayer.comp");
    computeGradientMapProgram               = ShaderCache::compute(":/shaders/paint/gradientMap.comp");
#endif
    computeWarpProgram         = ShaderCache::compute(":/shaders/paint/warp.comp");
    computeTransformProgram    = ShaderCache::compute(":/shaders/paint/transform.comp");
    computeFreeTransformProgram = ShaderCache::compute(":/shaders/paint/freeTransform.comp");
    computeHueSatLightProgram   = ShaderCache::compute(":/shaders/paint/hueSatLight.comp");
    computeBrightnessContrastProgram = ShaderCache::compute(":/shaders/paint/brightnessContrast.comp");
    computeColorBalanceProgram       = ShaderCache::compute(":/shaders/paint/colorBalance.comp");
    computeToneCurveProgram          = ShaderCache::compute(":/shaders/paint/toneCurve.comp");
    computeBelowCompositeProgram     = ShaderCache::compute(":/shaders/render/belowComposite.comp");
    renderProgram                    = ShaderCache::render();

    // ストローク色バッファ用のテクスチャユニットが取れるか。
    {
        GLint maxTexUnits = 0;
        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxTexUnits);
        strokeColorSupported_ = (maxTexUnits > kStrokeColorTexUnit);
        WINLOG(QStringLiteral("GL_MAX_TEXTURE_IMAGE_UNITS=%1 → ストローク色バッファ %2")
                   .arg(maxTexUnits).arg(strokeColorSupported_ ? "利用可" : "利用不可"));
    }

    const qint64 shaderMs = initTimer.elapsed();
    initTextures();
    const qint64 texMs = initTimer.elapsed() - shaderMs;
    glGenVertexArrays(1, &dummyVAO);
    // ペン先画像(キャンバスサイズに依存しないのでinitTextures()とは別に一度だけ確保する)。
    if (!setPenTipImage(toolCfg_->pen().tipImagePath()))
        setPenTipImage(":/textures/penTip/circle.png");

    // 紙質テクスチャも同様。
    if (!setPaperTexture(toolCfg_->pen().paperTexPath()))
        setPaperTexture(QString());

    // トーンカーブLUT(256x1, R8)。
    {
        glGenTextures(1, &toneCurveLUTTex);
        glBindTexture(GL_TEXTURE_2D, toneCurveLUTTex);
        quint8 identity[256];
        for (int i = 0; i < 256; i++) identity[i] = (quint8)i;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 1, 0, GL_RED, GL_UNSIGNED_BYTE, identity);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // グラデーションマップ(Pro限定)用のLUT。
    {
        glGenTextures(1, &gradientMapLUTTex);
        glBindTexture(GL_TEXTURE_2D, gradientMapLUTTex);
        quint8 blackToWhite[256 * 4];
        for (int i = 0; i < 256; i++) {
            blackToWhite[i * 4 + 0] = (quint8)i;
            blackToWhite[i * 4 + 1] = (quint8)i;
            blackToWhite[i * 4 + 2] = (quint8)i;
            blackToWhite[i * 4 + 3] = 255;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blackToWhite);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // ペンストロークのバッチスタンプ用SSBO(stroke.comp参照)。
    glGenBuffers(1, &strokeStampSSBO_);
    // 筆に乗っている絵の具(vec4 1個)。
    glGenBuffers(1, &brushPaintSSBO_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, brushPaintSSBO_);
    {
        const float init[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(init), init, GL_DYNAMIC_DRAW);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // シェーダー/テクスチャが揃ったので toolCtx_ を実体で埋め直す。
    setupToolContext();

    // FillTool を初期化してテクスチャ/シェーダーを注入。
    fillTool_.initialize(context());
    fillTool_.setTextures({layerTexBanks[0], maskTex, wallTex, outerJfaTex, innerJfaTex, sdfTex});
    fillTool_.setPrograms({
        computeWallProgram,
        computeJfaInitOuterProgram,
        computeJfaInitInnerProgram,
        computeJfaProgram,
        computeJfaFinalizeProgram,
        computeBakeProgram,
        computeMaskClearProgram
    });

    blurTool_.initialize(context());
    blurTool_.setProgram(computeBlurProgram);

    warpTool_.initialize(context());
    warpTool_.setProgram(computeWarpProgram);

    selectTool_.initialize(context());

    // 色調整/フィルター/変形/キャンバスサイズ系ツールの initialize は各 CanvasAction (src/actions/) へ移動。
    actions_.initializeAll(context());

    // ここまででシェーダーのコンパイル/リンクとtoolCtx_の初期化が完了する。
    glReady_ = true;
    // 開いた直後の1本目のストロークでも事前合成キャッシュが出来ているように、ここでも先読み作成を予約しておく(文書の初期化中は doc_->onChanged が抑止されているため、ここを入れないと1本目だけ書き始めが重くなる)。
    scheduleCompositeCachePrewarm();

    WINLOG(QStringLiteral("PERF CanvasWidget::initializeGL #%1 total=%2ms (shaders=%3ms textures=%4ms rest=%5ms)")
               .arg(initCount).arg(initTimer.elapsed())
               .arg(shaderMs).arg(texMs).arg(initTimer.elapsed() - shaderMs - texMs));

    updateCursor();
    emit initialized();
}

// テクスチャ初期化。
GLuint CanvasWidget::makeTexture2D(GLenum internalFormat, int w, int h, GLenum filter)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    return tex;
}

void CanvasWidget::syncBanksToToolContext()
{
    for (int i = 0; i < MAX_TILE_BANKS; i++) toolCtx_.layerTexBanks[i] = layerTexBanks[i];
    toolCtx_.slicesPerBank = slicesPerBank_;
}

void CanvasWidget::reserveTileSlices(int count)
{
    if (count <= 0) return;
    makeCurrent();
    sliceAllocator_.reserve(count);
    syncBanksToToolContext(); // reserve()内でonTextureRecreatedも呼ばれるが念のため
}

void CanvasWidget::bindLayerBanksForSampling(QOpenGLShaderProgram *prog)
{
    GLint units[MAX_TILE_BANKS];
    for (int i = 0; i < MAX_TILE_BANKS; i++) {
        glActiveTexture(GL_TEXTURE0 + LAYER_BANK_TEXUNIT_BASE + i);
        // 未生成バンクのスロットにもbank0を割り当てておく(サンプラー配列の全要素が有効なテクスチャを指している必要があるため。実際にサンプルされるのはsi < 生成済み容量 の範囲だけなので中身は問われない)。
        glBindTexture(GL_TEXTURE_2D_ARRAY, layerTexBanks[i] ? layerTexBanks[i] : layerTexBanks[0]);
        units[i] = LAYER_BANK_TEXUNIT_BASE + i;
    }
    // sampler2DArray配列は要素ごとに名前を組み立てず、配列名でまとめて設定する(要素名を毎回QByteArrayで作ると環境によってはQt内部でアサートに当たるため)。
    prog->setUniformValueArray("layerTexBanks", units, MAX_TILE_BANKS);
    prog->setUniformValue("uSlicesPerBank", slicesPerBank_);
    glActiveTexture(GL_TEXTURE0);
}

// [base, base+count) の連続グローバルスライス範囲を uClearColor でGPUクリアする。
void CanvasWidget::clearSliceRange(int base, int count, float r, float g, float b, float a)
{
    if (count <= 0 || slicesPerBank_ <= 0) return;
    int s = base, remaining = count;
    computeLayerClearProgram->bind();
    computeLayerClearProgram->setUniformValue("uClearColor", r, g, b, a);
    while (remaining > 0) {
        const int bank  = s / slicesPerBank_;
        const int local = s % slicesPerBank_;
        const int seg   = qMin(remaining, slicesPerBank_ - local);
        glBindImageTexture(1, layerTexBanks[bank], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);
        computeLayerClearProgram->setUniformValue("uBaseSlice", local);
        glDispatchCompute((TILE_SIZE + 15) / 16, (TILE_SIZE + 15) / 16, seg);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        s += seg;
        remaining -= seg;
    }
    computeLayerClearProgram->release();
}

void CanvasWidget::initTextures(bool createDefaultLayers) {
    // layerTexArrayに確保できるスライス数の上限を、固定の小さい定数ではなくこのGPU/ドライバが実際に許容する最大値(GL_MAX_ARRAY_TEXTURE_LAYERS)から決める。
    GLint maxArrayLayers = 0;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxArrayLayers);
    slicesPerBank_ = qMax((int)MAX_SLICES, (int)maxArrayLayers);

    // スライス確保ロジックのセットアップ(バンクテクスチャ群 layerTexBanks[] はCanvasWidgetが所有し続け、allocatorが生成・伸長・差し替えを行う)。
    sliceAllocator_.setup(this, layerTexBanks, TILE_SIZE, slicesPerBank_);
    sliceAllocator_.onTextureRecreated = [this] {
        syncBanksToToolContext();
        fillTool_.setTextures({layerTexBanks[0], maskTex, wallTex,
                                 outerJfaTex, innerJfaTex, sdfTex});
    };

    // maskTex (R8)
    strokeColorTex = 0;

    glGenTextures(1, &maskTex);
    glBindTexture(GL_TEXTURE_2D, maskTex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, canvasW, canvasH);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    QVector<uint8_t> emptyMask(canvasW * canvasH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvasW, canvasH,
                    GL_RED, GL_UNSIGNED_BYTE, emptyMask.constData());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindImageTexture(0, maskTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);

    computeMaskClearProgram->bind();
    glBindImageTexture(0, maskTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    // uDispatchOriginはプログラムオブジェクトに紐づくuniform状態であり、AirbrushTool(スタンプごとに部分範囲だけ処理するため非ゼロ値を設定する)が最後にこのプログラムを使った際の値がそのまま残っていることがある。
    {
        GLint loc = glGetUniformLocation(computeMaskClearProgram->programId(), "uDispatchOrigin");
        if (loc >= 0) glUniform2i(loc, 0, 0);
    }
    glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    computeMaskClearProgram->release();

    // タイルグリッドを doc に登録。
    doc_->initTileGrid(canvasW, canvasH);
 
    // layerTexArray（mipなし）。
    sliceAllocator_.createInitial(16);

    int mipLevels = 1 + (int)std::floor(std::log2(std::max(canvasW, canvasH)));
    glGenTextures(1, &compositedTex);
    glBindTexture(GL_TEXTURE_2D, compositedTex);
    glTexStorage2D(GL_TEXTURE_2D, mipLevels, GL_RGBA8, canvasW, canvasH);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // キャッシュ用。
    int tileCount = doc_->tilesX() * doc_->tilesY();

    // ナビゲーター用作業テクスチャ。
    glGenTextures(1, &compositedTileArr);
    glBindTexture(GL_TEXTURE_2D_ARRAY, compositedTileArr);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, TILE_SIZE, TILE_SIZE, tileCount);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // 塗りつぶし用。
    wallTex     = makeTexture2D(GL_R8,    canvasW, canvasH);
    outerJfaTex = makeTexture2D(GL_RG16F, canvasW, canvasH);
    innerJfaTex = makeTexture2D(GL_RG16F, canvasW, canvasH);
    sdfTex      = makeTexture2D(GL_R32F,  canvasW, canvasH);

    fullLayerTex = makeTexture2D(GL_RGBA8, canvasW, canvasH);

    // 「アクティブレイヤーより下」の合成結果キャッシュ(item4)。
    belowCompositeTex         = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
    belowCompositeClipBaseTex = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
    aboveCompositeTex         = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
    aboveCompositeClipScratch_ = makeTexture2D(GL_RGBA8, canvasW, canvasH);
    belowCompositeCacheValid_ = false;
    aboveCompositeCacheValid_ = false;

    // 選択範囲マスク(R8)。
    selectionMaskTex = makeTexture2D(GL_R8, canvasW, canvasH);
    {
        QVector<uint8_t> fullSel(canvasW * canvasH, 255);
        glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvasW, canvasH,
                        GL_RED, GL_UNSIGNED_BYTE, fullSel.constData());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    hasSelection_ = false;
    invalidateSelectionOutlineCache(); // selectionMaskTex自体を作り直したので古いキャッシュは無効

    // 変形ツール確定時の作業用スナップショット(初期値は使われないので未初期化のままでよい)。
    transformSrcTex        = makeTexture2D(GL_RGBA8, canvasW, canvasH);
    transformSrcSelMaskTex = makeTexture2D(GL_R8,    canvasW, canvasH);
    transformScratchW_ = canvasW;
    transformScratchH_ = canvasH;

    // SSBO 初期化 (すべてMAX_LAYERS個ぶん、フラットなレイヤーindexでそのまま引ける)。
    glGenBuffers(1, &ssboLayerOpacity);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerOpacity);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerVisible);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerVisible);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerBaseSlice);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerBaseSlice);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    // レイヤーマスクのベーススライス(-1なら無し)。
    glGenBuffers(1, &ssboLayerMaskBaseSlice);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerMaskBaseSlice);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerBlendMode);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerBlendMode);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerClipping);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerClipping);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerOriginTx);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerOriginTx);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerOriginTy);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerOriginTy);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerTilesX);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerTilesX);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerTilesY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerTilesY);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerIsSolidColor);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerIsSolidColor);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerSolidColor);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerSolidColor);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerAdjKind);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerAdjKind);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    // vec3配列はstd430で要素ストライドが16バイト(vec4扱い)になるため、レイヤーあたりfloat4分(x,y,z,pad)を確保する。
    glGenBuffers(1, &ssboLayerAdjParams);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerAdjParams);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    // フォルダー単位のマスクカスケード用(ivec4、レイヤーあたり最大4階層ぶんの祖先フォルダーのマスクベーススライス)。
    glGenBuffers(1, &ssboLayerAncestorMaskSlices);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerAncestorMaskSlices);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * 4 * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // レイヤー作成(この後 CacheAndNotify通知が飛ぶ)より前に toolCtx_ を実体で埋める。
    setupToolContext();

    // addLayer()自体がdoc_->onChanged経由でupdate()を呼び得るため。
    if (createDefaultLayers) {
        m_initializing = true;

        // 最初のレイヤーを2枚作成 (下: 常に白い単色レイヤー、上: 透明な作業レイヤー)。
        addSolidColorLayer("用紙");
        addLayer("レイヤー1");
        doc_->setActiveLayer(1);

        m_initializing = false;
    }
    // createDefaultLayers=false の場合はレイヤーを1枚も作らずに返す。

    update();
}

// ナビゲーター用: 全レイヤー合成して compositedTex を更新。
void CanvasWidget::updateCompositedTex() {
    makeCurrent();
    compositor_.updateCompositedTex(toolCtx_);
}

// 変形確定用の作業テクスチャを、指定サイズに合わせて作り直す
void CanvasWidget::ensureTransformScratchSize(int w, int h)
{
    if (w == transformScratchW_ && h == transformScratchH_ && fullLayerTex != 0 && transformSrcTex != 0)
        return;

    if (fullLayerTex)    { glDeleteTextures(1, &fullLayerTex);    fullLayerTex    = 0; }
    if (transformSrcTex) { glDeleteTextures(1, &transformSrcTex); transformSrcTex = 0; }

    fullLayerTex    = makeTexture2D(GL_RGBA8, w, h);
    transformSrcTex = makeTexture2D(GL_RGBA8, w, h);
    transformScratchW_ = w;
    transformScratchH_ = h;

    toolCtx_.fullLayerTex   = fullLayerTex;
    toolCtx_.transformSrcTex = transformSrcTex;
}

// 多段パスのフィルター用の中間バッファを確保する(CanvasWidget.hのコメント参照)。
GLuint CanvasWidget::ensureFilterScratch(int w, int h)
{
    if (w == filterScratchW_ && h == filterScratchH_ && filterScratchTex_ != 0)
        return filterScratchTex_;

    if (filterScratchTex_) { glDeleteTextures(1, &filterScratchTex_); filterScratchTex_ = 0; }

    filterScratchTex_ = makeTexture2D(GL_RGBA8, w, h);
    filterScratchW_ = w;
    filterScratchH_ = h;
    return filterScratchTex_;
}

// レイヤーの矩形を、キャンバスタイル座標系で指定範囲を覆うように拡張する。
bool CanvasWidget::growLayerBoundsToCoverCanvasTiles(int layerIndex, int minTx, int minTy, int maxTxEx, int maxTyEx)
{
    if (!doc_) return false;

    auto copyFn = [this](int srcSlice, int dstSlice) {
        writeSlicePixels(dstSlice, readSlicePixels(srcSlice));
    };
    auto clearFn = [this](int sliceIndex) {
        static const QByteArray zero(TILE_SIZE * TILE_SIZE * 4, 0);
        writeSlicePixels(sliceIndex, zero);
    };

    bool grew = doc_->growLayerBounds(layerIndex, minTx, minTy, maxTxEx, maxTyEx, copyFn, clearFn);
    if (grew)
        doc_->invalidateLayerTileUndoHistory(layerIndex);
    return grew;
}

// リサイズ / 描画。
void CanvasWidget::resizeGL(int w, int h) {
    // モニター間の移動や表示スケール変更でDPRが変わるので、ここで追従させる(ツール側はこの値でマウス座標をビュー空間へ換算する)。
    toolCtx_.viewDpr = viewDpr();
    // w/hは論理px。
    Q_UNUSED(w);
    Q_UNUSED(h);
    fitCanvasToView();
    actions_.positionActivePanel(); // 変形/色調整/フィルター/レイヤー編集系(controller管理アクション)
}

void CanvasWidget::fitCanvasToView()
{
    // ビューはデバイスピクセルで動く(CanvasWidget.hのviewDpr()のコメント参照)。
    const float w = viewWidth();
    const float h = viewHeight();
    if (w <= 0.0f || h <= 0.0f) return;

    float scaleX = w / canvasW;
    float scaleY = h / canvasH;
    float s = qMin(scaleX, scaleY) * 0.9f;

    view_.setScale(s);
    // キャンバス中心をウィジェット中心に合わせる。
    view_.setOffset(QVector2D(
        w / 2.0f - (canvasW / 2.0f) * s,
        h / 2.0f - (canvasH / 2.0f) * s
    ));
    view_.setRotation(0.0f);

    updateCursor(); // 表示倍率が変わるとペン円カーソルの画面上サイズも変わる
    update();
    emit viewChanged();
}

void CanvasWidget::setViewScaleCentered(float scale)
{
    scale = qMax(0.02f, scale);
    const QVector2D center(viewWidth() / 2.0f, viewHeight() / 2.0f);
    const float factor = scale / view_.scale();
    view_.zoomAround(center, factor);
    updateCursor();
    update();
    emit viewChanged();
}

void CanvasWidget::setFlippedX(bool flip)
{
    if (view_.flipX() == flip) return;

    // ViewTransform::matrix()は「回転→スケール(反転)→平行移動(offset)」の順で合成されており(T*S*R)、offsetは最後に screen 空間へそのまま足される。
    QVector2D off = view_.offset();
    off.setX(viewWidth() - off.x()); // offsetはビュー空間(デバイスpx)
    view_.setOffset(off);

    view_.setFlipX(flip);
    update();
    emit viewChanged();
}

QPolygonF CanvasWidget::visibleCanvasRectPolygon() const
{
    QPolygonF poly;
    // ビュー空間(デバイスpx)の四隅。
    const QPointF corners[4] = {
        QPointF(0, 0), QPointF((qreal)viewWidth(), 0),
        QPointF((qreal)viewWidth(), (qreal)viewHeight()), QPointF(0, (qreal)viewHeight())
    };
    for (const QPointF &c : corners) {
        QVector2D canvasPos = view_.widgetToCanvas(c, qRound(viewHeight())); // キャンバスピクセル座標(Y上向き)
        poly << QPointF(canvasPos.x(), canvasH - canvasPos.y());  // QImageと同じY下向きへ変換
    }
    return poly;
}

void CanvasWidget::panByCanvasDelta(const QVector2D &canvasDeltaYDown)
{
    // visibleCanvasRectPolygon()と同じ規則(Y下向き⇔ViewTransform内部のY上向き)で符号を揃えてから、
    // 平行移動を除いた線形部分(回転・拡縮・左右反転)だけをscreen空間(offset_と同じ座標系)へ写像し、offsetに加算する。
    const QVector2D canvasDeltaYUp(canvasDeltaYDown.x(), -canvasDeltaYDown.y());
    const QVector3D screenDelta = view_.matrix().mapVector(QVector3D(canvasDeltaYUp, 0.0f));
    view_.setOffset(view_.offset() + QVector2D(screenDelta.x(), screenDelta.y()));
    updateCursor();
    update();
    emit viewChanged();
}

void CanvasWidget::noteStrokeDirtyRegion(float minX, float minY, float maxX, float maxY)
{
    if (!strokeDirtyValid_) {
        strokeDirtyMinX_ = minX; strokeDirtyMinY_ = minY;
        strokeDirtyMaxX_ = maxX; strokeDirtyMaxY_ = maxY;
        strokeDirtyValid_ = true;
    } else {
        strokeDirtyMinX_ = qMin(strokeDirtyMinX_, minX);
        strokeDirtyMinY_ = qMin(strokeDirtyMinY_, minY);
        strokeDirtyMaxX_ = qMax(strokeDirtyMaxX_, maxX);
        strokeDirtyMaxY_ = qMax(strokeDirtyMaxY_, maxY);
    }
}

void CanvasWidget::paintGL() {

    // Qtが設定するviewportは小数DPRで切り捨てられることがあり、実際のFBOより1px以上小さくなる。
    const qreal currentDpr = devicePixelRatioF();
    glViewport(0, 0, qMax(1, qRound(width() * currentDpr)),
                     qMax(1, qRound(height() * currentDpr)));

    paintGlCalls_++;
    // ドラッグ中に、こちらが呼んだ同期repaint()以外の経路(どこかのupdate())で描かれたフレーム。
    if (!inSyncRepaint_ && WinLog::enabled() && viewDiagCount_ < 40) {
        viewDiagCount_++;
        WINLOG(QStringLiteral("PERF view: 余分なpaintGL(update由来)"));
    }

    // 計測用(初回のみ): 実際に描き込んでいるフレームバッファの画素数を問い合わせる。
    if (WinLog::enabled()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            GLint vp[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_VIEWPORT, vp);
            GLint objName = 0, objType = 0, fbW = 0, fbH = 0;
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                   GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &objType);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                   GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &objName);
            if (objType == GL_TEXTURE && objName) {
                GLint prev = 0;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
                glBindTexture(GL_TEXTURE_2D, (GLuint)objName);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &fbW);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &fbH);
                glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
            }
            WINLOG(QStringLiteral("PERF paintGL: FBO=%1x%2 viewport=%3x%4 widget(logical)=%5x%6 dpr=%7")
                       .arg(fbW).arg(fbH).arg(vp[2]).arg(vp[3])
                       .arg(width()).arg(height()).arg(devicePixelRatioF()));
            // presentが1回30ms近くかかっている件の切り分け用。
            const QWindow *topWin = window() ? window()->windowHandle() : nullptr;
            WINLOG(QStringLiteral("PERF swapInterval: default=%1 widgetCtx=%2 topWindow=%3")
                       .arg(QSurfaceFormat::defaultFormat().swapInterval())
                       .arg(context() ? context()->format().swapInterval() : -1)
                       .arg(topWin ? topWin->format().swapInterval() : -1));
        }
    }

    // paintGL本体の所要時間。
    QElapsedTimer paintClock;
    paintClock.start();
    const bool logPaint = WinLog::enabled() && strokeFrameLogCount_ < 12;

    // Qtがこのフレーム用に束縛したFBOを保持する。
    GLint prevDrawFbo = 0, prevReadFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);

    // フレームレート律速バッチ(Tool::flushPendingInput参照)。
    if (Tool *t = currentTool()) t->flushPendingInput(toolCtx_);

    // ストローク中の部分再描画: 前回paint以降に実際に変更されたキャンバス領域が分かっている場合、その矩形(+マージン)だけをシザーで描き直し、残りは前フレームのFBO内容(PartialUpdate)をそのまま使う。
    bool partialPaint = false;
    int  scissorW = 0, scissorH = 0; // 診断ログ用
    if (strokeDirtyValid_) {
        Tool *t = currentTool();
        if (t && t->isActive()) {
            // キャンバスpx(Y上向き) → ウィジェット座標。
            const QPointF c0 = toolCtx_.pixelToWidget(QVector2D(strokeDirtyMinX_, strokeDirtyMinY_));
            const QPointF c1 = toolCtx_.pixelToWidget(QVector2D(strokeDirtyMaxX_ + 1.0f, strokeDirtyMinY_));
            const QPointF c2 = toolCtx_.pixelToWidget(QVector2D(strokeDirtyMinX_, strokeDirtyMaxY_ + 1.0f));
            const QPointF c3 = toolCtx_.pixelToWidget(QVector2D(strokeDirtyMaxX_ + 1.0f, strokeDirtyMaxY_ + 1.0f));
            const qreal margin = 2.0;
            const qreal wx0 = qMin(qMin(c0.x(), c1.x()), qMin(c2.x(), c3.x())) - margin;
            const qreal wy0 = qMin(qMin(c0.y(), c1.y()), qMin(c2.y(), c3.y())) - margin;
            const qreal wx1 = qMax(qMax(c0.x(), c1.x()), qMax(c2.x(), c3.x())) + margin;
            const qreal wy1 = qMax(qMax(c0.y(), c1.y()), qMax(c2.y(), c3.y())) + margin;

            // ウィジェット座標(Y下向き) → FBO座標(Y上向き、物理px)。
            const qreal dpr = devicePixelRatioF();
            const int fbW = qMax(1, (int)std::lround(width()  * dpr));
            const int fbH = qMax(1, (int)std::lround(height() * dpr));
            int sx0 = qBound(0, (int)std::floor(wx0 * dpr), fbW);
            int sx1 = qBound(0, (int)std::ceil (wx1 * dpr), fbW);
            int syTop    = qBound(0, (int)std::floor(wy0 * dpr), fbH);
            int syBottom = qBound(0, (int)std::ceil (wy1 * dpr), fbH);
            const int sy0 = fbH - syBottom; // GLのシザーは左下原点
            const int sw  = sx1 - sx0;
            const int sh  = syBottom - syTop;

            glEnable(GL_SCISSOR_TEST);
            glScissor(sx0, sy0, qMax(0, sw), qMax(0, sh));
            partialPaint = true;
            scissorW = qMax(0, sw);
            scissorH = qMax(0, sh);
        }
    }
    strokeDirtyValid_ = false;

    // フィルターレイヤーがある文書では、ここでオフスクリーンの連鎖を組み直して「どこまで合成済みか」と、その結果テクスチャを受け取る。
    GLuint compositeBaseTex     = belowCompositeTex;
    GLuint compositeBaseClipTex = belowCompositeClipBaseTex;
    const int compositeStartZ   = prepareCompositeBase(compositeBaseTex, compositeBaseClipTex);
    const qint64 nsAfterBase    = logPaint ? paintClock.nsecsElapsed() : 0;

    // 上のflushPendingInput()/prepareCompositeBase()が一時FBOを使ってバインドを外していたら、ここでこのフレーム用のFBOへ戻す(理由は関数冒頭のprevDrawFbo のコメント参照)。
    {
        GLint nowDraw = 0, nowRead = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &nowDraw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &nowRead);
        if (nowDraw != prevDrawFbo || nowRead != prevReadFbo) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDrawFbo);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevReadFbo);
            WINLOG(QStringLiteral("PERF paintGL: FBO rebound (draw %1->%2 read %3->%4) "
                                  "— これが無いとこのフレームは捨てられていた")
                       .arg(nowDraw).arg(prevDrawFbo).arg(nowRead).arg(prevReadFbo));
        }
    }

    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT); // シザー有効時はシザー矩形内だけクリアされる
    if (!renderProgram->bind()) {
        if (partialPaint) glDisable(GL_SCISSOR_TEST);
        lastPaintGlCostNs_ = paintClock.nsecsElapsed();
        paintGlNsAccum_ += lastPaintGlCostNs_;
        return;
    }

    setLayerUniformsForRender(renderProgram);

    renderProgram->setUniformValue("uViewMatrix",        view_.matrix());
    renderProgram->setUniformValue("uViewMatrixInverse", view_.inverseMatrix());
    renderProgram->setUniformValue("uActiveLayerIndex",  doc_->activeLayerIndex());
    renderProgram->setUniformValue("uIsEditingMaskLayer",
        (editingMaskLayerIndex_ >= 0 && editingMaskLayerIndex_ == doc_->activeLayerIndex()) ? 1 : 0);
    renderProgram->setUniformValue("uCanvasSize",        QVector2D(canvasW, canvasH));
    // ビューポート(=FBO)はデバイスピクセルなので、ここも合わせる。
    renderProgram->setUniformValue("uWindowSize",        QVector2D(viewWidth(), viewHeight()));
    // 拡大表示中だけ画素中心へ吸着させる判定に使う(render.frag の uViewScale 参照)。
    renderProgram->setUniformValue("uViewScale",         view_.scale());
    // 縮小表示時の面積平均のサンプル数(1辺)。
    int minifySamples = qBound(1, (int)std::ceil(1.0f / qMax(view_.scale(), 0.0001f)), 4);

    // 移動・回転中は1点サンプルへ落とす。
    if (Tool *t = currentTool(); t && t->isActive() && t->transformsViewWhileActive())
        minifySamples = 1;
    renderProgram->setUniformValue("uMinifySamples", minifySamples);
    if (WinLog::enabled()) {
        static int lastLogged = -1;
        if (minifySamples != lastLogged) {
            lastLogged = minifySamples;
            WINLOG(QStringLiteral("PERF uViewScale=%1 uMinifySamples=%2 (loc=%3, -1ならシェーダーに届いていない)")
                       .arg(view_.scale()).arg(minifySamples)
                       .arg(renderProgram->uniformLocation("uMinifySamples")));
        }
    }
    renderProgram->setUniformValue("uTileSize",          TILE_SIZE);
    renderProgram->setUniformValue("uCanvasTilesX",      doc_->tilesX()); // レイヤーマスクのタイル参照(常にキャンバス全体)用

    // ブラシ色。
    const float brushOpacity = (activeTool == ToolType::Airbrush) ? toolCfg_->airbrush().opacity()
                                                                  : toolCfg_->pen().opacity();
    const EraseBrush erase = eraseBrushFor(activeTool == ToolType::Eraser,
                                           toolCfg_->color(), brushOpacity);
    QColor col = erase.active ? erase.shaderColor()
                              : toPreMulColor(toolCfg_->color().rawRGBA(), brushOpacity);
    renderProgram->setUniformValue("uEraseMode", erase.active ? 1 : 0);
    // ブラシの合成モードはペン専用の設定(消す動作のときは意味を持たない)。
    renderProgram->setUniformValue("uBrushBlendMode",
        (activeTool == ToolType::Pen && !erase.active) ? toolCfg_->pen().brushBlendMode() : 0);
    // スタンプごとの色(色のランダム等)。
    const bool useStrokeColor = (activeTool == ToolType::Pen && !erase.active
                                 && toolCfg_->pen().usesPerStampColor() && strokeColorTex != 0);
    renderProgram->setUniformValue("uUseStrokeColor", useStrokeColor ? 1 : 0);
    if (useStrokeColor) {
        glActiveTexture(GL_TEXTURE0 + kStrokeColorTexUnit);
        glBindTexture(GL_TEXTURE_2D, strokeColorTex);
        glActiveTexture(GL_TEXTURE0);
    }
    renderProgram->setUniformValue("uBrushColor",
        col.redF(), col.greenF(), col.blueF(), col.alphaF());

    // レイヤーマスク編集中のライブプレビュー用。
    QColor maskCol = erase.active
        ? maskBrushColor(true, toolCfg_->color().rawRGBA(), erase.strength)
        : maskBrushColor(false, toolCfg_->color().rawRGBA(), brushOpacity);
    renderProgram->setUniformValue("uMaskBrushColor",
        maskCol.redF(), maskCol.greenF(), maskCol.blueF(), maskCol.alphaF());

    renderProgram->setUniformValue("uHasSelection",    hasSelection_ ? 1 : 0);
    renderProgram->setUniformValue("uIsSelectionTool", (activeTool == ToolType::Selection) ? 1 : 0);

    // 変形/自由変形/色調整/フィルター系。
    actions_.applyRenderState(renderProgram);

    // 表示上の見た目だけを変えるカラーモード(RGB/CMYK擬似/グレースケール)。
    renderProgram->setUniformValue("uColorMode", (int)toolCfg_->colorMode().mode());

    // モニターキャリブレーション。
    const CalibrationConfig &cal = toolCfg_->calibration();
    renderProgram->setUniformValue("uCalBrightness", cal.brightness() / 200.0f);
    renderProgram->setUniformValue("uCalContrast",   1.0f + cal.contrast() / 100.0f);
    renderProgram->setUniformValue("uCalCyan",       cal.cyan()    / 100.0f);
    renderProgram->setUniformValue("uCalMagenta",    cal.magenta() / 100.0f);
    renderProgram->setUniformValue("uCalYellow",     cal.yellow()  / 100.0f);

    // キャンバス外側の背景色(実データには無関係の表示設定)。
    {
        const QColor bg = editingMaskLayerIndex_ >= 0
            ? Theme::accentHoverLight
            : toolCfg_->canvasBackground().color();
        renderProgram->setUniformValue("uCanvasOutsideBg",
            QVector3D(bg.redF(), bg.greenF(), bg.blueF()));
    }

    // 市松模様の2色(先端画像プレビュー・レイヤープレビューと共通のTheme値)。
    renderProgram->setUniformValue("uCheckerColorA",
        QVector3D(Theme::checkerDark.redF(), Theme::checkerDark.greenF(), Theme::checkerDark.blueF()));
    renderProgram->setUniformValue("uCheckerColorB",
        QVector3D(Theme::checkerLight.redF(), Theme::checkerLight.greenF(), Theme::checkerLight.blueF()));

    // binding 番号は render.frag の layout(binding=N) に合わせる。
    bindLayerBanksForSampling(renderProgram);
    renderProgram->setUniformValue("maskTex",           2);
    renderProgram->setUniformValue("selectionMaskTex",  3);
    renderProgram->setUniformValue("gaussianBlurPreviewTex", 4);
    renderProgram->setUniformValue("toneCurveLUTTex", 5);
    renderProgram->setUniformValue("gradientMapLUTTex", 0);
    renderProgram->setUniformValue("belowCompositeTex",         6);
    renderProgram->setUniformValue("belowCompositeClipBaseTex", 7);
    renderProgram->setUniformValue("aboveCompositeTex",         1);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, aboveCompositeTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, maskTex);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, transformSrcTex);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, toneCurveLUTTex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gradientMapLUTTex);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, compositeBaseTex);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, compositeBaseClipTex);

    // ストローク中はアクティブレイヤーより下をキャッシュから読む。
    // render.frag側はz=アクティブレイヤー以降だけを合成し直す。
    renderProgram->setUniformValue("uUseBelowCompositeCache", compositeStartZ >= 0 ? 1 : 0);
    renderProgram->setUniformValue("uCompositeStartZ",        compositeStartZ >= 0 ? compositeStartZ : 0);
    renderProgram->setUniformValue("uUseAboveCompositeCache", aboveCompositeCacheValid_ ? 1 : 0);

    glBindVertexArray(dummyVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    // TIEPOLO_GLFINISH=1のときだけGPU待ち時間を計測する。
    if (qEnvironmentVariableIsSet("TIEPOLO_GLFINISH")) {
        QElapsedTimer gpuClock;
        gpuClock.start();
        glFinish();
        if (WinLog::enabled() && strokeFrameLogCount_ < 12)
            WINLOG(QStringLiteral("PERF gpu: 描画のGPU実処理 %1ms (minify=%2 layers=%3)")
                       .arg(gpuClock.nsecsElapsed() / 1e6, 0, 'f', 2)
                       .arg(minifySamples).arg(doc_ ? doc_->layerCount() : 0));
    }

    renderProgram->release();

    if (logPaint)
        WINLOG(QStringLiteral("PERF paintGL: %1 base=%2ms draw=%3ms")
                   .arg(partialPaint ? QStringLiteral("partial %1x%2").arg(scissorW).arg(scissorH)
                                     : QStringLiteral("FULL"))
                   .arg(nsAfterBase / 1e6, 0, 'f', 2)
                   .arg((paintClock.nsecsElapsed() - nsAfterBase) / 1e6, 0, 'f', 2));

    if (partialPaint) {
        // 部分再描画のときはQPainterオーバーレイ(選択範囲の破線等)は描き直さない。
        glDisable(GL_SCISSOR_TEST);
        lastPaintGlCostNs_ = paintClock.nsecsElapsed();
        paintGlNsAccum_ += lastPaintGlCostNs_;
        return;
    }

    // 投げ縄選択の軌跡プレビューなど、GLの描画が終わった後にQPainterで重ねるオーバーレイ(ほとんどのツールはpaintOverlay()が空実装なので何も描かれない)。
    {
        QPainter painter(this);
        paintSelectionOutline(painter);
        if (!actions_.paintActiveOverlay(painter, toolCtx_)) {
            // controller管理アクション(変形/自由変形/キャンバスサイズ/色収差の円形ハンドル等)が何も描かなければ、通常ツールのオーバーレイを描く。
            if (Tool *t = currentTool()) t->paintOverlay(painter, toolCtx_);
        }
    }
    lastPaintGlCostNs_ = paintClock.nsecsElapsed();
    paintGlNsAccum_ += lastPaintGlCostNs_;
}

// 選択範囲マスク(selectionMaskTex)の境界形状(キャンバスpx座標系)をキャッシュへ再計算する。
void CanvasWidget::rebuildSelectionOutlineCache()
{
    QVector<uint8_t> buf(canvasW * canvasH);
    glBindTexture(GL_TEXTURE_2D, selectionMaskTex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, buf.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);

    QImage maskImg(canvasW, canvasH, QImage::Format_Mono);
    maskImg.setColor(0, qRgb(0, 0, 0));
    maskImg.setColor(1, qRgb(255, 255, 255));
    maskImg.fill(0);
    for (int y = 0; y < canvasH; y++) {
        for (int x = 0; x < canvasW; x++) {
            if (buf[y * canvasW + x] != 0) maskImg.setPixel(x, y, 1);
        }
    }

    const QRegion region(QBitmap::fromImage(maskImg));
    // QBitmap::fromImage()は全面白(=全域選択。全選択アクション直後がこれにあたる)の画像を変換すると空のQRegionになる(QBitmapは伝統的に「黒=set」の解釈を持つため、
    // 全白画像には有効ビットが1つも無いと判定される)。
    QPainterPath canvasPath;
    if (region.isEmpty()) {
        canvasPath.addRect(0, 0, canvasW, canvasH);
    } else {
        canvasPath.addRegion(region);
        // addRegion()は矩形の集合(1走査行の連続run単位でまとめられるため、斜めの辺を持つ形状だと行ごとに別々の矩形になり、多いと数百枚)をそのまま返す。
        canvasPath = canvasPath.simplified();
    }

    selectionOutlineCachePx_ = canvasPath;
}

void CanvasWidget::paintSelectionOutline(QPainter &painter)
{
    if (!hasSelection_ || canvasW <= 0 || canvasH <= 0 || !toolCtx_.pixelToWidget) return;

    if (selectionOutlineCacheDirty_) {
        rebuildSelectionOutlineCache();
        selectionOutlineCacheDirty_ = false;
    }

    // 変形中は selectionMaskTex 自体がまだ動いていない(確定時に transform.comp が動かす)ため、輪郭キャッシュも元の位置のまま。
    CanvasAction *activeAct = actions_.activeAction();

    // ウィジェット座標へ変換したパスをキャッシュする。
    const QMatrix4x4 viewMatrix = view_.matrix();
    const bool mapsPerFrame = (activeAct != nullptr); // 変形系はドラッグ中パスが動く
    if (!selectionOutlineWidgetPathValid_ || mapsPerFrame
        || viewMatrix != selectionOutlineWidgetPathView_
        || size() != selectionOutlineWidgetPathSize_) {
        QPainterPath widgetPath;
        for (int i = 0; i < selectionOutlineCachePx_.elementCount(); i++) {
            const QPainterPath::Element el = selectionOutlineCachePx_.elementAt(i);
            QPointF cp(el.x, el.y);
            if (activeAct) activeAct->mapSelectionOutlinePoint(cp);
            const QPointF wp = toolCtx_.pixelToWidget(QVector2D(cp.x(), cp.y()));
            if (el.type == QPainterPath::MoveToElement) widgetPath.moveTo(wp);
            else widgetPath.lineTo(wp); // addRegion()は矩形の集合なのでMoveTo/LineToのみ
        }
        selectionOutlineWidgetPath_     = widgetPath;
        selectionOutlineWidgetPathView_ = viewMatrix;
        selectionOutlineWidgetPathSize_ = size();
        // 変形中は次フレームも作り直す必要があるのでキャッシュ有効にはしない。
        selectionOutlineWidgetPathValid_ = !mapsPerFrame;
    }
    const QPainterPath &widgetPath = selectionOutlineWidgetPath_;

    // マーチングアンツはQRegionの境界(=軸平行の1px線)なので、アンチエイリアスを掛けても見た目はほぼ変わらないのに、数千セグメントの破線ストロークではラスタライズ費用が跳ね上がる。
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setBrush(Qt::NoBrush);

    QPen whitePen(QColor(255, 255, 255, 235));
    whitePen.setWidthF(1.0);
    whitePen.setStyle(Qt::DashLine);
    painter.setPen(whitePen);
    painter.drawPath(widgetPath);

    QPen blackPen(QColor(0, 0, 0, 235));
    blackPen.setWidthF(1.0);
    blackPen.setStyle(Qt::DashLine);
    blackPen.setDashOffset(4.0); // 白の破線と半周期ずらして交互に見せる
    painter.setPen(blackPen);
    painter.drawPath(widgetPath);
}

void CanvasWidget::setLayerUniformsForRender(QOpenGLShaderProgram *prog)
{
    compositor_.setLayerUniformsForRender(toolCtx_, prog);
}

// マウスイベント
