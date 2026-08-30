#pragma once

#include "SessionProfile.h"
#include "VaultManager.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

/**
 * The encrypted, versioned snapshot that is stored on the Gist.
 *
 * Unlike the on-disk vault (which deliberately keeps secrets inside the OS
 * keyring and out of connects.json), a sync payload is end-to-end encrypted
 * before it ever touches the network, so it safely carries the *complete*
 * profile data — including saved passwords, key passphrases and private key
 * material — inside the ciphertext. Nothing here is ever written in plaintext.
 *
 * Versioning: `rev` is a strictly increasing commit number and `timestamp` is a
 * wall-clock millisecond stamp. A later rev (or, on ties, a later timestamp)
 * wins during conflict reconciliation between devices.
 */
struct SyncPayload
{
    int format = 1;
    int rev = 0;                 // monotonic "commit" number
    qint64 timestampMs = 0;      // device clock at write time
    QString deviceId;            // stable identifier of the last writer
    QString deviceLabel;         // human-friendly label of the last writer (host)

    QVector<SessionProfile> profiles;
    QVector<StoredKey> keys;     // reusable private keyring entries (with PEM)
    QString notesMarkdown;       // Notes tab body (HTML; legacy Markdown accepted)
};

namespace SyncPayloadCodec {

/** Serialize to compact JSON bytes (not yet encrypted). */
QByteArray toJson(const SyncPayload& payload);

/** Parse from JSON bytes. Empty/deviceId means the payload is invalid. */
SyncPayload fromJson(const QByteArray& bytes, bool* okOut = nullptr);

} // namespace SyncPayloadCodec
