#pragma once

#include "AddonTypes.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

class AddonStore;
class AddonHostContext;
class IClientoshAddon;
class QPluginLoader;

/**
 * Loads installed+enabled addon binaries via QPluginLoader.
 * Uninstalled / disabled addons are not loaded — zero plugin RAM.
 */
class AddonHost : public QObject
{
    Q_OBJECT

public:
    explicit AddonHost(AddonStore* store, AddonHostContext* context, QObject* parent = nullptr);
    ~AddonHost() override;

    AddonStore* store() const { return m_store; }
    AddonHostContext* context() const { return m_context; }

    QVector<AddonInstallRecord> enabledInstalled() const;
    bool isLoaded(const QString& addonId) const;

    void reloadInstalled();
    void setAddonEnabled(const QString& addonId, bool enabled);

signals:
    void installedChanged();
    void statusMessage(const QString& message);
    void addonLoaded(const QString& addonId);
    void addonUnloaded(const QString& addonId);

private:
    struct LoadedAddon {
        QPluginLoader* loader = nullptr;
        IClientoshAddon* iface = nullptr;
    };

    void syncLoadedPlugins();
    bool loadAddon(const AddonInstallRecord& rec);
    void unloadAddon(const QString& addonId);

    AddonStore* m_store = nullptr;
    AddonHostContext* m_context = nullptr;
    QVector<AddonInstallRecord> m_installed;
    QHash<QString, LoadedAddon> m_loaded;
};
