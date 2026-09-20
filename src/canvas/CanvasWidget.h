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

// ---------------------------------------------------------------------------
// CanvasWidget
// ---------------------------------------------------------------------------
// CanvasWidget is the integration hub for the canvas widget.  Its implementation
// is intentionally split by responsibility so renderer/input/layer changes do
// not accumulate in one translation unit:
//   CanvasWidget.cpp             lifecycle, wiring, and tool routing
//   CanvasWidgetActions.cpp      CanvasAction entry points
//   CanvasWidgetGeometry.cpp     canvas recreation and geometry operations
//   CanvasWidgetCompositing.cpp  caches, filter chains, previews, and export
//   CanvasWidgetInput.cpp        pointer/tablet event routing
//   CanvasWidgetLayers.cpp       layer and texture-backed editing operations
//   CanvasWidgetRendering.cpp    OpenGL setup, view transforms, and painting
//   CanvasWidgetSelection.cpp    selection state and clipboard operations
//   CanvasWidgetUndo.cpp         document/stroke undo coordination
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
    // スポイトで色を拾った。transparentが真なら「透明色」を拾った(cは無意味)。
    // アルファは含まない(受け取り側が現在の値を維持する。DropperTool参照)。
    void colorDropperd(QColor c, bool transparent);
    void activeToolChanged(ToolType tool); // ツールが変わったら発行
    void activeToolPresetChanged(ToolType tool, int index); // 同じToolType内でツールプリセットが切り替わったら発行
    void modifiedChanged(bool modified);
    void selectionChanged(bool hasSelection); // 選択範囲の有無が変わったら発行
    void viewChanged(); // パン・ズーム・回転・左右反転など表示上のビュー変換が変わったら発行(NavigatorDock用)

public:
    // toolCfg: MainWindowが所有する唯一のToolConfigへの非所有ポインタ。
    // CanvasWidget自身と各Toolインスタンス(penTool_/eraserTool_/fillTool_)で共有する。
    explicit CanvasWidget(ToolConfig *toolCfg, QWidget *parent = nullptr);
    ~CanvasWidget() override;

    ToolConfig *toolConfig() const { return toolCfg_; }

    int canvasW   = 1920;
    int canvasH   = 1080;

    // ------------------------------------------------------------------
    // ビュー変換 (ViewTransform に委譲)
    // ------------------------------------------------------------------
    QMatrix4x4 viewMatrix()        const { return view_.matrix(); }
    QMatrix4x4 viewMatrixInverse() const { return view_.inverseMatrix(); }
    float       viewScale()        const { return view_.scale(); }
    void fitCanvasToView();

    // ウィジェット中心を基準に、指定の絶対倍率(1.0=100%)へズームする
    // (ナビゲータードックのズームスライダー用。マウス位置基準のwheelEventとは別に、
    // 常にウィジェット中心を基準にする)。
    void setViewScaleCentered(float scale);
    // 現在の表示倍率に対して指定の係数(例: 1.1 = 10%拡大, 1/1.1 = 約10%縮小)を
    // 掛けた倍率へズームする(ショートカットキー用。ウィジェット中心基準)。
    void zoomStep(float factor) { setViewScaleCentered(viewScale() * factor); }

    // 表示上の左右反転(キャンバスのデータ自体は変更しない。ナビゲータードックの
    // トグルボタン用)。ペンでの描画等もこのビュー変換を経由するため、反転した
    // 見た目のまま描いても実データには正しい位置に反映される。
    bool isFlippedX() const { return view_.flipX(); }
    void setFlippedX(bool flip);

    // 現在ウィジェットに表示されている範囲を、キャンバスピクセル座標系(Y下向き、
    // QImageと同じ原点)の四角形として返す(パン・ズーム・回転・左右反転をすべて
    // 反映済み)。ナビゲータードックが現在の表示範囲枠を描くのに使う。
    QPolygonF visibleCanvasRectPolygon() const;

    // ビューを平行移動する。deltaはvisibleCanvasRectPolygon()と同じキャンバス
    // ピクセル座標系(Y下向き)での移動量で、内容がこの分だけ画面上を動いて見える
    // ように(移動ツールでの見た目の移動と同じ向きに)ビューのoffsetを更新する
    // (ナビゲータードックの表示範囲枠ドラッグ用。枠を動かす向きはこれの逆になる
    // ため、呼び出し側で符号を反転させて渡す)。
    void panByCanvasDelta(const QVector2D &canvasDeltaYDown);

    // ------------------------------------------------------------------
    // ツール
    // ------------------------------------------------------------------
    ToolType previousTool = ToolType::Pen;  // スペース押下前のツール
    bool spaceHeld    = false;
    
    void     setActiveTool(ToolType tool);
    ToolType getActiveTool() const { return activeTool; }
    void returnToPreviousTool();

    // 同じToolType内でツールプリセット(名前付き設定プリセット)だけを切り替える。
    // toolがactiveToolと異なる場合はsetActiveTool()も内部で呼ぶ(ToolType自体も切り替える)。
    void setActiveToolPreset(ToolType tool, int index);

    // 現在のツール/ツールプリセット/表示倍率に応じてカーソルの見た目を更新する。
    // ブラシサイズなど、マウスイベントを介さずカーソルに影響する設定が変わった
    // 箇所(BrushSizeDock等)からも呼べるようpublicにしてある。
    void updateCursor();

    // ------------------------------------------------------------------
    // 選択範囲
    // ------------------------------------------------------------------
    // 選択範囲マスク(ctx.selectionMaskTex)は「選択範囲内にしか塗れないマスク」として
    // 塗りつぶし/ペン/ぼかし/ゆがみのbake/compute段階から参照される。
    // 今後の移動変形・色変更ツールもToolContext::selectionMaskTex経由で同じものを使える。
    bool hasSelection() const { return hasSelection_; }
    void clearSelection();
    // キャンバス範囲全体を選択する(レイヤーがキャンバスからはみ出ている部分は、
    // 選択範囲マスク自体がキャンバスサイズなので選択されない)。
    void selectAll();

    // ------------------------------------------------------------------
    // 選択範囲のUndo/Redo
    // ------------------------------------------------------------------
    // 選択範囲の作成/追加/削減/解除/全選択もUndo対象にする。レイヤーのタイル差分とは
    // 別種の操作なので、UndoKind::Selectionとして「変更前後のマスク全体」を自己完結で
    // 持つエントリを積む(CanvasResizeと同じ方式)。
    //
    // 使い方: 選択範囲を変えうる操作の直前に beginSelectionUndo()、変え終わった直後に
    // commitSelectionUndo()。変化が無ければcommit側で何も積まないので、空振りしそうな
    // 場所でも気にせず呼んでよい。SelectToolからはToolContext経由で呼ばれる。
    void beginSelectionUndo();
    // afterRaw に「貼ったばかりのマスク」、x/y/w/h にその矩形(キャンバスpx)を渡す。
    // Undoにはこの矩形ぶんだけを記録するので、全域を扱うより大幅に軽い。
    // w<=0 または h<=0 なら「変化なし」として何も積まない。
    void commitSelectionUndo(const QByteArray &afterRaw = QByteArray(),
                             int x = 0, int y = 0, int w = 0, int h = 0);

    // ------------------------------------------------------------------
    // コピー・ペースト(クリップボード)
    // ------------------------------------------------------------------
    // 選択範囲があればその範囲(選択形状でマスクした透過込み)、無ければアクティブ
    // レイヤーの全内容(キャンバス外にはみ出た部分も含む)を画像としてクリップボードへ
    // コピーする。コピー元のキャンバスpx座標も一緒に持たせ、貼り付け時に同じ位置へ
    // 貼り付けられるようにする。Undoは不要(ドキュメントの内容を変更しないため)。
    void copySelection();
    // クリップボードの画像をアクティブレイヤーへアルファ合成で貼り付ける。
    // コピー時に位置情報が付与されていればそこへ、無ければ(外部からの画像等)
    // キャンバス中央へ貼り付ける。Undo/Redo対象。
    void pasteClipboard();

    // ------------------------------------------------------------------
    // 拡大・縮小・回転(編集メニューのアクション。常設ツールではなく、
    // Ctrl+Tで開始しEnterで確定/Escapeでキャンセルする一回限りの操作)。
    // 自由変形と共に実体は src/actions/TransformActions へ移動した。
    // ------------------------------------------------------------------
    bool isTransformActionActive() const { return transformAction_->isActive(); }
    void startTransformAction();   // Ctrl+T: 選択範囲(無ければレイヤー全体)を対象に開始する
    void confirmTransformAction() { actions_.confirmActive(); } // Enter/確定ボタン: 実データへ焼き込んで終了する
    void cancelTransformAction()  { actions_.cancelActive(); }  // Escape/キャンセルボタン: 破棄して終了する

    // ------------------------------------------------------------------
    // 自由変形(編集メニューのアクション。Ctrl+Shift+Tで開始しEnterで確定/
    // Escapeでキャンセルする一回限りの操作。拡大・縮小・回転と同様に
    // 移動/回転もできるが、bboxの4頂点を個別にドラッグしてシアー変形もできる)
    // ------------------------------------------------------------------
    bool isFreeTransformActionActive() const { return freeTransformAction_->isActive(); }
    void startFreeTransformAction();
    void confirmFreeTransformAction() { actions_.confirmActive(); }
    void cancelFreeTransformAction()  { actions_.cancelActive(); }

    // ------------------------------------------------------------------
    // 色調整/フィルター系アクション(色相・彩度・明度 / 明るさ・コントラスト /
    // カラーバランス / トーンカーブ / ガウスぼかし / カスタムシェーダー / モザイク /
    // 色収差(Pro))。実体は src/actions/ の各 CanvasAction サブクラスへ移動した。
    // ここに残る start*() は「メニュー/ショートカットから呼ぶ入口」で、内部で
    // 対応する CanvasAction を actions_(CanvasActionController)へ start するだけ。
    // 確定/キャンセルは confirmActiveAction()/cancelActiveAction() に集約された。
    // 色収差は無料版ビルド or 未認証時は ProFeatureDialog を出すだけになる。
    // ------------------------------------------------------------------
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
    // (MainWindowShortcuts の Enter/Escape ハンドラから使う)
    void confirmActiveAction() { actions_.confirmActive(); }
    void cancelActiveAction()  { actions_.cancelActive(); }
    bool isAnyCanvasActionActive() const { return actions_.isBusy(); }

    // ------------------------------------------------------------------
    // キャンバスサイズ変更(編集メニューのアクション。Ctrl+Alt+Cで開始する。
    // 他の色調整系アクションと違い、キャンバスの枠(8箇所のハンドル)を直接ドラッグ
    // してトリミング/拡張できるほか、キャンバス上に出るパネルで9箇所のアンカーを
    // 選んで幅/高さを数値指定することもできる。確定/Enterで実際にリサイズし、
    // キャンセル/Escapeで破棄する一回限りの操作)
    // ------------------------------------------------------------------
    bool isCanvasSizeActionActive() const { return canvasSizeAction_->isActive(); }
    void startCanvasSizeAction();
    void confirmCanvasSizeAction() { actions_.confirmActive(); }
    void cancelCanvasSizeAction()  { actions_.cancelActive(); }

    // ------------------------------------------------------------------
    // 画像解像度変更(編集メニューのアクション。Ctrl+Alt+Iで開始する。見た目(縦横比・
    // 表示上のサイズ)は変えずに、キャンバス上に出るパネルで幅/高さを数値指定して
    // 全レイヤーの内容を新しい解像度へ拡大縮小(リサンプル)する。CanvasSizeToolと違い
    // キャンバス上のドラッグ操作は無い。確定/Enterで実際にリサンプルし、
    // キャンセル/Escapeで破棄する一回限りの操作)
    // ------------------------------------------------------------------
    bool isImageResolutionActionActive() const { return imageResolutionAction_->isActive(); }
    void startImageResolutionAction();
    void confirmImageResolutionAction() { actions_.confirmActive(); }
    void cancelImageResolutionAction()  { actions_.cancelActive(); }

    // ------------------------------------------------------------------
    // ブラシ
    // ------------------------------------------------------------------
    // 手振れ補正
    float smoothingStrength = 0.5f;
    void  setSmoothingStrength(float s) { smoothingStrength = qBound(0.01f, s, 1.0f); }
    float  getSmoothingStrength() { return smoothingStrength; }

    // ペン先(スタンプ)画像。pathはリソースパス(:/...)でもファイルシステム上のパスでもよい。
    // 読み込みに成功したらpenTipTexへアップロードしtoolCtx_にも反映する。失敗時はfalseを返し、
    // 現在のテクスチャは変更しない(呼び出し側はダイアログでエラー表示するなど)。
    bool setPenTipImage(const QString &path);
    QString currentPenTipImagePath() const { return penTipTexPath_; }

    // 紙質テクスチャ。考え方はsetPenTipImage()と同じだが、こちらはキャンバス座標へ
    // 繰り返して貼るのでGL_REPEATで、縮小時のモアレを避けるためミップも作る。
    // 空のpathを渡すと「紙質なし」になる(テクスチャは解放せず、使わないだけ)。
    bool setPaperTexture(const QString &path);
    QString currentPaperTexturePath() const { return paperTexPath_; }

    // toolCfg_->colorMode()/calibration()(表示モード/モニターキャリブレーション)が
    // 変更された際にMainWindowから呼ばれ、再描画する。
    void applyDisplayConfig();

    // ------------------------------------------------------------------
    // レイヤー操作
    // ------------------------------------------------------------------
    // opacity/visible/blendMode/clipping/name の取得・設定や、レイヤー一覧・
    // アクティブ切り替えなどは document() 経由で CanvasDocument に直接行う。
    // (例: glWidget->document().setLayerOpacity(layerIndex, v))
    //
    // ここに残っているのは、テクスチャの確保/ゼロクリアなど
    // GL資源の後処理が必要なためCanvasWidgetにしか置けないものだけ。
    // insertIndex: -1なら最前面に追加。clipping: 直下のレイヤーにクリップするか。
    // originTx/originTy/tilesXOverride/tilesYOverride: 省略時はキャンバス全面(原点0,0)を
    // 確保する通常の挙動。明示的に指定すると、その矩形(キャンバスより大きい/はみ出した
    // 位置も可)で確保する(CanvasSerializer::loadが保存済みレイヤーの矩形を復元するために使う)。
    // ancestorFolders: 挿入先が実際にフォルダーの中身であれば、そのフォルダー階層
    // チェーン(外側→内側、どの順でもよい)を渡す。CanvasDocument::addLayer参照。
    bool    addLayer(const QString &name = "新規レイヤー",
                     int insertIndex = -1, bool clipping = false,
                     int originTx = 0, int originTy = 0,
                     int tilesXOverride = -1, int tilesYOverride = -1,
                     LayerType layerType = LayerType::Normal,
                     const QVector<int> &ancestorFolders = {});

    // 単色レイヤーを追加する便利関数。addLayer()のlayerType=SolidColor版。
    bool    addSolidColorLayer(const QString &name = "単色レイヤー", int insertIndex = -1,
                                const QColor &color = QColor(255, 255, 255, 255),
                                const QVector<int> &ancestorFolders = {});

    // レイヤー追加をUndo対象にするための、呼び出し側で挟む3点セット
    // (UndoKind::LayerAdd。addLayer()自体はファイル読み込みやUndo復元でも通る
    //  共通経路なのでUndoを積まない)。
    //   beginLayerAddUndo();
    //   if (!addLayer(...)) { abortLayerAddUndo(); return; }
    //   ...種類ごとの設定・アクティブレイヤー変更...
    //   commitLayerAddUndo(insertIndex, ancestorFolders);
    // 追加直後のレイヤーは中身が空なので、ピクセルデータは一切保存しない
    // (Undo=削除 / Redo=同じ設定で作り直す)。LayerAddUndoDataのコメント参照。
    // duplicateSourceIndex >= 0 を渡すと「複製」として記録され、Redoでは空のレイヤーを
    // 作るのではなく同じ複製元から複製し直す(duplicateLayer()もこの3点セットで挟む)。
    // captureContent=true を渡すと、追加されたレイヤーの中身(タイルの生データ)も
    // 記録する。画像インポートのように「空でないレイヤーが1枚増える」場合に使う。
    void    beginLayerAddUndo();
    void    commitLayerAddUndo(int insertedIndex, const QVector<int> &ancestorFolders = {},
                                int duplicateSourceIndex = -1, bool captureContent = false);
    void    abortLayerAddUndo();

    // layerIndexにレイヤーマスクを追加する(1枚のみ)。タイルを確保した上で
    // 白(=全面表示)にGPUクリアする。既に持っている場合は何もせずfalseを返す。
    bool    addLayerMask(int layerIndex);
    // マスクを取り除く(GPUスライスを解放する)。持っていない場合はfalse。
    bool    removeLayerMask(int layerIndex);

    // 現在「マスクを編集中」のレイヤーindex(-1なら誰も編集していない)。
    // PenEraserTool/AirbrushToolはこれをToolContext::editingMaskLayerIndex経由で見て、
    // 描画先をレイヤー本体かマスクかを切り替える(LayerDock::LayerRowWidgetの
    // マスクサムネイルクリックから呼ばれる)。
    int     editingMaskLayerIndex() const { return editingMaskLayerIndex_; }
    // layerIndexを渡すと、すでにそのレイヤーを編集中なら解除(-1に戻す)、
    // それ以外ならそのレイヤーの編集を開始する(=1つだけがアクティブなトグル)。
    void    setEditingMaskLayer(int layerIndex);

    // ------------------------------------------------------------------
    // 一括レイヤーインポート(PSD読み込み等)
    // ------------------------------------------------------------------
    // addLayer()はレイヤー1枚ごとにlayersChanged()を発行し、LayerDock側で
    // 行ウィジェット総入れ替え+全サムネイル再生成(フルキャンバス合成)が走る。
    // 何十~何百枚もレイヤーを連続追加する場面(PSDインポート)でこれをそのまま
    // やるとO(レイヤー数^2)の重さになるため、begin~endの間はaddLayer()の
    // 通知を抑制し、end時に1回だけまとめて通知する。
    void    beginBulkLayerImport();
    void    endBulkLayerImport();

    // 読み込み前に「これから確保するタイル(スライス)総数の見積もり」を渡すと、
    // タイル用テクスチャ配列を一括で確保しておく(読み込み中の細かな伸長+コピーの
    // 繰り返しを避けて高速化する)。見積もりは概算でよい(過不足があっても、
    // 足りなければ従来通り都度伸長するだけで正しく動く)。
    void    reserveTileSlices(int count);

    // layerIndexがフォルダーなら中身ごと削除する。ancestorFoldersはaddLayer同様。
    // includeContentsはCanvasDocument::removeLayer参照(falseならマーカーのみ削除)。
    // 戻り値: 実際に削除できたか(最後の1枚を消そうとした場合等はfalse)。
    bool    removeLayer(int layerIndex, const QVector<int> &ancestorFolders = {}, bool includeContents = true);

    // ------------------------------------------------------------------
    // 調整レイヤーの編集(TextToolと同様、レイヤー自体はLayerDockの
    // 「新規調整レイヤー」で先に作成済み(色相・彩度・明度、全パラメータ0)の
    // ものを使い、LayerDockでそのレイヤーのプレビューをダブルクリックすると
    // 編集パネルを開く。一回限りの操作ではなく何度でも開き直して編集できる)
    // ------------------------------------------------------------------
    // 指定レイヤー(調整レイヤーであること)の編集パネル(明るさ・コントラスト/
    // 色相・彩度・明度、既存の編集アクションと同じパネルクラスを再利用)を出す。
    // パネルの値変更はリアルタイムでその調整レイヤー自身のパラメータに反映され、
    // 下のレイヤーへの効果もその場でプレビューされる(実データは一切書き換えない
    // 非破壊処理)。
    // 実体は src/actions/LayerEditActions.h の AdjustmentLayerEditAction へ移動した。
    bool isAdjustmentLayerActionActive() const { return adjustmentLayerEditAction_->isActive(); }
    void editAdjustmentLayer(int layerIndex);

    // フィルターレイヤーのパラメータ編集。レイヤーリストでプレビューをダブル
    // クリックすると開く。種類(FilterKind)がPro限定(色収差)かどうかで内部的に
    // ライセンス確認を行う(ぼかしは無料版でも常に開ける)。
    void editFilterLayer(int layerIndex);
    // フィルターレイヤーの連鎖キャッシュを無効化する(LayerDockが
    // layer.filter.kind/パラメータを直接書き換えたときに呼ぶ用の公開窓口。
    // 通常のパラメータ変更はCanvasActionHost::hostInvalidateFilterChain経由だが、
    // LayerDockはCanvasActionHostを持たないためこちらを直接呼ぶ)。
    void invalidateFilterChainCache() { invalidateFilterChain(); }
    void confirmAdjustmentLayerEdit() { actions_.confirmActive(); } // パネルの「確定」: 編集内容をそのまま残して終了
    void cancelAdjustmentLayerEdit()  { actions_.cancelActive(); }  // パネルの「キャンセル」: 編集開始前の内容に戻して終了

    // ------------------------------------------------------------------
    // 単色レイヤーの色編集(調整レイヤーと同じ考え方。レイヤー自体はLayerDockの
    // 「新規単色レイヤー」で先に作成済みのものを使い、プレビューのダブルクリックで
    // 編集パネル(OKLCHカラーサークル)を開く。何度でも開き直して編集できる)。
    // 実体は src/actions/LayerEditActions.h の SolidColorLayerEditAction へ移動した。
    // ------------------------------------------------------------------
    bool isSolidColorPickerActionActive() const { return solidColorLayerEditAction_->isActive(); }
    void editSolidColorLayer(int layerIndex);
    void confirmSolidColorLayerEdit() { actions_.confirmActive(); } // パネルの「確定」: 編集内容をそのまま残して終了
    void cancelSolidColorLayerEdit()  { actions_.cancelActive(); }  // パネルの「キャンセル」: 編集開始前の色に戻して終了

    // ------------------------------------------------------------------
    // テキストレイヤー編集(TextToolでテキストボックスをクリック/新規作成すると
    // 開始する。レイヤー自体はLayerDockの「新規テキストレイヤー」で先に作成済み
    // のものを使う。1レイヤーに複数のテキストボックス(textBoxes)を持てる)。
    // 実体は src/actions/LayerEditActions.h の TextBoxEditAction へ移動した。
    // ------------------------------------------------------------------
    // アクティブレイヤーのtextBoxes[boxIndex]を編集パネルで開く
    // (新規ボックスの場合はTextTool側が先にtextBoxesへpush_backしてから呼ぶ)。
    bool isTextLayerEditActive() const { return textBoxEditAction_->isActive(); }
    void startOrEditTextBox(int boxIndex); // TextTool::onMousePress/onMouseDoubleClickから呼ばれる
    void confirmTextLayerEdit() { actions_.confirmActive(); } // パネルの「確定」: 編集内容をそのまま残して終了
    void cancelTextLayerEdit()  { actions_.cancelActive(); }  // パネルの「キャンセル」: 編集開始前の内容に戻して終了
    // ドラッグ操作(移動/拡縮/回転)中に、パネルを介さずデバウンス付きで
    // 実ピクセルへ再ラスタライズしたい場合に呼ぶ(TextTool専用。編集パネルの
    // 確定/キャンセルとは別の独立したデバウンスタイマーを使う)。
    void scheduleTextRasterize(int layerIndex);

    // 「切り取り」: 選択範囲があればその範囲(選択形状どおり)、無ければアクティブ
    // レイヤーの全内容(キャンバス外の部分も含む)をクリップボードへコピーしてから
    // 透明にする。Undo/Redo対象。
    void    cutSelection();
    // nudgeActiveContent()のうち「アクティブレイヤーの実ピクセル内容を平行移動する」
    // 部分(変形アクションが実行中でないとき)。Undo/Redo対象。
    void    nudgeActiveLayer(int dx, int dy);

    // victimIndex を survivorIndex に統合する(victimIndexのblendModeで合成)。
    // 統合後のレイヤーは survivorIndex 側の name/blendMode/clipping を維持し、
    // 不透明度は焼き込まれるため 1.0 にリセットされる。victimは削除される
    // (ancestorFoldersはvictimが実際に属しているフォルダー階層チェーン。
    // フォルダーの中でレイヤーを結合する場合は必ず渡すこと。渡さないと
    // victim削除時にchildCountが正しく減算されない)。
    // 戻り値: 統合後にsurvivorが位置する最終インデックス(失敗時-1)。
    int     mergeLayers(int survivorIndex, int victimIndex, const QVector<int> &ancestorFolders = {});

    // sourceIndex のレイヤー(ピクセル内容含む)を insertIndex の位置に複製する。
    // name/opacity/blendMode/clipping はすべてsourceIndexのものを引き継ぐ
    // (nameには「 コピー」を付加)。フォルダーの場合は中身は複製されない(空の
    // フォルダーとして複製される)。ancestorFoldersはaddLayer同様。
    // 戻り値: 複製後の新規レイヤーのインデックス(失敗時-1)。
    int     duplicateLayer(int sourceIndex, int insertIndex, const QVector<int> &ancestorFolders = {});

    QImage  getLayerPreview(int zStart, int zEnd, int size = 64);
    QImage  getNavigatorPreview(int size, bool fullCanvas = true);
    // 指定レイヤーのマスク濃淡を size に収まるよう縮小したグレースケールプレビューを返す
    // (不透明度プレビューUIが表示に使う)。hasMask==falseなら空QImageを返す。
    QImage  getMaskPreview(int layerIndex, int size);

    // ------------------------------------------------------------------
    // Undo / Redo
    // ------------------------------------------------------------------
    void undo();
    void redo();
    bool canUndo() const { return doc_->canUndo(); }
    bool canRedo() const { return doc_->canRedo(); }
    void markSaved() { doc_->markSaved(); emit modifiedChanged(false); }
    bool isModified() const { return doc_->isModified(); }

    // ------------------------------------------------------------------
    // 塗りつぶし
    // ------------------------------------------------------------------
    void executeFill(const QPointF &pos, float wallThreshold = 0.2f);

    // 書き出し
    QImage exportCanvas();
    bool exportToImage(const QString &filePath);

    // ------------------------------------------------------------------
    // CanvasDocument (CanvasWidgetが所有する。1タブ = 1CanvasWidget = 1CanvasDocument)
    // ------------------------------------------------------------------
    // スライス確保はGLテクスチャ操作を伴うため、CanvasWidget(のLayerSliceAllocator)
    // からしか提供できない。CanvasDocument自体はこれらを関数オブジェクトとして
    // 受け取るだけなので、GLへの依存はここでも漏れない。
    CanvasDocument::SliceAllocFn sliceAllocFn() { return [this](int c) { return sliceAllocator_.allocContiguousSlices(c); }; }
    CanvasDocument::SliceFreeFn  sliceFreeFn()  { return [this](int s) { sliceAllocator_.freeSlice(s); }; }

    CanvasDocument &document()             { Q_ASSERT(doc_); return *doc_; }
    const CanvasDocument &document() const { Q_ASSERT(doc_); return *doc_; }

    // ファイルパス(このタブに紐づく保存先。未保存なら空文字)
    QString filePath() const { return filePath_; }
    void setFilePath(const QString &path) { filePath_ = path; }

    // 未保存の間、タブ見出し/タイトルバーに「無題」の代わりに表示する仮の名前
    // (例: "Canvas001"。保存済みファイルと被らないよう新規作成時に採番される)
    QString provisionalName() const { return provisionalName_; }
    void setProvisionalName(const QString &name) { provisionalName_ = name; }

    // initializeGL()が完了済みか(複数タブ化により、新規タブはGL初期化が非同期で
    // 完了するまでrecreateCanvas()/CanvasSerializer等を呼んではいけないため公開する)
    bool isGLReady() const { return glReady_; }

    // ストローク中、またはペンが板面上にある(直近にタブレットイベントが届いている=
    // 次のペンダウンが目前の可能性が高い)か。ナビゲーター/レイヤードックの重い
    // 再生成(フルキャンバス合成+同期glReadPixels)をこの間は先送りして、
    // ペンダウン応答(書き味)を優先するために使う。
    bool isInkingBusy();

    int    canvasWidth()             const { return canvasW; }
    int    canvasHeight()            const { return canvasH; }
    // キャンバスのサイズ変更と初期化を同時に行う関数
    // createDefaultLayers: falseにすると初期レイヤー(単色レイヤー+作業レイヤー)を作らず
    // 空の状態で返す(PsdCodec::loadがPSD側のレイヤー構成をそのまま復元するために使う)。
    void   recreateCanvas(int w, int h, bool createDefaultLayers = true,
                           bool wrapX = false, bool wrapY = false);

    // 画像ファイル(PNG/JPEG/BMP等)を読み込んだ際、キャンバスをその画像サイズで
    // 作り直し、1枚だけの通常レイヤーに内容をセットする(MainWindow::openFileIntoNewTab用)。
    bool   loadImageAsSingleLayer(const QImage &image, const QString &layerName = QStringLiteral("背景"));

    // 「画像を追加」: 画像ファイルを開いた内容を、既存ドキュメントの現在選択中のレイヤーの
    // 直上に新規の通常レイヤーとして挿入する(キャンバス自体は作り直さない)。
    // 画像はキャンバス中央に配置し、キャンバスよりはみ出す場合はレイヤーの矩形が
    // その分キャンバス外へ広がる(他のツールと同じ「はみ出し可」の仕組み)。
    bool   insertImageLayerAboveActive(const QImage &image, const QString &layerName);

    // キャンバスの回転・反転(見た目(ViewTransform)だけでなく、全レイヤーの実
    // ピクセルデータを書き換える)。ccwDegreesは90/180/270のいずれか(反時計回り)。
    // 90/270度では幅と高さが入れ替わる。戻り値: 実行できたか。
    bool   rotateCanvas(int ccwDegrees);
    bool   flipCanvasHorizontal();
    bool   flipCanvasVertical();
    bool   flipCanvasBy(bool horizontal, bool vertical); // flipCanvasHorizontal/Verticalの共通実装

    // キーボードショートカット(Shift+WASD)用: 拡大・縮小・回転/自由変形アクション
    // 実行中はその平行移動として、それ以外はアクティブレイヤーの実ピクセル内容の
    // 平行移動として扱う(dx,dyはキャンバスpx、Y上向き)。
    void   nudgeActiveContent(int dx, int dy);

    // キャッシュ更新を外部から呼べるようにする (シリアライザ用)
    void updateCachesPublic() { updateCompositedTex(); }

    // 全レイヤーをクリアして初期状態に戻す (ロード前に呼ぶ)
    void resetDocument();
    // CanvasSerializer::load()がresetDocument()後にdoc_->setWrap()でループ設定を
    // 復元した際、toolCtx_側のキャッシュ済みコピー(wrapX/wrapY)も揃えるために呼ぶ。
    void syncToolContextWrap() { toolCtx_.wrapX = doc_->wrapX(); toolCtx_.wrapY = doc_->wrapY(); }

    // シリアライザ用: スライスのピクセルデータを CPU に読み出す
    QByteArray readSlicePixels(int texArraySlice);
    // シリアライザ用: スライスにピクセルデータを書き込む
    void writeSlicePixels(int texArraySlice, const QByteArray &raw);
    // readSlicePixels()をタイル数ぶん逐次呼ぶ代わりに、PBOのリングバッファで
    // パイプライン化して読み出す(save()/captureAllLayerSnapshots()のような
    // 多数タイルをまとめて読む箇所向け)。戻り値はslicesと同じ順序・同じ要素数。
    QVector<QByteArray> readSlicePixelsBatch(const QVector<int> &slices);
    // 書き込み側のバッチ版(load()のような多数タイルをまとめて書く箇所向け)。
    // slicesとdataは同じ長さ・同じ順序であること。詳細はStrokeUndoRecorder側のコメント参照。
    void writeSlicePixelsBatch(const QVector<int> &slices,
                               const QVector<const QByteArray *> &data,
                               const std::function<void(int)> &onProgress = {});

    // ユーティリティ
    QByteArray loadShaderSource(const QString &path);
    QColor     toPreMulColor(const QColor &rawColor, float alpha);

    FillTool &fillTool() { return fillTool_; }

    // PsdCodec用: layer.textの内容からグリフをラスタライズしてタイルへ焼き込む
    // (通常はテキスト編集パネルからのみ呼ばれる内部処理だが、PSD読み込み時に
    // TySh由来のTextParamsをタイルへ反映するためにも必要なので公開する)。
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
    // WM_TABLET_QUERYSYSTEMGESTURESTATUSへの応答でプレス&ホールド等の
    // ペンジェスチャを無効化する(tabletEvent()の直接駆動コメント参照)。
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif

public:
    // ---- CanvasActionHost 実装(色調整/フィルター等のアクションが CanvasWidget の内部に
    // 直接依存せず必要資源だけを取得するための狭いインターフェース) ----
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
    // mousePressEvent/mouseMoveEventで使う、そのイベントに適用すべき筆圧を決める
    // (CanvasWidget.hのlastTabletPressure_/lastTabletEventClock_コメント参照)。
    float resolvePointerPressure(const QMouseEvent *event) const;

    // 生の筆圧へ「環境設定の全体カーブ → アクティブツールのカーブ」を順に適用する。
    // Tool::setPressure() を呼ぶ全ての箇所がここを通す(実装側のコメント参照)。
    float mapPressure(float rawPressure) const;

    // 直近のタブレットイベント由来の傾き・回転をツールへ渡す(マウス操作中は0)。
    // setPressure と同じ箇所から必ず一緒に呼ぶ。
    void applyPointerTilt(Tool *tool) const;

    // hostNotifyToolBlockingActionStarted/Ended で使う、パネルを開く直前のツールの退避先
    // (色調整/フィルター系パネルを開くと自動で移動ツールに切り替え、閉じると元へ戻す)。
    ToolType toolBeforeBlockingAction_ = ToolType::Pen;
    bool     toolBeforeBlockingActionSaved_ = false;

    // showEvent で Windows のペン/タッチ視覚フィードバックを無効化済みか(多重に
    // 呼んでも害はないが、ネイティブウィンドウ生成前に呼んでも意味が無いため
    // 初回表示時に一度だけ行う)。
    bool penTouchFeedbackDisabled_ = false;

    // updateCursor()がsetCursor()した直後に呼ぶ。Windowsのペンタブ入力では、
    // 新しいカーソル画像に切り替えてもペン先を実際に動かすまで古い画像が
    // 画面に残ることがあるため、その場合だけ回避策を行う(CanvasWidget.cppの
    // 実装コメント参照)。
    void forceCursorRedrawIfUnderMouse();

    // updateCursor()の実体。カーソルが実際に変化したときだけsetCursor()と
    // forceCursorRedrawIfUnderMouse()を行うためのヘルパーと、直前に適用した
    // カーソルの識別子(CanvasWidget.cppの実装コメント参照)。
    void applyCursor(const QCursor &c);
    void applyUnsetCursor();
    qint64 appliedCursorId_ = 0; // 0 = 未設定(unsetCursor済み)

    ToolConfig *toolCfg_ = nullptr;
    ToolContext toolCtx_;
    int editingMaskLayerIndex_ = -1; // setEditingMaskLayer()参照
    // setEditingMaskLayer()が「編集のために白マスクを自動生成した」レイヤーindex。
    // 一度も描かれずに編集モードを抜けたら、そのマスクを破棄して一律不透明度へ戻す。
    int autoCreatedMaskLayer_ = -1;
    CanvasActionController actions_; // 全アクションの所有・相互排他・描画/入力の委譲
    // コントローラが所有する各アクションへの参照(start する際のハンドル)。
    // 型は基底 CanvasAction* で持ち、具体クラスは src/actions/ 側にだけ依存させる。
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
    // ガウスぼかしは無料版でも使えるため常に登録される(Pro限定の色収差は
    // FilterLayerEditAction内部で#ifdefにより分離、editFilterLayer側が種類ごとに
    // ライセンス確認する)。
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
    // 色調整/フィルター/変形/キャンバスサイズ系のツール(HueSatLight/BrightnessContrast/
    // ColorBalance/ToneCurve/GaussianBlur/CustomShader/Mosaic/ChromaticAberration/
    // Transform/FreeTransform/CanvasSize/ImageResolution)は、各 CanvasAction サブクラス
    // (src/actions/)へ移動した。
    TextTool   textTool_;
    AirbrushTool airbrushTool_;

    Tool *currentTool(); // activeTool に応じて対応するToolを返す
    void setupToolContext();

    // initializeGL()がシェーダーコンパイル等を含め完全に完了したかどうか。
    // 完了前にマウス操作が届いても、未初期化のtoolCtx_/GL資源に触れないよう
    // マウスイベント側でこのフラグをガードに使う。
    bool glReady_ = false;
    bool m_initializing = false;
    bool m_suppressDocNotify = false; // doc_変更後にGL側の後処理を挟みたい間、通知を一時抑制する
    void wireDocumentNotifications(); // doc_.onChanged を配線する
    const int MAX_LAYERS = CanvasDocument::MAX_LAYERS;

    // ---- サブシステム ----------------------------------------------------
    // CanvasDocumentはCanvasWidgetが所有する(1タブ = 1CanvasWidget = 1CanvasDocument)
    std::unique_ptr<CanvasDocument> doc_;
    QString filePath_;
    QString provisionalName_;
    ViewTransform  view_;

    // ---- テクスチャ -------------------------------------------------------
    // タイル格納用テクスチャ配列の「バンク」群(詳細は CanvasDocument.h の MAX_TILE_BANKS
    // と LayerSliceAllocator)。グローバルスライス番号 si は bankTexOf(si)/localSliceOf(si)
    // で (バンクテクスチャ, バンク内ローカルスライス番号) に分解する。
    GLuint layerTexBanks[MAX_TILE_BANKS] = {0};
    int    slicesPerBank_ = 0; // 1バンクのスライス数(= GL_MAX_ARRAY_TEXTURE_LAYERS)
    GLuint bankTexOf(int si)    const { return layerTexBanks[slicesPerBank_ > 0 ? (si / slicesPerBank_) : 0]; }
    int    localSliceOf(int si) const { return slicesPerBank_ > 0 ? (si % slicesPerBank_) : si; }
    // [base, base+count) の連続スライス範囲を、バンク境界で分割しながら uClearColor で
    // GPUクリアする(1回のimageStoreディスパッチは1バンク内に閉じている必要があるため)。
    void clearSliceRange(int base, int count, float r, float g, float b, float a);
    // layerTexBanks[]/slicesPerBank_ の現在値を toolCtx_ へコピーする(バンク生成・伸長時)。
    void syncBanksToToolContext();
    // 全バンクをテクスチャユニット LAYER_BANK_TEXUNIT_BASE.. にバインドし、progの
    // sampler2DArray配列 uniform "layerTexBanks[]" と "uSlicesPerBank" を設定する
    // (render.frag / composite.comp / belowComposite.comp が layerTexArray を
    // サンプルしていた箇所すべてで使う)。未生成バンクのスロットにはbank0を割り当てる。
    void bindLayerBanksForSampling(QOpenGLShaderProgram *prog);
    GLuint maskTex       = 0;
    // ストローク色バッファ(RGBA16F, キャンバスサイズ)。スタンプごとに色が変わる
    // 設定(色のランダム、将来の混色・水彩)のときだけ使う。使わない人に
    // キャンバス1枚ぶんの追加メモリを払わせないよう、初回に必要になった時点で
    // 確保する(freeTextures()でキャンバスと一緒に解放される)。
    GLuint strokeColorTex = 0;
    // render.fragがこのテクスチャに使えるテクスチャユニットがあるか。
    // 0〜7は個別のテクスチャ、8〜15はレイヤーバンク(LAYER_BANK_TEXUNIT_BASE)で
    // 埋まっているため16番が要る。GL4.3の下限はちょうど16なので実行時に確かめ、
    // 足りない環境ではストローク色の経路自体を使わない。
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
    // ペン先(スタンプ)画像。キャンバスサイズに依存しないので、freeTextures()/initTextures()の
    // サイクル(recreateCanvas等)では作り直さず、initializeGL()で一度だけ確保し、
    // 以後はsetPenTipImage()で中身とサイズだけ差し替える。
    GLuint penTipTex     = 0;
    QString penTipTexPath_; // 現在penTipTexにアップロード済みのパス(再読み込みの重複防止用)
    // 紙質テクスチャ。扱いはpenTipTexと同じ(キャンバスサイズ非依存・都度差し替え)。
    GLuint paperTex      = 0;
    QString paperTexPath_;
    // initializeGL()内でinitializeOpenGLFunctions()が呼ばれるまでは、GL関数ポインタが
    // 未解決でGL呼び出しがクラッシュする。MainWindowの構築時(ToolPropDockが先端画像の
    // プレビューを初期化する時点など)はまだinitializeGL()が一度も走っていないことがあるため、
    // setPenTipImage()がこのフラグでGL呼び出しの可否を判断する。
    bool glFunctionsReady_ = false;
    // トーンカーブのLUT(256x1, R8)。キャンバスサイズに依存しないのでpenTipTexと
    // 同様initializeGL()で一度だけ確保し、以後はToneCurveTool::uploadLutが
    // glTexSubImage2Dで中身だけ差し替える。
    GLuint toneCurveLUTTex = 0;
    // グラデーションマップのLUT(256x1, RGBA8)。輝度1つから色を引くのでRGBA8である
    // 以外はtoneCurveLUTTexと同じ扱い(GradientMapTool::uploadLutが中身を差し替える)。
    GLuint gradientMapLUTTex = 0;
    // ペンストロークのバッチスタンプ用SSBO(stroke.comp参照)。1マウスイベント内で
    // 複数スタンプを打つ場合に、スタンプごとdispatch+barrierを繰り返す代わりに
    // 全スタンプ位置をここへ書き込んで1回のdispatchで処理するために使う
    // (PenEraserTool::onMouseMove)。キャンバスサイズに依存しないので
    // toneCurveLUTTexと同様initializeGL()で一度だけ確保する。
    GLuint strokeStampSSBO_ = 0;
    // 「筆に乗っている絵の具」(vec4 1個)。下地混色でストロークをまたいで持ち回る。
    GLuint brushPaintSSBO_  = 0;
    GLuint selectionMaskTex = 0; // 選択範囲マスク(未選択時は全域255)
    bool   hasSelection_ = false;
    // 選択アウトライン(マーチングアンツ)のキャッシュ。paintSelectionOutline()は
    // 以前、毎フレームselectionMaskTexをCPUへ読み戻して境界形状を再計算していたが
    // (glGetTexImage+全ピクセル走査+QRegion分解、キャンバスが大きいほど重い)、
    // 選択範囲マスクの中身が実際に変わった時だけ再計算すればよいので、キャンバスpx
    // 座標系のパスをキャッシュしておく(ビュー変換の適用は毎フレーム必要なので、
    // widget座標への変換自体は都度行う)。
    QPainterPath selectionOutlineCachePx_;
    bool         selectionOutlineCacheDirty_ = true;
    // 上記(キャンバスpx座標)をウィジェット座標へ変換済みのパス。マーチングアンツは
    // 数千セグメントになり得るので、毎フレーム作り直すと描画コストが支配的になる
    // (CanvasWidget::paintSelectionOutlineのコメント参照)。選択範囲・ビュー変換・
    // ウィジェットサイズのいずれかが変わったときだけ作り直す。
    QPainterPath selectionOutlineWidgetPath_;
    QMatrix4x4   selectionOutlineWidgetPathView_;
    QSize        selectionOutlineWidgetPathSize_;
    bool         selectionOutlineWidgetPathValid_ = false;
    void   rebuildSelectionOutlineCache();
    // 選択範囲マスクの中身を変更する操作(selectAll/clearSelection/投げ縄・ペン選択の
    // 確定等)から呼ぶ。次回のpaintSelectionOutline()で再計算させる。
    void   invalidateSelectionOutlineCache() {
        selectionOutlineCacheDirty_ = true;
        selectionOutlineWidgetPathValid_ = false; // 変換済みパスも作り直す
    }
    GLuint transformSrcTex        = 0; // 変形確定時の作業用スナップショット(RGBA8)
    GLuint transformSrcSelMaskTex = 0; // 変形確定時の作業用スナップショット(R8)
    int    transformScratchW_ = 0, transformScratchH_ = 0; // fullLayerTex/transformSrcTexの現在サイズ
    // fullLayerTex/transformSrcTexを指定ピクセルサイズに合わせて作り直す(サイズが同じなら何もしない)。
    // レイヤーの矩形がgrowLayerBoundsで変わるため、キャンバスサイズ固定では足りない。
    void   ensureTransformScratchSize(int w, int h);

    // 多段パスのフィルター専用の中間バッファ(RGBA8)。
    // ガウスぼかしは「横1D → 縦1D」の2パスに分離してあり、モザイクは
    // 「ブロックごとの平均を1テクセルに集約 → 引くだけ」の2パスにしてある。
    // どちらも読み込み元と書き込み先を同じテクスチャにできないため、
    // fullLayerTex(元画像。半径変更のたびに読み直すので壊せない)と
    // transformSrcTex(プレビュー結果。render.fragが表示に使う)とは別に
    // もう1枚必要になる。ensureTransformScratchSizeと違い、実際に多段パスの
    // フィルターを開いたときにだけ確保する(変形ツールなど他の用途では
    // 1枚ぶんのVRAMを無駄にしない)。
    GLuint filterScratchTex_ = 0;
    int    filterScratchW_ = 0, filterScratchH_ = 0;
    GLuint ensureFilterScratch(int w, int h);
    // レイヤーの矩形を、キャンバスタイル座標系で指定範囲を覆うように拡張する
    // (CanvasDocument::growLayerBoundsへの委譲。GPU側のコピー/クリアもここで行う)。
    // 拡張が発生した場合、そのレイヤーのタイル差分Undo履歴を無効化する。
    bool   growLayerBoundsToCoverCanvasTiles(int layerIndex, int minTx, int minTy, int maxTxEx, int maxTyEx);
    // 拡大・縮小・回転/自由変形アクション実行中かのフラグは、各 CanvasAction
    // サブクラス(src/actions/TransformActions)の isActive() へ移動した。

    // 調整レイヤー編集/単色レイヤー編集/テキストボックス編集アクションの実体は
    // src/actions/LayerEditActions.h の各 CanvasAction サブクラスへ移動した
    // (実行中フラグ・バックアップ・パネル・位置調整とも)。CanvasWidget に残るのは、
    // タイル書き込みを伴う低レベル処理(GL資源・スライス直接操作)である
    // rasterizeTextLayer() と、TextTool専用の独立したデバウンスタイマーだけ。
    // layer.textBoxesの内容からグリフをラスタライズしてタイルへ焼き込む
    // (テキスト編集パネルの値が変わるたび、またはTextToolでのドラッグ操作のたびに呼ぶ)。
    void   rasterizeTextLayer(int layerIndex);
    // 1文字ごとにrasterizeTextLayer(テクスチャ再確保を伴いうる)を連打すると
    // GPU側の処理が詰まってクラッシュしうるため、キー入力が止まってから
    // 一定時間後にまとめてラスタライズする(デバウンス。TextTool専用で、
    // TextBoxEditActionが編集パネル用に持つ別のデバウンスタイマーとは独立)。
    int     textRasterizeLayerIndex_ = -1;
    QTimer *textRasterizeTimer_ = nullptr;

    // フレームレート律速バッチ(Tool::flushPendingInput参照)用の定期フラッシュ
    // タイマー。当初はQOpenGLWidget::update()経由のpaintGL()呼び出しに便乗させて
    // いたが、ペンタブの高頻度なマウスイベントが単一スレッドのQtイベントループ上で
    // 優先度の低いUpdateRequest(update()が積むペイントイベント)を実質的に
    // 飢餓状態にしてしまい、ドラッグ中ずっとpaintGL()が一度も呼ばれず、離すまで
    // 線が全く表示されないという不具合を引き起こした。paintGL任せにせず、
    // 独立したタイマーで確実に一定間隔(60Hz目安)でflushPendingInput()を呼び、
    // その中でupdate()も明示的に発行することで、入力イベントの混雑状況に
    // 左右されずに描画を進める。
    QTimer *inputFlushTimer_ = nullptr;
    // inputFlushTimer_(独立タイマー)も、ペンタブの高頻度マウスイベントで単一スレッドの
    // Qtイベントループが埋まっている間はタイムアウトの配信自体が遅れることがあり、
    // 「ドラッグ中しばらく反応せず、指を止めた/緩めた瞬間にまとめて追いつく」症状が
    // 残っていた。タイマー/paintGL任せにせず、mousePressEvent/mouseMoveEvent自身の中で
    // 経過時間を見て必要ならその場でflushPendingInput()するための計測用。
    QElapsedTimer strokeFlushClock_;
    // ストローク中の画面更新はupdate()(=再描画の「予約」)ではなく、一定間隔で
    // repaint()(=その場で同期的にpaintGL)を強制するための計時。update()の予約は
    // ペンタブの高頻度イベントがキューへ流れ込み続けている間は配信が後回しに
    // され続け、「書き始めてもしばらく線が見えず、あとからパッとまとめて現れる」
    // 原因になる(タブレット直接駆動でイベント頻度が上がりこの飢餓が悪化した)。
    QElapsedTimer strokePaintClock_;
    // ストローク中の同期描画(repaint)の最短間隔[ms]。約80Hz。
    static constexpr int kStrokePaintIntervalMs = 12;
    // 実際に使う間隔。直近のrepaint実測コストに応じて適応させる(描画に使う時間が
    // 全体の約半分以下になるよう interval = max(最短, 実測コスト×2)。重い環境でも
    // 入力イベントの処理が滞らず、キューに入力が溜まって線が遅れて追いかけてくる
    // 状態にならない)。ストローク開始時に最短値へリセットする。
    int strokePaintIntervalMs_ = kStrokePaintIntervalMs;
    // 直近の paintGL() 本体の所要時間[ns]。上の適応間隔の材料。
    //
    // 【重要】repaint() の実測時間をそのまま使ってはいけない。repaint() には
    // paintGL() のあとのバックingストア転送とpresentが含まれ、そこでvsync待ちや
    // GPUキューの消化待ちが起きる(実測: paintGL本体0.3msに対しrepaint全体14ms、
    // 事前合成キャッシュを作った直後のフレームでは106ms)。この待ち時間を「描画が
    // 重い」と解釈して間隔を倍にすると、表示できる速度より遅く描くことになり、
    // 書き始めだけカクついて見える原因そのものになっていた。適応の目的は
    // 「paintGLのCPU処理が重い環境で入力処理が飢えないようにする」ことなので、
    // 材料は paintGL 本体の時間だけでよい。
    qint64 lastPaintGlCostNs_ = 0;
    // ToolContext::requestRepaint から viewChanged() を発行したときのビュー変換。
    // 変化していないのに毎入力イベントで発行しないための比較用(理由は
    // setupToolContext() の requestRepaint のコメント参照)。
    QMatrix4x4 lastEmittedViewMatrix_;
    // ドラッグ中に止めた viewChanged() が残っているか(理由は requestRepaint の
    // コメント参照)。止めたぶんは、ドラッグが終わった時点で必ず1回出す。
    bool          viewChangedPending_ = false;
    void emitViewChangedIfPending();
    // 同期描画(repaint)が「終わった」時刻。inputFlushTimer_ が取りこぼし用の
    // update() を積むかどうかの判定に使う(strokePaintClock_ は repaint の開始時に
    // restartされるため、この判定には使えない。詳細はタイマー側のコメント)。
    QElapsedTimer lastRepaintDoneClock_;
    // 「今処理している入力イベントが、発生してから何ms経っているか」。
    // 入力が溜まっている(＝画面がポインタから遅れている)かの判定に使う。
    // 詳細は mouseMoveEvent の同期描画の条件のコメント。
    quint64       inputBaseTimestamp_ = 0;
    QElapsedTimer inputBaseClock_;
    qint64        inputAgeMs_ = 0;
    void          updateInputAge(const QMouseEvent *event);
    // 診断ログ用: ストローク開始直後の数フレームだけ実測コストを記録するカウンタ
    // (TIEPOLO_WINLOG有効時のみ使う。書き始めの引っかかりを切り分けるため)。
    int strokeFrameLogCount_ = 0;
    // 診断ログ用: paintGL() が呼ばれた回数と、その本体で使った合計時間。
    // repaint() 1回の中で「自前のGL描画」と「Qtのバッキングストア合成+present」の
    // どちらに時間が行っているかを切り分けるために使う。
    quint64 paintGlCalls_    = 0;
    qint64  paintGlNsAccum_  = 0;
    // 診断ログ用: 同期描画(repaint)の中にいるか。ここが偽のまま paintGL() が
    // 呼ばれたら、どこかの update() が積んだ「余分なフレーム」ということになる。
    bool    inSyncRepaint_   = false;
    int     viewDiagCount_   = 0;

    // ストローク中の部分再描画(シザー)用: 前回のpaint以降にGPUディスパッチで実際に
    // 変更されたキャンバス領域(キャンバスpx、両端含む)の累積。paintGL()が消費して
    // リセットする。有効(strokeDirtyValid_)かつストローク中なら、paintGL()はこの
    // 矩形+マージンだけをシザーで再描画し、残りは前フレームのFBO内容をそのまま保持
    // する(コンストラクタでsetUpdateBehavior(PartialUpdate)を設定している)。
    // ウィンドウ全画素×(アクティブ以上のレイヤー数)の再合成が毎フレーム走るのを
    // 避ける仕組みで、これが無いとレイヤー数×ウィンドウ画素数に比例して1回の描画が
    // 重くなり、下のレイヤーに描くときほど極端に遅くなる。
    bool  strokeDirtyValid_ = false;
    float strokeDirtyMinX_ = 0, strokeDirtyMinY_ = 0, strokeDirtyMaxX_ = 0, strokeDirtyMaxY_ = 0;
    void  noteStrokeDirtyRegion(float minX, float minY, float maxX, float maxY);
    // タブレットの筆圧はtabletEvent()(実データ)とmousePress/MoveEvent(そこから
    // Qtが合成したQMouseEvent。environments/ドライバによってはOS側が同じ物理接触に
    // 対して本物の(=event->source()ではタブレット由来と見分けが付かない)重複した
    // マウスイベントも送ってくることがあり、その場合points().first().pressure()が
    // 実際の筆圧を反映していないことがある)の2経路から届く。直近(数十ms以内)に
    // tabletEvent()が届いていれば、そちらの値を優先的に使う。
    float lastTabletPressure_ = 1.0f;
    // 直近のタブレットイベントの傾き・ペン回転(Tool::setTilt へ渡す形に変換済み)。
    // 筆圧と同じく、直近数十msにタブレットイベントが来ていればタブレット操作中と
    // みなしてこれを使い、そうでなければマウス操作として 0 を渡す。
    float lastTabletTiltAmount_ = 0.0f;
    float lastTabletTiltAngle_  = 0.0f;
    float lastTabletRotation_   = 0.0f;
    QElapsedTimer lastTabletEventClock_;

    // ペン先ストロークをtabletEvent()から直接駆動している間(TabletPress〜
    // TabletRelease)true。この間に届く「本物の」マウスイベント(OS互換レイヤーが
    // 同じ接触から合成した遅延・重複ストリーム)は完全に無視する。
    // tabletEvent()自身が組み立てた合成QMouseEventだけは
    // dispatchingSyntheticTabletMouse_で見分けて通す。
    bool tabletDriving_ = false;
    bool dispatchingSyntheticTabletMouse_ = false;
    // ペンのダブルタップ検出用(マウス合成を止めるとOS/Qtのダブルクリック合成も
    // 来なくなるため、テキストボックス編集等のダブルクリック動作を自前で発火する)
    QElapsedTimer tabletLastPressClock_;
    QPointF       tabletLastPressPos_;

    // カラーバランス/トーンカーブ/ガウスぼかし/カスタムシェーダー/モザイク/色収差/
    // 拡大・縮小・回転/自由変形/キャンバスサイズ変更/画像解像度変更は各 CanvasAction
    // サブクラス(src/actions/)へ移動した(フラグ・パネル・位置調整とも)。
    bool   resizeCanvasKeepingContent(int newW, int newH, int offsetX, int offsetY);
    bool   resampleCanvasResolution(int newW, int newH);
    // キャンバスサイズ変更のUndo/Redo用ヘルパー(CanvasDocument::UndoKind::CanvasResize)
    QVector<LayerSnapshotData> captureAllLayerSnapshots(); // 現在のdoc_の全レイヤーをQImageで読み出す
    void rebuildCanvasFromSnapshots(int newW, int newH, const QVector<LayerSnapshotData> &snaps,
                                     int activeIndex, int offsetX = 0, int offsetY = 0);
    void applyCanvasResizeUndoEntry(const UndoEntry &entry, bool toBefore);
    // 選択範囲Undo(UndoKind::Selection)の適用。toBefore=trueならbeforeMask、falseならafterMaskへ戻す。
    void applySelectionUndoEntry(const UndoEntry &entry, bool toBefore);
    // 選択範囲マスクの読み出し/書き込み(いずれもqCompress済みのバイト列を扱う)
    QByteArray captureSelectionMask(); // 全域(生データ、非圧縮)
    void       restoreSelectionMaskRect(const QByteArray &compressed, int x, int y, int w, int h, bool hasSel);
    // beginSelectionUndo()で控えた「変更前」の状態
    QByteArray pendingSelectionUndoBefore_;
    bool       pendingSelectionUndoHadSel_ = false;
    bool       pendingSelectionUndoValid_  = false;
    // レイヤー追加(UndoKind::LayerAdd)の適用。追加を取り消す/やり直す。
    void applyLayerAddUndoEntry(const UndoEntry &entry, bool toBefore);

    // レイヤー削除(UndoKind::LayerRemove)。消えるレイヤーだけをタイルの生データごと
    // 保存し、Undoで挿入し直す(RemovedLayerDataのコメント参照)。
    RemovedLayerData captureRemovedLayer(int layerIndex);
    bool restoreRemovedLayer(const RemovedLayerData &d, int insertIndex,
                             const QVector<int> &ancestorFolders);
    void applyLayerRemoveUndoEntry(const UndoEntry &entry, bool toBefore);

    // レイヤー結合(UndoKind::LayerMerge)。変わるのはsurvivorのピクセルとvictimの消滅
    // だけなので、その2枚ぶんだけを保存する。undoOut=nullptrならUndo記録なし(Redo用)。
    int  mergeLayersInternal(int survivorIndex, int victimIndex,
                             const QVector<int> &ancestorFolders, LayerMergeUndoData *undoOut);
    void applyLayerMergeUndoEntry(const UndoEntry &entry, bool toBefore);
    // beginLayerAddUndo()〜commitLayerAddUndo()の間だけ >= 0(追加前のアクティブレイヤー)
    int  pendingLayerAddActiveBefore_ = -1;

    // ---- シェーダー -------------------------------------------------------
    QOpenGLShaderProgram *computeDrawProgram         = nullptr;
    QOpenGLShaderProgram *computeBakeProgram         = nullptr;
    // 下地混色で「筆に乗っている絵の具」をスタンプ列に沿って更新する小さなパス
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

    // 「アクティブレイヤーより下」の合成結果キャッシュ(item4: 描画中の毎フレーム
    // 全レイヤー再合成をやめるための仕組み)。PenEraserToolがストローク開始時に
    // 一度だけ updateBelowCompositeCache() で埋め、paintGL() は
    // belowCompositeCacheValid_==true の間、render.frag側でこのキャッシュを
    // result/clipBaseの初期値として使い、z=アクティブレイヤー以降だけを
    // 毎フレーム合成し直す(アクティブレイヤーが最上位に近いほど効果が大きい)。
    // ストローク中は自レイヤー以外のピクセルデータが変化しないことを前提にした
    // 最適化なので、ストローク終了(release)で必ず無効化する。
    GLuint belowCompositeTex          = 0; // RGBA8, canvasW x canvasH
    GLuint belowCompositeClipBaseTex  = 0; // RGBA8, canvasW x canvasH
    bool   belowCompositeCacheValid_  = false;
    // uptoExclusiveIndex未満(0..uptoExclusiveIndex-1)のレイヤーを合成してキャッシュへ書く。
    void updateBelowCompositeCache(int uptoExclusiveIndex);
    void invalidateBelowCompositeCache() { belowCompositeCacheValid_ = false; }
    // このキャッシュを作ったときの uptoExclusiveIndex。事前ウォームアップ
    // (prewarmCompositeCaches)が「今のアクティブレイヤー向けに作られているか」を
    // 判定するために覚えておく。
    int    belowCompositeCacheUpto_   = -1;

    // 「アクティブレイヤーより上」の事前合成キャッシュ(下キャッシュと対。ユーザー要望)。
    // アクティブより上のレイヤーが全て通常ブレンド・非クリッピング・非調整のときだけ
    // 有効化し、それらを1枚(aboveCompositeTex)へ事前合成する。render.frag は
    // aboveCompositeCacheValid_ の間、ループをアクティブで打ち切り最後にこれを1回重ねる。
    // これにより、アクティブより上にレイヤーが多数あってもストローク中の合成コストが
    // 一定になる(下のレイヤーに描くほど遅くなる問題の解消)。
    GLuint aboveCompositeTex          = 0; // RGBA8, canvasW x canvasH
    GLuint aboveCompositeClipScratch_ = 0; // belowComposite.compのclipBase出力用の捨てテクスチャ
    bool   aboveCompositeCacheValid_  = false;
    void updateAboveCompositeCache(int activeIndex);
    void invalidateAboveCompositeCache() { aboveCompositeCacheValid_ = false; }

    // ---- 事前合成キャッシュの先読み作成 -----------------------------------
    // 上下の事前合成キャッシュは、無効化されたあと最初のストローク開始時
    // (PenEraserTool::onMousePress → updateBelow/AboveCompositeCache)に作られる。
    // 中身はキャンバス全面 × レイヤー数ぶんのコンピュートディスパッチなので、
    // CPU側の発行は0.3msでも、GPUが実際に処理し終わるのを次のpresentが待つことに
    // なる(実測: 1280x1024・13レイヤーでpresentが106ms待たされた)。これが
    // 「ペンを置いてから最初の点が出たまま止まり、少し遅れて線が一気に追いつく」の
    // 正体だった。
    //
    // レイヤー切り替え等でキャッシュが無効になったあと、少し落ち着いたところで
    // 先に作っておけば、ストローク開始時はキャッシュヒットで済み、書き始めのコストが
    // ストローク中の1フレームと同じになる。
    QTimer *compositeCachePrewarmTimer_ = nullptr;
    // この無効化世代に対して既に先読み作成を試したか(updateAboveCompositeCache は
    // 「上に非通常ブレンドがある」等の条件でvalidにせず戻ることがあるため、
    // validフラグだけを見ると毎回作り直しに来てしまう)。
    bool    compositeCachePrewarmDone_  = false;
    void scheduleCompositeCachePrewarm();
    void prewarmCompositeCaches();

    // ---- フィルターレイヤーの連鎖 -----------------------------------------
    // フィルターレイヤー(LayerType::Filter)は近傍参照なので、render.frag の
    // 1フラグメント1パスの合成ループの中では計算できない(1タップごとに下の
    // 全レイヤー合成をやり直すことになる)。そこでレイヤースタックをフィルター
    // レイヤーの位置で区切り、「区間を合成(belowComposite.comp)→ フィルターを
    // かける」を交互に繰り返してオフスクリーンで作っておき、render.frag は
    // その続き(=一番上のフィルターレイヤーの1つ上)から合成する。
    // 結果とclipBaseをそれぞれping-pongさせるため4枚使うが、フィルターレイヤーを
    // 含む文書でだけ遅延確保する(キャンバス全面のRGBA8は1枚でも大きいため)。
    GLuint filterChainResult_[2] = {0, 0};
    GLuint filterChainClip_[2]   = {0, 0};
    // 多段パスのフィルターレイヤー(ぼかし)用の中間バッファ。フィルターレイヤーの
    // 破壊的フィルター版が使う filterScratchTex_ (CanvasWidget.h参照)とはあえて共用
    // しない ―― 「フィルターレイヤーより下に描いている間、毎フレーム連鎖を
    // 組み直す」経路と「破壊的フィルターのプレビュー更新」が同一フレーム内で
    // 両方走りうるため、共用するとサイズが食い違うたびに(層のサイズ≠キャンバス
    // サイズの場合)テクスチャを毎回作り直す無駄が生じる。専用に1枚持てば
    // その心配がない。
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
    // 非表示・不透明度0・非表示フォルダーの中にあるものは「効いていない」扱い。
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
    // 実際にかけた場合のみ true(無料版ビルドや未知の種類では false)。
    bool applyFilterLayer(int z, int srcIdx);
    // render.frag に渡すべき (startZ, 事前合成テクスチャ) を決める。
    // 戻り値 <0 なら事前合成なし(従来通り z=0 から全レイヤーを合成する)。
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

    // ---- ブラシ状態 -------------------------------------------------------
    ToolType      activeTool = ToolType::Pen;

    float brushPressure = 1.0f;

    // スライス管理 (CanvasDocument のコールバックとして渡す。実体は LayerSliceAllocator)
    LayerSliceAllocator sliceAllocator_;

    // ---- 内部ヘルパー -----------------------------------------------------
    void initTextures(bool createDefaultLayers = true);
    void updateCompositedTex(); // ナビゲーター用、重いので頻繁に呼ばない
    QColor getPixelColor(const QPointF &widgetPos, bool referenceCanvas = false);

    // ---- ビュー変換の座標系について -----------------------------------------
    // ViewTransform の「画面座標」はデバイスピクセルで扱う。こうすると拡大率1.0で
    // 「1キャンバスpx = 1デバイスpx」になり、等倍表示が実画素どおり(=書き出した
    // 画像をビューアで等倍表示したときと同じ)になる。
    //
    // 以前は width()/height()(論理px)を渡していたため、DPRが1でない環境では
    // 拡大率1.0でも実際には DPR 倍に拡大リサンプルされており、輪郭が不均一に
    // なって「ドットが目立つ・アンチエイリアスが効いていない」ように見えていた
    // (実測: FBO=1128x998 に対し uWindowSize=902x798、DPR=1.25)。
    //
    // Qtのウィジェット座標(マウス位置など)は論理pxなので、ビューへ渡す前に
    // viewDpr() を掛け、ビューから受け取った値は割ること。
    float viewDpr()    const { return float(devicePixelRatioF()); }
    float viewWidth()  const { return float(width())  * viewDpr(); }
    float viewHeight() const { return float(height()) * viewDpr(); }

    // ウィジェット座標(論理px) → キャンバスピクセル座標
    QVector2D widgetToPixel(const QPointF &pos) const {
        const float d = viewDpr();
        return view_.widgetToCanvas(QPointF(pos.x() * d, pos.y() * d), qRound(viewHeight()));
    }

    GLuint makeTexture2D(GLenum internalFormat, int w, int h,
                         GLenum filter = GL_NEAREST);

    // Undo 用スナップショット(実体は StrokeUndoRecorder。ここは薄い委譲メソッドのみ)
    StrokeUndoRecorder undoRecorder_;
    CanvasCompositor   compositor_;
    void      restoreLayer(const UndoEntry &entry);

    UndoEntry captureAllTiles(int layerIndex);
    // captureAllTilesの部分版。shapeで渡されたTileUndoたちの(tx,ty)位置だけをキャプチャする
    // (CanvasWidget::undo()/redo()が、これから復元するエントリが実際に触れるタイルだけを
    // "current"としてキャプチャするために使う)。
    UndoEntry captureTilesLike(int layerIndex, const QVector<TileUndo> &shape);
    void beginStrokeUndo();
    void expandStrokeUndoRegion(int txMin, int txMax, int tyMin, int tyMax);
    void commitStrokeUndo();
    void setLayerUniformsForRender(QOpenGLShaderProgram *prog);
    // 選択範囲マスクの境界に沿ってマーチングアンツ(白黒交互の点線)を描く。
    // paintGL()末尾のオーバーレイQPainterから呼ぶ。
    void paintSelectionOutline(QPainter &painter);

    void pushUndoSnapshot();

    void freeTextures(); // テクスチャのメモリ解放ヘルパー
};
