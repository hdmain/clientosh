#pragma once

#include "AddonTypes.h"

#include <QObject>
#include <QString>
#include <QVector>

class QNetworkAccessManager;

/**
 * Downloads and manages on-disk addons from a remote index.json catalog.
 * Also merges the local addons-bundle shipped next to the executable.
 * Plugin loading is AddonHost's job.
 */
class AddonStore : public QObject
{
    Q_OBJECT

public:
    explicit AddonStore(QObject* parent = nullptr);

    /** Root directory: …/clientosh/addons */
    static QString addonsRoot();
    static QString addonDir(const QString& addonId);
    /** Bundled packages next to the executable: …/addons-bundle */
    static QString bundledAddonsRoot();

    QVector<AddonInstallRecord> installed() const;
    bool isInstalled(const QString& addonId) const;
    AddonInstallRecord installRecord(const QString& addonId) const;

    const AddonCatalog& catalog() const { return m_catalog; }
    bool hasCatalog() const { return m_hasCatalog; }

    bool busy() const { return m_busy; }

public slots:
    void refreshCatalog();
    void installAddon(const QString& addonId);
    void removeAddon(const QString& addonId);

signals:
    void catalogUpdated();
    void installProgress(const QString& addonId, qint64 received, qint64 total);
    void installFinished(const QString& addonId, bool ok, const QString& error);
    void removeFinished(const QString& addonId, bool ok, const QString& error);
    void statusMessage(const QString& message);
    void errorOccurred(const QString& message);

private:
    bool writeInstallRecord(const AddonInstallRecord& rec, QString* errorOut);
    bool readInstallRecord(const QString& addonId, AddonInstallRecord* out) const;
    void mergeBundledCatalog();
    /** Write plugin bytes + manifest; clears m_busy and emits installFinished. */
    void finishInstallFromBytes(const AddonCatalogEntry& entry, const AddonArtifact& art,
                                const QByteArray& body);
    static bool parseCatalog(const QByteArray& json, AddonCatalog* out, QString* errorOut);
    static QString sha256Hex(const QByteArray& data);
    static QString guessPluginFileName(const QUrl& url);

    QNetworkAccessManager* m_nam = nullptr;
    AddonCatalog m_catalog;
    bool m_hasCatalog = false;
    bool m_busy = false;
};
