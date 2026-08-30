#include "VaultManager.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QStandardPaths>

#include <cstring>
#if defined(Q_OS_WIN)
#include <windows.h>
#else
#include <cstdio>
#endif

namespace {

/** Atomically swap `tmpPath` over `finalPath` (write+flush then rename). */
bool atomicReplace(const QString& tmpPath, const QString& finalPath)
{
#if defined(Q_OS_WIN)
    // MoveFileEx with REPLACE_EXISTING + WRITE_THROUGH performs the swap in one
    // primitive with flush before the directory entry is updated.
    return MoveFileExW(reinterpret_cast<const wchar_t*>(tmpPath.utf16()),
                       reinterpret_cast<const wchar_t*>(finalPath.utf16()),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    const QByteArray src = tmpPath.toUtf8();
    const QByteArray dst = finalPath.toUtf8();
    if (::rename(src.constData(), dst.constData()) == 0) {
        return true;
    }
    // Some filesystems (e.g. certain network mounts) may lack atomic rename
    // support; fall back to remove+rename so the vault still persists.
    ::remove(dst.constData());
    return ::rename(src.constData(), dst.constData()) == 0;
#endif
}

} // namespace

VaultManager::VaultManager()
{
    m_usingNative = KeyringAdapter::isNativeAvailable();
}

VaultManager::~VaultManager()
{
    if (!m_masterKey.isNull()) {
        std::memset(m_masterKey.data(), 0, size_t(m_masterKey.size()));
        m_masterKey.clear();
    }
}

QString VaultManager::vaultDir()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath();
    }
    return dir;
}

QString VaultManager::connectsPath()
{
    return vaultDir() + QLatin1String("/connects.json");
}

QString VaultManager::dbvaultPath()
{
    return vaultDir() + QLatin1String("/dbvault");
}

bool VaultManager::connectsExists() const
{
    return QFile::exists(connectsPath());
}

VaultManager::LoadOutcome VaultManager::loadConnectsJson(QByteArray& plainOut)
{
    const QString path = connectsPath();
    if (!QFile::exists(path)) {
        plainOut.clear();
        return LoadOutcome::NotFound;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return LoadOutcome::Corrupt;
    }
    const QByteArray blob = f.readAll();
    try {
        plainOut = CryptoEngine::decrypt(CryptoEngine::deriveMachineBoundKey(), blob);
        return LoadOutcome::Loaded;
    } catch (const CryptoError&) {
        plainOut.clear();
        return LoadOutcome::Corrupt;
    }
}

bool VaultManager::saveConnectsJson(const QByteArray& plain)
{
    QDir().mkpath(vaultDir());
    QByteArray blob;
    try {
        blob = CryptoEngine::encrypt(CryptoEngine::deriveMachineBoundKey(), plain);
    } catch (const CryptoError&) {
        return false;
    }

    const QString tmp = connectsPath() + QStringLiteral(".tmp");
    QFile out(tmp);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    out.write(blob);
    out.flush();
    out.close();

    return atomicReplace(tmp, connectsPath());
}

bool VaultManager::ensureDbKey()
{
    if (!m_masterKey.isEmpty()) {
        return true;
    }

    // Prefer an existing master key from the keyring.
    QByteArray stored;
    if (KeyringAdapter::retrieve(QStringLiteral("dbvault.master"), stored)
        && stored.size() == CryptoEngine::kKeyBytes) {
        m_masterKey = stored;
        std::memset(stored.data(), 0, size_t(stored.size()));
        return true;
    }

    // First launch (or keyring reset): create a fresh master key and persist it.
    QByteArray fresh(CryptoEngine::kKeyBytes, Qt::Uninitialized);
    for (int i = 0; i < fresh.size(); ++i) {
        fresh[i] = char(QRandomGenerator::global()->bounded(256));
    }
    if (!KeyringAdapter::store(QStringLiteral("dbvault.master"), fresh)) {
        std::memset(fresh.data(), 0, size_t(fresh.size()));
        return false;
    }
    m_masterKey = fresh;
    std::memset(fresh.data(), 0, size_t(fresh.size()));
    return true;
}

bool VaultManager::readDbvault()
{
    const QString path = dbvaultPath();
    if (!QFile::exists(path)) {
        return true; // nothing yet — treat as empty vault
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray blob = f.readAll();

    QByteArray plain;
    try {
        plain = CryptoEngine::decrypt(dbKey(), blob);
    } catch (const CryptoError&) {
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(plain);
    if (doc.isNull() || !doc.isObject()) {
        std::memset(plain.data(), 0, size_t(plain.size()));
        return false;
    }
    m_dbvault = doc.object();
    std::memset(plain.data(), 0, size_t(plain.size()));
    return true;
}

bool VaultManager::persistDbvault()
{
    if (!ensureDbKey()) {
        return false;
    }
    QByteArray plain = QJsonDocument(m_dbvault).toJson(QJsonDocument::Compact);

    QByteArray blob;
    try {
        blob = CryptoEngine::encrypt(dbKey(), plain);
    } catch (const CryptoError&) {
        std::memset(plain.data(), 0, size_t(plain.size()));
        return false;
    }
    std::memset(plain.data(), 0, size_t(plain.size()));

    QDir().mkpath(vaultDir());
    const QString tmp = dbvaultPath() + QStringLiteral(".tmp");
    QFile out(tmp);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    out.write(blob);
    out.flush();
    out.close();

    const bool ok = atomicReplace(tmp, dbvaultPath());
    if (ok) {
        m_dbDirty = false;
    }
    return ok;
}

bool VaultManager::storeSecret(const QString& profileId, const QString& field,
                               const QByteArray& secret)
{
    if (!ensureDbKey()) {
        return false;
    }
    if (!m_dbLoaded && !readDbvault()) {
        return false;
    }
    QJsonObject profile = m_dbvault.value(profileId).toObject();
    profile.insert(field, QJsonValue(QString::fromLatin1(secret.toBase64())));
    m_dbvault.insert(profileId, profile);
    m_dbLoaded = true;
    return persistDbvault();
}

bool VaultManager::retrieveSecret(const QString& profileId, const QString& field,
                                  QByteArray& secretOut)
{
    if (!ensureDbKey()) {
        return false;
    }
    if (!m_dbLoaded && !readDbvault()) {
        return false;
    }
    const QJsonObject profile = m_dbvault.value(profileId).toObject();
    const QString b64 = profile.value(field).toString();
    if (b64.isEmpty()) {
        return false;
    }
    secretOut = QByteArray::fromBase64(b64.toLatin1());
    return true;
}

bool VaultManager::removeSecret(const QString& profileId, const QString& field)
{
    if (!ensureDbKey()) {
        return false;
    }
    if (!m_dbLoaded && !readDbvault()) {
        return false;
    }
    if (!m_dbvault.contains(profileId)) {
        return true;
    }
    QJsonObject profile = m_dbvault.value(profileId).toObject();
    profile.remove(field);
    if (profile.isEmpty()) {
        m_dbvault.remove(profileId);
    } else {
        m_dbvault.insert(profileId, profile);
    }
    return persistDbvault();
}

// ---- Stored keys (reusable keyring entries) -----------------------------

namespace {
// Reserved top-level key in the dbvault object holding the reusable keys map.
constexpr const char* kStoredKeysNode = "__stored_keys__";
constexpr const char* kNotesNode = "__notes__";
} // namespace

bool VaultManager::storeStoredKey(const StoredKey& key)
{
    if (key.id.trimmed().isEmpty() || key.pem.trimmed().isEmpty()) {
        return false;
    }
    if (!ensureDbKey()) {
        return false;
    }
    if (!m_dbLoaded && !readDbvault()) {
        return false;
    }
    QJsonObject keysNode = m_dbvault.value(QLatin1String(kStoredKeysNode)).toObject();
    QJsonObject entry;
    entry.insert(QStringLiteral("name"), key.name);
    entry.insert(QStringLiteral("type"), key.type);
    entry.insert(QStringLiteral("fingerprint"), key.fingerprint);
    entry.insert(QStringLiteral("pem"), QString::fromLatin1(key.pem.toBase64()));
    entry.insert(QStringLiteral("hasPassphrase"), key.hasPassphrase);
    keysNode.insert(key.id, entry);
    m_dbvault.insert(QLatin1String(kStoredKeysNode), keysNode);
    m_dbLoaded = true;
    return persistDbvault();
}

bool VaultManager::retrieveStoredKey(const QString& id, StoredKey& keyOut)
{
    if (id.isEmpty()) {
        return false;
    }
    if (!ensureDbKey()) {
        return false;
    }
    if (!m_dbLoaded && !readDbvault()) {
        return false;
    }
    const QJsonObject keysNode = m_dbvault.value(QLatin1String(kStoredKeysNode)).toObject();
    const QJsonValue v = keysNode.value(id);
    if (!v.isObject()) {
        return false;
    }
    const QJsonObject entry = v.toObject();
    keyOut.id = id;
    keyOut.name = entry.value(QStringLiteral("name")).toString();
    keyOut.type = entry.value(QStringLiteral("type")).toString();
    keyOut.fingerprint = entry.value(QStringLiteral("fingerprint")).toString();
    keyOut.hasPassphrase = entry.value(QStringLiteral("hasPassphrase")).toBool(false);
    keyOut.pem = QByteArray::fromBase64(entry.value(QStringLiteral("pem")).toString().toLatin1());
    if (keyOut.pem.isEmpty()) {
        return false;
    }
    return true;
}

QVector<StoredKey> VaultManager::listStoredKeys()
{
    QVector<StoredKey> out;
    if (!ensureDbKey()) {
        return out;
    }
    if (!m_dbLoaded && !readDbvault()) {
        return out;
    }
    const QJsonObject keysNode = m_dbvault.value(QLatin1String(kStoredKeysNode)).toObject();
    const QStringList ids = keysNode.keys();
    out.reserve(ids.size());
    for (const QString& id : ids) {
        const QJsonObject entry = keysNode.value(id).toObject();
        StoredKey k;
        k.id = id;
        k.name = entry.value(QStringLiteral("name")).toString();
        k.type = entry.value(QStringLiteral("type")).toString();
        k.fingerprint = entry.value(QStringLiteral("fingerprint")).toString();
        k.hasPassphrase = entry.value(QStringLiteral("hasPassphrase")).toBool(false);
        out.push_back(k);
    }
    return out;
}

bool VaultManager::removeStoredKey(const QString& id)
{
    if (id.isEmpty()) {
        return false;
    }
    if (!ensureDbKey()) {
        return false;
    }
    if (!m_dbLoaded && !readDbvault()) {
        return false;
    }
    if (!m_dbvault.contains(QLatin1String(kStoredKeysNode))) {
        return removeStoredKeyPassphrase(id);
    }
    QJsonObject keysNode = m_dbvault.value(QLatin1String(kStoredKeysNode)).toObject();
    if (!keysNode.contains(id)) {
        return removeStoredKeyPassphrase(id);
    }
    keysNode.remove(id);
    if (keysNode.isEmpty()) {
        m_dbvault.remove(QLatin1String(kStoredKeysNode));
    } else {
        m_dbvault.insert(QLatin1String(kStoredKeysNode), keysNode);
    }
    const bool removed = persistDbvault();
    if (removed) {
        return removeStoredKeyPassphrase(id);
    }
    return false;
}

namespace {
QString storedKeySecretId(const QString& id)
{
    return QStringLiteral("stored-key/%1").arg(id);
}
}

bool VaultManager::storeStoredKeyPassphrase(const QString& id, const QByteArray& passphrase)
{
    return !id.trimmed().isEmpty()
        && storeSecret(storedKeySecretId(id), QStringLiteral("passphrase"), passphrase);
}

bool VaultManager::retrieveStoredKeyPassphrase(const QString& id, QByteArray& passphraseOut)
{
    return !id.trimmed().isEmpty()
        && retrieveSecret(storedKeySecretId(id), QStringLiteral("passphrase"), passphraseOut);
}

bool VaultManager::removeStoredKeyPassphrase(const QString& id)
{
    return id.trimmed().isEmpty()
        || removeSecret(storedKeySecretId(id), QStringLiteral("passphrase"));
}

bool VaultManager::storeNotesMarkdown(const QString& markdown)
{
    if (!ensureDbKey()) {
        return false;
    }
    if (!m_dbLoaded && !readDbvault()) {
        return false;
    }
    QJsonObject notes;
    notes.insert(QStringLiteral("markdown"), markdown);
    m_dbvault.insert(QLatin1String(kNotesNode), notes);
    m_dbLoaded = true;
    return persistDbvault();
}

QString VaultManager::retrieveNotesMarkdown()
{
    if (!ensureDbKey()) {
        return {};
    }
    if (!m_dbLoaded && !readDbvault()) {
        return {};
    }
    const QJsonValue v = m_dbvault.value(QLatin1String(kNotesNode));
    if (!v.isObject()) {
        return {};
    }
    return v.toObject().value(QStringLiteral("markdown")).toString();
}
