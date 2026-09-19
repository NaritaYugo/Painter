#include "widgets/NativeWindowLog.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>

namespace WinLog {
namespace {

int levelOf()
{
    // 環境変数は起動中に変わらないので一度だけ読む。
    static const int level = [] {
        const QByteArray v = qgetenv("TIEPOLO_WINLOG");
        if (v.isEmpty()) return 0;
        bool ok = false;
        const int n = v.toInt(&ok);
        return ok ? n : 1; // 「TIEPOLO_WINLOG=on」のような書き方も有効扱いにする
    }();
    return level;
}

QString resolvePath()
{
    const QByteArray custom = qgetenv("TIEPOLO_WINLOG_FILE");
    if (!custom.isEmpty()) return QString::fromLocal8Bit(custom);
    return QDir::temp().filePath(QStringLiteral("tiepolo-winlog.txt"));
}

QFile &logFile()
{
    static QFile f(resolvePath());
    static bool opened = [] {
        // 追記にする。切り分けは「設定を変えて何度か起動し、見比べる」作業なので、
        // 毎回作り直すと前の回が消えてしまう(実際それで2回分を失った)。
        // 代わりに起動ごとの区切りを入れて、1ファイルで見比べられるようにする。
        if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return false;
        const QString sep = QStringLiteral(
            "\n==================== RUN %1  WINLOG=%2 ====================\n")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
            .arg(qEnvironmentVariable("TIEPOLO_WINLOG"));
        f.write(sep.toUtf8());
        f.flush();
        return true;
    }();
    Q_UNUSED(opened);
    return f;
}

// 起動からの経過ms。点滅は「短時間に何回起きたか」が肝なので、
// 壁時計より経過時間のほうが読みやすい。
qint64 elapsedMs()
{
    static QElapsedTimer t = [] { QElapsedTimer e; e.start(); return e; }();
    return t.elapsed();
}

} // namespace

int  level()   { return levelOf(); }
bool enabled() { return levelOf() >= 1; }
bool verbose() { return levelOf() >= 2; }
QString path() { return resolvePath(); }

void write(const QString &line)
{
    QFile &f = logFile();
    if (!f.isOpen()) return;
    const QString out = QStringLiteral("[%1] %2\n")
                            .arg(elapsedMs(), 7)
                            .arg(line);
    f.write(out.toUtf8());
    f.flush(); // 強制終了しても末尾が残るように毎行フラッシュする
}

} // namespace WinLog
