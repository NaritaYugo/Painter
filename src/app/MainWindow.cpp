#include "app/MainWindow.h"
#include "app/MainWindowHelpers.h"
#include "docks/LayerDock.h"
#include "docks/ColorCircleDock.h"
#include "docks/ToolDock.h"
#include "docks/ToolPropertyDock.h"
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
#include "dialogs/CalibrationDialog.h"

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
    ColorWheelWidget::setHueTwist((float)SettingsDialog::loadValues().hueTwist);

    // 全ツール共通の筆圧カーブも環境設定由来のアプリ全体共通値。
    applyGlobalPressureCurve();

    createWidgets();
    setupActions();  // QAction生成・ショートカット登録(MainWindowShortcuts.cpp)

    // toolDockはsetupActions()より前のcreateWidgets()内で生成される(=構築時点ではshortcuts_にまだ何も登録されておらずキー表示が空になる)ため、
    // ここで改めてツールチップのキー表示を最新の状態に合わせておく。
    toolDock->refreshTooltips();

    connectSignals();
    createMenus();
    setupCaptionButtons(); // createMenus()でメニューバーが出来た後に置く

    // 中央には常にタブが1枚以上ある。
    addNewTab();

    qApp->installEventFilter(this);
    installNativeWindowFilter(); // タイトルバー非表示まわりのWindows固有処理
    addDockWidget(Qt::LeftDockWidgetArea, navigatorDockWidget);
    addDockWidget(Qt::LeftDockWidgetArea, toolPropertyDockWidget);
    addDockWidget(Qt::LeftDockWidgetArea, toolPresetDockWidget);
    addDockWidget(Qt::LeftDockWidgetArea, brushSizeDockWidget);
    addDockWidget(Qt::LeftDockWidgetArea, toolDockWidget);
    addDockWidget(Qt::RightDockWidgetArea, colorCircleDockWidget);
    addDockWidget(Qt::RightDockWidgetArea, layerDockWidget);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    setDockNestingEnabled(true);

    // 標準セパレータは0pxにし、見た目を持たない広めのヒット領域だけを境界へ重ねる。
    new DockResizeController(*this);

    // ドックを重ねてタブ化した際にQtが遅延生成する切り替え用QTabBarを捕まえてDockTitleBar風の見た目(閉じるボタン付き)に揃え直す。
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
        // resetDockLayout()(手動の「ドック配置をリセット」と共通)はrestoreState()を使っており、その復元結果はウィンドウの実サイズに依存する。
        QTimer::singleShot(0, this, &MainWindow::resetDockLayout); // 初回起動時は初期レイアウトを適用
    }

    // loadSettings()は保存済みの色(tool/color/rawRGBA)がある場合のみcolorCircleDockへ反映するため、
    // 初回起動時(保存設定が無い場合)はtoolCfgの初期色(黒)がカラーサークルのUIへ一度も反映されないまま(ColorWheelWidget自身のコンストラクタ既定値である中間グレー位置のまま)になってしまう。
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

    // 変形の確定(Enter)はメニューには出さない一時的なアクションなので、メニューに積まずに直接ウィンドウへ関連付けてショートカットだけ有効にする。
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
    windowMenu->addAction(toolPropertyDockWidget->toggleViewAction());
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
    centralStack = new QStackedWidget(this);
    centralStack->setObjectName(QStringLiteral("canvasWorkspace"));
    centralStack->setFrameShape(QFrame::NoFrame);
    centralStack->setContentsMargins(0, 0, 0, 0);
    setCentralWidget(centralStack);
    // 最初のペインとタブはコンストラクタ側(setupActions()の後)で作る。

    // Dock類のコンストラクタはCanvasWidget*を要求するため、タブが1枚も無い間のダミーとして使う非表示CanvasWidgetを1つ用意する(どのペインのタブにも追加せず、表示もしないためinitializeGL()は走らず、
    // GLリソースのコストは掛からない)。
    deckCanvasWidget_ = new CanvasWidget(toolCfg, this);
    deckCanvasWidget_->hide(); // 表示しない(initializeGL()を走らせないため。Dock構築用のダミー)

    toolDock       = new ToolDock(deckCanvasWidget_, shortcuts_, this);
    toolDockWidget = new QDockWidget("ツール", this);
    toolDockWidget->setObjectName("toolDockWidget");
    toolDockWidget->setAllowedAreas(Qt::AllDockWidgetAreas);
    toolDockWidget->setTitleBarWidget(new DockTitleBar("ツール", toolDockWidget));
    toolDockWidget->setWidget(toolDock);

    toolPropertyDock       = new ToolPropertyDock(deckCanvasWidget_, toolCfg, this);
    toolPropertyDockWidget = new QDockWidget("ツール設定", this);
    // QMainWindow::restoreState()が参照する永続ID。
    toolPropertyDockWidget->setObjectName("toolPropDockWidget");
    toolPropertyDockWidget->setAllowedAreas(Qt::AllDockWidgetAreas);
    toolPropertyDockWidget->setTitleBarWidget(new DockTitleBar("ツール設定", toolPropertyDockWidget));
    toolPropertyDockWidget->setWidget(toolPropertyDock);

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

    // 以前はここでtabWidgetのcurrentChanged/tabCloseRequestedを直接繋いでいたが、分割表示対応でペインが複数になりうるため、
    // ペインごとにcreatePane()内でCanvasPane::currentTabChanged/tabCloseRequestedへ接続するようになった。

    // 初期状態(タブ0枚 → StartPage表示、キャンバス依存アクション無効化、Dock非表示)への反映はここでは行わない。
}

// glWidget(現在アクティブなタブ)に対する接続を張り直す。

void MainWindow::connectSignals() {
    // Dock間の接続
    connect(toolDock, &ToolDock::toolChanged, this, [this](ToolType curr) {
        toolCfg->color().setRawRGBA(colorCircleDock->color());
        toolCfg->color().setTransparent(colorCircleDock->isTransparent());
        brushSizeDock->syncSize(brushSizeFor(toolCfg, curr));
        toolPropertyDock->setCurrentTool(curr);
    });
    connect(toolPropertyDock, &ToolPropertyDock::sizeChanged, this, [this]() {
        // タブが1枚も無い間はglWidgetがnullptrになるため、その間はダミーのdeckCanvasWidget_を使う(ToolDockのアクティブツールもタブ0枚の間はdeckCanvasWidget_側に反映されているので、
        // これで正しい値が取れる)。
        CanvasWidget *gl = glWidget ? glWidget : deckCanvasWidget_;
        ToolType curr = gl->getActiveTool();
        brushSizeDock->syncSize(brushSizeFor(toolCfg, curr));
        gl->updateCursor();
    });
    connect(brushSizeDock, &BrushSizeDock::brushSizeChanged, this, [this](int px) {
        CanvasWidget *gl = glWidget ? glWidget : deckCanvasWidget_;
        ToolType curr = gl->getActiveTool();
        setBrushSizeFor(toolCfg, curr, px);
        toolPropertyDock->syncSize(px);
        gl->updateCursor();
    });
    connect(colorCircleDock, &ColorCircleDock::colorChanged, this, [this](const QColor &c) {
        toolCfg->color().setRawRGBA(c);
    });
    // 「透明色」のオン/オフ。
    connect(colorCircleDock, &ColorCircleDock::transparentChanged, this, [this](bool on) {
        toolCfg->color().setTransparent(on);
        if (glWidget) glWidget->update(); // ライブプレビューの色が変わるので描き直す
    });
}

// Photoshopのブラシファイル(.abr)
