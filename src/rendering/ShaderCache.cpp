#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "rendering/ShaderCache.h"
#include "app/NativeWindowLog.h"

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QMutex>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QSet>
#include <QThread>
#include <QWaitCondition>

namespace {

QMutex                                  g_mutex;
QHash<QString, QOpenGLShaderProgram *>  g_cache;    // 完成したもの
QSet<QString>                           g_building; // 今どこかのスレッドが作っているもの
QWaitCondition                          g_done;     // 1本出来るたびに起こす

const char *kRenderKey = ":/shaders/render/render.vert+frag";

QByteArray loadSource(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open shader:" << path;
        return {};
    }
    return f.readAll();
}

// 共通ヘッダは全シェーダーへ貼り込むので一度だけ読む。
const QByteArray &commonHeader()
{
    static const QByteArray header = loadSource(QStringLiteral(":/shaders/header/common.glsl"));
    return header;
}

// 長い待ち/コンパイルの合間に、OSへ「このスレッドは生きている」と答える。
//
// Windowsは「ウィンドウを持つスレッドが5秒間メッセージを取りに来ない」とアプリを
// ハングとみなし、ゴーストウィンドウ(タイトルが「(応答なし)」に変わる薄暗い
// 身代わり)へ差し替える。PeekMessage() は、キューに溜まった自前のメッセージを
// 取り出さない(PM_NOREMOVE)場合でも、他スレッドから送られてきたメッセージだけは
// 処理する。ハング判定はまさにその応答性で行われているので、これを挟むだけで
// 判定に引っかからなくなる。
//
// QCoreApplication::processEvents() は使わないこと。あちらは自前のイベントも
// 動かすため、GL初期化が途中の状態で paintGL やマウス操作が割り込みうる。
//
// GUIスレッド以外にはウィンドウが無いので何もしない。
void keepAliveForOS()
{
#ifdef Q_OS_WIN
    if (!qGuiApp || QThread::currentThread() != qGuiApp->thread()) return;
    MSG msg;
    ::PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
#endif
}

// キャッシュから引く。無ければ build() で作る。
// 別のスレッドが同じものを作っている最中なら、二重に作らず出来上がりを待つ。
QOpenGLShaderProgram *getOrBuild(const QString &key,
                                 const std::function<QOpenGLShaderProgram *()> &build)
{
    {
        QMutexLocker lock(&g_mutex);
        forever {
            const auto it = g_cache.constFind(key);
            if (it != g_cache.constEnd()) return *it;
            if (!g_building.contains(key)) break;   // 誰も作っていない → 自分が作る
            // 作成中。待つ。GUIスレッドで待つ場合に「応答なし」にならないよう、
            // 短く区切って待ち、その合間にOSへ応答する。
            g_done.wait(&g_mutex, 500);
            keepAliveForOS();
        }
        g_building.insert(key);
    }

    QElapsedTimer t; t.start();
    QOpenGLShaderProgram *prog = build();
    const qint64 ms = t.elapsed();

    QMutexLocker lock(&g_mutex);
    g_cache.insert(key, prog);
    g_building.remove(key);
    g_done.wakeAll();
    if (WinLog::enabled()) {
        WINLOG(QStringLiteral("PERF shader %1ms %2 (%3)")
                   .arg(ms).arg(key)
                   .arg(QThread::currentThread() == qGuiApp->thread() ? "GUI" : "背景"));
    }
    return prog;
}

// ---------------------------------------------------------------------------
// 事前コンパイル用のワーカースレッド
// ---------------------------------------------------------------------------
QAtomicInt   g_stopWarmUp = 0;   // 終了要求
QThread     *g_warmUpThread = nullptr;
std::function<void()> g_readyForUse;   // 要るぶんが揃ったらメインスレッドで呼ぶ

// 「キャンバスを開くのに要るシェーダーが揃った」ことをメインスレッドへ知らせる。
// 2度呼ばれないよう、呼んだら手放す。
void notifyReadyForUse()
{
    if (!g_readyForUse) return;
    auto cb = g_readyForUse;
    g_readyForUse = nullptr;
    if (!qGuiApp) return;
    QMetaObject::invokeMethod(qGuiApp, [cb] { cb(); }, Qt::QueuedConnection);
}

class WarmUpThread : public QThread
{
public:
    WarmUpThread(QOpenGLContext *ctx, QOffscreenSurface *surface)
        : ctx_(ctx), surface_(surface) {}

protected:
    void run() override
    {
        QElapsedTimer t; t.start();
        if (!ctx_->makeCurrent(surface_)) {
            WINLOG(QStringLiteral("PERF shader warm-up: makeCurrent に失敗したので中止"));
            notifyReadyForUse(); // 起動画面を出しっぱなしにしない
            return;
        }
        const QStringList paths     = ShaderCache::warmUpOrder();
        const int         essential = ShaderCache::essentialWarmUp().size();
        int built = 0;
        for (const QString &path : paths) {
            // 1本ごとに終了要求を見る。コンパイル中の1本はドライバの処理なので
            // 途中で止められない ― 打ち切れるのはここだけ。
            if (g_stopWarmUp.loadAcquire()) {
                WINLOG(QStringLiteral("PERF shader warm-up: 終了要求により%1本で中断").arg(built));
                break;
            }
            if (path == QLatin1String(kRenderKey)) ShaderCache::render();
            else                                   ShaderCache::compute(path);
            built++;

            // 要るぶんが揃った時点で操作を解禁する。残り(フィルタ・変形など)は
            // このまま裏で作り続ける ― 1本あたり最大133msなので、操作しながらでも
            // 引っかかりにならない。
            if (built == essential) {
                WINLOG(QStringLiteral("PERF shader warm-up: 必須%1本が揃った (%2ms) ― 残りは背景で継続")
                           .arg(essential).arg(t.elapsed()));
                notifyReadyForUse();
            }
        }
        ctx_->doneCurrent();
        // 後始末はメインスレッドでやるので、コンテキストの持ち主を戻しておく。
        ctx_->moveToThread(qGuiApp->thread());
        WINLOG(QStringLiteral("PERF shader warm-up: %1本を背景で用意 (%2ms)")
                   .arg(built).arg(t.elapsed()));
        notifyReadyForUse();
    }

private:
    QOpenGLContext    *ctx_;
    QOffscreenSurface *surface_;
};

} // namespace

// ===========================================================================
namespace ShaderCache {

QOpenGLShaderProgram *compute(const QString &path)
{
    return getOrBuild(path, [&path]() -> QOpenGLShaderProgram * {
        // 親を持たせない(このキャッシュが所有する)
        auto *prog = new QOpenGLShaderProgram();
        QByteArray src = loadSource(path).replace("//COMMON_INCLUDE", commonHeader());
        prog->addShaderFromSourceCode(QOpenGLShader::Compute, src);
        if (!prog->link())
            qWarning() << path << ":" << prog->log();
        keepAliveForOS();
        return prog;
    });
}

QOpenGLShaderProgram *render()
{
    return getOrBuild(QLatin1String(kRenderKey), []() -> QOpenGLShaderProgram * {
        auto *prog = new QOpenGLShaderProgram();
        const QByteArray vs = loadSource(QStringLiteral(":/shaders/render/render.vert"))
                                  .replace("//COMMON_INCLUDE", commonHeader());
        const QByteArray fs = loadSource(QStringLiteral(":/shaders/render/render.frag"))
                                  .replace("//COMMON_INCLUDE", commonHeader());
        prog->addShaderFromSourceCode(QOpenGLShader::Vertex,   vs);
        prog->addShaderFromSourceCode(QOpenGLShader::Fragment, fs);
        if (!prog->link())
            qWarning() << "render:" << prog->log();
        keepAliveForOS();
        return prog;
    });
}

QStringList ShaderCache::essentialWarmUp()
{
    // キャンバスを1枚開いて描き始めるのに要るものだけ。ここが揃った時点で
    // 操作を解禁する(main.cpp)。重いのはこの中に集中している ―
    // 実測: render.vert+frag 約3.0秒 / belowComposite 約1.0秒 /
    //       composite 約0.39秒 / bake 約0.2秒(残り4本は合計0.07秒)。
    return {
        QLatin1String(kRenderKey),
        QStringLiteral(":/shaders/render/belowComposite.comp"),
        QStringLiteral(":/shaders/render/composite.comp"),
        QStringLiteral(":/shaders/paint/bake.comp"),
        QStringLiteral(":/shaders/paint/stroke.comp"),
        QStringLiteral(":/shaders/paint/brushState.comp"),
        QStringLiteral(":/shaders/paint/maskclear.comp"),
        QStringLiteral(":/shaders/paint/layerclear.comp"),
    };
}

QStringList ShaderCache::warmUpOrder()
{
    QStringList paths = essentialWarmUp();
    // ここから下は、その機能を使うまで要らないもの。1本あたり最大133ms、
    // 合計でも約1.0秒なので、操作しながら裏で作っても邪魔にならない。
    paths += QStringList{
        QStringLiteral(":/shaders/fill/wall.comp"),
        QStringLiteral(":/shaders/fill/jfaInitOuter.comp"),
        QStringLiteral(":/shaders/fill/jfaInitInner.comp"),
        QStringLiteral(":/shaders/fill/jfa.comp"),
        QStringLiteral(":/shaders/fill/jfaFinalize.comp"),
        QStringLiteral(":/shaders/paint/blur.comp"),
        QStringLiteral(":/shaders/paint/gaussianBlurFilter.comp"),
        QStringLiteral(":/shaders/paint/mosaicReduce.comp"),
        QStringLiteral(":/shaders/paint/mosaicFilter.comp"),
        QStringLiteral(":/shaders/paint/motionBlurFilter.comp"),
        QStringLiteral(":/shaders/paint/noiseFilter.comp"),
        QStringLiteral(":/shaders/render/gaussianBlurLayer.comp"),
        QStringLiteral(":/shaders/render/motionBlurLayer.comp"),
        QStringLiteral(":/shaders/render/mosaicReduceLayer.comp"),
        QStringLiteral(":/shaders/render/mosaicLayer.comp"),
        QStringLiteral(":/shaders/render/noiseLayer.comp"),
        QStringLiteral(":/shaders/paint/warp.comp"),
        QStringLiteral(":/shaders/paint/transform.comp"),
        QStringLiteral(":/shaders/paint/freeTransform.comp"),
        QStringLiteral(":/shaders/paint/hueSatLight.comp"),
        QStringLiteral(":/shaders/paint/brightnessContrast.comp"),
        QStringLiteral(":/shaders/paint/colorBalance.comp"),
        QStringLiteral(":/shaders/paint/toneCurve.comp"),
    };
#ifdef TIEPOLO_PRO_BUILD
    paths << QStringLiteral(":/shaders/paint/chromaticAberrationFilter.comp")
          << QStringLiteral(":/shaders/render/chromaticAberrationLayer.comp")
          << QStringLiteral(":/shaders/paint/lensBlurFilter.comp")
          << QStringLiteral(":/shaders/render/lensBlurLayer.comp")
          << QStringLiteral(":/shaders/paint/gradientMap.comp");
#endif
    return paths;
}

void setReadyForUseCallback(std::function<void()> cb)
{
    g_readyForUse = std::move(cb);
}

void startBackgroundWarmUp()
{
    static bool started = false;
    if (started) return;
    started = true;
    // 切り分け用の逃げ道。事前コンパイルを止めると、以前と同じ
    // 「最初のキャンバスでまとめてコンパイル」の動きに戻る。
    if (qEnvironmentVariableIntValue("TIEPOLO_NO_SHADER_WARMUP")) {
        WINLOG(QStringLiteral("PERF shader warm-up: TIEPOLO_NO_SHADER_WARMUP により行わない"));
        notifyReadyForUse(); return;
    }

    // 共有グループの親。AA_ShareOpenGLContexts が有効なら、ここで(まだ無ければ)
    // 作られる。カレントにはできない決まりなので、これと共有する別のコンテキストを作る。
    QOpenGLContext *share = QOpenGLContext::globalShareContext();
    if (!share) {
        WINLOG(QStringLiteral("PERF shader warm-up: 共有コンテキストが無いので行わない"));
        notifyReadyForUse(); return;
    }

    // QOffscreenSurface はメインスレッドで作る決まり(使うのは別スレッドでよい)。
    auto *surface = new QOffscreenSurface();
    surface->setFormat(share->format());
    surface->create();
    if (!surface->isValid()) {
        WINLOG(QStringLiteral("PERF shader warm-up: オフスクリーン面を作れないので行わない"));
        delete surface;
        notifyReadyForUse(); return;
    }

    auto *ctx = new QOpenGLContext();
    ctx->setFormat(share->format());
    ctx->setShareContext(share);
    if (!ctx->create()) {
        WINLOG(QStringLiteral("PERF shader warm-up: コンテキストを作れないので行わない"));
        delete ctx;
        delete surface;
        notifyReadyForUse(); return;
    }

    auto *thread = new WarmUpThread(ctx, surface);
    g_warmUpThread = thread;
    ctx->moveToThread(thread);
    QObject::connect(thread, &QThread::finished, qGuiApp, [thread, ctx, surface] {
        delete ctx;       // run() の最後にメインスレッドへ返してある
        delete surface;
        g_warmUpThread = nullptr;
        thread->deleteLater();
    });
    // 終了時、Qtが共有コンテキストを壊すより前に必ず止める(stopBackgroundWarmUp参照)。
    QObject::connect(qGuiApp, &QCoreApplication::aboutToQuit, qGuiApp, [] { stopBackgroundWarmUp(); });
    // 描画やUIの邪魔をしないよう、優先度は下げておく。
    thread->start(QThread::LowPriority);
}

void stopBackgroundWarmUp()
{
    QThread *thread = g_warmUpThread;
    if (!thread) return;
    g_stopWarmUp.storeRelease(1);
    QElapsedTimer t; t.start();
    thread->wait();
    WINLOG(QStringLiteral("PERF shader warm-up: 終了待ち %1ms").arg(t.elapsed()));
}

} // namespace ShaderCache
