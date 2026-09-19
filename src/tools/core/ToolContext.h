#pragma once

#include "backend/CanvasDocument.h"
#include "backend/ViewTransform.h"
#include <QOpenGLFunctions_4_3_Core>
#include <QOpenGLShaderProgram>
#include <QVector2D>
#include <QColor>

// ---------------------------------------------------------------------------
// ToolContext
// ---------------------------------------------------------------------------
// GLWidget が保持する共有GL資源・ドキュメントへの「参照だけ」を集めた構造体。
// 各 Tool はこれを使って、共有のテクスチャ/バッファへ直接書き込む。
// 所有権は一切持たない（GLWidget が生成・破棄する）。
// ---------------------------------------------------------------------------
struct ToolContext {
    QOpenGLFunctions_4_3_Core *gl = nullptr;   // GLWidget自身(protected継承のfunctions呼び出し用)
 
    CanvasDocument *doc   = nullptr;
    ViewTransform  *view  = nullptr;

    // ViewTransformの「画面座標」はデバイスピクセル(拡大率1.0で1キャンバスpxが
    // 1デバイスpxに一致し、等倍表示が実画素どおりになる。詳細はGLWidget.hの
    // viewDpr()のコメント)。一方Qtのマウス座標は論理pxなので、viewへ直接渡す前に
    // これを掛けること。逆に「画面上の見た目のサイズ」(ブラシ円カーソルの直径など)を
    // 論理pxで求めるときは view->scale() をこれで割る。
    float viewDpr = 1.0f;
 
    int canvasW = 0;
    int canvasH = 0;
    bool wrapX = false; // 左右ループ
    bool wrapY = false; // 上下ループ
    int tileSize = 256;

    // 現在「レイヤーマスクを編集中」のレイヤーindex(-1なら誰も編集していない=通常通り
    // レイヤー本体の色を編集する)。PenEraserTool/AirbrushToolは、この値がアクティブ
    // レイヤーのindexと一致するときだけ、描画先をLayer::tilesではなくLayer::maskTiles
    // へ切り替える(GLWidgetがマスクサムネイルのクリックに応じて更新する)。
    int editingMaskLayerIndex = -1;
 
    // テクスチャ(所有はGLWidget)
    // タイル格納用テクスチャ配列の「バンク」群。グローバルなスライス番号siは
    // bankTexOf(si)/localSliceOf(si) で (どのバンクテクスチャか, そのバンク内の
    // ローカルスライス番号) に分解する。詳細は CanvasDocument.h の MAX_TILE_BANKS。
    GLuint layerTexBanks[MAX_TILE_BANKS] = {0};
    int    slicesPerBank = 0; // 1バンクあたりのスライス数(= GPUのGL_MAX_ARRAY_TEXTURE_LAYERS)
    GLuint bankTexOf(int si)    const { return layerTexBanks[slicesPerBank > 0 ? (si / slicesPerBank) : 0]; }
    int    localSliceOf(int si) const { return slicesPerBank > 0 ? (si % slicesPerBank) : si; }
    int    bankIndexOf(int si)  const { return slicesPerBank > 0 ? (si / slicesPerBank) : 0; }
    GLuint maskTex       = 0;
    // ストローク色バッファ(RGBA16F)を必要なら確保して返す。スタンプごとに色が
    // 変わる設定のときだけ使う(GLWidget::ensureStrokeColorTexへの委譲)。
    // 未対応環境では0を返すので、その場合は従来の1色経路へフォールバックすること。
    std::function<GLuint()> ensureStrokeColorTex;
    GLuint fullLayerTex  = 0;
    GLuint penTipTex     = 0; // ペン先(スタンプ)画像。PenEraserToolのテクスチャ付きブラシ用
    // 紙質テクスチャ(グレースケール、GL_REPEAT)。PenEraserToolが「紙の目」として
    // ブラシの濃度に掛ける。未設定(=紙質なし)なら0。
    GLuint paperTex      = 0;
    GLuint toneCurveLUTTex = 0; // トーンカーブのLUT(256x1, R8)。ToneCurveToolが値変更のたびに書き換える
    GLuint gradientMapLUTTex = 0; // グラデーションマップのLUT(256x1, RGBA8)。GradientMapToolが書き換える
    // ペンストロークのバッチスタンプ用SSBO(所有はGLWidget)。stroke.comp の
    // StampBuf(binding=2)にバインドして使う。PenEraserTool::onMouseMove専用。
    GLuint strokeStampSSBO = 0;
    // 「筆に乗っている絵の具」(vec4 1個)。下地混色でbrushState.compが読み書きする。
    GLuint brushPaintSSBO  = 0;

    // 「アクティブレイヤーより下」の合成結果キャッシュ(所有はGLWidget)。
    // updateBelowCompositeCache/invalidateBelowCompositeCacheとセットで使う
    // (GLWidget.h のコメント参照)。
    GLuint belowCompositeTex         = 0;
    GLuint belowCompositeClipBaseTex = 0;
    std::function<void(int uptoExclusiveIndex)> updateBelowCompositeCache;
    std::function<void()> invalidateBelowCompositeCache;
    // 「アクティブレイヤーより上」の事前合成キャッシュ(下と対。GLWidget.hのコメント参照)。
    std::function<void(int activeIndex)> updateAboveCompositeCache;
    std::function<void()> invalidateAboveCompositeCache;

    // 選択範囲マスク(所有はGLWidget)。canvasW x canvasH の R8。
    // 選択が無い間は全域255(=どこでも塗れる)に保たれる。
    // 塗りつぶし/ペン/ぼかし/ゆがみなどの「塗る」系ツールは、bake/compute時に
    // このテクスチャの値を乗算することで「選択範囲内にしか塗れないマスク」として使う。
    // 移動変形・色変更など今後追加されるツールも同じテクスチャをそのまま参照できる。
    GLuint selectionMaskTex = 0;

    // 変形確定(TransformTool::applyTransform/FreeTransformTool::applyTransform)専用の
    // 作業用スナップショットテクスチャ(所有はGLWidget、両アクションで共用)。
    // fullLayerTex/selectionMaskTexへ書き込む直前に、変形元として読む
    // 「変形前の内容」をここへコピーしておく(読み込み元と書き込み先を
    // 同じテクスチャにできないため)。
    GLuint transformSrcTex        = 0; // RGBA8, canvasW x canvasH
    GLuint transformSrcSelMaskTex = 0; // R8,    canvasW x canvasH

    // 合成結果テクスチャ(所有はGLWidget、CanvasCompositorが更新する)
    GLuint compositedTex     = 0; // ナビゲーター用 単一2Dテクスチャ
    GLuint compositedTileArr = 0; // ナビゲーター用 作業タイル配列(initTexturesで事前確保)
 
    // シェーダー(所有はGLWidget)
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

    // キャンバスサイズ変更確定時に呼ぶ(CanvasSizeTool専用)。GLWidget::resizeCanvasKeepingContentへの委譲。
    // newW,newH: 新しいキャンバスサイズ。offsetX,offsetY: 新キャンバス原点(0,0)が旧キャンバス
    // 座標系のどこに位置するか(旧ピクセル(x,y)は新ピクセル(x-offsetX,y-offsetY)に写る)。
    std::function<bool(int newW, int newH, int offsetX, int offsetY)> resizeCanvasKeepingContent;

    // 指定レイヤーの矩形を、キャンバスタイル座標系で[minTx,maxTxEx) x [minTy,maxTyEx)を
    // 覆うように拡張する(既存矩形との和集合。GLWidget::growLayerBoundsToCoverCanvasTilesへの委譲)。
    // 拡大・縮小・回転/自由変形が、キャンバス外へ出た内容を失わずに保持するために使う
    // (Transform/FreeTransformTool::applyTransformがbake前に呼ぶ)。
    // 戻り値: 実際に拡張が発生したか。
    std::function<bool(int layerIndex, int minCanvasTx, int minCanvasTy,
                        int maxCanvasTxEx, int maxCanvasTyEx)> growLayerBounds;

    // 変形確定(TransformTool/FreeTransformTool::applyTransform)専用の作業用テクスチャ
    // (fullLayerTex/transformSrcTex)を、指定ピクセルサイズに合わせて作り直す
    // (レイヤーの矩形がgrowLayerBoundsで変わるため、キャンバスサイズ固定では足りない)。
    std::function<void(int w, int h)> ensureTransformScratchSize;

    // 多段パスのフィルター(分離型ガウスぼかし・モザイク)用の中間バッファを
    // 指定サイズで確保し、そのテクスチャIDを返す(GLWidget::ensureFilterScratchへの委譲)。
    // 読み込み元(fullLayerTex)と書き込み先(transformSrcTex)のどちらも潰せないため、
    // パスの間に挟むもう1枚が要る。詳細は GLWidget.h の filterScratchTex_ のコメント参照。
    std::function<GLuint(int w, int h)> ensureFilterScratch;

    // 画像解像度変更確定時に呼ぶ(ImageResolutionTool専用)。GLWidget::resampleCanvasResolutionへの委譲。
    // 見た目(縦横比・表示上のサイズ)を変えずに、全レイヤーの内容を新しい解像度(newW,newH)へ
    // 拡大縮小(リサンプル)する。
    std::function<bool(int newW, int newH)> resampleCanvasResolution;
 
    // レイヤー情報をGPUに渡すためのSSBO(所有はGLWidget)。すべてフラットな
    // レイヤーindex(下から数えたz)でそのまま引ける(旧レーン方式のような
    // 「lane*16+layer」変換は不要になった)。
    GLuint ssboLayerOpacity   = 0;
    GLuint ssboLayerVisible   = 0;
    GLuint ssboLayerBaseSlice = 0;
    // レイヤーマスクのベーススライス(-1なら無し)。binding=1、CanvasCompositor.cpp参照。
    GLuint ssboLayerMaskBaseSlice = 0;
    // このレイヤーを含むフォルダー(祖先、最大4階層ぶん)がそれぞれ持つマスクの
    // ベーススライス(ivec4、-1なら該当階層は無し/マスク無し)。フォルダー自体の
    // 表示・不透明度は uLayerVisible/uLayerOpacity 側で祖先ぶんを乗算済みの値を
    // 渡すのでここでは扱わない。binding=2、CanvasCompositor.cpp参照。
    GLuint ssboLayerAncestorMaskSlices = 0;
    GLuint ssboLayerBlendMode = 0;
    GLuint ssboLayerClipping  = 0;
    // レイヤーごとの矩形(タイル単位の原点+サイズ)。キャンバスより大きい/はみ出した
    // レイヤーを合成する際、シェーダー側で「このキャンバスタイルはレイヤーの矩形外」を
    // 判定するために使う(範囲外なら透明として扱う)。
    GLuint ssboLayerOriginTx  = 0;
    GLuint ssboLayerOriginTy  = 0;
    GLuint ssboLayerTilesX    = 0;
    GLuint ssboLayerTilesY    = 0;
    // 単色レイヤー(タイルを持たず、常に不透明の単色として合成される)かどうか、
    // およびその色(RGBA、0〜1)。
    GLuint ssboLayerIsSolidColor = 0;
    GLuint ssboLayerSolidColor   = 0; // vec4[]: 単色レイヤーの色(それ以外のレイヤーでは未使用)
    // 調整レイヤー(タイルを持たず、下のレイヤーの合成結果に色調整をかける)関連。
    // SSBOバインディング数がこのGPUでは16個までしか使えない(GL_MAX_SHADER_STORAGE_
    // BUFFER_BINDINGS)ため、isAdjustment+kindを1本(0=調整レイヤーでない、
    // 1=明るさ・コントラスト、2=色相・彩度・明度)に、param0/1/2をvec3配列1本に
    // まとめている。
    GLuint ssboLayerAdjKind   = 0; // 0=調整レイヤーでない, 1=明るさ・コントラスト, 2=色相・彩度・明度
    GLuint ssboLayerAdjParams = 0; // vec3[]: kind別に(明るさ,コントラスト,未使用) or (色相,彩度,明度)
    int    maxLayers          = 256; // CanvasDocument::MAX_LAYERS と一致させる

    // テキストボックスの編集を開始する(TextTool専用)。アクティブレイヤー内の
    // textBoxes[boxIndex]を対象に編集パネルを開く(新規ボックスの場合は、
    // TextTool側が先にtextBoxesへpush_backしてからそのインデックスを渡す)。
    // GLWidget::startOrEditTextBoxへの委譲(パネルを開き、確定/キャンセルは
    // パネルのシグナル経由でGLWidget側が処理する)。
    std::function<void(int boxIndex)> startOrEditTextBox;

    // テキストボックスをキャンバス上で直接ドラッグ移動/拡縮/回転している間、
    // パネルを介さずにレイヤーの実ピクセルへ焼き込みたい場合に呼ぶ
    // (GLWidget::scheduleTextRasterizeへの委譲。デバウンスされる)。
    std::function<void(int layerIndex)> requestTextRasterize;

    // ウィジェット座標から色を拾う(スポイト/塗りつぶしの開始色判定に使う)
    // 第2引数 referenceCanvas: false=現在のレイヤーのみ参照 / true=全レイヤー合成後のキャンバスを参照
    std::function<QColor(const QPointF&, bool)> getPixelColor;
 
    // 現在選択中のブラシ設定を「プリマル済み色」で返す(塗りつぶしの塗り色に使う)
    // 元の GLWidget::toPreMulColor(activeSettings().rawColor, activeSettings().opacity) と同じ
    std::function<QColor()> activeBrushPreMulColor;
 
    // 選択範囲のUndo記録(GLWidget::beginSelectionUndo/commitSelectionUndoへの委譲)。
    // 選択範囲を変えうる操作の直前にbegin、変え終わった直後にcommitを呼ぶ。
    // 変化が無ければcommit側で何も積まないので、空振りしそうな場所でも呼んでよい。
    std::function<void()> beginSelectionUndo;
    // 「貼ったばかりのマスク」とその矩形(キャンバスpx)を渡す。Undoにはこの矩形ぶんだけを
    // 記録するので、全域を扱うより大幅に軽くメモリも小さい。変化が無い場合は
    // 空のQByteArrayと w=h=0 を渡せばよい(その場合は何も積まれない)。
    std::function<void(const QByteArray &afterRaw, int x, int y, int w, int h)> commitSelectionUndo;
    // 選択範囲を解除する(GLWidget::clearSelectionへの委譲。内部でUndo記録も行う)。
    std::function<void()> clearSelection;

    // スポイトで拾った色を通知する(GLWidget::colorDropperdシグナルの発行)。
    // transparentが真なら「透明色を拾った」(色は無意味)。
    // アルファは拾わない(受け取り側が現在の値を維持する。DropperTool参照)。
    std::function<void(QColor, bool transparent)> notifyColorDropped;
 
    // ウィジェット座標 -> キャンバスピクセル座標変換
    // (GLWidget::widgetToPixel を呼ぶための関数ポインタ的委譲)
    std::function<QVector2D(const QPointF&)> widgetToPixel;

    // キャンバスピクセル座標 -> ウィジェット座標変換(widgetToPixelの逆)。
    // 投げ縄選択のプレビュー線など、GL描画後にQPainterで重ねて描く際に使う。
    std::function<QPointF(const QVector2D&)> pixelToWidget;

    // 選択範囲が確定した(投げ縄を閉じた/ペン選択のドラッグを離した)ことを通知する。
    // GLWidget側のhasSelection状態とselectionChangedシグナルの発行に使う。
    std::function<void(bool)> setHasSelection;

    // 現在選択範囲があるかどうかを問い合わせる(GLWidget::hasSelection()への委譲)。
    // 変形ツールが「対象を選択範囲にするかレイヤー全体にするか」を判定するのに使う。
    std::function<bool()> getHasSelection;

    // selectionMaskTexの中身が変わった(投げ縄/ペン選択の確定等)ことを通知する。
    // GLWidget側のマーチングアンツ用キャッシュ(paintSelectionOutline)を無効化する。
    // setHasSelectionと違い、選択範囲の有無(bool)が変わらない場合(既存の選択範囲を
    // 新しい選択で置き換える等)でも必ず呼ぶ必要がある。
    std::function<void()> invalidateSelectionOutline;

    // ウィジェットの中心座標 (width()/2, height()/2)。回転ツールの中心点に使う。
    // リサイズで変わるので毎回呼び出す形にしている(値をキャッシュしない)。
    std::function<QVector2D()> widgetCenter;
 
    // Undo記録用のコールバック(GLWidgetのストローク単位Undoに委譲)
    std::function<void()>                    beginStrokeUndo;
    std::function<void(int,int,int,int)>     expandStrokeUndoRegion; // txMin,txMax,tyMin,tyMax
    std::function<void()>                    commitStrokeUndo;
 
    // 更新通知
    std::function<void()> requestRepaint;       // update()
    std::function<void()> notifyLayersChanged;  // emit layersChanged()

    // ストローク中の部分再描画用: 今回のGPUディスパッチで実際に変更された
    // キャンバス領域(キャンバスpx、両端含む)をGLWidgetへ報告する。GLWidget::paintGL()は
    // ストローク中この矩形だけをシザーで再描画し、残りは前フレームの内容を保持する
    // (ウィンドウ全画素×レイヤー数の再合成を毎フレーム行わないための仕組み)。
    std::function<void(float minX, float minY, float maxX, float maxY)> noteStrokeDirtyRegion;

    // 全レイヤーを合成してcompositedTexを最新化する(塗りつぶしの「キャンバス参照」用)
    std::function<void()> updateCompositedTex;
 
    // QOpenGLWidget::defaultFramebufferObject() を返す(0決め打ち禁止)
    std::function<GLuint()> defaultFbo;
};