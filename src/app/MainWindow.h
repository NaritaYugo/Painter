#pragma once

#include "document/CanvasSerializer.h"
#include "io/PsdCodec.h"
#include "tools/core/ToolConfig.h"
#include "dialogs/SettingsDialog.h"
#include "dialogs/NewCanvasDialog.h"
#include "shortcuts/ShortcutRegistry.h"
#include "canvas/CanvasWidget.h"
#include "components/ThemeColors.h"

#include <QMainWindow>
#include <QDockWidget>
#include <QSettings>
#include <QMap>
#include <QHash>
#include <QVector>
#include <QElapsedTimer>
#include <QMetaObject>
#include <memory>

class CanvasWidget;
class LayerDock;
class ColorCircleDock;
class ToolDock;
class ToolPropertyDock;
class ToolPresetDock;
class BrushSizeDock;
class NavigatorDock;
class StartPage;
class QTabWidget;
class QStackedWidget;
class QTabBar;
class QSplitter;
class CalibrationDialog;
class QTimer;
class CanvasPane;
class CanvasTabPage;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // 外部から渡されたファイルを新しいタブで開く。
    void openFilesFromArgs(const QStringList &paths);

    // Windows固有処理 (MainWindowNative.cpp)
    bool handleNativeWindowMessage(unsigned message, quintptr wParam, qintptr lParam,
                                    void *hwndOpaque, qintptr *result);
    // クライアント座標(論理px)が「タイトルバー相当」として扱ってよい場所か。
    bool isWindowDragArea(const QPoint &clientPos) const;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    // QEvent::WinIdChange(ネイティブウィンドウの作り直し)を捕まえるためだけの override。
    bool event(QEvent *event) override;

private:
    // タブ1枚 = CanvasTabPage 1枚。
    QStackedWidget  *centralStack        = nullptr;
    // 中央のキャンバス領域(分割表示対応)。
    QWidget         *paneTreeRoot_       = nullptr;
    CanvasPane      *activePane_         = nullptr; // 操作権を持つペイン
    // CanvasTabPage -> それを保持しているCanvasPaneの逆引き表。
    QHash<CanvasTabPage *, CanvasPane *> tabOwnerPane_;
    // 現在アクティブなタブのCanvasWidget。
    CanvasWidget        *glWidget           = nullptr;
    // currentChangedからの再入を防ぐ。
    bool             activatingTab_      = false;
    // キャンバス未選択時にDockへ渡すダミー。
    CanvasWidget        *deckCanvasWidget_       = nullptr;
    ToolConfig      *toolCfg            = nullptr;
    // glWidget(=現在のタブ)に対して貼っている接続。
    QVector<QMetaObject::Connection> glWidgetConnections_;
    // 設定ダイアログで変更した手ブレ補正の強さ。
    float pendingSmoothing_ = 0.5f;

    ColorCircleDock *colorCircleDock    = nullptr;
    ToolDock        *toolDock           = nullptr;
    ToolPropertyDock    *toolPropertyDock       = nullptr;
    ToolPresetDock     *toolPresetDock        = nullptr;
    LayerDock       *layerDock          = nullptr;
    BrushSizeDock   *brushSizeDock      = nullptr;
    NavigatorDock   *navigatorDock      = nullptr;

    QDockWidget *toolDockWidget         = nullptr;
    QDockWidget *toolPropertyDockWidget     = nullptr;
    QDockWidget *toolPresetDockWidget      = nullptr;
    QDockWidget *colorCircleDockWidget  = nullptr;
    QDockWidget *layerDockWidget        = nullptr;
    QDockWidget *brushSizeDockWidget    = nullptr;
    QDockWidget *navigatorDockWidget    = nullptr;

    QToolBar *mainToolBar = nullptr;

    QAction *cutAction = nullptr;
    QAction *exportAction = nullptr;
    QAction *addImageLayerAction = nullptr;
    QAction *saveAction   = nullptr;
    QAction *saveAsAction = nullptr;
    QAction *loadAction   = nullptr;
    QAction *helpAction   = nullptr;
    QAction *versionAction   = nullptr;
    QAction *settingsAction = nullptr;
    QAction *shortcutsAction = nullptr;
    QAction *newCanvasAction = nullptr;
    QAction *newTabAction = nullptr; // 「タブを追加」(スタートページのタブを1枚増やす)
    QAction *fitAction = nullptr;
    QAction *zoomInAction = nullptr;
    QAction *zoomOutAction = nullptr;
    QAction *viewFlipXAction = nullptr;
    QAction *nudgeUpAction = nullptr;
    QAction *nudgeLeftAction = nullptr;
    QAction *nudgeDownAction = nullptr;
    QAction *nudgeRightAction = nullptr;
    QAction *resetLayoutAction = nullptr;
    QAction *undoAction = nullptr;
    QAction *redoAction = nullptr;
    QAction *copyAction = nullptr;
    QAction *pasteAction = nullptr;
    QAction *selectAllAction = nullptr;
    QAction *clearSelectionAction = nullptr;
    QAction *transformAction = nullptr;
    QAction *confirmTransformAction = nullptr;
    QAction *freeTransformAction = nullptr;
    QAction *hueSatLightAction = nullptr;
    QAction *brightnessContrastAction = nullptr;
    QAction *colorBalanceAction = nullptr;
    QAction *toneCurveAction = nullptr;
    QAction *canvasSizeAction = nullptr;
    QAction *imageResolutionAction = nullptr;
    QAction *rotateCanvas90Action = nullptr;
    QAction *rotateCanvas180Action = nullptr;
    QAction *rotateCanvas270Action = nullptr;
    QAction *flipCanvasHorizontalAction = nullptr;
    QAction *flipCanvasVerticalAction = nullptr;
    QAction *gaussianBlurFilterAction = nullptr;
    QAction *customShaderFilterAction = nullptr;
    QAction *mosaicFilterAction = nullptr;
    QAction *motionBlurFilterAction = nullptr;
    QAction *noiseFilterAction = nullptr;
    QAction *chromaticAberrationFilterAction = nullptr; // Pro版限定(無料版では常にダイアログを出すだけ)
    QAction *lensBlurFilterAction = nullptr;            // Pro版限定(無料版では常にダイアログを出すだけ)
    QAction *gradientMapAction = nullptr;               // Pro版限定(無料版では常にダイアログを出すだけ)
    QAction *brushBiggerAction = nullptr;
    QAction *brushSmallerAction = nullptr;
    QAction *dumpStateAction = nullptr;

    // レイヤーメニュー(操作の実体はLayerDock側のメンバ関数を直接呼ぶ)。
    QAction *newLayerAction = nullptr;
    QAction *newClippingLayerAction = nullptr;
    QAction *newSolidColorLayerAction = nullptr;
    QAction *newAdjustmentLayerAction = nullptr;
    QAction *newTextLayerAction = nullptr;
    QAction *duplicateLayerAction = nullptr;
    QAction *mergeLayerAction = nullptr;
    QAction *deleteLayerMenuAction = nullptr;

    // カラーモード(表示上の見た目だけを変えるプレビュー機能。実データはRGBAのまま)。
    QAction *colorModeRgbAction            = nullptr;
    QAction *colorModeCmykAction           = nullptr;
    QAction *colorModeGrayLuminanceAction  = nullptr;
    QAction *colorModeGrayLightnessAction  = nullptr;

    // モニターキャリブレーション(明るさ・コントラスト・CMY)。
    QAction *calibrationAction = nullptr;
    CalibrationDialog *calibrationDialog = nullptr;

    // UIテーマ(ライト/ダーク)。
    QAction *themeDarkAction  = nullptr;
    QAction *themeLightAction = nullptr;

    // キャンバス外側の背景色(現在は設定メニューから変更、実データには影響しない)。
    QAction *canvasBgColorAction = nullptr;

    // デバッグ用: 保存済みの全設定(QSettings)をリセットして終了する。
    QAction *resetAllSettingsAction = nullptr;
    void resetAllSettingsAndQuit();
    bool m_resettingSettings_ = false; // closeEvent()でのsaveSettings()を抑止するためのフラグ

    // ショートカットキー
    ShortcutRegistry shortcuts_;
    QMap<int, QElapsedTimer> m_keyPressTimers; // キー → 押した時刻(ms)
    static constexpr int HOLD_THRESHOLD_MS = 300; // 長押し判定の閾値

    void applyToolShortcut(int key);    // 押下時
    void releaseToolShortcut(int key);  // 離した時

    // 長押し管理。
    int m_heldShortcutKey = 0; // 現在長押し中のキー（0=なし）
    // 長押し開始時点でアクティブだったツール。
    ToolType m_toolBeforeHold = ToolType::Pen;
    // プリセットのショートカットは対象ツールのアクティブプリセットも書き換えるため、離したときにツールだけ戻しても選ばれているプリセットが変わったままになる。
    ToolType m_presetHoldTool   = ToolType::Pen;
    int      m_presetBeforeHold = -1;

    void setupActions(); // MainWindowShortcuts.cpp で定義(QAction生成+ショートカット登録)
    void createMenus();
    void connectSignals();
    void createWidgets();

    // ドックを重ねてタブ化したときにQtが内部生成する切り替え用QTabBarは、DockTitleBarとは別の標準スタイルで(かつ下寄せで)出てくる。
    void styleNewDockTabBars();
    // タブ化中は、切り替えタブ(上)と各ドック自前のDockTitleBar(下)が二重に出てしまうため、タブ化されている間だけ各ドックのDockTitleBarを高さ0のダミーに差し替えて隠す(タブ解除時は元に戻す)。
    void updateTabifiedTitleBars();
    QMap<QDockWidget *, QWidget *> hiddenDockTitleBars_;

    // ドックの切り替えタブ(dockGroupTabBar)自体をドラッグしてタブ化グループから外す(フロート化する)ための状態。
    QTabBar     *dragTabBar_       = nullptr;
    int          dragTabIndex_     = -1;
    QPoint       dragStartPos_;
    bool         dragDetached_     = false;
    QDockWidget *dragDock_         = nullptr;
    QPoint       dragCursorOffset_;
    QString getUniqueFilePath(const QString &basePath, int fillDigit);

    // フレームレスウィンドウ
    QAction     *alwaysOnTopAction_ = nullptr;
    QToolButton *maximizeBtn_       = nullptr;
    void setupCaptionButtons();   // メニューバー右端へ最小化/最大化/閉じるを置く
    void updateMaximizeButton();  // 最大化⇔元に戻すで見た目を切り替える
    // 常に最前面に固定する(穴越しに後ろのウィンドウを操作しても隠れないようにする)。
    void setAlwaysOnTop(bool on);
    // アプリ全体のネイティブイベントフィルタを張る(MainWindowNative.cpp)。
    void installNativeWindowFilter();
    // 「応答なし」の身代わりウィンドウ(ゴースト)を作らせない。
    void disableWindowGhosting();
    // OS側の「最大化しているか」。
    bool osIsMaximized() const;
    // ネイティブウィンドウが作り直された後に、フレーム計算(WM_NCCALCSIZE)とウィンドウリージョン(透過タブの穴)を貼り直す。
    void refreshNativeFrame();
    // メニューバーの空き部分を掴んだときに、OS標準のキャプションドラッグを始める。
    void beginNativeWindowDrag();
    // ウィンドウ全体を子ウィジェットごと塗り直す(理由はMainWindowNative.cpp参照)。
    void repaintNativeWindow();
    // 角を丸めるかどうかをDWMへ明示指定する(最大化中は丸めない)。
    void updateWindowCornerStyle();
    // 直前に設定した角の指定(-1=未設定)。
    int lastCornerPref_ = -1;
    // 次のイベントループで1回だけ塗り直す(連続要求はまとめる)。
    void scheduleNativeRepaint();
    // 診断用: ウィンドウの合成まわりの状態(拡張スタイル・DWM・リージョン・実際のクライアント原点)を1行にまとめてログへ出す。
    void logWindowCompositionState(const QString &tag);
    // 診断用: ウィンドウと各ドックの実サイズをログへ出す(実装はMainWindow.cpp)。
    void logDockLayout(const QString &tag);
    // loadSettings()で復元したドック配置のバイト列。
    QByteArray savedDockState_;
    void scheduleDockLayoutRestore();
    // 最初のキャンバスを作るとウィンドウが作り直され、その拍子にドック配置が崩れることがある。
    bool firstCanvasCreated_ = false;
    void restoreDockLayoutAfterFirstCanvas(const QByteArray &before, int attempt);
    bool dockRestorePending_    = false; // 貼り直しがまだ済んでいないか(起動時の1回だけ)
    int  dockRestoreGeneration_ = 0;     // デバウンス用の世代番号
    // 全画面状態を最大化へ落とす処理の再入防止(setWindowState()が同期的に次のWindowStateChangeを飛ばすため。MainWindow::event()のコメント参照)。
    bool droppingFullScreen_ = false;
    bool nativeRepaintScheduled_ = false;
    // 表示前にrefreshNativeFrame()が呼ばれた(=フレームの再計算を先送りした)か。
    bool pendingFrameRefresh_ = false;
    // 複数タブ・分割表示
    CanvasPane *createPane();
    // 新しいタブを1枚(スタートページ状態で)生成してtargetPaneに追加する。
    CanvasTabPage *createTabPage(CanvasPane *targetPane);
    // pageにCanvasWidgetがまだ無ければ生成して載せる(=スタートページからキャンバスへ切替)。
    CanvasWidget *ensureCanvas(CanvasTabPage *page);
    // 新しいドキュメントを開く先のタブを返す。
    CanvasTabPage *targetTabPageForNewDocument();
    // 「タブを追加」: スタートページのタブを1枚増やしてアクティブにする。
    void addNewTab();
    // glWidgetメンバを指定のCanvasWidgetに切り替え、そのCanvasWidgetの各シグナルをMainWindow/各Dockへ接続し(古い接続はglWidgetConnections_経由で解除済みである前提)、
    // 各Dockのretarget(setCanvasWidget)とrefresh()を行う。
    void bindCanvasWidget(CanvasWidget *gl);
    // 現在のCanvasWidgetへの接続を切ってglWidgetをnullptrに戻し、各Dockをダミー(deckCanvasWidget_)へretargetする。
    void unbindCanvasWidget();
    // 操作権をpageへ移す単一の入口。
    void activateTabPage(CanvasTabPage *page);
    // paneの現在のタブへ操作権を移す(クリックされたペインをアクティブにする用)。
    void activatePane(CanvasPane *pane);
    // paneの端(orientation/insertBeforeで方向を指定)へタブがドロップされたときの処理。
    void splitPane(CanvasPane *pane, Qt::Orientation orientation, bool insertBefore,
                    QTabWidget *sourceTabs, int sourceIndex);
    // destPaneの中央/タブバーへタブがドロップされたときの処理(分割はしない)。
    void movePaneTab(CanvasPane *destPane, int destIndex, QTabWidget *sourceTabs, int sourceIndex);
    // paneのタブが0枚になったら、親から取り除いて削除する。
    void closePaneIfEmpty(CanvasPane *pane);
    // paneTreeRoot_(CanvasPane*またはQSplitter*)を再帰的に辿り、全ペインの全タブのうちキャンバスになっているものだけを集めて返す(スタートページのタブは含まない)。
    QVector<CanvasWidget *> allCanvasWidgets() const;
    // 新規タブの追加先ペインを返す。
    CanvasPane *ensureRootPane();
    // 未保存の変更があるかを確認し、破棄してよいかをユーザーに尋ねる。
    bool confirmDiscardChanges(CanvasWidget *gl, const QString &message, const QString &yesLabel);
    // 該当タブのタブ見出しを更新する(キャンバスならファイル名+未保存マーク、まだスタートページなら「スタートページ」)。
    void updateTabLabel(CanvasTabPage *page);
    void updateTabLabel(CanvasWidget *gl); // glの所属タブを引いて上のオーバーロードへ委譲
    // 新規キャンバス作成時、未保存の間タブ見出しに表示する仮の名前("Canvas001"等)を採番する。
    QString generateUniqueCanvasName() const;
    // pageをキャンバスに変え、そのCanvasWidget::initialized完了後にpathを読み込む。
    void openFileIntoTab(CanvasTabPage *page, const QString &path);
    // ファイルダイアログでパスを選んでからopenFileIntoTab()する。
    void loadFileIntoTab(CanvasTabPage *page);
    // 新規キャンバスダイアログを出してからpageをキャンバスに変える。
    void newCanvasInTab(CanvasTabPage *page);
    // 追加先タブを自動で選ぶ版(コマンドライン引数・ドラッグ&ドロップ用)。
    void openFileIntoNewTab(const QString &path);
    // centralStackの表示ページをpaneTreeRoot_に合わせ、キャンバス依存アクションの有効/無効を揃える。
    void updateCentralPage();
    // 「透過タブ」が表示されているぶんだけ、ウィンドウ自体に穴を開ける。
    void updateWindowMask();
    // holesのぶんだけ穴の空いたウィンドウリージョンを実際に適用する(実装はMainWindowNative.cpp)。
    void applyWindowRegion(const QVector<QRect> &holes);
    // 直前に適用した穴。
    QVector<QRect> lastAppliedHoles_;
    // 直前に適用したときのウィンドウ矩形(スクリーン座標・物理px)。
    QRect lastAppliedWindowRect_;
    void updateTabRelatedActionsEnabled();

    // カラーモードとモニターキャリブレーション
    void setColorMode(ColorMode m);
    void openCalibrationDialog();
    void setCalibration(int brightness, int contrast, int cyan, int magenta, int yellow);
    void propagateDisplayConfigToAllTabs();

    // UIテーマ
    void setUiTheme(Theme::Name n);

    // キャンバス外側の背景色
    void chooseCanvasBackgroundColor();
    void setCanvasBackgroundColor(const QColor &c);

private slots:
    void exportImage();
    // Photoshopのブラシファイル(.abr)とペン設定のやり取り(io/AbrCodec.h参照)。
    void importPenSettingsFromAbr();
    void exportPenSettingsToAbr();
    void addImageLayer(); // 「画像を追加」: 画像ファイルを選び、現在選択中のレイヤーの上に挿入する
    void saveFile();
    void saveFileAs();
    void loadFile();
    void showVersionDialog();
    void updateWindowTitle();
    void openSettingsDialog();
    void openNewCanvasDialog();
    void saveSettings();
    void loadSettings();
    void resetDockLayout();
    void performAutoSave();

private:
    // CanvasPane::currentTabChanged / tabCloseRequestedへ、createPane()内でペインごとに接続する(単一tabWidgetのスロットだった頃と違い、
    // どのペインの出来事かをpane引数で受け取る必要があるため、シグナルと直接同じ引数列にしてある)。
    void onCurrentTabChanged(CanvasPane *pane, int index);
    void onTabCloseRequested(CanvasPane *pane, int index);
    // 設定ダイアログの自動保存設定(有効/間隔)を読み込み、タイマーへ反映する(起動時のloadSettings()後、および設定ダイアログでOKされた直後に呼ぶ)。
    void applyAutoSaveSettings();
    // 環境設定の「全体の筆圧カーブ」をToolConfigへ反映する(起動時/設定OK時)。
    void applyGlobalPressureCurve();
    QTimer *autoSaveTimer_ = nullptr;
};
