#include "AddonHost.h"

#include "AddonConfig.h"
#include "AddonHostContext.h"
#include "AddonStore.h"
#include "IClientoshAddon.h"

#include <QDir>
#include <QFileInfo>
#include <QPluginLoader>
#include <QSet>
#include <QTimer>

AddonHost::AddonHost(AddonStore* store, AddonHostContext* context, QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_context(context)
{
    if (m_store) {
        connect(m_store, &AddonStore::installFinished, this,
                [this](const QString&, bool ok, const QString&) {
                    if (ok) {
                        reloadInstalled();
                    }
                });
        connect(m_store, &AddonStore::removeFinished, this,
                [this](const QString& id, bool ok, const QString&) {
                    if (ok) {
                        unloadAddon(id);
                        reloadInstalled();
                    }
                });
    }
    // Defer first load so MainWindow finishes constructing before plugins activate UI.
    QTimer::singleShot(0, this, &AddonHost::reloadInstalled);
}

AddonHost::~AddonHost()
{
    const QStringList ids = m_loaded.keys();
    for (const QString& id : ids) {
        unloadAddon(id);
    }
}

QVector<AddonInstallRecord> AddonHost::enabledInstalled() const
{
    QVector<AddonInstallRecord> out;
    for (const AddonInstallRecord& r : m_installed) {
        if (AddonConfig::isEnabled(r.id)) {
            out.append(r);
        }
    }
    return out;
}

bool AddonHost::isLoaded(const QString& addonId) const
{
    return m_loaded.contains(addonId);
}

void AddonHost::reloadInstalled()
{
    m_installed = m_store ? m_store->installed() : QVector<AddonInstallRecord>{};
    syncLoadedPlugins();
    emit installedChanged();
    emit statusMessage(QStringLiteral("%1 addon(s) installed, %2 enabled, %3 loaded.")
                           .arg(m_installed.size())
                           .arg(enabledInstalled().size())
                           .arg(m_loaded.size()));
}

void AddonHost::setAddonEnabled(const QString& addonId, bool enabled)
{
    AddonConfig::setEnabled(addonId, enabled);
    if (!enabled) {
        unloadAddon(addonId);
    }
    reloadInstalled();
    emit statusMessage(enabled ? QStringLiteral("Addon \"%1\" enabled.").arg(addonId)
                               : QStringLiteral("Addon \"%1\" disabled.").arg(addonId));
}

void AddonHost::syncLoadedPlugins()
{
    QSet<QString> shouldLoad;
    for (const AddonInstallRecord& r : m_installed) {
        if (AddonConfig::isEnabled(r.id) && !r.pluginFile.isEmpty()) {
            shouldLoad.insert(r.id);
        }
    }

    const QStringList loadedIds = m_loaded.keys();
    for (const QString& id : loadedIds) {
        if (!shouldLoad.contains(id)) {
            unloadAddon(id);
        }
    }

    for (const AddonInstallRecord& r : m_installed) {
        if (shouldLoad.contains(r.id) && !m_loaded.contains(r.id)) {
            loadAddon(r);
        }
    }
}

bool AddonHost::loadAddon(const AddonInstallRecord& rec)
{
    if (!m_store || rec.id.isEmpty() || rec.pluginFile.isEmpty()) {
        return false;
    }
    if (m_loaded.contains(rec.id)) {
        return true;
    }

    const QString path = AddonStore::addonDir(rec.id) + QLatin1Char('/') + rec.pluginFile;
    if (!QFileInfo::exists(path)) {
        emit statusMessage(QStringLiteral("Addon \"%1\" plugin missing: %2").arg(rec.id, path));
        return false;
    }

    auto* loader = new QPluginLoader(path, this);
    QObject* instance = loader->instance();
    if (!instance) {
        const QString err = loader->errorString();
        delete loader;
        emit statusMessage(QStringLiteral("Failed to load \"%1\": %2").arg(rec.id, err));
        return false;
    }

    IClientoshAddon* iface = qobject_cast<IClientoshAddon*>(instance);
    if (!iface) {
        loader->unload();
        delete loader;
        emit statusMessage(QStringLiteral("\"%1\" is not a clientosh addon plugin.").arg(rec.id));
        return false;
    }

    if (m_context) {
        iface->activate(m_context);
    }

    LoadedAddon loaded;
    loaded.loader = loader;
    loaded.iface = iface;
    m_loaded.insert(rec.id, loaded);
    emit addonLoaded(rec.id);
    emit statusMessage(QStringLiteral("Loaded addon \"%1\".").arg(rec.name.isEmpty() ? rec.id : rec.name));
    return true;
}

void AddonHost::unloadAddon(const QString& addonId)
{
    auto it = m_loaded.find(addonId);
    if (it == m_loaded.end()) {
        return;
    }
    LoadedAddon loaded = it.value();
    m_loaded.erase(it);

    if (loaded.iface) {
        loaded.iface->deactivate();
    }
    if (loaded.loader) {
        loaded.loader->unload();
        loaded.loader->deleteLater();
    }
    emit addonUnloaded(addonId);
}
