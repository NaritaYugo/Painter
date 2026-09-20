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
using MainWindowHelpers::collectPanes;
using MainWindowHelpers::firstPaneIn;
// MainWindowWorkspace.cpp: responsibilities separated from the MainWindow integration hub.

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

CanvasWidget *MainWindow::ensureCanvas(CanvasTabPage *page)
{
    if (!page) return nullptr;
    if (CanvasWidget *existing = page->canvas()) return existing;

    // このウィンドウで最初のキャンバスを作る前に、今のドック配置を控えておく
    // (使い道は下のCanvasWidget::initializedハンドラのコメント)。
    const bool firstCanvas = !firstCanvasCreated_;
    firstCanvasCreated_ = true;
    const QByteArray dockStateBeforeCanvas = firstCanvas ? saveState() : QByteArray();

    auto *gl = new CanvasWidget(toolCfg, page);
    gl->setSmoothingStrength(pendingSmoothing_);
    gl->document().setMaxUndo(SettingsDlg::loadValues().undoHistoryLimit);
    // この時点ではまだスタートページを表示したままにしておく。表示への切り替えは
    // 呼び出し側がCanvasWidget::initializedへの接続を済ませてからpage->showCanvas()で行う
    // (CanvasTabPage::setCanvas()のコメント参照)。
    page->setCanvas(gl);
    updateTabLabel(page);
    // このキャンバスのGL初期化が終わった時点でもう一度塗り直す。ウィンドウ生成時
    // (WinIdChange)の塗り直しはGLの初回描画より前に走ってしまうため、それだけだと
    // 塗られないまま残る領域ができる。
    connect(gl, &CanvasWidget::initialized, this, [this, gl, firstCanvas, dockStateBeforeCanvas] {
        WINLOG(QStringLiteral("EVT  CanvasWidget::initialized  glFormat: alpha=%1 rgb=%2/%3/%4 swap=%5")
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

void MainWindow::bindCanvasWidget(CanvasWidget *gl)
{
    for (const QMetaObject::Connection &c : glWidgetConnections_)
        QObject::disconnect(c);
    glWidgetConnections_.clear();

    glWidget = gl;

    glWidgetConnections_ << connect(glWidget, &CanvasWidget::initialized,    layerDock, &LayerDock::refresh);
    // layersChanged()はストローク確定の連打や不透明度スライダーのドラッグ等で短時間に
    // 連続発火しうる。直接refresh()(フルキャンバス合成を伴いうる重い処理)へ繋ぐと
    // 発火のたびに毎回それが走ってしまうため、各ドック側のデバウンス経由にする。
    glWidgetConnections_ << connect(glWidget, &CanvasWidget::layersChanged,  layerDock, &LayerDock::scheduleRefresh);
    glWidgetConnections_ << connect(glWidget, &CanvasWidget::layersChanged,  navigatorDock, &NavigatorDock::scheduleRefresh);
    glWidgetConnections_ << connect(glWidget, &CanvasWidget::modifiedChanged, this, &MainWindow::updateWindowTitle);
    // (clearSelectionActionはEscapeキーで変形系アクションのキャンセルも兼ねるため、
    // 選択範囲の有無に関わらず常に有効にしておく。選択が無い状態でのクリア自体は
    // clearSelection()内で安全に無視される)
    glWidgetConnections_ << connect(glWidget, &CanvasWidget::colorDropperd, this,
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
    glWidgetConnections_ << connect(glWidget, &CanvasWidget::activeToolChanged, this, [this](ToolType tool) {
        toolDock->syncButton(tool);
        toolCfg->color().setRawRGBA(colorCircleDock->color());
        toolCfg->color().setTransparent(colorCircleDock->isTransparent());
        toolPropDock->setCurrentTool(tool);
        brushSizeDock->syncSize(brushSizeFor(toolCfg, tool));
    });
    glWidgetConnections_ << connect(glWidget, &CanvasWidget::activeToolPresetChanged, this, [this](ToolType tool, int) {
        // ToolType自体は変わらずツールプリセットだけ切り替わった場合、表示中の設定値を更新する
        toolPropDock->refreshFromSettings();
        brushSizeDock->syncSize(brushSizeFor(toolCfg, tool));
    });

    // 各Dockを新しいタブへretargetし、表示内容をそのタブの状態で作り直す
    toolDock->setCanvasWidget(glWidget);
    toolPropDock->setCanvasWidget(glWidget);
    toolPresetDock->setCanvasWidget(glWidget);
    layerDock->setCanvasWidget(glWidget);
    navigatorDock->setCanvasWidget(glWidget);
    brushSizeDock->syncSize(brushSizeFor(toolCfg, glWidget->getActiveTool()));
}

// ---------------------------------------------------------------------------
// 分割表示(複数ペイン)関連
// ---------------------------------------------------------------------------

QVector<CanvasWidget *> MainWindow::allCanvasWidgets() const
{
    QVector<CanvasPane *> panes;
    collectPanes(paneTreeRoot_, panes);
    QVector<CanvasWidget *> result;
    for (CanvasPane *pane : panes) {
        QTabWidget *tabs = pane->tabWidget();
        for (int i = 0; i < tabs->count(); ++i) {
            auto *page = qobject_cast<CanvasTabPage *>(tabs->widget(i));
            if (!page) continue;
            // まだスタートページのままのタブはキャンバスを持たないので飛ばす
            if (CanvasWidget *gl = page->canvas()) result.append(gl);
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

// 現在のCanvasWidgetへの接続を切り、各Dockをダミー(deckCanvasWidget_)へ戻す。
// アクティブなタブがまだスタートページのときの状態。
void MainWindow::unbindCanvasWidget()
{
    for (const QMetaObject::Connection &c : glWidgetConnections_)
        QObject::disconnect(c);
    glWidgetConnections_.clear();
    glWidget = nullptr;

    toolDock->setCanvasWidget(deckCanvasWidget_);
    toolPropDock->setCanvasWidget(deckCanvasWidget_);
    toolPresetDock->setCanvasWidget(deckCanvasWidget_);
    layerDock->setCanvasWidget(deckCanvasWidget_);
    navigatorDock->setCanvasWidget(deckCanvasWidget_);
}

// 操作権をpageへ移す単一の入口。所属ペインを特定してそのタブをカレントにし、
// アクティブペインの枠線を付け替えたうえで、キャンバスならbindCanvasWidget()、まだ
// スタートページならunbindCanvasWidget()する。ペイン内のタブ切替
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

    CanvasWidget *gl = page->canvas();
    if (!gl) {
        // このタブはまだスタートページか透過タブ。スタートページなら最近使ったファイル
        // 一覧を最新にしてから、キャンバス依存のUIをダミーへ逃がす。
        if (!page->isTransparent()) page->startPage()->refresh();
        unbindCanvasWidget();
        updateWindowTitle();
        updateTabRelatedActionsEnabled();
        return;
    }

    if (gl == glWidget) return; // 既にこのキャンバスがアクティブ

    if (gl->isGLReady()) {
        bindCanvasWidget(gl);
        updateWindowTitle();
    } else {
        // 作成直後でまだinitializeGL()が完了していないキャンバスの場合、完了を
        // 待ってから接続を張る。
        connect(gl, &CanvasWidget::initialized, this, [this, gl]() {
            if (glWidget == gl) return; // 待っている間に別経路で既にアクティブ化済み
            bindCanvasWidget(gl);
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
    // ここでページ(CanvasWidgetを含む)が別ペインの子になる=reparent。QOpenGLWidgetは
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
// Dock類は常に表示したままにする(キャンバスが無い間はダミーのdeckCanvasWidget_を
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

static QString displayNameFor(CanvasWidget *gl)
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
    if (CanvasWidget *gl = page->canvas()) {
        label = displayNameFor(gl);
        if (gl->isModified()) label = "*" + label;
    }
    pane->tabWidget()->setTabText(idx, label);
}

void MainWindow::updateTabLabel(CanvasWidget *gl)
{
    // CanvasWidgetの親は必ずそれを載せているCanvasTabPage(CanvasTabPage::setCanvas()参照)。
    if (gl) updateTabLabel(qobject_cast<CanvasTabPage *>(gl->parentWidget()));
}

QString MainWindow::generateUniqueCanvasName() const
{
    const QString docPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QDir projectDir(docPath + "/TiepoloProjects");

    auto nameInUse = [this](const QString &name) {
        for (CanvasWidget *gl : allCanvasWidgets())
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

bool MainWindow::confirmDiscardChanges(CanvasWidget *gl, const QString &message, const QString &yesLabel)
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

    CanvasWidget *gl = page->canvas();
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
    for (CanvasWidget *gl : allCanvasWidgets()) {
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

