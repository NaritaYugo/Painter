#pragma once

#include "document/CanvasDocument.h"
#include "rendering/ViewTransform.h"
#include "document/StrokeUndoRecorder.h"
#include "rendering/CanvasCompositor.h"
#include "rendering/LayerSliceAllocator.h"
#include "tools/core/Tool.h"
#include "tools/core/ToolType.h"
#include "tools/canvas/PenEraserTool.h"
#include "tools/canvas/FillTool.h"
#include "tools/canvas/MoveTool.h"
#include "tools/canvas/RotateTool.h"
#include "tools/canvas/DropperTool.h"
#include "tools/canvas/BlurTool.h"
#include "tools/canvas/WarpTool.h"
#include "tools/canvas/SelectTool.h"
#include "tools/canvas/TextTool.h"
#include "tools/canvas/AirbrushTool.h"
#include "tools/core/ToolConfig.h"
#include "actions/CanvasActionHost.h"
#include "actions/CanvasActionController.h"
#include "actions/LayerEditActions.h"

#include <QOpenGLWidget>
#include <QOpenGLFunctions_4_3_Core>
#include <QOpenGLShaderProgram>
#include <QColor>
#include <QVector>
#include <QVector2D>
#include <QPolygonF>
#include <QPainterPath>
#include <QElapsedTimer>
#include <memory>

// CanvasWidget
class QTimer;
class AdjustmentLayerEditAction;
class FilterLayerEditAction;
class SolidColorLayerEditAction;
class TextBoxEditAction;

class CanvasWidget : public QOpenGLWidget, protected QOpenGLFunctions_4_3_Core, public CanvasActionHost
{
    Q_OBJECT

signals:
    void initialized();
    void layersChanged();  // レイヤー構成・プロパティが変わった
    // スポイトで色を拾った。
    void colorDropperd(QColor c, bool transparent);
    void activeToolChanged(ToolType tool); // ツールが変わったら発行
    void activeToolPresetChanged(ToolType tool, int index); // 同じToolType内でツールプリセットが切り替わったら発行
    void modifiedChanged(bool modified);
    void selectionChanged(bool hasSelection); // 選択範囲の有無が変わったら発行
    void viewChanged(); // パン・ズーム・回転・左右反転など表示上のビュー変換が変わったら発行(NavigatorDock用)

public:
    // toolCfg: MainWindowが所有する唯一のToolConfigへの非所有ポインタ。
    explicit CanvasWidget(ToolConfig *toolCfg, QWidget *parent = nullptr);
    ~CanvasWidget() override;

    ToolConfig *toolConfig() const { return toolCfg_; }

    int canvasW   = 1920;
    int canvasH   = 1080;

    // ビュー変換 (ViewTransform に委譲)。
    QMatrix4x4 viewMatrix()        const { return view_.matrix(); }
    QMatrix4x4 viewMatrixInverse() const { return view_.inverseMatrix(); }
    float       viewScale()        const { return view_.scale(); }
    void fitCanvasToView();

    // ウィジェット中心を基準に、指定の絶対倍率(1.0=100%)へズームする(ナビゲータードックのズームスライダー用。マウス位置基準のwheelEventとは別に、常にウィジェット中心を基準にする)。
    void setViewScaleCentered(float scale);
    // 現在の表示倍率に対して指定の係数(例: 1.1 = 10%拡大, 1/1.1 = 約10%縮小)を掛けた倍率へズームする(ショートカットキー用。ウィジェット中心基準)。
    void zoomStep(float factor) { setViewScaleCentered(viewScale() * factor); }

    // 表示上の左右反転(キャンバスのデータ自体は変更しない。ナビゲータードックのトグルボタン用)。
    bool isFlippedX() const { return view_.flipX(); }
    void setFlippedX(bool flip);

    // 現在ウィジェットに表示されている範囲を、キャンバスピクセル座標系(Y下向き、QImageと同じ原点)の四角形として返す(パン・ズーム・回転・左右反転をすべて反映済み)。
    QPolygonF visibleCanvasRectPolygon() const;

    // ビューを平行移動する。
    void panByCanvasDelta(const QVector2D &canvasDeltaYDown);

    // ツール。
    ToolType previousTool = ToolType::Pen;  // スペース押下前のツール
    bool spaceHeld    = false;
    
    void     setActiveTool(ToolType tool);
    ToolType getActiveTool() const { return activeTool; }
    void returnToPreviousTool();

    // 同じToolType内でツールプリセット(名前付き設定プリセット)だけを切り替える。
    void setActiveToolPreset(ToolType tool, int index);

    // 現在のツール/ツールプリセット/表示倍率に応じてカーソルの見た目を更新する。
    void updateCursor();

    // 選択範囲
    bool hasSelection() const { return hasSelection_; }
    void clearSelection();
    // キャンバス範囲全体を選択する(レイヤーがキャンバスからはみ出ている部分は、選択範囲マスク自体がキャンバスサイズなので選択されない)。
    void selectAll();

    // 選択範囲のUndo/Redo
    void beginSelectionUndo();
    // afterRaw に「貼ったばかりのマスク」、x/y/w/h にその矩形(キャンバスpx)を渡す。
    void commitSelectionUndo(const QByteArray &afterRaw = QByteArray(),
                             int x = 0, int y = 0, int w = 0, int h = 0);

    // コピー・ペースト(クリップボード)
    void copySelection();
    // クリップボードの画像をアクティブレイヤーへアルファ合成で貼り付ける。
    void pasteClipboard();

    // 拡大・縮小・回転(編集メニューのアクション。常設ツールではなく、Ctrl+Tで開始しEnterで確定/Escapeでキャンセルする一回限りの操作)。
    bool isTransformActionActive() const { return transformAction_->isActive(); }
    void startTransformAction();   // Ctrl+T: 選択範囲(無ければレイヤー全体)を対象に開始する
    void confirmTransformAction() { actions_.confirmActive(); } // Enter/確定ボタン: 実データへ焼き込んで終了する
    void cancelTransformAction()  { actions_.cancelActive(); }  // Escape/キャンセルボタン: 破棄して終了する

    // 自由変形
    bool isFreeTransformActionActive() const { return freeTransformAction_->isActive(); }
    void startFreeTransformAction();
    void confirmFreeTransformAction() { actions_.confirmActive(); }
    void cancelFreeTransformAction()  { actions_.cancelActive(); }

    // 色調整/フィルター系アクション(色相・彩度・明度 / 明るさ・コントラスト /カラーバランス / トーンカーブ / ガウスぼかし / カスタムシェーダー / モザイク /色収差(Pro))。
    void startHueSatLightAction();
    void startBrightnessContrastAction();
    void startColorBalanceAction();
    void startToneCurveAction();
    void startGaussianBlurAction();
    void startCustomShaderAction();
    void startMosaicAction();
    void startMotionBlurAction();
    void startNoiseAction();
    void startChromaticAberrationAction();
    void startLensBlurAction();
    void startGradientMapAction();

    // controller が管理する現在アクティブなアクションの確定/キャンセル/実行中判定。
    void confirmActiveAction() { actions_.confirmActive(); }
    void cancelActiveAction()  { actions_.cancelActive(); }
    bool isAnyCanvasActionActive() const { return actions_.isBusy(); }

    // キャンバスサイズ変更。
    bool isCanvasSizeActionActive() const { return canvasSizeAction_->isActive(); }
    void startCanvasSizeAction();
    void confirmCanvasSizeAction() { actions_.confirmActive(); }
    void cancelCanvasSizeAction()  { actions_.cancelActive(); }

    // 画像解像度変更
    bool isImageResolutionActionActive() const { return imageResolutionAction_->isActive(); }
    void startImageResolutionAction();
    void confirmImageResolutionAction() { actions_.confirmActive(); }
    void cancelImageResolutionAction()  { actions_.cancelActive(); }

    // ブラシ
    float smoothingStrength = 0.5f;
    void  setSmoothingStrength(float s) { smoothingStrength = qBound(0.01f, s, 1.0f); }
    float  getSmoothingStrength() { return smoothingStrength; }

    // ペン先(スタンプ)画像。
    bool setPenTipImage(const QString &path);
    QString currentPenTipImagePath() const { return penTipTexPath_; }

    // 紙質テクスチャ。
    bool setPaperTexture(const QString &path);
    QString currentPaperTexturePath() const { return paperTexPath_; }

    // toolCfg_->colorMode()/calibration()(表示モード/モニターキャリブレーション)が変更された際にMainWindowから呼ばれ、再描画する。
    void applyDisplayConfig();

    // レイヤー操作
    bool    addLayer(const QString &name = "新規レイヤー",
                     int insertIndex = -1, bool clipping = false,
                     int originTx = 0, int originTy = 0,
                     int tilesXOverride = -1, int tilesYOverride = -1,
                     LayerType layerType = LayerType::Normal,
                     const QVector<int> &ancestorFolders = {});

    // 単色レイヤーを追加する便利関数。
    bool    addSolidColorLayer(const QString &name = "単色レイヤー", int insertIndex = -1,
                                const QColor &color = QColor(255, 255, 255, 255),
                                const QVector<int> &ancestorFolders = {});

    // レイヤー追加をUndo対象にするための、呼び出し側で挟む3点セット(UndoKind::LayerAdd。addLayer()自体はファイル読み込みやUndo復元でも通る共通経路なのでUndoを積まない)。
    void    beginLayerAddUndo();
    void    commitLayerAddUndo(int insertedIndex, const QVector<int> &ancestorFolders = {},
                                int duplicateSourceIndex = -1, bool captureContent = false);
    void    abortLayerAddUndo();

    // layerIndexにレイヤーマスクを追加する(1枚のみ)。
    bool    addLayerMask(int layerIndex);
    // マスクを取り除く(GPUスライスを解放する)。
    bool    removeLayerMask(int layerIndex);

    // 現在「マスクを編集中」のレイヤーindex(-1なら誰も編集していない)。
    int     editingMaskLayerIndex() const { return editingMaskLayerIndex_; }
    // layerIndexを渡すと、すでにそのレイヤーを編集中なら解除(-1に戻す)、それ以外ならそのレイヤーの編集を開始する(=1つだけがアクティブなトグル)。
    void    setEditingMaskLayer(int layerIndex);

    // 一括レイヤーインポート(PSD読み込み等)
    void    beginBulkLayerImport();
    void    endBulkLayerImport();

    // 読み込み前に「これから確保するタイル(スライス)総数の見積もり」を渡すと、タイル用テクスチャ配列を一括で確保しておく(読み込み中の細かな伸長+コピーの繰り返しを避けて高速化する)。
    void    reserveTileSlices(int count);

    // layerIndexがフォルダーなら中身ごと削除する。
    bool    removeLayer(int layerIndex, const QVector<int> &ancestorFolders = {}, bool includeContents = true);

    // 調整レイヤーの編集。
    bool isAdjustmentLayerActionActive() const { return adjustmentLayerEditAction_->isActive(); }
    void editAdjustmentLayer(int layerIndex);

    // フィルターレイヤーのパラメータ編集。
    void editFilterLayer(int layerIndex);
    // LayerDockからフィルターキャッシュを無効化する。
    void invalidateFilterChainCache() { invalidateFilterChain(); }
    void confirmAdjustmentLayerEdit() { actions_.confirmActive(); } // パネルの「確定」: 編集内容をそのまま残して終了
    void cancelAdjustmentLayerEdit()  { actions_.cancelActive(); }  // パネルの「キャンセル」: 編集開始前の内容に戻して終了

    // 単色レイヤーの色編集(調整レイヤーと同じ考え方。レイヤー自体はLayerDockの「新規単色レイヤー」で先に作成済みのものを使い、プレビューのダブルクリックで編集パネル(OKLCHカラーサークル)を開く。何度でも開き直して編集できる)。
    bool isSolidColorPickerActionActive() const { return solidColorLayerEditAction_->isActive(); }
    void editSolidColorLayer(int layerIndex);
    void confirmSolidColorLayerEdit() { actions_.confirmActive(); } // パネルの「確定」: 編集内容をそのまま残して終了
    void cancelSolidColorLayerEdit()  { actions_.cancelActive(); }  // パネルの「キャンセル」: 編集開始前の色に戻して終了

    // テキストレイヤー編集(TextToolでテキストボックスをクリック/新規作成すると開始する。レイヤー自体はLayerDockの「新規テキストレイヤー」で先に作成済みのものを使う。1レイヤーに複数のテキストボックス(textBoxes)を持てる
    // )。
    bool isTextLayerEditActive() const { return textBoxEditAction_->isActive(); }
    void startOrEditTextBox(int boxIndex); // TextTool::onMousePress/onMouseDoubleClickから呼ばれる
    void confirmTextLayerEdit() { actions_.confirmActive(); } // パネルの「確定」: 編集内容をそのまま残して終了
    void cancelTextLayerEdit()  { actions_.cancelActive(); }  // パネルの「キャンセル」: 編集開始前の内容に戻して終了
    // ドラッグ操作(移動/拡縮/回転)中に、パネルを介さずデバウンス付きで実ピクセルへ再ラスタライズしたい場合に呼ぶ(TextTool専用。編集パネルの確定/キャンセルとは別の独立したデバウンスタイマーを使う)。
    void scheduleTextRasterize(int layerIndex);

    // 「切り取り」: 選択範囲があればその範囲(選択形状どおり)、無ければアクティブレイヤーの全内容(キャンバス外の部分も含む)をクリップボードへコピーしてから透明にする。
    void    cutSelection();
    // nudgeActiveContent()のうち「アクティブレイヤーの実ピクセル内容を平行移動する」部分(変形アクションが実行中でないとき)。
    void    nudgeActiveLayer(int dx, int dy);

    // victimIndex を survivorIndex に統合する(victimIndexのblendModeで合成)。
    int     mergeLayers(int survivorIndex, int victimIndex, const QVector<int> &ancestorFolders = {});

    // sourceIndex のレイヤー(ピクセル内容含む)を insertIndex の位置に複製する。
    int     duplicateLayer(int sourceIndex, int insertIndex, const QVector<int> &ancestorFolders = {});

    QImage  getLayerPreview(int zStart, int zEnd, int size = 64);
    QImage  getNavigatorPreview(int size, bool fullCanvas = true);
    // 指定レイヤーのマスク濃淡を size に収まるよう縮小したグレースケールプレビューを返す(不透明度プレビューUIが表示に使う)。
    QImage  getMaskPreview(int layerIndex, int size);

    // Undo / Redo
    void undo();
    void redo();
    bool canUndo() const { return doc_->canUndo(); }
    bool canRedo() const { return doc_->canRedo(); }
    void markSaved() { doc_->markSaved(); emit modifiedChanged(false); }
    bool isModified() const { return doc_->isModified(); }

    // 塗りつぶし。
    void executeFill(const QPointF &pos, float wallThreshold = 0.2f);

    // 書き出し。
    QImage exportCanvas();
    bool exportToImage(const QString &filePath);

    // CanvasDocument (CanvasWidgetが所有する。1タブ = 1CanvasWidget = 1CanvasDocument)スライス確保はGLテクスチャ操作を伴うため、
    // CanvasWidget(のLayerSliceAllocator)からしか提供できない。
    CanvasDocument::SliceAllocFn sliceAllocFn() { return [this](int c) { return sliceAllocator_.allocContiguousSlices(c); }; }
    CanvasDocument::SliceFreeFn  sliceFreeFn()  { return [this](int s) { sliceAllocator_.freeSlice(s); }; }

    CanvasDocument &document()             { Q_ASSERT(doc_); return *doc_; }
    const CanvasDocument &document() const { Q_ASSERT(doc_); return *doc_; }

    // ファイルパス(このタブに紐づく保存先。未保存なら空文字)。
    QString filePath() const { return filePath_; }
    void setFilePath(const QString &path) { filePath_ = path; }

    // 未保存の間、タブ見出し/タイトルバーに「無題」の代わりに表示する仮の名前。
    QString provisionalName() const { return provisionalName_; }
    void setProvisionalName(const QString &name) { provisionalName_ = name; }

    // initializeGL()が完了済みか。
    bool isGLReady() const { return glReady_; }

    // ストローク中、またはペンが板面上にある(直近にタブレットイベントが届いている=次のペンダウンが目前の可能性が高い)か。
    bool isInkingBusy();

    int    canvasWidth()             const { return canvasW; }
    int    canvasHeight()            const { return canvasH; }
    // キャンバスのサイズ変更と初期化を同時に行う関数
    void   recreateCanvas(int w, int h, bool createDefaultLayers = true,
                           bool wrapX = false, bool wrapY = false);

    // 画像ファイル(PNG/JPEG/BMP等)を読み込んだ際、キャンバスをその画像サイズで作り直し、1枚だけの通常レイヤーに内容をセットする(MainWindow::openFileIntoNewTab用)。
    bool   loadImageAsSingleLayer(const QImage &image, const QString &layerName = QStringLiteral("背景"));

    // 「画像を追加」: 画像ファイルを開いた内容を、既存ドキュメントの現在選択中のレイヤーの直上に新規の通常レイヤーとして挿入する(キャンバス自体は作り直さない)。
    bool   insertImageLayerAboveActive(const QImage &image, const QString &layerName);

    // キャンバスの回転・反転(見た目(ViewTransform)だけでなく、全レイヤーの実ピクセルデータを書き換える)。
    bool   rotateCanvas(int ccwDegrees);
    bool   flipCanvasHorizontal();
    bool   flipCanvasVertical();
    bool   flipCanvasBy(bool horizontal, bool vertical); // flipCanvasHorizontal/Verticalの共通実装

    // キーボードショートカット(Shift+WASD)用: 拡大・縮小・回転/自由変形アクション実行中はその平行移動として、それ以外はアクティブレイヤーの実ピクセル内容の平行移動として扱う(dx,dyはキャンバスpx、Y上向き)。
    void   nudgeActiveContent(int dx, int dy);

    // キャッシュ更新を外部から呼べるようにする (シリアライザ用)。
    void updateCachesPublic() { updateCompositedTex(); }

    // 全レイヤーをクリアして初期状態に戻す (ロード前に呼ぶ)。
    void resetDocument();
    // CanvasSerializer::load()がresetDocument()後にdoc_->setWrap()でループ設定を復元した際、toolCtx_側のキャッシュ済みコピー(wrapX/wrapY)も揃えるために呼ぶ。
    void syncToolContextWrap() { toolCtx_.wrapX = doc_->wrapX(); toolCtx_.wrapY = doc_->wrapY(); }

    // シリアライザ用: スライスのピクセルデータを CPU に読み出す。
    QByteArray readSlicePixels(int texArraySlice);
    // シリアライザ用: スライスにピクセルデータを書き込む。
    void writeSlicePixels(int texArraySlice, const QByteArray &raw);
    // readSlicePixels()をタイル数ぶん逐次呼ぶ代わりに、PBOのリングバッファでパイプライン化して読み出す(save()/captureAllLayerSnapshots()のような多数タイルをまとめて読む箇所向け)。
    QVector<QByteArray> readSlicePixelsBatch(const QVector<int> &slices);
    // 書き込み側のバッチ版(load()のような多数タイルをまとめて書く箇所向け)。
    void writeSlicePixelsBatch(const QVector<int> &slices,
                               const QVector<const QByteArray *> &data,
                               const std::function<void(int)> &onProgress = {});

    // ユーティリティ。
    QByteArray loadShaderSource(const QString &path);
    QColor     toPreMulColor(const QColor &rawColor, float alpha);

    FillTool &fillTool() { return fillTool_; }

    // PsdCodec用: layer.textの内容からグリフをラスタライズしてタイルへ焼き込む(通常はテキスト編集パネルからのみ呼ばれる内部処理だが、
    // PSD読み込み時にTySh由来のTextParamsをタイルへ反映するためにも必要なので公開する)。
    void rasterizeTextLayerPublic(int layerIndex) { rasterizeTextLayer(layerIndex); }

protected:
    void initializeGL()            override;
    void resizeGL(int w, int h)    override;
    void paintGL()                 override;
    void mousePressEvent(QMouseEvent   *e) override;
    void mouseMoveEvent(QMouseEvent    *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent        *e) override;
    void tabletEvent(QTabletEvent      *e) override;
    void showEvent(QShowEvent          *e) override;
#ifdef Q_OS_WIN
    // WM_TABLET_QUERYSYSTEMGESTURESTATUSへの応答でプレス&ホールド等のペンジェスチャを無効化する(tabletEvent()の直接駆動コメント参照)。
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif

public:
    // CanvasActionHost
    QWidget       *hostWidget()      override { return this; }
    ToolContext   &hostToolContext() override { return toolCtx_; }
    CanvasDocument *hostDocument()   override { return doc_.get(); }
    void hostMakeCurrent()  override { makeCurrent(); }
    void hostUpdate()       override { update(); }
    void hostUpdateCursor() override { updateCursor(); }
    void hostRasterizeTextLayer(int layerIndex) override { rasterizeTextLayer(layerIndex); }
    void hostNotifyLayersChanged() override { emit layersChanged(); }
    void hostInvalidateFilterChain() override { invalidateFilterChain(); }
    void hostNotifyToolBlockingActionStarted() override;
    void hostNotifyToolBlockingActionEnded()   override;

private:
    // mousePressEvent/mouseMoveEventで使う、そのイベントに適用すべき筆圧を決める(CanvasWidget.hのlastTabletPressure_/lastTabletEventClock_コメント参照)。
    float resolvePointerPressure(const QMouseEvent *event) const;

    // 生の筆圧へ「環境設定の全体カーブ → アクティブツールのカーブ」を順に適用する。
    float mapPressure(float rawPressure) const;

    // 直近のタブレットイベント由来の傾き・回転をツールへ渡す(マウス操作中は0)。
    void applyPointerTilt(Tool *tool) const;

    // hostNotifyToolBlockingActionStarted/Ended で使う、パネルを開く直前のツールの退避先(色調整/フィルター系パネルを開くと自動で移動ツールに切り替え、閉じると元へ戻す)。
    ToolType toolBeforeBlockingAction_ = ToolType::Pen;
    bool     toolBeforeBlockingActionSaved_ = false;

    // showEvent で Windows のペン/タッチ視覚フィードバックを無効化済みか(多重に呼んでも害はないが、ネイティブウィンドウ生成前に呼んでも意味が無いため初回表示時に一度だけ行う)。
    bool penTouchFeedbackDisabled_ = false;

    // updateCursor()がsetCursor()した直後に呼ぶ。
    void forceCursorRedrawIfUnderMouse();

    // updateCursor()の実体。
    void applyCursor(const QCursor &c);
    void applyUnsetCursor();
    qint64 appliedCursorId_ = 0; // 0 = 未設定(unsetCursor済み)

    ToolConfig *toolCfg_ = nullptr;
    ToolContext toolCtx_;
    int editingMaskLayerIndex_ = -1; // setEditingMaskLayer()参照
    // setEditingMaskLayer()が「編集のために白マスクを自動生成した」レイヤーindex。
    int autoCreatedMaskLayer_ = -1;
    CanvasActionController actions_; // 全アクションの所有・相互排他・描画/入力の委譲
    // コントローラが所有する各アクションへの参照(start する際のハンドル)。
    CanvasAction *hueSatLightAction_        = nullptr;
    CanvasAction *brightnessContrastAction_ = nullptr;
    CanvasAction *colorBalanceAction_       = nullptr;
    CanvasAction *toneCurveAction_          = nullptr;
    CanvasAction *gaussianBlurAction_       = nullptr;
    CanvasAction *customShaderAction_       = nullptr;
    CanvasAction *mosaicAction_             = nullptr;
    CanvasAction *motionBlurAction_         = nullptr;
    CanvasAction *noiseAction_              = nullptr;
    CanvasAction *transformAction_          = nullptr;
    CanvasAction *freeTransformAction_      = nullptr;
    CanvasAction *canvasSizeAction_         = nullptr;
    CanvasAction *imageResolutionAction_    = nullptr;
    // レイヤー編集系(setTarget()で対象を指定してからactions_.start()する)。
    AdjustmentLayerEditAction *adjustmentLayerEditAction_ = nullptr;
    // ガウスぼかしは無料版でも使えるため常に登録される(Pro限定の色収差はFilterLayerEditAction内部で#ifdefにより分離、editFilterLayer側が種類ごとにライセンス確認する)。
    FilterLayerEditAction     *filterLayerEditAction_     = nullptr;
    SolidColorLayerEditAction *solidColorLayerEditAction_ = nullptr;
    TextBoxEditAction         *textBoxEditAction_         = nullptr;
#ifdef TIEPOLO_PRO_BUILD
    CanvasAction *chromaticAberrationAction_ = nullptr;
    CanvasAction *lensBlurAction_            = nullptr;
    CanvasAction *gradientMapAction_         = nullptr;
#endif
    void registerCanvasActions();       // アクションを controller に登録する(コンストラクタから呼ぶ)
    void cancelNonControllerActions();  // まだ CanvasWidget 側に残る変形/キャンバス/レイヤー編集アクションを cancel
    PenEraserTool penTool_;
    PenEraserTool eraserTool_;
    FillTool   fillTool_;
    MoveTool   moveTool_;
    RotateTool rotateTool_;
    DropperTool dropperTool_;
    BlurTool   blurTool_;
    WarpTool   warpTool_;
    SelectTool selectTool_;
    // 色調整/フィルター/変形/キャンバスサイズ系のツール。
    TextTool   textTool_;
    AirbrushTool airbrushTool_;

    Tool *currentTool(); // activeTool に応じて対応するToolを返す
    void setupToolContext();

    // initializeGL()がシェーダーコンパイル等を含め完全に完了したかどうか。
    bool glReady_ = false;
    bool m_initializing = false;
    bool m_suppressDocNotify = false; // doc_変更後にGL側の後処理を挟みたい間、通知を一時抑制する
    void wireDocumentNotifications(); // doc_.onChanged を配線する
    const int MAX_LAYERS = CanvasDocument::MAX_LAYERS;

    // サブシステム
    std::unique_ptr<CanvasDocument> doc_;
    QString filePath_;
    QString provisionalName_;
    ViewTransform  view_;

    // タイル格納用テクスチャバンク
    GLuint layerTexBanks[MAX_TILE_BANKS] = {0};
    int    slicesPerBank_ = 0; // 1バンクのスライス数(= GL_MAX_ARRAY_TEXTURE_LAYERS)
    GLuint bankTexOf(int si)    const { return layerTexBanks[slicesPerBank_ > 0 ? (si / slicesPerBank_) : 0]; }
    int    localSliceOf(int si) const { return slicesPerBank_ > 0 ? (si % slicesPerBank_) : si; }
    // [base, base+count) の連続スライス範囲を、バンク境界で分割しながら uClearColor でGPUクリアする(1回のimageStoreディスパッチは1バンク内に閉じている必要があるため)。
    void clearSliceRange(int base, int count, float r, float g, float b, float a);
    // layerTexBanks[]/slicesPerBank_ の現在値を toolCtx_ へコピーする(バンク生成・伸長時)。
    void syncBanksToToolContext();
    // 全バンクをテクスチャユニット LAYER_BANK_TEXUNIT_BASE..
    void bindLayerBanksForSampling(QOpenGLShaderProgram *prog);
    GLuint maskTex       = 0;
    // ストローク色バッファ(RGBA16F, キャンバスサイズ)。
    GLuint strokeColorTex = 0;
    // render.fragがこのテクスチャに使えるテクスチャユニットがあるか。
    bool   strokeColorSupported_ = false;
    // 必要なら確保して返す(未対応環境や確保失敗時は0)。
    GLuint ensureStrokeColorTex();
    GLuint dummyVAO      = 0;
    GLuint wallTex       = 0;
    GLuint outerJfaTex   = 0;
    GLuint innerJfaTex   = 0;
    GLuint sdfTex        = 0;
    GLuint compositedTex = 0;
    GLuint compositedTileArr = 0;
    GLuint fullLayerTex  = 0; // bake 用フルキャンバス作業テクスチャ
    // ペン先(スタンプ)画像。
    GLuint penTipTex     = 0;
    QString penTipTexPath_; // 現在penTipTexにアップロード済みのパス(再読み込みの重複防止用)
    // 紙質テクスチャ。
    GLuint paperTex      = 0;
    QString paperTexPath_;
    // initializeGL()内でinitializeOpenGLFunctions()が呼ばれるまでは、GL関数ポインタが未解決でGL呼び出しがクラッシュする。
    bool glFunctionsReady_ = false;
    // トーンカーブのLUT(256x1, R8)。
    GLuint toneCurveLUTTex = 0;
    // グラデーションマップのLUT(256x1, RGBA8)。
    GLuint gradientMapLUTTex = 0;
    // ペンストロークのバッチスタンプ用SSBO(stroke.comp参照)。
    GLuint strokeStampSSBO_ = 0;
    // 「筆に乗っている絵の具」(vec4 1個)。
    GLuint brushPaintSSBO_  = 0;
    GLuint selectionMaskTex = 0; // 選択範囲マスク(未選択時は全域255)
    bool   hasSelection_ = false;
    // 選択アウトライン(マーチングアンツ)のキャッシュ。
    QPainterPath selectionOutlineCachePx_;
    bool         selectionOutlineCacheDirty_ = true;
    // 上記(キャンバスpx座標)をウィジェット座標へ変換済みのパス。
    QPainterPath selectionOutlineWidgetPath_;
    QMatrix4x4   selectionOutlineWidgetPathView_;
    QSize        selectionOutlineWidgetPathSize_;
    bool         selectionOutlineWidgetPathValid_ = false;
    void   rebuildSelectionOutlineCache();
    // 選択範囲マスクの中身を変更する操作(selectAll/clearSelection/投げ縄・ペン選択の確定等)から呼ぶ。
    void   invalidateSelectionOutlineCache() {
        selectionOutlineCacheDirty_ = true;
        selectionOutlineWidgetPathValid_ = false; // 変換済みパスも作り直す
    }
    GLuint transformSrcTex        = 0; // 変形確定時の作業用スナップショット(RGBA8)
    GLuint transformSrcSelMaskTex = 0; // 変形確定時の作業用スナップショット(R8)
    int    transformScratchW_ = 0, transformScratchH_ = 0; // fullLayerTex/transformSrcTexの現在サイズ
    // fullLayerTex/transformSrcTexを指定ピクセルサイズに合わせて作り直す(サイズが同じなら何もしない)。
    void   ensureTransformScratchSize(int w, int h);

    // 多段パスのフィルター専用の中間バッファ(RGBA8)。
    GLuint filterScratchTex_ = 0;
    int    filterScratchW_ = 0, filterScratchH_ = 0;
    GLuint ensureFilterScratch(int w, int h);
    // レイヤーの矩形を、キャンバスタイル座標系で指定範囲を覆うように拡張する(CanvasDocument::growLayerBoundsへの委譲。GPU側のコピー/クリアもここで行う)。
    bool   growLayerBoundsToCoverCanvasTiles(int layerIndex, int minTx, int minTy, int maxTxEx, int maxTyEx);
    // 拡大・縮小・回転/自由変形アクション実行中かのフラグは、各 CanvasActionサブクラス(src/actions/TransformActions)の isActive() へ移動した。

    // 調整レイヤー編集/単色レイヤー編集/テキストボックス編集アクションの実体はsrc/actions/LayerEditActions.h の各 CanvasAction
    // サブクラスへ移動した(実行中フラグ・バックアップ・パネル・位置調整とも)。
    void   rasterizeTextLayer(int layerIndex);
    // 1文字ごとにrasterizeTextLayer(テクスチャ再確保を伴いうる)を連打するとGPU側の処理が詰まってクラッシュしうるため、キー入力が止まってから一定時間後にまとめてラスタライズする(デバウンス。TextTool専用で、
    // TextBoxEditActionが編集パネル用に持つ別のデバウンスタイマーとは独立)。
    int     textRasterizeLayerIndex_ = -1;
    QTimer *textRasterizeTimer_ = nullptr;

    // フレームレート律速バッチ(Tool::flushPendingInput参照)用の定期フラッシュタイマー。
    QTimer *inputFlushTimer_ = nullptr;
    // inputFlushTimer_(独立タイマー)も、ペンタブの高頻度マウスイベントで単一スレッドのQtイベントループが埋まっている間はタイムアウトの配信自体が遅れることがあり、「ドラッグ中しばらく反応せず、
    // 指を止めた/緩めた瞬間にまとめて追いつく」症状が残っていた。
    QElapsedTimer strokeFlushClock_;
    // ストローク中の画面更新はupdate()(=再描画の「予約」)ではなく、一定間隔でrepaint()(=その場で同期的にpaintGL)を強制するための計時。
    QElapsedTimer strokePaintClock_;
    // ストローク中の同期描画(repaint)の最短間隔[ms]。
    static constexpr int kStrokePaintIntervalMs = 12;
    // 実際に使う間隔。
    int strokePaintIntervalMs_ = kStrokePaintIntervalMs;
    // 直近の paintGL() 本体の所要時間[ns]。
    qint64 lastPaintGlCostNs_ = 0;
    // ToolContext::requestRepaint から viewChanged() を発行したときのビュー変換。
    QMatrix4x4 lastEmittedViewMatrix_;
    // ドラッグ中に止めた viewChanged() が残っているか(理由は requestRepaint のコメント参照)。
    bool          viewChangedPending_ = false;
    void emitViewChangedIfPending();
    // 同期描画(repaint)が「終わった」時刻。
    QElapsedTimer lastRepaintDoneClock_;
    // 「今処理している入力イベントが、発生してから何ms経っているか」。
    quint64       inputBaseTimestamp_ = 0;
    QElapsedTimer inputBaseClock_;
    qint64        inputAgeMs_ = 0;
    void          updateInputAge(const QMouseEvent *event);
    // 診断ログ用: ストローク開始直後の数フレームだけ実測コストを記録するカウンタ(TIEPOLO_WINLOG有効時のみ使う。書き始めの引っかかりを切り分けるため)。
    int strokeFrameLogCount_ = 0;
    // 診断ログ用: paintGL() が呼ばれた回数と、その本体で使った合計時間。
    quint64 paintGlCalls_    = 0;
    qint64  paintGlNsAccum_  = 0;
    // 診断ログ用: 同期描画(repaint)の中にいるか。
    bool    inSyncRepaint_   = false;
    int     viewDiagCount_   = 0;

    // ストローク中の部分再描画(シザー)用: 前回のpaint以降にGPUディスパッチで実際に変更されたキャンバス領域(キャンバスpx、両端含む)の累積。
    bool  strokeDirtyValid_ = false;
    float strokeDirtyMinX_ = 0, strokeDirtyMinY_ = 0, strokeDirtyMaxX_ = 0, strokeDirtyMaxY_ = 0;
    void  noteStrokeDirtyRegion(float minX, float minY, float maxX, float maxY);
    // タブレットの筆圧はtabletEvent()(実データ)とmousePress/MoveEvent。
    float lastTabletPressure_ = 1.0f;
    // 直近のタブレットイベントの傾き・ペン回転(Tool::setTilt へ渡す形に変換済み)。
    float lastTabletTiltAmount_ = 0.0f;
    float lastTabletTiltAngle_  = 0.0f;
    float lastTabletRotation_   = 0.0f;
    QElapsedTimer lastTabletEventClock_;

    // ペン先ストロークをtabletEvent()から直接駆動している間(TabletPress〜TabletRelease)true。
    bool tabletDriving_ = false;
    bool dispatchingSyntheticTabletMouse_ = false;
    // ペンのダブルタップ検出用。
    QElapsedTimer tabletLastPressClock_;
    QPointF       tabletLastPressPos_;

    // カラーバランス/トーンカーブ/ガウスぼかし/カスタムシェーダー/モザイク/色収差/拡大・縮小・回転/自由変形/キャンバスサイズ変更/画像解像度変更は各
    // CanvasActionサブクラス(src/actions/)へ移動した(フラグ・パネル・位置調整とも)。
    bool   resizeCanvasKeepingContent(int newW, int newH, int offsetX, int offsetY);
    bool   resampleCanvasResolution(int newW, int newH);
    // キャンバスサイズ変更のUndo/Redo用ヘルパー(CanvasDocument::UndoKind::CanvasResize)。
    QVector<LayerSnapshotData> captureAllLayerSnapshots(); // 現在のdoc_の全レイヤーをQImageで読み出す
    void rebuildCanvasFromSnapshots(int newW, int newH, const QVector<LayerSnapshotData> &snaps,
                                     int activeIndex, int offsetX = 0, int offsetY = 0);
    void applyCanvasResizeUndoEntry(const UndoEntry &entry, bool toBefore);
    // 選択範囲Undo(UndoKind::Selection)の適用。
    void applySelectionUndoEntry(const UndoEntry &entry, bool toBefore);
    // 選択範囲マスクの読み出し/書き込み(いずれもqCompress済みのバイト列を扱う)。
    QByteArray captureSelectionMask(); // 全域(生データ、非圧縮)
    void       restoreSelectionMaskRect(const QByteArray &compressed, int x, int y, int w, int h, bool hasSel);
    // beginSelectionUndo()で控えた「変更前」の状態。
    QByteArray pendingSelectionUndoBefore_;
    bool       pendingSelectionUndoHadSel_ = false;
    bool       pendingSelectionUndoValid_  = false;
    // レイヤー追加(UndoKind::LayerAdd)の適用。
    void applyLayerAddUndoEntry(const UndoEntry &entry, bool toBefore);

    // レイヤー削除(UndoKind::LayerRemove)。
    RemovedLayerData captureRemovedLayer(int layerIndex);
    bool restoreRemovedLayer(const RemovedLayerData &d, int insertIndex,
                             const QVector<int> &ancestorFolders);
    void applyLayerRemoveUndoEntry(const UndoEntry &entry, bool toBefore);

    // レイヤー結合(UndoKind::LayerMerge)。
    int  mergeLayersInternal(int survivorIndex, int victimIndex,
                             const QVector<int> &ancestorFolders, LayerMergeUndoData *undoOut);
    void applyLayerMergeUndoEntry(const UndoEntry &entry, bool toBefore);
    // beginLayerAddUndo()〜commitLayerAddUndo()の間だけ >= 0(追加前のアクティブレイヤー)。
    int  pendingLayerAddActiveBefore_ = -1;

    // シェーダー
    QOpenGLShaderProgram *computeDrawProgram         = nullptr;
    QOpenGLShaderProgram *computeBakeProgram         = nullptr;
    // 下地混色で「筆に乗っている絵の具」をスタンプ列に沿って更新する小さなパス。
    QOpenGLShaderProgram *computeBrushStateProgram   = nullptr;
    QOpenGLShaderProgram *computeMaskClearProgram    = nullptr;
    QOpenGLShaderProgram *computeLayerClearProgram   = nullptr;
    QOpenGLShaderProgram *computeCompositeProgram    = nullptr;
    QOpenGLShaderProgram *renderProgram              = nullptr;
    QOpenGLShaderProgram *computeWallProgram         = nullptr;
    QOpenGLShaderProgram *computeJfaProgram          = nullptr;
    QOpenGLShaderProgram *computeJfaInitOuterProgram = nullptr;
    QOpenGLShaderProgram *computeJfaInitInnerProgram = nullptr;
    QOpenGLShaderProgram *computeJfaFinalizeProgram  = nullptr;
    QOpenGLShaderProgram *computeBlurProgram         = nullptr;
    QOpenGLShaderProgram *computeGaussianBlurFilterProgram = nullptr;
    QOpenGLShaderProgram *computeMosaicFilterProgram = nullptr;
    QOpenGLShaderProgram *computeMosaicReduceProgram     = nullptr; // モザイクの1パス目(ブロック平均の集約)
    QOpenGLShaderProgram *computeMotionBlurFilterProgram = nullptr;
    QOpenGLShaderProgram *computeNoiseFilterProgram      = nullptr;
    QOpenGLShaderProgram *computeChromaticAberrationFilterProgram = nullptr; // Pro限定(Free版では常にnullptrのまま)
    QOpenGLShaderProgram *computeLensBlurFilterProgram            = nullptr; // Pro限定(Free版では常にnullptrのまま)
    QOpenGLShaderProgram *computeGradientMapProgram               = nullptr; // Pro限定(Free版では常にnullptrのまま)
    QOpenGLShaderProgram *computeWarpProgram         = nullptr;
    QOpenGLShaderProgram *computeTransformProgram    = nullptr;
    QOpenGLShaderProgram *computeFreeTransformProgram = nullptr;
    QOpenGLShaderProgram *computeHueSatLightProgram   = nullptr;
    QOpenGLShaderProgram *computeBrightnessContrastProgram = nullptr;
    QOpenGLShaderProgram *computeColorBalanceProgram        = nullptr;
    QOpenGLShaderProgram *computeToneCurveProgram           = nullptr;
    QOpenGLShaderProgram *computeBelowCompositeProgram      = nullptr;

    // アクティブレイヤーより下の合成結果。
    GLuint belowCompositeTex          = 0; // RGBA8, canvasW x canvasH
    GLuint belowCompositeClipBaseTex  = 0; // RGBA8, canvasW x canvasH
    bool   belowCompositeCacheValid_  = false;
    // uptoExclusiveIndex未満(0..uptoExclusiveIndex-1)のレイヤーを合成してキャッシュへ書く。
    void updateBelowCompositeCache(int uptoExclusiveIndex);
    void invalidateBelowCompositeCache() { belowCompositeCacheValid_ = false; }
    // このキャッシュを作ったときの uptoExclusiveIndex。
    int    belowCompositeCacheUpto_   = -1;

    // アクティブレイヤーより上の合成結果。
    GLuint aboveCompositeTex          = 0; // RGBA8, canvasW x canvasH
    GLuint aboveCompositeClipScratch_ = 0; // belowComposite.compのclipBase出力用の捨てテクスチャ
    bool   aboveCompositeCacheValid_  = false;
    void updateAboveCompositeCache(int activeIndex);
    void invalidateAboveCompositeCache() { aboveCompositeCacheValid_ = false; }

    // 事前合成キャッシュの先読み
    QTimer *compositeCachePrewarmTimer_ = nullptr;
    // この無効化世代に対して既に先読み作成を試したか(updateAboveCompositeCache は「上に非通常ブレンドがある」等の条件でvalidにせず戻ることがあるため、validフラグだけを見ると毎回作り直しに来てしまう)。
    bool    compositeCachePrewarmDone_  = false;
    void scheduleCompositeCachePrewarm();
    void prewarmCompositeCaches();

    // フィルターレイヤーの多段合成
    GLuint filterChainResult_[2] = {0, 0};
    GLuint filterChainClip_[2]   = {0, 0};
    // 多段パスのフィルターレイヤー(ぼかし)用の中間バッファ。
    GLuint filterChainBlurScratch_ = 0;
    bool   filterChainValid_  = false;
    int    filterChainStartZ_ = -1; // 直近に組んだ連鎖が「どこまで」合成済みか
    GLuint filterChainOutResult_ = 0; // 連鎖の最終出力(render.fragへ渡す)
    GLuint filterChainOutClip_   = 0;
    QOpenGLShaderProgram *computeChromaticAberrationLayerProgram = nullptr; // Pro限定(Free版では常にnullptrのまま)
    QOpenGLShaderProgram *computeGaussianBlurLayerProgram        = nullptr; // 無料版でも使える
    QOpenGLShaderProgram *computeMotionBlurLayerProgram          = nullptr; // 無料版でも使える
    QOpenGLShaderProgram *computeLensBlurLayerProgram             = nullptr; // Pro限定(Free版では常にnullptrのまま)
    QOpenGLShaderProgram *computeMosaicReduceLayerProgram          = nullptr; // 無料版でも使える(モザイクレイヤー1パス目)
    QOpenGLShaderProgram *computeMosaicLayerProgram                 = nullptr; // 無料版でも使える(モザイクレイヤー2パス目)
    QOpenGLShaderProgram *computeNoiseLayerProgram                   = nullptr; // 無料版でも使える

    // 表示に効いている一番上のフィルターレイヤーのindex(無ければ-1)。
    int  topmostActiveFilterLayer() const;
    bool ensureFilterChainTextures();
    void freeFilterChainTextures();
    // [0, startZ) を、フィルターレイヤーで区切りながらフィルター適用済みで合成する。
    void rebuildFilterChain(int startZ, bool withPaintPreview);
    void invalidateFilterChain() { filterChainValid_ = false; }
    // belowComposite.comp を1区間ぶんディスパッチする(連鎖の内部で使う)。
    void dispatchCompositeSegment(int targetStart, int targetEnd,
                                  bool useInit, int initResultIdx, int initClipIdx,
                                  int dstResultIdx, int dstClipIdx, bool withPaintPreview);
    // フィルターレイヤーzの効果を filterChainResult_[srcIdx] -> [srcIdx^1] へかける。
    bool applyFilterLayer(int z, int srcIdx);
    // render.frag に渡すべき (startZ, 事前合成テクスチャ) を決める。
    int  prepareCompositeBase(GLuint &outResult, GLuint &outClip);
    // 書き出し用: 全レイヤーをフィルター適用済みで合成して読み戻す。
    QImage renderExportViaFilterChain();

    // SSBO
    GLuint ssboLayerOpacity   = 0;
    GLuint ssboLayerVisible   = 0;
    GLuint ssboLayerBaseSlice = 0;
    GLuint ssboLayerMaskBaseSlice = 0;
    GLuint ssboLayerAncestorMaskSlices = 0;
    GLuint ssboLayerBlendMode = 0;
    GLuint ssboLayerClipping  = 0;
    GLuint ssboLayerOriginTx  = 0;
    GLuint ssboLayerOriginTy  = 0;
    GLuint ssboLayerTilesX    = 0;
    GLuint ssboLayerTilesY    = 0;
    GLuint ssboLayerIsSolidColor = 0;
    GLuint ssboLayerSolidColor   = 0;
    GLuint ssboLayerAdjKind   = 0;
    GLuint ssboLayerAdjParams = 0;

    // ブラシ状態
    ToolType      activeTool = ToolType::Pen;

    float brushPressure = 1.0f;

    // スライス管理 (CanvasDocument のコールバックとして渡す。実体は LayerSliceAllocator)。
    LayerSliceAllocator sliceAllocator_;

    // 内部ヘルパー
    void initTextures(bool createDefaultLayers = true);
    void updateCompositedTex(); // ナビゲーター用、重いので頻繁に呼ばない
    QColor getPixelColor(const QPointF &widgetPos, bool referenceCanvas = false);

    // ViewTransformの画面座標はデバイスピクセル。
    float viewDpr()    const { return float(devicePixelRatioF()); }
    float viewWidth()  const { return float(width())  * viewDpr(); }
    float viewHeight() const { return float(height()) * viewDpr(); }

    // ウィジェット座標(論理px) → キャンバスピクセル座標。
    QVector2D widgetToPixel(const QPointF &pos) const {
        const float d = viewDpr();
        return view_.widgetToCanvas(QPointF(pos.x() * d, pos.y() * d), qRound(viewHeight()));
    }

    GLuint makeTexture2D(GLenum internalFormat, int w, int h,
                         GLenum filter = GL_NEAREST);

    // Undo 用スナップショット(実体は StrokeUndoRecorder。ここは薄い委譲メソッドのみ)。
    StrokeUndoRecorder undoRecorder_;
    CanvasCompositor   compositor_;
    void      restoreLayer(const UndoEntry &entry);

    UndoEntry captureAllTiles(int layerIndex);
    // captureAllTilesの部分版。
    UndoEntry captureTilesLike(int layerIndex, const QVector<TileUndo> &shape);
    void beginStrokeUndo();
    void expandStrokeUndoRegion(int txMin, int txMax, int tyMin, int tyMax);
    void commitStrokeUndo();
    void setLayerUniformsForRender(QOpenGLShaderProgram *prog);
    // 選択範囲マスクの境界に沿ってマーチングアンツ(白黒交互の点線)を描く。
    void paintSelectionOutline(QPainter &painter);

    void pushUndoSnapshot();

    void freeTextures(); // テクスチャのメモリ解放ヘルパー
};
