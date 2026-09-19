#include "tools/core/ToolConfig.h"
#include "tools/core/ToolRegistry.h"

void ToolConfig::saveToSettings(QSettings &s)
{
    s.beginGroup("toolPresets");
    for (const ToolTypeMeta &meta : toolTypeRegistry()) {
        IToolPresetList *list = toolPresetList(meta.type);
        if (!list) continue;

        s.beginGroup(meta.settingsKey);
        s.setValue("active", list->activeIndex());

        const QVariantList data = list->toVariantList();
        s.beginWriteArray("entries");
        for (int i = 0; i < data.size(); ++i) {
            s.setArrayIndex(i);
            const QVariantMap m = data[i].toMap();
            for (auto it = m.constBegin(); it != m.constEnd(); ++it)
                s.setValue(it.key(), it.value());
        }
        s.endArray();
        s.endGroup();
    }
    s.endGroup();
}

void ToolConfig::loadFromSettings(QSettings &s)
{
    s.beginGroup("toolPresets");
    for (const ToolTypeMeta &meta : toolTypeRegistry()) {
        IToolPresetList *list = toolPresetList(meta.type);
        if (!list) continue;

        s.beginGroup(meta.settingsKey);
        const int count = s.beginReadArray("entries");
        QVariantList data;
        for (int i = 0; i < count; ++i) {
            s.setArrayIndex(i);
            QVariantMap m;
            for (const QString &key : s.childKeys())
                m[key] = s.value(key);
            data.append(m);
        }
        s.endArray();

        if (!data.isEmpty()) {
            list->fromVariantList(data, meta.label);
            list->setActiveIndex(s.value("active", 0).toInt());
        }
        s.endGroup();
    }
    s.endGroup();
}
