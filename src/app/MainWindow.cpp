#include "app/MainWindow.h"
#include "app/MainWindowHelpers.h"
#include "docks/LayerDock.h"
#include "docks/ColorCircleDock.h"
#include "docks/ToolDock.h"
#include "docks/ToolPropDock.h"
#include "docks/ToolPresetDock.h"
#include "docks/BrushSizeDock.h"
#include "docks/NavigatorDock.h"
#include "components/DockTitleBar.h"
#include "components/DockResizeController.h"
#include "components/ColorWheelWidget.h" // 色相ツイスト(環境設定)の反映
#include "app/StartPage.h"
#include "canvas/CanvasPane.h"
#include "canvas/CanvasTabBar.h"
#include "canvas/CanvasTabPage.h"
#include "app/NativeWindowLog.h"
#include "document/RecentFiles.h"
#include "io/AbrCodec.h"
#include "io/AbrPenMapping.h"
#include "dialogs/CalibrationDlg.h"

#include <QScreen>
#include <QShowEvent>
#include <QMenuBar>
#include <QToolButton>
#include <QWindow>
#include <QWindowStateChangeEvent>
#include <QToolBar>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QAction>
#include <QActionGroup>
#include <QShortcut>
#include <QFileDialog>
#include <QInputDialog>
#include <QColorDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QPushButton>
#include <QDialog>
#include <QTextBrowser>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QApplication>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QTabWidget>
#include <QTabBar>
#include <QStackedWidget>
#include <QSplitter>
#include <QTimer>
#include <QStyle>
#include <QMouseEvent>
#include <QCursor>
#include <QProgressDialog>
#include <QElapsedTimer>

using MainWindowHelpers::brushSizeFor;
using MainWindowHelpers::setBrushSizeFor;
// MainWindow.cpp: responsibilities separated from the MainWindow integration hub.

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    // ウィンドウを作る前に呼ぶこと(以後に作られるウィンドウへ効く)。
    disableWindowGhosting();

    #ifdef TIEPOLO_PRO_BUILD
    setWindowTitle("Tiepolo Pro");
    #else
    setWindowTitle("Tiepolo");
    #endif

    // タイトルバーを出さないためのウィンドウフラグ。
    //
    // Qt::WindowTitleHintを「付ける」こと。見た目のタイトルバーはWM_NCCALCSIZEで
    // 非クライアント領域ごと潰すので出てこない。Claude・Discord・CLIP STUDIO PAINTなど
    // 自前タイトルバーのアプリは、どれもこの形(スタイル上は普通の枠付きウィンドウの
    // まま、描画領域だけ消す)になっている。
    //
    // 以前はTitleHintを外してWS_CAPTIONごと落としていたが、それが多くの不具合の
    // 大元だった。TitleHintが無いとQtはWS_CAPTIONだけでなくWS_THICKFRAMEまで落とし
    // (実測: 起動直後に STYLE: CAPTION|THICKFRAME|... -> POPUP|SYSMENU)、
    // リサイズ枠を持たないウィンドウになる。すると最大化しても
    // 「枠のぶん画面外へはみ出す」形にならず、ウィンドウ矩形がモニタ矩形と
    // ぴったり一致してしまう。Windowsのシェルはこれを「最大化」ではなく
    // 「全画面アプリ」とみなすため、
    //   ・自動的に隠れるタスクバーが出てこない(SHQueryUserNotificationState=BUSY)
    //   ・DWMがDirect Flipに切り替わり、メニュー等が重なるたび点滅・暗転する
    //   ・GLの面のアルファがそのまま合成され、メニューバーが透ける
    // が同時に起きていた。「一度ウィンドウを動かして最大化し直すと以後は直る」のは、
    // そのときOSが枠込みで最大化ジオメトリを計算し直すため。
    //
    // WS_CAPTIONが残るとQtがフレーム余白を多めに見積もる件は、ネイティブウィンドウが
    // 出来た直後にSWP_FRAMECHANGEDでフレームを取り直すことで解消している
    // (MainWindowNative.cpp の refreshNativeFrame() 参照。実測でクライアント原点が
    //  (1,38) から (0,0) になることを確認済み)。
    //
    // Qt::FramelessWindowHintは使わない ― あれもWS_THICKFRAME等ごと落としてしまい、
    // 上と同じ状態になる。最小化/最大化ボタンのヒントは、見た目のボタンではなく
    // WS_MINIMIZEBOX/WS_MAXIMIZEBOXを残すために付けている(これが無いと画面端への
    // スナップやWin+矢印が効かない)。
    setWindowFlags(Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint
                    | Qt::WindowSystemMenuHint
                    | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint);

    WINLOG(QStringLiteral("CTOR setWindowFlags -> 0x%1").arg((quint32)windowFlags(), 0, 16));

    if (QSettings().value("window/alwaysOnTop", false).toBool()) {
        setWindowFlag(Qt::WindowStaysOnTopHint, true);
        WINLOG(QStringLiteral("CTOR alwaysOnTop -> flags 0x%1").arg((quint32)windowFlags(), 0, 16));
    }

    toolCfg = new ToolConfig();

    // カラーサークルの色相ツイストは環境設定由来のアプリ全体共通値。
    // createWidgets()でColorWheelWidgetが作られる前に反映しておくと、最初から
    // 正しいツイスト量でスクエア画像が構築される(後で入れると作り直し+
    // 復元した現在色の上書きが発生してしまう)。
    ColorWheelWidget::setHueTwist((float)SettingsDlg::loadValues().hueTwist);

    // 全ツール共通の筆圧カーブも環境設定由来のアプリ全体共通値。ToolConfigが
    // 持ち、CanvasWidget::mapPressure()がツールごとのカーブより先に適用する。
    applyGlobalPressureCurve();

    createWidgets();
    setupActions();  // QAction生成・ショートカット登録(MainWindowShortcuts.cpp)

    // toolDockはsetupActions()より前のcreateWidgets()内で生成される(=構築時点では
    // shortcuts_にまだ何も登録されておらずキー表示が空になる)ため、ここで改めて
    // ツールチップのキー表示を最新の状態に合わせておく。
    toolDock->refreshTooltips();

    connectSignals();
    createMenus();
    setupCaptionButtons(); // createMenus()でメニューバーが出来た後に置く

    // 中央には常にタブが1枚以上ある。起動直後はスタートページのタブ1枚から始める
    // (updateTabRelatedActionsEnabled()が触るQAction群を使うため、setupActions()より後)。
    addNewTab();

    // 【試して駄目だったこと】最初のキャンバスが出来た直後にウィンドウが
    // 作り直される(WM_WINDOWPOSCHANGING flags=FRAMECHANGED|SHOWWINDOW → WM_NCCALCSIZE
    // → WM_SIZE)のを避けようとして、ネイティブウィンドウ生成より前に描画に使わない
    // QOpenGLWidgetを1枚置き、最初からOpenGLの面としてウィンドウを作らせる手を
    // 試した。隠した場合も1x1で表示した場合も、作り直しは同じタイミング
    // (GL初期化と初回paintGLの直後)に起きたので撤回。
    // よく考えると createWidgets() の deckCanvasWidget_ が既に同じ条件を満たしており、
    // 「QOpenGLWidgetがウィジェット木に居ること」では決まらないことが分かる
    // (実際に描画されて初めて切り替わる)。同じ手を再発明しないこと。
    //
    // 作り直し自体はウィンドウ矩形もQt側のレイアウトも変えない(この環境の実測では
    // win/menu/central/各ドックのサイズが前後で完全一致)。ただし環境によっては
    // このときのドック再配分で幅が潰れるため、崩れたら戻す形で受け止める
    // (MainWindow::restoreDockLayoutAfterFirstCanvas())。

    qApp->installEventFilter(this);
    installNativeWindowFilter(); // タイトルバー非表示まわりのWindows固有処理
    // ここでrefreshNativeFrame()を予約しないこと。フィルタは表示前から効いており
    // WM_NCCALCSIZEはウィンドウ生成の時点から正しく適用されるため、フレームの
    // 再計算を改めて要求する意味が無い。むしろ起動時の余計なリサイズになる
    // (作り直されたときはQEvent::WinIdChange側で貼り直している)。

    addDockWidget(Qt::LeftDockWidgetArea, navigatorDockWidget);
    addDockWidget(Qt::LeftDockWidgetArea, toolPropDockWidget);
    addDockWidget(Qt::LeftDockWidgetArea, toolPresetDockWidget);
    addDockWidget(Qt::LeftDockWidgetArea, brushSizeDockWidget);
    addDockWidget(Qt::LeftDockWidgetArea, toolDockWidget);
    addDockWidget(Qt::RightDockWidgetArea, colorCircleDockWidget);
    addDockWidget(Qt::RightDockWidgetArea, layerDockWidget);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    setDockNestingEnabled(true);

    // 標準セパレータは0pxにし、見た目を持たない広めのヒット領域だけを境界へ重ねる。
    // Controller自身がMainWindowの子になるため、ここで所有ポインタを保持する必要はない。
    new DockResizeController(*this);

    // ドックを重ねてタブ化した際にQtが遅延生成する切り替え用QTabBarを捕まえて
    // DockTitleBar風の見た目(閉じるボタン付き)に揃え直す。生成タイミングを
    // 捉えるクリーンなフックが無いため、軽い定期ポーリングで済ませる。
    auto *dockTabBarTimer = new QTimer(this);
    connect(dockTabBarTimer, &QTimer::timeout, this, [this] {
        styleNewDockTabBars();
        updateTabifiedTitleBars();
    });
    dockTabBarTimer->start(300);

    QSettings settings;
    if (settings.contains("window/state")) {
        loadSettings();
    } else {
        // resetDockLayout()(手動の「ドック配置をリセット」と共通)はrestoreState()を
        // 使っており、その復元結果はウィンドウの実サイズに依存する。手動実行時は
        // ウィンドウがすでに表示・最大化済みの状態で呼ばれるのに対し、コンストラクタ
        // 内でこのまま同期的に呼ぶとウィンドウがまだ一度も表示されておらず実サイズが
        // 確定していないため、同じrestoreState()の結果が変わってしまう(一部のドックが
        // 正しく配置されない)。main()のw.show()が実行され、イベントループが回り始めて
        // ウィンドウが実際に表示された直後に呼ばれるよう遅延させることで、手動実行時と
        // 同じ条件(表示済みウィンドウに対して呼ぶ)に揃える。
        QTimer::singleShot(0, this, &MainWindow::resetDockLayout); // 初回起動時は初期レイアウトを適用
    }

    // loadSettings()は保存済みの色(tool/color/rawRGBA)がある場合のみ
    // colorCircleDockへ反映するため、初回起動時(保存設定が無い場合)は
    // toolCfgの初期色(黒)がカラーサークルのUIへ一度も反映されないまま
    // (ColorWheelWidget自身のコンストラクタ既定値である中間グレー位置の
    // まま)になってしまう。ここで無条件に同期しておく。
    colorCircleDock->setColor(toolCfg->color().rawRGBA());

    autoSaveTimer_ = new QTimer(this);
    connect(autoSaveTimer_, &QTimer::timeout, this, &MainWindow::performAutoSave);
    applyAutoSaveSettings();
}

MainWindow::~MainWindow() = default;

void MainWindow::createMenus() {
    QMenu *fileMenu = menuBar()->addMenu("ファイル(&F)");
    fileMenu->addAction(newTabAction);
    fileMenu->addSeparator();
    fileMenu->addAction(newCanvasAction);
    fileMenu->addAction(loadAction);
    fileMenu->addSeparator();
    fileMenu->addAction(saveAction);
    fileMenu->addAction(saveAsAction);
    fileMenu->addSeparator();
    fileMenu->addAction(exportAction);
    fileMenu->addSeparator();
    fileMenu->addAction(addImageLayerAction);
    fileMenu->addSeparator();
    fileMenu->addAction("ブラシ設定を読み込み(.abr)...", this, &MainWindow::importPenSettingsFromAbr);
    fileMenu->addAction("ブラシ設定を書き出し(.abr)...", this, &MainWindow::exportPenSettingsToAbr);
    fileMenu->addSeparator();
    fileMenu->addAction("終了", this, &QWidget::close);

    QMenu *editMenu = menuBar()->addMenu("編集(&E)");
    editMenu->addAction(undoAction);
    editMenu->addAction(redoAction);
    editMenu->addSeparator();
    editMenu->addAction(cutAction);
    editMenu->addAction(copyAction);
    editMenu->addAction(pasteAction);
    editMenu->addAction(selectAllAction);
    editMenu->addAction(clearSelectionAction);
    editMenu->addSeparator();
    editMenu->addAction(brushBiggerAction);
    editMenu->addAction(brushSmallerAction);
    editMenu->addSeparator();
    editMenu->addAction(canvasSizeAction);
    editMenu->addAction(imageResolutionAction);
    editMenu->addAction(rotateCanvas90Action);
    editMenu->addAction(rotateCanvas180Action);
    editMenu->addAction(rotateCanvas270Action);
    editMenu->addAction(flipCanvasHorizontalAction);
    editMenu->addAction(flipCanvasVerticalAction);

    // 変形の確定(Enter)はメニューには出さない一時的なアクションなので、
    // メニューに積まずに直接ウィンドウへ関連付けてショートカットだけ有効にする。
    addAction(confirmTransformAction);

    QMenu *processMenu = menuBar()->addMenu("処理(&P)");
    QMenu *transformMenu = processMenu->addMenu("変形");
    transformMenu->addAction(transformAction);
    transformMenu->addAction(freeTransformAction);
    QMenu *toneMenu = processMenu->addMenu("色調補正");
    toneMenu->addAction(brightnessContrastAction);
    toneMenu->addAction(hueSatLightAction);
    toneMenu->addAction(colorBalanceAction);
    toneMenu->addAction(toneCurveAction);
    toneMenu->addAction(gradientMapAction);
    QMenu *blurMenu = processMenu->addMenu("ぼかし");
    blurMenu->addAction(gaussianBlurFilterAction);
    blurMenu->addAction(motionBlurFilterAction);
    blurMenu->addAction(lensBlurFilterAction);
    blurMenu->addAction(mosaicFilterAction);
    QMenu *effectMenu = processMenu->addMenu("効果");
    effectMenu->addAction(chromaticAberrationFilterAction);
    effectMenu->addAction(noiseFilterAction);
    processMenu->addAction(customShaderFilterAction);
    
    QMenu *layerMenu = menuBar()->addMenu("レイヤー(&L)");
    newLayerAction = layerMenu->addAction("新規レイヤー");
    connect(newLayerAction, &QAction::triggered, this, [this] { layerDock->addRow(); });
    newClippingLayerAction = layerMenu->addAction("新規クリッピングレイヤー");
    connect(newClippingLayerAction, &QAction::triggered, this, [this] { layerDock->addColumn(); });
    newSolidColorLayerAction = layerMenu->addAction("新規単色レイヤー");
    connect(newSolidColorLayerAction, &QAction::triggered, this, [this] { layerDock->insertSolidColorLayer(); });

    newAdjustmentLayerAction = layerMenu->addAction("新規調整レイヤー");
    connect(newAdjustmentLayerAction, &QAction::triggered, this, [this] { layerDock->insertAdjustmentLayer(); });

    newTextLayerAction = layerMenu->addAction("新規テキストレイヤー");
    connect(newTextLayerAction, &QAction::triggered, this, [this] { layerDock->insertTextLayer(); });
    layerMenu->addSeparator();
    duplicateLayerAction = layerMenu->addAction("レイヤーを複製");
    connect(duplicateLayerAction, &QAction::triggered, this, [this] { layerDock->duplicateSelected(); });
    layerMenu->addSeparator();
    mergeLayerAction = layerMenu->addAction("レイヤーを結合");
    connect(mergeLayerAction, &QAction::triggered, this, [this] { layerDock->mergeSelected(); });
    layerMenu->addSeparator();
    deleteLayerMenuAction = layerMenu->addAction("レイヤーを削除");
    connect(deleteLayerMenuAction, &QAction::triggered, this, [this] { layerDock->deleteActiveLayer(); });

    QMenu *viewMenu = menuBar()->addMenu("表示(&V)");
    viewMenu->addAction(zoomInAction);
    viewMenu->addAction(zoomOutAction);
    viewMenu->addAction(fitAction);
    viewMenu->addAction(viewFlipXAction);
    viewMenu->addSeparator();

    alwaysOnTopAction_ = viewMenu->addAction("常に最前面に表示");
    alwaysOnTopAction_->setCheckable(true);
    alwaysOnTopAction_->setChecked(QSettings().value("window/alwaysOnTop", false).toBool());
    alwaysOnTopAction_->setToolTip("透過タブの穴から後ろのウィンドウを操作しても、"
                                    "このウィンドウが後ろに隠れないようにします。");
    connect(alwaysOnTopAction_, &QAction::toggled, this, &MainWindow::setAlwaysOnTop);
    viewMenu->addSeparator();

    // カラープレビュー: 表示上の見た目だけを変えるプレビュー機能(データはRGBAのまま)。
    QMenu *colorModeMenu = viewMenu->addMenu("カラープレビュー");
    auto *colorModeGroup = new QActionGroup(this);
    colorModeRgbAction           = colorModeMenu->addAction("RGB");
    colorModeCmykAction          = colorModeMenu->addAction("CMYK");
    colorModeGrayLuminanceAction = colorModeMenu->addAction("グレースケール(輝度)");
    colorModeGrayLightnessAction = colorModeMenu->addAction("グレースケール(明度)");
    for (QAction *a : {colorModeRgbAction, colorModeCmykAction,
                        colorModeGrayLuminanceAction, colorModeGrayLightnessAction}) {
        a->setCheckable(true);
        colorModeGroup->addAction(a);
    }
    colorModeRgbAction->setChecked(true);
    connect(colorModeRgbAction,  &QAction::triggered, this, [this]{ setColorMode(ColorMode::RGB); });
    connect(colorModeCmykAction, &QAction::triggered, this, [this]{ setColorMode(ColorMode::CMYK); });
    connect(colorModeGrayLuminanceAction, &QAction::triggered, this, [this]{ setColorMode(ColorMode::GrayscaleLuminance); });
    connect(colorModeGrayLightnessAction, &QAction::triggered, this, [this]{ setColorMode(ColorMode::GrayscaleLightness); });

    QMenu *windowMenu = menuBar()->addMenu("ウィンドウ(&W)");
    windowMenu->addAction(toolDockWidget->toggleViewAction());
    windowMenu->addAction(toolPropDockWidget->toggleViewAction());
    windowMenu->addAction(toolPresetDockWidget->toggleViewAction());
    windowMenu->addAction(colorCircleDockWidget->toggleViewAction());
    windowMenu->addAction(layerDockWidget->toggleViewAction());
    windowMenu->addAction(brushSizeDockWidget->toggleViewAction());
    windowMenu->addAction(navigatorDockWidget->toggleViewAction());
    windowMenu->addSeparator();
    windowMenu->addAction(resetLayoutAction);
    windowMenu->addAction(dumpStateAction);

    QMenu *settingMenu = menuBar()->addMenu("設定(&S)");
    settingMenu->addAction(settingsAction);
    settingMenu->addAction(shortcutsAction);
    settingMenu->addSeparator();
    settingMenu->addAction(resetAllSettingsAction);

    // UIテーマ: アプリ全体の配色(resources/style.qss + Theme::)をライト/ダークで切り替える。
    QMenu *themeMenu = settingMenu->addMenu("テーマ");
    auto *themeGroup = new QActionGroup(this);
    themeDarkAction  = themeMenu->addAction("ダーク");
    themeLightAction = themeMenu->addAction("ライト");
    for (QAction *a : {themeDarkAction, themeLightAction}) {
        a->setCheckable(true);
        themeGroup->addAction(a);
    }
    (Theme::currentName == Theme::Name::Dark ? themeDarkAction : themeLightAction)->setChecked(true);
    connect(themeDarkAction,  &QAction::triggered, this, [this]{ setUiTheme(Theme::Name::Dark); });
    connect(themeLightAction, &QAction::triggered, this, [this]{ setUiTheme(Theme::Name::Light); });

    canvasBgColorAction = settingMenu->addAction("キャンバス背景色...");
    connect(canvasBgColorAction, &QAction::triggered, this, &MainWindow::chooseCanvasBackgroundColor);
    settingMenu->addSeparator();

    calibrationAction = settingMenu->addAction("モニターキャリブレーション...");
    connect(calibrationAction, &QAction::triggered, this, &MainWindow::openCalibrationDialog);
    settingMenu->addSeparator();

    QMenu *helpMenu = menuBar()->addMenu("ヘルプ(&H)");
    helpMenu->addAction(helpAction);
    helpMenu->addAction(versionAction);
}

void MainWindow::createWidgets() {
    // centralStackはペインツリー(paneTreeRoot_)を丸ごと差し替えるためだけの入れ物。
    // 分割/畳みでルートがCanvasPaneとQSplitterの間を行き来するので、QMainWindowの
    // centralWidgetを直接付け替える(=古い方が破棄される)のを避けている。
    // スタートページはもう独立したページではなく、各タブ(CanvasTabPage)の中にある。
    centralStack = new QStackedWidget(this);
    centralStack->setObjectName(QStringLiteral("canvasWorkspace"));
    centralStack->setFrameShape(QFrame::NoFrame);
    centralStack->setContentsMargins(0, 0, 0, 0);
    setCentralWidget(centralStack);
    // 最初のペインとタブはコンストラクタ側(setupActions()の後)で作る。

    // Dock類のコンストラクタはCanvasWidget*を要求するため、タブが1枚も無い間のダミーとして
    // 使う非表示CanvasWidgetを1つ用意する(どのペインのタブにも追加せず、表示もしないため
    // initializeGL()は走らず、GLリソースのコストは掛からない)。
    deckCanvasWidget_ = new CanvasWidget(toolCfg, this);
    deckCanvasWidget_->hide(); // 表示しない(initializeGL()を走らせないため。Dock構築用のダミー)

    toolDock       = new ToolDock(deckCanvasWidget_, shortcuts_, this);
    toolDockWidget = new QDockWidget("ツール", this);
    toolDockWidget->setObjectName("toolDockWidget");
    toolDockWidget->setAllowedAreas(Qt::AllDockWidgetAreas);
    toolDockWidget->setTitleBarWidget(new DockTitleBar("ツール", toolDockWidget));
    toolDockWidget->setWidget(toolDock);

    toolPropDock       = new ToolPropDock(deckCanvasWidget_, toolCfg, this);
    toolPropDockWidget = new QDockWidget("ツール設定", this);
    toolPropDockWidget->setObjectName("toolPropDockWidget");
    toolPropDockWidget->setAllowedAreas(Qt::AllDockWidgetAreas);
    toolPropDockWidget->setTitleBarWidget(new DockTitleBar("ツール設定", toolPropDockWidget));
    toolPropDockWidget->setWidget(toolPropDock);

    toolPresetDock       = new ToolPresetDock(deckCanvasWidget_, toolCfg, this);
    toolPresetDockWidget = new QDockWidget("ツールプリセット", this);
    toolPresetDockWidget->setObjectName("toolPresetDockWidget");
    toolPresetDockWidget->setAllowedAreas(Qt::AllDockWidgetAreas);
    toolPresetDockWidget->setTitleBarWidget(new DockTitleBar("ツールプリセット", toolPresetDockWidget));
    toolPresetDockWidget->setWidget(toolPresetDock);

    brushSizeDock       = new BrushSizeDock(this);
    brushSizeDockWidget = new QDockWidget("ブラシサイズ", this);
    brushSizeDockWidget->setObjectName("brushSizeDock");
    brushSizeDockWidget->setAllowedAreas(Qt::AllDockWidgetAreas);
    brushSizeDockWidget->setTitleBarWidget(new DockTitleBar("ブラシサイズ", brushSizeDockWidget));
    brushSizeDockWidget->setWidget(brushSizeDock);

    navigatorDock       = new NavigatorDock(deckCanvasWidget_, this);
    navigatorDockWidget = new QDockWidget("ナビゲーター", this);
    navigatorDockWidget->setObjectName("navigatorDock");
    navigatorDockWidget->setAllowedAreas(Qt::AllDockWidgetAreas);
    navigatorDockWidget->setTitleBarWidget(new DockTitleBar("ナビゲーター", navigatorDockWidget));
    navigatorDockWidget->setWidget(navigatorDock);

    colorCircleDock       = new ColorCircleDock(this);
    colorCircleDockWidget = new QDockWidget("カラーサークル", this);
    colorCircleDockWidget->setObjectName("colorCircleDockWidget");
    colorCircleDockWidget->setAllowedAreas(Qt::AllDockWidgetAreas);
    colorCircleDockWidget->setTitleBarWidget(new DockTitleBar("カラーサークル", colorCircleDockWidget));
    colorCircleDockWidget->setWidget(colorCircleDock);

    layerDock       = new LayerDock(deckCanvasWidget_, this);
    layerDockWidget = new QDockWidget("レイヤー", this);
    layerDockWidget->setObjectName("layerDockWidget");
    layerDockWidget->setAllowedAreas(Qt::AllDockWidgetAreas);
    layerDockWidget->setTitleBarWidget(new DockTitleBar("レイヤー", layerDockWidget));
    layerDockWidget->setWidget(layerDock);

    // 以前はここでtabWidgetのcurrentChanged/tabCloseRequestedを直接繋いでいたが、
    // 分割表示対応でペインが複数になりうるため、ペインごとにcreatePane()内で
    // CanvasPane::currentTabChanged/tabCloseRequestedへ接続するようになった。

    // 初期状態(タブ0枚 → StartPage表示、キャンバス依存アクション無効化、Dock非表示)への
    // 反映はここでは行わない。updateTabRelatedActionsEnabled()が触るQAction群は
    // まだ生成されていない(setupActions()はcreateWidgets()の後に呼ばれる)ため、
    // MainWindowコンストラクタ側でsetupActions()の後にupdateCentralPage()を呼ぶ。
}

// glWidget(現在アクティブなタブ)に対する接続を張り直す。タブ作成直後・タブ切替の
// どちらからも呼ばれる。呼び出し前に古い接続(glWidgetConnections_)を解除しておくこと。

void MainWindow::connectSignals() {
    // ---- Dock間の接続(いずれもglWidgetに依存しないので、タブ切替に関係なく
    // アプリ生存中ずっと固定でよい。glWidget依存の接続はbindCanvasWidget()側にある) ----
    connect(toolDock, &ToolDock::toolChanged, this, [this](ToolType curr) {
        toolCfg->color().setRawRGBA(colorCircleDock->color());
        toolCfg->color().setTransparent(colorCircleDock->isTransparent());
        brushSizeDock->syncSize(brushSizeFor(toolCfg, curr));
        toolPropDock->setCurrentTool(curr);
    });
    connect(toolPropDock, &ToolPropDock::sizeChanged, this, [this]() {
        // タブが1枚も無い間はglWidgetがnullptrになるため、その間はダミーの
        // deckCanvasWidget_を使う(ToolDockのアクティブツールもタブ0枚の間は
        // deckCanvasWidget_側に反映されているので、これで正しい値が取れる)。
        CanvasWidget *gl = glWidget ? glWidget : deckCanvasWidget_;
        ToolType curr = gl->getActiveTool();
        brushSizeDock->syncSize(brushSizeFor(toolCfg, curr));
        gl->updateCursor();
    });
    connect(brushSizeDock, &BrushSizeDock::brushSizeChanged, this, [this](int px) {
        CanvasWidget *gl = glWidget ? glWidget : deckCanvasWidget_;
        ToolType curr = gl->getActiveTool();
        setBrushSizeFor(toolCfg, curr, px);
        toolPropDock->syncSize(px);
        gl->updateCursor();
    });
    connect(colorCircleDock, &ColorCircleDock::colorChanged, this, [this](const QColor &c) {
        toolCfg->color().setRawRGBA(c);
    });
    // 「透明色」のオン/オフ。色そのもの(アルファ=消す強さを含む)はcolorChanged側で
    // 通知され続けるので、ここではフラグだけを移す。
    connect(colorCircleDock, &ColorCircleDock::transparentChanged, this, [this](bool on) {
        toolCfg->color().setTransparent(on);
        if (glWidget) glWidget->update(); // ライブプレビューの色が変わるので描き直す
    });
}

// ===========================================================================
// Photoshopのブラシファイル(.abr)
// ---------------------------------------------------------------------------
// 対応するのは両方に同じ意味がある項目だけ(AbrPenMapping参照)。移せなかった
// ものは黙って落とさず、実行後にまとめて提示する。
// ===========================================================================

