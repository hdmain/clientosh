#include "AddonStore.h"

#include "AddonConfig.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QUrl>

namespace {

const char* kUserAgent = "clientosh-addons/1.0";

} // namespace

AddonStore::AddonStore(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    QDir().mkpath(addonsRoot());
}

QString AddonStore::addonsRoot()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return base + QStringLiteral("/addons");
}

QString AddonStore::addonDir(const QString& addonId)
{
    return addonsRoot() + QLatin1Char('/') + addonId;
}

QString AddonStore::bundledAddonsRoot()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/addons-bundle");
}

QVector<AddonInstallRecord> AddonStore::installed() const
{
    QVector<AddonInstallRecord> out;
    QDir root(addonsRoot());
    if (!root.exists()) {
        return out;
    }
    const QStringList dirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& id : dirs) {
        AddonInstallRecord rec;
        if (readInstallRecord(id, &rec)) {
            rec.enabled = AddonConfig::isEnabled(id);
            out.append(rec);
        }
    }
    return out;
}

bool AddonStore::isInstalled(const QString& addonId) const
{
    return QFileInfo::exists(addonDir(addonId) + QStringLiteral("/manifest.json"));
}

AddonInstallRecord AddonStore::installRecord(const QString& addonId) const
{
    AddonInstallRecord rec;
    if (readInstallRecord(addonId, &rec)) {
        rec.enabled = AddonConfig::isEnabled(addonId);
    }
    return rec;
}

bool AddonStore::readInstallRecord(const QString& addonId, AddonInstallRecord* out) const
{
    if (!out || addonId.isEmpty()) {
        return false;
    }
    QFile f(addonDir(addonId) + QStringLiteral("/manifest.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject o = doc.object();
    out->id = o.value(QStringLiteral("id")).toString(addonId);
    out->name = o.value(QStringLiteral("name")).toString();
    out->version = o.value(QStringLiteral("version")).toString();
    out->author = o.value(QStringLiteral("author")).toString();
    out->description = o.value(QStringLiteral("description")).toString();
    out->pluginFile = o.value(QStringLiteral("pluginFile")).toString();
    out->sha256 = o.value(QStringLiteral("sha256")).toString();
    out->abi = o.value(QStringLiteral("abi")).toString();
    out->enabled = o.value(QStringLiteral("enabled")).toBool(true);
    out->installedAtMs = static_cast<qint64>(o.value(QStringLiteral("installedAt")).toDouble(0));
    return !out->id.isEmpty();
}

bool AddonStore::writeInstallRecord(const AddonInstallRecord& rec, QString* errorOut)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), rec.id);
    o.insert(QStringLiteral("name"), rec.name);
    o.insert(QStringLiteral("version"), rec.version);
    o.insert(QStringLiteral("author"), rec.author);
    o.insert(QStringLiteral("description"), rec.description);
    o.insert(QStringLiteral("pluginFile"), rec.pluginFile);
    o.insert(QStringLiteral("sha256"), rec.sha256);
    o.insert(QStringLiteral("abi"), rec.abi);
    o.insert(QStringLiteral("enabled"), rec.enabled);
    o.insert(QStringLiteral("installedAt"), rec.installedAtMs);

    const QString dir = addonDir(rec.id);
    if (!QDir().mkpath(dir)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Could not create addon directory.");
        }
        return false;
    }
    QFile f(dir + QStringLiteral("/manifest.json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Could not write addon manifest.");
        }
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return true;
}

bool AddonStore::parseCatalog(const QByteArray& json, AddonCatalog* out, QString* errorOut)
{
    if (!out) {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Catalog is not valid JSON.");
        }
        return false;
    }
    const QJsonObject root = doc.object();
    out->format = root.value(QStringLiteral("format")).toInt(1);
    out->updated = root.value(QStringLiteral("updated")).toString();
    out->addons.clear();

    const QJsonArray arr = root.value(QStringLiteral("addons")).toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        AddonCatalogEntry e;
        e.id = o.value(QStringLiteral("id")).toString().trimmed();
        e.name = o.value(QStringLiteral("name")).toString().trimmed();
        e.description = o.value(QStringLiteral("description")).toString().trimmed();
        e.version = o.value(QStringLiteral("version")).toString().trimmed();
        e.author = o.value(QStringLiteral("author")).toString().trimmed();
        e.homepage = o.value(QStringLiteral("homepage")).toString().trimmed();
        if (e.id.isEmpty() || e.name.isEmpty()) {
            continue;
        }
        const QJsonArray arts = o.value(QStringLiteral("artifacts")).toArray();
        for (const QJsonValue& av : arts) {
            const QJsonObject ao = av.toObject();
            AddonArtifact a;
            a.abi = ao.value(QStringLiteral("abi")).toString().trimmed();
            a.url = ao.value(QStringLiteral("url")).toString().trimmed();
            a.sha256 = ao.value(QStringLiteral("sha256")).toString().trimmed().toLower();
            a.size = static_cast<qint64>(ao.value(QStringLiteral("size")).toDouble(0));
            if (!a.abi.isEmpty() && !a.url.isEmpty()) {
                e.artifacts.append(a);
            }
        }
        out->addons.append(e);
    }
    return true;
}

QString AddonStore::sha256Hex(const QByteArray& data)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QString AddonStore::guessPluginFileName(const QUrl& url)
{
    QString name = QFileInfo(url.path()).fileName();
    if (name.isEmpty()) {
#if defined(Q_OS_WIN)
        name = QStringLiteral("plugin.dll");
#elif defined(Q_OS_MACOS)
        name = QStringLiteral("plugin.dylib");
#else
        name = QStringLiteral("plugin.so");
#endif
    }
    return name;
}

void AddonStore::mergeBundledCatalog()
{
    const QString path = bundledAddonsRoot() + QStringLiteral("/index.json");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    AddonCatalog bundled;
    QString err;
    if (!parseCatalog(f.readAll(), &bundled, &err)) {
        return;
    }

    // Bundled entries win on id collision so the first-party AI agent always shows up.
    QHash<QString, int> byId;
    for (int i = 0; i < m_catalog.addons.size(); ++i) {
        byId.insert(m_catalog.addons.at(i).id, i);
    }
    for (const AddonCatalogEntry& e : bundled.addons) {
        if (byId.contains(e.id)) {
            m_catalog.addons[byId.value(e.id)] = e;
        } else {
            m_catalog.addons.push_back(e);
        }
    }
    m_hasCatalog = true;
}

void AddonStore::refreshCatalog()
{
    if (m_busy) {
        return;
    }
    const QUrl url(AddonConfig::repositoryUrl());
    if (!url.isValid() || url.scheme().isEmpty()) {
        // Still expose the on-disk bundled catalog (AI agent, etc.).
        m_catalog = AddonCatalog{};
        mergeBundledCatalog();
        if (m_hasCatalog) {
            emit catalogUpdated();
            emit statusMessage(QStringLiteral("Using bundled addon catalog (%1 addons).")
                                   .arg(m_catalog.addons.size()));
        } else {
            emit errorOccurred(QStringLiteral("Addon repository URL is invalid."));
        }
        return;
    }

    m_busy = true;
    emit statusMessage(QStringLiteral("Fetching addon catalog…"));

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QLatin1String(kUserAgent));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(30000);

    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_busy = false;

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const bool httpOk = (status == 0 && reply->error() == QNetworkReply::NoError)
            || (status >= 200 && status < 300);
        if (reply->error() != QNetworkReply::NoError || !httpOk) {
            QString err = reply->errorString();
            if (status > 0) {
                err += QStringLiteral(" (HTTP %1)").arg(status);
            }
            // Fall back to bundled packages so AI agent remains installable offline.
            m_catalog = AddonCatalog{};
            mergeBundledCatalog();
            if (m_hasCatalog) {
                emit catalogUpdated();
                emit statusMessage(
                    QStringLiteral("Remote catalog unavailable (%1); using bundled addons.")
                        .arg(err));
            } else {
                emit errorOccurred(QStringLiteral("Could not fetch catalog: %1").arg(err));
            }
            return;
        }

        AddonCatalog cat;
        QString parseErr;
        if (!parseCatalog(body, &cat, &parseErr)) {
            m_catalog = AddonCatalog{};
            mergeBundledCatalog();
            if (m_hasCatalog) {
                emit catalogUpdated();
                emit statusMessage(QStringLiteral("Remote catalog invalid; using bundled addons."));
            } else {
                emit errorOccurred(parseErr);
            }
            return;
        }
        m_catalog = cat;
        m_hasCatalog = true;
        mergeBundledCatalog();
        emit catalogUpdated();
        emit statusMessage(QStringLiteral("Catalog updated (%1 addons).")
                               .arg(m_catalog.addons.size()));
    });
}

void AddonStore::finishInstallFromBytes(const AddonCatalogEntry& entry, const AddonArtifact& art,
                                        const QByteArray& body)
{
    const QString gotHash = sha256Hex(body);
    if (gotHash.compare(art.sha256, Qt::CaseInsensitive) != 0) {
        m_busy = false;
        emit installFinished(
            entry.id, false,
            QStringLiteral("Checksum mismatch (expected %1, got %2).")
                .arg(art.sha256, gotHash));
        return;
    }

    const QString dir = addonDir(entry.id);
    if (QDir(dir).exists()) {
        QDir(dir).removeRecursively();
    }
    if (!QDir().mkpath(dir)) {
        m_busy = false;
        emit installFinished(entry.id, false,
                             QStringLiteral("Could not create addon directory."));
        return;
    }

    const QString fileName = guessPluginFileName(QUrl(art.url));
    QFile plugin(dir + QLatin1Char('/') + fileName);
    if (!plugin.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_busy = false;
        emit installFinished(entry.id, false, QStringLiteral("Could not write plugin file."));
        return;
    }
    plugin.write(body);
    plugin.close();

    AddonInstallRecord rec;
    rec.id = entry.id;
    rec.name = entry.name;
    rec.version = entry.version;
    rec.author = entry.author;
    rec.description = entry.description;
    rec.pluginFile = fileName;
    rec.sha256 = art.sha256.toLower();
    rec.abi = art.abi;
    rec.enabled = true;
    rec.installedAtMs = QDateTime::currentMSecsSinceEpoch();

    QString writeErr;
    if (!writeInstallRecord(rec, &writeErr)) {
        m_busy = false;
        QDir(dir).removeRecursively();
        emit installFinished(entry.id, false, writeErr);
        return;
    }

    AddonConfig::setEnabled(entry.id, true);
    m_busy = false;
    emit installFinished(entry.id, true, QString());
    emit statusMessage(QStringLiteral("Installed %1 %2.").arg(entry.name, entry.version));
}

void AddonStore::installAddon(const QString& addonId)
{
    if (m_busy) {
        emit errorOccurred(QStringLiteral("Wait for the current addon operation to finish."));
        return;
    }
    if (!m_hasCatalog) {
        emit errorOccurred(QStringLiteral("Refresh the catalog before installing."));
        return;
    }

    const AddonCatalogEntry* entry = nullptr;
    for (const AddonCatalogEntry& e : m_catalog.addons) {
        if (e.id == addonId) {
            entry = &e;
            break;
        }
    }
    if (!entry) {
        emit errorOccurred(QStringLiteral("Addon \"%1\" was not found in the catalog.").arg(addonId));
        return;
    }

    const AddonArtifact art = entry->artifactForThisPlatform();
    if (art.url.isEmpty()) {
        emit errorOccurred(
            QStringLiteral("No package for this platform (ABI %1).").arg(clientoshAddonAbi()));
        return;
    }
    if (art.sha256.isEmpty()) {
        emit errorOccurred(QStringLiteral("Catalog entry is missing a SHA-256 checksum."));
        return;
    }

    m_busy = true;

    const QUrl url(art.url);
    // Bundled addons use file:// — read directly (avoids QNAM local-file quirks).
    if (url.isLocalFile() || url.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0) {
        const QString localPath = url.isLocalFile() ? url.toLocalFile() : url.path();
        emit statusMessage(QStringLiteral("Installing %1…").arg(entry->name));
        QFile f(localPath);
        if (!f.open(QIODevice::ReadOnly)) {
            m_busy = false;
            emit installFinished(entry->id, false,
                                 QStringLiteral("Could not read package: %1").arg(localPath));
            return;
        }
        const QByteArray body = f.readAll();
        emit installProgress(addonId, body.size(), body.size());
        finishInstallFromBytes(*entry, art, body);
        return;
    }

    emit statusMessage(QStringLiteral("Downloading %1…").arg(entry->name));

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QLatin1String(kUserAgent));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(120000);

    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, addonId](qint64 received, qint64 total) {
                emit installProgress(addonId, received, total);
            });

    const AddonCatalogEntry entryCopy = *entry;
    const AddonArtifact artCopy = art;

    connect(reply, &QNetworkReply::finished, this, [this, reply, entryCopy, artCopy]() {
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const bool httpOk = (status == 0 && reply->error() == QNetworkReply::NoError)
            || (status >= 200 && status < 300);
        if (reply->error() != QNetworkReply::NoError || !httpOk) {
            m_busy = false;
            QString err = reply->errorString();
            if (status > 0) {
                err += QStringLiteral(" (HTTP %1)").arg(status);
            }
            emit installFinished(entryCopy.id, false,
                                 QStringLiteral("Download failed: %1").arg(err));
            return;
        }

        finishInstallFromBytes(entryCopy, artCopy, body);
    });
}

void AddonStore::removeAddon(const QString& addonId)
{
    if (m_busy) {
        emit errorOccurred(QStringLiteral("Wait for the current addon operation to finish."));
        return;
    }
    if (addonId.trimmed().isEmpty()) {
        return;
    }
    const QString dir = addonDir(addonId);
    if (!QDir(dir).exists()) {
        emit removeFinished(addonId, true, QString());
        return;
    }
    if (!QDir(dir).removeRecursively()) {
        emit removeFinished(addonId, false, QStringLiteral("Could not delete addon files."));
        return;
    }
    AddonConfig::setEnabled(addonId, false);
    emit removeFinished(addonId, true, QString());
    emit statusMessage(QStringLiteral("Removed addon \"%1\".").arg(addonId));
}
