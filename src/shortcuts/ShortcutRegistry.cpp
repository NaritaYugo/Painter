#include "shortcuts/ShortcutRegistry.h"

#include <QAction>
#include <QKeySequence>

void ShortcutRegistry::registerTool(ToolType tool, const QString &id, const QString &label,
                                     const QString &category, int defaultKey)
{
    ActionSpec spec;
    spec.id         = id;
    spec.label      = label;
    spec.category   = category;
    spec.defaultKey = defaultKey;
    spec.currentKey = defaultKey;
    spec.kind       = ActionKind::Tool;
    spec.tool       = tool;

    idToIndex_[id] = actions_.size();
    actions_.append(spec);
    if (defaultKey != 0)
        keyToId_[defaultKey] = id;
}

QAction *ShortcutRegistry::registerCommand(QObject *actionParent, const QString &id, const QString &label,
                                            const QString &category, int defaultKey,
                                            std::function<void()> onTriggered)
{
    auto *action = new QAction(label, actionParent);
    QObject::connect(action, &QAction::triggered, actionParent, std::move(onTriggered));

    ActionSpec spec;
    spec.id         = id;
    spec.label      = label;
    spec.category   = category;
    spec.defaultKey = defaultKey;
    spec.currentKey = defaultKey;
    spec.kind       = ActionKind::Command;
    spec.action     = action;

    idToIndex_[id] = actions_.size();
    actions_.append(spec);
    if (defaultKey != 0)
        keyToId_[defaultKey] = id;

    applyKeyToAction(actions_[idToIndex_[id]]);
    return action;
}

void ShortcutRegistry::applyKeyToAction(ActionSpec &spec)
{
    if (spec.kind == ActionKind::Command && spec.action)
        spec.action->setShortcut(QKeySequence(spec.currentKey));
}

int ShortcutRegistry::keyFor(const QString &id) const
{
    auto it = idToIndex_.find(id);
    if (it == idToIndex_.end()) return 0;
    return actions_[it.value()].currentKey;
}

int ShortcutRegistry::keyForTool(ToolType tool) const
{
    for (const ActionSpec &spec : actions_) {
        if (spec.kind == ActionKind::Tool && spec.tool == tool)
            return spec.currentKey;
    }
    return 0;
}

void ShortcutRegistry::setKey(const QString &id, int newKey)
{
    auto it = idToIndex_.find(id);
    if (it == idToIndex_.end()) return;

    ActionSpec &spec = actions_[it.value()];
    if (spec.currentKey == newKey) return;

    if (spec.currentKey != 0 && keyToId_.value(spec.currentKey) == id)
        keyToId_.remove(spec.currentKey);

    spec.currentKey = newKey;
    if (newKey != 0)
        keyToId_[newKey] = id;

    applyKeyToAction(spec);
}

QString ShortcutRegistry::idForKey(int key) const
{
    if (key == 0) return QString();
    return keyToId_.value(key);
}

std::optional<ToolType> ShortcutRegistry::toolForKey(int key) const
{
    const QString id = idForKey(key);
    if (id.isEmpty()) return std::nullopt;

    // keyToId_にはプリセットのpresetIdも入っているので、actions_に無いidは
    // ここで弾く(idToIndex_.value()の既定値0でactions_[0]を返してしまわないように)。
    auto it = idToIndex_.find(id);
    if (it == idToIndex_.end()) return std::nullopt;

    const ActionSpec &spec = actions_[it.value()];
    if (spec.kind != ActionKind::Tool) return std::nullopt;
    return spec.tool;
}

// ===========================================================================
// ツールプリセットのショートカット
// ===========================================================================
// presetId は "preset_<ToolTypeの整数値>_<uid>"。uidはQUuid由来(16進とハイフン)で
// アンダースコアを含まないため、最初の2つの'_'で機械的に分解できる。
QString ShortcutRegistry::presetId(ToolType tool, const QString &uid)
{
    return QStringLiteral("preset_%1_%2").arg((int)tool).arg(uid);
}

bool ShortcutRegistry::parsePresetId(const QString &id, PresetRef &out)
{
    if (!id.startsWith(QStringLiteral("preset_"))) return false;
    const QString rest = id.mid(7);
    const int sep = rest.indexOf(QLatin1Char('_'));
    if (sep <= 0) return false;

    bool ok = false;
    const int toolInt = rest.left(sep).toInt(&ok);
    if (!ok) return false;

    out.tool = (ToolType)toolInt;
    out.uid  = rest.mid(sep + 1);
    return !out.uid.isEmpty();
}

int ShortcutRegistry::presetKeyFor(ToolType tool, const QString &uid) const
{
    return presetToKey_.value(presetId(tool, uid), 0);
}

void ShortcutRegistry::setPresetKey(ToolType tool, const QString &uid, int newKey)
{
    if (uid.isEmpty()) return;
    const QString id = presetId(tool, uid);

    const int oldKey = presetToKey_.value(id, 0);
    if (oldKey == newKey) return;

    if (oldKey != 0 && keyToId_.value(oldKey) == id)
        keyToId_.remove(oldKey);

    if (newKey == 0) {
        presetToKey_.remove(id);
    } else {
        presetToKey_[id] = newKey;
        keyToId_[newKey] = id;
    }
}

void ShortcutRegistry::clearPresetKeys()
{
    for (auto it = presetToKey_.constBegin(); it != presetToKey_.constEnd(); ++it) {
        if (keyToId_.value(it.value()) == it.key())
            keyToId_.remove(it.value());
    }
    presetToKey_.clear();
}

std::optional<ShortcutRegistry::PresetRef> ShortcutRegistry::presetForKey(int key) const
{
    const QString id = idForKey(key);
    if (id.isEmpty()) return std::nullopt;

    PresetRef ref;
    if (!parsePresetId(id, ref)) return std::nullopt;
    return ref;
}

void ShortcutRegistry::resetToDefaults()
{
    for (int i = 0; i < actions_.size(); ++i)
        setKey(actions_[i].id, actions_[i].defaultKey);
    clearPresetKeys(); // プリセットには既定キーが無いので、戻す先は「未設定」
}

void ShortcutRegistry::load(QSettings &settings)
{
    for (int i = 0; i < actions_.size(); ++i) {
        const QString id = actions_[i].id;
        const int key = settings.value("shortcuts/" + id, actions_[i].defaultKey).toInt();
        setKey(id, key);
    }

    // プリセットのショートカット。プリセット本体(ToolConfig)より先に読まれても
    // 問題ないよう、ここでは存在確認をせずuidのまま保持する
    // (対応するプリセットが消えていれば、単に発火しないキーとして残るだけ)。
    clearPresetKeys();
    settings.beginGroup("shortcuts");
    const QStringList keys = settings.childKeys();
    settings.endGroup();
    for (const QString &id : keys) {
        PresetRef ref;
        if (!parsePresetId(id, ref)) continue;
        const int key = settings.value("shortcuts/" + id, 0).toInt();
        if (key != 0) setPresetKey(ref.tool, ref.uid, key);
    }
}

void ShortcutRegistry::save(QSettings &settings) const
{
    // 過去のツール構成が違った時期の残留キーが溜まらないよう、
    // 書き込み前にグループを丸ごとクリアしてから現在の内容を書き直す。
    settings.remove("shortcuts");
    for (const ActionSpec &spec : actions_)
        settings.setValue("shortcuts/" + spec.id, spec.currentKey);
    for (auto it = presetToKey_.constBegin(); it != presetToKey_.constEnd(); ++it)
        settings.setValue("shortcuts/" + it.key(), it.value());
}
