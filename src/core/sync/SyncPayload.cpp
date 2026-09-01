#include "SyncPayload.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QUuid>

namespace {

QJsonObject profileToJsonFull(const SessionProfile& p)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), p.id);
    o.insert(QStringLiteral("name"), p.name);
    o.insert(QStringLiteral("host"), p.host);
    o.insert(QStringLiteral("port"), p.port);
    o.insert(QStringLiteral("user"), p.user);
    o.insert(QStringLiteral("password"), p.password);
    o.insert(QStringLiteral("savePassword"), p.savePassword);
    o.insert(QStringLiteral("privateKeyPath"), p.privateKeyPath);
    o.insert(QStringLiteral("privateKeyId"), p.privateKeyId);
    o.insert(QStringLiteral("authMethod"), authMethodToString(p.authMethod));
    o.insert(QStringLiteral("keyPassphrase"), p.keyPassphrase);
    o.insert(QStringLiteral("saveKeyPassphrase"), p.saveKeyPassphrase);
    const QString mode = connectionModeToString(p.connectionMode);
    o.insert(QStringLiteral("connectionMode"), mode);
    o.insert(QStringLiteral("system"), p.system);
    o.insert(QStringLiteral("serialBaudRate"), p.serialBaudRate);
    o.insert(QStringLiteral("serialDataBits"), p.serialDataBits);
    o.insert(QStringLiteral("serialParity"), p.serialParity);
    o.insert(QStringLiteral("serialStopBits"), p.serialStopBits);
    o.insert(QStringLiteral("serialFlowControl"), p.serialFlowControl);
    o.insert(QStringLiteral("rdpDomain"), p.rdpDomain);
    return o;
}

SessionProfile profileFromJsonFull(const QJsonObject& o)
{
    SessionProfile p;
    p.id = o.value(QStringLiteral("id")).toString();
    p.name = o.value(QStringLiteral("name")).toString();
    p.host = o.value(QStringLiteral("host")).toString();
    p.port = o.value(QStringLiteral("port")).toInt(22);
    p.user = o.value(QStringLiteral("user")).toString();
    p.password = o.value(QStringLiteral("password")).toString();
    p.savePassword = o.value(QStringLiteral("savePassword")).toBool(false);
    p.privateKeyPath = o.value(QStringLiteral("privateKeyPath")).toString();
    p.privateKeyId = o.value(QStringLiteral("privateKeyId")).toString();
    p.authMethod = authMethodFromString(o.value(QStringLiteral("authMethod")).toString(),
                                        p.privateKeyId, p.privateKeyPath);
    p.keyPassphrase = o.value(QStringLiteral("keyPassphrase")).toString();
    p.saveKeyPassphrase = o.value(QStringLiteral("saveKeyPassphrase")).toBool(false);
    const QString mode = o.value(QStringLiteral("connectionMode")).toString();
    p.connectionMode = connectionModeFromString(mode);
    if (p.port <= 0 && !p.isSerial()) {
        if (p.isTelnet()) {
            p.port = 23;
        } else if (p.isRdp()) {
            p.port = 3389;
        } else {
            p.port = 22;
        }
    }
    p.system = o.value(QStringLiteral("system")).toString();
    p.serialBaudRate = o.value(QStringLiteral("serialBaudRate")).toInt(115200);
    p.serialDataBits = o.value(QStringLiteral("serialDataBits")).toInt(8);
    p.serialParity = o.value(QStringLiteral("serialParity")).toString(QStringLiteral("none"));
    p.serialStopBits = o.value(QStringLiteral("serialStopBits")).toInt(1);
    p.serialFlowControl = o.value(QStringLiteral("serialFlowControl")).toString(QStringLiteral("none"));
    p.rdpDomain = o.value(QStringLiteral("rdpDomain")).toString();
    p.normalizeAuthentication();
    if (p.id.isEmpty()) {
        p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    return p;
}

QJsonObject keyToJsonFull(const StoredKey& k)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), k.id);
    o.insert(QStringLiteral("name"), k.name);
    o.insert(QStringLiteral("type"), k.type);
    o.insert(QStringLiteral("fingerprint"), k.fingerprint);
    o.insert(QStringLiteral("pem"), QString::fromLatin1(k.pem.toBase64()));
    o.insert(QStringLiteral("hasPassphrase"), k.hasPassphrase);
    return o;
}

StoredKey keyFromJsonFull(const QJsonObject& o)
{
    StoredKey k;
    k.id = o.value(QStringLiteral("id")).toString();
    k.name = o.value(QStringLiteral("name")).toString();
    k.type = o.value(QStringLiteral("type")).toString();
    k.fingerprint = o.value(QStringLiteral("fingerprint")).toString();
    const QByteArray pemB64 = o.value(QStringLiteral("pem")).toString().toLatin1();
    k.pem = QByteArray::fromBase64(pemB64);
    k.hasPassphrase = o.value(QStringLiteral("hasPassphrase")).toBool(false);
    return k;
}

} // namespace

namespace SyncPayloadCodec {

QByteArray toJson(const SyncPayload& payload)
{
    QJsonObject root;
    root.insert(QStringLiteral("format"), payload.format);
    root.insert(QStringLiteral("rev"), payload.rev);
    root.insert(QStringLiteral("ts"), payload.timestampMs);
    root.insert(QStringLiteral("deviceId"), payload.deviceId);
    root.insert(QStringLiteral("deviceLabel"), payload.deviceLabel);

    QJsonArray profArr;
    for (const SessionProfile& p : payload.profiles) {
        profArr.append(profileToJsonFull(p));
    }
    root.insert(QStringLiteral("profiles"), profArr);

    QJsonArray keyArr;
    for (const StoredKey& k : payload.keys) {
        keyArr.append(keyToJsonFull(k));
    }
    root.insert(QStringLiteral("keys"), keyArr);
    root.insert(QStringLiteral("notesMarkdown"), payload.notesMarkdown);

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

SyncPayload fromJson(const QByteArray& bytes, bool* okOut)
{
    SyncPayload payload;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (doc.isNull() || !doc.isObject()) {
        if (okOut) {
            *okOut = false;
        }
        return payload;
    }
    const QJsonObject root = doc.object();
    payload.format = root.value(QStringLiteral("format")).toInt(1);
    payload.rev = static_cast<int>(root.value(QStringLiteral("rev")).toInteger(
        root.value(QStringLiteral("rev")).toInt(0)));
    {
        const QJsonValue tsVal = root.value(QStringLiteral("ts"));
        // ts fits in 53-bit JS integer; toDouble then cast is lossless at ms granularity.
        // Use qint64 round-trip so we don't depend on value type.
        const qint64 tsLegacy = static_cast<qint64>(tsVal.toDouble(0.0));
        payload.timestampMs = tsLegacy;
    }
    payload.deviceId = root.value(QStringLiteral("deviceId")).toString();
    payload.deviceLabel = root.value(QStringLiteral("deviceLabel")).toString();

    const QJsonArray profArr = root.value(QStringLiteral("profiles")).toArray();
    payload.profiles.reserve(profArr.size());
    for (const QJsonValue& v : profArr) {
        payload.profiles.append(profileFromJsonFull(v.toObject()));
    }

    const QJsonArray keyArr = root.value(QStringLiteral("keys")).toArray();
    payload.keys.reserve(keyArr.size());
    for (const QJsonValue& v : keyArr) {
        payload.keys.append(keyFromJsonFull(v.toObject()));
    }

    payload.notesMarkdown = root.value(QStringLiteral("notesMarkdown")).toString();

    if (okOut) {
        *okOut = true;
    }
    return payload;
}

} // namespace SyncPayloadCodec
