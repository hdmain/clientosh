#include "ServerStatsClient.h"
#include "AppSettings.h"
#include "NetworkProxyManager.h"
#include "PrivateKeyLoader.h"
#include "SshPasswordAuth.h"

#include <libssh/libssh.h>

#include <QSettings>
#include <QTimer>

#include <cstdlib>

namespace {
constexpr const char* kClassicKeyExchange =
    "curve25519-sha256,curve25519-sha256@libssh.org,"
    "ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,"
    "diffie-hellman-group-exchange-sha256,"
    "diffie-hellman-group16-sha512,diffie-hellman-group18-sha512,"
    "diffie-hellman-group14-sha256";

// Compact Linux metrics; one SSH exec per poll.
constexpr const char* kStatsCommand =
    "awk '/^cpu /{u=$2+$3+$4+$7+$8+$9; i=$5+$6; print \"CPU\",i,u+i}' /proc/stat 2>/dev/null; "
    "awk '/^MemTotal:/{t=$2} /^MemAvailable:/{a=$2} /^MemFree:/{f=$2} "
    "END{if(a==0)a=f; if(t>0) print \"MEM\", (t-a)*1024, t*1024}' "
    "/proc/meminfo 2>/dev/null; "
    "df -PB1 / 2>/dev/null | awk 'NR==2{print \"DISK\",$3+0,$2+0}'";
}

ServerStatsClient::ServerStatsClient(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<ServerStats>("ServerStats");
    qRegisterMetaType<SessionProfile>("SessionProfile");
}

ServerStatsClient::~ServerStatsClient()
{
    cleanup();
}

void ServerStatsClient::cleanup()
{
    if (m_timer) {
        m_timer->stop();
        m_timer->deleteLater();
        m_timer = nullptr;
    }
    if (m_session) {
        auto* session = static_cast<ssh_session>(m_session);
        ssh_disconnect(session);
        ssh_free(session);
        m_session = nullptr;
    }
    m_connected = false;
    m_havePrevCpu = false;
    m_prevIdle = 0;
    m_prevTotal = 0;
    m_proxyTunnel = {};
}

bool ServerStatsClient::authenticate(const SessionProfile& profile, QString* errorOut)
{
    auto* session = static_cast<ssh_session>(m_session);
    const QString keyPath = profile.privateKeyPath.trimmed();
    const QString keyId = profile.privateKeyId.trimmed();

    if (profile.authMethod == AuthMethod::SshAgent) {
        if (ssh_userauth_agent(session, nullptr) == SSH_AUTH_SUCCESS) return true;
        if (errorOut) {
            *errorOut = QStringLiteral("SSH agent authentication failed: %1")
                            .arg(QString::fromUtf8(ssh_get_error(session)));
        }
        return false;
    }

    if (profile.authMethod == AuthMethod::StoredKey
        || profile.authMethod == AuthMethod::KeyFile) {
        ssh_key privkey = nullptr;
        QString loadErr;
        if (!loadProfilePrivateKey(profile, &privkey, &loadErr)) {
            if (errorOut) {
                *errorOut = loadErr.isEmpty() ? QStringLiteral("failed to load private key") : loadErr;
            }
            return false;
        }
        const int rc = ssh_userauth_publickey(session, nullptr, privkey);
        ssh_key_free(privkey);
        if (rc == SSH_AUTH_SUCCESS) {
            return true;
        }
        if (errorOut) {
            *errorOut = QStringLiteral("key auth failed: %1")
                            .arg(QString::fromUtf8(ssh_get_error(session)));
        }
        return false;
    }

    if (profile.authMethod != AuthMethod::Password || profile.password.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("no credentials for stats probe");
        }
        return false;
    }

    QString authErr;
    if (!sshUserauthPasswordFlexible(session, profile.password, profile.user, &authErr)) {
        if (errorOut) {
            *errorOut = QStringLiteral("auth failed: %1").arg(authErr);
        }
        return false;
    }
    return true;
}

void ServerStatsClient::start(const SessionProfile& profile)
{
    stop();

    ssh_session session = ssh_new();
    if (!session) {
        emit failed(QStringLiteral("failed to create ssh session"));
        return;
    }
    m_session = session;

    const QByteArray host = profile.host.toUtf8();
    const QByteArray user = profile.user.toUtf8();
    int port = profile.port;
    ssh_options_set(session, SSH_OPTIONS_HOST, host.constData());
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session, SSH_OPTIONS_USER, user.constData());
    long timeoutSec = 15;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeoutSec);
    ssh_options_set(session, SSH_OPTIONS_KEY_EXCHANGE, kClassicKeyExchange);
    int strict = 0;
    ssh_options_set(session, SSH_OPTIONS_STRICTHOSTKEYCHECK, &strict);

    m_proxyTunnel = {};
    QString tunnelError;
    if (!NetworkProxy::openSshTunnel(profile.host, port, &m_proxyTunnel, &tunnelError)) {
        cleanup();
        emit failed(tunnelError);
        return;
    }
    if (m_proxyTunnel.socket) {
        NetworkProxy::applyTunnelToSshSession(session, m_proxyTunnel);
    }

    if (ssh_connect(session) != SSH_OK) {
        const QString err = NetworkProxy::sshConnectErrorMessage(session);
        cleanup();
        emit failed(QStringLiteral("stats connect failed: %1").arg(err));
        return;
    }

    QString authError;
    if (!authenticate(profile, &authError)) {
        cleanup();
        emit failed(authError);
        return;
    }

    m_connected = true;
    m_timer = new QTimer(this);
    m_timer->setInterval(AppSettings::statsIntervalSec() * 1000);
    connect(m_timer, &QTimer::timeout, this, &ServerStatsClient::poll);
    m_timer->start();
    poll();
}

void ServerStatsClient::stop()
{
    cleanup();
}

bool ServerStatsClient::ensureConnected(QString* errorOut)
{
    if (m_connected && m_session) {
        return true;
    }
    if (errorOut) {
        *errorOut = QStringLiteral("stats probe not connected");
    }
    return false;
}

QByteArray ServerStatsClient::execCommand(const char* command, QString* errorOut)
{
    auto* session = static_cast<ssh_session>(m_session);
    ssh_channel channel = ssh_channel_new(session);
    if (!channel) {
        if (errorOut) {
            *errorOut = QStringLiteral("failed to open channel");
        }
        return {};
    }

    if (ssh_channel_open_session(channel) != SSH_OK) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(ssh_get_error(session));
        }
        ssh_channel_free(channel);
        return {};
    }

    if (ssh_channel_request_exec(channel, command) != SSH_OK) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(ssh_get_error(session));
        }
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return {};
    }

    QByteArray out;
    char buf[1024];
    for (;;) {
        const int n = ssh_channel_read_timeout(channel, buf, sizeof(buf), 0, 8000);
        if (n > 0) {
            out.append(buf, n);
            continue;
        }
        if (n == SSH_EOF || ssh_channel_is_eof(channel)) {
            break;
        }
        if (n == SSH_ERROR) {
            if (errorOut) {
                *errorOut = QString::fromUtf8(ssh_get_error(session));
            }
            break;
        }
        if (n == 0) {
            break;
        }
    }

    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    return out;
}

void ServerStatsClient::poll()
{
    QString err;
    if (!ensureConnected(&err)) {
        emit failed(err);
        return;
    }

    const QByteArray raw = execCommand(kStatsCommand, &err);
    if (raw.isEmpty() && !err.isEmpty()) {
        emit failed(err);
        return;
    }

    ServerStats stats;
    const QString text = QString::fromUtf8(raw);
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    for (const QString& line : lines) {
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 3) {
            continue;
        }
        if (parts[0] == QLatin1String("CPU")) {
            const quint64 idle = parts[1].toULongLong();
            const quint64 total = parts[2].toULongLong();
            if (m_havePrevCpu && total > m_prevTotal) {
                const quint64 dIdle = idle - m_prevIdle;
                const quint64 dTotal = total - m_prevTotal;
                if (dTotal > 0) {
                    stats.cpuPercent = 100.0 * (1.0 - (double(dIdle) / double(dTotal)));
                    if (stats.cpuPercent < 0.0) {
                        stats.cpuPercent = 0.0;
                    }
                    if (stats.cpuPercent > 100.0) {
                        stats.cpuPercent = 100.0;
                    }
                }
            }
            m_prevIdle = idle;
            m_prevTotal = total;
            m_havePrevCpu = true;
        } else if (parts[0] == QLatin1String("MEM")) {
            stats.memUsedBytes = parts[1].toLongLong();
            stats.memTotalBytes = parts[2].toLongLong();
        } else if (parts[0] == QLatin1String("DISK")) {
            stats.diskUsedBytes = parts[1].toLongLong();
            stats.diskTotalBytes = parts[2].toLongLong();
        }
    }

    stats.valid = (stats.cpuPercent >= 0.0) || (stats.memTotalBytes > 0) || (stats.diskTotalBytes > 0);
    emit statsUpdated(stats);
}
