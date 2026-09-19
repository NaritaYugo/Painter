#include "actions/TransformActions.h"
#include "actions/CanvasActionHost.h"
#include "backend/CanvasDocument.h"

#include <QOpenGLShaderProgram>
#include <QtMath>

// ===========================================================================
// TransformAction (拡大・縮小・回転)
// ===========================================================================
bool TransformAction::canActivate() const
{
    CanvasDocument *doc = host_.hostDocument();
    return doc && !doc->layers.isEmpty();
}

bool TransformAction::onActivate()
{
    tool_.activate(host_.hostToolContext());
    return true;
}

void TransformAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
}

void TransformAction::onCancel()
{
    tool_.deactivate();
}

void TransformAction::applyRenderState(QOpenGLShaderProgram *renderProgram)
{
    const bool transformActive = isActive() && tool_.engaged();
    renderProgram->setUniformValue("uIsTransformTool", transformActive ? 1 : 0);
    if (!transformActive) return;
    const ToolContext &ctx = host_.hostToolContext();
    renderProgram->setUniformValue("uTransformPivot",     tool_.pivot());
    renderProgram->setUniformValue("uTransformRotCosSin",
        QVector2D(std::cos(tool_.rotation()), std::sin(tool_.rotation())));
    renderProgram->setUniformValue("uTransformScale",     tool_.scale());
    renderProgram->setUniformValue("uTransformC0",        tool_.center0());
    renderProgram->setUniformValue("uTransformHalfSize",  tool_.halfSize());
    renderProgram->setUniformValue("uTransformWrapX", ctx.wrapX ? 1 : 0);
    renderProgram->setUniformValue("uTransformWrapY", ctx.wrapY ? 1 : 0);
}

// 選択範囲の点線を変形プレビューへ追従させる。
// render.frag / transform.comp は「表示位置 → 変形前の位置」という逆変換で色を拾うが、
// ここで欲しいのはその逆、つまり「変形前の輪郭点 → 今表示されるべき位置」なので、
// 同じ式を順方向に組み立てる(逆変換の各ステップを逆順・逆演算でたどる)。
bool TransformAction::mapSelectionOutlinePoint(QPointF &canvasPx) const
{
    if (!isActive() || !tool_.engaged()) return false;

    const QVector2D c0    = tool_.center0();
    const QVector2D scale = tool_.scale();
    const QVector2D pivot = tool_.pivot();
    const float cosT = std::cos(tool_.rotation());
    const float sinT = std::sin(tool_.rotation());

    // 逆変換:  local = R(-θ)·(p - pivot);  srcLocal = local / scale;  src = srcLocal + c0
    // 順変換:  srcLocal = src - c0;  local = srcLocal * scale;  p = R(+θ)·local + pivot
    const float sx = (float)canvasPx.x() - c0.x();
    const float sy = (float)canvasPx.y() - c0.y();
    const float lx = sx * scale.x();
    const float ly = sy * scale.y();
    canvasPx.setX(cosT * lx - sinT * ly + pivot.x());
    canvasPx.setY(sinT * lx + cosT * ly + pivot.y());
    return true;
}

// ===========================================================================
// FreeTransformAction (自由変形)
// ===========================================================================
bool FreeTransformAction::canActivate() const
{
    CanvasDocument *doc = host_.hostDocument();
    return doc && !doc->layers.isEmpty();
}

bool FreeTransformAction::onActivate()
{
    tool_.activate(host_.hostToolContext());
    return true;
}

void FreeTransformAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
}

void FreeTransformAction::onCancel()
{
    tool_.deactivate();
}

void FreeTransformAction::applyRenderState(QOpenGLShaderProgram *renderProgram)
{
    const bool freeTransformActive = isActive() && tool_.engaged();
    renderProgram->setUniformValue("uIsFreeTransformTool", freeTransformActive ? 1 : 0);
    if (!freeTransformActive) return;
    renderProgram->setUniformValue("uFreeP0", tool_.corner(0));
    renderProgram->setUniformValue("uFreeP1", tool_.corner(1));
    renderProgram->setUniformValue("uFreeP2", tool_.corner(2));
    renderProgram->setUniformValue("uFreeP3", tool_.corner(3));
    renderProgram->setUniformValue("uFreeC0",       tool_.center0());
    renderProgram->setUniformValue("uFreeHalfSize", tool_.halfSize());
}

// TransformAction::mapSelectionOutlinePoint と同じ狙い(そちらのコメント参照)。
// 自由変形の逆変換は invBilinear(表示位置 → 単位正方形のuv)なので、順方向は
// 「元の位置 → uv → 4頂点の双一次補間」になる。
bool FreeTransformAction::mapSelectionOutlinePoint(QPointF &canvasPx) const
{
    if (!isActive() || !tool_.engaged()) return false;

    const QVector2D c0   = tool_.center0();
    const QVector2D half = tool_.halfSize();
    if (half.x() <= 0.0001f || half.y() <= 0.0001f) return false;

    // 元の位置 → uv(0..1)。render.fragの
    //   srcCanvasPx = c0 + ((uv*2-1) * half)
    // を uv について解いたもの。
    const float u = (((float)canvasPx.x() - c0.x()) / half.x() + 1.0f) * 0.5f;
    const float v = (((float)canvasPx.y() - c0.y()) / half.y() + 1.0f) * 0.5f;

    // uv → 4頂点の双一次補間(invBilinearのA,B,C,Dの取り方に合わせる)
    const QVector2D p0 = tool_.corner(0); // TL (u,v)=(0,0)
    const QVector2D p1 = tool_.corner(1); // TR (1,0)
    const QVector2D p2 = tool_.corner(2); // BR (1,1)
    const QVector2D p3 = tool_.corner(3); // BL (0,1)
    const QVector2D p = p0
                      + (p1 - p0) * u
                      + (p3 - p0) * v
                      + (p0 - p1 + p2 - p3) * (u * v);
    canvasPx.setX(p.x());
    canvasPx.setY(p.y());
    return true;
}
