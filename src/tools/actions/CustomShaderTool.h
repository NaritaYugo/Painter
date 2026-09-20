#pragma once
#include "tools/core/ToolContext.h"
#include <QOpenGLFunctions_4_3_Core>
#include <QOpenGLShaderProgram>
#include <QString>

// ---------------------------------------------------------------------------
// CustomShaderTool
// ---------------------------------------------------------------------------
// フィルターメニュー「カスタムシェーダー」アクション本体。ユーザーが
//   vec4 main(vec2 uv, vec2 pos, vec2 res, vec4 src) { ... }
// という関数本体だけを書き、それをコンピュートシェーダーのテンプレートへ
// 埋め込んでランタイムコンパイルする。
//   uv  = 0〜1に正規化した座標(左下が(0,0)、右上が(1,1))
//   pos = 中央(レイヤーの中心)が原点(0,0)で、縦横比を保った座標
//         (短い方の辺が[-1,1]になるよう正規化。uvだけだと非正方形レイヤーで
//          円が楕円に潰れてしまうため、円や放射状パターンを書きたいときはこちらを使う)
//   res = レイヤーのピクセルサイズ
//   src = そのピクセルの現在色(imageLoad結果をunpremultiplyしたstraight色)
// 戻り値もstraight(non-premultiplied)のRGBAとして扱う(内部でpremultiplyし直す)。
//
// GaussianBlurTool/MosaicToolと同じ「1回だけ計算してtransformSrcTexへ焼く、
// render.fragは焼き済みテクスチャをサンプルするだけ」というプレビュー方式を使う
// (ユーザーコードは近傍サンプリングも書けてしまうため、毎フレームの直接実行は
// 重すぎる可能性があるのと、GLSLコンパイルコスト自体もフレームごとには払えないため)。
// setSource()はデバウンスされた入力の確定時にのみ呼ばれる想定(呼び出し側=
// CustomShaderPanel/CanvasWidgetの責務)。
// ---------------------------------------------------------------------------
class CustomShaderTool : protected QOpenGLFunctions_4_3_Core
{
public:
    void initialize(QOpenGLContext *ctx);
    // GL資源(動的コンパイル済みプログラム)の解放。GLコンテキストがまだ有効な
    // 間にCanvasWidgetのデストラクタから明示的に呼ぶこと(暗黙のメンバ破棄はdoneCurrent()の後になるため)。
    void releaseGL();

    void activate(ToolContext &ctx);   // アクション開始: レイヤーを集約する
    void deactivate();                 // キャンセル(GPU上のレイヤー本体には一切書き込まない)
    void confirm(ToolContext &ctx);    // 直近のプレビュー結果をタイルへ焼き込んで終了する

    bool engaged() const { return engaged_; }

    // GLSL関数本体(ユーザーが書いた「vec4 main(vec2 uv, vec2 res, vec4 src) {...}」)を
    // ランタイムコンパイルし、成功すればその場でプレビューを再計算する。
    // 戻り値: 成功時は空文字列、失敗時はコンパイル/リンクエラーメッセージ。
    QString setSource(ToolContext &ctx, const QString &userSrc);
    QString source() const { return source_; }

    // render.frag用: プレビュー結果(ctx.transformSrcTex)がキャンバス座標系のどこに
    // 対応するか(レイヤー原点のピクセル座標とサイズ)。
    QVector2D previewOriginPx() const { return previewOriginPx_; }
    QVector2D previewSizePx()   const { return QVector2D((float)layerW_, (float)layerH_); }

    static QString defaultSource();

private:
    bool engaged_ = false;
    int  layerW_ = 0, layerH_ = 0; // activate()時に集約したレイヤーのピクセルサイズ
    int  layerIndex_ = -1;
    QVector2D previewOriginPx_;

    QOpenGLShaderProgram *dynamicProgram_ = nullptr; // 直近にコンパイル成功したユーザーシェーダー
    QString source_ = defaultSource();

    void updatePreview(ToolContext &ctx);
};
