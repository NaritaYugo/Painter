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
using MainWindowHelpers::collectPanes;
// MainWindowChrome.cpp: responsibilities separated from the MainWindow integration hub.

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
               .arg(d(toolPropertyDockWidget,    "prop"))
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
