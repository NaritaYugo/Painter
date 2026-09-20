#pragma once

#include "tools/core/Tool.h"
#include "tools/core/ToolConfig.h"

#include <QVector>
#include <QPoint>
#include <QOpenGLFunctions_4_3_Core>
#include <QOpenGLShaderProgram>
#include <functional>
#include <cstdint>

struct BrushSettings;

class FillTool : public Tool,  protected QOpenGLFunctions_4_3_Core
{
public:
    struct Textures {
        GLuint layerTexArray = 0;
        GLuint maskTex       = 0;
        GLuint wallTex       = 0;
        GLuint outerJfaTex   = 0;
        GLuint innerJfaTex   = 0;
        GLuint sdfTex        = 0;
    };

    struct Programs {
        QOpenGLShaderProgram *wall        = nullptr;
        QOpenGLShaderProgram *jfaInit     = nullptr;
        QOpenGLShaderProgram *jfaInitInner= nullptr;
        QOpenGLShaderProgram *jfa         = nullptr;
        QOpenGLShaderProgram *jfaFinalize = nullptr;
        QOpenGLShaderProgram *bake        = nullptr;
        QOpenGLShaderProgram *maskClear   = nullptr;
    };
    
    void initialize(QOpenGLContext *ctx);

    // テクスチャ/シェーダーを CanvasWidget 側から注入
    void setTextures(const Textures &tex)   { tex_ = tex; }
    void setPrograms(const Programs &prog)  { prog_ = prog; }

    // MainWindowが所有する唯一のToolConfigへの非所有ポインタ(CanvasWidget経由で渡される)
    void setToolConfig(ToolConfig *cfg) { toolCfg_ = cfg; }

    void onMousePress(QMouseEvent *event, ToolContext &ctx) override
    {
        execute(ctx, event->position());
    }

    // CanvasWidget::executeFill(pos, threshold) のような外部公開APIからも
    // 同じロジックを呼べるように、実処理を独立したメンバ関数にしてある。
    bool execute(ToolContext &ctx, const QPointF &widgetPos);

    void  setWallThreshold(float t) { wallThreshold_ = t; }
    float wallThreshold() const     { return wallThreshold_; }

private:
    ToolConfig *toolCfg_ = nullptr;

    float wallThreshold_ = 0.2f;

    Textures tex_;
    Programs prog_;

    void runJfaPasses(GLuint tex, GLenum fmt, int maxStep, int canvasW, int canvasH);
    QVector<float> buildSdf(int canvasW, int canvasH, GLuint defaultFbo);

    // computeFillRegion()の結果: 塗りマスク(0/255)と実際に塗られた範囲のbbox
    struct FillRegion {
        QVector<uint8_t> mask;                          // canvasW*canvasH、塗る=255
        int minX = 0, maxX = -1, minY = 0, maxY = -1;   // maxX<minXなら空
    };
    FillRegion computeFillRegion(int canvasW, int canvasH, int sx, int sy,
                                 const QVector<float> &distField, bool wrapX, bool wrapY);

    // 任意の2Dテクスチャから1ピクセルだけCPUへ読み出す(参照先="キャンバス"時の開始色取得用)
    QColor readPixel(GLuint tex, int x, int y, GLuint defaultFbo);
};
