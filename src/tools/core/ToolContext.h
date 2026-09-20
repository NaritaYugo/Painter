#pragma once

#include "document/CanvasDocument.h"
#include "rendering/ViewTransform.h"
#include <QOpenGLFunctions_4_3_Core>
#include <QOpenGLShaderProgram>
#include <QVector2D>
#include <QColor>

// ToolContext
struct ToolContext {
    QOpenGLFunctions_4_3_Core *gl = nullptr;   // CanvasWidget自身(protected継承のfunctions呼び出し用)
 
    CanvasDocument *doc   = nullptr;
    ViewTransform  *view  = nullptr;

    // ViewTransformの「画面座標」はデバイスピクセル(拡大率1.0で1キャンバスpxが1デバイスpxに一致し、等倍表示が実画素どおりになる。詳細はCanvasWidget.hのviewDpr()のコメント)。
    float viewDpr = 1.0f;
 
    int canvasW = 0;
    int canvasH = 0;
    bool wrapX = false; // 左右ループ
    bool wrapY = false; // 上下ループ
    int tileSize = 256;

    // 現在「レイヤーマスクを編集中」のレイヤーindex(-1なら誰も編集していない=通常通りレイヤー本体の色を編集する)。
    int editingMaskLayerIndex = -1;
 
    // テクスチャ(所有はCanvasWidget)
    GLuint layerTexBanks[MAX_TILE_BANKS] = {0};
    int    slicesPerBank = 0; // 1バンクあたりのスライス数(= GPUのGL_MAX_ARRAY_TEXTURE_LAYERS)
    GLuint bankTexOf(int si)    const { return layerTexBanks[slicesPerBank > 0 ? (si / slicesPerBank) : 0]; }
    int    localSliceOf(int si) const { return slicesPerBank > 0 ? (si % slicesPerBank) : si; }
    int    bankIndexOf(int si)  const { return slicesPerBank > 0 ? (si / slicesPerBank) : 0; }
    GLuint maskTex       = 0;
    // ストローク色バッファ(RGBA16F)を必要なら確保して返す。
    std::function<GLuint()> ensureStrokeColorTex;
    GLuint fullLayerTex  = 0;
    GLuint penTipTex     = 0; // ペン先(スタンプ)画像。PenEraserToolのテクスチャ付きブラシ用
    // 紙質テクスチャ(グレースケール、GL_REPEAT)。
    GLuint paperTex      = 0;
    GLuint toneCurveLUTTex = 0; // トーンカーブのLUT(256x1, R8)。ToneCurveToolが値変更のたびに書き換える
    GLuint gradientMapLUTTex = 0; // グラデーションマップのLUT(256x1, RGBA8)。GradientMapToolが書き換える
    // ペンストロークのバッチスタンプ用SSBO(所有はCanvasWidget)。
    GLuint strokeStampSSBO = 0;
    // 「筆に乗っている絵の具」(vec4 1個)。
    GLuint brushPaintSSBO  = 0;

    // 「アクティブレイヤーより下」の合成結果キャッシュ(所有はCanvasWidget)。
    GLuint belowCompositeTex         = 0;
    GLuint belowCompositeClipBaseTex = 0;
    std::function<void(int uptoExclusiveIndex)> updateBelowCompositeCache;
    std::function<void()> invalidateBelowCompositeCache;
    // 「アクティブレイヤーより上」の事前合成キャッシュ(下と対。CanvasWidget.hのコメント参照)。
    std::function<void(int activeIndex)> updateAboveCompositeCache;
    std::function<void()> invalidateAboveCompositeCache;

    // 選択範囲マスク(所有はCanvasWidget)。
    GLuint selectionMaskTex = 0;

    // 変形確定(TransformTool::applyTransform/FreeTransformTool::applyTransform)専用の作業用スナップショットテクスチャ(所有はCanvasWidget、両アクションで共用)。
    GLuint transformSrcTex        = 0; // RGBA8, canvasW x canvasH
    GLuint transformSrcSelMaskTex = 0; // R8,    canvasW x canvasH

    // 合成結果テクスチャ(所有はCanvasWidget、CanvasCompositorが更新する)。
    GLuint compositedTex     = 0; // ナビゲーター用 単一2Dテクスチャ
    GLuint compositedTileArr = 0; // ナビゲーター用 作業タイル配列(initTexturesで事前確保)
 
    // シェーダー(所有はCanvasWidget)。
    QOpenGLShaderProgram *computeDrawProgram       = nullptr;
    QOpenGLShaderProgram *computeBakeProgram       = nullptr;
    QOpenGLShaderProgram *computeBrushStateProgram = nullptr; // 下地混色(brushState.comp)
    QOpenGLShaderProgram *computeMaskClearProgram  = nullptr;
    QOpenGLShaderProgram *computeCompositeProgram  = nullptr;
    QOpenGLShaderProgram *computeTransformProgram  = nullptr;
    QOpenGLShaderProgram *computeFreeTransformProgram = nullptr;
    QOpenGLShaderProgram *computeHueSatLightProgram   = nullptr;
    QOpenGLShaderProgram *computeBrightnessContrastProgram = nullptr;
    QOpenGLShaderProgram *computeColorBalanceProgram        = nullptr;
    QOpenGLShaderProgram *computeToneCurveProgram           = nullptr;
    QOpenGLShaderProgram *computeGaussianBlurFilterProgram  = nullptr;
    QOpenGLShaderProgram *computeMosaicReduceProgram        = nullptr; // モザイクの1パス目(ブロック平均の集約)
    QOpenGLShaderProgram *computeMosaicFilterProgram        = nullptr;
    QOpenGLShaderProgram *computeMotionBlurFilterProgram    = nullptr;
    QOpenGLShaderProgram *computeNoiseFilterProgram         = nullptr;
    QOpenGLShaderProgram *computeChromaticAberrationFilterProgram = nullptr; // Pro限定(Free版では常にnullptrのまま)
    QOpenGLShaderProgram *computeLensBlurFilterProgram            = nullptr; // Pro限定(Free版では常にnullptrのまま)
    QOpenGLShaderProgram *computeGradientMapProgram               = nullptr; // Pro限定(Free版では常にnullptrのまま)

    // キャンバスサイズ変更確定時に呼ぶ(CanvasSizeTool専用)。
    std::function<bool(int newW, int newH, int offsetX, int offsetY)> resizeCanvasKeepingContent;

    // 指定レイヤーの矩形を、キャンバスタイル座標系で[minTx,maxTxEx) x [minTy,maxTyEx)を覆うように拡張する(既存矩形との和集合。CanvasWidget::growLayerBoundsToCoverCanvasTi
    // lesへの委譲)。
    std::function<bool(int layerIndex, int minCanvasTx, int minCanvasTy,
                        int maxCanvasTxEx, int maxCanvasTyEx)> growLayerBounds;

    // 変形確定(TransformTool/FreeTransformTool::applyTransform)専用の作業用テクスチャ(fullLayerTex/transformSrcTex)を、
    // 指定ピクセルサイズに合わせて作り直す(レイヤーの矩形がgrowLayerBoundsで変わるため、キャンバスサイズ固定では足りない)。
    std::function<void(int w, int h)> ensureTransformScratchSize;

    // 多段パスのフィルター(分離型ガウスぼかし・モザイク)用の中間バッファを指定サイズで確保し、そのテクスチャIDを返す(CanvasWidget::ensureFilterScratchへの委譲)。
    std::function<GLuint(int w, int h)> ensureFilterScratch;

    // 画像解像度変更確定時に呼ぶ(ImageResolutionTool専用)。
    std::function<bool(int newW, int newH)> resampleCanvasResolution;
 
    // レイヤー情報をGPUに渡すためのSSBO(所有はCanvasWidget)。
    GLuint ssboLayerOpacity   = 0;
    GLuint ssboLayerVisible   = 0;
    GLuint ssboLayerBaseSlice = 0;
    // レイヤーマスクのベーススライス(-1なら無し)。
    GLuint ssboLayerMaskBaseSlice = 0;
    // このレイヤーを含むフォルダー(祖先、最大4階層ぶん)がそれぞれ持つマスクのベーススライス(ivec4、-1なら該当階層は無し/マスク無し)。
    GLuint ssboLayerAncestorMaskSlices = 0;
    GLuint ssboLayerBlendMode = 0;
    GLuint ssboLayerClipping  = 0;
    // レイヤーごとの矩形(タイル単位の原点+サイズ)。
    GLuint ssboLayerOriginTx  = 0;
    GLuint ssboLayerOriginTy  = 0;
    GLuint ssboLayerTilesX    = 0;
    GLuint ssboLayerTilesY    = 0;
    // 単色レイヤー(タイルを持たず、常に不透明の単色として合成される)かどうか、およびその色(RGBA、0〜1)。
    GLuint ssboLayerIsSolidColor = 0;
    GLuint ssboLayerSolidColor   = 0; // vec4[]: 単色レイヤーの色(それ以外のレイヤーでは未使用)
    // 調整レイヤー(タイルを持たず、下のレイヤーの合成結果に色調整をかける)関連。
    GLuint ssboLayerAdjKind   = 0; // 0=調整レイヤーでない, 1=明るさ・コントラスト, 2=色相・彩度・明度
    GLuint ssboLayerAdjParams = 0; // vec3[]: kind別に(明るさ,コントラスト,未使用) or (色相,彩度,明度)
    int    maxLayers          = 256; // CanvasDocument::MAX_LAYERS と一致させる

    // テキストボックスの編集を開始する(TextTool専用)。
    std::function<void(int boxIndex)> startOrEditTextBox;

    // テキストボックスをキャンバス上で直接ドラッグ移動/拡縮/回転している間、パネルを介さずにレイヤーの実ピクセルへ焼き込みたい場合に呼ぶ(CanvasWidget::scheduleTextRasterizeへの委譲。デバウンスされる)。
    std::function<void(int layerIndex)> requestTextRasterize;

    // ウィジェット座標から色を拾う(スポイト/塗りつぶしの開始色判定に使う)。
    std::function<QColor(const QPointF&, bool)> getPixelColor;
 
    // 現在選択中のブラシ設定を「プリマル済み色」で返す(塗りつぶしの塗り色に使う)。
    std::function<QColor()> activeBrushPreMulColor;
 
    // 選択範囲のUndo記録(CanvasWidget::beginSelectionUndo/commitSelectionUndoへの委譲)。
    std::function<void()> beginSelectionUndo;
    // 「貼ったばかりのマスク」とその矩形(キャンバスpx)を渡す。
    std::function<void(const QByteArray &afterRaw, int x, int y, int w, int h)> commitSelectionUndo;
    // 選択範囲を解除する(CanvasWidget::clearSelectionへの委譲。内部でUndo記録も行う)。
    std::function<void()> clearSelection;

    // スポイトで拾った色を通知する(CanvasWidget::colorDropperdシグナルの発行)。
    std::function<void(QColor, bool transparent)> notifyColorDropped;
 
    // ウィジェット座標 -> キャンバスピクセル座標変換
    std::function<QVector2D(const QPointF&)> widgetToPixel;

    // キャンバスピクセル座標 -> ウィジェット座標変換(widgetToPixelの逆)。
    std::function<QPointF(const QVector2D&)> pixelToWidget;

    // 選択範囲が確定した(投げ縄を閉じた/ペン選択のドラッグを離した)ことを通知する。
    std::function<void(bool)> setHasSelection;

    // 現在選択範囲があるかどうかを問い合わせる(CanvasWidget::hasSelection()への委譲)。
    std::function<bool()> getHasSelection;

    // selectionMaskTexの中身が変わった(投げ縄/ペン選択の確定等)ことを通知する。
    std::function<void()> invalidateSelectionOutline;

    // ウィジェットの中心座標 (width()/2, height()/2)。
    std::function<QVector2D()> widgetCenter;
 
    // Undo記録用のコールバック(CanvasWidgetのストローク単位Undoに委譲)。
    std::function<void()>                    beginStrokeUndo;
    std::function<void(int,int,int,int)>     expandStrokeUndoRegion; // txMin,txMax,tyMin,tyMax
    std::function<void()>                    commitStrokeUndo;
 
    // 更新通知。
    std::function<void()> requestRepaint;       // update()
    std::function<void()> notifyLayersChanged;  // emit layersChanged()

    // ストローク中の部分再描画用: 今回のGPUディスパッチで実際に変更されたキャンバス領域(キャンバスpx、両端含む)をCanvasWidgetへ報告する。
    std::function<void(float minX, float minY, float maxX, float maxY)> noteStrokeDirtyRegion;

    // 全レイヤーを合成してcompositedTexを最新化する(塗りつぶしの「キャンバス参照」用)。
    std::function<void()> updateCompositedTex;
 
    // QOpenGLWidget::defaultFramebufferObject() を返す(0決め打ち禁止)。
    std::function<GLuint()> defaultFbo;
};
