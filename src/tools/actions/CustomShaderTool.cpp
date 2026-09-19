#include "tools/actions/CustomShaderTool.h"

#include <QFile>
#include <QRegularExpression>

// ---------------------------------------------------------------------------
// テンプレート: ユーザーが書いた「vec4 userMain(vec2 uv, vec2 res, vec4 src)」を
// 呼び出すだけのコンピュートシェーダー本体。gaussianBlurFilter.compと同じ
// uSrc(読み取り専用)/uDst(書き込み専用)/selMask の3枚構成。
// ---------------------------------------------------------------------------
static const char *kShaderHeader = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;
layout(rgba8, binding = 0) uniform readonly  image2D uSrc;
layout(rgba8, binding = 1) uniform writeonly image2D uDst;
layout(r8,    binding = 2) uniform readonly  image2D selMask;
uniform ivec2 uSelMaskOffset;

)GLSL";

static const char *kShaderMain = R"GLSL(

void main() {
    ivec2 p  = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(uSrc);
    if (p.x >= sz.x || p.y >= sz.y) return;

    vec4 srcPremul = imageLoad(uSrc, p);
    vec4 src = vec4(safeUnpremul(srcPremul.rgb, srcPremul.a), srcPremul.a);

    vec2 res = vec2(sz);
    vec2 uv  = (vec2(p) + 0.5) / res;
    // 中央が原点(0,0)で、縦横比を保った(潰れない/伸びない)座標。短い方の辺が
    // ちょうど[-1, 1]になるよう正規化する(長い方の辺はその比率ぶんだけ広がる)。
    // uvだけだと正方形でないレイヤーで円が楕円に潰れてしまうため、その対策として
    // 併せて渡す。
    vec2 pos = (vec2(p) + 0.5 - res * 0.5) / min(res.x, res.y) * 2.0;

    vec4 outc = clamp(userMain(uv, pos, res, src), 0.0, 1.0);
    vec4 result = vec4(outc.rgb * outc.a, outc.a);

    ivec2 selCoord = p + uSelMaskOffset;
    ivec2 selSz = imageSize(selMask);
    float sel = 0.0;
    if (selCoord.x >= 0 && selCoord.y >= 0 && selCoord.x < selSz.x && selCoord.y < selSz.y)
        sel = imageLoad(selMask, selCoord).r;

    imageStore(uDst, p, mix(srcPremul, result, sel));
}
)GLSL";

QString CustomShaderTool::defaultSource()
{
    return QStringLiteral(
        "vec4 main(vec2 uv, vec2 pos, vec2 res, vec4 src) {\n"
        "    return src;\n"
        "}\n");
}

void CustomShaderTool::initialize(QOpenGLContext *ctx)
{
    Q_UNUSED(ctx);
    initializeOpenGLFunctions();
}

void CustomShaderTool::releaseGL()
{
    delete dynamicProgram_;
    dynamicProgram_ = nullptr;
}

QString CustomShaderTool::setSource(ToolContext &ctx, const QString &userSrc)
{
    static const QRegularExpression re(QStringLiteral(R"(\bvec4\s+main\s*\()"));
    QRegularExpressionMatch m = re.match(userSrc);
    if (!m.hasMatch())
        return QStringLiteral("vec4 main(vec2 uv, vec2 pos, vec2 res, vec4 src) { ... } という形式で書いてください");

    QString wrapped = userSrc;
    wrapped.replace(m.capturedStart(), m.capturedLength(), QStringLiteral("vec4 userMain("));

    QFile commonFile(QStringLiteral(":/shaders/header/common.glsl"));
    if (!commonFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return QStringLiteral("common.glslの読み込みに失敗しました");
    const QByteArray common = commonFile.readAll();

    QByteArray full;
    full += "#version 430 core\n";
    full += common;
    full += kShaderHeader;
    full += wrapped.toUtf8();
    full += kShaderMain;

    auto *prog = new QOpenGLShaderProgram();
    prog->addShaderFromSourceCode(QOpenGLShader::Compute, full);
    if (!prog->link()) {
        const QString log = prog->log();
        delete prog;
        return log.isEmpty() ? QStringLiteral("コンパイルに失敗しました") : log;
    }

    delete dynamicProgram_;
    dynamicProgram_ = prog;
    source_ = userSrc;

    if (engaged_) updatePreview(ctx);
    return QString();
}

void CustomShaderTool::activate(ToolContext &ctx)
{
    layerIndex_ = ctx.doc->activeLayerIndex();
    const Layer &layer = ctx.doc->layers[layerIndex_];
    layerW_ = layer.tilesX() * ctx.tileSize;
    layerH_ = layer.tilesY() * ctx.tileSize;
    previewOriginPx_ = QVector2D((float)(layer.originTx * ctx.tileSize), (float)(layer.originTy * ctx.tileSize));

    engaged_ = (layerW_ > 0 && layerH_ > 0);
    if (!engaged_) return;

    ctx.ensureTransformScratchSize(layerW_, layerH_);

    // 1. アクティブレイヤーのタイルを fullLayerTex へ展開する(GaussianBlurToolと同じ)。
    const GLuint defaultFbo = ctx.defaultFbo();
    {
        GLuint srcFbo = 0, dstFbo = 0;
        glGenFramebuffers(1, &srcFbo);
        glGenFramebuffers(1, &dstFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.fullLayerTex, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        for (int ty = 0; ty < layer.tilesY(); ty++)
            for (int tx = 0; tx < layer.tilesX(); tx++) {
                int si = layer.tiles[ty][tx];
                int dstX = tx * ctx.tileSize, dstY = ty * ctx.tileSize;
                glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                glBlitFramebuffer(0, 0, ctx.tileSize, ctx.tileSize,
                                  dstX, dstY, dstX + ctx.tileSize, dstY + ctx.tileSize,
                                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &srcFbo);
        glDeleteFramebuffers(1, &dstFbo);
    }
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    // 2. まだ有効なコンパイル済みシェーダーが無い間でもconfirm()が安全な no-op になるよう、
    // transformSrcTexへ元画像をそのままコピーしておく。
    {
        GLuint srcFbo = 0, dstFbo = 0;
        glGenFramebuffers(1, &srcFbo);
        glGenFramebuffers(1, &dstFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.fullLayerTex, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.transformSrcTex, 0);
        glBlitFramebuffer(0, 0, layerW_, layerH_, 0, 0, layerW_, layerH_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &srcFbo);
        glDeleteFramebuffers(1, &dstFbo);
    }

    if (dynamicProgram_) updatePreview(ctx);
}

void CustomShaderTool::deactivate()
{
    engaged_ = false;
    layerIndex_ = -1;
}

void CustomShaderTool::updatePreview(ToolContext &ctx)
{
    const Layer &layer = ctx.doc->layers[layerIndex_];

    glBindImageTexture(0, ctx.fullLayerTex,     0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA8);
    glBindImageTexture(1, ctx.transformSrcTex,  0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(2, ctx.selectionMaskTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R8);

    dynamicProgram_->bind();
    {
        GLint loc = glGetUniformLocation(dynamicProgram_->programId(), "uSelMaskOffset");
        if (loc >= 0)
            glUniform2i(loc, layer.originTx * ctx.tileSize, layer.originTy * ctx.tileSize);
    }
    glDispatchCompute((layerW_ + 15) / 16, (layerH_ + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    dynamicProgram_->release();
}

void CustomShaderTool::confirm(ToolContext &ctx)
{
    if (!engaged_) return;
    const Layer &layer = ctx.doc->layers[layerIndex_];
    const GLuint defaultFbo = ctx.defaultFbo();

    ctx.beginStrokeUndo();
    ctx.expandStrokeUndoRegion(0, ctx.doc->tilesX() - 1, 0, ctx.doc->tilesY() - 1);

    {
        GLuint srcFbo = 0, dstFbo = 0;
        glGenFramebuffers(1, &srcFbo);
        glGenFramebuffers(1, &dstFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx.transformSrcTex, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
        for (int ty = 0; ty < layer.tilesY(); ty++)
            for (int tx = 0; tx < layer.tilesX(); tx++) {
                int si = layer.tiles[ty][tx];
                int srcX = tx * ctx.tileSize, srcY = ty * ctx.tileSize;
                glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
                glBlitFramebuffer(srcX, srcY, srcX + ctx.tileSize, srcY + ctx.tileSize,
                                  0, 0, ctx.tileSize, ctx.tileSize,
                                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
        glDeleteFramebuffers(1, &srcFbo);
        glDeleteFramebuffers(1, &dstFbo);
    }
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    ctx.commitStrokeUndo();
    deactivate();
    ctx.requestRepaint();
    ctx.notifyLayersChanged();
}
