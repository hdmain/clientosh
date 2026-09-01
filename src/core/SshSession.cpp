#include "SshSession.h"

#include "NetworkProxyManager.h"
#include "PrivateKeyLoader.h"
#include "SshPasswordAuth.h"

#include <libssh/libssh.h>

#include <QAtomicInt>

#ifdef Q_OS_WIN
#  include <winsock2.h>
#else
#  include <sys/socket.h>
#endif

namespace {
QAtomicInt g_sshInited{0};

void ensureSshLibrary()
{
    if (g_sshInited.loadAcquire() == 0) {
        if (ssh_init() == SSH_OK) {
            g_sshInited.storeRelease(1);
        }
    }
}

// libssh 0.12 may advertise ML-KEM hybrid KEX that fails keypair generation
// with some OpenSSL builds ("Failed to construct client init buffer").
constexpr const char* kClassicKeyExchange =
    "curve25519-sha256,curve25519-sha256@libssh.org,"
    "ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,"
    "diffie-hellman-group-exchange-sha256,"
    "diffie-hellman-group16-sha512,diffie-hellman-group18-sha512,"
    "diffie-hellman-group14-sha256";

/** Infer a short OS label from an SSH server banner string. Unknown → empty. */
QString detectSystemFromBanner(ssh_session session)
{
    QByteArray banner;
    if (const char* b = ssh_get_serverbanner(session)) {
        banner = QByteArray(b);
    }
    const QByteArray low = banner.toLower();

    if (low.contains("ubuntu")) {
        return QStringLiteral("Ubuntu");
    }
    if (low.contains("debian")) {
        return QStringLiteral("Debian");
    }
    if (low.contains("raspbian") || low.contains("raspberry")) {
        return QStringLiteral("Raspbian");
    }
    if (low.contains("kali")) {
        return QStringLiteral("Kali");
    }
    if (low.contains("alpine")) {
        return QStringLiteral("Alpine Linux");
    }
    if (low.contains("arch")) {
        return QStringLiteral("Arch");
    }
    if (low.contains("fedora")) {
        return QStringLiteral("Fedora");
    }
    if (low.contains("centos")) {
        return QStringLiteral("CentOS");
    }
    if (low.contains("red hat") || low.contains("rhel")) {
        return QStringLiteral("Red Hat");
    }
    if (low.contains("suse") || low.contains("opensuse")) {
        return QStringLiteral("openSUSE");
    }
    if (low.contains("freebsd")) {
        return QStringLiteral("FreeBSD");
    }
    if (low.contains("openbsd")) {
        return QStringLiteral("OpenBSD");
    }
    if (low.contains("netbsd")) {
        return QStringLiteral("NetBSD");
    }
    if (low.contains("linux")) {
        return QStringLiteral("Linux");
    }
    if (low.contains("apple") || low.contains("darwin") || low.contains("macos")
        || low.contains("mac os")) {
        return QStringLiteral("macOS");
    }
    if (low.contains("microsoft") || low.contains("windows") || low.contains("wilco")) {
        return QStringLiteral("Windows");
    }
    return {};
}
} // namespace

SshSession::SshSession(QObject* parent)
    : QThread(parent)
{
    ensureSshLibrary();
}

SshSession::~SshSession()
{
    disconnectFromHost();
    if (!wait(8000)) {
        // Last resort: avoid destroying a live QThread (undefined behavior).
        terminate();
        wait(2000);
    }
}

bool SshSession::isConnected() const
{
    QMutexLocker lock(&m_mutex);
    return m_connected;
}

bool SshSession::stopRequested() const
{
    QMutexLocker lock(&m_mutex);
    return m_stopRequested;
}

void SshSession::abortTransport()
{
    // Caller must hold m_mutex when touching m_session during connect/auth.
    if (!m_session) {
        return;
    }
    const socket_t fd = ssh_get_fd(m_session);
    if (fd == SSH_INVALID_SOCKET) {
        return;
    }
#ifdef Q_OS_WIN
    ::shutdown(static_cast<SOCKET>(fd), SD_BOTH);
#else
    ::shutdown(fd, SHUT_RDWR);
#endif
}

void SshSession::connectTo(const QString& host,
                           int port,
                           const QString& user,
                           const QString& password,
                           const QString& privateKeyPath,
                           const QString& privateKeyId,
                           const QString& keyPassphrase,
                           AuthMethod authMethod)
{
    bool needStart = false;
    {
        QMutexLocker lock(&m_mutex);
        m_host = host;
        m_port = port;
        m_user = user;
        m_password = password;
        m_privateKeyPath = privateKeyPath;
        m_privateKeyId = privateKeyId;
        m_keyPassphrase = keyPassphrase;
        m_authMethod = authMethod;
        m_pendingWrite.clear();
        // Keep m_cols/m_rows from resizePty() — do not reset size here.
        m_resizePending = false;

        if (isRunning()) {
            // Soft-restart: abort current attempt and reconnect with new params.
            m_restartRequested = true;
            m_stopRequested = true;
            abortTransport();
        } else {
            m_stopRequested = false;
            m_restartRequested = false;
            needStart = true;
        }
    }

    if (needStart) {
        start();
    }
}

void SshSession::disconnectFromHost()
{
    QMutexLocker lock(&m_mutex);
    m_stopRequested = true;
    m_restartRequested = false;
    abortTransport();
}

void SshSession::sendData(const QByteArray& data)
{
    QMutexLocker lock(&m_mutex);
    m_pendingWrite.append(data);
}

void SshSession::resizePty(int cols, int rows)
{
    QMutexLocker lock(&m_mutex);
    m_cols = qMax(1, cols);
    m_rows = qMax(1, rows);
    m_resizePending = true;
}

void SshSession::cleanup()
{
    if (m_channel) {
        if (ssh_channel_is_open(m_channel)) {
            ssh_channel_send_eof(m_channel);
            ssh_channel_close(m_channel);
        }
        ssh_channel_free(m_channel);
        m_channel = nullptr;
    }

    ssh_session session = nullptr;
    {
        QMutexLocker lock(&m_mutex);
        session = m_session;
        m_session = nullptr;
        m_connected = false;
    }
    if (session) {
        ssh_disconnect(session);
        ssh_free(session);
    }
    m_proxyTunnel = {};
}

bool SshSession::authenticate(const QString& password,
                              const QString& privateKeyPath,
                              const QString& privateKeyId,
                              const QString& keyPassphrase,
                              AuthMethod authMethod,
                              QString* errorOut)
{
    if (authMethod == AuthMethod::SshAgent) {
        emit statusChanged(QStringLiteral("authenticating with SSH agent..."));
        const int rc = ssh_userauth_agent(m_session, nullptr);
        if (rc == SSH_AUTH_SUCCESS) {
            return true;
        }
        if (errorOut) {
            *errorOut = QStringLiteral("SSH agent authentication failed: %1")
                            .arg(QString::fromUtf8(ssh_get_error(m_session)));
        }
        return false;
    }

    if (authMethod == AuthMethod::StoredKey || authMethod == AuthMethod::KeyFile) {
        emit statusChanged(QStringLiteral("authenticating with private key..."));

        SessionProfile loaderProfile;
        loaderProfile.privateKeyId = privateKeyId;
        loaderProfile.privateKeyPath = privateKeyPath;
        loaderProfile.keyPassphrase = keyPassphrase;
        loaderProfile.authMethod = authMethod;

        ssh_key privkey = nullptr;
        QString loadErr;
        const bool loaded = loadProfilePrivateKey(loaderProfile, &privkey, &loadErr);
        if (!loaded) {
            if (errorOut) {
                *errorOut = QStringLiteral("%1: %2")
                                .arg(loadErr.isEmpty() ? QStringLiteral("failed to load private key")
                                                       : loadErr,
                                     QString::fromUtf8(ssh_get_error(m_session)));
            }
            return false;
        }

        if (stopRequested()) {
            ssh_key_free(privkey);
            return false;
        }

        const int authRc = ssh_userauth_publickey(m_session, nullptr, privkey);
        ssh_key_free(privkey);

        if (authRc == SSH_AUTH_SUCCESS) {
            return true;
        }

        if (errorOut) {
            *errorOut = QStringLiteral("public-key auth failed: %1")
                            .arg(QString::fromUtf8(ssh_get_error(m_session)));
        }
        return false;
    }

    if (stopRequested()) {
        return false;
    }

    emit statusChanged(QStringLiteral("authenticating with password..."));
    const QByteArray passUtf8 = password.toUtf8();
    if (ssh_userauth_password(m_session, nullptr, passUtf8.constData()) == SSH_AUTH_SUCCESS) {
        return true;
    }
    const QString passErr = QString::fromUtf8(ssh_get_error(m_session));

    if (stopRequested()) {
        return false;
    }

    emit statusChanged(QStringLiteral("password auth failed, trying keyboard-interactive..."));
    QString kbdintErr;
    if (sshTryKbdintPassword(m_session, password, m_user, &kbdintErr)) {
        return true;
    }

    if (errorOut) {
        *errorOut = QStringLiteral("auth failed: password (%1); keyboard-interactive (%2)")
                        .arg(passErr.isEmpty() ? QStringLiteral("denied") : passErr,
                             kbdintErr.isEmpty() ? QStringLiteral("denied") : kbdintErr);
    }
    return false;
}

void SshSession::run()
{
    for (;;) {
        QString host;
        QString user;
        QString password;
        QString privateKeyPath;
        QString privateKeyId;
        QString keyPassphrase;
        AuthMethod authMethod = AuthMethod::Password;
        int port = 22;
        int cols = 80;
        int rows = 24;

        {
            QMutexLocker lock(&m_mutex);
            host = m_host;
            user = m_user;
            password = m_password;
            privateKeyPath = m_privateKeyPath;
            privateKeyId = m_privateKeyId;
            keyPassphrase = m_keyPassphrase;
            authMethod = m_authMethod;
            port = m_port;
            cols = m_cols;
            rows = m_rows;
            m_stopRequested = false;
            m_restartRequested = false;
            m_connected = false;
        }

        emit statusChanged(QStringLiteral("connecting to %1:%2...").arg(host).arg(port));

        ssh_session session = ssh_new();
        if (!session) {
            emit errorOccurred(QStringLiteral("failed to create ssh session"));
            break;
        }

        {
            QMutexLocker lock(&m_mutex);
            m_session = session;
        }

        ssh_options_set(session, SSH_OPTIONS_HOST, host.toUtf8().constData());
        ssh_options_set(session, SSH_OPTIONS_PORT, &port);
        ssh_options_set(session, SSH_OPTIONS_USER, user.toUtf8().constData());
        long timeoutSec = 12;
        ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeoutSec);
        ssh_options_set(session, SSH_OPTIONS_KEY_EXCHANGE, kClassicKeyExchange);

        int strict = 0;
        ssh_options_set(session, SSH_OPTIONS_STRICTHOSTKEYCHECK, &strict);

        m_proxyTunnel = {};
        QString tunnelError;
        if (!NetworkProxy::openSshTunnel(host, port, &m_proxyTunnel, &tunnelError)) {
            cleanup();
            emit errorOccurred(tunnelError);
            break;
        }
        if (m_proxyTunnel.socket) {
            NetworkProxy::applyTunnelToSshSession(session, m_proxyTunnel);
        }

        if (stopRequested()) {
            cleanup();
            break;
        }

        if (ssh_connect(session) != SSH_OK) {
            const bool cancelled = stopRequested();
            const QString err = NetworkProxy::sshConnectErrorMessage(session);
            cleanup();
            if (!cancelled) {
                emit errorOccurred(QStringLiteral("connect failed: %1").arg(err));
            }
            goto after_attempt;
        }

        emit systemDetected(detectSystemFromBanner(session));

        if (stopRequested()) {
            cleanup();
            goto after_attempt;
        }

        {
            QString authError;
            if (!authenticate(password, privateKeyPath, privateKeyId, keyPassphrase,
                              authMethod, &authError)) {
                const bool cancelled = stopRequested();
                cleanup();
                if (!cancelled) {
                    emit errorOccurred(authError.isEmpty()
                                           ? QStringLiteral("authentication cancelled")
                                           : authError);
                }
                goto after_attempt;
            }
        }

        if (stopRequested()) {
            cleanup();
            goto after_attempt;
        }

        m_channel = ssh_channel_new(session);
        if (!m_channel) {
            cleanup();
            emit errorOccurred(QStringLiteral("failed to create channel"));
            goto after_attempt;
        }

        if (ssh_channel_open_session(m_channel) != SSH_OK) {
            const QString err = QString::fromUtf8(ssh_get_error(session));
            cleanup();
            emit errorOccurred(QStringLiteral("open session failed: %1").arg(err));
            goto after_attempt;
        }

        // Re-read size after layout may have updated m_cols/m_rows during auth.
        {
            QMutexLocker lock(&m_mutex);
            cols = m_cols;
            rows = m_rows;
            m_resizePending = false;
        }

        if (ssh_channel_request_pty_size(m_channel, "xterm-256color", cols, rows) != SSH_OK) {
            if (ssh_channel_request_pty(m_channel) != SSH_OK) {
                const QString err = QString::fromUtf8(ssh_get_error(session));
                cleanup();
                emit errorOccurred(QStringLiteral("pty request failed: %1").arg(err));
                goto after_attempt;
            }
            ssh_channel_change_pty_size(m_channel, cols, rows);
        }

        ssh_channel_request_env(m_channel, "LANG", "C.UTF-8");
        ssh_channel_request_env(m_channel, "LC_ALL", "C.UTF-8");

        if (ssh_channel_request_shell(m_channel) != SSH_OK) {
            const QString err = QString::fromUtf8(ssh_get_error(session));
            cleanup();
            emit errorOccurred(QStringLiteral("shell request failed: %1").arg(err));
            goto after_attempt;
        }

        {
            QMutexLocker lock(&m_mutex);
            m_connected = true;
        }

        emit connected();
        emit statusChanged(QStringLiteral("connected %1@%2:%3").arg(user, host).arg(port));

        char buffer[4096];
        while (true) {
            bool stop = false;
            QByteArray toWrite;
            bool doResize = false;
            int newCols = cols;
            int newRows = rows;

            {
                QMutexLocker lock(&m_mutex);
                stop = m_stopRequested;
                if (!m_pendingWrite.isEmpty()) {
                    toWrite.swap(m_pendingWrite);
                }
                if (m_resizePending) {
                    doResize = true;
                    newCols = m_cols;
                    newRows = m_rows;
                    m_resizePending = false;
                }
            }

            if (stop || ssh_channel_is_eof(m_channel) || !ssh_channel_is_open(m_channel)) {
                break;
            }

            if (doResize) {
                cols = newCols;
                rows = newRows;
                ssh_channel_change_pty_size(m_channel, cols, rows);
            }

            if (!toWrite.isEmpty()) {
                int offset = 0;
                while (offset < toWrite.size()) {
                    const int n = ssh_channel_write(m_channel,
                                                    toWrite.constData() + offset,
                                                    static_cast<uint32_t>(toWrite.size() - offset));
                    if (n < 0) {
                        emit errorOccurred(QStringLiteral("write failed: %1")
                                               .arg(QString::fromUtf8(ssh_get_error(session))));
                        stop = true;
                        break;
                    }
                    offset += n;
                }
                if (stop) {
                    break;
                }
            }

            const int nbytes = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 0);
            if (nbytes > 0) {
                emit dataReceived(QByteArray(buffer, nbytes));
            } else if (nbytes < 0) {
                emit errorOccurred(QStringLiteral("read failed: %1")
                                       .arg(QString::fromUtf8(ssh_get_error(session))));
                break;
            } else {
                ssh_channel_poll_timeout(m_channel, 50, 0);
            }
        }

        {
            QMutexLocker lock(&m_mutex);
            m_connected = false;
        }

        cleanup();
        emit disconnected();
        emit statusChanged(QStringLiteral("disconnected"));

    after_attempt:
        bool restart = false;
        {
            QMutexLocker lock(&m_mutex);
            restart = m_restartRequested;
            if (restart) {
                m_restartRequested = false;
                m_stopRequested = false;
            }
        }
        if (!restart) {
            break;
        }
        emit statusChanged(QStringLiteral("reconnecting..."));
    }
}
