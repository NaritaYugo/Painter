#include "app/MainWindow.h"
#include "app/NativeWindowLog.h"

#include <QWindow>
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QStringList>
#include <QTimer>
#include <cmath>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h> // SHQueryUserNotificationState(診断ログのシェル全画面判定)
#include <dwmapi.h>
#include <cstring>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

// SDKのバージョンによっては未定義なので自前で用意する
// (DwmSetWindowAttribute自体はdwmapi.dllに昔からある)。
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#define TIEPOLO_DWMWCP_DEFAULT     0
#define TIEPOLO_DWMWCP_DONOTROUND  1
#endif

// ---------------------------------------------------------------------------
// Windows固有の処理をまとめたファイル。役割は次の2つ。
//
//  (1) タイトルバーを消す      : WM_NCCALCSIZE で非クライアント領域を潰し、
//                                WM_NCHITTEST で四辺のリサイズ枠を教え直す
//  (2) applyWindowRegion()     : 透過タブのぶんだけウィンドウに穴を開ける
//
// この2つは切り離せない。(1)でタイトルバーぶんを潰すと、Qtが把握している
// フレームマージン(ウィンドウスタイルからAdjustWindowRectExで逆算した値)と
// 実際のクライアント領域がずれるためである。QWidget::setMask()はそのQtの値で
// 領域をオフセットしてSetWindowRgnへ渡すので、そのまま使うと穴がキャプション
// 高さぶん下へずれ、上端(=メニューバー)が領域外に出て切り落とされてしまう。
// そこで(2)ではsetMask()を使わず、ClientToScreen()で得た「実際の」クライアント
// 原点を基準に自前でSetWindowRgn()する。
//
// 【長く原因が分からなかった不具合の記録】
// 「起動時に点滅する / メニュー開閉で点滅する / キャンバスを開くと暗転する /
//  メニューバーが透明になる / 自動的に隠れるタスクバーが出てこない」が同時に出て
// いた時期がある。真因はこのファイルではなく、保存済みジオメトリの復元で
// Qt::WindowFullScreen が紛れ込んでいたこと(MainWindow::event() のコメント参照)。
// 全画面状態のQtはウィンドウスタイルをWS_POPUPへ差し替え、ジオメトリを画面矩形
// ぴったりに合わせるため、シェルから「最大化」ではなく「全画面アプリ」と判定され、
// そこから上記すべてが派生していた。
//
// このファイル側で試して効かなかったもの(同じ道を辿らないこと):
//   ・WM_NCACTIVATE を lParam=-1 で処理して非クライアント領域を再描画させない
//   ・表示後に SetWindowPos(SWP_FRAMECHANGED) でフレームを確定させる
//   ・DwmExtendFrameIntoClientArea でDWMにフレームを延ばさせる
//   ・ウィンドウのレイヤード化(WS_EX_LAYERED + LWA_ALPHA 255)
//   ・DwmEnableBlurBehindWindow で「透明領域なし」を宣言する
//   ・最大化時にクライアント領域を1px詰める(Direct Flipには効くがタスクバーには
//     効かない。シェルが見ているのはクライアント矩形ではなくウィンドウ矩形のため)
//   ・WS_CAPTION/WS_THICKFRAME を後から SetWindowLongPtr で貼り直す
//     (その時点で既に最大化ジオメトリが確定しているため効かない)
//
// 教訓: ウィンドウメッセージのログだけでは足りず、Qt側のウィンドウ状態
// (windowState())まで出して初めて原因が見えた。診断ログ(NativeWindowLog.h)は
// そのまま残してある。TIEPOLO_WINLOG=1 で有効。
// ---------------------------------------------------------------------------

#ifdef Q_OS_WIN
namespace {

// ---- 診断ログ用のヘルパー(TIEPOLO_WINLOG が設定されているときだけ動く) --------

const char *msgName(UINT m)
{
    switch (m) {
    case WM_CREATE:                return "WM_CREATE";
    case WM_DESTROY:               return "WM_DESTROY";
    case WM_MOVE:                  return "WM_MOVE";
    case WM_SIZE:                  return "WM_SIZE";
    case WM_ACTIVATE:              return "WM_ACTIVATE";
    case WM_SETFOCUS:              return "WM_SETFOCUS";
    case WM_KILLFOCUS:             return "WM_KILLFOCUS";
    case WM_ENABLE:                return "WM_ENABLE";
    case WM_SETREDRAW:             return "WM_SETREDRAW";
    case WM_PAINT:                 return "WM_PAINT";
    case WM_CLOSE:                 return "WM_CLOSE";
    case WM_ERASEBKGND:            return "WM_ERASEBKGND";
    case WM_SYSCOLORCHANGE:        return "WM_SYSCOLORCHANGE";
    case WM_SHOWWINDOW:            return "WM_SHOWWINDOW";
    case WM_ACTIVATEAPP:           return "WM_ACTIVATEAPP";
    case WM_SETCURSOR:             return "WM_SETCURSOR";
    case WM_MOUSEACTIVATE:         return "WM_MOUSEACTIVATE";
    case WM_WINDOWPOSCHANGING:     return "WM_WINDOWPOSCHANGING";
    case WM_WINDOWPOSCHANGED:      return "WM_WINDOWPOSCHANGED";
    case WM_STYLECHANGING:         return "WM_STYLECHANGING";
    case WM_STYLECHANGED:          return "WM_STYLECHANGED";
    case WM_DISPLAYCHANGE:         return "WM_DISPLAYCHANGE";
    case WM_NCCREATE:              return "WM_NCCREATE";
    case WM_NCDESTROY:             return "WM_NCDESTROY";
    case WM_NCCALCSIZE:            return "WM_NCCALCSIZE";
    case WM_NCHITTEST:             return "WM_NCHITTEST";
    case WM_NCPAINT:               return "WM_NCPAINT";
    case WM_NCACTIVATE:            return "WM_NCACTIVATE";
    case WM_GETDLGCODE:            return "WM_GETDLGCODE";
    case WM_SYNCPAINT:             return "WM_SYNCPAINT";
    case WM_NCMOUSEMOVE:           return "WM_NCMOUSEMOVE";
    case WM_NCLBUTTONDOWN:         return "WM_NCLBUTTONDOWN";
    case WM_NCLBUTTONUP:           return "WM_NCLBUTTONUP";
    case WM_MOUSEMOVE:             return "WM_MOUSEMOVE";
    case WM_LBUTTONDOWN:           return "WM_LBUTTONDOWN";
    case WM_LBUTTONUP:             return "WM_LBUTTONUP";
    case WM_INITMENU:              return "WM_INITMENU";
    case WM_INITMENUPOPUP:         return "WM_INITMENUPOPUP";
    case WM_ENTERMENULOOP:         return "WM_ENTERMENULOOP";
    case WM_EXITMENULOOP:          return "WM_EXITMENULOOP";
    case WM_ENTERIDLE:             return "WM_ENTERIDLE";
    case WM_ENTERSIZEMOVE:         return "WM_ENTERSIZEMOVE";
    case WM_EXITSIZEMOVE:          return "WM_EXITSIZEMOVE";
    case WM_SIZING:                return "WM_SIZING";
    case WM_MOVING:                return "WM_MOVING";
    case WM_CAPTURECHANGED:        return "WM_CAPTURECHANGED";
    case WM_SYSCOMMAND:            return "WM_SYSCOMMAND";
    case WM_GETMINMAXINFO:         return "WM_GETMINMAXINFO";
    case WM_DPICHANGED:            return "WM_DPICHANGED";
    case WM_THEMECHANGED:          return "WM_THEMECHANGED";
    case WM_DWMCOMPOSITIONCHANGED: return "WM_DWMCOMPOSITIONCHANGED";
    case WM_DWMNCRENDERINGCHANGED: return "WM_DWMNCRENDERINGCHANGED";
    case WM_DWMCOLORIZATIONCOLORCHANGED: return "WM_DWMCOLORIZATIONCOLORCHANGED";
    case WM_DWMWINDOWMAXIMIZEDCHANGE:    return "WM_DWMWINDOWMAXIMIZEDCHANGE";
    default:                       return nullptr;
    }
}

// マウス移動のたびに飛ぶものはログを埋め尽くすので、TIEPOLO_WINLOG=2 のときだけ出す。
// ただし「マウスを動かしただけで点滅する」症状を追うときは 2 が要る。
bool isNoisyMessage(UINT m)
{
    switch (m) {
    case WM_NCHITTEST: case WM_SETCURSOR: case WM_MOUSEMOVE: case WM_NCMOUSEMOVE:
    case WM_GETICON:   case WM_TIMER:     case WM_ENTERIDLE: case WM_GETDLGCODE:
    case WM_GETMINMAXINFO:
        return true;
    default:
        return false;
    }
}

// どのウィンドウ宛かを見分けられるように、ハンドルとウィンドウクラス名を出す。
// Qtのメニューポップアップは "Qt5152QWindowToolSaveBitsPopup..." のような
// クラス名になるので、これでメインウィンドウと区別できる。
QString hwndDesc(HWND h)
{
    wchar_t cls[64] = {0};
    ::GetClassNameW(h, cls, 63);
    return QStringLiteral("%1(%2)").arg((quintptr)h, 0, 16).arg(QString::fromWCharArray(cls));
}

QString rectStr(const RECT &r)
{
    return QStringLiteral("(%1,%2 %3x%4)").arg(r.left).arg(r.top)
               .arg(r.right - r.left).arg(r.bottom - r.top);
}

// SetWindowPos系のフラグのうち、再描画に関わるものだけを読める形にする。
QString swpFlagsStr(UINT f)
{
    QStringList s;
    if (f & SWP_NOSIZE)       s << "NOSIZE";
    if (f & SWP_NOMOVE)       s << "NOMOVE";
    if (f & SWP_NOZORDER)     s << "NOZORDER";
    if (f & SWP_NOREDRAW)     s << "NOREDRAW";
    if (f & SWP_NOACTIVATE)   s << "NOACTIVATE";
    if (f & SWP_FRAMECHANGED) s << "FRAMECHANGED";
    if (f & SWP_SHOWWINDOW)   s << "SHOWWINDOW";
    if (f & SWP_HIDEWINDOW)   s << "HIDEWINDOW";
    if (f & SWP_NOCOPYBITS)   s << "NOCOPYBITS";
    if (f & SWP_NOSENDCHANGING) s << "NOSENDCHANGING";
    return s.isEmpty() ? QStringLiteral("-") : s.join('|');
}

// メッセージ1件を読める1行にする。点滅の犯人は「全面再描画を起こすもの」なので、
// 再描画に関わるメッセージだけは中身まで開いて出す。
QString describeMessage(UINT message, WPARAM wParam, LPARAM lParam, HWND hwnd)
{
    const char *name = msgName(message);
    QString head = name ? QString::fromLatin1(name)
                        : QStringLiteral("0x%1").arg(message, 4, 16, QLatin1Char('0'));

    switch (message) {
    case WM_NCCALCSIZE:
        if (wParam) {
            auto *p = reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam);
            head += QStringLiteral(" wnd=%1 zoomed=%2").arg(rectStr(p->rgrc[0])).arg(::IsZoomed(hwnd) ? 1 : 0);
        } else {
            head += QStringLiteral(" (simple)");
        }
        break;
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED: {
        auto *p = reinterpret_cast<WINDOWPOS *>(lParam);
        head += QStringLiteral(" pos=(%1,%2 %3x%4) flags=%5")
                    .arg(p->x).arg(p->y).arg(p->cx).arg(p->cy).arg(swpFlagsStr(p->flags));
        break;
    }
    case WM_NCACTIVATE:
        head += QStringLiteral(" active=%1 lParam=%2").arg(wParam ? 1 : 0).arg((qint64)lParam);
        break;
    case WM_ACTIVATE:
        head += QStringLiteral(" %1").arg(LOWORD(wParam) == WA_INACTIVE ? "INACTIVE"
                                          : (LOWORD(wParam) == WA_CLICKACTIVE ? "CLICKACTIVE" : "ACTIVE"));
        break;
    case WM_ACTIVATEAPP:
        head += QStringLiteral(" active=%1").arg(wParam ? 1 : 0);
        break;
    case WM_SETREDRAW:
        head += QStringLiteral(" allow=%1").arg(wParam ? 1 : 0);
        break;
    case WM_SIZE: {
        const char *k = wParam == SIZE_MAXIMIZED ? "MAXIMIZED"
                       : wParam == SIZE_MINIMIZED ? "MINIMIZED"
                       : wParam == SIZE_RESTORED  ? "RESTORED" : "other";
        head += QStringLiteral(" %1 %2x%3").arg(k).arg(LOWORD(lParam)).arg(HIWORD(lParam));
        break;
    }
    case WM_PAINT:
    case WM_NCPAINT: {
        RECT upd{};
        // 更新矩形がウィンドウ全体なら「全面再描画」= 点滅の正体である可能性が高い。
        if (::GetUpdateRect(hwnd, &upd, FALSE)) head += QStringLiteral(" update=%1").arg(rectStr(upd));
        else                                     head += QStringLiteral(" update=(none)");
        break;
    }
    case WM_ERASEBKGND:
        head += QStringLiteral(" hdc=%1").arg((quintptr)wParam, 0, 16);
        break;
    case WM_SHOWWINDOW:
        head += QStringLiteral(" show=%1 reason=%2").arg(wParam ? 1 : 0).arg((qint64)lParam);
        break;
    case WM_STYLECHANGING:
    case WM_STYLECHANGED: {
        // どのビットが増減したのかまで出す。同じフラグ指定から環境ごとに違う
        // スタイルが出来ている以上、「誰がいつ何を外したか」が要る。
        auto *ss = reinterpret_cast<STYLESTRUCT *>(lParam);
        auto bits = [](DWORD s, bool ex) {
            QStringList v;
            if (!ex) {
                if (s & WS_CAPTION)    v << "CAPTION";
                if (s & WS_THICKFRAME) v << "THICKFRAME";
                if (s & WS_POPUP)      v << "POPUP";
                if (s & WS_MINIMIZEBOX) v << "MINBOX";
                if (s & WS_MAXIMIZEBOX) v << "MAXBOX";
                if (s & WS_SYSMENU)    v << "SYSMENU";
                if (s & WS_BORDER)     v << "BORDER";
                if (s & WS_DLGFRAME)   v << "DLGFRAME";
                if (s & WS_VISIBLE)    v << "VISIBLE";
                if (s & WS_DISABLED)   v << "DISABLED";
            } else {
                if (s & WS_EX_LAYERED)             v << "LAYERED";
                if (s & WS_EX_TOPMOST)             v << "TOPMOST";
                if (s & WS_EX_NOREDIRECTIONBITMAP) v << "NOREDIR";
                if (s & WS_EX_TOOLWINDOW)          v << "TOOLWIN";
            }
            return v.isEmpty() ? QStringLiteral("-") : v.join('|');
        };
        const bool ex = (wParam == GWL_EXSTYLE);
        head += QStringLiteral(" %1: %2 -> %3").arg(ex ? "EXSTYLE" : "STYLE")
                    .arg(bits(ss->styleOld, ex)).arg(bits(ss->styleNew, ex));
        break;
    }
    case WM_SYSCOMMAND:
        head += QStringLiteral(" cmd=0x%1").arg(wParam & 0xFFF0, 0, 16);
        break;
    default:
        break;
    }
    return head;
}

// アプリ全体のネイティブイベントフィルタ。QWidget::nativeEvent()はメッセージに
// よってはQt内部の処理が先に走って届かないことがある(実際WM_NCCALCSIZEは届くのに
// WM_NCHITTESTは効かなかった)。こちらはQtがウィンドウプロシージャに入って最初に、
// 無条件で呼ばれるフックなので確実に横取りできる。
class NativeWindowFilter : public QAbstractNativeEventFilter
{
public:
    explicit NativeWindowFilter(MainWindow *w) : w_(w) {}

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override
    {
        if (eventType != QByteArrayLiteral("windows_generic_MSG")) return false;
        auto *msg = static_cast<MSG *>(message);
        if (!w_) return false;
        // 宛先の判定にwinId()を使わないこと。winId()はハンドルが無ければ生成しに行く
        // 副作用があり、ネイティブウィンドウの作り直し中に呼ぶと再入して危険。
        // QWidget::find()はただの逆引きなので安全で、作り直し後も正しく追従する。
        if (QWidget::find(reinterpret_cast<WId>(msg->hwnd)) != w_) {
            // レベル3では他のウィンドウ(メニューのポップアップ等)宛も記録する。
            // メインウィンドウ宛のメッセージだけを見ていても分からない現象
            // (ポップアップの生成・破棄がDWMに何をさせているか等)を追うため。
            if (WinLog::level() >= 3 && !isNoisyMessage(msg->message))
                WinLog::write(QStringLiteral("OTH  %1 %2")
                                  .arg(hwndDesc(msg->hwnd))
                                  .arg(describeMessage(msg->message, msg->wParam, msg->lParam, msg->hwnd)));
            return false;
        }
        return w_->handleNativeWindowMessage(msg->message, msg->wParam, msg->lParam, msg->hwnd, result);
    }

private:
    MainWindow *w_;
};
} // namespace
#endif

void MainWindow::installNativeWindowFilter()
{
#ifdef Q_OS_WIN
    // ここでwinId()を呼ばないこと。ネイティブウィンドウを前倒しで生成してしまい、
    // このあとのジオメトリ復元・ドック配置で余計なリサイズを何度も招く。
    // 宛先の判定は副作用のないQWidget::find()で行うので、この時点でハンドルが
    // 存在している必要はない。
    // MainWindowはアプリに1つだけなので、staticな実体で足りる。
    static NativeWindowFilter filter(this);
    qApp->installNativeEventFilter(&filter);

    WINLOG(QStringLiteral("=== Tiepolo window diagnostics (level=%1) ===").arg(WinLog::level()));
    WINLOG(QStringLiteral("hint: '***' の付いた行が全面再描画を起こす操作。"
                           "MSG行のWM_PAINT update= がウィンドウ全体なら点滅として見える。"));
#endif
}

// ネイティブウィンドウが作り直されたとき(QEvent::WinIdChange)と初回表示後に呼ぶ。
//
// 最初のQOpenGLWidget(=キャンバス)が現れると、描画サーフェスの種別が変わるため
// Qtはトップレベルのネイティブウィンドウを作り直す。新しいHWNDには、こちらが
// WM_NCCALCSIZEで作ったフレームの状態もSetWindowRgn()で開けた穴も引き継がれない。
// Windowsはフレーム情報をキャッシュしていて、SWP_FRAMECHANGEDで明示的に要求しないと
// WM_NCCALCSIZEを送り直してくれないため、放っておくと「クライアント領域の認識」と
// 実際の描画・ヒットテストがずれたままになる(上端が透明かつ掴めない状態)。
void MainWindow::refreshNativeFrame()
{
#ifdef Q_OS_WIN
    QWindow *handle = windowHandle();
    if (!handle) return;
    auto hwnd = reinterpret_cast<HWND>(handle->winId());
    if (!hwnd) return;

    WINLOG(QStringLiteral("CALL refreshNativeFrame visible=%1 zoomed=%2")
               .arg(isVisible() ? 1 : 0).arg(::IsZoomed(hwnd) ? 1 : 0));

    // ウィンドウを作り直すと角の指定も失われるので貼り直す。前回値のキャッシュも
    // 無効化しないと「同じ値だから」と省略されてしまう。ただしこの時点ではまだ
    // 最大化が確定していないことがある(作り直した直後など)ため、状態が落ち着いて
    // からもう一度当て直す。
    lastCornerPref_ = -1;
    updateWindowCornerStyle();
    QTimer::singleShot(0, this, &MainWindow::updateWindowCornerStyle);

    // 新しいHWNDにはリージョンが無いので、同じ穴でも貼り直させる。
    lastAppliedHoles_.clear();

    // まだ表示前なら、ここではフレームの再計算を要求しない。このあとジオメトリの
    // 復元やドック配置で何度もリサイズが走るため、今やっても無駄打ちになる。
    //
    // ただし「やらない」で済ませてはいけない。Windowsはフレームの計算結果を
    // キャッシュしており、SWP_FRAMECHANGEDで明示的に要求しない限りWM_NCCALCSIZEを
    // 送り直さない。実測(STATEログ)では、この早期リターンのせいで
    //   起動直後   : clientOrigin=(1,38) ← 38pxのキャプションが実在したまま
    //   キャンバス後: clientOrigin=(0,0)  ← ようやくWM_NCCALCSIZEが効いた状態
    // という2つの状態が生まれ、キャンバスを開くまでずっと「Qtはタイトルバーが無い
    // 前提でレイアウトしているのに、実際には38px残っている」というズレたまま
    // 動いていた。表示された時点で必ず取り直す。
    if (!isVisible()) {
        pendingFrameRefresh_ = true;
        WINLOG(QStringLiteral("CALL refreshNativeFrame -> deferred until shown"));
        return;
    }
    pendingFrameRefresh_ = false;

    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    updateWindowMask();
    repaintNativeWindow();
    scheduleNativeRepaint(); // 直後の再レイアウトで再び穴が空くことがあるので念のため
#endif
}

// 【試して駄目だったこと】合成経路をDWM側へ固定する目的で、ウィンドウを
// レイヤード化(WS_EX_LAYERED + SetLayeredWindowAttributes(alpha=255))する手を
// 試したが、点滅・暗転・メニューバー透明のどれにも効かず、そのうえ自動的に
// 隠れるタスクバーが出てこなくなった。2026-08-04に実測して撤回。
// 同じくDwmEnableBlurBehindWindowで「透明領域なし」を宣言する手も、
// fEnable=FALSE+領域指定はE_INVALIDARGで弾かれ、fEnable=TRUEは(領域が空でも)
// ブラー背景を有効化する呼び出しでDWMにアルファを尊重させてしまい悪化した。

// 【試して駄目だったこと】
// ・reapplyOsMaximize(): 表示後に SetWindowPlacement(SW_SHOWMAXIMIZED) で最大化
//   ジオメトリをOSに計算し直させる。
// ・enforceNativeWindowStyle(): WS_CAPTION/WS_THICKFRAME を後から SetWindowLongPtr で
//   貼り直す。
// どちらも「ウィンドウ矩形がモニタ矩形とぴったり一致してしまう」現象への対症療法
// だったが、真因は Qt::WindowFullScreen が紛れ込んでいたことで、全画面状態のQtが
// スタイルをWS_POPUPへ差し替えて画面矩形に合わせていたのが理由だった
// (MainWindow::event() の WindowStateChange のコメント参照)。真因を断ったので
// どちらも不要になり削除した。スタイルを後から貼り直しても、その時点で既に
// 最大化ジオメトリは確定しているため元々効いていなかった。2026-08-04。

// ちらつき・透明化はウィンドウメッセージの層には現れなかった(実測: メニュー開閉でも
// キャンバス表示でもMainWindow宛の描画メッセージはゼロ)。ということは、原因は
// 「何を描くか」ではなく「ウィンドウの面がどう合成されるか」の側にある。
// per-pixel alphaを持つ面だと、描かれていない画素はそのまま透明として抜けるため、
// 「メニューバーが透明」「一瞬透ける=点滅」がどちらも説明できる。
// その裏を取るために、合成に効く状態をまとめて出す。
void MainWindow::logWindowCompositionState(const QString &tag)
{
#ifdef Q_OS_WIN
    if (!WinLog::enabled()) return;
    QWindow *handle = windowHandle();
    if (!handle) { WINLOG(QStringLiteral("STATE %1: windowHandle=null").arg(tag)); return; }
    auto hwnd = reinterpret_cast<HWND>(handle->winId());
    if (!hwnd) { WINLOG(QStringLiteral("STATE %1: hwnd=null").arg(tag)); return; }

    const LONG_PTR ex = ::GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    const LONG_PTR st = ::GetWindowLongPtr(hwnd, GWL_STYLE);
    QStringList exs;
    if (ex & WS_EX_LAYERED)              exs << "LAYERED";
    if (ex & WS_EX_TRANSPARENT)          exs << "TRANSPARENT";
    if (ex & WS_EX_COMPOSITED)           exs << "COMPOSITED";
    if (ex & WS_EX_NOREDIRECTIONBITMAP)  exs << "NOREDIRECTIONBITMAP";
    if (ex & WS_EX_TOPMOST)              exs << "TOPMOST";
    QStringList sts;
    if (st & WS_CAPTION)    sts << "CAPTION";
    if (st & WS_THICKFRAME) sts << "THICKFRAME";
    if (st & WS_POPUP)      sts << "POPUP";
    if (st & WS_DISABLED)   sts << "DISABLED";

    RECT wr{}, cr{};
    ::GetWindowRect(hwnd, &wr);
    ::GetClientRect(hwnd, &cr);
    POINT origin{0, 0};
    ::ClientToScreen(hwnd, &origin);

    // ウィンドウ矩形がモニタ矩形をはみ出しているか。OSが本当に最大化したウィンドウは
    // 枠のぶん外側へはみ出す。ぴったり一致していたら「画面と同じ大きさに置かれただけ」で、
    // シェルは全画面アプリとみなす(=タスクバーが出ない・Direct Flipで点滅する)。
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    ::GetMonitorInfo(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const bool overhangs = wr.left < mi.rcMonitor.left || wr.top < mi.rcMonitor.top
                        || wr.right > mi.rcMonitor.right || wr.bottom > mi.rcMonitor.bottom;
    // シェルが今このアプリを全画面扱いしているか(2=QUNS_BUSY, 3=D3D全画面)。
    int notifyState = 0;
    ::SHQueryUserNotificationState(reinterpret_cast<QUERY_USER_NOTIFICATION_STATE *>(&notifyState));

    // リージョンが設定されているか(設定されていればその外側は完全に抜ける)
    RECT rgn{};
    const int rgnType = ::GetWindowRgnBox(hwnd, &rgn);

    BOOL ncRendering = FALSE;
    ::DwmGetWindowAttribute(hwnd, DWMWA_NCRENDERING_ENABLED, &ncRendering, sizeof(ncRendering));

    // 面がアルファを持っているかが本丸。QOpenGLWidgetの実際のフォーマットを見る。
    const QSurfaceFormat fmt = handle->format();

    // Qt側のウィンドウ状態。全画面(0x4)が立っていたらそれが諸症状の元
    // (MainWindow::loadSettings()のコメント参照)。
    QStringList qs;
    if (windowState() & Qt::WindowMinimized)  qs << "Min";
    if (windowState() & Qt::WindowMaximized)  qs << "Max";
    if (windowState() & Qt::WindowFullScreen) qs << "FULLSCREEN";
    if (windowState() & Qt::WindowActive)     qs << "Active";

    WINLOG(QStringLiteral("STATE %1: qtState=%16 qtFlags=0x%15 style=%2 ex=%3 zoomed=%4 win=%5 monitor=%6 overhangs=%7 shellState=%8 "
                           "client=%9 clientOrigin=(%10,%11) rgn=%12 dwmNC=%13 alpha=%14")
               .arg(tag)
               .arg(sts.isEmpty() ? QStringLiteral("-") : sts.join('|'))
               .arg(exs.isEmpty() ? QStringLiteral("-") : exs.join('|'))
               .arg(::IsZoomed(hwnd) ? 1 : 0)
               .arg(rectStr(wr)).arg(rectStr(mi.rcMonitor))
               // overhangs=0 なら「画面と同じ大きさに置かれただけ」= シェルは全画面扱い
               .arg(overhangs ? 1 : 0)
               // 2=BUSY(全画面アプリ扱い) / 5=ACCEPTS_NOTIFICATIONS(通常)
               .arg(notifyState)
               .arg(rectStr(cr))
               .arg(origin.x).arg(origin.y)
               .arg(rgnType == 0 /*ERROR*/ ? QStringLiteral("none") : rectStr(rgn))
               .arg(ncRendering ? 1 : 0)
               .arg(fmt.alphaBufferSize())
               // Qtが持っているウィンドウフラグ。Win32スタイルが剥がされる原因が
               // 「Qtのフラグ自体が変わっている」のか「フラグは同じなのにQtの
               // 変換結果が違う」のかを切り分けるために出す。
               .arg((quint32)windowFlags(), 0, 16)
               .arg(qs.isEmpty() ? QStringLiteral("Normal") : qs.join('|')));
#else
    Q_UNUSED(tag);
#endif
}

// ウィンドウの角を丸めるかどうかをDWMへ明示的に指定する。
//
// Windows 11では通常DWMが自動で判断し、最大化中は角を丸めない。ところがこちらは
// WM_NCCALCSIZEで非クライアント領域を潰しているため、その判定が噛み合わず最大化中
// でも丸められてしまうことがある。最大化中は「丸めない」を明示し、通常時は
// Windows標準の見た目に任せる。
//
// 最大化しているかの判定にQtのisMaximized()を使わないこと。QEvent::WindowStateChangeは
// 最小化・最大化だけでなくQt::WindowActive(アクティブ状態)の変化でも飛ぶため、
// メニューを開いてウィンドウが一時的に非アクティブになった瞬間などに過渡的な値を
// 読んでしまい、「メニューを開くと丸くなり閉じると四角に戻る」という往復が起きる。
// IsZoomed()はOS側の正解なのでそれが無い。
// 「応答なし」の身代わりウィンドウ(ゴーストウィンドウ)を作らせない。
//
// Windowsは「ウィンドウを持つスレッドが5秒間メッセージを取りに来ない」とアプリを
// ハングとみなし、本物のウィンドウを隠して薄暗い身代わりを出す。身代わりには
// OS標準のタイトルバーが付くので、タイトルバーを自前で描いているこのアプリでは
// 「見たことのない灰色のタイトルバーが現れ、閉じるボタンが赤く光る」という
// いかにも壊れたような見た目になる。
//
// 実際にはコンピュートシェーダーの初回コンパイル(数秒)が終われば必ず戻ってくる
// ので、ハングではない。CanvasWidget::initializeGL() 側でコンパイルの合間に
// PeekMessage() を挟んでハング判定自体に引っかからないようにしてあるが、
// 将来ほかの重い処理が入ったときにまた身代わりが出るのは避けたいので、
// ゴースト機能そのものもここで切っておく(二重の備え)。
//
// 副作用として、本当にハングした場合に「身代わり越しに閉じる」ことはできなくなる。
// その場合はタスクマネージャーからの終了になる。
void MainWindow::disableWindowGhosting()
{
#ifdef Q_OS_WIN
    ::DisableProcessWindowsGhosting();
#endif
}

// OS側の「最大化しているか」(WS_MAXIMIZEが立っているか)。
// Qtの isMaximized()/isFullScreen() は、状態変化の途中の値だったり、
// ジオメトリからの推測(isFullScreen_sys())が混じったりする。確定した答えが
// 要るところではこちらを使う。
bool MainWindow::osIsMaximized() const
{
#ifdef Q_OS_WIN
    QWindow *handle = windowHandle();
    if (!handle) return false;
    auto hwnd = reinterpret_cast<HWND>(handle->winId());
    return hwnd && ::IsZoomed(hwnd);
#else
    return isMaximized();
#endif
}

void MainWindow::updateWindowCornerStyle()
{
#ifdef Q_OS_WIN
    QWindow *handle = windowHandle();
    if (!handle) return;
    auto hwnd = reinterpret_cast<HWND>(handle->winId());
    if (!hwnd) return;

    const DWORD pref = ::IsZoomed(hwnd) ? DWORD(TIEPOLO_DWMWCP_DONOTROUND)
                                         : DWORD(TIEPOLO_DWMWCP_DEFAULT);
    // 値が同じでもDwmSetWindowAttribute()を呼ぶとDWMがフレームを描き直し、それが
    // 点滅として見える。変わったときだけ実際に設定する。
    if (int(pref) == lastCornerPref_) {
        WINLOG(QStringLiteral("CALL updateWindowCornerStyle pref=%1 (skipped, unchanged)").arg(pref));
        return;
    }
    WINLOG(QStringLiteral("CALL updateWindowCornerStyle pref=%1 <- %2 *** DwmSetWindowAttribute ***")
               .arg(pref).arg(lastCornerPref_));
    lastCornerPref_ = int(pref);
    ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
#endif
}

// ウィンドウ全体を子ウィジェットごと明示的に描き直させる。
//
// キャンバス(QOpenGLWidget)があるとウィンドウ全体がOpenGL経由で合成され、その
// 合成面はアルファを持つ。描かれなかった画素はアルファ0のまま=透明として見えるため、
// 「構成は変わったのに中身が描き直されていない」領域がそのまま透明な帯や細い線に
// なって残る(クリックは通るので、リージョンで切り抜かれているのとは区別できる)。
void MainWindow::repaintNativeWindow()
{
#ifdef Q_OS_WIN
    QWindow *handle = windowHandle();
    if (!handle || isMinimized()) return;
    // RDW_ERASEは付けないこと。背景消去(WM_ERASEBKGND)を伴うため、ウィンドウクラスの
    // 背景ブラシで一度塗りつぶされる。GLがまだ描ける状態でないタイミング(最初の
    // キャンバス生成直後など)と重なると、画面全体が暗く塗られて見える。
    if (auto hwnd = reinterpret_cast<HWND>(handle->winId())) {
        // RedrawWindowはウィンドウ全体+全子ウィジェットの同期再描画。全面再描画を
        // 起こす数少ない箇所なので、呼ばれたことがログに残るようにしておく。
        WINLOG(QStringLiteral("CALL repaintNativeWindow *** RedrawWindow(ALLCHILDREN|UPDATENOW) ***"));
        ::RedrawWindow(hwnd, nullptr, nullptr,
                        RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
#endif
    update();
}

// 次のイベントループで1回だけ塗り直す。レイアウト変更やGLの初期化が落ち着いてから
// 塗りたい場面のために遅延させ、連続して要求されても1回にまとめる
// (RedrawWindowは同期的な全面再描画なので、続けて呼ぶと重い)。
void MainWindow::scheduleNativeRepaint()
{
    if (nativeRepaintScheduled_) return;
    nativeRepaintScheduled_ = true;
    WINLOG(QStringLiteral("CALL scheduleNativeRepaint (queued)"));
    QTimer::singleShot(0, this, [this] {
        nativeRepaintScheduled_ = false;
        repaintNativeWindow();
    });
}

// 実際の処理本体。上のNativeWindowFilterからのみ呼ばれる。
// (QWidget::nativeEvent()側にも同じ委譲を置いていた時期があるが、フィルタが先に
//  処理して消費するため一度も違う結果にならない二重経路でしかなく、削除した)
// hwndはHWND(ヘッダにwindows.hを持ち込まないためvoid*で受ける)。
bool MainWindow::handleNativeWindowMessage(unsigned message, quintptr wParam, qintptr lParam,
                                            void *hwndOpaque, qintptr *result)
{
#ifdef Q_OS_WIN
    {
        auto hwnd = static_cast<HWND>(hwndOpaque);
        struct { UINT message; WPARAM wParam; LPARAM lParam; HWND hwnd; }
            m{ message, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam), hwnd };
        auto *msg = &m;

        // 診断ログ(TIEPOLO_WINLOG が設定されているときだけ)。ここはMainWindowの
        // HWND宛のメッセージが必ず1回通る唯一の場所なので、点滅の引き金を探す
        // 定点観測点として使える。
        if (WinLog::enabled() && (WinLog::verbose() || !isNoisyMessage(msg->message)))
            WinLog::write(QStringLiteral("MSG  ") + describeMessage(msg->message, msg->wParam, msg->lParam, msg->hwnd));

        switch (msg->message) {

        // ---- タイトルバーの領域をクライアント領域に取り込む --------------------
        // Qt::FramelessWindowHintは使わない。あれはWS_CAPTIONと一緒にWS_THICKFRAME
        // まで落としてしまい、画面上端へのドラッグでの最大化(スナップ)、最大化時の
        // 作業領域への収まり、自動的に隠れるタスクバーの復帰といったOS側の挙動が
        // まとめて効かなくなるため。ウィンドウスタイルは通常のまま残し、非クライアント
        // 領域の計算だけを横取りする。
        case WM_NCCALCSIZE: {
            if (!msg->wParam) break; // 矩形1つぶんの単純な問い合わせなので触らない
            auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);

            // 通常時: rgrc[0](=ウィンドウ矩形)に一切手を加えずに0を返す。
            // これでクライアント領域がウィンドウ矩形と一致し、非クライアント領域が
            // 無くなる = タイトルバーも枠も描かれない。
            //
            // 以前はDefWindowProcに計算させてから上端だけ戻していたが、最大化時に
            // 枠の厚みぶんを手で足し引きする必要があり、その誤差が最大化⇔復元の
            // たびに積み上がってウィンドウがどんどん縮んでいた。
            if (::IsZoomed(msg->hwnd)) {
                // 最大化時のウィンドウ矩形は枠のぶん画面外へ広がっているので、
                // クライアント領域を作業領域(タスクバーを除く範囲)にぴったり合わせる。
                // これで画面外へはみ出さず、タスクバーも隠れない。
                HMONITOR mon = ::MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                // 【試して駄目だったこと】ここでクライアント領域を1px詰めれば
                // Direct Flipを回避できるが、自動的に隠れるタスクバーはそれでは
                // 出てこない。シェルが見ているのはクライアント矩形ではなく
                // ウィンドウ矩形だから(実測)。どちらも真因(全画面状態の紛れ込み)を
                // 断てば起きないので、1px詰めは入れていない。2026-08-04。
                if (::GetMonitorInfo(mon, &mi)) params->rgrc[0] = mi.rcWork;
            }

            *result = 0;
            return true;
        }

        // ---- 「どこがタイトルバーか」をWindowsへ教え直す ------------------------
        // WM_NCCALCSIZEで非クライアント領域を潰すと、既定のヒットテストは
        // ほぼ全面がHTCLIENT(ただのクライアント領域)になる。Windowsの
        // ウィンドウ操作は例外なくこのヒットテスト結果を基準に駆動されているため、
        // 教え直さないと
        //   ・ドラッグでの移動、画面端へのスナップ、上端ドラッグでの最大化
        //   ・ダブルクリックでの最大化/復元、シェイク、Alt+Space
        //   ・右クリックのシステムメニュー、四辺四隅でのリサイズ
        // がまとめて効かなくなる。逆にここさえ返せば、これらは全部OSがやってくれる
        // (アプリ側でドラッグ処理を書く必要はない)。
        case WM_NCHITTEST: {
            RECT wr{};
            ::GetWindowRect(msg->hwnd, &wr);
            const int px = GET_X_LPARAM(msg->lParam);
            const int py = GET_Y_LPARAM(msg->lParam);

            // リサイズ枠の厚み。最大化中はリサイズしないので判定ごと省く。
            if (!::IsZoomed(msg->hwnd)) {
                const int border = ::GetSystemMetrics(SM_CXSIZEFRAME)
                                 + ::GetSystemMetrics(SM_CXPADDEDBORDER);
                const bool l = px <  wr.left   + border;
                const bool r = px >= wr.right  - border;
                const bool t = py <  wr.top    + border;
                const bool b = py >= wr.bottom - border;
                if (t && l) { *result = HTTOPLEFT;     return true; }
                if (t && r) { *result = HTTOPRIGHT;    return true; }
                if (b && l) { *result = HTBOTTOMLEFT;  return true; }
                if (b && r) { *result = HTBOTTOMRIGHT; return true; }
                if (t)      { *result = HTTOP;         return true; }
                if (b)      { *result = HTBOTTOM;      return true; }
                if (l)      { *result = HTLEFT;        return true; }
                if (r)      { *result = HTRIGHT;       return true; }
            }

            // メニューバーはHTCAPTIONにしない。ここでHTCAPTIONを返すと押下が
            // 非クライアント扱いになり、環境によっては移動が始まらないうえQt側にも
            // 押下が届かず、代替手段も動けない状態になる。クライアント扱いのまま
            // Qtに押下を拾わせ、MainWindow::beginNativeWindowDrag()から明示的に
            // キャプションドラッグを開始する(そちらのコメント参照)。
            *result = HTCLIENT;
            return true;
        }

        // WM_NCACTIVATE / WM_NCPAINT は横取りしないこと。
        // WM_NCACTIVATEをlParam=-1で処理してもメニュー開閉時のちらつきには効かず、
        // WM_NCPAINTを握りつぶすとWindowsがウィンドウの縁に描く1pxの枠まで消える。

        default:
            break;
        }
    }
#else
    Q_UNUSED(message); Q_UNUSED(wParam); Q_UNUSED(lParam);
    Q_UNUSED(hwndOpaque); Q_UNUSED(result);
#endif
    return false;
}

// メニューバーの空き部分を掴んだときに、Windowsのキャプションドラッグを開始する。
//
// 「ReleaseCapture() してから WM_NCLBUTTONDOWN(HTCAPTION) を送る」のは、クライアント
// 領域を掴んでウィンドウを動かすための定石。以降はOSの標準の移動ループに乗るので、
// Aeroスナップ・画面上端へのドラッグでの最大化・シェイクもそのまま効く。
// WM_NCHITTESTでHTCAPTIONを返す方法より確実で、押下がQt側に届いたうえで始まるため
// 「掴めないのに押下だけ消える」状態にならない。
void MainWindow::beginNativeWindowDrag()
{
#ifdef Q_OS_WIN
    QWindow *handle = windowHandle();
    if (!handle) return;
    auto hwnd = reinterpret_cast<HWND>(handle->winId());
    if (!hwnd) return;
    ::ReleaseCapture(); // Qtが握っているマウスキャプチャを外さないと移動ループに入れない
    ::SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
#endif
}

// holes は「このウィンドウのクライアント座標系(論理px)」で渡す。
// 空ならリージョンを解除して通常のウィンドウへ戻す。
void MainWindow::applyWindowRegion(const QVector<QRect> &holes)
{
#ifdef Q_OS_WIN
    QWindow *handle = windowHandle();
    if (!handle) return;
    auto hwnd = reinterpret_cast<HWND>(handle->winId());
    if (!hwnd) return;

    // 最小化中はGetWindowRect()/ClientToScreen()が実際の表示位置・サイズを返さず、
    // 画面外の特殊な矩形になる。その値でリージョンを作ると復帰後にずれた穴が残り、
    // 中央ウィジェットの縁やタブ列の背景が切り抜かれて見えてしまう。
    // lastAppliedHoles_はあえて更新せず、復帰時(refreshNativeFrame)に貼り直させる。
    if (isMinimized()) {
        WINLOG(QStringLiteral("CALL applyWindowRegion holes=%1 (skipped, minimized)").arg(holes.size()));
        return;
    }

    RECT curWr{};
    ::GetWindowRect(hwnd, &curWr);
    const QRect curRect(curWr.left, curWr.top, curWr.right - curWr.left, curWr.bottom - curWr.top);

    // SetWindowRgn()はウィンドウ全体の再描画を伴うため、リサイズ中など高頻度で
    // 呼ばれると目に見えて重くなる。内容が変わっていないときは何もしない
    // (穴が0個のまま呼ばれ続ける場合も含む。透過タブを使っていなければ常にこの
    //  ケースなので、ここを空判定から外すと毎回ウィンドウ全体が再描画される)。
    if (holes == lastAppliedHoles_) {
        // ただしリージョンはウィンドウ座標系で、ウィンドウのサイズが変わっても
        // 自動では追従しない。穴が同じでもウィンドウ矩形が変わっていれば、
        // 貼ってあるリージョンは古いサイズのままで切り抜き位置がずれている。
        // ここに引っかかるようなら「メニューバーが透明になる」の直接の原因。
        if (!holes.isEmpty() && lastAppliedWindowRect_.isValid() && curRect != lastAppliedWindowRect_)
            WINLOG(QStringLiteral("WARN applyWindowRegion STALE REGION: holes unchanged but window %1 -> %2")
                       .arg(QStringLiteral("(%1,%2 %3x%4)").arg(lastAppliedWindowRect_.x()).arg(lastAppliedWindowRect_.y())
                                .arg(lastAppliedWindowRect_.width()).arg(lastAppliedWindowRect_.height()))
                       .arg(QStringLiteral("(%1,%2 %3x%4)").arg(curRect.x()).arg(curRect.y())
                                .arg(curRect.width()).arg(curRect.height())));
        else
            WINLOG(QStringLiteral("CALL applyWindowRegion holes=%1 (skipped, unchanged)").arg(holes.size()));
        return;
    }
    lastAppliedHoles_     = holes;
    lastAppliedWindowRect_ = curRect;

    if (holes.isEmpty()) {
        WINLOG(QStringLiteral("CALL applyWindowRegion holes=0 *** SetWindowRgn(NULL, redraw) ***"));
        ::SetWindowRgn(hwnd, nullptr, TRUE);
        return;
    }

    const RECT wr = curWr;

    // WM_NCCALCSIZEを通した後の「実際の」クライアント原点。Qtが持っている
    // フレームマージンは使わない(上のコメント参照)。
    POINT clientOrigin{0, 0};
    ::ClientToScreen(hwnd, &clientOrigin);
    const int offsetX = clientOrigin.x - wr.left;
    const int offsetY = clientOrigin.y - wr.top;

    // 論理px -> 物理px。1つのウィンドウ内では倍率が一定なので単純な掛け算でよい。
    const qreal dpr = handle->devicePixelRatio();

    HRGN region = ::CreateRectRgn(0, 0, wr.right - wr.left, wr.bottom - wr.top);
    for (const QRect &r : holes) {
        // 内側へ丸める(左上は切り上げ、右下は切り捨て)。四捨五入すると倍率次第で
        // ページより1px大きい穴になり、タブバーやペインの縁に透明の隙間が見える。
        HRGN hole = ::CreateRectRgn(offsetX + int(std::ceil (r.left()      * dpr)),
                                     offsetY + int(std::ceil (r.top()       * dpr)),
                                     offsetX + int(std::floor((r.left() + r.width())  * dpr)),
                                     offsetY + int(std::floor((r.top()  + r.height()) * dpr)));
        ::CombineRgn(region, region, hole, RGN_DIFF);
        ::DeleteObject(hole);
    }
    WINLOG(QStringLiteral("CALL applyWindowRegion holes=%1 win=%2 clientOff=(%3,%4) dpr=%5 *** SetWindowRgn(redraw) ***")
               .arg(holes.size()).arg(rectStr(wr)).arg(offsetX).arg(offsetY).arg(dpr));
    // SetWindowRgn成功後はリージョンの所有権がOSへ移るため、こちらでは破棄しない。
    ::SetWindowRgn(hwnd, region, TRUE);
#else
    // Windows以外ではQt任せ(フレームマージンのずれも起きないため素直に使える)。
    if (holes.isEmpty()) { clearMask(); return; }
    QRegion region(rect());
    for (const QRect &r : holes) region -= r;
    setMask(region);
#endif
}
