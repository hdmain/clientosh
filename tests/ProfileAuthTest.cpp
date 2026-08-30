#include "core/SessionProfile.h"
#include "core/sync/SyncPayload.h"

#include <QJsonObject>

#include <cassert>

int main()
{
    QJsonObject legacyStored;
    legacyStored.insert(QStringLiteral("privateKeyId"), QStringLiteral("key-1"));
    SessionProfile profile = VaultPrivate::profileFromJson(legacyStored);
    assert(profile.authMethod == AuthMethod::StoredKey);
    assert(profile.usesPrivateKey());

    QJsonObject legacyFile;
    legacyFile.insert(QStringLiteral("privateKeyPath"), QStringLiteral("/tmp/id_ed25519"));
    profile = VaultPrivate::profileFromJson(legacyFile);
    assert(profile.authMethod == AuthMethod::KeyFile);

    profile.authMethod = AuthMethod::SshAgent;
    profile.privateKeyId.clear();
    profile.privateKeyPath.clear();
    const QJsonObject agentJson = VaultPrivate::profileToJson(profile);
    const SessionProfile agentRoundTrip = VaultPrivate::profileFromJson(agentJson);
    assert(agentRoundTrip.authMethod == AuthMethod::SshAgent);
    assert(agentRoundTrip.usesSshAgent());
    assert(!agentRoundTrip.usesPrivateKey());

    SyncPayload payload;
    payload.profiles.push_back(agentRoundTrip);
    StoredKey key;
    key.id = QStringLiteral("key-1");
    key.name = QStringLiteral("deploy");
    key.type = QStringLiteral("ssh-ed25519");
    key.fingerprint = QStringLiteral("SHA256:test");
    key.pem = QByteArrayLiteral("private-key-data");
    payload.keys.push_back(key);

    bool ok = false;
    const SyncPayload decoded = SyncPayloadCodec::fromJson(SyncPayloadCodec::toJson(payload), &ok);
    assert(ok);
    assert(decoded.profiles.size() == 1);
    assert(decoded.profiles.front().authMethod == AuthMethod::SshAgent);
    assert(decoded.keys.size() == 1);
    assert(decoded.keys.front().fingerprint == QStringLiteral("SHA256:test"));

    payload.notesMarkdown = QStringLiteral("# Hello\n\n||secret||");
    const SyncPayload decodedNotes =
        SyncPayloadCodec::fromJson(SyncPayloadCodec::toJson(payload), &ok);
    assert(ok);
    assert(decodedNotes.notesMarkdown.contains(QStringLiteral("||secret||")));
    return 0;
}
