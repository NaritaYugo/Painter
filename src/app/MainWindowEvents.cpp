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
// MainWindowEvents.cpp: responsibilities separated from the MainWindow integration hub.

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
    // 見つけ、そこが非アクティブならアクティブにする。キャンバス(CanvasWidget)だけでなく
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
        // CanvasWidget::returnToPreviousTool()(previousTool)は、スポイトの自動復帰など
        // 他の「ひとつ前のツールに戻る」機能とも共有される単一スロットのため、長押し中に
        // それらが割り込むと(例: スポイトを長押しで一時選択→クリックして色を取得→
        // 自動復帰が発火→離したときにさらにreturnToPreviousTool()すると復帰後の値を
        // 上書きしてしまい、意図した「長押し前のツール」に戻らなくなる)。長押し専用に
        // 保持しておいたm_toolBeforeHold へ直接戻すことでこの競合を避ける。
        glWidget->setActiveTool(m_toolBeforeHold);
        toolDock->syncButton(m_toolBeforeHold);
    }
}
