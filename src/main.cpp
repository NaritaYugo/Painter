#include "app/MainWindow.h"
#include "components/ThemeColors.h"
#include "rendering/ShaderCache.h"

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
#include "licensing/LicenseManager.h"
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

// 起動スプラッシュ
static QPixmap makeSplashPixmap(qreal dpr, const QString &message)
{
    const int w = 300, h = 108;
    QPixmap pm(qRound(w * dpr), qRound(h * dpr));
    pm.setDevicePixelRatio(dpr);
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

// 起動が終わるまで利用者の入力を捨てるフィルタ

// Alt+F4 だけは通す
// このキーイベントを捨てると起動が終わるまでアプリを閉じられなくなる
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

    // タブごとに独立したQOpenGLWidget(CanvasWidget)を動的に生成・破棄するため
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // VSyncを無効化する
    // 有効だと、ストローク中の線の追従が遅れる
    {
        QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
        fmt.setSwapInterval(0);

        // 背景が透明になることがある
        // setSwapBehaviorは意味なし
        // setAlphaBufferSize(0)も意味なし
        QSurfaceFormat::setDefaultFormat(fmt);
    }

    QApplication app(argc, argv);
    QApplication::setOrganizationName("pwxwx");
    QApplication::setApplicationName("Tiepolo");

    // 計測用
    if (qEnvironmentVariableIsSet("TIEPOLO_PAINTLOG"))
        app.installEventFilter(new PaintTally(&app));

    // MainWindow構築より前に、保存済みのテーマ設定を反映しておく
    const auto savedTheme = (Theme::Name)QSettings().value("ui/theme", (int)Theme::Name::Dark).toInt();
    Theme::setTheme(savedTheme);
    Theme::applyToApplication(app);
    app.setWindowIcon(QIcon(":/icons/app/app_icon.png"));

    // 起動中の表示
    SplashWindow splash(app.primaryScreen() ? app.primaryScreen()->devicePixelRatio() : 1.0);
    splash.show();

    StartupInputBlocker inputBlocker;
    app.installEventFilter(&inputBlocker);

    // MainWindowの構築は「スプラッシュが実際に画面へ出てから」始める
    //
    // Qtのshow()は「表示すると決める」だけで、OSのウィンドウが実際に可視になるのは
    // イベントループがその要求を処理したとき。構築を続けて呼ぶと1.5秒間ループへ
    // 戻らないので、スプラッシュはその間ずっと不可視のまま終わる
    //
    // 出たかどうかは QWindow::isExposed() で分かるので、それを短い間隔で見て、
    // 出たら構築を始める。何らかの理由で出ないままでも起動は続けたいので、
    // 上限時間を過ぎたら構わず構築する。
    //
    // w は exec() を抜けるまで生かす必要があるのでここで持つ
    std::unique_ptr<MainWindow> w;
    QElapsedTimer splashClock;
    splashClock.start();
    auto *waitForSplash = new QTimer(&app);
    waitForSplash->setInterval(16);
    QObject::connect(waitForSplash, &QTimer::timeout, &app, [&, waitForSplash] {
        const QWindow *h = splash.windowHandle();
        const bool onScreen = (h && h->isExposed());
        if (!onScreen && splashClock.elapsed() < 400) return;
        waitForSplash->stop();
        waitForSplash->deleteLater();
#ifdef TIEPOLO_PRO_BUILD
        LicenseManager::instance().loadDefault();
#endif
        w = std::make_unique<MainWindow>();
        // ウィンドウの配置はMainWindowのコンストラクタ内で既に決まっている
        // ここでは単に表示するだけ
        w->show();

        // シェーダーの事前コンパイル
        splash.setMessage(QStringLiteral("描画の準備中..."));
        splash.raise();

        // 準備ができたら起動画面を消して操作を受け付ける
        auto *release = new QTimer(w.get());
        release->setSingleShot(true);
        release->setInterval(30000);

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

        // シェーダーを裏で先に用意しておく
        QTimer::singleShot(0, w.get(), [] { ShaderCache::startBackgroundWarmUp(); });

        // コマンドライン引数で渡されたファイルを開く
        const QStringList fileArgs = QCoreApplication::arguments().mid(1);
        if (!fileArgs.isEmpty())
            QTimer::singleShot(0, w.get(), [&w, fileArgs]() { w->openFilesFromArgs(fileArgs); });
    });
    waitForSplash->start();

    return app.exec();
}
