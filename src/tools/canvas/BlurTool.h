#pragma once

#include "tools/core/Tool.h"
#include "tools/core/ToolConfig.h"
#include "tools/core/CursorUtils.h"

#include <QOpenGLFunctions_4_3_Core>
#include <QOpenGLShaderProgram>
#include <QVector2D>

// ---------------------------------------------------------------------------
// BlurTool
// ---------------------------------------------------------------------------
// ドラッグ中、ブラシ範囲のピクセルを近傍平均でぼかすツール。
// FillTool と同じ「タイル -> fullLayerTex(収集) -> compute -> タイル(書き戻し)」の
// ラウンドトリップ方式を使うが、対象をブラシのバウンディングボックスに限定して
// ドラッグ中でも軽く動くようにしてある。
//
// compute シェーダーは読み取り元(fullLayerTex)と書き込み先(blurTex_)を分けており、
// カーネル参照時の read-after-write ハザードを避けている。
// ---------------------------------------------------------------------------
class BlurTool : public Tool, protected QOpenGLFunctions_4_3_Core
{
public:
    void initialize(QOpenGLContext *ctx);

    // MainWindowが所有する唯一のToolConfigへの非所有ポインタ(GLWidget経由で渡される)
    void setToolConfig(ToolConfig *cfg) { toolCfg_ = cfg; }
    // GLWidget が initializeGL でコンパイルした blur.comp を注入する
    void setProgram(QOpenGLShaderProgram *p) { prog_ = p; }

    void onMousePress(QMouseEvent *event, ToolContext &ctx)   override;
    void onMouseMove(QMouseEvent *event, ToolContext &ctx)    override;
    void onMouseRelease(QMouseEvent *event, ToolContext &ctx) override;
    bool isActive() const override { return dragging_; }

    std::optional<QCursor> cursor(const ToolContext &ctx) const override;

private:
    ToolConfig           *toolCfg_ = nullptr;
    QOpenGLShaderProgram *prog_    = nullptr;

    bool      dragging_ = false;
    QVector2D lastPos_;

    // ぼかし結果の書き込み先(読み取り元 fullLayerTex とは別テクスチャにする)。
    // キャンバスサイズに合わせて遅延生成し、サイズが変わったら作り直す。
    GLuint blurTex_  = 0;
    int    blurTexW_ = 0;
    int    blurTexH_ = 0;
    void   ensureBlurTex(int w, int h);

    // from→to の線分に沿ってブラシ範囲をぼかす(press/move から呼ぶ)
    void applyBlur(ToolContext &ctx, const QVector2D &from, const QVector2D &to);
};
