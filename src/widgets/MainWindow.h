#pragma once

#include "backend/CanvasSerializer.h"
#include "backend/PsdCodec.h"
#include "tools/core/ToolConfig.h"
#include "dialogs/SettingsDlg.h"
#include "dialogs/NewCanvasDlg.h"
#include "shortcuts/ShortcutRegistry.h"
#include "widgets/GLWidget.h"
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

class GLWidget;
class LayerDock;
class ColorCircleDock;
class ToolDock;
class ToolPropDock;
class ToolPresetDock;
class BrushSizeDock;
class NavigatorDock;
class StartPage;
class QTabWidget;
class QStackedWidget;
class QTabBar;
class QSplitter;
class CalibrationDlg;
class QTimer;
class CanvasPane;
class CanvasTabPage;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // コマンドライン引数やOSの「プログラムから開く」/ドラッグ&ドロップで渡された
    // ファイルを新しいタブで開く(存在するファイルだけ。openFileIntoNewTabへの薄い委譲)。
    void openFilesFromArgs(const QStringList &paths);

    // ---- Windows固有処理の内部プラミング(実装はMainWindowNative.cpp) ------------
    // アプリ全体のネイティブイベントフィルタから呼ぶため公開しているが、外部から
    // 使うことは想定していない。hwndはHWND(ヘッダにwindows.hを持ち込まないためvoid*)。
    bool handleNativeWindowMessage(unsigned message, quintptr wParam, qintptr lParam,
                                    void *hwndOpaque, qintptr *result);
    // クライアント座標(論理px)が「タイトルバー相当」として扱ってよい場所か。
    // WM_NCHITTESTでHTCAPTIONを返すかどうかの判定に使う。移動・スナップ・
    // ダブルクリック最大化・システムメニューはすべてOSがHTCAPTIONを基準に駆動する
    // ため、アプリ側でドラッグ処理を書く必要はない。
    bool isWindowDragArea(const QPoint &clientPos) const;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    // QEvent::WinIdChange(ネイティブウィンドウの作り直し)を捕まえるためだけの override。
    bool event(QEvent *event) override;

private:
    // タブ1枚 = CanvasTabPage 1枚。CanvasTabPageはスタートページとキャンバス(GLWidget)を
    // 内部に持ち、どちらか一方を表示する。タブは必ずスタートページとして生まれ、そこで
    // 作品を選ぶ/新規作成するとキャンバスへ切り替わる。CanvasDocumentはGLWidget自身が
    // 所有するため、MainWindow側で並行のドキュメント管理構造体を持つ必要はない。
    // centralStackはペインツリーの差し替え(分割・畳み)を扱うためだけの1ページ構成。
    QStackedWidget  *centralStack        = nullptr;
    // 中央のキャンバス領域(分割表示対応)。ペインが1枚だけならCanvasPane*そのものを、
    // 分割されるとQSplitter*(子はCanvasPane*または入れ子のQSplitter*)を指す。
    // 起動直後にスタートページのタブを1枚作るため、以後は常に非nullptr。
    QWidget         *paneTreeRoot_       = nullptr;
    CanvasPane      *activePane_         = nullptr; // 操作権を持つペイン
    // CanvasTabPage -> それを保持しているCanvasPaneの逆引き表。クリックのたびにペイン
    // ツリーを辿らず即座に所属ペインを引けるようにするため(activateTabPage()参照)。
    QHash<CanvasTabPage *, CanvasPane *> tabOwnerPane_;
    // 現在アクティブなタブのGLWidget。アクティブなタブがまだスタートページの間はnullptr。
    GLWidget        *glWidget           = nullptr;
    // activateTabPage()内のsetCurrentWidget()がcurrentChanged経由で同じ関数へ同期的に
    // 再入するため、再入側を素通りさせて外側に処理を任せるためのフラグ。
    bool             activatingTab_      = false;
    // Dock類(ToolDock等)のコンストラクタはGLWidget*を要求するため、タブが1枚も
    // 無い起動直後でも渡せるダミーとして使う。どのペインのタブにも追加せず、表示も
    // しない(initializeGL()も走らないため、GLリソースのコストは掛からない)。
    GLWidget        *deckGLWidget_       = nullptr;
    ToolConfig      *toolCfg            = nullptr;
    // glWidget(=現在のタブ)に対して貼っている接続。タブ切替時にまとめて解除し、
    // 新しいタブのGLWidgetに対して張り直す(bindGLWidget()参照)。
    QVector<QMetaObject::Connection> glWidgetConnections_;
    // 設定ダイアログで変更した手ブレ補正の強さ。glWidgetが無い間や、新しいタブを
    // 作った直後にも適用できるよう、値そのものをここに保持しておく。
    float pendingSmoothing_ = 0.5f;

    ColorCircleDock *colorCircleDock    = nullptr;
    ToolDock        *toolDock           = nullptr;
    ToolPropDock    *toolPropDock       = nullptr;
    ToolPresetDock     *toolPresetDock        = nullptr;
    LayerDock       *layerDock          = nullptr;
    BrushSizeDock   *brushSizeDock      = nullptr;
    NavigatorDock   *navigatorDock      = nullptr;

    QDockWidget *toolDockWidget         = nullptr;
    QDockWidget *toolPropDockWidget     = nullptr;
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

    // レイヤーメニュー(操作の実体はLayerDock側のメンバ関数を直接呼ぶ)
    QAction *newLayerAction = nullptr;
    QAction *newClippingLayerAction = nullptr;
    QAction *newSolidColorLayerAction = nullptr;
    QAction *newAdjustmentLayerAction = nullptr;
    QAction *newTextLayerAction = nullptr;
    QAction *duplicateLayerAction = nullptr;
    QAction *mergeLayerAction = nullptr;
    QAction *deleteLayerMenuAction = nullptr;

    // カラーモード(表示上の見た目だけを変えるプレビュー機能。実データはRGBAのまま)
    QAction *colorModeRgbAction            = nullptr;
    QAction *colorModeCmykAction           = nullptr;
    QAction *colorModeGrayLuminanceAction  = nullptr;
    QAction *colorModeGrayLightnessAction  = nullptr;

    // モニターキャリブレーション(明るさ・コントラスト・CMY)。非モーダルダイアログを
    // 遅延生成し、以後は使い回す(閉じても破棄しない)。
    QAction *calibrationAction = nullptr;
    CalibrationDlg *calibrationDlg = nullptr;

    // UIテーマ(ライト/ダーク)
    QAction *themeDarkAction  = nullptr;
    QAction *themeLightAction = nullptr;

    // キャンバス外側の背景色(現在は設定メニューから変更、実データには影響しない)
    QAction *canvasBgColorAction = nullptr;

    // デバッグ用: 保存済みの全設定(QSettings)をリセットして終了する
    QAction *resetAllSettingsAction = nullptr;
    void resetAllSettingsAndQuit();
    bool m_resettingSettings_ = false; // closeEvent()でのsaveSettings()を抑止するためのフラグ

    // ショートカットキー
    // 個々のアクションの生成・キー割り当て・保存/読込・ダイアログへの表示は
    // すべてShortcutRegistryに集約されている。新しいショートカットを
    // 増やしたいときは MainWindowShortcuts.cpp の setupActions() に
    // 1ブロック足すだけでよい(このファイル・ヘッダは触らなくてよい)。
    ShortcutRegistry shortcuts_;
    QMap<int, QElapsedTimer> m_keyPressTimers; // キー → 押した時刻(ms)
    static constexpr int HOLD_THRESHOLD_MS = 300; // 長押し判定の閾値

    void applyToolShortcut(int key);    // 押下時
    void releaseToolShortcut(int key);  // 離した時

    // 長押し管理
    int m_heldShortcutKey = 0; // 現在長押し中のキー（0=なし）
    // 長押し開始時点でアクティブだったツール。GLWidget::previousTool(スポイトの
    // 自動復帰など、他の「ひとつ前のツールに戻る」機能とも共有される単一スロット)を
    // 使うと、長押し中にそれらが割り込んで上書きしてしまうため、長押し機能専用に
    // 別で保持する(離したときはこちらへ直接setActiveTool()で戻す)。
    ToolType m_toolBeforeHold = ToolType::Pen;
    // プリセットのショートカットは対象ツールのアクティブプリセットも書き換えるため、
    // 離したときにツールだけ戻しても選ばれているプリセットが変わったままになる。
    // 書き換えた対象ツールと、その変更前のプリセット番号も控えて両方戻す。
    // (m_presetHoldTool は m_presetBeforeHold >= 0 のときだけ意味を持つ)
    ToolType m_presetHoldTool   = ToolType::Pen;
    int      m_presetBeforeHold = -1;

    void setupActions(); // MainWindowShortcuts.cpp で定義(QAction生成+ショートカット登録)
    void createMenus();
    void connectSignals();
    void createWidgets();

    // ドックを重ねてタブ化したときにQtが内部生成する切り替え用QTabBarは、
    // DockTitleBarとは別の標準スタイルで(かつ下寄せで)出てくる。生成タイミングを
    // 捕まえるフックが無いため、タイマーで定期的に探して見た目を揃え直す。
    void styleNewDockTabBars();
    // タブ化中は、切り替えタブ(上)と各ドック自前のDockTitleBar(下)が二重に
    // 出てしまうため、タブ化されている間だけ各ドックのDockTitleBarを高さ0の
    // ダミーに差し替えて隠す(タブ解除時は元に戻す)。ドラッグでの取り外しは
    // 下記のタブバー自体のドラッグ処理で行うため、このDockTitleBarはもう
    // ドラッグ用の掴み手にはならない。
    void updateTabifiedTitleBars();
    QMap<QDockWidget *, QWidget *> hiddenDockTitleBars_;

    // ドックの切り替えタブ(dockGroupTabBar)自体をドラッグしてタブ化グループから
    // 外す(フロート化する)ための状態。Qt標準の「タブをドラッグして外す」機構は
    // タブ化グループの先頭(=元々そこにあったドック)には効かない不具合がある
    // ため、全タブに対して均一に効くよう自前でmousePress/Move/Releaseを処理する。
    QTabBar     *dragTabBar_       = nullptr;
    int          dragTabIndex_     = -1;
    QPoint       dragStartPos_;
    bool         dragDetached_     = false;
    QDockWidget *dragDock_         = nullptr;
    QPoint       dragCursorOffset_;
    QString getUniqueFilePath(const QString &basePath, int fillDigit);

    // ---- フレームレスウィンドウ ------------------------------------------------
    // OS標準のタイトルバーは出さず(Qt::FramelessWindowHint)、最小化/最大化/閉じるは
    // メニューバー右端のコーナーウィジェットに置く。透過タブでウィンドウに穴を開ける
    // 都合上もこの方が都合がよい(Qtはマスクをクライアント領域基準で扱い、フレーム分を
    // オフセットして SetWindowRgn するため、フレームがあるとタイトルバーが領域の外に
    // 出て切り落とされてしまう)。
    QAction     *alwaysOnTopAction_ = nullptr;
    QToolButton *maximizeBtn_       = nullptr;
    void setupCaptionButtons();   // メニューバー右端へ最小化/最大化/閉じるを置く
    void updateMaximizeButton();  // 最大化⇔元に戻すで見た目を切り替える
    // 常に最前面に固定する(穴越しに後ろのウィンドウを操作しても隠れないようにする)。
    void setAlwaysOnTop(bool on);
    // アプリ全体のネイティブイベントフィルタを張る(MainWindowNative.cpp)。
    // QWidget::nativeEvent()はメッセージによってはQt内部の処理が先に走って届かない
    // ことがあるため、確実に横取りできるこちらを主経路にする。
    void installNativeWindowFilter();
    // 「応答なし」の身代わりウィンドウ(ゴースト)を作らせない。ウィンドウを作る前に
    // 一度だけ呼ぶ。詳細はMainWindowNative.cppの実装のコメント。
    void disableWindowGhosting();
    // OS側の「最大化しているか」。QtのisMaximized()/isFullScreen()は過渡的な値や
    // 推測が混じるので、確定した答えが要るところではこちらを使う
    // (updateWindowCornerStyle()のコメント参照)。
    bool osIsMaximized() const;
    // ネイティブウィンドウが作り直された後に、フレーム計算(WM_NCCALCSIZE)と
    // ウィンドウリージョン(透過タブの穴)を貼り直す。理由はMainWindowNative.cpp参照。
    void refreshNativeFrame();
    // メニューバーの空き部分を掴んだときに、OS標準のキャプションドラッグを始める。
    void beginNativeWindowDrag();
    // ウィンドウ全体を子ウィジェットごと塗り直す(理由はMainWindowNative.cpp参照)。
    void repaintNativeWindow();
    // 角を丸めるかどうかをDWMへ明示指定する(最大化中は丸めない)。
    void updateWindowCornerStyle();
    // 直前に設定した角の指定(-1=未設定)。同じ値で呼び直すとDWMがフレームを
    // 描き直して点滅するため、変わったときだけ設定するのに使う。
    int lastCornerPref_ = -1;
    // 次のイベントループで1回だけ塗り直す(連続要求はまとめる)。レイアウト変更や
    // GLの初期化が落ち着いてから塗りたい場面で使う。
    void scheduleNativeRepaint();
    // 診断用: ウィンドウの合成まわりの状態(拡張スタイル・DWM・リージョン・実際の
    // クライアント原点)を1行にまとめてログへ出す。ちらつきがウィンドウメッセージの
    // 層に現れないことが実測で分かったため、合成側を見るために足した
    // (実装はMainWindowNative.cpp、TIEPOLO_WINLOG未設定なら何もしない)。
    void logWindowCompositionState(const QString &tag);
    // 診断用: ウィンドウと各ドックの実サイズをログへ出す(実装はMainWindow.cpp)。
    void logDockLayout(const QString &tag);
    // loadSettings()で復元したドック配置のバイト列。起動直後はウィンドウサイズが
    // 確定しておらず、そのまま貼るとドックが最小幅まで潰れるため、サイズが
    // 落ち着いてから貼り直す(理由の詳細はMainWindow.cppのコメント)。
    QByteArray savedDockState_;
    void scheduleDockLayoutRestore();
    // 最初のキャンバスを作るとウィンドウが作り直され、その拍子にドック配置が
    // 崩れることがある。作る直前の配置を控えて、崩れていたら戻す。
    bool firstCanvasCreated_ = false;
    void restoreDockLayoutAfterFirstCanvas(const QByteArray &before, int attempt);
    bool dockRestorePending_    = false; // 貼り直しがまだ済んでいないか(起動時の1回だけ)
    int  dockRestoreGeneration_ = 0;     // デバウンス用の世代番号
    // 全画面状態を最大化へ落とす処理の再入防止(setWindowState()が同期的に
    // 次のWindowStateChangeを飛ばすため。MainWindow::event()のコメント参照)。
    bool droppingFullScreen_ = false;
    bool nativeRepaintScheduled_ = false;
    // 表示前にrefreshNativeFrame()が呼ばれた(=フレームの再計算を先送りした)か。
    // Windowsはフレームの計算結果をキャッシュするので、先送りしたまま放置すると
    // 「Qtはタイトルバー無しでレイアウトしているのに実際は38px残っている」状態のまま
    // 動き続ける(実測済み)。表示された時点で必ず取り直すためのフラグ。
    bool pendingFrameRefresh_ = false;
    // 注意: ここで「通常表示中のジオメトリを覚えて復元時に貼り直す」ようなことは
    // しないこと。最大化の途中(まだisMaximized()がfalseの瞬間)に届くResizeで作業領域
    // いっぱいの矩形を覚えてしまい、それをsetGeometry()で当てると最大化が解除されて
    // 「作業領域サイズの復元状態」(Windows 11では角丸のまま)になる。最大化⇔復元で
    // サイズが縮んでいく問題は、WM_NCCALCSIZEを定石の形に直した時点で解消済み。

    // ---- 複数タブ(複数キャンバス)・分割表示(複数ペイン)関連 ----------------
    // 新しいペインを1つ生成し、4つのシグナル(currentTabChanged/tabCloseRequested/
    // splitRequested/tabInsertRequested)をこのMainWindow内のハンドラへ接続して返す
    // (ペインツリーへの組み込みは呼び出し側が行う)。
    CanvasPane *createPane();
    // 新しいタブを1枚(スタートページ状態で)生成してtargetPaneに追加する。アクティブ化は
    // しない(呼び出し側がactivateTabPage()する)。そのタブのスタートページの3ボタン/
    // カードは、このタブ自身を対象に動くよう接続される。
    CanvasTabPage *createTabPage(CanvasPane *targetPane);
    // pageにGLWidgetがまだ無ければ生成して載せる(=スタートページからキャンバスへ切替)。
    // すでにキャンバスなら既存のGLWidgetをそのまま返す。
    GLWidget *ensureCanvas(CanvasTabPage *page);
    // 新しいドキュメントを開く先のタブを返す。アクティブなタブがまだスタートページなら
    // それを再利用し(「タブを追加」直後にメニューから開いても空タブが残らない)、
    // すでにキャンバスなら新しいタブを1枚作る。
    CanvasTabPage *targetTabPageForNewDocument();
    // 「タブを追加」: スタートページのタブを1枚増やしてアクティブにする。
    void addNewTab();
    // glWidgetメンバを指定のGLWidgetに切り替え、そのGLWidgetの各シグナルをMainWindow/
    // 各Dockへ接続し(古い接続はglWidgetConnections_経由で解除済みである前提)、
    // 各Dockのretarget(setGLWidget)とrefresh()を行う。タブ作成時・タブ切替時の両方から呼ぶ。
    void bindGLWidget(GLWidget *gl);
    // 現在のGLWidgetへの接続を切ってglWidgetをnullptrに戻し、各Dockをダミー
    // (deckGLWidget_)へretargetする。アクティブなタブがスタートページになったとき用。
    void unbindGLWidget();
    // 操作権をpageへ移す単一の入口。所属ペイン(tabOwnerPane_)を特定してそのタブを
    // カレントにし、アクティブペインの見た目(枠線)を付け替えたうえで、pageが
    // キャンバスならbindGLWidget()、まだスタートページならunbindGLWidget()する。
    // ペイン内のタブ切替・ドラッグでのタブ移動・クリックによる操作権の移動の
    // すべてがここへ集約される。
    void activateTabPage(CanvasTabPage *page);
    // paneの現在のタブへ操作権を移す(クリックされたペインをアクティブにする用)。
    void activatePane(CanvasPane *pane);
    // paneの端(orientation/insertBeforeで方向を指定)へタブがドロップされたときの処理。
    // 新規ペインを作り、paneの直接の親がすでに同じorientationのQSplitterならそこへ
    // 兄弟として追加、そうでなければ新しいQSplitterで[pane, newPane]を包んで元の
    // 位置に差し込む。続けてsourceTabsからタブを移し、アクティブ化してから、移動元が
    // 空になっていればclosePaneIfEmpty()する(この順序が重要。activateTabPageが先)。
    void splitPane(CanvasPane *pane, Qt::Orientation orientation, bool insertBefore,
                    QTabWidget *sourceTabs, int sourceIndex);
    // destPaneの中央/タブバーへタブがドロップされたときの処理(分割はしない)。
    // splitPane()と同じ移動・アクティブ化・後始末だけを行う。
    void movePaneTab(CanvasPane *destPane, int destIndex, QTabWidget *sourceTabs, int sourceIndex);
    // paneのタブが0枚になったら、親から取り除いて削除する。親QSplitterの子が1個に
    // なったら、その1個を親の位置へ直接差し込んでスプリッター自体を畳む
    // (スプリッターは常に子2個以上、という不変条件により1段だけの操作で済む)。
    // paneがpaneTreeRoot_自体(=ペインがそれ1枚しか無かった)場合はペインを消さず、
    // 新しいスタートページのタブを1枚作って戻す(タブは常に1枚以上ある)。
    void closePaneIfEmpty(CanvasPane *pane);
    // paneTreeRoot_(CanvasPane*またはQSplitter*)を再帰的に辿り、全ペインの全タブの
    // うちキャンバスになっているものだけを集めて返す(スタートページのタブは含まない)。
    QVector<GLWidget *> allCanvasWidgets() const;
    // 新規タブの追加先ペインを返す。既存のペインツリーがあればactivePane_(通常は
    // 常に有効)、無ければ(起動直後)createPane()で最初のペインを新規作成して
    // centralStackへ組み込む。
    CanvasPane *ensureRootPane();
    // 未保存の変更があるかを確認し、破棄してよいかをユーザーに尋ねる。
    // 変更が無ければ即true。ダイアログで「保存せず進める」を選べばtrue、「戻る」ならfalse。
    bool confirmDiscardChanges(GLWidget *gl, const QString &message, const QString &yesLabel);
    // 該当タブのタブ見出しを更新する(キャンバスならファイル名+未保存マーク、
    // まだスタートページなら「スタートページ」)。
    void updateTabLabel(CanvasTabPage *page);
    void updateTabLabel(GLWidget *gl); // glの所属タブを引いて上のオーバーロードへ委譲
    // 新規キャンバス作成時、未保存の間タブ見出しに表示する仮の名前("Canvas001"等)を
    // 採番する。保存先候補(TiepoloProjects/CanvasNNN.tplo)に既存ファイルが無く、かつ
    // 他の未保存タブの仮の名前とも被らない番号を選ぶ。
    QString generateUniqueCanvasName() const;
    // pageをキャンバスに変え、そのGLWidget::initialized完了後にpathを読み込む。
    void openFileIntoTab(CanvasTabPage *page, const QString &path);
    // ファイルダイアログでパスを選んでからopenFileIntoTab()する。pageがnullptrなら
    // 開くパスが確定してから追加先タブを決める(キャンセル時に空タブを作らないため)。
    void loadFileIntoTab(CanvasTabPage *page);
    // 新規キャンバスダイアログを出してからpageをキャンバスに変える。pageがnullptrなら
    // OKされてから追加先タブを決める(同上)。
    void newCanvasInTab(CanvasTabPage *page);
    // 追加先タブを自動で選ぶ版(コマンドライン引数・ドラッグ&ドロップ用)。
    void openFileIntoNewTab(const QString &path);
    // centralStackの表示ページをpaneTreeRoot_に合わせ、キャンバス依存アクションの
    // 有効/無効を揃える。
    void updateCentralPage();
    // 「透過タブ」が表示されているぶんだけ、ウィンドウ自体に穴を開ける。
    // 各ペインのカレントタブを見て透過タブの矩形(このウィンドウのクライアント座標系)を
    // 集め、applyWindowRegion()へ渡す。
    void updateWindowMask();
    // holesのぶんだけ穴の空いたウィンドウリージョンを実際に適用する(実装は
    // MainWindowNative.cpp)。QWidget::setMask()は使えない ― Qtが領域を
    // 「スタイルから逆算したフレームマージン」ぶんずらしてしまい、WM_NCCALCSIZEで
    // タイトルバーを潰した実際のクライアント領域とずれるため(詳細はそちらのコメント)。
    void applyWindowRegion(const QVector<QRect> &holes);
    // 直前に適用した穴。同じ内容なら再適用を省く(SetWindowRgn()はウィンドウ全体の
    // 再描画を伴うので、リサイズ中の連続呼び出しがそのまま重さになる)。
    QVector<QRect> lastAppliedHoles_;
    // 直前に適用したときのウィンドウ矩形(スクリーン座標・物理px)。リージョンは
    // ウィンドウ座標系で作られ、ウィンドウのサイズが変わっても自動追従しないため、
    // 「穴は同じだがウィンドウが変わった=貼ってあるリージョンが古い」を検出するのに使う
    // (診断ログのSTALE REGION判定。NativeWindowLog.h参照)。
    QRect lastAppliedWindowRect_;
    void updateTabRelatedActionsEnabled();

    // ---- カラーモード/モニターキャリブレーション(見た目だけのプレビュー) ----
    // toolCfg->colorMode()/calibration()へ書き込み、開いている全タブに
    // applyDisplayConfig()を呼んで即座に反映する。
    void setColorMode(ColorMode m);
    void openCalibrationDialog();
    void setCalibration(int brightness, int contrast, int cyan, int magenta, int yellow);
    void propagateDisplayConfigToAllTabs();

    // ---- UIテーマ ----
    void setUiTheme(Theme::Name n);

    // ---- キャンバス外側の背景色 ----
    void chooseCanvasBackgroundColor();
    void setCanvasBackgroundColor(const QColor &c);

private slots:
    void exportImage();
    // Photoshopのブラシファイル(.abr)とペン設定のやり取り(backend/AbrCodec.h参照)。
    // 対応するのは両方に同じ意味がある項目だけなので、移せなかったものは
    // 実行後にまとめて提示する。
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
    // CanvasPane::currentTabChanged / tabCloseRequestedへ、createPane()内で
    // ペインごとに接続する(単一tabWidgetのスロットだった頃と違い、どのペインの
    // 出来事かをpane引数で受け取る必要があるため、シグナルと直接同じ引数列にしてある)。
    void onCurrentTabChanged(CanvasPane *pane, int index);
    void onTabCloseRequested(CanvasPane *pane, int index);
    // 設定ダイアログの自動保存設定(有効/間隔)を読み込み、タイマーへ反映する
    // (起動時のloadSettings()後、および設定ダイアログでOKされた直後に呼ぶ)。
    void applyAutoSaveSettings();
    // 環境設定の「全体の筆圧カーブ」をToolConfigへ反映する(起動時/設定OK時)
    void applyGlobalPressureCurve();
    QTimer *autoSaveTimer_ = nullptr;
};