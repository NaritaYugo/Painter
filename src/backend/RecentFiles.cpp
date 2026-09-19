#include "backend/RecentFiles.h"

#include <QSettings>
#include <QFileInfo>

static const char *kSettingsKey = "recentFiles/paths";

QStringList RecentFiles::list()
{
    QSettings settings;
    QStringList paths = settings.value(kSettingsKey).toStringList();

    QStringList existing;
    for (const QString &p : paths) {
        if (QFileInfo::exists(p))
            existing.append(p);
    }
    return existing;
}

void RecentFiles::touch(const QString &path)
{
    QSettings settings;
    QStringList paths = settings.value(kSettingsKey).toStringList();

    paths.removeAll(path);
    paths.prepend(path);
    while (paths.size() > kMaxStored)
        paths.removeLast();

    settings.setValue(kSettingsKey, paths);
}
