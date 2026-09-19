#pragma once

#include <QString>
#include <QVector>
#include <QVariantMap>
#include <QVariantList>
#include <QUuid>

// ===========================================================================
// ToolPresetList<ConfigT>
// ---------------------------------------------------------------------------
// 1つのToolType(ペン/消しゴム/移動...)に属する「ツールプリセット」―名前付き設定
// プリセットの一覧を保持する。ConfigTはPenToolConfig等、各ツールの設定クラス。
//
// ConfigTには以下の2メソッドが必要:
//   QVariantMap toMap() const;
//   void        fromMap(const QVariantMap &m);
// (QSettingsへの保存/復元に使う。パラメータを持たないツールは
//  EmptyToolConfig をそのまま使い回せばよい)
// ===========================================================================
template<typename ConfigT>
class ToolPresetList
{
public:
    // uid: このプリセットを一意に指す不変のID。名前は変更でき、インデックスは
    // 並び替えや削除でずれるため、ショートカットの割り当て先として使えるのは
    // これだけ(設定にも保存され、起動をまたいで同じプリセットを指し続ける)。
    struct Entry { QString name; ConfigT config; QString uid; };

    static QString makeUid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

    explicit ToolPresetList(const QString &defaultName = QStringLiteral("設定1"))
    {
        entries_.append({ defaultName, ConfigT(), makeUid() });
    }

    int count() const { return entries_.size(); }

    QString name(int i) const { return valid(i) ? entries_[i].name : QString(); }
    void    setName(int i, const QString &n) { if (valid(i)) entries_[i].name = n; }

    QString uid(int i) const { return valid(i) ? entries_[i].uid : QString(); }
    int indexOfUid(const QString &uid) const
    {
        if (uid.isEmpty()) return -1;
        for (int i = 0; i < entries_.size(); i++)
            if (entries_[i].uid == uid) return i;
        return -1;
    }

    ConfigT       &config(int i)       { return entries_[i].config; }
    const ConfigT &config(int i) const { return entries_[i].config; }

    int  activeIndex() const { return activeIndex_; }
    void setActiveIndex(int i) { if (valid(i)) activeIndex_ = i; }

    ConfigT       &active()       { return entries_[activeIndex_].config; }
    const ConfigT &active() const { return entries_[activeIndex_].config; }

    // 末尾に新規追加する。戻り値: 追加後のインデックス
    int add(const QString &name, const ConfigT &cfg)
    {
        entries_.append({ name, cfg, makeUid() });
        return entries_.size() - 1;
    }

    // i番目を直後に複製する。戻り値: 複製後のインデックス
    int duplicate(int i)
    {
        if (!valid(i)) return -1;
        Entry e = entries_[i];
        e.name += QStringLiteral(" コピー");
        e.uid = makeUid(); // 複製は別のプリセットなので、ショートカットは引き継がない
        entries_.insert(i + 1, e);
        if (activeIndex_ > i) activeIndex_++;
        return i + 1;
    }

    // 最低1つは残す(全削除は不可)
    bool remove(int i)
    {
        if (!valid(i) || entries_.size() <= 1) return false;
        entries_.remove(i);
        if (activeIndex_ >= entries_.size()) activeIndex_ = entries_.size() - 1;
        else if (activeIndex_ > i)           activeIndex_--;
        return true;
    }

    // ---- 永続化 ----
    QVariantList toVariantList() const
    {
        QVariantList out;
        for (const Entry &e : entries_) {
            QVariantMap m = e.config.toMap();
            m[QStringLiteral("name")] = e.name;
            m[QStringLiteral("__uid")] = e.uid;
            out.append(m);
        }
        return out;
    }

    // data が空なら何もしない(既定の1件をそのまま残す)
    void fromVariantList(const QVariantList &data, const QString &fallbackName)
    {
        if (data.isEmpty()) return;
        entries_.clear();
        for (const QVariant &v : data) {
            const QVariantMap m = v.toMap();
            Entry e;
            e.name = m.value(QStringLiteral("name"), fallbackName).toString();
            // uidを持たない古い設定から読んだ場合はここで採番する
            e.uid  = m.value(QStringLiteral("__uid")).toString();
            if (e.uid.isEmpty()) e.uid = makeUid();
            e.config.fromMap(m);
            entries_.append(e);
        }
        if (entries_.isEmpty())
            entries_.append({ fallbackName, ConfigT(), makeUid() });
        activeIndex_ = qBound(0, activeIndex_, entries_.size() - 1);
    }

private:
    bool valid(int i) const { return i >= 0 && i < entries_.size(); }

    QVector<Entry> entries_;
    int activeIndex_ = 0;
};

// ===========================================================================
// IToolPresetList / ToolPresetListAdapter<ConfigT>
// ---------------------------------------------------------------------------
// ToolPresetDockや永続化コードがConfigTの実型を知らなくても操作できるようにする
// 型消去インターフェース。新しいツールを追加する際、ToolConfigに
// ToolPresetList<XxxConfig> を1つ足してこのアダプタでラップするだけで、
// UI側・保存/復元側は無改造で新ツールのツールプリセットを扱えるようになる。
// ===========================================================================
class IToolPresetList
{
public:
    virtual ~IToolPresetList() = default;

    virtual int     count() const = 0;
    virtual QString name(int i) const = 0;
    virtual void    setName(int i, const QString &n) = 0;

    // ショートカットの割り当て先を指すための不変ID(ToolPresetList::Entry::uid参照)
    virtual QString uid(int i) const = 0;
    virtual int     indexOfUid(const QString &uid) const = 0;

    virtual int  activeIndex() const = 0;
    virtual void setActiveIndex(int i) = 0;

    // 現在アクティブな設定をコピーして新規ツールプリセットを追加する。戻り値: 新規インデックス
    virtual int  addNew(const QString &name) = 0;
    virtual int  duplicate(int i) = 0;
    virtual bool remove(int i) = 0;

    virtual QVariantList toVariantList() const = 0;
    virtual void fromVariantList(const QVariantList &data, const QString &fallbackName) = 0;

    // i番目の設定値そのもの(ConfigT::toMap()と同じ形)。
    // ツールプリセット一覧のプレビュー画像を、その設定から作り直すために使う
    // (ToolPreview::render()。ConfigTの実型を知らないままでも値を渡せる)。
    virtual QVariantMap valuesAt(int i) const = 0;
};

template<typename ConfigT>
class ToolPresetListAdapter : public IToolPresetList
{
public:
    explicit ToolPresetListAdapter(ToolPresetList<ConfigT> *list) : list_(list) {}

    int     count() const override { return list_->count(); }
    QString name(int i) const override { return list_->name(i); }
    void    setName(int i, const QString &n) override { list_->setName(i, n); }

    QString uid(int i) const override { return list_->uid(i); }
    int     indexOfUid(const QString &u) const override { return list_->indexOfUid(u); }

    int  activeIndex() const override { return list_->activeIndex(); }
    void setActiveIndex(int i) override { list_->setActiveIndex(i); }

    int  addNew(const QString &name) override { return list_->add(name, list_->active()); }
    int  duplicate(int i) override { return list_->duplicate(i); }
    bool remove(int i) override { return list_->remove(i); }

    QVariantList toVariantList() const override { return list_->toVariantList(); }
    void fromVariantList(const QVariantList &data, const QString &fallbackName) override
    {
        list_->fromVariantList(data, fallbackName);
    }

    QVariantMap valuesAt(int i) const override
    {
        return (i >= 0 && i < list_->count()) ? list_->config(i).toMap() : QVariantMap();
    }

private:
    ToolPresetList<ConfigT> *list_ = nullptr;
};
