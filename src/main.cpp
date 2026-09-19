#include "widgets/MainWindow.cpp"
#include "components/ThemeColors.h"
#include "backend/ShaderCache.h"

#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QSettings>
#include <QElapsedTimer>
#include <QHash>
#include <QKeyEvent>
#include <QTimer>
#include <QSurfaceFormat>
#include <QWindow>
#include <cstdio>
#include <memory>

#ifdef TIEPOLO_PRO_BUILD
#include "backend/LicenseManager.h"
#endif

// "QWindowsWindow::setGeometry: Unable to set geometry ..." は、ダイアログの
// 初期ジオメトリ確定時にWindows側のWM_GETMINMAXINFOと競合して出る無害な警告
// (Qt側の既知の挙動で実害はない)。ターミナルを汚さないようここだけ抑制し、
// それ以外のメッセージは標準のデフォルトハンドラと同じ形式でstderrへ流す。
static void filteredMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context);
    if (msg.contains("QWindowsWindow::setGeometry")) return;

    const QByteArray localMsg = msg.toLocal8Bit();
    fprintf(stderr, "%s\n", localMsg.constData());
    fflush(stderr);
    if (type == QtFatalMsg) abort();
}

// ===========================================================================
// 【計測用】どのウィジェットが再描画されているかを数える
// ---------------------------------------------------------------------------
// TIEPOLO_PAINTLOG=1 のときだけ有効(未設定ならフィルタ自体を入れないので
// コストは無い)。1秒ごとに、その間に来た QEvent::Paint をクラス名別に集計して
// 1行出す。
//
// 「1フレームが重い」ときに、キャンバス以外のウィジェットが巻き込まれて
// 描き直されていないかを確かめるためのもの。ビュー変換のカクつきを追ったときは、
// これでドラッグ中もNavigatorDockと周辺のボタン類が毎秒6回描き直されていることが
// 分かった(GLWidget::setupToolContext の requestRepaint のコメント参照)。
// ===========================================================================
class PaintTally : public QObject
{
public:
    explicit PaintTally(QObject *parent) : QObject(parent)
    {
        auto *t = new QTimer(this);
        t->setInterval(1000);
        connect(t, &QTimer::timeout, this, [this] {
            if (counts_.isEmpty()) return;
            QStringList parts;
            for (auto it = counts_.cbegin(); it != counts_.cend(); ++it)
                parts << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
            std::sort(parts.begin(), parts.end());
            WINLOG(QStringLiteral("PAINTTALLY %1").arg(parts.join(QLatin1Char(' '))));
            counts_.clear();
        });
        t->start();
    }

    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::Paint)
            counts_[QString::fromLatin1(o->metaObject()->className())]++;
        return false;
    }

private:
    QHash<QString, int> counts_;
};

// ---------------------------------------------------------------------------
// 起動スプラッシュの絵を作る
// ---------------------------------------------------------------------------
// 実測(プロセス起動→ウィンドウが実際に表示される)で約1.5秒あり、その間画面には
// 何も出ない。何を待たされているのか分からないので、その間だけ小さなパネルを出す。
// 画像ファイルは持たず、テーマの色でその場で描く(テーマを切り替えても追従する)。
static QPixmap makeSplashPixmap(qreal dpr, const QString &message)
{
    const int w = 300, h = 108;
    QPixmap pm(qRound(w * dpr), qRound(h * dpr));
    pm.setDevicePixelRatio(dpr);
    // 不透明で描く。角丸+半透明(WA_TranslucentBackground)も試したが、
    // QSplashScreenでは何も表示されなくなったので四角にしてある。
    pm.fill(Theme::bgPanel);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    p.setPen(QPen(Theme::bgButton, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0));

    QPixmap icon(":/icons/app/app_icon.png");
    if (!icon.isNull()) {
        const int side = 44;
        QPixmap scaled = icon.scaled(qRound(side * dpr), qRound(side * dpr),
                                      Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        p.drawPixmap(QPoint(26, (h - side) / 2), scaled);
    }

    const int textX = 92;
    QFont nameFont = p.font();
    nameFont.setPointSizeF(nameFont.pointSizeF() + 3.0);
    nameFont.setBold(true);
    p.setFont(nameFont);
    p.setPen(Theme::text);
#ifdef TIEPOLO_PRO_BUILD
    p.drawText(QRect(textX, 32, w - textX - 20, 26), Qt::AlignLeft | Qt::AlignVCenter, "Tiepolo Pro");
#else
    p.drawText(QRect(textX, 32, w - textX - 20, 26), Qt::AlignLeft | Qt::AlignVCenter, "Tiepolo");
#endif

    QFont msgFont = p.font();
    msgFont.setPointSizeF(msgFont.pointSizeF() - 3.0);
    msgFont.setBold(false);
    p.setFont(msgFont);
    p.setPen(Theme::textDisabled);
    p.drawText(QRect(textX, 56, w - textX - 20, 22), Qt::AlignLeft | Qt::AlignVCenter, message);
    return pm;
}

// 起動中に出す小さなウィンドウ。
//
// QSplashScreen を使うと、この環境では最後まで画面に出てこなかった
// (ウィンドウ自体はEnumWindowsで見えるがWS_VISIBLEが立つのが構築完了後。
//  半透明をやめる/最前面を明示する/show()直後にrepaint()+processEvents()する/
//  構築をsingleShot(0)で後回しにする、のどれでも変わらず)。
// 素のQWidgetなら普通に出るので、自前で持つ。
class SplashWindow : public QWidget
{
public:
    explicit SplashWindow(qreal dpr)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
          dpr_(dpr), pm_(makeSplashPixmap(dpr, QStringLiteral("起動中...")))
    {
        setAttribute(Qt::WA_ShowWithoutActivating);
        const QSize logical = pm_.size() / pm_.devicePixelRatio();
        setFixedSize(logical);
        if (QScreen *sc = QGuiApplication::primaryScreen()) {
            const QRect g = sc->availableGeometry();
            move(g.center() - QPoint(logical.width() / 2, logical.height() / 2));
        }
    }

    void setMessage(const QString &msg)
    {
        pm_ = makeSplashPixmap(dpr_, msg);
        update();
        repaint(); // 直後に重い処理へ入っても文字が変わって見えるように
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.drawPixmap(0, 0, pm_);
    }

private:
    qreal   dpr_;
    QPixmap pm_;
};

// 起動が終わるまで利用者の入力を捨てるフィルタ。
//
// 事前コンパイル中はUIスレッド自体は動いているが、ドライバがGPUを占有するため
// 画面への反映が大きく遅れる(実測: この間のクリックが画面に出るまで3.9秒。
// 終わった後は45ms前後)。操作を受け付けてしまうと「押したのに何も起きない」
// 状態になり、二重に押されてしまうので、準備が終わるまで入力自体を止める。
//
// 止めるのはポインタ・キー・ホイールだけ。
//
// ただし Alt+F4 だけは通す。Windowsではこれを DefWindowProc が
// WM_SYSCOMMAND(SC_CLOSE) へ変換して初めてWM_CLOSEになるので、ここでキーイベントを
// 捨ててしまうと変換自体が起きず、起動が終わるまでアプリを閉じられなくなる
// (実測で確認)。待たされている間に閉じられないのは困るので例外にする。
class StartupInputBlocker : public QObject
{
public:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        Q_UNUSED(obj);
        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::KeyRelease: {
            const auto *ke = static_cast<QKeyEvent *>(event);
            if (ke->key() == Qt::Key_F4 && ke->modifiers().testFlag(Qt::AltModifier))
                return false; // 閉じる操作は通す
            return true;
        }
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::ContextMenu:
        case QEvent::TabletPress:
        case QEvent::TabletRelease:
        case QEvent::TabletMove:
            return true;   // 捨てる
        default:
            return false;
        }
    }
};

int main(int argc, char *argv[])
{
    qInstallMessageHandler(filteredMessageHandler);

    // タブごとに独立したQOpenGLWidget(GLWidget)を動的に生成・破棄するため、GL
    // コンテキストの共有をウィジェットの生存期間に依存しないグローバルな共有コンテキスト
    // にしておく(Qt公式ドキュメント推奨の設定)。これが無いと、あるQOpenGLWidgetを
    // 破棄した際に暗黙の共有グループごと壊れ、他のタブの描画がクラッシュしうる。
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // VSync(スワップ同期)を無効化する。有効(既定=1)だと、描画のたびにバッファ表示が
    // 垂直帰線を待って最大1フレーム(~16ms)ブロックする。ストローク中は入力ハンドラの
    // 中から同期描画(repaint)を呼んでペン先へ追従させているため、このブロックがそのまま
    // 「描くたびに16ms入力が止まる」=線が超スロー・カクつく原因になっていた。0にすると
    // 待たずに即表示され、ペンストロークが滑らかに追従する(多少のティアリングは
    // 描画中のみで実害はほぼ無い)。
    {
        QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
        fmt.setSwapInterval(0);

        // 【試して駄目だったこと】setSwapBehavior(TripleBuffer) を試したが、
        // ビュー変換中の1フレームは 22.5〜25.3ms → 21.6〜22.9ms で、実行ごとの
        // ばらつきに埋もれる程度しか変わらなかった(この環境のWGLドライバは
        // バッファ枚数の要求を見ていないと思われる。2026-08-12に実測して撤回)。

        // 【試して駄目だったこと】この環境で実際に確保されるGLの面は alpha=8 rgb=8/8/8。
        // 「描かれなかった画素がアルファ0=透明として残る」症状の元を断とうとして
        // setAlphaBufferSize(0)を試したが、ドライバはアルファを外す代わりに
        // RGB10A2(alpha=2 rgb=10/10/10)を選んでくる。RGBのビット数まで8で明示しても
        // 同じだった。タイルがRGBA8のこのアプリで10bitの面に変わるほうが害が大きいので
        // 入れていない(2026-08-03に実測して撤回)。
        QSurfaceFormat::setDefaultFormat(fmt);
    }

    QApplication app(argc, argv);
    QApplication::setOrganizationName("pwxwx");
    QApplication::setApplicationName("Tiepolo");

    // 【計測用】PaintTally のコメント参照
    if (qEnvironmentVariableIsSet("TIEPOLO_PAINTLOG"))
        app.installEventFilter(new PaintTally(&app));

    // MainWindow構築(メニュー生成)より前に、保存済みのテーマ設定を反映しておく。
    const auto savedTheme = (Theme::Name)QSettings().value("ui/theme", (int)Theme::Name::Dark).toInt();
    Theme::setTheme(savedTheme);
    Theme::applyToApplication(app);
    app.setWindowIcon(QIcon(":/icons/app/app_icon.png"));

    // 起動中の表示。ここから MainWindow の構築と表示までが実測で約1.5秒あり、
    // その間まったく何も出ないので、待っていることが分かるように出しておく。
    // テーマを反映した後(色を使うため)に作ること。
    SplashWindow splash(app.primaryScreen() ? app.primaryScreen()->devicePixelRatio() : 1.0);
    splash.show();

    // 準備が終わるまで入力を止める。理由はStartupInputBlockerのコメント。
    StartupInputBlocker inputBlocker;
    app.installEventFilter(&inputBlocker);

    // 【重要】MainWindowの構築は「スプラッシュが実際に画面へ出てから」始める。
    //
    // Qtのshow()は「表示すると決める」だけで、OSのウィンドウが実際に可視になるのは
    // イベントループがその要求を処理したとき。構築を続けて呼ぶと1.5秒間ループへ
    // 戻らないので、スプラッシュはその間ずっと不可視のまま終わる
    // (実測: EnumWindowsで見ると WS_VISIBLE が立つのは構築が終わった後だった。
    //  show()の直後に processEvents()/repaint() を挟んでも、singleShot(0)で
    //  構築を次の反復へ回しても、どちらも間に合わなかった)。
    //
    // 出たかどうかは QWindow::isExposed() で分かるので、それを短い間隔で見て、
    // 出たら構築を始める。何らかの理由で出ないままでも起動は続けたいので、
    // 上限時間を過ぎたら構わず構築する。
    //
    // w は exec() を抜けるまで生かす必要があるのでここで持つ(app より後に
    // 宣言してあるので、破棄は w → app の順になる)。
    std::unique_ptr<MainWindow> w;
    QElapsedTimer splashClock;
    splashClock.start();
    auto *waitForSplash = new QTimer(&app);
    waitForSplash->setInterval(16);
    QObject::connect(waitForSplash, &QTimer::timeout, &app, [&, waitForSplash] {
        const QWindow *h = splash.windowHandle();
        const bool onScreen = (h && h->isExposed());
        if (!onScreen && splashClock.elapsed() < 400) return; // まだ。ただし待ちすぎない
        waitForSplash->stop();
        waitForSplash->deleteLater();
#ifdef TIEPOLO_PRO_BUILD
        // %APPDATA%/pwxwx/Tiepolo/license.tiepololicense があれば読み込んで検証する。
        // 無い/検証失敗時はLicenseManager::instance().isUnlocked()がfalseのままになるだけで、
        // 起動は継続する(Pro機能が使えない無料版相当の動作になる)。
        LicenseManager::instance().loadDefault();
#endif
        w = std::make_unique<MainWindow>();
        // ウィンドウの位置・サイズ(最大化状態を含む)は、MainWindowのコンストラクタ内で
        // 既に決まっている(保存済み設定があればloadSettings()のrestoreGeometry()、
        // 無ければresetDockLayout()のshowMaximized())。ここでは単に表示するだけでよい。
        w->show();

        // ウィンドウは出たが、ここからシェーダーの事前コンパイルが数秒走る。その間は
        // ドライバがGPUを占有していて操作しても画面に出てこないので、起動画面と
        // 入力の停止はそのまま続ける(閉じるだけはいつでもできる)。
        splash.setMessage(QStringLiteral("描画の準備中..."));
        splash.raise();

        // 準備ができたら起動画面を消して操作を受け付ける。
        // 何かの理由で通知が来なくても操作不能のままにはしないよう、保険で
        // 30秒後にも同じことをする。
        auto *release = new QTimer(w.get());
        release->setSingleShot(true);
        release->setInterval(30000);
        // 解除済みかは専用のフラグで持つこと。「起動画面が見えているか」で判定すると、
        // 利用者が起動画面を閉じてしまった場合に解除が走らず、入力が止まったままになる。
        auto released = std::make_shared<bool>(false);
        auto finishStartup = [&splash, &inputBlocker, &app, release, released] {
            if (*released) return;
            *released = true;
            release->stop();
            app.removeEventFilter(&inputBlocker);
            splash.close();
        };
        QObject::connect(release, &QTimer::timeout, w.get(), finishStartup);
        release->start();
        ShaderCache::setReadyForUseCallback(finishStartup);

        // シェーダーを裏で先に用意しておく(ShaderCache.h の説明を参照)。
        QTimer::singleShot(0, w.get(), [] { ShaderCache::startBackgroundWarmUp(); });

        // コマンドライン引数で渡されたファイル(OSの「プログラムから開く」や
        // ドラッグ&ドロップ、ファイルの関連付け起動を含む)を開く。
        const QStringList fileArgs = QCoreApplication::arguments().mid(1);
        if (!fileArgs.isEmpty())
            QTimer::singleShot(0, w.get(), [&w, fileArgs]() { w->openFilesFromArgs(fileArgs); });
    });
    waitForSplash->start();

    return app.exec();
}
