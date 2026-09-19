#include "widgets/MainWindow.h"
#include "docks/LayerDock.h"
#include "docks/ColorCircleDock.h"
#include "docks/ToolDock.h"
#include "docks/ToolPropDock.h"
#include "docks/ToolPresetDock.h"
#include "docks/BrushSizeDock.h"
#include "docks/NavigatorDock.h"
#include "components/DockTitleBar.h"
#include "components/ColorWheelWidget.h" // 色相ツイスト(環境設定)の反映
#include "widgets/StartPage.h"
#include "widgets/CanvasPane.h"
#include "widgets/CanvasTabBar.h"
#include "widgets/CanvasTabPage.h"
#include "widgets/NativeWindowLog.h"
#include "backend/RecentFiles.h"
#include "backend/AbrCodec.h"
#include "backend/AbrPenMapping.h"
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

// ===========================================================================
// ブラシサイズを持つツール(ペン/消しゴム/ぼかし)のサイズを、ToolTypeから
// 正しいConfigに読み書きするヘルパー。ブラシサイズドックとの同期に使う。
// (新しくサイズを持つツールを足す場合はここに1行追加する)
// ===========================================================================
static int brushSizeFor(ToolConfig *cfg, ToolType t)
{
    if (t == ToolType::Eraser)    return cfg->eraser().size();
    if (t == ToolType::Blur)      return cfg->blur().size();
    if (t == ToolType::Warp)      return cfg->warp().size();
    if (t == ToolType::Selection) return cfg->selection().size();
    if (t == ToolType::Airbrush)  return cfg->airbrush().size();
    return cfg->pen().size();
}
static void setBrushSizeFor(ToolConfig *cfg, ToolType t, int px)
{
    if (t == ToolType::Eraser)         cfg->eraser().setSize(px);
    else if (t == ToolType::Blur)      cfg->blur().setSize(px);
    else if (t == ToolType::Warp)      cfg->warp().setSize(px);
    else if (t == ToolType::Selection) cfg->selection().setSize(px);
    else if (t == ToolType::Airbrush)  cfg->airbrush().setSize(px);
    else                                cfg->pen().setSize(px);
}

// ===========================================================================
// MainWindow の実装
// ===========================================================================
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
    // 持ち、GLWidget::mapPressure()がツールごとのカーブより先に適用する。
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
    // よく考えると createWidgets() の deckGLWidget_ が既に同じ条件を満たしており、
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

CanvasTabPage *MainWindow::createTabPage(CanvasPane *targetPane)
{
    auto *page = new CanvasTabPage(targetPane->tabWidget());
    targetPane->tabWidget()->addTab(page, "スタートページ");
    tabOwnerPane_.insert(page, targetPane);
    page->startPage()->refresh(); // 最近使ったファイル一覧を今の内容で組み立てる

    // スタートページの操作は、どのタブのスタートページかを問わず「そのタブ自身」を
    // キャンバスへ変える(新しいタブを作るのではない)。
    connect(page->startPage(), &StartPage::newCanvasRequested, this,
            [this, page] { newCanvasInTab(page); });
    connect(page->startPage(), &StartPage::openFileRequested, this,
            [this, page] { loadFileIntoTab(page); });
    connect(page->startPage(), &StartPage::openRecentFileRequested, this,
            [this, page](const QString &path) { openFileIntoTab(page, path); });
    connect(page->startPage(), &StartPage::transparentTabRequested, this, [this, page] {
        page->makeTransparent();
        updateTabLabel(page);
        activateTabPage(page);
    });
    // 透過タブの穴の位置・大きさ・表示状態が変わったらウィンドウのマスクを貼り直す
    // (ウィンドウのリサイズ、分割・スプリッターのドラッグ、タブ切替がここへ集まる)。
    connect(page, &CanvasTabPage::transparentGeometryChanged, this, &MainWindow::updateWindowMask);

    return page;
}

GLWidget *MainWindow::ensureCanvas(CanvasTabPage *page)
{
    if (!page) return nullptr;
    if (GLWidget *existing = page->canvas()) return existing;

    // このウィンドウで最初のキャンバスを作る前に、今のドック配置を控えておく
    // (使い道は下のGLWidget::initializedハンドラのコメント)。
    const bool firstCanvas = !firstCanvasCreated_;
    firstCanvasCreated_ = true;
    const QByteArray dockStateBeforeCanvas = firstCanvas ? saveState() : QByteArray();

    auto *gl = new GLWidget(toolCfg, page);
    gl->setSmoothingStrength(pendingSmoothing_);
    gl->document().setMaxUndo(SettingsDlg::loadValues().undoHistoryLimit);
    // この時点ではまだスタートページを表示したままにしておく。表示への切り替えは
    // 呼び出し側がGLWidget::initializedへの接続を済ませてからpage->showCanvas()で行う
    // (CanvasTabPage::setCanvas()のコメント参照)。
    page->setCanvas(gl);
    updateTabLabel(page);
    // このキャンバスのGL初期化が終わった時点でもう一度塗り直す。ウィンドウ生成時
    // (WinIdChange)の塗り直しはGLの初回描画より前に走ってしまうため、それだけだと
    // 塗られないまま残る領域ができる。
    connect(gl, &GLWidget::initialized, this, [this, gl, firstCanvas, dockStateBeforeCanvas] {
        WINLOG(QStringLiteral("EVT  GLWidget::initialized  glFormat: alpha=%1 rgb=%2/%3/%4 swap=%5")
                   .arg(gl->format().alphaBufferSize())
                   .arg(gl->format().redBufferSize()).arg(gl->format().greenBufferSize())
                   .arg(gl->format().blueBufferSize()).arg(gl->format().swapInterval()));
        logWindowCompositionState(QStringLiteral("after-GL-init"));
        logDockLayout(QStringLiteral("after-GL-init"));
        scheduleNativeRepaint();
        if (firstCanvas)
            restoreDockLayoutAfterFirstCanvas(dockStateBeforeCanvas, 0);
    });
    return gl;
}

// 最初のキャンバスが出来た直後にドック配置が崩れていたら、作る前の配置へ戻す。
//
// 最初のキャンバス(=最初のQOpenGLWidget)が現れると、Qtはトップレベルのウィンドウを
// 作り直す。実測(TIEPOLO_WINLOG)では、GL初期化と初回paintGLの直後に
//   WM_WINDOWPOSCHANGING flags=FRAMECHANGED|SHOWWINDOW → WM_NCCALCSIZE → WM_SIZE
// が飛ぶ(2枚目以降のキャンバスでは飛ばない。実測: 2枚目のinitializeGLは20msで
// ウィンドウメッセージも無し)。ウィンドウ矩形自体は変わらないが、Qtには改めて
// リサイズが届くため、QMainWindowはドックの幅を配分し直す。
//
// このときloadSettings()の restoreState と同じ罠がある: 途中の小さいサイズを基準に
// 配分されるとドックが最小幅まで潰れ、QMainWindowは後から増えた幅を中央ウィジェット
// (=キャンバス)に全部渡すので、潰れたドックは潰れたまま残る。利用者から見ると
// 「キャンバスが最大化して左右のドックが最小化した」状態になる。
//
// 起動時は dockRestorePending_ の仕組みが貼り直してくれるが、あれは起動の1回で
// 終わるためここには効かない。そこでキャンバス生成の前後を比べ、変わっていたら戻す。
//
// 直後の1回では捕まらない(作り直しは初回paintGLの後に来る)ので、少し待って
// もう一度見る。変わっていなければ何もしないので、崩れない環境では完全に無変化。
void MainWindow::restoreDockLayoutAfterFirstCanvas(const QByteArray &before, int attempt)
{
    if (before.isEmpty()) return;

    if (saveState() != before) {
        WINLOG(QStringLiteral("CALL restoreDockLayoutAfterFirstCanvas -> 配置が変わっていたので戻す (attempt=%1)")
                   .arg(attempt));
        logDockLayout(QStringLiteral("first-canvas(before restore)"));
        restoreState(before);
        logDockLayout(QStringLiteral("first-canvas(after restore)"));
    }

    // GL初期化の直後から 0 / 250 / 600 / 1000ms の4回見る。ウィンドウの作り直しは
    // 初回paintGLより後に来るため、1回だけでは必ず取りこぼす
    // (この環境の実測では、GL初期化から250〜600ms後に
    //  「復元サイズへ縮む → WM_SIZE → 最大化サイズへ戻る」が起きる)。
    // 遅い環境でも入るよう最後は1秒まで見る。1秒を超えて監視し続けると、
    // 利用者が自分でドック幅を変えたのを戻してしまうのでここで打ち切る。
    static const int kDelaysMs[] = { 250, 350, 400 };
    if (attempt < (int)(sizeof(kDelaysMs) / sizeof(kDelaysMs[0]))) {
        const int delay = kDelaysMs[attempt];
        QTimer::singleShot(delay, this, [this, before, attempt] {
            restoreDockLayoutAfterFirstCanvas(before, attempt + 1);
        });
    }
}

void MainWindow::addNewTab()
{
    CanvasPane *pane = ensureRootPane();
    activateTabPage(createTabPage(pane));
    updateCentralPage();
}

// 新しいドキュメントを開く先のタブ。アクティブなタブがまだスタートページならそれを
// 再利用する(「タブを追加」直後にメニューから開いたときに空タブを残さないため)。
CanvasTabPage *MainWindow::targetTabPageForNewDocument()
{
    CanvasPane *pane = ensureRootPane();
    if (auto *cur = qobject_cast<CanvasTabPage *>(pane->tabWidget()->currentWidget()))
        if (!cur->isCanvas()) return cur;
    return createTabPage(pane);
}

void MainWindow::createWidgets() {
    // centralStackはペインツリー(paneTreeRoot_)を丸ごと差し替えるためだけの入れ物。
    // 分割/畳みでルートがCanvasPaneとQSplitterの間を行き来するので、QMainWindowの
    // centralWidgetを直接付け替える(=古い方が破棄される)のを避けている。
    // スタートページはもう独立したページではなく、各タブ(CanvasTabPage)の中にある。
    centralStack = new QStackedWidget(this);
    setCentralWidget(centralStack);
    // 最初のペインとタブはコンストラクタ側(setupActions()の後)で作る。

    // Dock類のコンストラクタはGLWidget*を要求するため、タブが1枚も無い間のダミーとして
    // 使う非表示GLWidgetを1つ用意する(どのペインのタブにも追加せず、表示もしないため
    // initializeGL()は走らず、GLリソースのコストは掛からない)。
    deckGLWidget_ = new GLWidget(toolCfg, this);
    deckGLWidget_->hide(); // 表示しない(initializeGL()を走らせないため。Dock構築用のダミー)

    toolDock       = new ToolDock(deckGLWidget_, shortcuts_, this);
    toolDockWidget = new QDockWidget("ツール", this);
    toolDockWidget->setObjectName("toolDockWidget");
    toolDockWidget->setAllowedAreas(Qt::AllDockWidgetAreas);
    toolDockWidget->setTitleBarWidget(new DockTitleBar("ツール", toolDockWidget));
    toolDockWidget->setWidget(toolDock);

    toolPropDock       = new ToolPropDock(deckGLWidget_, toolCfg, this);
    toolPropDockWidget = new QDockWidget("ツール設定", this);
    toolPropDockWidget->setObjectName("toolPropDockWidget");
    toolPropDockWidget->setAllowedAreas(Qt::AllDockWidgetAreas);
    toolPropDockWidget->setTitleBarWidget(new DockTitleBar("ツール設定", toolPropDockWidget));
    toolPropDockWidget->setWidget(toolPropDock);

    toolPresetDock       = new ToolPresetDock(deckGLWidget_, toolCfg, this);
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

    navigatorDock       = new NavigatorDock(deckGLWidget_, this);
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

    layerDock       = new LayerDock(deckGLWidget_, this);
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
void MainWindow::bindGLWidget(GLWidget *gl)
{
    for (const QMetaObject::Connection &c : glWidgetConnections_)
        QObject::disconnect(c);
    glWidgetConnections_.clear();

    glWidget = gl;

    glWidgetConnections_ << connect(glWidget, &GLWidget::initialized,    layerDock, &LayerDock::refresh);
    // layersChanged()はストローク確定の連打や不透明度スライダーのドラッグ等で短時間に
    // 連続発火しうる。直接refresh()(フルキャンバス合成を伴いうる重い処理)へ繋ぐと
    // 発火のたびに毎回それが走ってしまうため、各ドック側のデバウンス経由にする。
    glWidgetConnections_ << connect(glWidget, &GLWidget::layersChanged,  layerDock, &LayerDock::scheduleRefresh);
    glWidgetConnections_ << connect(glWidget, &GLWidget::layersChanged,  navigatorDock, &NavigatorDock::scheduleRefresh);
    glWidgetConnections_ << connect(glWidget, &GLWidget::modifiedChanged, this, &MainWindow::updateWindowTitle);
    // (clearSelectionActionはEscapeキーで変形系アクションのキャンセルも兼ねるため、
    // 選択範囲の有無に関わらず常に有効にしておく。選択が無い状態でのクリア自体は
    // clearSelection()内で安全に無視される)
    glWidgetConnections_ << connect(glWidget, &GLWidget::colorDropperd, this,
                                     [this](QColor c, bool transparent) {
        glWidget->returnToPreviousTool();
        // スポイトが拾うのは「RGB、または透明色」だけ。不透明度は現在の値を維持する
        // (アルファは通常色では濃さ、透明色では消す強さを表すブラシ側の設定であって、
        //  画面から拾ってくる性質の値ではない。DropperTool参照)。
        colorCircleDock->setTransparent(transparent);
        toolCfg->color().setTransparent(transparent);
        if (!transparent) {
            c.setAlpha(toolCfg->color().rawRGBA().alpha());
            toolCfg->color().setRawRGBA(c);
            colorCircleDock->pickColor(c);
        }
    });
    glWidgetConnections_ << connect(glWidget, &GLWidget::activeToolChanged, this, [this](ToolType tool) {
        toolDock->syncButton(tool);
        toolCfg->color().setRawRGBA(colorCircleDock->color());
        toolCfg->color().setTransparent(colorCircleDock->isTransparent());
        toolPropDock->setCurrentTool(tool);
        brushSizeDock->syncSize(brushSizeFor(toolCfg, tool));
    });
    glWidgetConnections_ << connect(glWidget, &GLWidget::activeToolPresetChanged, this, [this](ToolType tool, int) {
        // ToolType自体は変わらずツールプリセットだけ切り替わった場合、表示中の設定値を更新する
        toolPropDock->refreshFromSettings();
        brushSizeDock->syncSize(brushSizeFor(toolCfg, tool));
    });

    // 各Dockを新しいタブへretargetし、表示内容をそのタブの状態で作り直す
    toolDock->setGLWidget(glWidget);
    toolPropDock->setGLWidget(glWidget);
    toolPresetDock->setGLWidget(glWidget);
    layerDock->setGLWidget(glWidget);
    navigatorDock->setGLWidget(glWidget);
    brushSizeDock->syncSize(brushSizeFor(toolCfg, glWidget->getActiveTool()));
}

// ---------------------------------------------------------------------------
// 分割表示(複数ペイン)関連
// ---------------------------------------------------------------------------

// paneTreeRoot_(CanvasPane*またはQSplitter*)を再帰的に辿り、見つかったCanvasPaneを
// 全部集める(allCanvasWidgets()と、跡地に残ったペインを探すclosePaneIfEmpty()の
// 両方から使う共通ロジック)。
static void collectPanes(QWidget *node, QVector<CanvasPane *> &out)
{
    if (!node) return;
    if (auto *pane = qobject_cast<CanvasPane *>(node)) { out.append(pane); return; }
    if (auto *split = qobject_cast<QSplitter *>(node)) {
        for (int i = 0; i < split->count(); ++i)
            collectPanes(split->widget(i), out);
    }
}

// closePaneIfEmpty()が、閉じたペインの跡地に代わりにアクティブ化すべきペインを
// 探すためだけに使う(先頭に見つかったもの1つで十分)。
static CanvasPane *firstPaneIn(QWidget *node)
{
    QVector<CanvasPane *> panes;
    collectPanes(node, panes);
    return panes.isEmpty() ? nullptr : panes.first();
}

QVector<GLWidget *> MainWindow::allCanvasWidgets() const
{
    QVector<CanvasPane *> panes;
    collectPanes(paneTreeRoot_, panes);
    QVector<GLWidget *> result;
    for (CanvasPane *pane : panes) {
        QTabWidget *tabs = pane->tabWidget();
        for (int i = 0; i < tabs->count(); ++i) {
            auto *page = qobject_cast<CanvasTabPage *>(tabs->widget(i));
            if (!page) continue;
            // まだスタートページのままのタブはキャンバスを持たないので飛ばす
            if (GLWidget *gl = page->canvas()) result.append(gl);
        }
    }
    return result;
}

CanvasPane *MainWindow::createPane()
{
    auto *pane = new CanvasPane();
    connect(pane, &CanvasPane::currentTabChanged,   this, &MainWindow::onCurrentTabChanged);
    connect(pane, &CanvasPane::tabCloseRequested,   this, &MainWindow::onTabCloseRequested);
    connect(pane, &CanvasPane::splitRequested,      this, &MainWindow::splitPane);
    connect(pane, &CanvasPane::tabInsertRequested, this,
            [this](CanvasPane *destPane, QTabWidget *sourceTabs, int sourceIndex, int destIndex) {
                movePaneTab(destPane, destIndex, sourceTabs, sourceIndex);
            });
    return pane;
}

CanvasPane *MainWindow::ensureRootPane()
{
    if (paneTreeRoot_) {
        // 既存のツリーがあれば、通常はactivePane_が常に有効な追加先。万一(理論上
        // 起こらないはずだが)nullなら先頭のペインへ安全側にフォールバックする。
        if (activePane_) return activePane_;
        if (CanvasPane *p = firstPaneIn(paneTreeRoot_)) return p;
    }
    CanvasPane *pane = createPane();
    paneTreeRoot_ = pane;
    centralStack->addWidget(pane);
    return pane;
}

// 現在のGLWidgetへの接続を切り、各Dockをダミー(deckGLWidget_)へ戻す。
// アクティブなタブがまだスタートページのときの状態。
void MainWindow::unbindGLWidget()
{
    for (const QMetaObject::Connection &c : glWidgetConnections_)
        QObject::disconnect(c);
    glWidgetConnections_.clear();
    glWidget = nullptr;

    toolDock->setGLWidget(deckGLWidget_);
    toolPropDock->setGLWidget(deckGLWidget_);
    toolPresetDock->setGLWidget(deckGLWidget_);
    layerDock->setGLWidget(deckGLWidget_);
    navigatorDock->setGLWidget(deckGLWidget_);
}

// 操作権をpageへ移す単一の入口。所属ペインを特定してそのタブをカレントにし、
// アクティブペインの枠線を付け替えたうえで、キャンバスならbindGLWidget()、まだ
// スタートページならunbindGLWidget()する。ペイン内のタブ切替
// (CanvasPane::currentTabChanged)・ドラッグでのタブ移動・クリックによる操作権の
// 移動(eventFilter)のすべてがここへ集約される。
void MainWindow::activateTabPage(CanvasTabPage *page)
{
    // activatingTab_が立っている = 下のsetCurrentWidget()から再入してきた呼び出し。
    // 二重に処理せず、呼び出し元(外側)に続きを任せる。
    if (!page || activatingTab_) return;

    CanvasPane *pane = tabOwnerPane_.value(page);
    if (!pane) return; // 登録前(構築中)は無視

    if (pane->tabWidget()->currentWidget() != page) {
        activatingTab_ = true;
        pane->tabWidget()->setCurrentWidget(page); // currentChanged経由でここへ再入する
        activatingTab_ = false;
    }

    if (activePane_ && activePane_ != pane) activePane_->setActive(false);
    pane->setActive(true);
    activePane_ = pane;

    GLWidget *gl = page->canvas();
    if (!gl) {
        // このタブはまだスタートページか透過タブ。スタートページなら最近使ったファイル
        // 一覧を最新にしてから、キャンバス依存のUIをダミーへ逃がす。
        if (!page->isTransparent()) page->startPage()->refresh();
        unbindGLWidget();
        updateWindowTitle();
        updateTabRelatedActionsEnabled();
        return;
    }

    if (gl == glWidget) return; // 既にこのキャンバスがアクティブ

    if (gl->isGLReady()) {
        bindGLWidget(gl);
        updateWindowTitle();
    } else {
        // 作成直後でまだinitializeGL()が完了していないキャンバスの場合、完了を
        // 待ってから接続を張る。
        connect(gl, &GLWidget::initialized, this, [this, gl]() {
            if (glWidget == gl) return; // 待っている間に別経路で既にアクティブ化済み
            bindGLWidget(gl);
            updateWindowTitle();
        }, Qt::SingleShotConnection);
    }
    updateTabRelatedActionsEnabled();
}

void MainWindow::activatePane(CanvasPane *pane)
{
    if (!pane || pane == activePane_) return;
    activateTabPage(qobject_cast<CanvasTabPage *>(pane->tabWidget()->currentWidget()));
}

// paneの端(orientation/insertBeforeで方向を指定)へタブがドロップされたときの処理。
void MainWindow::splitPane(CanvasPane *pane, Qt::Orientation orientation, bool insertBefore,
                            QTabWidget *sourceTabs, int sourceIndex)
{
    if (!pane || !sourceTabs || sourceIndex < 0 || sourceIndex >= sourceTabs->count()) return;
    auto *movedPage = qobject_cast<CanvasTabPage *>(sourceTabs->widget(sourceIndex));
    if (!movedPage) return;
    // 自分自身の(唯一の)タブを自分自身へ分割しようとした場合、分割先が無くなって
    // しまうので何もしない。
    if (sourceTabs == pane->tabWidget() && sourceTabs->count() <= 1) return;

    // 計測用。起動後に初めて分割したときだけ10秒近くかかるという報告があるため、
    // どの段が重いのかを段階ごとに出す(TIEPOLO_WINLOG=1 のときだけ)。
    QElapsedTimer splitTimer;
    splitTimer.start();

    CanvasPane *newPane = createPane();
    const qint64 tCreatePane = splitTimer.elapsed();

    // 折りたたみ規則: paneの直接の親がすでに同じorientationのQSplitterなら、
    // 新規QSplitterで包まずそこへ兄弟として追加するだけにする(無駄な入れ子を作らない)。
    auto *parentSplitter = qobject_cast<QSplitter *>(pane->parentWidget());
    if (parentSplitter && parentSplitter->orientation() == orientation) {
        const int idx = parentSplitter->indexOf(pane);
        parentSplitter->insertWidget(insertBefore ? idx : idx + 1, newPane);
    } else {
        const bool paneWasRoot = (pane == paneTreeRoot_);
        const int  idx         = parentSplitter ? parentSplitter->indexOf(pane) : -1;
        auto *newSplitter = new QSplitter(orientation);

        // 必ず「先にpaneをnewSplitterへ移す」→「後からnewSplitterをツリーへ組み込む」の
        // 順で行う。逆順にすると、centralStack->removeWidget(pane)の時点でQStackedLayoutが
        // 外したページをhide()してしまい(=明示的な非表示フラグが立つ)、その後
        // QSplitter::addWidget()へ渡してもQSplitterは明示的に隠されたウィジェットを
        // 表示し直さないため、paneが見えないままになる。
        if (insertBefore) { newSplitter->addWidget(newPane); newSplitter->addWidget(pane); }
        else               { newSplitter->addWidget(pane);   newSplitter->addWidget(newPane); }
        pane->show();    // 上記の事情があるため、保険として明示的に表示状態へ戻しておく
        newPane->show();

        if (paneWasRoot) {
            // paneは既に上のaddWidget()でcentralStackから外れている(reparentに伴う
            // ChildRemovedでQStackedLayoutが自動的にページを削除する)。
            paneTreeRoot_ = newSplitter;
            centralStack->addWidget(newSplitter);
        } else if (parentSplitter) {
            parentSplitter->insertWidget(idx, newSplitter);
        }
    }

    const qint64 tTree = splitTimer.elapsed();

    CanvasPane *sourcePane = qobject_cast<CanvasPane *>(sourceTabs->parentWidget());

    sourceTabs->removeTab(sourceIndex);
    const qint64 tRemoveTab = splitTimer.elapsed();
    // ここでページ(GLWidgetを含む)が別ペインの子になる=reparent。QOpenGLWidgetは
    // 親が変わるとコンテキストを作り直すため、重いとすればこの1行の可能性が高い。
    newPane->tabWidget()->addTab(movedPage, QString());
    const qint64 tAddTab = splitTimer.elapsed();

    tabOwnerPane_.insert(movedPage, newPane);
    updateTabLabel(movedPage);
    newPane->tabWidget()->setCurrentWidget(movedPage);
    activateTabPage(movedPage); // closePaneIfEmpty()より必ず先に行う(activePane_のダングリング防止)
    const qint64 tActivate = splitTimer.elapsed();

    if (sourcePane) closePaneIfEmpty(sourcePane);
    const qint64 tCloseEmpty = splitTimer.elapsed();

    WINLOG(QStringLiteral("PERF splitPane total=%1ms | createPane=%2 tree=%3 removeTab=%4 "
                           "addTab(reparent)=%5 activate=%6 closeEmpty=%7")
               .arg(splitTimer.elapsed())
               .arg(tCreatePane).arg(tTree - tCreatePane).arg(tRemoveTab - tTree)
               .arg(tAddTab - tRemoveTab).arg(tActivate - tAddTab).arg(tCloseEmpty - tActivate));

    // paneTreeRoot_が新しいQSplitterに差し替わっている場合、centralStackへ
    // addWidget()しただけではカレントページにならない(むしろ元のページを外した
    // 時点でStartPage側へ切り替わってしまう)ため、明示的に貼り直す。
    updateCentralPage();
    WINLOG(QStringLiteral("PERF splitPane done (incl. updateCentralPage) = %1ms").arg(splitTimer.elapsed()));
}

// destPaneの中央/タブバーへタブがドロップされたときの処理(分割はしない)。
void MainWindow::movePaneTab(CanvasPane *destPane, int destIndex, QTabWidget *sourceTabs, int sourceIndex)
{
    if (!destPane || !sourceTabs || sourceIndex < 0 || sourceIndex >= sourceTabs->count()) return;
    auto *movedPage = qobject_cast<CanvasTabPage *>(sourceTabs->widget(sourceIndex));
    if (!movedPage) return;

    const bool samePane = (sourceTabs == destPane->tabWidget());
    if (samePane && sourceIndex == destIndex) return; // 無意味な自己ドロップ

    CanvasPane *sourcePane = qobject_cast<CanvasPane *>(sourceTabs->parentWidget());

    sourceTabs->removeTab(sourceIndex);
    int insertAt = destIndex;
    if (samePane && sourceIndex < destIndex) insertAt -= 1; // removeTab済みの分だけ後ろへずれる
    insertAt = qBound(0, insertAt, destPane->tabWidget()->count());

    destPane->tabWidget()->insertTab(insertAt, movedPage, QString());
    tabOwnerPane_.insert(movedPage, destPane);
    updateTabLabel(movedPage);
    destPane->tabWidget()->setCurrentWidget(movedPage);
    activateTabPage(movedPage); // closePaneIfEmpty()より必ず先に行う(activePane_のダングリング防止)

    if (!samePane && sourcePane) closePaneIfEmpty(sourcePane);
}

// paneのタブが0枚になったら、親から取り除いて削除する。親QSplitterの子が1個に
// なったら、その1個を親の位置へ直接差し込んでスプリッター自体を畳む(スプリッターは
// 常に子2個以上、という不変条件により1段だけの操作で済む)。paneがpaneTreeRoot_
// 自体(=ペインがそれ1枚しか無かった)場合は、ペインを消さず新しいスタートページの
// タブを1枚作って戻す(中央には常にタブが1枚以上ある)。
void MainWindow::closePaneIfEmpty(CanvasPane *pane)
{
    if (!pane || pane->tabWidget()->count() > 0) return;

    if (pane == paneTreeRoot_) {
        // 最後のタブが閉じられた。ペインごと消すと中央が空になってしまうので、
        // 代わりにスタートページのタブを1枚入れ直す(旧・タブ0枚時のStartPage表示に
        // 相当する状態)。activateTabPage()側でglWidgetのnullptr化とDockの
        // ダミーへのretargetも行われる。
        activateTabPage(createTabPage(pane));
        updateCentralPage();
        return;
    }

    auto *parentSplitter = qobject_cast<QSplitter *>(pane->parentWidget());
    if (!parentSplitter) { pane->deleteLater(); return; } // 通常起こらない

    pane->setParent(nullptr); // parentSplitterから外す
    pane->deleteLater();

    if (parentSplitter->count() == 1) {
        QWidget *remaining = parentSplitter->widget(0);
        auto *grandParentSplitter = qobject_cast<QSplitter *>(parentSplitter->parentWidget());
        if (parentSplitter == paneTreeRoot_) {
            // remainingをcentralStackへ移すと、その時点でparentSplitterからは自動的に
            // 外れる。QStackedLayoutは追加したページをいったんhide()するので、
            // 表示はこの関数末尾のupdateCentralPage()に任せる。
            paneTreeRoot_ = remaining;
            centralStack->addWidget(remaining);
            centralStack->removeWidget(parentSplitter);
        } else if (grandParentSplitter) {
            const int idx = grandParentSplitter->indexOf(parentSplitter);
            grandParentSplitter->insertWidget(idx, remaining); // remainingを差し込む(自動的にparentSplitterから外れる)
        }
        parentSplitter->deleteLater();
    }

    if (activePane_ == pane) {
        activePane_ = nullptr;
        if (CanvasPane *replacement = firstPaneIn(paneTreeRoot_))
            activateTabPage(qobject_cast<CanvasTabPage *>(replacement->tabWidget()->currentWidget()));
    }

    // スプリッターを畳んでpaneTreeRoot_が差し替わった場合に、centralStackの
    // カレントページを新しいルートへ貼り直す(splitPane()末尾と同じ理由)。
    updateCentralPage();
}

// centralStackの表示ページを現在のペインツリーへ合わせ、キャンバス依存アクションの
// 有効/無効を揃える。分割・畳みでpaneTreeRoot_がCanvasPaneとQSplitterの間を行き来
// するため、そのたびにここで貼り直す(centralStack->addWidget()しただけでは
// カレントページにならず、QStackedLayoutは追加したページをいったんhide()する)。
// Dock類は常に表示したままにする(キャンバスが無い間はダミーのdeckGLWidget_を
// 対象にしているだけ)。以前はDockごと非表示にしていたが、QDockWidgetを
// 非表示→再表示するとQtが配置をsizeHint基準に巻き戻してしまい、保存したはずの
// ドック配置が復元されないように見える不具合があったため。
void MainWindow::updateCentralPage()
{
    WINLOG(QStringLiteral("CALL updateCentralPage"));
    if (paneTreeRoot_) centralStack->setCurrentWidget(paneTreeRoot_);
    updateTabRelatedActionsEnabled();
    updateWindowMask();
    // ペインの分割・畳み・タブの増減で構成が変わったぶんを塗り直す。ネイティブ
    // ウィンドウは作り直されないので、これをしないと元の配置のまま塗られていない
    // 領域が透明として残る(repaintNativeWindow()のコメント参照)。
    scheduleNativeRepaint();
}

// ---------------------------------------------------------------------------
// フレームレスウィンドウ(タイトルバー無し + メニューバー右端のキャプションボタン)
// ---------------------------------------------------------------------------

void MainWindow::setupCaptionButtons()
{
    auto *bar = new QWidget(menuBar());
    bar->setObjectName("captionButtons");
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto makeButton = [&](const QString &glyph, const QString &tip, const char *name) {
        auto *btn = new QToolButton(bar);
        btn->setObjectName(name);
        btn->setText(glyph);
        btn->setToolTip(tip);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setAutoRaise(true);
        layout->addWidget(btn);
        return btn;
    };

    QToolButton *minBtn = makeButton(QStringLiteral("─"), "最小化", "captionMinimize");
    connect(minBtn, &QToolButton::clicked, this, &QWidget::showMinimized);

    maximizeBtn_ = makeButton(QStringLiteral("□"), "最大化", "captionMaximize");
    connect(maximizeBtn_, &QToolButton::clicked, this, [this] {
        if (isMaximized()) showNormal();
        else               showMaximized();
    });

    QToolButton *closeBtn = makeButton(QStringLiteral("✕"), "閉じる", "captionClose");
    connect(closeBtn, &QToolButton::clicked, this, &QWidget::close);

    menuBar()->setCornerWidget(bar, Qt::TopRightCorner);
    updateMaximizeButton();
}

void MainWindow::updateMaximizeButton()
{
    if (!maximizeBtn_) return;
    const bool max = isMaximized();
    maximizeBtn_->setText(max ? QStringLiteral("❐") : QStringLiteral("□"));
    maximizeBtn_->setToolTip(max ? "元のサイズに戻す" : "最大化");
}

void MainWindow::setAlwaysOnTop(bool on)
{
    if (QWindow *handle = windowHandle()) {
        // 表示後はQWindow側でフラグを差し替える。QWidget::setWindowFlag()を使うと
        // 一度hide()されてネイティブウィンドウが作り直され、setMask()で開けた穴
        // (透過タブ)が失われてしまうため。
        Qt::WindowFlags f = handle->flags();
        f.setFlag(Qt::WindowStaysOnTopHint, on);
        handle->setFlags(f);
        updateWindowMask(); // 念のため貼り直す
    } else {
        setWindowFlag(Qt::WindowStaysOnTopHint, on); // まだ表示前(コンストラクタ)
    }
    QSettings().setValue("window/alwaysOnTop", on);
}

// ドック配置の貼り直しを予約する。
//
// 起動直後のウィンドウサイズは 764 -> 1536 -> 778 -> 1536 のように何度も変わる
// (最大化の適用、全画面フラグの除去など)。途中の小さいサイズを基準に貼ると
// ドックがまた最小幅へ潰れてしまうため、「変化が収まってから1回だけ」貼る。
//
// 世代番号で追い越しを判定する単純なデバウンス。新しい要求が来たら古い予約は
// 何もせずに捨てられるので、最後の1回だけが実際に貼り直す。
void MainWindow::scheduleDockLayoutRestore()
{
    if (!dockRestorePending_ || savedDockState_.isEmpty()) return;

    // 最大化での起動なら「最終形」がはっきり分かる: 全画面フラグが無く、
    // ジオメトリが作業領域とぴったり一致した状態。そこまで来たら待つ意味が無いので
    // すぐ貼る。デバウンスの300msを待っていると、その間にキャンバスの初回GL初期化
    // (シェーダーのコンパイル)がイベントループを数秒止めるため、貼り直しが起動から
    // 5秒近く後にずれ込んで、レイアウトが後から動くのが見えてしまっていた。
    // 最大化かどうかはOS側に聞く。最大化中はQtのwindowState()に全画面フラグが
    // 混じる(MainWindow::event()のWindowStateChangeのコメント参照)ため、
    // isMaximized() && !isFullScreen() では永久に成立しなくなる。
    const bool settledMaximized =
        isVisible() && osIsMaximized()
        && screen() && geometry() == screen()->availableGeometry();

    if (settledMaximized) {
        // タイマーに逃がさず、その場で貼る。singleShot(0)でも、直後に始まる
        // キャンバスの初回GL初期化がイベントループを数秒止めるため、実行が
        // その後ろへ回ってしまう(実測: 確定は1737ms、貼り直しは5037ms)。
        dockRestorePending_ = false;
        ++dockRestoreGeneration_; // 予約済みのデバウンスを無効化する
        WINLOG(QStringLiteral("CALL scheduleDockLayoutRestore -> サイズ確定を検出したので即座に貼り直す"));
        restoreState(savedDockState_);
        logDockLayout(QStringLiteral("after immediate restoreState"));
        return;
    }

    const int gen = ++dockRestoreGeneration_;
    QTimer::singleShot(300, this, [this, gen] {
        if (!dockRestorePending_ || gen != dockRestoreGeneration_) return; // 追い越された
        if (!isVisible()) return; // 表示前は実サイズが決まらないので待つ
        dockRestorePending_ = false;
        WINLOG(QStringLiteral("CALL scheduleDockLayoutRestore -> サイズ確定後に貼り直す"));
        restoreState(savedDockState_);
        logDockLayout(QStringLiteral("after deferred restoreState"));
    });
}

// 診断用: ドック配置が復元されない件を追うため、ウィンドウと各ドックの実サイズを
// 1行で出す。restoreState()はそのときのウィンドウサイズを基準に配分を決めるので、
// 「いつ・どのサイズで」適用されたかが分からないと原因が特定できない。
// TIEPOLO_WINLOG 未設定なら何もしない。
void MainWindow::logDockLayout(const QString &tag)
{
    if (!WinLog::enabled()) return;
    auto d = [](QDockWidget *dw, const char *name) {
        if (!dw) return QStringLiteral("%1=null").arg(name);
        return QStringLiteral("%1=%2x%3%4").arg(name).arg(dw->width()).arg(dw->height())
                   .arg(dw->isVisible() ? QString() : QStringLiteral("(hidden)"));
    };
    // メニューバーと中央ウィジェットは「位置」も出す。ドックが潰れる不具合とは別に、
    // 起動直後だけメニューバーが高さ0のまま(=中身が上へずれる)ことがあり、
    // 大きさだけ見ていても気づけないため。
    const QRect mb = menuBar() ? menuBar()->geometry() : QRect();
    const QRect cw = centralWidget() ? centralWidget()->geometry() : QRect();
    WINLOG(QStringLiteral("DOCK %1: win=%2x%3 menu=%4,%5 %6x%7%8 central=%9,%10 %11x%12")
               .arg(tag).arg(width()).arg(height())
               .arg(mb.x()).arg(mb.y()).arg(mb.width()).arg(mb.height())
               .arg(menuBar() && menuBar()->isVisible() ? QString() : QStringLiteral("(hidden)"))
               .arg(cw.x()).arg(cw.y()).arg(cw.width()).arg(cw.height()));
    WINLOG(QStringLiteral("DOCK %1: %2 %3 %4 %5 %6 %7 %8")
               .arg(tag)
               .arg(d(navigatorDockWidget,   "nav"))
               .arg(d(toolPresetDockWidget,  "preset"))
               .arg(d(toolPropDockWidget,    "prop"))
               .arg(d(brushSizeDockWidget,   "brush"))
               .arg(d(toolDockWidget,        "tools"))
               .arg(d(colorCircleDockWidget, "color"))
               .arg(d(layerDockWidget,       "layer")));
}

// clientPos(このウィンドウのクライアント座標系・論理px)が「タイトルバー相当」として
// 扱ってよい場所かを返す。WM_NCHITTESTからHTCAPTIONを返すかどうかの判定に使う
// (MainWindowNative.cpp参照)。メニュー項目やキャプションボタンの上は、そちらの
// クリックを優先したいので対象外にする。
bool MainWindow::isWindowDragArea(const QPoint &clientPos) const
{
    QMenuBar *menu = menuBar();
    if (!menu || !menu->isVisible()) return false;
    if (!menu->geometry().contains(clientPos)) return false;

    const QPoint inMenu = clientPos - menu->geometry().topLeft();
    if (menu->actionAt(inMenu)) return false;
    if (QWidget *corner = menu->cornerWidget(Qt::TopRightCorner))
        if (corner->geometry().contains(inMenu)) return false;
    return true;
}

// 表示中の透過タブのぶんだけウィンドウに穴を開ける。分割していれば複数の穴が空く。
void MainWindow::updateWindowMask()
{
    QVector<QRect> holes;
    QVector<CanvasPane *> panes;
    collectPanes(paneTreeRoot_, panes);
    for (CanvasPane *pane : panes) {
        auto *page = qobject_cast<CanvasTabPage *>(pane->tabWidget()->currentWidget());
        if (!page || !page->isTransparent() || !page->isVisible()) continue;
        // ページの矩形をこのウィンドウのクライアント座標系(論理px)へ移す。
        // 物理pxへの換算とウィンドウ枠ぶんのオフセットはapplyWindowRegion()が行う。
        holes.append(QRect(page->mapTo(this, QPoint(0, 0)), page->size()));
    }
    WINLOG(QStringLiteral("CALL updateWindowMask panes=%1 holes=%2").arg(panes.size()).arg(holes.size()));
    applyWindowRegion(holes);
}

// キャンバス(=アクティブなタブ)が無いと意味を成さない/クラッシュするアクションを
// まとめて有効/無効にする。新規作成・開く・設定・ヘルプ等はタブの有無に関わらず常に有効。
void MainWindow::updateTabRelatedActionsEnabled()
{
    const bool hasTab = (glWidget != nullptr);

    saveAction->setEnabled(hasTab);
    saveAsAction->setEnabled(hasTab);
    exportAction->setEnabled(hasTab);
    addImageLayerAction->setEnabled(hasTab);
    cutAction->setEnabled(hasTab);
    fitAction->setEnabled(hasTab);
    zoomInAction->setEnabled(hasTab);
    zoomOutAction->setEnabled(hasTab);
    viewFlipXAction->setEnabled(hasTab);
    nudgeUpAction->setEnabled(hasTab);
    nudgeLeftAction->setEnabled(hasTab);
    nudgeDownAction->setEnabled(hasTab);
    nudgeRightAction->setEnabled(hasTab);
    undoAction->setEnabled(hasTab);
    redoAction->setEnabled(hasTab);
    copyAction->setEnabled(hasTab);
    pasteAction->setEnabled(hasTab);
    selectAllAction->setEnabled(hasTab);
    clearSelectionAction->setEnabled(hasTab);
    transformAction->setEnabled(hasTab);
    confirmTransformAction->setEnabled(hasTab);
    freeTransformAction->setEnabled(hasTab);
    hueSatLightAction->setEnabled(hasTab);
    brightnessContrastAction->setEnabled(hasTab);
    colorBalanceAction->setEnabled(hasTab);
    toneCurveAction->setEnabled(hasTab);
    canvasSizeAction->setEnabled(hasTab);
    imageResolutionAction->setEnabled(hasTab);
    rotateCanvas90Action->setEnabled(hasTab);
    rotateCanvas180Action->setEnabled(hasTab);
    rotateCanvas270Action->setEnabled(hasTab);
    flipCanvasHorizontalAction->setEnabled(hasTab);
    flipCanvasVerticalAction->setEnabled(hasTab);
    gaussianBlurFilterAction->setEnabled(hasTab);
    customShaderFilterAction->setEnabled(hasTab);
    mosaicFilterAction->setEnabled(hasTab);
    motionBlurFilterAction->setEnabled(hasTab);
    noiseFilterAction->setEnabled(hasTab);
    chromaticAberrationFilterAction->setEnabled(hasTab);
    lensBlurFilterAction->setEnabled(hasTab);
    gradientMapAction->setEnabled(hasTab);
    brushBiggerAction->setEnabled(hasTab);
    brushSmallerAction->setEnabled(hasTab);

    newLayerAction->setEnabled(hasTab);
    newClippingLayerAction->setEnabled(hasTab);
    newSolidColorLayerAction->setEnabled(hasTab);
    newAdjustmentLayerAction->setEnabled(hasTab);
    newTextLayerAction->setEnabled(hasTab);
    duplicateLayerAction->setEnabled(hasTab);
    mergeLayerAction->setEnabled(hasTab);
    deleteLayerMenuAction->setEnabled(hasTab);
}

// ---- カラーモード/モニターキャリブレーション(見た目だけのプレビュー) -----

void MainWindow::setColorMode(ColorMode m)
{
    toolCfg->colorMode().setMode(m);
    QAction *checked = colorModeRgbAction;
    if (m == ColorMode::CMYK) checked = colorModeCmykAction;
    else if (m == ColorMode::GrayscaleLuminance) checked = colorModeGrayLuminanceAction;
    else if (m == ColorMode::GrayscaleLightness) checked = colorModeGrayLightnessAction;
    checked->setChecked(true);
    propagateDisplayConfigToAllTabs();
    colorCircleDock->setColorMode(m);
}

void MainWindow::openCalibrationDialog()
{
    if (!calibrationDlg) {
        calibrationDlg = new CalibrationDlg(this);
        connect(calibrationDlg, &CalibrationDlg::valuesChanged, this, &MainWindow::setCalibration);
    }
    const CalibrationConfig &cal = toolCfg->calibration();
    calibrationDlg->setValues(cal.brightness(), cal.contrast(), cal.cyan(), cal.magenta(), cal.yellow());
    calibrationDlg->show();
    calibrationDlg->raise();
    calibrationDlg->activateWindow();
}

void MainWindow::setCalibration(int brightness, int contrast, int cyan, int magenta, int yellow)
{
    CalibrationConfig &cal = toolCfg->calibration();
    cal.setBrightness(brightness);
    cal.setContrast(contrast);
    cal.setCyan(cyan);
    cal.setMagenta(magenta);
    cal.setYellow(yellow);
    propagateDisplayConfigToAllTabs();
    colorCircleDock->setCalibration(brightness, contrast, cyan, magenta, yellow);
}

void MainWindow::propagateDisplayConfigToAllTabs()
{
    for (GLWidget *gl : allCanvasWidgets())
        gl->applyDisplayConfig();
}

// ---- UIテーマ --------------------------------------------------------------

void MainWindow::setUiTheme(Theme::Name n)
{
    Theme::setTheme(n);
    Theme::applyToApplication(*qobject_cast<QApplication *>(QApplication::instance()));

    (n == Theme::Name::Dark ? themeDarkAction : themeLightAction)->setChecked(true);

    // QSSの再適用だけではDraggablePanel::paintEvent()等、Theme::を直接参照する
    // QPainter描画は自動で再描画されないため、明示的に全ウィジェットへupdate()する。
    const auto widgets = QApplication::allWidgets();
    for (QWidget *w : widgets) w->update();

    QSettings().setValue("ui/theme", (int)n);
}

// ---- キャンバス外側の背景色 --------------------------------------------------

void MainWindow::chooseCanvasBackgroundColor()
{
    const QColor current = toolCfg->canvasBackground().color();
    // ネイティブのWindows色選択ダイアログは、環境によって(マルチモニタ構成等)
    // ジオメトリ設定に失敗しダイアログが壊れた状態になることがあるため、
    // Qt自前描画のダイアログを強制する。
    const QColor picked = QColorDialog::getColor(current, this, "キャンバス背景色を選択",
                                                   QColorDialog::DontUseNativeDialog);
    if (!picked.isValid()) return;
    setCanvasBackgroundColor(picked);
}

void MainWindow::setCanvasBackgroundColor(const QColor &c)
{
    toolCfg->canvasBackground().setColor(c);
    propagateDisplayConfigToAllTabs();
    QSettings().setValue("display/canvasBgColor", c);
}

void MainWindow::connectSignals() {
    // ---- Dock間の接続(いずれもglWidgetに依存しないので、タブ切替に関係なく
    // アプリ生存中ずっと固定でよい。glWidget依存の接続はbindGLWidget()側にある) ----
    connect(toolDock, &ToolDock::toolChanged, this, [this](ToolType curr) {
        toolCfg->color().setRawRGBA(colorCircleDock->color());
        toolCfg->color().setTransparent(colorCircleDock->isTransparent());
        brushSizeDock->syncSize(brushSizeFor(toolCfg, curr));
        toolPropDock->setCurrentTool(curr);
    });
    connect(toolPropDock, &ToolPropDock::sizeChanged, this, [this]() {
        // タブが1枚も無い間はglWidgetがnullptrになるため、その間はダミーの
        // deckGLWidget_を使う(ToolDockのアクティブツールもタブ0枚の間は
        // deckGLWidget_側に反映されているので、これで正しい値が取れる)。
        GLWidget *gl = glWidget ? glWidget : deckGLWidget_;
        ToolType curr = gl->getActiveTool();
        brushSizeDock->syncSize(brushSizeFor(toolCfg, curr));
        gl->updateCursor();
    });
    connect(brushSizeDock, &BrushSizeDock::brushSizeChanged, this, [this](int px) {
        GLWidget *gl = glWidget ? glWidget : deckGLWidget_;
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
void MainWindow::importPenSettingsFromAbr()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "ブラシ設定を読み込み", QString(),
        "Photoshopブラシ (*.abr);;すべてのファイル (*)");
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "読み込み失敗", "ファイルを開けませんでした。");
        return;
    }
    const AbrReadResult res = AbrCodec::read(f.readAll());

    // 未知のキーを後から調べられるよう、記述子の中身をログへ落としておく
    // (対応付けはファイル側の公開仕様が無く推定を含むため)。
    if (WinLog::enabled()) {
        WINLOG(QStringLiteral("ABR read: version=%1.%2 brushes=%3 %4")
                   .arg(res.version).arg(res.subversion).arg(res.brushes.size())
                   .arg(res.ok ? QString() : "error: " + res.error));
        for (const QString &line : res.dumpLines) WINLOG("ABR   " + line);
    }

    if (!res.ok || res.brushes.isEmpty()) {
        QMessageBox::warning(this, "読み込み失敗",
            QStringLiteral("このファイルからブラシ設定を読み取れませんでした。\n%1").arg(res.error));
        return;
    }

    // 複数本入っていることが多いので、どれを取り込むか選ばせる。
    int index = 0;
    if (res.brushes.size() > 1) {
        QStringList names;
        for (int i = 0; i < res.brushes.size(); i++) {
            const AbrBrush &b = res.brushes[i];
            names << QStringLiteral("%1: %2%3").arg(i + 1)
                         .arg(b.name.isEmpty() ? QStringLiteral("(名前なし)") : b.name)
                         .arg(b.tip.isNull() ? QString()
                                             : QStringLiteral("  [先端画像 %1×%2]")
                                                   .arg(b.tip.width()).arg(b.tip.height()));
        }
        bool okPressed = false;
        const QString chosen = QInputDialog::getItem(this, "ブラシを選択",
            QStringLiteral("%1本のブラシが入っています。取り込むものを選んでください:")
                .arg(res.brushes.size()),
            names, 0, false, &okPressed);
        if (!okPressed) return;
        index = qMax(0, names.indexOf(chosen));
    }

    QStringList notes = res.notes;
    notes += AbrPenMapping::applyToPen(res.brushes[index], toolCfg->pen());

    toolPropDock->refreshFromSettings();
    brushSizeDock->syncSize(toolCfg->pen().size());
    if (glWidget) glWidget->updateCursor();

    QString msg = QStringLiteral("「%1」をペンの設定へ取り込みました。")
                      .arg(res.brushes[index].name.isEmpty() ? "(名前なし)" : res.brushes[index].name);
    if (!notes.isEmpty()) msg += "\n\n・" + notes.join("\n・");
    QMessageBox::information(this, "読み込み完了", msg);
}

void MainWindow::exportPenSettingsToAbr()
{
    QString path = QFileDialog::getSaveFileName(
        this, "ブラシ設定を書き出し", QStringLiteral("brush.abr"),
        "Photoshopブラシ (*.abr)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".abr", Qt::CaseInsensitive)) path += ".abr";

    const QString name = QFileInfo(path).completeBaseName();
    const AbrBrush brush = AbrPenMapping::fromPen(toolCfg->pen(), name);
    const QByteArray data = AbrCodec::write(brush);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
        QMessageBox::warning(this, "書き出し失敗", "ファイルを書き込めませんでした。");
        return;
    }
    f.close();

    QString msg = QStringLiteral("「%1」を書き出しました。").arg(name);
    msg += brush.tip.isNull()
             ? QStringLiteral("\n(手続き的な丸ブラシとして出力)")
             : QStringLiteral("\n(先端画像 %1×%2 を含む)").arg(brush.tip.width()).arg(brush.tip.height());
    const QStringList lost = AbrPenMapping::unmappedFromPen(toolCfg->pen());
    if (!lost.isEmpty())
        msg += QStringLiteral("\n\n次の設定は.abrに対応する項目が無いため含まれません:\n・%1")
                   .arg(lost.join("\n・"));
    QMessageBox::information(this, "書き出し完了", msg);
}

void MainWindow::exportImage()
{
    QDir exportDir(SettingsDlg::loadValues().exportSaveDir);
    if (!exportDir.exists()) {
        exportDir.mkpath(".");
    }

    QString docName = "Canvas";
    if (!glWidget->filePath().isEmpty()) {
        QFileInfo projectFileInfo(glWidget->filePath());
        docName = projectFileInfo.baseName();
    }

    QString candidatePath = exportDir.filePath(docName + "_out" + ".png");
    QString defaultPath = getUniqueFilePath(candidatePath, 2);

    QString selectedFilter;
    QString selectedPath = QFileDialog::getSaveFileName(
        this,
        "画像として書き出し",
        defaultPath,
        "PNG 画像 (*.png);;JPEG 画像 (*.jpg *.jpeg);;Photoshop 形式 (*.psd);;すべてのファイル (*)",
        &selectedFilter
    );

    if (selectedPath.isEmpty()) return;

    QString finalPath = selectedPath;

    // 拡張子が省略されている場合は、選んだフィルタに応じて補う
    if (QFileInfo(finalPath).suffix().isEmpty()) {
        if (selectedFilter.contains("*.psd")) finalPath += ".psd";
        else if (selectedFilter.contains("*.jpg")) finalPath += ".jpg";
        else finalPath += ".png";
    }

    if (QFileInfo(finalPath).suffix().compare("psd", Qt::CaseInsensitive) == 0) {
        PsdCodec codec(glWidget);
        if (!codec.save(finalPath)) {
            QMessageBox::warning(this, "書き出し失敗", codec.lastError());
            return;
        }
        return;
    }

    if (!glWidget->exportToImage(finalPath)) {
        QMessageBox::warning(this, "書き出し失敗", "画像の書き出しに失敗しました。");
        return;
    }
}

void MainWindow::saveFile()
{
    if (glWidget->filePath().isEmpty()) {
        saveFileAs();
        return;
    }

    CanvasSerializer serializer(glWidget);
    if (!serializer.save(glWidget->filePath())) {
        QMessageBox::warning(this, "保存失敗", serializer.lastError());
        return;
    }
    glWidget->markSaved();
    RecentFiles::touch(glWidget->filePath());
    updateWindowTitle();
}
 
void MainWindow::saveFileAs()
{
    QDir projectDir(SettingsDlg::loadValues().projectSaveDir);
    if (!projectDir.exists()) {
        projectDir.mkpath(".");
    }

    QString candidatePath = projectDir.filePath("Canvas.tplo");
    QString defaultPath = getUniqueFilePath(candidatePath, 3); // ここで untitled_01.tplo などになる

    QString path = QFileDialog::getSaveFileName(
        this, 
        "名前を付けて保存",
        defaultPath,
        "Tiepolo ファイル (*.tplo)"
    );
    if (path.isEmpty()) return;

    QString finalPath = path;

    CanvasSerializer serializer(glWidget);
    if (!serializer.save(finalPath)) {
        QMessageBox::warning(this, "保存失敗", serializer.lastError());
        return;
    }

    glWidget->setFilePath(finalPath);
    glWidget->markSaved();
    RecentFiles::touch(finalPath);
    updateWindowTitle();
}

// 「画像を追加」: 画像ファイルを選び、現在開いているドキュメントの
// 現在選択中のレイヤーの直上に新規レイヤーとして挿入する
// (loadFile()と違い、新しいタブ/ドキュメントは作らない)。
void MainWindow::addImageLayer()
{
    if (!glWidget || glWidget->document().layerCount() == 0) return;

    const QString path = QFileDialog::getOpenFileName(
        this, "画像を追加",
        QString(),
        "画像ファイル (*.png *.jpg *.jpeg *.bmp)"
    );
    if (path.isEmpty()) return;

    const QImage image(path);
    if (image.isNull() || !glWidget->insertImageLayerAboveActive(image, QFileInfo(path).completeBaseName())) {
        QMessageBox::warning(this, "読み込み失敗", "画像ファイルを読み込めませんでした。");
        return;
    }
}

void MainWindow::loadFile()
{
    loadFileIntoTab(nullptr); // 開くパスが確定してから追加先タブを決める
}

// pageがnullptrなら、パスが確定した時点でtargetTabPageForNewDocument()に決めさせる
// (キャンセルされたときに空のタブを作ってしまわないようにするため)。
void MainWindow::loadFileIntoTab(CanvasTabPage *page)
{
    QString path = QFileDialog::getOpenFileName(
        this, "ファイルを開く",
        QString(),
        "対応ファイル (*.tplo *.psd *.png *.jpg *.jpeg *.bmp);;"
        "Tiepolo ファイル (*.tplo);;Photoshop 形式 (*.psd);;"
        "画像ファイル (*.png *.jpg *.jpeg *.bmp)"
    );
    if (path.isEmpty()) return;

    if (!page) page = targetTabPageForNewDocument();
    openFileIntoTab(page, path);
}

void MainWindow::openFilesFromArgs(const QStringList &paths)
{
    for (const QString &p : paths) {
        if (p.isEmpty() || !QFileInfo::exists(p)) continue;
        openFileIntoNewTab(p);
    }
}

void MainWindow::openFileIntoNewTab(const QString &path)
{
    openFileIntoTab(targetTabPageForNewDocument(), path);
}

// pageをキャンバスに変えてpathを読み込む。スタートページのカードのクリック
// (=そのタブ自身をキャンバスにする)と、メニューの「開く」の両方から使う。
void MainWindow::openFileIntoTab(CanvasTabPage *page, const QString &path)
{
    if (!page) return;
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool isPsd = suffix == "psd";
    const bool isImage = suffix == "png" || suffix == "jpg" || suffix == "jpeg" || suffix == "bmp";

    // GLWidgetは作られた直後はinitializeGL()完了(=シェーダーコンパイル・テクスチャ確保
    // 完了)が非同期なため、GLWidget::initialized を待ってから実際の読み込みを行う。
    GLWidget *gl = ensureCanvas(page);
    connect(gl, &GLWidget::initialized, gl, [this, gl, path, isPsd, isImage]() {
        // GLWidget::initialized はinitializeGL()の最後(=Qtが内部でこのウィジェットの
        // 初回paint/exposeイベントを処理している最中)に同期的にemitされる。この
        // コールスタックの中でQCoreApplication::processEvents()を呼んでイベント
        // ループを再入させると、initializeGL/paintGLへの再入(GLコンテキストの
        // 競合)で応答なし・クラッシュを招く。QTimer::singleShot(0, ...)で次の
        // イベントループの繰り返しまで遅延させ、Qtの内部処理を抜けてから実行する。
        QTimer::singleShot(0, gl, [this, gl, path, isPsd, isImage]() {
        // 進捗表示中にQCoreApplication::processEvents()でイベントループを回すと、
        // 他のタブ(や自分自身)のGLWidgetへ塗り直しイベントが配送されうる。
        // codec.load()/serializer.load()は「1つのGLコンテキストを掴んだまま
        // 一連のGL呼び出しを行う」ことを前提にしており、その最中にpaintGL()が
        // 割り込むとコンテキスト状態が壊れて応答なし/クラッシュにつながる。
        // 読み込みが終わるまで全タブのGLWidgetの再描画を止めて、これを防ぐ。
        struct DisableGLRepaintGuard {
            QVector<GLWidget*> widgets;
            explicit DisableGLRepaintGuard(const QVector<GLWidget *> &all) {
                for (GLWidget *w : all) {
                    widgets.append(w);
                    w->setUpdatesEnabled(false);
                }
            }
            ~DisableGLRepaintGuard() {
                for (GLWidget *w : widgets) w->setUpdatesEnabled(true);
            }
        } noRepaint(allCanvasWidgets());

        // 画像1枚(png/jpg等)はレイヤー展開が無く基本軽いため対象外。tplo/psdは
        // レイヤー数・タイル数に比例して重くなり得るため進捗バーを出す。
        // setMinimumDurationにより、実際に指定時間以上かかった場合だけ表示される
        // (短時間で終わればちらつかせず一切出ない)。
        QProgressDialog progress("ファイルを読み込んでいます…", QString(), 0, 100, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(500);
        progress.setValue(0);
        // 毎レイヤー呼ばれうるので、イベントループを回す(processEvents)のは
        // 一定間隔おきに間引く(応答性は保ちつつ、無駄なオーバーヘッドを避ける)。
        QElapsedTimer pumpTimer;
        pumpTimer.start();
        auto reportProgress = [&progress, &pumpTimer](int current, int total) {
            if (total <= 0) return;
            // 間引きの判定はsetValue()より前に行う必要がある。QProgressDialogが
            // モーダル(上でWindowModalにしている)のとき、setValue()は内部で
            // processEvents()を呼ぶため、setValue()を毎回通してしまうと
            // 「processEvents()だけ間引く」つもりが実際には全く間引かれず、
            // タイル1枚ごとにイベントループが回って読み込み時間の大半を食う。
            // 完了時(current==total)だけは必ず反映して表示を100%にする。
            if (current < total && pumpTimer.elapsed() < 60) return;
            pumpTimer.restart();
            progress.setMaximum(total);
            progress.setValue(current);
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        };

        if (isPsd) {
            PsdCodec codec(gl);
            codec.progressCallback = reportProgress;
            if (!codec.load(path)) {
                QMessageBox::warning(this, "読み込み失敗", codec.lastError());
                return;
            }
            // PSDはTiepolo独自形式ではないため、そのまま上書き保存はできない。
            // 「保存」時に改めて名前を付けて保存(.tplo)させるため、現在のパスは持たない
            // (未保存状態のまま=タブ見出し/タイトルバーに変更ありの印が出る、という扱いでよい)。
            gl->setFilePath(QString());
        } else if (isImage) {
            const QImage image(path);
            // 単一の画像ファイル(ドキュメント形式ではない)を開いた場合は、
            // レイヤー構成等の情報を持たないため、レイヤーを1枚だけ作りそこへセットする。
            if (image.isNull() || !gl->loadImageAsSingleLayer(image, QFileInfo(path).completeBaseName())) {
                QMessageBox::warning(this, "読み込み失敗", "画像ファイルを読み込めませんでした。");
                return;
            }
            // PSDと同様、Tiepolo独自形式ではないためそのまま上書き保存はできない。
            gl->setFilePath(QString());
        } else {
            CanvasSerializer serializer(gl);
            serializer.progressCallback = reportProgress;
            if (!serializer.load(path)) {
                QMessageBox::warning(this, "読み込み失敗", serializer.lastError());
                return;
            }
            gl->setFilePath(path);
            RecentFiles::touch(path);
        }
        updateTabLabel(gl);
        if (gl == glWidget) updateWindowTitle();
        }); // QTimer::singleShot(0, ...) 終わり
    }, Qt::SingleShotConnection);

    // 接続を張り終えたのでキャンバス表示へ切り替える。ここで初めてGLWidgetが表示され、
    // initializeGL() → 上のinitializedハンドラ → 読み込み、の順で動く。
    page->showCanvas();
    activateTabPage(page);
    updateCentralPage();
}

// ファイルパスがあればそのファイル名、無ければ仮の名前("Canvas001"等、無ければ「無題」)
static QString displayNameFor(GLWidget *gl)
{
    if (!gl->filePath().isEmpty()) return QFileInfo(gl->filePath()).fileName();
    if (!gl->provisionalName().isEmpty()) return gl->provisionalName();
    return "無題";
}

void MainWindow::updateWindowTitle()
{
    #ifdef TIEPOLO_PRO_BUILD
    QString appName = "Tiepolo Pro";
    #else
    QString appName = "Tiepolo";
    #endif
    // アクティブなタブがまだスタートページの間はキャンバスが無いのでアプリ名だけ出す
    if (!glWidget) { setWindowTitle(appName); return; }
    QString title = displayNameFor(glWidget) + " - " + appName;
    if (glWidget->isModified())
        title = "*" + title;
    setWindowTitle(title);
    updateTabLabel(glWidget);
}

void MainWindow::updateTabLabel(CanvasTabPage *page)
{
    if (!page) return;
    CanvasPane *pane = tabOwnerPane_.value(page);
    if (!pane) return;
    const int idx = pane->tabWidget()->indexOf(page);
    if (idx < 0) return;

    QString label = page->isTransparent() ? QStringLiteral("透過") : QStringLiteral("スタートページ");
    if (GLWidget *gl = page->canvas()) {
        label = displayNameFor(gl);
        if (gl->isModified()) label = "*" + label;
    }
    pane->tabWidget()->setTabText(idx, label);
}

void MainWindow::updateTabLabel(GLWidget *gl)
{
    // GLWidgetの親は必ずそれを載せているCanvasTabPage(CanvasTabPage::setCanvas()参照)。
    if (gl) updateTabLabel(qobject_cast<CanvasTabPage *>(gl->parentWidget()));
}

QString MainWindow::generateUniqueCanvasName() const
{
    const QString docPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QDir projectDir(docPath + "/TiepoloProjects");

    auto nameInUse = [this](const QString &name) {
        for (GLWidget *gl : allCanvasWidgets())
            if (gl->filePath().isEmpty() && gl->provisionalName() == name) return true;
        return false;
    };

    int counter = 1;
    QString name;
    do {
        name = QString("Canvas%1").arg(counter, 3, 10, QChar('0'));
        counter++;
    } while (QFileInfo::exists(projectDir.filePath(name + ".tplo")) || nameInUse(name));

    return name;
}

bool MainWindow::confirmDiscardChanges(GLWidget *gl, const QString &message, const QString &yesLabel)
{
    if (!gl->isModified()) return true;

    QMessageBox msgBox(this);
    msgBox.setWindowTitle("確認");
    msgBox.setText(message);
    msgBox.setIcon(QMessageBox::Question);
    QPushButton *yesBtn = msgBox.addButton(yesLabel, QMessageBox::AcceptRole);
    QPushButton *noBtn  = msgBox.addButton("戻る", QMessageBox::RejectRole);
    msgBox.setDefaultButton(noBtn);
    msgBox.exec();

    return msgBox.clickedButton() == yesBtn;
}

void MainWindow::onCurrentTabChanged(CanvasPane *pane, int index)
{
    // index == -1(そのペインのタブが0枚になった)ならwidget()がnullptrになり、
    // activateTabPage()側で無視される。ペインの後始末(削除・スプリッターの折りたたみ・
    // 最後の1枚ならスタートページのタブを入れ直す)はすべてclosePaneIfEmpty()の責務で、
    // onTabCloseRequested()/movePaneTab()/splitPane()の各末尾から呼ばれる。
    activateTabPage(qobject_cast<CanvasTabPage *>(pane->tabWidget()->widget(index)));
}

void MainWindow::onTabCloseRequested(CanvasPane *pane, int index)
{
    auto *page = qobject_cast<CanvasTabPage *>(pane->tabWidget()->widget(index));
    if (!page) return;

    GLWidget *gl = page->canvas();
    if (gl && !confirmDiscardChanges(gl, "保存されていない変更があります。閉じますか？", "保存せず閉じる"))
        return;

    pane->tabWidget()->removeTab(index);
    tabOwnerPane_.remove(page);

    if (gl) {
        // ここでdeleteLater()して実際にQOpenGLWidgetを破棄すると、同じウィンドウ内の
        // 他のQOpenGLWidget(=他のタブ)と暗黙に共有しているGLコンテキストグループが
        // 道連れで壊れ、他のタブの描画がクラッシュすることがある(特に最初に作られた
        // タブを閉じた場合)。タブを閉じる操作自体は頻繁ではないため、破棄はせず
        // タブバーから外して非表示のまま保持し、実際の解放はアプリ終了時(MainWindow
        // 破棄に伴う通常の親子関係のクリーンアップ)に任せる。
        page->hide();
    } else {
        // まだスタートページのままのタブはGLリソースを持たないので普通に破棄してよい。
        page->deleteLater();
    }

    closePaneIfEmpty(pane);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    bool anyModified = false;
    for (GLWidget *gl : allCanvasWidgets()) {
        if (gl->isModified()) {
            anyModified = true;
            break;
        }
    }

    if (anyModified) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle("確認");
        msgBox.setText("保存されていない変更があります。終了しますか？");
        msgBox.setIcon(QMessageBox::Question);
        QPushButton *yesBtn = msgBox.addButton("保存せず終了", QMessageBox::AcceptRole);
        QPushButton *noBtn  = msgBox.addButton("戻る", QMessageBox::RejectRole);
        msgBox.setDefaultButton(noBtn);
        msgBox.exec();

        if (msgBox.clickedButton() != yesBtn) {
            event->ignore();
            return;
        }
    }
    if (!m_resettingSettings_) saveSettings();
    event->accept();
}

// デバッグ用: 保存済みの全設定(QSettings、ウィンドウ配置・ショートカット・
// 環境設定・最近使ったファイル一覧など)をリセットしてアプリを終了する。
// 次回起動時は初回起動時と同じ状態になる。sandbox等での初回起動状態の
// テストを毎回わざわざ用意しなくて済むようにするための機能。
void MainWindow::resetAllSettingsAndQuit()
{
    const auto reply = QMessageBox::warning(
        this, "全設定をリセット",
        "保存されている全ての設定(ウィンドウ配置・ショートカットキー・環境設定・"
        "最近使ったファイル一覧など)をリセットしてアプリを終了します。\n"
        "次回起動時は、このアプリを初めて開いたときと同じ状態になります。\n"
        "この操作は取り消せません。よろしいですか?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    // closeEvent()内のsaveSettings()でリセット前の値を書き戻されてしまわないよう、
    // まずclose()を通して(未保存の変更があれば通常通り確認ダイアログも出る)、
    // 実際にウィンドウが閉じられたのを確認してからQSettingsを消す。
    m_resettingSettings_ = true;
    const bool closed = close();
    if (!closed) {
        m_resettingSettings_ = false; // ユーザーが「戻る」を選んでキャンセルした
        return;
    }
    QSettings().clear();
}

void MainWindow::showVersionDialog()
{
    QDialog *dialog = new QDialog(this);
    dialog->setWindowTitle("バージョン情報");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModal(false);

    QVBoxLayout *layout = new QVBoxLayout(dialog);

    QLabel *label = new QLabel(dialog);
    QFile file(":/texts/version.txt");
    (void)file.open(QIODevice::ReadOnly);
    QString text = QString::fromUtf8(file.readAll());
        
    label->setText(text);
    layout->addWidget(label);

    QPushButton *closeButton = new QPushButton("閉じる", dialog);
    layout->addWidget(closeButton);

    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);

    dialog->show();
}

QString MainWindow::getUniqueFilePath(const QString &basePath, int fillDigit) {
    QFileInfo fileInfo(basePath);
    QString dirPath = fileInfo.absolutePath();
    QString baseName = fileInfo.baseName();
    QString suffix = fileInfo.suffix();

    int counter = 1;
    QString newPath;
    do {
        // 例: filename_01.png, filename_02.png
        QString numberedName = QString("%1%2.%3")
                               .arg(baseName)
                               .arg(counter, fillDigit, 10, QChar('0'))
                               .arg(suffix);
        newPath = QDir(dirPath).filePath(numberedName);
        counter++;
    } while (QFileInfo::exists(newPath)); // 未使用のファイル名が見つかるまでループ

    return newPath;
}

void MainWindow::saveSettings()
{
    QSettings settings;

    // ウィンドウの状態
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state",    saveState());

    // 各ツールのツールプリセット一覧(名前+設定値)をまるごと保存する
    toolCfg->saveToSettings(settings);

    settings.setValue("tool/color/rawRGBA", toolCfg->color().rawRGBA());
    settings.setValue("tool/color/transparent", toolCfg->color().isTransparent());

    // 選択中ツールは各GLWidgetインスタンス(タブ)ごとの状態なので、終了時点で
    // 実際にUIに表示されていたもの(現在のタブ、タブが1枚も無ければdeckGLWidget_)
    // を保存する。
    {
        GLWidget *gl = glWidget ? glWidget : deckGLWidget_;
        settings.setValue("tool/activeType", (int)gl->getActiveTool());
    }

    settings.setValue("display/colorMode", (int)toolCfg->colorMode().mode());

    const CalibrationConfig &cal = toolCfg->calibration();
    settings.setValue("display/calBrightness", cal.brightness());
    settings.setValue("display/calContrast",   cal.contrast());
    settings.setValue("display/calCyan",       cal.cyan());
    settings.setValue("display/calMagenta",    cal.magenta());
    settings.setValue("display/calYellow",     cal.yellow());

    settings.setValue("display/canvasBgColor", toolCfg->canvasBackground().color());
}

void MainWindow::loadSettings()
{
    QSettings settings;

    if (settings.contains("window/geometry"))
        restoreGeometry(settings.value("window/geometry").toByteArray());

    // このアプリに全画面表示の機能は無い。にもかかわらず保存済みジオメトリに
    // 全画面状態が入っていることがあり、restoreGeometry()がそれを復元してしまう。
    //
    // Qtの全画面処理はウィンドウスタイルをWS_POPUPに差し替え(=WS_CAPTIONも
    // WS_THICKFRAMEも落ちる)、ジオメトリを画面矩形ぴったりに合わせる。その結果、
    // ウィンドウ矩形がモニタ矩形と完全一致し、Windowsのシェルはこれを「最大化」では
    // なく「全画面アプリ」と判定する。そこから
    //   ・自動的に隠れるタスクバーが出てこない
    //   ・DWMがDirect Flipに切り替わり、メニュー等が重なるたび点滅・暗転する
    //   ・GLの面のアルファがそのまま合成され、メニューバーが透ける
    // が同時に起きる。しかも終了時にまた全画面として保存されるため、一度入ると
    // 起動のたびに再現し続ける(「一度手で動かして最大化し直すと以後は直る」のは、
    // その操作で全画面状態を抜けるため)。
    //
    // 全画面で保存されていたら最大化として復元し、この循環を断つ。
    if (isFullScreen()) {
        WINLOG(QStringLiteral("CTOR 保存済みジオメトリが全画面状態だったため最大化へ変換する"));
        setWindowState((windowState() & ~Qt::WindowFullScreen) | Qt::WindowMaximized);
    }
    logDockLayout(QStringLiteral("before restoreState"));
    if (settings.contains("window/state")) {
        savedDockState_ = settings.value("window/state").toByteArray();
        restoreState(savedDockState_);

        // ここで貼っただけでは足りない。restoreState()は「そのときのウィンドウ
        // サイズ」を基準にドックの幅を配分するが、この時点のウィンドウはまだ
        // 「通常時のサイズ」しかない。最大化で保存されていた場合、実際の最大化
        // サイズが決まるのは表示されてからなので、小さいサイズを基準に配分されて
        // ドックが最小幅まで潰れる。しかもQMainWindowは後から増えた幅を中央
        // ウィジェットに全部渡すため、潰れたドックは潰れたまま残る
        // (実測: restoreState時 win=764 -> nav=100px、最終 win=1536 で
        //  central だけ 362 -> 1134 に広がり nav=100 のまま)。
        //
        // そこでサイズが落ち着いてからもう一度貼り直す。ここで貼っておくのは
        // 起動直後の見た目を大きく崩さないため(貼り直しは差分の微調整になる)。
        dockRestorePending_ = true;
    }
    logDockLayout(QStringLiteral("after restoreState"));

    // 以前のバージョンでは、タブが1枚も無い間はDockごと非表示にしていたため、
    // その頃保存されたレイアウトを読み込むと非表示状態のまま復元されてしまう。
    // 現在はDockを常に表示する方針(タブが無ければ中身が空になるだけ)なので、
    // 起動時に一度だけ強制的に可視化しておく(以降はユーザーが表示(V)メニューで
    // 個別に隠す分には、この後は一切触らない)。
    toolDockWidget->show();
    toolPropDockWidget->show();
    toolPresetDockWidget->show();
    brushSizeDockWidget->show();
    navigatorDockWidget->show();
    colorCircleDockWidget->show();
    layerDockWidget->show();

    if (settings.contains("toolPresets/pen/active")) {
        // 新形式: ツールプリセット一覧をまるごと復元
        toolCfg->loadFromSettings(settings);
    } else {
        // 旧形式(ツールプリセット機能追加前、単一プリセットのみ)からの移行
        if (settings.contains("tool/pen/size")) {
            toolCfg->pen().setSize(settings.value("tool/pen/size").toInt());
            toolCfg->pen().setOpacity(settings.value("tool/pen/opacity").toInt());
            toolCfg->pen().setHardness(settings.value("tool/pen/hardness").toInt());
        }
        if (settings.contains("tool/eraser/size")) {
            toolCfg->eraser().setSize(settings.value("tool/eraser/size").toInt());
            toolCfg->eraser().setHardness(settings.value("tool/eraser/hardness").toInt());
        }
    }

    if (settings.contains("brush/smoothing"))
        pendingSmoothing_ = settings.value("brush/smoothing").toFloat();

    if (settings.contains("tool/color/rawRGBA")) {
        const QColor restoredColor = settings.value("tool/color/rawRGBA").value<QColor>();
        toolCfg->color().setRawRGBA(restoredColor);
        // toolCfg側には反映されるが、カラーサークルの表示(見た目)は別に
        // 設定してやらないと復元されない。
        colorCircleDock->setColor(restoredColor);
    }
    {
        const bool transparent = settings.value("tool/color/transparent", false).toBool();
        toolCfg->color().setTransparent(transparent);
        colorCircleDock->setTransparent(transparent);
    }

    if (settings.contains("tool/activeType")) {
        const ToolType restoredTool = (ToolType)settings.value("tool/activeType").toInt();
        // タブが1枚も無い起動直後はdeckGLWidget_がUIの実体なので、そちらの
        // アクティブツールを復元してからボタン表示を同期させる(GLWidget側の
        // activeToolはインスタンスごとの状態で、QSettingsには保存されていない
        // ため、UI(ToolDock)側だけ直しても実体と再びズレてしまう)。
        deckGLWidget_->setActiveTool(restoredTool);
        toolDock->syncButton(restoredTool);
        toolPropDock->setCurrentTool(restoredTool);
        brushSizeDock->syncSize(brushSizeFor(toolCfg, restoredTool));
    }

    if (settings.contains("display/colorMode")) {
        const auto mode = (ColorMode)settings.value("display/colorMode", (int)ColorMode::RGB).toInt();
        toolCfg->colorMode().setMode(mode);
        QAction *checked = colorModeRgbAction;
        if (mode == ColorMode::CMYK) checked = colorModeCmykAction;
        else if (mode == ColorMode::GrayscaleLuminance) checked = colorModeGrayLuminanceAction;
        else if (mode == ColorMode::GrayscaleLightness) checked = colorModeGrayLightnessAction;
        checked->setChecked(true);
        colorCircleDock->setColorMode(mode);
    }

    if (settings.contains("display/calBrightness")) {
        CalibrationConfig &cal = toolCfg->calibration();
        cal.setBrightness(settings.value("display/calBrightness", 0).toInt());
        cal.setContrast(settings.value("display/calContrast", 0).toInt());
        cal.setCyan(settings.value("display/calCyan", 0).toInt());
        cal.setMagenta(settings.value("display/calMagenta", 0).toInt());
        cal.setYellow(settings.value("display/calYellow", 0).toInt());
        colorCircleDock->setCalibration(cal.brightness(), cal.contrast(), cal.cyan(), cal.magenta(), cal.yellow());
    }

    if (settings.contains("display/canvasBgColor"))
        toolCfg->canvasBackground().setColor(settings.value("display/canvasBgColor").value<QColor>());

    toolPropDock->refreshFromSettings();
    toolPresetDock->refresh();

    updateCentralPage();
}

void MainWindow::resetDockLayout()
{
    // ドックが現在フローティング状態(独立ウィンドウとして切り離されている)だと、
    // このあとの配置がその浮いたドックをうまく元のドック領域へ戻せない
    // (浮いたトップレベルウィンドウのままになる)ことがあるため、まず全部
    // 明示的にドッキング状態へ戻してから配置し直す。
    for (QDockWidget *dw : { toolDockWidget, toolPropDockWidget, toolPresetDockWidget,
                             brushSizeDockWidget, navigatorDockWidget,
                             colorCircleDockWidget, layerDockWidget }) {
        if (dw->isFloating()) dw->setFloating(false);
    }

    showMaximized();

    // QMainWindow::saveState()のバイナリ(restoreState())だけに頼ると、実際に
    // ドラッグ&ドロップでドックを並べた際の内部的な親子階層(どのsplitterの下に
    // どのドックがぶら下がっているか)がGUI上の見た目だけでは判別できず、見た目は
    // 意図通りでも階層がずれてしまうことがあった。そのため、まず1) splitDockWidget()で
    // 親子階層そのものを常に同じ形になるよう明示的に確定させ、2) resizeDocks()で
    // 最低限見られる状態のサイズを既定値として設定したうえで、3) この階層と一致する
    // 状態でダンプされたinitial_layout.txtがあれば、そのサイズ配分だけを
    // restoreState()で上書き適用して微調整する(階層自体はすでに1で確定しているため、
    // 一致するダンプであればrestoreState()はサイズの復元だけを行い階層を壊さない)。
    //
    // 左端: ナビゲーター/ツールプリセット/ツール設定/ブラシサイズを縦に積んだ1列
    // その右: ツールアイコン列(toolDockWidget)
    // 右端: カラーサークル/レイヤーを縦に積んだ1列
    splitDockWidget(navigatorDockWidget, toolDockWidget, Qt::Horizontal);
    splitDockWidget(navigatorDockWidget, toolPresetDockWidget, Qt::Vertical);
    splitDockWidget(toolPresetDockWidget, toolPropDockWidget, Qt::Vertical);
    splitDockWidget(toolPropDockWidget, brushSizeDockWidget, Qt::Vertical);
    splitDockWidget(colorCircleDockWidget, layerDockWidget, Qt::Vertical);

    resizeDocks({ navigatorDockWidget, toolDockWidget }, { 220, 40 }, Qt::Horizontal);
    resizeDocks({ navigatorDockWidget, toolPresetDockWidget, toolPropDockWidget, brushSizeDockWidget },
                { 260, 200, 260, 120 }, Qt::Vertical);
    resizeDocks({ colorCircleDockWidget, layerDockWidget }, { 260, 400 }, Qt::Vertical);

    // 3) 上記と同じ階層で保存されたダンプがあれば、サイズ配分だけを微調整として適用する。
    // (:/texts/initial_layout.txtが無い/空の場合は何もしない=上のresizeDocks()の
    //  既定値がそのまま使われる。「レイアウトをダンプ」で書き出したファイルを
    //  resources/texts/initial_layout.txtに置き、resources.qrcに登録すると有効になる)
    {
        QFile file(":/texts/initial_layout.txt");
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray state = QByteArray::fromBase64(file.readAll());
            if (!state.isEmpty())
                restoreState(state);
        }
    }

    toolDockWidget->show();
    toolPropDockWidget->show();
    toolPresetDockWidget->show();
    brushSizeDockWidget->show();
    navigatorDockWidget->show();
    colorCircleDockWidget->show();
    layerDockWidget->show();

    updateCentralPage();
}

// ドックを重ねてタブ化すると、Qtは切り替え用に独自のQTabBarを遅延生成する
// (DockTitleBarとは別物で、各ドックの閉じるボタンも引き継がれない)。それを
// 見つけ次第、DockTitleBarと揃うQSS(#dockGroupTabBar)を当て、タブごとに
// 閉じるボタンを付け直す。
void MainWindow::styleNewDockTabBars()
{
    const auto bars = findChildren<QTabBar *>();
    for (QTabBar *bar : bars) {
        // findChildren はQObjectの親子関係を辿るため、MainWindowを親にして開いた
        // ダイアログ(ショートカット設定など)が持つタブバーまで拾ってしまう。
        // ドックのグループ用タブバーはMainWindow自身の中に作られるので、
        // 別ウィンドウに属するものはここで除外する(これを入れないと、
        // タブ付きダイアログのタブに閉じるボタンとドラッグ用の
        // イベントフィルターが付いてしまう)。
        if (bar->window() != this)
            continue;
        if (qobject_cast<CanvasTabBar *>(bar))
            continue; // キャンバス切替用タブ(各CanvasPaneが持つ)は対象外
        if (bar->objectName() == QLatin1String("dockGroupTabBar"))
            continue; // 処理済み

        bar->setObjectName("dockGroupTabBar");
        bar->setTabsClosable(true);
        bar->setExpanding(false);
        bar->setDrawBase(false);
        bar->style()->unpolish(bar);
        bar->style()->polish(bar);
        // タブをつまんでフロート化(タブ化解除)できるよう、mousePress/Move/Releaseを
        // eventFilter側で横取りして自前でドラッグを実装する。Qt標準の「タブを
        // ドラッグして外す」機構はグループ先頭のドックには効かないため。
        bar->installEventFilter(this);

        connect(bar, &QTabBar::tabCloseRequested, this, [this, bar](int index) {
            const QString title = bar->tabText(index);
            const auto docks = findChildren<QDockWidget *>();
            for (QDockWidget *dw : docks) {
                if (dw->windowTitle() == title) {
                    dw->close();
                    break;
                }
            }
        });
    }
}

// タブ化中は切り替えタブ(上)と各ドックのDockTitleBar(下)が二重に出るため、
// タブ化されている間だけDockTitleBarを高さ0のダミーに差し替えて隠す。
// タブから外すためのドラッグは、DockTitleBarではなくタブバー自体で
// (下のeventFilter経由で)扱うため、この置き換えで支障は無い。
void MainWindow::updateTabifiedTitleBars()
{
    const QList<QDockWidget *> docks = {
        toolDockWidget, toolPropDockWidget, toolPresetDockWidget,
        brushSizeDockWidget, navigatorDockWidget, colorCircleDockWidget, layerDockWidget,
    };
    for (QDockWidget *dock : docks) {
        const bool tabified = !tabifiedDockWidgets(dock).isEmpty();
        const bool hidden = hiddenDockTitleBars_.contains(dock);

        if (tabified && !hidden) {
            QWidget *normal = dock->titleBarWidget();
            if (!normal) continue;
            hiddenDockTitleBars_.insert(dock, normal);
            auto *placeholder = new QWidget(dock);
            placeholder->setFixedHeight(0);
            dock->setTitleBarWidget(placeholder);
        } else if (!tabified && hidden) {
            QWidget *placeholder = dock->titleBarWidget();
            QWidget *normal = hiddenDockTitleBars_.take(dock);
            dock->setTitleBarWidget(normal);
            if (placeholder) placeholder->deleteLater();
        }
    }
}

void MainWindow::openSettingsDialog()
{
    SettingsDlg dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;

    const SettingsDlg::Values v = SettingsDlg::loadValues();

    // Undo履歴の保持数は既に開いている全タブへ即座に反映する
    // (新規に作られるキャンバスはensureCanvas()が毎回読み込む)。
    for (GLWidget *gl : allCanvasWidgets())
        gl->document().setMaxUndo(v.undoHistoryLimit);

    // 色相ツイストはカラーサークル全体で共有する設定なので、生きている
    // 全インスタンスへ一括で反映する(ColorWheelWidget::setHueTwist)。
    ColorWheelWidget::setHueTwist((float)v.hueTwist);

    applyGlobalPressureCurve();

    applyAutoSaveSettings();
}

// 環境設定の「全体の筆圧カーブ」をToolConfigへ反映する
// (起動時のMainWindowコンストラクタ、および設定ダイアログでOKされた直後に呼ぶ)。
// ToolConfigはアプリに1つでキャンバスをまたいで共有されるため、開いている
// タブごとの反映は要らない。
void MainWindow::applyGlobalPressureCurve()
{
    if (!toolCfg) return;
    toolCfg->globalPressureCurve().setPoints(SettingsDlg::loadValues().pressureCurve);
}

// 設定ダイアログの自動保存の有効/間隔をタイマーへ反映する
// (起動時のMainWindowコンストラクタ、および設定ダイアログでOKされた直後に呼ぶ)。
void MainWindow::applyAutoSaveSettings()
{
    const SettingsDlg::Values v = SettingsDlg::loadValues();
    autoSaveTimer_->stop();
    if (v.autoSaveEnabled)
        autoSaveTimer_->start(v.autoSaveInterval * 60 * 1000);
}

// 自動保存: 保存先が既に決まっている(=一度でも保存/別名で保存/開くを行った)
// タブだけを対象に、そのファイルへ上書き保存する。未保存の新規タブは対象外
// (保存ダイアログを勝手に出すと作業の邪魔になるため)。
void MainWindow::performAutoSave()
{
    for (GLWidget *gl : allCanvasWidgets()) {
        if (gl->filePath().isEmpty() || !gl->isModified()) continue;

        CanvasSerializer serializer(gl);
        if (serializer.save(gl->filePath())) {
            gl->markSaved();
            if (gl == glWidget) updateWindowTitle();
        }
        // 失敗しても自動保存なのでダイアログは出さず、次回のタイマーに任せる。
    }
}

void MainWindow::openNewCanvasDialog()
{
    newCanvasInTab(nullptr); // OKされてから追加先タブを決める
}

// pageをキャンバスに変えて新規キャンバスを作る。pageがnullptrなら、ダイアログで
// OKされた時点でtargetTabPageForNewDocument()に決めさせる(キャンセルされたときに
// 空のタブを作ってしまわないようにするため)。
void MainWindow::newCanvasInTab(CanvasTabPage *page)
{
    NewCanvasDlg dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;

    if (!page) page = targetTabPageForNewDocument();

    // 他のタブは一切変更せず、このタブにだけキャンバスを作成する。GLWidgetは
    // 作られた直後はinitializeGL()完了が非同期なため、GLWidget::initializedを
    // 待ってからrecreateCanvas()を呼ぶ(shaderプログラム/テクスチャがまだ無い
    // 状態で呼ぶと壊れるため)。
    GLWidget *gl = ensureCanvas(page);
    gl->setProvisionalName(generateUniqueCanvasName());
    updateTabLabel(page);
    const int w = dlg.canvasWidth(), h = dlg.canvasHeight();
    const bool wx = dlg.wrapX(), wy = dlg.wrapY();
    connect(gl, &GLWidget::initialized, gl, [gl, w, h, wx, wy]() {
        gl->recreateCanvas(w, h, true, wx, wy);
    }, Qt::SingleShotConnection);

    // 接続を張り終えてからキャンバス表示へ切り替える(openFileIntoTab()と同じ理由)。
    page->showCanvas();
    activateTabPage(page);
    updateCentralPage();
}

// 最初のキャンバス(QOpenGLWidget)が現れると、Qtは描画サーフェスの種別を変えるために
// トップレベルのネイティブウィンドウを作り直す。そのたびにフレーム計算とウィンドウ
// リージョンを貼り直さないと、メニューバーが透明かつ掴めない状態になる
// (MainWindow::refreshNativeFrame()のコメント参照)。
bool MainWindow::event(QEvent *event)
{
    if (event->type() == QEvent::WinIdChange) {
        WINLOG(QStringLiteral("EVT  WinIdChange (ネイティブウィンドウ作り直し)"));
        refreshNativeFrame();
        logWindowCompositionState(QStringLiteral("after-WinIdChange"));
    }
    // 表示前に先送りしたフレームの再計算を、ここで必ず取り戻す。
    // (先送りしたままだとWindowsのキャッシュが効いたままで、タイトルバーぶんの
    //  38pxが実在する状態でQtだけが「無い前提」でレイアウトし続ける)
    // ウィンドウサイズが変わるたびに貼り直しの予約を出し直す(デバウンス)。
    // 起動直後はサイズが何度も変わるので、収まった最後の1回だけが実際に貼る。
    if (event->type() == QEvent::Resize && dockRestorePending_)
        scheduleDockLayoutRestore();

    if (event->type() == QEvent::Show && pendingFrameRefresh_) {
        logWindowCompositionState(QStringLiteral("on-Show(before refresh)"));
        // ジオメトリ復元とドック配置が落ち着いてから1回だけ当てる。
        logDockLayout(QStringLiteral("on-Show"));
        scheduleDockLayoutRestore();
        QTimer::singleShot(0, this, [this] {
            if (!pendingFrameRefresh_) return;
            refreshNativeFrame();
            logWindowCompositionState(QStringLiteral("on-Show(after refresh)"));
            logDockLayout(QStringLiteral("on-Show(after refresh)"));
        });
    }

    if (event->type() == QEvent::WindowStateChange) {
        auto *e = static_cast<QWindowStateChangeEvent *>(event);
        WINLOG(QStringLiteral("EVT  WindowStateChange old=0x%1 new=0x%2")
                   .arg((int)e->oldState(), 0, 16).arg((int)windowState(), 0, 16));

        // このアプリに全画面表示の機能は無い(Qt::WindowFullScreenを立てる箇所は
        // どこにも無い)。にもかかわらず、保存済みジオメトリからの復元で全画面状態が
        // 入り込むことがある。しかもコンストラクタではなく表示より後に反映されるため、
        // 復元直後に潰すだけでは捕まらない(実測: on-Show時点ではMaxのみ、その後
        // WindowStateChangeで0x2->0x6=Maximized|FullScreenになる)。
        //
        // 全画面状態のQtはウィンドウスタイルをWS_POPUPへ差し替え(WS_CAPTIONも
        // WS_THICKFRAMEも落ちる)、ジオメトリを画面矩形ぴったりに合わせる。すると
        // ウィンドウ矩形がモニタ矩形と完全一致し、Windowsのシェルはこれを
        // 「最大化」ではなく「全画面アプリ」と判定する。そこから
        //   ・自動的に隠れるタスクバーが出てこない
        //   ・DWMがDirect Flipに切り替わり、メニュー等が重なるたび点滅・暗転する
        //   ・GLの面のアルファがそのまま合成され、メニューバーが透ける
        // が同時に起きる。終了時にまた全画面として保存されるので、一度入ると
        // 起動のたびに再現し続ける(「一度手で動かして最大化し直すと以後は直る」のは
        // その操作で全画面状態を抜けるため)。
        //
        // どの経路で入っても確実に捕まえられるよう、状態変化のたびに見て落とす。
        // 落とした結果また状態変化が飛ぶが、そのときは既にフラグが無いので止まる。
        // droppingFullScreen_は再入防止。setWindowState()は同期的に次の
        // WindowStateChangeを飛ばすため、ガードが無いと状態が数回往復する
        // (実測で 0x6->0x2->0x0->0x6->0x6->0x2 と揺れた)。
        //
        // ただし「OSが最大化だと言っている」ときは落とさない ― これはQtの誤検出で、
        // 落としに行く方が有害だから。
        //
        // Qt(windowsプラグイン)は WM_SIZE/SIZE_MAXIMIZED を受けるたびに
        // isFullScreen_sys() を呼び、「クライアント領域が画面と完全一致していれば
        // 全画面」と判断する。このアプリはWM_NCCALCSIZEで非クライアント領域を潰して
        // いるので、最大化するとクライアント領域はモニタと1pxの狂いもなく一致する
        // (実測: client=(0,0 1920x1080) monitor=(0,0 1920x1080))。つまり最大化する
        // たびに必ず全画面と誤検出される。
        //
        // これは「Qtが状態を報告してきた」だけで、ウィンドウ自体は正しく最大化されて
        // いる(WS_MAXIMIZE、スタイルもCAPTION|THICKFRAMEのまま)。害は無い。
        // ところがここで setWindowState() を呼ぶと、Qtは全画面→最大化の遷移を
        // 「本当に」実行する ― Windowsでは全画面はスタイルとジオメトリの差し替えで
        // 模倣されているため、抜けるときは一度「通常サイズへ復元」してから最大化し直す。
        // これが利用者から見える「一瞬ウィンドウが縮んでまた戻る」の正体で、
        // さらにその縮んだ瞬間のサイズでドック幅が配分し直されて潰れていた
        // (実測: 最大化1536px幅 → 復元778px幅 → nav等が最小の100pxへ → 最大化に
        //  戻ったぶんの幅は全部中央ウィジェットへ)。
        //
        // 本物の全画面(Qtが実際に適用したもの)はスタイルをWS_POPUPに差し替えて
        // ジオメトリを画面に合わせるだけなので、WS_MAXIMIZEは立たない。
        // つまり IsZoomed() が偽。ここでの判定はそれで足りる。
        if (windowState().testFlag(Qt::WindowFullScreen) && !droppingFullScreen_
            && osIsMaximized()) {
            WINLOG(QStringLiteral("EVT  full-screen flag は最大化に伴うQtの誤検出 -> 何もしない"));
        }
        else if (windowState().testFlag(Qt::WindowFullScreen) && !droppingFullScreen_) {
            // どこから全画面が来ているのかを見るため、ウィジェットのジオメトリと
            // 画面のジオメトリを並べて出す。両者が一致していれば、Qtが
            // 「画面を覆っている＝全画面」と推測して立てている可能性が高い。
            if (QScreen *sc = screen()) {
                const QRect g = geometry(), fg = frameGeometry();
                WINLOG(QStringLiteral("EVT  full-screen slipped in: geom=(%1,%2 %3x%4) frame=(%5,%6 %7x%8) "
                                       "screen=(%9,%10 %11x%12) available=(%13,%14 %15x%16)")
                           .arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height())
                           .arg(fg.x()).arg(fg.y()).arg(fg.width()).arg(fg.height())
                           .arg(sc->geometry().x()).arg(sc->geometry().y())
                           .arg(sc->geometry().width()).arg(sc->geometry().height())
                           .arg(sc->availableGeometry().x()).arg(sc->availableGeometry().y())
                           .arg(sc->availableGeometry().width()).arg(sc->availableGeometry().height()));
            }
            WINLOG(QStringLiteral("EVT  full-screen state slipped in -> forcing maximized"));
            logDockLayout(QStringLiteral("before drop-fullscreen"));
            droppingFullScreen_ = true;
            setWindowState((windowState() & ~Qt::WindowFullScreen) | Qt::WindowMaximized);
            droppingFullScreen_ = false;
            logDockLayout(QStringLiteral("after drop-fullscreen"));
            return QMainWindow::event(event);
        }
        logDockLayout(QStringLiteral("WindowStateChange"));
        scheduleDockLayoutRestore(); // 最大化⇔通常でサイズが変わるので貼り直しを予約し直す

        // このイベントは最小化・最大化だけでなくQt::WindowActive(アクティブ状態)の
        // 変化でも飛ぶ。メニューを開閉するだけでも来るため、実際に最小化/最大化/
        // 全画面が変わったときだけ処理する(でないとメニューの開閉のたびに
        // DWMがフレームを描き直して点滅する)。
        const Qt::WindowStates changed = e->oldState() ^ windowState();
        if (!changed.testAnyFlags(Qt::WindowMaximized | Qt::WindowMinimized
                                   | Qt::WindowFullScreen))
            return QMainWindow::event(event);

        // 最小化から復帰したら、ウィンドウリージョン(透過タブの穴)を貼り直して
        // 全体を塗り直す。最小化中は座標が当てにならないので貼るのを見送っており、
        // ここで初めて正しい位置で貼れる。ジオメトリが落ち着いてからにしたいので
        // イベントループを1回まわす。
        //
        // ここでSetWindowPos(SWP_FRAMECHANGED)まではやらないこと。最小化・復帰で
        // フレームの構成は変わっておらず、最大化状態のウィンドウをつつくと
        // 状態が崩れる原因になる(貼り直しが要るのはリージョンと描画だけ)。
        if (e->oldState().testFlag(Qt::WindowMinimized) && !isMinimized()) {
            QTimer::singleShot(0, this, [this] {
                lastAppliedHoles_.clear(); // 同じ穴でも貼り直させる
                updateWindowMask();
                repaintNativeWindow();
            });
        }

        // 注意: ここでSetWindowPos(SWP_FRAMECHANGED)によるフレームの再確定を
        // 行わないこと。メニュー開閉時の点滅には効かず、起動時の点滅を倍増させる
        // だけだった(2026-08-03に試して撤回)。

        // 最大化⇔通常で角の丸めの指定を切り替える(最大化中は丸めない)
        updateWindowCornerStyle();
        updateMaximizeButton();
    }

    return QMainWindow::event(event);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (QApplication::activeModalWidget())
        return QMainWindow::eventFilter(obj, event);

    // (最大化⇔通常に伴うキャプションボタンの見た目の更新はMainWindow::event()側で行う)

    // メニューバーの空き部分を掴んでのウィンドウ移動と、ダブルクリックでの最大化。
    // WM_NCHITTESTでHTCAPTIONを返す方法は環境によって移動が始まらないことがあった
    // ため、押下はクライアント扱いのままQt側で拾い、OS標準のキャプションドラッグを
    // こちらから開始する(MainWindow::beginNativeWindowDrag()参照)。
    if (obj == menuBar() && (event->type() == QEvent::MouseButtonPress
                              || event->type() == QEvent::MouseButtonDblClick)) {
        auto *me = static_cast<QMouseEvent *>(event);
        // メニューバー座標 -> MainWindowのクライアント座標
        const QPoint clientPos = menuBar()->geometry().topLeft() + me->position().toPoint();
        if (me->button() == Qt::LeftButton && isWindowDragArea(clientPos)) {
            if (event->type() == QEvent::MouseButtonDblClick) {
                if (isMaximized()) showNormal();
                else               showMaximized();
            } else {
                beginNativeWindowDrag();
            }
            return true; // メニューバー側の既定処理は行わせない
        }
    }

    // 分割表示での操作権の移動。押されたウィジェットから親を辿って所属CanvasPaneを
    // 見つけ、そこが非アクティブならアクティブにする。キャンバス(GLWidget)だけでなく
    // スタートページ上のボタン・カードやタブバーのクリックでも均一に効かせたいので、
    // 個々のウィジェット側にシグナルを持たせず、ここでqApp全体を見て一括で処理する
    // (イベントは消費しない。押下の本来の処理はそのまま続行される)。
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TabletPress) {
        for (QWidget *w = qobject_cast<QWidget *>(obj); w; w = w->parentWidget()) {
            if (auto *pane = qobject_cast<CanvasPane *>(w)) { activatePane(pane); break; }
        }
    }

    // ドックの切り替えタブ(dockGroupTabBar)をつまんでドラッグすると、その
    // ドックをタブ化グループから外してフロート化する。Qt標準の「タブを
    // ドラッグして外す」機構はグループ先頭のドックに対しては効かないため、
    // mousePress/Move/Releaseを自前で処理する(全タブに均一に効く)。
    // タブ化中のドック領域内のマウスイベントは、実際にはQTabBar自体ではなく
    // 前面に出ているドック(QDockWidgetやその中身)に配送されるため、objの型
    // では判定せず、常にQCursor::pos()とタブバーの画面上の矩形とを突き合わせて
    // ヒットテストする。
    if (event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && !dragTabBar_) {
            const QPoint globalPos = QCursor::pos();
            const auto bars = findChildren<QTabBar *>();
            for (QTabBar *bar : bars) {
                if (bar->objectName() != QLatin1String("dockGroupTabBar")) continue;
                const QPoint localPos = bar->mapFromGlobal(globalPos);
                if (!bar->rect().contains(localPos)) continue;
                const int idx = bar->tabAt(localPos);
                if (idx >= 0) {
                    dragTabBar_   = bar;
                    dragTabIndex_ = idx;
                    dragStartPos_ = globalPos;
                    dragDetached_ = false;
                    dragDock_     = nullptr;
                }
                break;
            }
        }
        // クリックでのタブ切り替え自体は妨げないよう、ここでは消費しない
    } else if (event->type() == QEvent::MouseMove && dragTabBar_) {
        const QPoint globalPos = QCursor::pos();
        if (!dragDetached_) {
            if ((globalPos - dragStartPos_).manhattanLength() >= QApplication::startDragDistance()) {
                const QString title = dragTabBar_->tabText(dragTabIndex_);
                const auto docks = findChildren<QDockWidget *>();
                for (QDockWidget *dw : docks) {
                    if (dw->windowTitle() == title) { dragDock_ = dw; break; }
                }
                if (dragDock_) {
                    dragDetached_     = true;
                    dragCursorOffset_ = QPoint(20, 10);
                    dragDock_->setFloating(true);
                    dragDock_->move(globalPos - dragCursorOffset_);
                    dragDock_->show();
                    dragDock_->raise();
                    dragDock_->activateWindow();
                }
            }
        } else if (dragDock_) {
            dragDock_->move(globalPos - dragCursorOffset_);
        }
        // 一度フロート化した後は、Qt自身の内部ドックレイアウト処理に同じ
        // マウスイベントを渡し続けないよう消費する。渡してしまうと、
        // レイアウトの内部リストから既に外れたウィジェットを前提にした
        // 処理が走り、クラッシュ(QList::at の範囲外アクセス)する。
        if (dragDetached_) return true;
    } else if (event->type() == QEvent::MouseButtonRelease && dragTabBar_) {
        const bool wasDetached = dragDetached_;
        dragTabBar_   = nullptr;
        dragTabIndex_ = -1;
        dragDetached_ = false;
        dragDock_     = nullptr;
        if (wasDetached) return true;
    }

    // テキスト入力欄(テキストレイヤーの編集パネル、レイヤー名リネーム欄など)に
    // フォーカスがある間は、1文字キーがツール切り替えショートカットとして
    // 奪われてしまわないよう、ツールショートカット判定自体をスキップする。
    // QPlainTextEdit/QTextEditはQAbstractScrollArea派生で、実際にキー入力を
    // 受け取るのは内部のviewport()子ウィジェットのため、QApplication::focusWidget()
    // はそのviewportを返す(QPlainTextEdit自身は返らない)。そのため祖先を
    // たどってテキスト入力ウィジェットかどうかを判定する。
    for (QWidget *w = QApplication::focusWidget(); w; w = w->parentWidget()) {
        if (qobject_cast<QLineEdit *>(w) || qobject_cast<QPlainTextEdit *>(w) || qobject_cast<QTextEdit *>(w))
            return QMainWindow::eventFilter(obj, event);
    }

    if (event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (!ke->isAutoRepeat()) {
            const int modifiers = ke->modifiers() & ~Qt::KeypadModifier;
            const int fullKey   = ke->key() | modifiers;
            if (shortcuts_.toolForKey(fullKey) || shortcuts_.presetForKey(fullKey)) {
                applyToolShortcut(fullKey);
                return true;
            }
        }
    }
    else if (event->type() == QEvent::KeyRelease) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Control || ke->key() == Qt::Key_Shift ||
            ke->key() == Qt::Key_Alt     || ke->key() == Qt::Key_Meta) {
            if (m_heldShortcutKey != 0)
                releaseToolShortcut(m_heldShortcutKey);
        }
        if (!ke->isAutoRepeat()) {
            const int modifiers = ke->modifiers() & ~Qt::KeypadModifier;
            const int fullKey   = ke->key() | modifiers;
            if (shortcuts_.toolForKey(fullKey) || shortcuts_.presetForKey(fullKey)) {
                releaseToolShortcut(fullKey);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::applyToolShortcut(int key)
{
    if (!glWidget) return; // キャンバスが1枚も無い
    if (m_heldShortcutKey != 0) return;

    // ---- ツールプリセットのショートカット ----
    // ツールを切り替えたうえで、そのツールのアクティブプリセットも指定のものにする
    // (ツールのショートカットが「そのツールで最後に選ばれていたプリセット」に
    //  なるのに対し、こちらは狙ったプリセットへ直接飛べる)。
    if (const auto preset = shortcuts_.presetForKey(key)) {
        IToolPresetList *list = toolCfg->toolPresetList(preset->tool);
        if (!list) return;
        const int index = list->indexOfUid(preset->uid);
        if (index < 0) return; // 対応するプリセットが既に削除されている

        const ToolType curTool = glWidget->getActiveTool();
        // 既にそのツール・そのプリセットが選ばれているなら何もしない
        // (無駄な状態変更で長押し復帰用の記録を汚さない)
        if (curTool == preset->tool && list->activeIndex() == index) return;

        m_heldShortcutKey   = key;
        m_toolBeforeHold    = curTool;
        m_presetHoldTool    = preset->tool;
        m_presetBeforeHold  = list->activeIndex(); // 対象ツール側のプリセットも戻せるように控える
        m_keyPressTimers[key].start();

        glWidget->setActiveToolPreset(preset->tool, index);
        toolDock->syncButton(preset->tool);
        return;
    }

    // ---- ツールのショートカット(従来どおり) ----
    const auto target = shortcuts_.toolForKey(key);
    if (!target || *target == glWidget->getActiveTool()) return;

    m_heldShortcutKey  = key;
    m_toolBeforeHold   = glWidget->getActiveTool(); // 離したとき直接ここへ戻す(下記参照)
    m_presetBeforeHold = -1;                        // プリセットは触っていない
    m_keyPressTimers[key].start();
    glWidget->setActiveTool(*target);
    toolDock->syncButton(*target);
}

void MainWindow::releaseToolShortcut(int key)
{
    if (!glWidget) return; // キャンバスが1枚も無い
    if (m_heldShortcutKey != key) return;
    m_heldShortcutKey = 0;

    const bool wasHeld = m_keyPressTimers[key].elapsed() >= HOLD_THRESHOLD_MS;

    // プリセットのショートカットで書き換えた「対象ツールのアクティブプリセット」を
    // 先に戻す。ツールを戻すより先に行うのは、setActiveToolPreset()が
    // 「ツールも違えばsetActiveTool()に委ねる」作りのため、順序を逆にすると
    // ツールを戻す処理が二重に走るのを避けるため。
    if (m_presetBeforeHold >= 0) {
        const ToolType presetTool = m_presetHoldTool;
        const int      prevIndex  = m_presetBeforeHold;
        m_presetBeforeHold = -1;
        if (wasHeld) {
            if (IToolPresetList *list = toolCfg->toolPresetList(presetTool))
                if (prevIndex >= 0 && prevIndex < list->count())
                    glWidget->setActiveToolPreset(presetTool, prevIndex);
        }
    }

    if (wasHeld) {
        // GLWidget::returnToPreviousTool()(previousTool)は、スポイトの自動復帰など
        // 他の「ひとつ前のツールに戻る」機能とも共有される単一スロットのため、長押し中に
        // それらが割り込むと(例: スポイトを長押しで一時選択→クリックして色を取得→
        // 自動復帰が発火→離したときにさらにreturnToPreviousTool()すると復帰後の値を
        // 上書きしてしまい、意図した「長押し前のツール」に戻らなくなる)。長押し専用に
        // 保持しておいたm_toolBeforeHold へ直接戻すことでこの競合を避ける。
        glWidget->setActiveTool(m_toolBeforeHold);
        toolDock->syncButton(m_toolBeforeHold);
    }
}