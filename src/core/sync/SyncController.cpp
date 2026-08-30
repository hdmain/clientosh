#include "SyncController.h"

#include "SyncConfig.h"
#include "SyncCrypto.h"
#include "SyncKey.h"
#include "SyncPayload.h"

#include "CryptoEngine.h"

#include <QDateTime>
#include <QHash>
#include <QMetaType>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace {
inline QString b64url(const QByteArray& data)
{
    return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding
                                             | QByteArray::OmitTrailingEquals));
}

/** Same connection target → treat as one host even when UUIDs differ. */
QString profileContentKey(const SessionProfile& p)
{
    const QString host = p.host.trimmed().toLower();
    const QString user = p.user.trimmed().toLower();
    if (host.isEmpty() && user.isEmpty()) {
        return {};
    }
    const QString mode = connectionModeToString(p.connectionMode);
    const QString serial = p.isSerial()
        ? QStringLiteral("\n%1\n%2\n%3\n%4\n%5")
              .arg(p.serialBaudRate).arg(p.serialDataBits, 0, 10)
              .arg(p.serialParity).arg(p.serialStopBits).arg(p.serialFlowControl)
        : QString();
    return mode + QLatin1Char('\n') + host + QLatin1Char('\n') + QString::number(p.port)
        + QLatin1Char('\n') + user + serial;
}

QString keyContentKey(const StoredKey& k)
{
    const QString name = k.name.trimmed().toLower();
    if (name.isEmpty() && k.pem.isEmpty()) {
        return {};
    }
    return name + QLatin1Char('\n') + QString::fromLatin1(k.pem.left(64).toHex());
}

/**
 * Collapse duplicate hosts/keys that share an id or the same connection target.
 * Prefer entries that appear earlier in `preferred` order (remote first).
 */
SyncPayload dedupePayload(const SyncPayload& in)
{
    SyncPayload out = in;

    QHash<QString, SessionProfile> byId;
    QHash<QString, QString> contentToId;
    QVector<SessionProfile> profiles;
    profiles.reserve(in.profiles.size());

    for (SessionProfile p : in.profiles) {
        if (p.id.isEmpty()) {
            p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        if (byId.contains(p.id)) {
            continue;
        }
        const QString ck = profileContentKey(p);
        if (!ck.isEmpty() && contentToId.contains(ck)) {
            // Same host/user/port under a different UUID — keep the first one.
            continue;
        }
        byId.insert(p.id, p);
        if (!ck.isEmpty()) {
            contentToId.insert(ck, p.id);
        }
        profiles.append(p);
    }
    out.profiles = profiles;

    QHash<QString, StoredKey> keysById;
    QHash<QString, QString> keyContentToId;
    QVector<StoredKey> keys;
    keys.reserve(in.keys.size());
    for (StoredKey k : in.keys) {
        if (k.id.isEmpty()) {
            k.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        if (keysById.contains(k.id)) {
            continue;
        }
        const QString ck = keyContentKey(k);
        if (!ck.isEmpty() && keyContentToId.contains(ck)) {
            continue;
        }
        keysById.insert(k.id, k);
        if (!ck.isEmpty()) {
            keyContentToId.insert(ck, k.id);
        }
        keys.append(k);
    }
    out.keys = keys;
    return out;
}

bool payloadShrunk(const SyncPayload& before, const SyncPayload& after)
{
    return after.profiles.size() < before.profiles.size()
        || after.keys.size() < before.keys.size();
}

SyncPayload mergeSnapshots(const SyncPayload& local, const SyncPayload& remote)
{
    // Remote wins on id conflicts; local-only entries are kept only when they
    // are not the same connection (host/port/user) as something already remote.
    // That stops "same host, new UUID" duplicates after join / re-seed.
    SyncPayload combined = remote;
    QSet<QString> seenIds;
    QSet<QString> seenContent;
    for (const SessionProfile& p : remote.profiles) {
        if (!p.id.isEmpty()) {
            seenIds.insert(p.id);
        }
        const QString ck = profileContentKey(p);
        if (!ck.isEmpty()) {
            seenContent.insert(ck);
        }
    }
    for (const SessionProfile& p : local.profiles) {
        if (!p.id.isEmpty() && seenIds.contains(p.id)) {
            continue;
        }
        const QString ck = profileContentKey(p);
        if (!ck.isEmpty() && seenContent.contains(ck)) {
            continue;
        }
        combined.profiles.append(p);
        if (!p.id.isEmpty()) {
            seenIds.insert(p.id);
        }
        if (!ck.isEmpty()) {
            seenContent.insert(ck);
        }
    }

    QSet<QString> seenKeyIds;
    QSet<QString> seenKeyContent;
    for (const StoredKey& k : remote.keys) {
        if (!k.id.isEmpty()) {
            seenKeyIds.insert(k.id);
        }
        const QString ck = keyContentKey(k);
        if (!ck.isEmpty()) {
            seenKeyContent.insert(ck);
        }
    }
    for (const StoredKey& k : local.keys) {
        if (!k.id.isEmpty() && seenKeyIds.contains(k.id)) {
            continue;
        }
        const QString ck = keyContentKey(k);
        if (!ck.isEmpty() && seenKeyContent.contains(ck)) {
            continue;
        }
        combined.keys.append(k);
        if (!k.id.isEmpty()) {
            seenKeyIds.insert(k.id);
        }
        if (!ck.isEmpty()) {
            seenKeyContent.insert(ck);
        }
    }

    // Notes: remote wins when present; otherwise keep local notebook.
    if (combined.notesMarkdown.trimmed().isEmpty()
        && !local.notesMarkdown.trimmed().isEmpty()) {
        combined.notesMarkdown = local.notesMarkdown;
    }

    return dedupePayload(combined);
}
} // namespace

SyncController::SyncController(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<SyncKey>("SyncKey");

    m_pollTimer = new QTimer(this);
    m_pollTimer->setTimerType(Qt::VeryCoarseTimer);
    m_pollTimer->setSingleShot(false);
    connect(m_pollTimer, &QTimer::timeout, this, &SyncController::onPollTick);
}

SyncController::~SyncController()
{
    stopWorker();
}

void SyncController::setDataProvider(SerializeFn serialize, ApplyFn apply)
{
    m_serialize = std::move(serialize);
    m_apply = std::move(apply);
}

void SyncController::setDeviceId(const QString& id, const QString& label)
{
    m_deviceId = id;
    m_deviceLabel = label;
}

void SyncController::setState(State st)
{
    if (m_state == st) {
        return;
    }
    m_state = st;
    emit stateChanged(st);
}

QString SyncController::syncKeyString() const
{
    return m_state == State::Disabled ? QString() : SyncKeyCodec::encode(m_key);
}

bool SyncController::hasToken() const
{
    return m_tokenLocal && !m_token.isEmpty();
}

void SyncController::setPollIntervalSec(int seconds)
{
    m_pollIntervalSec = std::max(10, seconds);
    if (m_state == State::Active && !m_paused) {
        startPollTimer();
    }
}

void SyncController::startPollTimer()
{
    m_pollTimer->stop();
    if (m_paused) {
        return;
    }
    m_pollTimer->start(m_pollIntervalSec * 1000);
}

void SyncController::startWorker()
{
    if (m_thread) {
        return;
    }
    m_thread = new QThread(this);
    m_worker = new SyncWorker;
    m_worker->moveToThread(m_thread);

    connect(this, &SyncController::requestCreate, m_worker, &SyncWorker::runCreate);
    connect(this, &SyncController::requestPush, m_worker, &SyncWorker::runPush);
    connect(this, &SyncController::requestPull, m_worker, &SyncWorker::runPull);
    connect(this, &SyncController::requestTestToken, m_worker, &SyncWorker::runTestToken);

    connect(m_worker, &SyncWorker::createFinished, this, &SyncController::onCreateFinished);
    connect(m_worker, &SyncWorker::pushFinished, this, &SyncController::onPushFinished);
    connect(m_worker, &SyncWorker::pullFinished, this, &SyncController::onPullFinished);
    connect(m_worker, &SyncWorker::testFinished, this, &SyncController::onTestFinished);

    m_thread->start();
}

void SyncController::stopWorker()
{
    if (!m_thread) {
        m_pending = PendingOp::None;
        return;
    }
    m_thread->quit();
    m_thread->wait(35000);
    delete m_worker;
    m_worker = nullptr;
    delete m_thread;
    m_thread = nullptr;
    m_pending = PendingOp::None;
}

// ---- Lifecycle ---------------------------------------------------------

void SyncController::createSync(const QString& token, const QString& gistDescription)
{
    const QString trimmed = token.trimmed();
    if (trimmed.isEmpty()) {
        emit errorOccurred(QStringLiteral("A GitHub token is required to create a sync."));
        return;
    }
    if (m_state == State::Connecting) {
        return;
    }
    if (m_pending != PendingOp::None) {
        emit errorOccurred(QStringLiteral("Wait for the current GitHub request to finish, then try again."));
        return;
    }
    startWorker();
    setState(State::Connecting);
    m_paused = false;
    m_token = trimmed;
    m_tokenLocal = true;
    m_gistDesc = gistDescription.trimmed().isEmpty()
        ? QStringLiteral("clientosh saved-sessions sync")
        : gistDescription.trimmed();

    // First-time setup on Computer 1: generate all local crypto keys + identity.
    m_key = SyncCrypto::generateSyncKey();
    m_key.token = trimmed.toUtf8();
    m_filename = QStringLiteral("clientosh-sync-%1.json").arg(b64url(m_key.syncUuid));

    m_lastKnownRev = 0;
    SyncConfig::setLastKnownRev(0);

    QByteArray encrypted;
    try {
        encrypted = SyncCrypto::encryptPayload(m_key, serializeWithFraming()).toBase64();
    } catch (const CryptoEngine::CryptoError& e) {
        setState(State::Disabled);
        emit errorOccurred(QStringLiteral("Could not encrypt the sync payload: %1").arg(e.message));
        return;
    }

    m_pending = PendingOp::Create;
    emit requestCreate(m_key, m_token, m_gistDesc, m_filename, encrypted);
}

void SyncController::joinSync(const QString& syncKeyText, const QString& token)
{
    beginJoin(syncKeyText, token, false);
}

void SyncController::restoreExisting(const QString& syncKeyText, const QString& token)
{
    beginJoin(syncKeyText, token, true);
}

void SyncController::beginJoin(const QString& syncKeyText, const QString& token, bool restoring)
{
    const SyncKey parsed = SyncKeyCodec::decode(syncKeyText);
    if (!parsed.isValid()) {
        emit errorOccurred(QStringLiteral("That sync key is malformed. Please copy the exact key and try again."));
        return;
    }
    QString effectiveToken = token.trimmed();
    if (effectiveToken.isEmpty() && !parsed.token.isEmpty()) {
        effectiveToken = QString::fromUtf8(parsed.token).trimmed();
    }
    if (effectiveToken.isEmpty()) {
        emit errorOccurred(QStringLiteral("A GitHub token is required to read the synchronized gist."));
        return;
    }
    if (m_state == State::Connecting) {
        return;
    }
    if (m_pending != PendingOp::None) {
        emit errorOccurred(QStringLiteral("Wait for the current GitHub request to finish, then try again."));
        return;
    }
    startWorker();
    setState(State::Connecting);
    m_paused = false;
    m_key = parsed;
    m_key.token = effectiveToken.toUtf8();
    m_token = effectiveToken;
    m_tokenLocal = true;
    m_gistDesc.clear();
    m_filename = QStringLiteral("clientosh-sync-%1.json").arg(b64url(m_key.syncUuid));
    m_lastKnownRev = restoring ? std::max(0, SyncConfig::lastKnownRev()) : 0;
    if (restoring) {
        m_pending = PendingOp::Pull;
    } else {
        m_pending = PendingOp::Join;
    }
    emit requestPull(m_key, m_token, m_filename);
}

void SyncController::testToken(const QString& token)
{
    const QString trimmed = token.trimmed();
    if (trimmed.isEmpty()) {
        emit errorOccurred(QStringLiteral("Enter a GitHub token to test."));
        return;
    }
    startWorker();
    emit requestTestToken(trimmed);
}

void SyncController::disable()
{
    m_pollTimer->stop();
    setState(State::Disabled);
    m_key = SyncKey{};
    m_token.clear();
    m_tokenLocal = false;
    m_gistDesc.clear();
    m_filename.clear();
    m_lastKnownRev = 0;
    m_paused = false;
    m_pushQueued = false;
    m_syncNowQueued = false;
    m_pending = PendingOp::None;
    stopWorker();
    emit statusMessage(QStringLiteral("Synchronization disabled. Local data is unchanged."));
}

void SyncController::setPaused(bool paused)
{
    m_paused = paused;
    if (paused) {
        m_pollTimer->stop();
        emit statusMessage(QStringLiteral("Sync paused on this machine. Local data is unchanged."));
        return;
    }
    if (m_state == State::Active) {
        startPollTimer();
        emit statusMessage(QStringLiteral("Sync resumed."));
    }
}

void SyncController::pullNow()
{
    if (m_state == State::Disabled || m_key.gistId.isEmpty() || !m_tokenLocal || m_paused) {
        return;
    }
    if (m_pending != PendingOp::None) {
        return;
    }
    startWorker();
    m_pending = PendingOp::Pull;
    emit requestPull(m_key, m_token, m_filename);
}

void SyncController::pushNow()
{
    if (m_state == State::Disabled || m_key.gistId.isEmpty() || !m_tokenLocal || m_paused) {
        return;
    }
    push();
}

void SyncController::syncNow()
{
    if (m_state == State::Disabled || m_key.gistId.isEmpty() || !m_tokenLocal || m_paused) {
        return;
    }
    m_syncNowQueued = true;
    if (m_pending != PendingOp::None) {
        return;
    }
    pullNow();
}

void SyncController::push()
{
    if (m_state == State::Disabled || m_key.gistId.isEmpty() || !m_tokenLocal || m_paused) {
        return;
    }
    if (m_pending != PendingOp::None) {
        m_pushQueued = true;
        return;
    }
    startWorker();
    QByteArray encrypted;
    try {
        encrypted = SyncCrypto::encryptPayload(m_key, serializeWithFraming()).toBase64();
    } catch (const CryptoEngine::CryptoError& e) {
        emit errorOccurred(QStringLiteral("Could not encrypt the sync payload: %1").arg(e.message));
        return;
    }
    m_pending = PendingOp::Push;
    emit requestPush(m_key, m_token, m_filename, encrypted);
}

void SyncController::flushQueuedPush()
{
    if (m_syncNowQueued) {
        m_syncNowQueued = false;
        m_pushQueued = false;
        push();
        return;
    }
    if (m_pushQueued) {
        m_pushQueued = false;
        push();
    }
}

void SyncController::onPollTick()
{
    if (m_state == State::Disabled || m_paused) {
        return;
    }
    pullNow();
}

// ---- Result handlers ---------------------------------------------------

void SyncController::onCreateFinished(bool ok, const QString& gistId, const QString& error)
{
    if (m_pending != PendingOp::Create) {
        return;
    }
    m_pending = PendingOp::None;
    if (!ok) {
        setState(State::Disabled);
        emit errorOccurred(QStringLiteral("Could not create the sync gist: %1").arg(error));
        return;
    }
    m_key.gistId = gistId;
    SyncConfig::setSyncKeyText(SyncKeyCodec::encode(m_key));
    m_lastKnownRev = std::max(m_lastKnownRev, 1);
    SyncConfig::setLastKnownRev(m_lastKnownRev);
    m_connectedOnce = true;
    setState(State::Active);
    startPollTimer();
    emit statusMessage(QStringLiteral("Sync created. Share the key below with other devices."));
}

void SyncController::onPushFinished(bool ok, const QString& error)
{
    if (m_pending != PendingOp::Push) {
        return;
    }
    m_pending = PendingOp::None;
    if (!ok) {
        emit errorOccurred(QStringLiteral("Upload failed: %1").arg(error));
        flushQueuedPush();
        return;
    }
    ++m_lastKnownRev;
    SyncConfig::setLastKnownRev(m_lastKnownRev);
    emit statusMessage(QStringLiteral("Changes uploaded."));
    flushQueuedPush();
}

void SyncController::onPullFinished(bool ok, bool notFound, const QString& body,
                                    const QString& error)
{
    const PendingOp op = m_pending;
    if (op != PendingOp::Pull && op != PendingOp::Join) {
        return;
    }
    Q_UNUSED(notFound);
    m_pending = PendingOp::None;

    if (!ok) {
        if (op == PendingOp::Join || m_state == State::Connecting) {
            setState(State::Disabled);
            emit errorOccurred(op == PendingOp::Join
                                   ? QStringLiteral("Could not join the sync: %1").arg(error)
                                   : QStringLiteral("Could not reconnect to sync: %1").arg(error));
        } else {
            emit errorOccurred(QStringLiteral("Sync check failed: %1").arg(error));
        }
        m_syncNowQueued = false;
        return;
    }

    SyncPayload remote;
    try {
        const QByteArray cipher = QByteArray::fromBase64(body.toLatin1());
        if (cipher.isEmpty() && !body.trimmed().isEmpty()) {
            throw std::runtime_error("base64");
        }
        if (cipher.isEmpty()) {
            remote = SyncPayload{};
        } else {
            const QByteArray plain = SyncCrypto::decryptPayload(m_key, cipher);
            bool parsedOk = false;
            remote = SyncPayloadCodec::fromJson(plain, &parsedOk);
            if (!parsedOk) {
                throw std::runtime_error("payload parse");
            }
        }
    } catch (...) {
        if (op == PendingOp::Join || m_state == State::Connecting) {
            setState(State::Disabled);
        }
        emit errorOccurred(QStringLiteral("Received data could not be decrypted. The sync key may be wrong."));
        m_syncNowQueued = false;
        return;
    }

    const bool joining = (op == PendingOp::Join);
    reconcileFromRemote(remote, joining);

    if (joining) {
        SyncConfig::setSyncKeyText(SyncKeyCodec::encode(m_key));
        setState(State::Active);
        startPollTimer();
        emit statusMessage(QStringLiteral("Connected to an existing sync."));
        const SyncPayload local = currentLocalPayload();
        if (!local.profiles.isEmpty() || !local.keys.isEmpty()
            || !local.notesMarkdown.trimmed().isEmpty()) {
            m_pushQueued = true;
        }
    } else if (m_state == State::Connecting) {
        SyncConfig::setSyncKeyText(SyncKeyCodec::encode(m_key));
        setState(State::Active);
        startPollTimer();
        emit statusMessage(QStringLiteral("Reconnected to sync."));
    }

    flushQueuedPush();
}

void SyncController::onTestFinished(bool ok, const QString& error)
{
    if (ok) {
        emit statusMessage(QStringLiteral("GitHub token is valid."));
    } else {
        emit errorOccurred(QStringLiteral("GitHub token rejected: %1").arg(error));
    }
}

// ---- Reconciliation ----------------------------------------------------

SyncPayload SyncController::currentLocalPayload() const
{
    if (!m_serialize) {
        return {};
    }
    bool ok = false;
    return SyncPayloadCodec::fromJson(m_serialize(), &ok);
}

void SyncController::reconcileFromRemote(const SyncPayload& remote, bool joining)
{
    const bool remoteEmpty = remote.profiles.isEmpty() && remote.keys.isEmpty()
        && remote.notesMarkdown.trimmed().isEmpty()
        && (remote.deviceId.isEmpty() || remote.rev <= 1);
    const SyncPayload local = currentLocalPayload();
    const bool localHasData = !local.profiles.isEmpty() || !local.keys.isEmpty()
        || !local.notesMarkdown.trimmed().isEmpty();

    // Never adopt an empty/placeholder gist over existing local sessions.
    if (remoteEmpty && localHasData) {
        if (joining || m_lastKnownRev == 0 || remote.rev <= m_lastKnownRev) {
            m_pushQueued = true;
            return;
        }
        // A strictly newer empty revision is a real "delete everything" from
        // another device — fall through and apply it.
    }

    if (joining && localHasData
        && (!remote.profiles.isEmpty() || !remote.keys.isEmpty()
            || !remote.notesMarkdown.trimmed().isEmpty())) {
        SyncPayload merged = mergeSnapshots(local, remote);
        merged.rev = std::max(remote.rev, m_lastKnownRev);
        merged.timestampMs = std::max(remote.timestampMs, local.timestampMs);
        bool applied = false;
        if (m_apply) {
            applied = m_apply(SyncPayloadCodec::toJson(merged));
        }
        m_lastKnownRev = std::max(m_lastKnownRev, merged.rev);
        SyncConfig::setLastKnownRev(m_lastKnownRev);
        if (applied) {
            emit dataUpdated();
            emit statusMessage(QStringLiteral("Merged local and remote sessions."));
        }
        // Always push after join so the healed/deduped snapshot becomes canonical.
        m_pushQueued = true;
        return;
    }

    const bool remoteNewer = remote.rev > m_lastKnownRev;
    if (!remoteNewer) {
        // Heal duplicates that may already live locally (or only on the gist)
        // after an older buggy join — even when the revision did not advance.
        const SyncPayload localClean = dedupePayload(local);
        const SyncPayload remoteClean = dedupePayload(remote);
        const bool localDirty = payloadShrunk(local, localClean);
        const bool remoteDirty = payloadShrunk(remote, remoteClean);
        if (localDirty) {
            bool applied = false;
            if (m_apply) {
                SyncPayload healed = localClean;
                healed.rev = std::max(remote.rev, m_lastKnownRev);
                healed.timestampMs = std::max(remote.timestampMs, local.timestampMs);
                applied = m_apply(SyncPayloadCodec::toJson(healed));
            }
            if (applied) {
                emit dataUpdated();
                emit statusMessage(QStringLiteral("Removed duplicate synced hosts."));
            }
            m_pushQueued = true;
        } else if (remoteDirty) {
            // Local is already clean; upload it so the gist stops reintroducing
            // duplicates on the next pull from another machine.
            m_pushQueued = true;
        }
        return;
    }

    SyncPayload toApply = dedupePayload(remote);
    bool applied = false;
    if (m_apply) {
        applied = m_apply(SyncPayloadCodec::toJson(toApply));
    }
    m_lastKnownRev = std::max(m_lastKnownRev, remote.rev);
    SyncConfig::setLastKnownRev(m_lastKnownRev);

    if (applied) {
        emit dataUpdated();
        emit statusMessage(QStringLiteral("Newer data downloaded from sync."));
    }
    // If the gist still carried host duplicates, push the cleaned snapshot back.
    if (payloadShrunk(remote, toApply)) {
        m_pushQueued = true;
    }
}

QByteArray SyncController::serializeWithFraming()
{
    if (m_serialize) {
        const QByteArray own = m_serialize();
        SyncPayload parsed = SyncPayloadCodec::fromJson(own, nullptr);
        parsed.rev = m_lastKnownRev + 1;
        parsed.timestampMs = static_cast<qint64>(QDateTime::currentMSecsSinceEpoch());
        parsed.deviceId = m_deviceId;
        parsed.deviceLabel = m_deviceLabel;
        return SyncPayloadCodec::toJson(parsed);
    }
    SyncPayload payload;
    payload.rev = m_lastKnownRev + 1;
    payload.timestampMs = static_cast<qint64>(QDateTime::currentMSecsSinceEpoch());
    payload.deviceId = m_deviceId;
    payload.deviceLabel = m_deviceLabel;
    return SyncPayloadCodec::toJson(payload);
}
