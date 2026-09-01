#pragma once

#include "VaultManager.h"

#include <QHash>
#include <QString>
#include <QVector>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSet>
#include <QUuid>
#include <QMetaType>

enum class ConnectionMode {
    Ssh = 0,     // interactive terminal + optional SFTP
    SftpOnly = 1, // open SFTP file manager only
    Telnet = 2,   // raw telnet terminal (no SFTP)
    Serial = 3,   // local serial/COM terminal
    Rdp = 4       // remote desktop (external RDP client)
};

enum class AuthMethod {
    Password = 0,
    StoredKey = 1,
    KeyFile = 2,
    SshAgent = 3
};

inline QString authMethodToString(AuthMethod method)
{
    switch (method) {
    case AuthMethod::StoredKey: return QStringLiteral("storedKey");
    case AuthMethod::KeyFile: return QStringLiteral("keyFile");
    case AuthMethod::SshAgent: return QStringLiteral("sshAgent");
    case AuthMethod::Password:
    default: return QStringLiteral("password");
    }
}

inline AuthMethod authMethodFromString(const QString& value,
                                       const QString& legacyKeyId = {},
                                       const QString& legacyKeyPath = {})
{
    if (value.compare(QLatin1String("storedKey"), Qt::CaseInsensitive) == 0
        || value.compare(QLatin1String("keyring"), Qt::CaseInsensitive) == 0) {
        return AuthMethod::StoredKey;
    }
    if (value.compare(QLatin1String("keyFile"), Qt::CaseInsensitive) == 0
        || value.compare(QLatin1String("file"), Qt::CaseInsensitive) == 0) {
        return AuthMethod::KeyFile;
    }
    if (value.compare(QLatin1String("sshAgent"), Qt::CaseInsensitive) == 0
        || value.compare(QLatin1String("agent"), Qt::CaseInsensitive) == 0) {
        return AuthMethod::SshAgent;
    }
    if (value.compare(QLatin1String("password"), Qt::CaseInsensitive) == 0) {
        return AuthMethod::Password;
    }
    // Profiles created before authMethod existed inferred the method from the
    // presence of a key reference.
    if (!legacyKeyId.trimmed().isEmpty()) return AuthMethod::StoredKey;
    if (!legacyKeyPath.trimmed().isEmpty()) return AuthMethod::KeyFile;
    return AuthMethod::Password;
}

struct SessionProfile {
    QString id;
    QString name;
    QString host;
    int port = 22;
    QString user;
    QString password;
    bool savePassword = false;
    QString privateKeyPath; // if set, prefer public-key auth (filesystem path)
    QString privateKeyId;   // if set, use a key pre-saved in the keyring (overrides path)
    QString keyPassphrase;  // for encrypted keys (not persisted by default)
    bool saveKeyPassphrase = false;
    AuthMethod authMethod = AuthMethod::Password;
    ConnectionMode connectionMode = ConnectionMode::Ssh;
    int serialBaudRate = 115200;
    int serialDataBits = 8;
    QString serialParity = QStringLiteral("none");
    int serialStopBits = 1;
    QString serialFlowControl = QStringLiteral("none");
    QString rdpDomain;
    QString system; // OS detected from the SSH banner on last connect (e.g. "Linux", "Ubuntu").

    bool usesPrivateKey() const
    {
        return authMethod == AuthMethod::StoredKey || authMethod == AuthMethod::KeyFile;
    }

    bool usesSshAgent() const { return authMethod == AuthMethod::SshAgent; }

    void normalizeAuthentication()
    {
        switch (authMethod) {
        case AuthMethod::StoredKey:
            privateKeyPath.clear();
            password.clear();
            savePassword = false;
            break;
        case AuthMethod::KeyFile:
            privateKeyId.clear();
            password.clear();
            savePassword = false;
            break;
        case AuthMethod::SshAgent:
            privateKeyId.clear();
            privateKeyPath.clear();
            password.clear();
            savePassword = false;
            keyPassphrase.clear();
            saveKeyPassphrase = false;
            break;
        case AuthMethod::Password:
        default:
            privateKeyId.clear();
            privateKeyPath.clear();
            keyPassphrase.clear();
            saveKeyPassphrase = false;
            break;
        }
    }

    /** "—" placeholder when the OS has not been detected yet. */
    QString systemLabel() const
    {
        return system.trimmed().isEmpty() ? QStringLiteral("—") : system.trimmed();
    }

    bool isSftpOnly() const
    {
        return connectionMode == ConnectionMode::SftpOnly;
    }

    bool isTelnet() const
    {
        return connectionMode == ConnectionMode::Telnet;
    }

    bool isSerial() const { return connectionMode == ConnectionMode::Serial; }

    bool isRdp() const { return connectionMode == ConnectionMode::Rdp; }

    bool isTerminal() const
    {
        return connectionMode == ConnectionMode::Ssh || connectionMode == ConnectionMode::Telnet
            || connectionMode == ConnectionMode::Serial;
    }

    QString connectionTypeLabel() const
    {
        if (isSftpOnly()) {
            return QStringLiteral("SFTP");
        }
        if (isTelnet()) {
            return QStringLiteral("Telnet");
        }
        if (isSerial()) {
            return QStringLiteral("Serial");
        }
        if (isRdp()) {
            return QStringLiteral("RDP");
        }
        return QStringLiteral("SSH");
    }

    /** Display name without exposing host/IP on the dashboard. */
    QString displayTitle() const
    {
        if (!name.trimmed().isEmpty()) {
            return name.trimmed();
        }
        if (!user.isEmpty()) {
            return user;
        }
        return QStringLiteral("session");
    }

    QString endpoint() const
    {
        if (isSerial()) {
            return QStringLiteral("%1 @ %2 baud").arg(host).arg(serialBaudRate);
        }
        return QStringLiteral("%1:%2").arg(host).arg(port);
    }
};

Q_DECLARE_METATYPE(SessionProfile)

inline ConnectionMode connectionModeFromString(const QString& value)
{
    if (value.compare(QLatin1String("sftp"), Qt::CaseInsensitive) == 0
        || value.compare(QLatin1String("sftpOnly"), Qt::CaseInsensitive) == 0) {
        return ConnectionMode::SftpOnly;
    }
    if (value.compare(QLatin1String("telnet"), Qt::CaseInsensitive) == 0) {
        return ConnectionMode::Telnet;
    }
    if (value.compare(QLatin1String("serial"), Qt::CaseInsensitive) == 0
        || value.compare(QLatin1String("com"), Qt::CaseInsensitive) == 0) {
        return ConnectionMode::Serial;
    }
    if (value.compare(QLatin1String("rdp"), Qt::CaseInsensitive) == 0) {
        return ConnectionMode::Rdp;
    }
    return ConnectionMode::Ssh;
}

inline QString connectionModeToString(ConnectionMode mode)
{
    switch (mode) {
    case ConnectionMode::SftpOnly:
        return QStringLiteral("sftp");
    case ConnectionMode::Telnet:
        return QStringLiteral("telnet");
    case ConnectionMode::Serial:
        return QStringLiteral("serial");
    case ConnectionMode::Rdp:
        return QStringLiteral("rdp");
    case ConnectionMode::Ssh:
    default:
        return QStringLiteral("ssh");
    }
}

// ---- Legacy QSettings persistence (migration + graceful fallback) -------
inline QVector<SessionProfile> profilesFromQSettingsStore(QSettings& s)
{
    QVector<SessionProfile> out;
    const int n = s.beginReadArray(QStringLiteral("profiles"));
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        SessionProfile p;
        p.id = s.value(QStringLiteral("id")).toString();
        p.name = s.value(QStringLiteral("name")).toString();
        p.host = s.value(QStringLiteral("host")).toString();
        p.port = s.value(QStringLiteral("port"), 22).toInt();
        p.user = s.value(QStringLiteral("user")).toString();
        p.savePassword = s.value(QStringLiteral("savePassword"), false).toBool();
        // Read secrets only for one-shot migration into the encrypted vault.
        if (p.savePassword) {
            p.password = s.value(QStringLiteral("password")).toString();
        }
        p.privateKeyPath = s.value(QStringLiteral("privateKeyPath")).toString();
        p.privateKeyId = s.value(QStringLiteral("privateKeyId")).toString();
        p.authMethod = authMethodFromString(s.value(QStringLiteral("authMethod")).toString(),
                                            p.privateKeyId, p.privateKeyPath);
        p.saveKeyPassphrase = s.value(QStringLiteral("saveKeyPassphrase"), false).toBool();
        if (p.saveKeyPassphrase) {
            p.keyPassphrase = s.value(QStringLiteral("keyPassphrase")).toString();
        }
        p.connectionMode = connectionModeFromString(
            s.value(QStringLiteral("connectionMode"), QStringLiteral("ssh")).toString());
        p.serialBaudRate = s.value(QStringLiteral("serialBaudRate"), 115200).toInt();
        p.serialDataBits = s.value(QStringLiteral("serialDataBits"), 8).toInt();
        p.serialParity = s.value(QStringLiteral("serialParity"), QStringLiteral("none")).toString();
        p.serialStopBits = s.value(QStringLiteral("serialStopBits"), 1).toInt();
        p.serialFlowControl = s.value(QStringLiteral("serialFlowControl"), QStringLiteral("none")).toString();
        p.rdpDomain = s.value(QStringLiteral("rdpDomain")).toString();
        if (p.id.isEmpty()) {
            p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        p.normalizeAuthentication();
        out.push_back(p);
    }
    s.endArray();
    return out;
}

inline QVector<SessionProfile> profilesFromQSettings()
{
    // Prefer INI (current default), then fall back to NativeFormat leftovers.
    QSettings ini(QSettings::IniFormat, QSettings::UserScope,
                  QStringLiteral("clientosh"), QStringLiteral("clientosh"));
    QVector<SessionProfile> out = profilesFromQSettingsStore(ini);
    if (!out.isEmpty()) {
        return out;
    }
    QSettings native(QSettings::NativeFormat, QSettings::UserScope,
                     QStringLiteral("clientosh"), QStringLiteral("clientosh"));
    return profilesFromQSettingsStore(native);
}

inline void wipeLegacyQSettingsProfiles()
{
    auto wipe = [](QSettings::Format format) {
        QSettings s(format, QSettings::UserScope,
                    QStringLiteral("clientosh"), QStringLiteral("clientosh"));
        const QStringList keys = s.allKeys();
        for (const QString& key : keys) {
            const QString lower = key.toLower();
            if (lower == QLatin1String("password")
                || lower == QLatin1String("keypassphrase")
                || lower.endsWith(QLatin1String("/password"))
                || lower.endsWith(QLatin1String("\\password"))
                || lower.endsWith(QLatin1String("/keypassphrase"))
                || lower.endsWith(QLatin1String("\\keypassphrase"))) {
                s.remove(key);
            }
        }
        s.beginGroup(QLatin1String("profiles"));
        s.remove(QString());
        s.endGroup();
        s.remove(QLatin1String("profiles"));
        s.sync();
    };
    wipe(QSettings::IniFormat);
    wipe(QSettings::NativeFormat);
}

// ---- Vault (encrypted connects.json + keyring dbvault) --------------------
namespace VaultPrivate {

inline QJsonObject profileToJson(const SessionProfile& p)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), p.id);
    o.insert(QStringLiteral("name"), p.name);
    o.insert(QStringLiteral("host"), p.host);
    o.insert(QStringLiteral("port"), p.port);
    o.insert(QStringLiteral("user"), p.user);
    o.insert(QStringLiteral("savePassword"), p.savePassword);
    o.insert(QStringLiteral("privateKeyPath"), p.privateKeyPath);
    o.insert(QStringLiteral("privateKeyId"), p.privateKeyId);
    o.insert(QStringLiteral("authMethod"), authMethodToString(p.authMethod));
    o.insert(QStringLiteral("saveKeyPassphrase"), p.saveKeyPassphrase);
    o.insert(QStringLiteral("connectionMode"), connectionModeToString(p.connectionMode));
    o.insert(QStringLiteral("system"), p.system);
    o.insert(QStringLiteral("serialBaudRate"), p.serialBaudRate);
    o.insert(QStringLiteral("serialDataBits"), p.serialDataBits);
    o.insert(QStringLiteral("serialParity"), p.serialParity);
    o.insert(QStringLiteral("serialStopBits"), p.serialStopBits);
    o.insert(QStringLiteral("serialFlowControl"), p.serialFlowControl);
    o.insert(QStringLiteral("rdpDomain"), p.rdpDomain);
    return o;
}

inline SessionProfile profileFromJson(const QJsonObject& o)
{
    SessionProfile p;
    p.id = o.value(QStringLiteral("id")).toString();
    p.name = o.value(QStringLiteral("name")).toString();
    p.host = o.value(QStringLiteral("host")).toString();
    p.port = o.value(QStringLiteral("port")).toInt(22);
    p.user = o.value(QStringLiteral("user")).toString();
    p.savePassword = o.value(QStringLiteral("savePassword")).toBool(false);
    p.privateKeyPath = o.value(QStringLiteral("privateKeyPath")).toString();
    p.privateKeyId = o.value(QStringLiteral("privateKeyId")).toString();
    p.authMethod = authMethodFromString(o.value(QStringLiteral("authMethod")).toString(),
                                        p.privateKeyId, p.privateKeyPath);
    p.saveKeyPassphrase = o.value(QStringLiteral("saveKeyPassphrase")).toBool(false);
    p.connectionMode = connectionModeFromString(
        o.value(QStringLiteral("connectionMode")).toString(QStringLiteral("ssh")));
    p.system = o.value(QStringLiteral("system")).toString();
    p.serialBaudRate = o.value(QStringLiteral("serialBaudRate")).toInt(115200);
    p.serialDataBits = o.value(QStringLiteral("serialDataBits")).toInt(8);
    p.serialParity = o.value(QStringLiteral("serialParity")).toString(QStringLiteral("none"));
    p.serialStopBits = o.value(QStringLiteral("serialStopBits")).toInt(1);
    p.serialFlowControl = o.value(QStringLiteral("serialFlowControl")).toString(QStringLiteral("none"));
    p.rdpDomain = o.value(QStringLiteral("rdpDomain")).toString();
    p.normalizeAuthentication();
    // Leave empty ids empty here — loadProfiles() assigns and persists once.
    return p;
}

} // namespace VaultPrivate

inline bool saveProfiles(const QVector<SessionProfile>& profiles)
{
    // 1) Persist non-sensitive metadata to the encrypted connects.json.
    QJsonArray arr;
    for (const SessionProfile& p : profiles) {
        arr.append(VaultPrivate::profileToJson(p));
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 2);
    root.insert(QStringLiteral("profiles"), arr);
    const QByteArray plain = QJsonDocument(root).toJson(QJsonDocument::Compact);

    VaultManager vault;
    bool ok = vault.saveConnectsJson(plain);

    // 2) Store/remove sensitive fields in the keyring-protected dbvault.
    QSet<QString> storedKeyIds;
    for (const SessionProfile& p : profiles) {
        if (p.savePassword) {
            QByteArray password = p.password.toUtf8();
            ok = vault.storeSecret(p.id, QStringLiteral("password"), password) && ok;
            password.fill('\0');
        } else {
            ok = vault.removeSecret(p.id, QStringLiteral("password")) && ok;
        }

        if (p.authMethod == AuthMethod::StoredKey && !p.privateKeyId.trimmed().isEmpty()) {
            storedKeyIds.insert(p.privateKeyId);
            // Remove the old per-profile copy after migration.
            ok = vault.removeSecret(p.id, QStringLiteral("keyPassphrase")) && ok;
        } else if (p.authMethod == AuthMethod::KeyFile && p.saveKeyPassphrase) {
            QByteArray passphrase = p.keyPassphrase.toUtf8();
            ok = vault.storeSecret(p.id, QStringLiteral("keyPassphrase"), passphrase) && ok;
            passphrase.fill('\0');
        } else {
            ok = vault.removeSecret(p.id, QStringLiteral("keyPassphrase")) && ok;
        }
    }
    for (const QString& id : storedKeyIds) {
        const SessionProfile* owner = nullptr;
        for (const SessionProfile& profile : profiles) {
            if (profile.authMethod == AuthMethod::StoredKey
                && profile.privateKeyId == id
                && profile.saveKeyPassphrase
                && !profile.keyPassphrase.isEmpty()) {
                owner = &profile;
                break;
            }
        }
        if (owner) {
            QByteArray passphrase = owner->keyPassphrase.toUtf8();
            ok = vault.storeStoredKeyPassphrase(id, passphrase) && ok;
            passphrase.fill('\0');
        } else {
            ok = vault.removeStoredKeyPassphrase(id) && ok;
        }
    }
    return ok;
}

inline QVector<SessionProfile> loadProfiles()
{
    VaultManager vault;

    QByteArray plain;
    const VaultManager::LoadOutcome oc = vault.loadConnectsJson(plain);

    // First run / nothing saved yet (or legacy QSettings fallback on corruption):
    // migrate any leftover plaintext QSettings profiles into the encrypted vault,
    // then wipe them so passwords never remain in INI/registry.
    if (oc == VaultManager::LoadOutcome::NotFound || oc == VaultManager::LoadOutcome::Corrupt) {
        QVector<SessionProfile> legacy = profilesFromQSettings();
        if (!legacy.isEmpty()) {
            saveProfiles(legacy);
            wipeLegacyQSettingsProfiles();
        }
        return legacy;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(plain);
    if (doc.isNull() || !doc.isObject()) {
        return {};
    }
    const QJsonArray arr = doc.object().value(QStringLiteral("profiles")).toArray();

    QVector<SessionProfile> out;
    out.reserve(arr.size());
    bool generatedIds = false;
    bool migratedKeyPassphrases = false;
    for (const QJsonValue& v : arr) {
        SessionProfile p = VaultPrivate::profileFromJson(v.toObject());
        if (p.id.isEmpty()) {
            p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            generatedIds = true;
        }
        // Pull sensitive material from the keyring-protected dbvault.
        QByteArray secret;
        if (p.savePassword && vault.retrieveSecret(p.id, QStringLiteral("password"), secret)) {
            p.password = QString::fromUtf8(secret);
            secret.fill('\0');
        }
        if (p.saveKeyPassphrase) {
            bool found = false;
            if (p.authMethod == AuthMethod::StoredKey) {
                found = vault.retrieveStoredKeyPassphrase(p.privateKeyId, secret);
                if (!found && vault.retrieveSecret(p.id, QStringLiteral("keyPassphrase"), secret)) {
                    const bool migrated = vault.storeStoredKeyPassphrase(p.privateKeyId, secret);
                    migratedKeyPassphrases = migrated || migratedKeyPassphrases;
                    found = true; // The legacy secret is still usable for this launch.
                }
            } else if (p.authMethod == AuthMethod::KeyFile) {
                found = vault.retrieveSecret(p.id, QStringLiteral("keyPassphrase"), secret);
            }
            if (found) {
                p.keyPassphrase = QString::fromUtf8(secret);
                secret.fill('\0');
            }
        }
        out.push_back(p);
    }

    // If plaintext leftovers still hold secrets the vault is missing, import them
    // into dbvault first — then permanently delete the plaintext stores.
    {
        const QVector<SessionProfile> legacy = profilesFromQSettings();
        bool mergedSecrets = false;
        if (!legacy.isEmpty()) {
            QHash<QString, SessionProfile> legacyById;
            for (const SessionProfile& lp : legacy) {
                if (!lp.id.isEmpty()) {
                    legacyById.insert(lp.id, lp);
                }
            }
            for (SessionProfile& p : out) {
                const SessionProfile lp = legacyById.value(p.id);
                if (lp.id.isEmpty()) {
                    continue;
                }
                if (p.savePassword && p.password.isEmpty() && !lp.password.isEmpty()) {
                    p.password = lp.password;
                    mergedSecrets = true;
                }
                if (p.saveKeyPassphrase && p.keyPassphrase.isEmpty() && !lp.keyPassphrase.isEmpty()) {
                    p.keyPassphrase = lp.keyPassphrase;
                    mergedSecrets = true;
                }
            }
            if (mergedSecrets || generatedIds || migratedKeyPassphrases) {
                saveProfiles(out);
                generatedIds = false;
            }
            wipeLegacyQSettingsProfiles();
        } else {
            // No legacy profiles, but scrub any stray plaintext secret keys.
            wipeLegacyQSettingsProfiles();
            if (generatedIds || migratedKeyPassphrases) {
                saveProfiles(out);
            }
        }
    }
    return out;
}

inline SessionProfile makeProfile(const QString& host, int port, const QString& user,
                                  const QString& password, const QString& name = {})
{
    SessionProfile p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.host = host;
    p.port = port;
    p.user = user;
    p.password = password;
    p.name = name;
    return p;
}
