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
    // 計測用。QOpenGLWidgetは親が変わるとコンテキストごと作り直されるため、
    // タブを別ペインへ移す(=分割)たびにここが再実行されうる。その場合は
    // シェーダーを全部コンパイルし直すので、何回呼ばれて各段が何msかかっているかを
    // 見られるようにしておく(TIEPOLO_WINLOG=1 のときだけ)。
    static int initCount = 0;
    ++initCount;
    QElapsedTimer initTimer;
    initTimer.start();

    initializeOpenGLFunctions();
    glFunctionsReady_ = true;

    // シェーダープログラムはプロセス全体で1組だけ作り、全タブ(CanvasWidget)で共有する。
    // 実体と事前コンパイルは ShaderCache が持つ(理由と計測値はそちらのコメント参照)。
    // ここで引くときには、たいてい起動直後の背景コンパイルで出来上がっている。
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

    // ストローク色バッファ用のテクスチャユニットが取れるか。render.fragは
    // 0〜7を個別のテクスチャ、8〜15をレイヤーバンクで使い切っているので16番が要る。
    // GL4.3の下限がちょうど16なので、ここで実際の上限を確かめておく
    // (足りない環境ではスタンプごとの色の経路自体を使わない)。
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
    // 保存済み設定にパスが残っていればそれを、読み込みに失敗したら既定の丸ブラシを使う。
    if (!setPenTipImage(toolCfg_->pen().tipImagePath()))
        setPenTipImage(":/textures/penTip/circle.png");

    // 紙質テクスチャも同様。既定は「なし」(空パス)なので、通常はここで何もしない。
    if (!setPaperTexture(toolCfg_->pen().paperTexPath()))
        setPaperTexture(QString());

    // トーンカーブLUT(256x1, R8)。キャンバスサイズに依存しないので一度だけ確保し、
    // 恒等カーブ(出力=入力)で初期化しておく。以後はToneCurveTool::uploadLutが
    // glTexSubImage2Dで中身だけ差し替える。GL_LINEARで補間することで、256段階の
    // 粗さを感じさせずになめらかに色を変換できる。
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

    // グラデーションマップ(Pro限定)用のLUT。上のトーンカーブLUTと同じ扱いだが、
    // 輝度1つから色(RGB)を引くのでRGBA8。初期値は黒→白の素直なグラデーション。
    // 無料版ビルドではこのLUTを書き換えるツール自体が存在しないが、render.fragは
    // 常にユニット0へこれを束縛するので、テクスチャ自体は両版で確保しておく。
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

    // ペンストロークのバッチスタンプ用SSBO(stroke.comp参照)。中身は使用のたびに
    // glBufferSubData/glBufferDataで書き換えるので、ここでは器だけ確保しておく。
    glGenBuffers(1, &strokeStampSSBO_);
    // 筆に乗っている絵の具(vec4 1個)。中身はストローク開始時にCPU側が初期化する。
    glGenBuffers(1, &brushPaintSSBO_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, brushPaintSSBO_);
    {
        const float init[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(init), init, GL_DYNAMIC_DRAW);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // シェーダー/テクスチャが揃ったので toolCtx_ を実体で埋め直す
    // (コンストラクタ時点ではまだ全部nullptr/0だったため)
    setupToolContext();

    // FillTool を初期化してテクスチャ/シェーダーを注入
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

    // 色調整/フィルター/変形/キャンバスサイズ系ツールの initialize は
    // 各 CanvasAction (src/actions/) へ移動。
    actions_.initializeAll(context());

    // ここまででシェーダーのコンパイル/リンクとtoolCtx_の初期化が完了する。
    // initializeGL()完了前(シェーダーコンパイル中など)にマウス操作が届いても、
    // 未初期化のtoolCtx_/GL資源に触れてしまわないよう、マウスイベント側で
    // glReady_をガードに使う。
    glReady_ = true;
    // 開いた直後の1本目のストロークでも事前合成キャッシュが出来ているように、
    // ここでも先読み作成を予約しておく(文書の初期化中は doc_->onChanged が
    // 抑止されているため、ここを入れないと1本目だけ書き始めが重くなる)。
    scheduleCompositeCachePrewarm();

    WINLOG(QStringLiteral("PERF CanvasWidget::initializeGL #%1 total=%2ms (shaders=%3ms textures=%4ms rest=%5ms)")
               .arg(initCount).arg(initTimer.elapsed())
               .arg(shaderMs).arg(texMs).arg(initTimer.elapsed() - shaderMs - texMs));

    updateCursor();
    emit initialized();
}

// ===========================================================================
// テクスチャ初期化
// ===========================================================================
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
        // 未生成バンクのスロットにもbank0を割り当てておく(サンプラー配列の全要素が
        // 有効なテクスチャを指している必要があるため。実際にサンプルされるのは
        // si < 生成済み容量 の範囲だけなので中身は問われない)。
        glBindTexture(GL_TEXTURE_2D_ARRAY, layerTexBanks[i] ? layerTexBanks[i] : layerTexBanks[0]);
        units[i] = LAYER_BANK_TEXUNIT_BASE + i;
    }
    // sampler2DArray配列は要素ごとに名前を組み立てず、配列名でまとめて設定する
    // (要素名を毎回QByteArrayで作ると環境によってはQt内部でアサートに当たるため)。
    prog->setUniformValueArray("layerTexBanks", units, MAX_TILE_BANKS);
    prog->setUniformValue("uSlicesPerBank", slicesPerBank_);
    glActiveTexture(GL_TEXTURE0);
}

// [base, base+count) の連続グローバルスライス範囲を uClearColor でGPUクリアする。
// 1回のディスパッチ(layerclear.comp)は image2DArray 1本(=1バンク)に閉じている
// 必要があるため、バンク境界でセグメントに分割し、各バンクを image unit 1 にバインド
// して「そのバンク内ローカルなuBaseSlice」で個別にディスパッチする。
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
    // layerTexArrayに確保できるスライス数の上限を、固定の小さい定数ではなく
    // このGPU/ドライバが実際に許容する最大値(GL_MAX_ARRAY_TEXTURE_LAYERS)から
    // 決める。これによりPhotoshop同様、キャンバスサイズ・レイヤー数(=タイル数)を
    // 「VRAM/ドライバの限界まで」増やせるようになる(伸長ロジック自体は
    // LayerSliceAllocator::grow()が2倍ずつ確保し直す形で既に実装済み)。
    // MAX_SLICESはOpenGL仕様上の最低保証値なので、問い合わせが異常な値を返した
    // 場合のフォールバック下限として使う。
    // 1バンクあたりのスライス数を、固定の小さい定数ではなくGPU/ドライバが実際に
    // 許容する最大値(GL_MAX_ARRAY_TEXTURE_LAYERS)にする。さらにそのバンクを
    // 最大 MAX_TILE_BANKS 本まで並べるので、タイル数の上限は実質「VRAM/バンク数」まで
    // 引き上がる(詳細は CanvasDocument.h / LayerSliceAllocator.h)。MAX_SLICESは
    // OpenGL仕様上の最低保証値で、問い合わせが異常値を返した場合の下限フォールバック。
    GLint maxArrayLayers = 0;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxArrayLayers);
    slicesPerBank_ = qMax((int)MAX_SLICES, (int)maxArrayLayers);

    // スライス確保ロジックのセットアップ(バンクテクスチャ群 layerTexBanks[] は
    // CanvasWidgetが所有し続け、allocatorが生成・伸長・差し替えを行う)。
    sliceAllocator_.setup(this, layerTexBanks, TILE_SIZE, slicesPerBank_);
    sliceAllocator_.onTextureRecreated = [this] {
        syncBanksToToolContext();
        fillTool_.setTextures({layerTexBanks[0], maskTex, wallTex,
                                 outerJfaTex, innerJfaTex, sdfTex});
    };

    // maskTex (R8)
    // フィルタはlayerTexArray(GL_LINEAR)に合わせる。ここをGL_NEARESTのままにすると、
    // ストローク中のプレビュー(render.fragがmaskTexをtexture()でサンプルする経路)だけ
    // ズーム時にドット単位でカクカクした境界になり、ベイク後(同じアルファ値が
    // layerTexArrayへ焼き込まれ、GL_LINEARで滑らかに補間される)と見た目が変わってしまう。
    // ストローク色バッファはキャンバスと寿命を共にするが、使う設定になるまで確保しない
    // (ensureStrokeColorTex参照)。ここではハンドルを空にしておくだけ。
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
    // uDispatchOriginはプログラムオブジェクトに紐づくuniform状態であり、
    // AirbrushTool(スタンプごとに部分範囲だけ処理するため非ゼロ値を設定する)が
    // 最後にこのプログラムを使った際の値がそのまま残っていることがある。
    // ここはキャンバス全域を対象にした呼び出しなので、明示的に(0,0)へ戻す。
    {
        GLint loc = glGetUniformLocation(computeMaskClearProgram->programId(), "uDispatchOrigin");
        if (loc >= 0) glUniform2i(loc, 0, 0);
    }
    glDispatchCompute((canvasW + 15) / 16, (canvasH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    computeMaskClearProgram->release();

    // タイルグリッドを doc に登録
    doc_->initTileGrid(canvasW, canvasH);
 
    // layerTexArray（mipなし）
    sliceAllocator_.createInitial(16);

    int mipLevels = 1 + (int)std::floor(std::log2(std::max(canvasW, canvasH)));
    glGenTextures(1, &compositedTex);
    glBindTexture(GL_TEXTURE_2D, compositedTex);
    glTexStorage2D(GL_TEXTURE_2D, mipLevels, GL_RGBA8, canvasW, canvasH);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // キャッシュ用
    int tileCount = doc_->tilesX() * doc_->tilesY();

    // ナビゲーター用作業テクスチャ(以前は updateCompositedTex() 内で遅延生成していたが、
    // CanvasCompositor はテクスチャを所有しないため、ここで確保しておく)
    glGenTextures(1, &compositedTileArr);
    glBindTexture(GL_TEXTURE_2D_ARRAY, compositedTileArr);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, TILE_SIZE, TILE_SIZE, tileCount);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // 塗りつぶし用
    wallTex     = makeTexture2D(GL_R8,    canvasW, canvasH);
    outerJfaTex = makeTexture2D(GL_RG16F, canvasW, canvasH);
    innerJfaTex = makeTexture2D(GL_RG16F, canvasW, canvasH);
    sdfTex      = makeTexture2D(GL_R32F,  canvasW, canvasH);

    fullLayerTex = makeTexture2D(GL_RGBA8, canvasW, canvasH);

    // 「アクティブレイヤーより下」の合成結果キャッシュ(item4)。キャンバスサイズに
    // 連動するので他の canvasW x canvasH テクスチャと同じくここで確保する
    // (中身は使われる直前に updateBelowCompositeCache() が必ず書くので、
    // 初期値は未定でよい)。
    // フィルターは GL_LINEAR にしておく必要がある。これらは render.frag が
    // 画面表示のために直接サンプリングするテクスチャで、キャッシュを使わない経路が
    // 読むレイヤーのタイル配列も GL_LINEAR(LayerSliceAllocator参照)だからである。
    // 既定の GL_NEAREST のままだと、キャッシュが有効になった瞬間(=ストローク開始)に
    // 表示だけが最近傍補間に切り替わり、キャンバス全体のアンチエイリアスが
    // 失われたように見える(レイヤー操作やUndoでキャッシュが破棄されると戻る)。
    belowCompositeTex         = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
    belowCompositeClipBaseTex = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
    aboveCompositeTex         = makeTexture2D(GL_RGBA8, canvasW, canvasH, GL_LINEAR);
    aboveCompositeClipScratch_ = makeTexture2D(GL_RGBA8, canvasW, canvasH);
    belowCompositeCacheValid_ = false;
    aboveCompositeCacheValid_ = false;

    // 選択範囲マスク(R8)。未選択状態=全域255(どこでも塗れる)で初期化する。
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

    // 変形ツール確定時の作業用スナップショット(初期値は使われないので未初期化のままでよい)
    transformSrcTex        = makeTexture2D(GL_RGBA8, canvasW, canvasH);
    transformSrcSelMaskTex = makeTexture2D(GL_R8,    canvasW, canvasH);
    transformScratchW_ = canvasW;
    transformScratchH_ = canvasH;

    // SSBO 初期化 (すべてMAX_LAYERS個ぶん、フラットなレイヤーindexでそのまま引ける)
    glGenBuffers(1, &ssboLayerOpacity);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerOpacity);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerVisible);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerVisible);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssboLayerBaseSlice);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerBaseSlice);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    // レイヤーマスクのベーススライス(-1なら無し)。binding=1は他のSSBOと違いテクスチャ/
    // 画像の名前空間とも重ならない空きスロット(CanvasCompositor.cppのコメント参照)。
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

    // vec3配列はstd430で要素ストライドが16バイト(vec4扱い)になるため、
    // レイヤーあたりfloat4分(x,y,z,pad)を確保する。
    glGenBuffers(1, &ssboLayerAdjParams);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerAdjParams);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    // フォルダー単位のマスクカスケード用(ivec4、レイヤーあたり最大4階層ぶんの
    // 祖先フォルダーのマスクベーススライス)。
    glGenBuffers(1, &ssboLayerAncestorMaskSlices);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssboLayerAncestorMaskSlices);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_LAYERS * 4 * sizeof(int), nullptr, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // レイヤー作成(この後 CacheAndNotify通知が飛ぶ)より前に toolCtx_ を実体で埋める。
    // ここまでに作ったテクスチャ/SSBO/シェーダーが出揃っているため。
    setupToolContext();

    // addLayer()自体がdoc_->onChanged経由でupdate()を呼び得るため、
    // レイヤーがまだ0件のこの時点で誤発火しないよう、
    // 初期構築が終わるまでは先にガードを立てておく
    if (createDefaultLayers) {
        m_initializing = true;

        // 最初のレイヤーを2枚作成 (下: 常に白い単色レイヤー、上: 透明な作業レイヤー)
        addSolidColorLayer("用紙");
        addLayer("レイヤー1");
        doc_->setActiveLayer(1);

        m_initializing = false;
    }
    // createDefaultLayers=false の場合はレイヤーを1枚も作らずに返す
    // (キャンバスサイズ変更アクションが、既存レイヤーの中身を復元するため)

    update();
}

// ナビゲーター用: 全レイヤー合成して compositedTex を更新
void CanvasWidget::updateCompositedTex() {
    makeCurrent();
    compositor_.updateCompositedTex(toolCtx_);
}

// ===========================================================================
// 変形確定用の作業テクスチャを、指定サイズに合わせて作り直す
// (レイヤーの矩形がgrowLayerBoundsで変わるため、キャンバスサイズ固定では足りない)
// ===========================================================================
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

// ===========================================================================
// 多段パスのフィルター用の中間バッファを確保する(CanvasWidget.hのコメント参照)
// ===========================================================================
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

// ===========================================================================
// レイヤーの矩形を、キャンバスタイル座標系で指定範囲を覆うように拡張する
// (CanvasDocument::growLayerBoundsへ委譲し、GPU側のタイルコピー/クリアを担当する)
// ===========================================================================
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

// ===========================================================================
// リサイズ / 描画
// ===========================================================================
void CanvasWidget::resizeGL(int w, int h) {
    // モニター間の移動や表示スケール変更でDPRが変わるので、ここで追従させる
    // (ツール側はこの値でマウス座標をビュー空間へ換算する)。
    toolCtx_.viewDpr = viewDpr();
    // w/hは論理px。実際のFBO用viewportはpaintGL()冒頭で物理pxへ明示的に揃える。
    Q_UNUSED(w);
    Q_UNUSED(h);
    fitCanvasToView();
    actions_.positionActivePanel(); // 変形/色調整/フィルター/レイヤー編集系(controller管理アクション)
}

void CanvasWidget::fitCanvasToView()
{
    // ビューはデバイスピクセルで動く(CanvasWidget.hのviewDpr()のコメント参照)
    const float w = viewWidth();
    const float h = viewHeight();
    if (w <= 0.0f || h <= 0.0f) return;

    float scaleX = w / canvasW;
    float scaleY = h / canvasH;
    float s = qMin(scaleX, scaleY) * 0.9f;

    view_.setScale(s);
    // キャンバス中心をウィジェット中心に合わせる
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

    // ViewTransform::matrix()は「回転→スケール(反転)→平行移動(offset)」の順で
    // 合成されており(T*S*R)、offsetは最後に screen 空間へそのまま足される。
    // そのため反転(xスケールの符号反転)だけをそのまま行うと、キャンバス原点(0,0)が
    // 画面上のどこにあるか(=offset.x)を軸にミラーされてしまい、パンしていると
    // 変な位置を軸に反転して見える。
    // screen = offset + S(±scale,scale)*R(rot)*canvasPos なので、反転前後で
    // 「現在画面中央に映っている点」が画面中央に留まるようにするには、
    // offset.x を ウィジェット幅基準で鏡映(offset.x -> width - offset.x)させれば
    // よい(この補正はoffset.yやcanvasPos自体に依存しない、常に成り立つ関係式)。
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
    // ビュー空間(デバイスpx)の四隅
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
    // visibleCanvasRectPolygon()と同じ規則(Y下向き⇔ViewTransform内部のY上向き)で
    // 符号を揃えてから、平行移動を除いた線形部分(回転・拡縮・左右反転)だけを
    // screen空間(offset_と同じ座標系)へ写像し、offsetに加算する。
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

    // Qtが設定するviewportは小数DPRで切り捨てられることがあり、実際のFBOより
    // 1px以上小さくなる。さらにドック操作でQOpenGLWidgetが拡大した直後には古い
    // viewportが残る環境があり、増えた領域が黒い帯になる。毎フレーム、Qtと同じ
    // 丸め方でFBO全体へ明示的に揃える。
    const qreal currentDpr = devicePixelRatioF();
    glViewport(0, 0, qMax(1, qRound(width() * currentDpr)),
                     qMax(1, qRound(height() * currentDpr)));

    paintGlCalls_++;
    // ドラッグ中に、こちらが呼んだ同期repaint()以外の経路(どこかのupdate())で
    // 描かれたフレーム。同じ絵をもう1枚描くだけの無駄で、1枚につきウィンドウ全体の
    // 再合成とpresentが増える。ここが並ぶようなら、その update() を止められないかを
    // 疑うこと(inputFlushTimer_ / requestRepaint の各コメント参照)。
    if (!inSyncRepaint_ && WinLog::enabled() && viewDiagCount_ < 40) {
        viewDiagCount_++;
        WINLOG(QStringLiteral("PERF view: 余分なpaintGL(update由来)"));
    }

    // 計測用(初回のみ): 実際に描き込んでいるフレームバッファの画素数を問い合わせる。
    // ここが「ウィジェットの論理サイズ」なのか「デバイスピクセル(論理×DPR)」なのかで、
    // キャンバスが実画素どおりに出ているかが決まる。
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
            // presentが1回30ms近くかかっている件の切り分け用。ここが1なら、
            // main.cppでswapInterval(0)にしたつもりが効いていない(=垂直帰線待ち)。
            const QWindow *topWin = window() ? window()->windowHandle() : nullptr;
            WINLOG(QStringLiteral("PERF swapInterval: default=%1 widgetCtx=%2 topWindow=%3")
                       .arg(QSurfaceFormat::defaultFormat().swapInterval())
                       .arg(context() ? context()->format().swapInterval() : -1)
                       .arg(topWin ? topWin->format().swapInterval() : -1));
        }
    }

    // paintGL本体の所要時間。ストローク中の同期描画間隔の適応に使う
    // (CanvasWidget.hのlastPaintGlCostNs_のコメント参照。repaint()全体の時間ではなく
    //  ここを測るのが要点)。
    QElapsedTimer paintClock;
    paintClock.start();
    const bool logPaint = WinLog::enabled() && strokeFrameLogCount_ < 12;

    // 【重要】Qtがこのフレーム用に束縛したFBOを控えておく。
    //
    // この関数はこの後 flushPendingInput() と prepareCompositeBase() を呼ぶが、
    // その先(StrokeUndoRecorder::expandRegion のタイル読み戻し、AirbrushToolの
    // スタンプ焼き込み、CanvasCompositorの各種オフスクリーン合成)は一時FBOを使い、
    // 後始末でフレームバッファのバインドを 0 に戻す。それらは元々「マウスイベント
    // 処理中に呼ばれる(=次のpaintGL()が改めて自前のFBOを束縛し直す)」前提で
    // 書かれていたが、実際にはこの関数の中からも呼ばれる経路がある。
    // 戻さないままだと、このフレームのclear/drawがウィジェットのFBOではなく
    // フレームバッファ0へ行き、描画結果がまるごと捨てられる。
    // 症状: ストローク開始直後の部分再描画(シザー)が画面に出ず、あとで全面
    // 再描画が来たときに初めてポンと現れる。以前は毎フレーム余分な全面再描画が
    // 走っていたためそれに隠れていたが、その無駄を削った結果表に出た。
    GLint prevDrawFbo = 0, prevReadFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);

    // フレームレート律速バッチ(Tool::flushPendingInput参照)。ペンタブの高頻度
    // サンプルをonMouseMove()側で貯めておき、実際に画面を更新するこのタイミングで
    // まとめて1回だけGPUディスパッチする(この中からnoteStrokeDirtyRegion()が
    // 呼ばれて下のシザー矩形が更新されるため、必ずシザー計算より先に行う)。
    if (Tool *t = currentTool()) t->flushPendingInput(toolCtx_);

    // ストローク中の部分再描画: 前回paint以降に実際に変更されたキャンバス領域が
    // 分かっている場合、その矩形(+マージン)だけをシザーで描き直し、残りは前フレームの
    // FBO内容(PartialUpdate)をそのまま使う。1回の描画コストが「ウィンドウ全画素×
    // アクティブ以上のレイヤー数」から「ブラシ周辺×同」へ激減し、レイヤー数や
    // ウィンドウサイズにほぼ依存しなくなる(他のペイントソフトと同じダーティ矩形方式)。
    bool partialPaint = false;
    int  scissorW = 0, scissorH = 0; // 診断ログ用
    if (strokeDirtyValid_) {
        Tool *t = currentTool();
        if (t && t->isActive()) {
            // キャンバスpx(Y上向き) → ウィジェット座標。回転・反転があっても正しく
            // 覆えるよう、矩形の4隅を変換してそのバウンディングボックスをとる。
            const QPointF c0 = toolCtx_.pixelToWidget(QVector2D(strokeDirtyMinX_, strokeDirtyMinY_));
            const QPointF c1 = toolCtx_.pixelToWidget(QVector2D(strokeDirtyMaxX_ + 1.0f, strokeDirtyMinY_));
            const QPointF c2 = toolCtx_.pixelToWidget(QVector2D(strokeDirtyMinX_, strokeDirtyMaxY_ + 1.0f));
            const QPointF c3 = toolCtx_.pixelToWidget(QVector2D(strokeDirtyMaxX_ + 1.0f, strokeDirtyMaxY_ + 1.0f));
            const qreal margin = 2.0;
            const qreal wx0 = qMin(qMin(c0.x(), c1.x()), qMin(c2.x(), c3.x())) - margin;
            const qreal wy0 = qMin(qMin(c0.y(), c1.y()), qMin(c2.y(), c3.y())) - margin;
            const qreal wx1 = qMax(qMax(c0.x(), c1.x()), qMax(c2.x(), c3.x())) + margin;
            const qreal wy1 = qMax(qMax(c0.y(), c1.y()), qMax(c2.y(), c3.y())) + margin;

            // ウィジェット座標(Y下向き) → FBO座標(Y上向き、物理px)
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

    // フィルターレイヤーがある文書では、ここでオフスクリーンの連鎖を組み直して
    // 「どこまで合成済みか」と、その結果テクスチャを受け取る。フィルターが無ければ
    // 従来通りストローク中の下キャッシュだけを見る(戻り値<0で事前合成なし)。
    // renderProgram をbindする前に済ませること(内部で別のプログラムをbindするため)。
    GLuint compositeBaseTex     = belowCompositeTex;
    GLuint compositeBaseClipTex = belowCompositeClipBaseTex;
    const int compositeStartZ   = prepareCompositeBase(compositeBaseTex, compositeBaseClipTex);
    const qint64 nsAfterBase    = logPaint ? paintClock.nsecsElapsed() : 0;

    // 上のflushPendingInput()/prepareCompositeBase()が一時FBOを使って
    // バインドを外していたら、ここでこのフレーム用のFBOへ戻す(理由は関数冒頭の
    // prevDrawFbo のコメント参照)。以降のclear/drawがこのFBOへ入る。
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
    // ビューポート(=FBO)はデバイスピクセルなので、ここも合わせる。論理pxを渡すと
    // 拡大率1.0でも常にDPR倍へ拡大リサンプルされる(CanvasWidget.hのviewDpr()参照)。
    renderProgram->setUniformValue("uWindowSize",        QVector2D(viewWidth(), viewHeight()));
    // 拡大表示中だけ画素中心へ吸着させる判定に使う(render.frag の uViewScale 参照)
    renderProgram->setUniformValue("uViewScale",         view_.scale());
    // 縮小表示時の面積平均のサンプル数(1辺)。1画素が覆うキャンバスpxぶんを刻む。
    // 縮小するほどキャンバスが占める画面画素数は倍率の二乗で減るので、
    // 1画素あたりのサンプル数を1/倍率まで増やしても画面全体の処理量はほぼ一定。
    // 上限4は保険(極端な縮小でサンプル数が爆発しないように)。
    int minifySamples = qBound(1, (int)std::ceil(1.0f / qMax(view_.scale(), 0.0001f)), 4);

    // 【重要】キャンバスを移動/回転している間は1点サンプルまで落とす。
    //
    // このサンプル数は1画面画素あたりの composeCanvasAt() 呼び出し回数を
    // 二乗で増やす(3なら9回、上限の4なら16回)。実測(35%表示・キャンバス全面)で
    // この描画のGPU実処理は
    //     minify=1 → 約10ms / minify=2 → 約29ms / minify=3 → 約60ms
    // で、ほぼサンプル数の二乗に比例していた(TIEPOLO_GLFINISH=1 で実測)。ビュー変換のドラッグ中は
    // 毎フレーム全面を描き直す(ストロークのような部分再描画が効かない)ので、
    // これがそのまま1フレームの時間になり、60ms=約16fpsまで落ちる。これが
    // 「ビュー変換がカクつく」の本体だった。
    //
    // 面積平均は「止まっている絵の斜め線を階段状に見せない」ための品質処理で、
    // 動かしている最中は見えない。動かしている間だけ落とし、指を離した時点の
    // 再描画(mouseReleaseEvent)で本来の品質に戻す。
    //
    // 【計測の注意】paintGL本体の所要時間(lastPaintGlCostNs_)にはこのコストは
    // 現れない。GLの呼び出しは非同期で、CPUは命令を積んだら戻ってくるため
    // (実測0.3ms)。GPUの実処理はその後のpresentで待たされる形で現れるので、
    // 一見「Qtのウィンドウ合成が遅い」ように見える。切り分けるには
    // TIEPOLO_GLFINISH=1 を使うこと(下の glFinish のところ)。
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

    // ブラシ色。消しゴムツールと透明色は「消す」動作になり、そのときの uBrushColor.a は
    // 色の不透明度ではなく「消す強さ」を表す(ToolConfig.h の EraseBrush / bake.comp 参照)。
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
    // スタンプごとの色(色のランダム等)。焼き込み側(PenEraserTool)と同じ判定にする。
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

    // レイヤーマスク編集中のライブプレビュー用。焼き込み側(PenEraserTool::onMouseRelease /
    // AirbrushTool::stampAndBake)がbake.compへ渡すのと同じ色をシェーダーへ渡し、
    // render.frag側で同じ式を先回りして「焼き込んだらこうなる」マスクを表示させる
    // (両者がずれるとプレビューと確定結果が食い違う。MaskBrush.hのコメント参照)。
    // マスクは色ではなく濃淡なので、「消す」ときは黒(=隠す)へその強さで寄せる。
    QColor maskCol = erase.active
        ? maskBrushColor(true, toolCfg_->color().rawRGBA(), erase.strength)
        : maskBrushColor(false, toolCfg_->color().rawRGBA(), brushOpacity);
    renderProgram->setUniformValue("uMaskBrushColor",
        maskCol.redF(), maskCol.greenF(), maskCol.blueF(), maskCol.alphaF());

    renderProgram->setUniformValue("uHasSelection",    hasSelection_ ? 1 : 0);
    renderProgram->setUniformValue("uIsSelectionTool", (activeTool == ToolType::Selection) ? 1 : 0);

    // 変形/自由変形/色調整/フィルター系(拡大・縮小・回転/自由変形/色相・彩度・明度/
    // 明るさ・コントラスト/カラーバランス/トーンカーブ/ガウスぼかし/モザイク/
    // カスタムシェーダー/色収差)の uIsXxxTool + origin/size 等の uniform は、
    // 各 CanvasAction (src/actions/) の applyRenderState() へ移動した。非アクティブな
    // アクションも自分の uniform を 0 に戻すため、登録済み全アクションに対して
    // 毎フレーム呼ぶ(コントローラ側の実装を参照)。
    actions_.applyRenderState(renderProgram);

    // 表示上の見た目だけを変えるカラーモード(RGB/CMYK擬似/グレースケール)。
    // ツールのactivate/deactivateとは無関係に、常にtoolCfg_->colorMode()の現在値を反映する。
    renderProgram->setUniformValue("uColorMode", (int)toolCfg_->colorMode().mode());

    // モニターキャリブレーション。値域はBrightnessContrastTool/ColorBalanceToolと
    // 同じ-100〜100の整数なので、同じ換算式でfloatに直す(CanvasWidget.cpp内の
    // uBrightnessShift/uContrastFactor/uCyanShift等の設定箇所と揃えてある)。
    const CalibrationConfig &cal = toolCfg_->calibration();
    renderProgram->setUniformValue("uCalBrightness", cal.brightness() / 200.0f);
    renderProgram->setUniformValue("uCalContrast",   1.0f + cal.contrast() / 100.0f);
    renderProgram->setUniformValue("uCalCyan",       cal.cyan()    / 100.0f);
    renderProgram->setUniformValue("uCalMagenta",    cal.magenta() / 100.0f);
    renderProgram->setUniformValue("uCalYellow",     cal.yellow()  / 100.0f);

    // キャンバス外側の背景色(実データには無関係の表示設定)。マスク編集中は
    // UI共通の淡いブルーへ切り替え、描画先がレイヤー本体かマスクかをキャンバスを
    // 見るだけで判別できるようにする。文書の画素・保存データには影響しない。
    {
        const QColor bg = editingMaskLayerIndex_ >= 0
            ? Theme::accentHoverLight
            : toolCfg_->canvasBackground().color();
        renderProgram->setUniformValue("uCanvasOutsideBg",
            QVector3D(bg.redF(), bg.greenF(), bg.blueF()));
    }

    // 市松模様の2色(先端画像プレビュー・レイヤープレビューと共通のTheme値)
    renderProgram->setUniformValue("uCheckerColorA",
        QVector3D(Theme::checkerDark.redF(), Theme::checkerDark.greenF(), Theme::checkerDark.blueF()));
    renderProgram->setUniformValue("uCheckerColorB",
        QVector3D(Theme::checkerLight.redF(), Theme::checkerLight.greenF(), Theme::checkerLight.blueF()));

    // binding 番号は render.frag の layout(binding=N) に合わせる。
    // タイル配列(旧 layerTexArray)は複数バンクへ分割されたので、専用ヘルパーで
    // ユニット8..15へまとめてバインドし sampler2DArray 配列 uniform を設定する。
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

    // item4: ストローク中(PenEraserToolがupdateBelowCompositeCache()で埋めた間)は、
    // 「アクティブレイヤーより下」をキャッシュテクスチャから読み、render.frag側は
    // z=アクティブレイヤー以降だけを合成し直す。フィルターレイヤーがある文書では
    // 代わりに連鎖の結果(=一番上のフィルターレイヤーまで合成済み)から始める。
    // どちらでもなければ毎フレーム通常通りz=0から全レイヤーを合成する。
    renderProgram->setUniformValue("uUseBelowCompositeCache", compositeStartZ >= 0 ? 1 : 0);
    renderProgram->setUniformValue("uCompositeStartZ",        compositeStartZ >= 0 ? compositeStartZ : 0);
    renderProgram->setUniformValue("uUseAboveCompositeCache", aboveCompositeCacheValid_ ? 1 : 0);

    glBindVertexArray(dummyVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    // 【計測用】TIEPOLO_GLFINISH=1 のときだけ有効。
    //
    // GLの呼び出しは非同期なので、paintGL本体の所要時間にはGPUの実処理が含まれない
    // (CPUが命令を積むまでの時間しか測れていない。実測0.3ms)。実処理はその後の
    // presentで待たされる形で現れるため、放っておくと「Qtのウィンドウ合成が遅い」と
    // 誤読する ―― 実際その取り違えをした。ここで待たせると、この描画のGPU実処理が
    // paintGL 側に現れて切り分けられる(常用すると描画が直列化して遅くなるので、
    // 計測のときだけ付けること)。
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
        // 前フレームのFBOに描かれた分がそのまま残っており、ストローク中に
        // オーバーレイの形状は変わらないため見た目は維持される(破線のアニメーション
        // だけ一時停止するが実害はない)。ストローク終了後の通常paintで再開する。
        glDisable(GL_SCISSOR_TEST);
        lastPaintGlCostNs_ = paintClock.nsecsElapsed();
        paintGlNsAccum_ += lastPaintGlCostNs_;
        return;
    }

    // 投げ縄選択の軌跡プレビューなど、GLの描画が終わった後にQPainterで重ねる
    // オーバーレイ(ほとんどのツールはpaintOverlay()が空実装なので何も描かれない)。
    // 変形アクション実行中はactiveToolに関係なくtransformTool_の枠を描く。
    //
    // 【試して駄目だったこと】ビュー変換のドラッグ中にこのブロックを丸ごと省いて
    // みたが、1フレームは 22.5ms → 22.4ms でまったく変わらなかった(2026-08-12)。
    // ここは重くない。
    {
        QPainter painter(this);
        paintSelectionOutline(painter);
        if (!actions_.paintActiveOverlay(painter, toolCtx_)) {
            // controller管理アクション(変形/自由変形/キャンバスサイズ/色収差の円形ハンドル等)
            // が何も描かなければ、通常ツールのオーバーレイを描く。
            if (Tool *t = currentTool()) t->paintOverlay(painter, toolCtx_);
        }
    }
    lastPaintGlCostNs_ = paintClock.nsecsElapsed();
    paintGlNsAccum_ += lastPaintGlCostNs_;
}

// 選択範囲マスク(selectionMaskTex)の境界形状(キャンバスpx座標系)をキャッシュへ
// 再計算する。QRegion(QBitmap::fromImage(...))で境界を求めることで、投げ縄/ペン選択の
// ような任意形状にも正しく対応する。selectionMaskTexの中身が実際に変わった時
// (selectAll/clearSelection/投げ縄・ペン選択の確定等)だけ呼べばよい重い処理
// (glGetTexImage+全ピクセル走査+QRegion分解)なので、paintSelectionOutline()から
// 分離してある。
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
    // QBitmap::fromImage()は全面白(=全域選択。全選択アクション直後がこれにあたる)の
    // 画像を変換すると空のQRegionになる(QBitmapは伝統的に「黒=set」の解釈を持つため、
    // 全白画像には有効ビットが1つも無いと判定される)。ここでhasSelection_は既にtrueと
    // 分かっているため(関数冒頭でfalseなら早期return済み)、regionが空ならそれは
    // 「選択が無い」のではなく「キャンバス全域が選択されている」ことを意味する。
    // その場合はキャンバス全体を囲む矩形を輪郭として使う。
    QPainterPath canvasPath;
    if (region.isEmpty()) {
        canvasPath.addRect(0, 0, canvasW, canvasH);
    } else {
        canvasPath.addRegion(region);
        // addRegion()は矩形の集合(1走査行の連続run単位でまとめられるため、斜めの辺を
        // 持つ形状だと行ごとに別々の矩形になり、多いと数百枚)をそのまま返す。この矩形群を
        // simplified()せずに輪郭線として描画すると、隣り合う矩形どうしの内部辺(本来は
        // 見えるべきでない、形状内部の水平な仕切り線)まで大量に重なって描かれてしまい、
        // 選択範囲の縦幅ぶんが太い帯のように潰れて見えてしまっていた。simplified()で
        // 矩形群を1つの輪郭(外周だけ)に統合してから描画する。
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

    // 変形中は selectionMaskTex 自体がまだ動いていない(確定時に transform.comp が
    // 動かす)ため、輪郭キャッシュも元の位置のまま。何もしないとドラッグ中だけ点線が
    // 取り残されるので、アクティブなアクションに各点をプレビューと同じ変換で写して
    // もらう(CanvasAction::mapSelectionOutlinePoint参照。変形系以外は何もしない)。
    CanvasAction *activeAct = actions_.activeAction();

    // 【軽量化】ウィジェット座標へ変換したパスをキャッシュする。
    //
    // 輪郭キャッシュ(selectionOutlineCachePx_)はキャンバスpx座標系なので、描くには
    // ウィジェット座標へ移す必要がある。以前はこれを毎フレーム「点ごとに」やっていたが、
    // ペンで作った選択範囲はQRegion由来で数千セグメントになるため、この作り直しだけで
    // 1フレーム十数msかかっていた(実測で paintGL の97%がこのオーバーレイだった)。
    //
    // 実際にパスが変わるのは「選択範囲そのものが変わった(dirtyフラグ)」「ビュー変換が
    // 変わった(パン・ズーム・回転・反転)」「ウィジェットサイズが変わった」ときだけ。
    // 投げ縄のドラッグ中はどれも変わらないので、作り直しは完全に無駄だった。
    // 変形アクション中だけは毎フレーム写る位置が変わるのでキャッシュしない。
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
        // 変形中は次フレームも作り直す必要があるのでキャッシュ有効にはしない
        selectionOutlineWidgetPathValid_ = !mapsPerFrame;
    }
    const QPainterPath &widgetPath = selectionOutlineWidgetPath_;

    // マーチングアンツはQRegionの境界(=軸平行の1px線)なので、アンチエイリアスを
    // 掛けても見た目はほぼ変わらないのに、数千セグメントの破線ストロークでは
    // ラスタライズ費用が跳ね上がる。切っておく。
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

// ===========================================================================
// マウスイベント
// ===========================================================================
// mousePressEvent/mouseMoveEventのどちらでも使う、このイベントに適用すべき筆圧。
//
// 以前はQMouseEvent::source()で「タブレット由来の合成イベントか、素のマウスか」を
// 判定し(NotSynthesizedなら素のマウスとみなして1.0にリセット)、素のマウスならば
// event->points().first().pressure()を、そうでなければtabletEvent()由来の値を
// 使い分けていた。しかしWindows環境ではOSのペン→マウス互換レイヤーが、Qtが
// タブレットイベントから合成する正規のマウスイベントとは別に、同じ物理接触に対して
// 本物の(=event->source()では区別が付かない)マウスイベントを重複して送ってくる
// ことがある。この重複イベントのpoints().first().pressure()は実際の筆圧を反映して
// いないことが多く、従来のロジックでは「タブレットで描いている最中なのに素の
// マウス操作と誤認してフル筆圧(1.0)を使ってしまう」ことがあった
// (書き始め・ストローク中を問わず、太い点が混じる不具合の原因)。
//
// tabletEvent()は同じ物理サンプルについて必ずこれらマウスイベントより先に届き、
// 実際のハードウェア筆圧を直接持っている。直近(数十ms以内)にtabletEvent()が
// 届いていればそれを信頼できるタブレット操作中とみなしてその値を使い、
// 届いていなければ素のマウス操作とみなしてevent->points()の値(通常1.0)を使う。
