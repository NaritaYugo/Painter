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
// MainWindowSettings.cpp: responsibilities separated from the MainWindow integration hub.

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
    if (!calibrationDialog) {
        calibrationDialog = new CalibrationDialog(this);
        connect(calibrationDialog, &CalibrationDialog::valuesChanged, this, &MainWindow::setCalibration);
    }
    const CalibrationConfig &cal = toolCfg->calibration();
    calibrationDialog->setValues(cal.brightness(), cal.contrast(), cal.cyan(), cal.magenta(), cal.yellow());
    calibrationDialog->show();
    calibrationDialog->raise();
    calibrationDialog->activateWindow();
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
    for (CanvasWidget *gl : allCanvasWidgets())
        gl->applyDisplayConfig();
}

// UIテーマ

void MainWindow::setUiTheme(Theme::Name n)
{
    Theme::setTheme(n);
    Theme::applyToApplication(*qobject_cast<QApplication *>(QApplication::instance()));

    (n == Theme::Name::Dark ? themeDarkAction : themeLightAction)->setChecked(true);

    // QSSの再適用だけではDraggablePanel::paintEvent()等、Theme::を直接参照するQPainter描画は自動で再描画されないため、明示的に全ウィジェットへupdate()する。
    const auto widgets = QApplication::allWidgets();
    for (QWidget *w : widgets) w->update();

    QSettings().setValue("ui/theme", (int)n);
}

// キャンバス外側の背景色

void MainWindow::chooseCanvasBackgroundColor()
{
    const QColor current = toolCfg->canvasBackground().color();
    // ネイティブのWindows色選択ダイアログは、環境によって(マルチモニタ構成等)ジオメトリ設定に失敗しダイアログが壊れた状態になることがあるため、Qt自前描画のダイアログを強制する。
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

    // closeEvent()内のsaveSettings()でリセット前の値を書き戻されてしまわないよう、まずclose()を通して(未保存の変更があれば通常通り確認ダイアログも出る)、
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
        // 例: filename_01.png, filename_02.png。
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

    // ウィンドウの状態。
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state",    saveState());

    // 各ツールのツールプリセット一覧(名前+設定値)をまるごと保存する。
    toolCfg->saveToSettings(settings);

    settings.setValue("tool/color/rawRGBA", toolCfg->color().rawRGBA());
    settings.setValue("tool/color/transparent", toolCfg->color().isTransparent());

    // 選択中ツールは各CanvasWidgetインスタンス(タブ)ごとの状態なので、終了時点で実際にUIに表示されていたもの(現在のタブ、タブが1枚も無ければdeckCanvasWidget_)を保存する。
    {
        CanvasWidget *gl = glWidget ? glWidget : deckCanvasWidget_;
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

    // このアプリに全画面表示の機能は無い。
    if (isFullScreen()) {
        WINLOG(QStringLiteral("CTOR 保存済みジオメトリが全画面状態だったため最大化へ変換する"));
        setWindowState((windowState() & ~Qt::WindowFullScreen) | Qt::WindowMaximized);
    }
    logDockLayout(QStringLiteral("before restoreState"));
    if (settings.contains("window/state")) {
        savedDockState_ = settings.value("window/state").toByteArray();
        restoreState(savedDockState_);

        // ここで貼っただけでは足りない。
        dockRestorePending_ = true;
    }
    logDockLayout(QStringLiteral("after restoreState"));

    // 以前のバージョンでは、タブが1枚も無い間はDockごと非表示にしていたため、その頃保存されたレイアウトを読み込むと非表示状態のまま復元されてしまう。
    toolDockWidget->show();
    toolPropertyDockWidget->show();
    toolPresetDockWidget->show();
    brushSizeDockWidget->show();
    navigatorDockWidget->show();
    colorCircleDockWidget->show();
    layerDockWidget->show();

    if (settings.contains("toolPresets/pen/active")) {
        // 新形式: ツールプリセット一覧をまるごと復元。
        toolCfg->loadFromSettings(settings);
    } else {
        // 旧形式(ツールプリセット機能追加前、単一プリセットのみ)からの移行。
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
        // toolCfg側には反映されるが、カラーサークルの表示(見た目)は別に設定してやらないと復元されない。
        colorCircleDock->setColor(restoredColor);
    }
    {
        const bool transparent = settings.value("tool/color/transparent", false).toBool();
        toolCfg->color().setTransparent(transparent);
        colorCircleDock->setTransparent(transparent);
    }

    if (settings.contains("tool/activeType")) {
        const ToolType restoredTool = (ToolType)settings.value("tool/activeType").toInt();
        // タブが1枚も無い起動直後はdeckCanvasWidget_がUIの実体なので、そちらのアクティブツールを復元してからボタン表示を同期させる(CanvasWidget側のactiveToolはインスタンスごとの状態で、
        // QSettingsには保存されていないため、UI(ToolDock)側だけ直しても実体と再びズレてしまう)。
        deckCanvasWidget_->setActiveTool(restoredTool);
        toolDock->syncButton(restoredTool);
        toolPropertyDock->setCurrentTool(restoredTool);
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

    toolPropertyDock->refreshFromSettings();
    toolPresetDock->refresh();

    updateCentralPage();
}

void MainWindow::resetDockLayout()
{
    // ドックが現在フローティング状態(独立ウィンドウとして切り離されている)だと、このあとの配置がその浮いたドックをうまく元のドック領域へ戻せない(浮いたトップレベルウィンドウのままになる)ことがあるため、
    // まず全部明示的にドッキング状態へ戻してから配置し直す。
    for (QDockWidget *dw : { toolDockWidget, toolPropertyDockWidget, toolPresetDockWidget,
                             brushSizeDockWidget, navigatorDockWidget,
                             colorCircleDockWidget, layerDockWidget }) {
        if (dw->isFloating()) dw->setFloating(false);
    }

    showMaximized();

    // QMainWindow::saveState()のバイナリ(restoreState())だけに頼ると、実際にドラッグ&ドロップでドックを並べた際の内部的な親子階層(どのsplitterの下にどのドックがぶら下がっているか)がGUI上の見た目
    // だけでは判別できず、見た目は意図通りでも階層がずれてしまうことがあった。
    splitDockWidget(navigatorDockWidget, toolDockWidget, Qt::Horizontal);
    splitDockWidget(navigatorDockWidget, toolPresetDockWidget, Qt::Vertical);
    splitDockWidget(toolPresetDockWidget, toolPropertyDockWidget, Qt::Vertical);
    splitDockWidget(toolPropertyDockWidget, brushSizeDockWidget, Qt::Vertical);
    splitDockWidget(colorCircleDockWidget, layerDockWidget, Qt::Vertical);

    resizeDocks({ navigatorDockWidget, toolDockWidget }, { 220, 40 }, Qt::Horizontal);
    resizeDocks({ navigatorDockWidget, toolPresetDockWidget, toolPropertyDockWidget, brushSizeDockWidget },
                { 260, 200, 260, 120 }, Qt::Vertical);
    resizeDocks({ colorCircleDockWidget, layerDockWidget }, { 260, 400 }, Qt::Vertical);

    // 3) 上記と同じ階層で保存されたダンプがあれば、サイズ配分だけを微調整として適用する。
    {
        QFile file(":/texts/initial_layout.txt");
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray state = QByteArray::fromBase64(file.readAll());
            if (!state.isEmpty())
                restoreState(state);
        }
    }

    toolDockWidget->show();
    toolPropertyDockWidget->show();
    toolPresetDockWidget->show();
    brushSizeDockWidget->show();
    navigatorDockWidget->show();
    colorCircleDockWidget->show();
    layerDockWidget->show();

    updateCentralPage();
}

// ドックを重ねてタブ化すると、Qtは切り替え用に独自のQTabBarを遅延生成する(DockTitleBarとは別物で、各ドックの閉じるボタンも引き継がれない)。
void MainWindow::styleNewDockTabBars()
{
    const auto bars = findChildren<QTabBar *>();
    for (QTabBar *bar : bars) {
        // findChildren はQObjectの親子関係を辿るため、MainWindowを親にして開いたダイアログ(ショートカット設定など)が持つタブバーまで拾ってしまう。
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
        // タブをつまんでフロート化(タブ化解除)できるよう、mousePress/Move/ReleaseをeventFilter側で横取りして自前でドラッグを実装する。
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

// タブ化中は切り替えタブ(上)と各ドックのDockTitleBar(下)が二重に出るため、タブ化されている間だけDockTitleBarを高さ0のダミーに差し替えて隠す。
void MainWindow::updateTabifiedTitleBars()
{
    const QList<QDockWidget *> docks = {
        toolDockWidget, toolPropertyDockWidget, toolPresetDockWidget,
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
    SettingsDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;

    const SettingsDialog::Values v = SettingsDialog::loadValues();

    // Undo履歴の保持数は既に開いている全タブへ即座に反映する
    for (CanvasWidget *gl : allCanvasWidgets())
        gl->document().setMaxUndo(v.undoHistoryLimit);

    // 色相ツイストはカラーサークル全体で共有する設定なので、生きている
    ColorWheelWidget::setHueTwist((float)v.hueTwist);

    applyGlobalPressureCurve();

    applyAutoSaveSettings();
}

// 環境設定の「全体の筆圧カーブ」をToolConfigへ反映する
void MainWindow::applyGlobalPressureCurve()
{
    if (!toolCfg) return;
    toolCfg->globalPressureCurve().setPoints(SettingsDialog::loadValues().pressureCurve);
}

// 設定ダイアログの自動保存の有効/間隔をタイマーへ反映する
void MainWindow::applyAutoSaveSettings()
{
    const SettingsDialog::Values v = SettingsDialog::loadValues();
    autoSaveTimer_->stop();
    if (v.autoSaveEnabled)
        autoSaveTimer_->start(v.autoSaveInterval * 60 * 1000);
}

// 自動保存: 保存先が既に決まっている(=一度でも保存/別名で保存/開くを行った)タブだけを対象に、そのファイルへ上書き保存する。
void MainWindow::performAutoSave()
{
    for (CanvasWidget *gl : allCanvasWidgets()) {
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

// pageをキャンバスに変えて新規キャンバスを作る。
void MainWindow::newCanvasInTab(CanvasTabPage *page)
{
    NewCanvasDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;

    if (!page) page = targetTabPageForNewDocument();

    // 他のタブは一切変更せず、このタブにだけキャンバスを作成する。
    CanvasWidget *gl = ensureCanvas(page);
    gl->setProvisionalName(generateUniqueCanvasName());
    updateTabLabel(page);
    const int w = dialog.canvasWidth(), h = dialog.canvasHeight();
    const bool wx = dialog.wrapX(), wy = dialog.wrapY();
    connect(gl, &CanvasWidget::initialized, gl, [gl, w, h, wx, wy]() {
        gl->recreateCanvas(w, h, true, wx, wy);
    }, Qt::SingleShotConnection);

    // 接続を張り終えてからキャンバス表示へ切り替える(openFileIntoTab()と同じ理由)。
    page->showCanvas();
    activateTabPage(page);
    updateCentralPage();
}

// 最初のキャンバス(QOpenGLWidget)が現れると、Qtは描画サーフェスの種別を変えるためにトップレベルのネイティブウィンドウを作り直す。
