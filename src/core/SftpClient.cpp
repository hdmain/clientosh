#include "SftpClient.h"

#include "core/AppSettings.h"
#include "core/NetworkProxyManager.h"
#include "core/PrivateKeyLoader.h"
#include "core/SshPasswordAuth.h"

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <fcntl.h>

#include <algorithm>
#include <cstdlib>

namespace {
constexpr const char* kClassicKeyExchange =
    "curve25519-sha256,curve25519-sha256@libssh.org,"
    "ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,"
    "diffie-hellman-group-exchange-sha256,"
    "diffie-hellman-group16-sha512,diffie-hellman-group18-sha512,"
    "diffie-hellman-group14-sha256";

// NOTE: libssh's sftp_open() accepts POSIX O_* access flags (O_RDONLY, O_WRONLY,
// O_CREAT, O_TRUNC, ...) and internally translates them to the SFTP wire-level
// SSH_FXF_* bitmap. Passing the raw SSH_FXF_* constants here is WRONG because
// their bit values (e.g. SSH_FXF_WRITE=0x02, SSH_FXF_CREAT=0x08,
// SSH_FXF_TRUNC=0x10) do not line up with the O_* values on any libc; on
// Windows/mingw SSH_FXF_WRITE is even equal to O_RDWR. That mismatch means
// CREAT/TRUNC are never sent to the server, so creating a file fails with
// SSH_FX_NO_SUCH_FILE. Always build access types from the portable O_* flags.
constexpr int sftpWriteFlags = O_WRONLY | O_CREAT | O_TRUNC;
constexpr int sftpReadFlags = O_RDONLY;

QString normalizeRemotePath(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.contains(QLatin1String("//"))) {
        path.replace(QLatin1String("//"), QLatin1String("/"));
    }
    if (path.isEmpty()) {
        path = QStringLiteral("/");
    }
    return path;
}

QString sftpErrorName(int code)
{
    switch (code) {
    case SSH_FX_OK:
        return QStringLiteral("OK");
    case SSH_FX_EOF:
        return QStringLiteral("EOF");
    case SSH_FX_NO_SUCH_FILE:
        return QStringLiteral("NO_SUCH_FILE");
    case SSH_FX_PERMISSION_DENIED:
        return QStringLiteral("PERMISSION_DENIED");
    case SSH_FX_FAILURE:
        return QStringLiteral("FAILURE");
    case SSH_FX_BAD_MESSAGE:
        return QStringLiteral("BAD_MESSAGE");
    case SSH_FX_NO_CONNECTION:
        return QStringLiteral("NO_CONNECTION");
    case SSH_FX_CONNECTION_LOST:
        return QStringLiteral("CONNECTION_LOST");
    case SSH_FX_OP_UNSUPPORTED:
        return QStringLiteral("OP_UNSUPPORTED");
    case SSH_FX_INVALID_HANDLE:
        return QStringLiteral("INVALID_HANDLE");
    case SSH_FX_NO_SUCH_PATH:
        return QStringLiteral("NO_SUCH_PATH");
    case SSH_FX_FILE_ALREADY_EXISTS:
        return QStringLiteral("FILE_ALREADY_EXISTS");
    case SSH_FX_WRITE_PROTECT:
        return QStringLiteral("WRITE_PROTECT");
    case SSH_FX_NO_MEDIA:
        return QStringLiteral("NO_MEDIA");
    default:
        return QStringLiteral("code=%1").arg(code);
    }
}

QString fileErrorDetail(const QFile& file, const QString& path)
{
    const QFileDevice::FileError e = file.error();
    const QString s = file.errorString();
    if (s.isEmpty()) {
        return QStringLiteral("%1 (err=%2)").arg(path).arg(int(e));
    }
    return QStringLiteral("%1 — %2 (err=%3)").arg(path, s).arg(int(e));
}

QString fileErrorDetailForPath(const QString& path, const QString& op)
{
    QFile f(path);
    QFileDevice::FileError e = f.error();
    Q_UNUSED(e)
    return QStringLiteral("%1: %2").arg(op, path);
}
} // namespace

SftpClient::SftpClient(QObject* parent)
    : QObject(parent)
    , m_verbose(AppSettings::sftpVerboseLogging())
{
    qRegisterMetaType<SftpEntry>("SftpEntry");
    qRegisterMetaType<QVector<SftpEntry>>("QVector<SftpEntry>");
    qRegisterMetaType<SessionProfile>("SessionProfile");
}

SftpClient::~SftpClient()
{
    cleanup();
}

void SftpClient::vlog(const QString& msg)
{
    if (!m_verbose) {
        return;
    }
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    emit debugLog(QStringLiteral("[sftp %1] %2").arg(ts, msg));
}

QString SftpClient::sftpErrorString() const
{
    if (!m_session || !m_sftp) {
        return QStringLiteral("no session");
    }
    auto* session = static_cast<ssh_session>(m_session);
    auto* sftp = static_cast<sftp_session>(m_sftp);
    const int code = sftp_get_error(sftp);
    const QString sshErr = QString::fromUtf8(ssh_get_error(session)).trimmed();
    const QString fxName = sftpErrorName(code);
    if (!sshErr.isEmpty()) {
        return QStringLiteral("%1 (%2) — %3").arg(fxName).arg(code).arg(sshErr);
    }
    return QStringLiteral("%1 (%2)").arg(fxName).arg(code);
}

void SftpClient::setVerboseEnabled(bool enabled)
{
    m_verbose = enabled;
    vlog(enabled ? QStringLiteral("verbose logging enabled") : QStringLiteral("verbose logging disabled"));
}

void SftpClient::requestCancelTransfer()
{
    m_cancelRequested.storeRelaxed(true);
}

void SftpClient::cancelTransfer()
{
    requestCancelTransfer();
}


bool SftpClient::mkdirP(const QString& remoteDir, void* sftpVoid)
{
    auto* sftp = static_cast<sftp_session>(sftpVoid);
    const QString norm = normalizeRemotePath(remoteDir);
    if (norm == QLatin1String("/")) {
        return true;
    }
    // Quick check: does the full path already exist as a directory?
    {
        sftp_attributes st = sftp_stat(sftp, norm.toUtf8().constData());
        if (st) {
            const bool isDir = (st->type == SSH_FILEXFER_TYPE_DIRECTORY);
            sftp_attributes_free(st);
            if (isDir) {
                vlog(QStringLiteral("mkdirP: '%1' already exists (dir)").arg(norm));
                return true;
            }
            vlog(QStringLiteral("mkdirP: '%1' exists but is not a dir").arg(norm));
            return false;
        }
        const int code = sftp_get_error(sftp);
        vlog(QStringLiteral("mkdirP: full stat '%1' -> %2 (%3)").arg(norm, sftpErrorName(code), sftpErrorString()));
        // If permission denied, don't try to create — surface it.
        if (code == SSH_FX_PERMISSION_DENIED) {
            return false;
        }
    }
    const QStringList parts = norm.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString cur;
    for (const QString& part : parts) {
        cur += QLatin1Char('/') + part;
        const QByteArray curUtf8 = cur.toUtf8();
        sftp_attributes pre = sftp_stat(sftp, curUtf8.constData());
        if (pre) {
            const bool isDir = (pre->type == SSH_FILEXFER_TYPE_DIRECTORY);
            sftp_attributes_free(pre);
            if (isDir) {
                vlog(QStringLiteral("mkdirP: '%1' exists, skip").arg(cur));
                continue;
            }
            vlog(QStringLiteral("mkdirP: '%1' exists but is not a directory").arg(cur));
            return false;
        }
        const int preErr = sftp_get_error(sftp);
        vlog(QStringLiteral("mkdirP: stat '%1' -> %2 (%3)").arg(cur, sftpErrorName(preErr), sftpErrorString()));
        if (preErr == SSH_FX_PERMISSION_DENIED) {
            // Can't even stat — likely permission issue, don't blindly mkdir.
            return false;
        }
        if (sftp_mkdir(sftp, curUtf8.constData(), 0755) == SSH_OK) {
            vlog(QStringLiteral("mkdirP: created '%1'").arg(cur));
            continue;
        }
        const int err = sftp_get_error(sftp);
        vlog(QStringLiteral("mkdirP: mkdir '%1' failed err=%2 (%3)").arg(cur, sftpErrorName(err), sftpErrorString()));
        if (err == SSH_FX_FILE_ALREADY_EXISTS || err == SSH_FX_OK) {
            continue;
        }
        // Some servers return FAILURE for existing dir — re-stat.
        sftp_attributes st = sftp_stat(sftp, curUtf8.constData());
        if (st) {
            const bool isDir = (st->type == SSH_FILEXFER_TYPE_DIRECTORY);
            sftp_attributes_free(st);
            if (isDir) {
                vlog(QStringLiteral("mkdirP: '%1' now exists after race, continue").arg(cur));
                continue;
            }
            vlog(QStringLiteral("mkdirP: '%1' exists but is not a directory err=%2").arg(cur, sftpErrorName(err)));
            return false;
        }
        const int postErr = sftp_get_error(sftp);
        vlog(QStringLiteral("mkdirP: mkdir '%1' failed and post-stat %2 (%3)")
                 .arg(cur, sftpErrorName(postErr), sftpErrorString()));
        return false;
    }
    return true;
}

bool SftpClient::ensureParentDirExists(const QString& remoteFilePath, void* sftpVoid)
{
    const QString norm = normalizeRemotePath(remoteFilePath);
    const int slash = norm.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0) {
        // parent is "/" — always exists
        return true;
    }
    const QString parent = norm.left(slash);
    vlog(QStringLiteral("ensureParentDirExists: file='%1' parent='%2'").arg(norm, parent));
    return mkdirP(parent, sftpVoid);
}

void SftpClient::cleanup()
{
    vlog(QStringLiteral("cleanup: disconnecting"));
    if (m_sftp) {
        sftp_free(static_cast<sftp_session>(m_sftp));
        m_sftp = nullptr;
    }
    if (m_session) {
        auto* session = static_cast<ssh_session>(m_session);
        ssh_disconnect(session);
        ssh_free(session);
        m_session = nullptr;
    }
    m_proxyTunnel = {};
    m_connected = false;
    m_cwd.clear();
}

bool SftpClient::authenticate(const SessionProfile& profile, QString* errorOut)
{
    auto* session = static_cast<ssh_session>(m_session);
    const QString keyPath = profile.privateKeyPath.trimmed();
    const QString keyId = profile.privateKeyId.trimmed();

    if (profile.authMethod == AuthMethod::SshAgent) {
        vlog(QStringLiteral("authenticate: trying SSH agent"));
        emit statusChanged(QStringLiteral("authenticating with SSH agent..."));
        const int rc = ssh_userauth_agent(session, nullptr);
        if (rc == SSH_AUTH_SUCCESS) {
            return true;
        }
        if (errorOut) {
            *errorOut = QStringLiteral("SSH agent authentication failed: %1")
                            .arg(QString::fromUtf8(ssh_get_error(session)));
        }
        return false;
    }

    if (profile.authMethod == AuthMethod::StoredKey
        || profile.authMethod == AuthMethod::KeyFile) {
        const QString keyDesc = !keyId.isEmpty() ? keyId : keyPath;
        vlog(QStringLiteral("authenticate: trying private key %1").arg(keyDesc));
        emit statusChanged(QStringLiteral("authenticating with private key..."));
        ssh_key privkey = nullptr;
        QString loadErr;
        if (!loadProfilePrivateKey(profile, &privkey, &loadErr) || !privkey) {
            const QString detail = QString::fromUtf8(ssh_get_error(session));
            vlog(QStringLiteral("authenticate: failed to load private key: %1").arg(detail));
            if (errorOut) {
                *errorOut = QStringLiteral("%1: %2").arg(loadErr, detail);
            }
            if (privkey) {
                ssh_key_free(privkey);
            }
            return false;
        }

        const int rc = ssh_userauth_publickey(session, nullptr, privkey);
        ssh_key_free(privkey);
        if (rc == SSH_AUTH_SUCCESS) {
            vlog(QStringLiteral("authenticate: public-key auth succeeded"));
            return true;
        }
        const QString pubErr = QString::fromUtf8(ssh_get_error(session));
        vlog(QStringLiteral("authenticate: public-key auth failed rc=%1 err=%2").arg(rc).arg(pubErr));
        if (errorOut) {
            *errorOut = QStringLiteral("public-key auth failed for '%1': %2").arg(keyDesc, pubErr);
        }
        return false;
    }

    if (profile.authMethod != AuthMethod::Password) {
        if (errorOut) *errorOut = QStringLiteral("unsupported authentication method");
        return false;
    }
    vlog(QStringLiteral("authenticate: trying password auth for user '%1'").arg(profile.user));
    emit statusChanged(QStringLiteral("authenticating with password..."));
    QString authErr;
    if (!sshUserauthPasswordFlexible(session, profile.password, profile.user, &authErr)) {
        vlog(QStringLiteral("authenticate: password/kbdint auth failed: %1").arg(authErr));
        if (errorOut) {
            *errorOut = QStringLiteral("auth failed for %1@%2: %3")
                            .arg(profile.user, profile.host, authErr);
        }
        return false;
    }
    vlog(QStringLiteral("authenticate: password auth succeeded"));
    return true;
}

void SftpClient::connectHost(const SessionProfile& profile)
{
    cleanup();

    vlog(QStringLiteral("connectHost: %1@%2:%3").arg(profile.user, profile.host).arg(profile.port));
    emit statusChanged(QStringLiteral("connecting sftp to %1:%2...")
                           .arg(profile.host)
                           .arg(profile.port));

    ssh_session session = ssh_new();
    if (!session) {
        const QString msg = QStringLiteral("failed to create ssh session");
        vlog(msg);
        emit errorOccurred(msg);
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
    int logLevel = m_verbose ? SSH_LOG_PROTOCOL : SSH_LOG_NOLOG;
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &logLevel);

    m_proxyTunnel = {};
    QString tunnelError;
    if (!NetworkProxy::openSshTunnel(profile.host, port, &m_proxyTunnel, &tunnelError)) {
        vlog(tunnelError);
        cleanup();
        emit errorOccurred(tunnelError);
        return;
    }
    if (m_proxyTunnel.socket) {
        NetworkProxy::applyTunnelToSshSession(session, m_proxyTunnel);
    }

    vlog(QStringLiteral("connectHost: ssh_connect timeout=%1s kex=classic strictHostKeyCheck=off logLevel=%2")
             .arg(timeoutSec)
             .arg(logLevel));
    if (ssh_connect(session) != SSH_OK) {
        const QString err = NetworkProxy::sshConnectErrorMessage(session);
        vlog(QStringLiteral("connectHost: ssh_connect failed: %1").arg(err));
        cleanup();
        emit errorOccurred(QStringLiteral("connect failed to %1:%2 — %3").arg(profile.host).arg(port).arg(err));
        return;
    }
    vlog(QStringLiteral("connectHost: ssh_connect succeeded, authenticating"));

    QString authError;
    if (!authenticate(profile, &authError)) {
        vlog(QStringLiteral("connectHost: authenticate failed: %1").arg(authError));
        cleanup();
        emit errorOccurred(authError);
        return;
    }

    vlog(QStringLiteral("connectHost: creating sftp session"));
    sftp_session sftp = sftp_new(session);
    if (!sftp) {
        const QString detail = QString::fromUtf8(ssh_get_error(session));
        vlog(QStringLiteral("connectHost: sftp_new failed: %1").arg(detail));
        cleanup();
        emit errorOccurred(QStringLiteral("failed to create sftp session: %1").arg(detail));
        return;
    }
    if (sftp_init(sftp) != SSH_OK) {
        const QString detail = sftpErrorString();
        // also try ssh error before freeing
        // sftpErrorString uses m_session/m_sftp; set m_sftp temporarily for richer error
        m_sftp = sftp;
        const QString err = sftpErrorString();
        m_sftp = nullptr;
        vlog(QStringLiteral("connectHost: sftp_init failed: %1").arg(err));
        Q_UNUSED(detail)
        sftp_free(sftp);
        cleanup();
        emit errorOccurred(QStringLiteral("sftp init failed on %1: %2").arg(profile.host, err));
        return;
    }
    m_sftp = sftp;
    m_connected = true;

    char* home = sftp_canonicalize_path(sftp, ".");
    m_cwd = home ? QString::fromUtf8(home) : QStringLiteral("/");
    if (home) {
        free(home);
    }
    m_cwd = normalizeRemotePath(m_cwd);
    vlog(QStringLiteral("connectHost: connected cwd=%1").arg(m_cwd));

    emit statusChanged(QStringLiteral("sftp ready"));
    emit connected(m_cwd);
}

void SftpClient::disconnectHost()
{
    vlog(QStringLiteral("disconnectHost"));
    cleanup();
    emit disconnected();
    emit statusChanged(QStringLiteral("sftp disconnected"));
}

bool SftpClient::ensureConnected(QString* errorOut)
{
    if (m_connected && m_session && m_sftp) {
        return true;
    }
    const QString msg = QStringLiteral("sftp is not connected (connected=%1 session=%2 sftp=%3 cwd='%4')")
                            .arg(m_connected)
                            .arg(m_session ? QStringLiteral("yes") : QStringLiteral("no"))
                            .arg(m_sftp ? QStringLiteral("yes") : QStringLiteral("no"))
                            .arg(m_cwd);
    vlog(QStringLiteral("ensureConnected: %1").arg(msg));
    if (errorOut) {
        *errorOut = QStringLiteral("sftp is not connected — try reconnecting");
    }
    return false;
}

QString SftpClient::resolvePath(const QString& path) const
{
    QString p = path.trimmed();
    if (p.isEmpty()) {
        return m_cwd.isEmpty() ? QStringLiteral("/") : m_cwd;
    }
    if (p.startsWith(QLatin1Char('/'))) {
        return normalizeRemotePath(p);
    }
    if (m_cwd == QLatin1String("/")) {
        return normalizeRemotePath(QStringLiteral("/") + p);
    }
    return normalizeRemotePath(m_cwd + QLatin1Char('/') + p);
}

void SftpClient::listDirectory(const QString& path)
{
    QString err;
    if (!ensureConnected(&err)) {
        emit errorOccurred(err);
        return;
    }

    auto* sftp = static_cast<sftp_session>(m_sftp);
    const QString target = resolvePath(path);
    vlog(QStringLiteral("listDirectory: target='%1' (raw='%2' cwd='%3')").arg(target, path, m_cwd));
    emit statusChanged(QStringLiteral("listing %1").arg(target));

    sftp_dir dir = sftp_opendir(sftp, target.toUtf8().constData());
    if (!dir) {
        const QString detail = sftpErrorString();
        vlog(QStringLiteral("listDirectory: opendir failed target='%1' err=%2").arg(target, detail));
        emit errorOccurred(QStringLiteral("opendir failed for '%1': %2").arg(target, detail));
        return;
    }

    QVector<SftpEntry> entries;
    while (sftp_attributes attr = sftp_readdir(sftp, dir)) {
        SftpEntry e;
        e.name = QString::fromUtf8(attr->name ? attr->name : "");
        if (e.name == QLatin1String(".") || e.name == QLatin1String("..")) {
            sftp_attributes_free(attr);
            continue;
        }
        e.size = attr->size;
        e.isDir = (attr->type == SSH_FILEXFER_TYPE_DIRECTORY);
        e.isLink = (attr->type == SSH_FILEXFER_TYPE_SYMLINK);
        e.longName = e.isDir ? QStringLiteral("dir") : (e.isLink ? QStringLiteral("link") : QStringLiteral("file"));
        entries.push_back(e);
        sftp_attributes_free(attr);
    }
    const int sftpErr = sftp_get_error(sftp);
    if (sftpErr != SSH_FX_OK && sftpErr != SSH_FX_EOF) {
        vlog(QStringLiteral("listDirectory: readdir ended with err=%1 (%2)").arg(sftpErr).arg(sftpErrorName(sftpErr)));
    }
    sftp_closedir(dir);

    std::sort(entries.begin(), entries.end(), [](const SftpEntry& a, const SftpEntry& b) {
        if (a.isDir != b.isDir) {
            return a.isDir && !b.isDir;
        }
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });

    m_cwd = target;
    vlog(QStringLiteral("listDirectory: ok target='%1' entries=%2").arg(target).arg(entries.size()));
    emit directoryListed(target, entries);
    emit statusChanged(QStringLiteral("%1 · %2 items").arg(target).arg(entries.size()));
}

void SftpClient::downloadFile(const QString& remotePath, const QString& localPath)
{
    m_cancelRequested.storeRelaxed(false);
    vlog(QStringLiteral("downloadFile: remote='%1' local='%2' cwd='%3'").arg(remotePath, localPath, m_cwd));
    QString err;
    if (!ensureConnected(&err)) {
        vlog(QStringLiteral("downloadFile: not connected"));
        emit transferFinished(false, err);
        return;
    }

    auto* sftp = static_cast<sftp_session>(m_sftp);
    const QString remote = resolvePath(remotePath);
    vlog(QStringLiteral("downloadFile: resolved remote='%1'").arg(remote));
    emit statusChanged(QStringLiteral("downloading %1").arg(remote));

    sftp_file file = sftp_open(sftp, remote.toUtf8().constData(), sftpReadFlags, 0);
    if (!file) {
        const QString detail = sftpErrorString();
        vlog(QStringLiteral("downloadFile: sftp_open failed remote='%1' err=%2").arg(remote, detail));
        emit transferFinished(false, QStringLiteral("open remote '%1' failed: %2").arg(remote, detail));
        return;
    }
    vlog(QStringLiteral("downloadFile: sftp_open ok remote='%1'").arg(remote));

    QFile out(localPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString detail = fileErrorDetail(out, localPath);
        vlog(QStringLiteral("downloadFile: cannot open local for write: %1").arg(detail));
        sftp_close(file);
        emit transferFinished(false, QStringLiteral("cannot write local file '%1': %2").arg(localPath, out.errorString()));
        return;
    }

    qint64 total = 0;
    sftp_attributes attrs = sftp_fstat(file);
    if (attrs) {
        total = static_cast<qint64>(attrs->size);
        vlog(QStringLiteral("downloadFile: remote size=%1").arg(total));
        sftp_attributes_free(attrs);
    } else {
        vlog(QStringLiteral("downloadFile: sftp_fstat failed err=%1").arg(sftpErrorString()));
    }

    char buffer[32768];
    qint64 done = 0;
    while (true) {
        if (isCancelled()) {
            sftp_close(file);
            out.close();
            QFile::remove(localPath);
            vlog(QStringLiteral("downloadFile: cancelled remote='%1' after %2/%3 bytes").arg(remote).arg(done).arg(total));
            emit transferFinished(false, QStringLiteral("cancelled %1 (%2 bytes)").arg(remote).arg(done));
            emit statusChanged(QStringLiteral("download cancelled"));
            return;
        }
        const ssize_t n = sftp_read(file, buffer, sizeof(buffer));
        if (n < 0) {
            const QString detail = sftpErrorString();
            vlog(QStringLiteral("downloadFile: sftp_read failed after %1 bytes err=%2").arg(done).arg(detail));
            sftp_close(file);
            out.close();
            emit transferFinished(false,
                                  QStringLiteral("read failed for '%1' at %2/%3 bytes: %4")
                                      .arg(remote)
                                      .arg(done)
                                      .arg(total)
                                      .arg(detail));
            return;
        }
        if (n == 0) {
            break;
        }
        if (out.write(buffer, n) != n) {
            const QString detail = fileErrorDetail(out, localPath);
            vlog(QStringLiteral("downloadFile: local write failed: %1").arg(detail));
            sftp_close(file);
            out.close();
            emit transferFinished(false,
                                  QStringLiteral("local write failed for '%1': %2").arg(localPath, out.errorString()));
            return;
        }
        done += n;
        emit transferProgress(remote, done, total);
    }

    const int closeRc = sftp_close(file);
    if (closeRc != SSH_OK) {
        vlog(QStringLiteral("downloadFile: sftp_close returned %1 err=%2").arg(closeRc).arg(sftpErrorString()));
    }
    out.close();
    vlog(QStringLiteral("downloadFile: done remote='%1' local='%2' bytes=%3").arg(remote, localPath).arg(done));
    emit transferFinished(true, QStringLiteral("downloaded %1 (%2 bytes)").arg(QFileInfo(localPath).fileName()).arg(done));
    emit statusChanged(QStringLiteral("download complete"));
}

void SftpClient::uploadFile(const QString& localPath, const QString& remotePath)
{
    m_cancelRequested.storeRelaxed(false);
    vlog(QStringLiteral("uploadFile: local='%1' remote='%2' cwd='%3'").arg(localPath, remotePath, m_cwd));
    QString err;
    if (!ensureConnected(&err)) {
        vlog(QStringLiteral("uploadFile: not connected"));
        emit transferFinished(false, err);
        return;
    }

    QFile in(localPath);
    if (!in.open(QIODevice::ReadOnly)) {
        const QString detail = fileErrorDetail(in, localPath);
        vlog(QStringLiteral("uploadFile: cannot open local for read: %1").arg(detail));
        emit transferFinished(false, QStringLiteral("cannot read local file '%1': %2").arg(localPath, in.errorString()));
        return;
    }
    const qint64 localSize = in.size();
    vlog(QStringLiteral("uploadFile: local size=%1").arg(localSize));

    auto* sftp = static_cast<sftp_session>(m_sftp);
    const QString remote = resolvePath(remotePath);
    vlog(QStringLiteral("uploadFile: resolved remote='%1'").arg(remote));
    emit statusChanged(QStringLiteral("uploading %1").arg(remote));

    sftp_file file = sftp_open(sftp, remote.toUtf8().constData(), sftpWriteFlags, 0644);
    if (!file) {
        const int code = sftp_get_error(sftp);
        const QString detail0 = sftpErrorString();
        vlog(QStringLiteral("uploadFile: sftp_open failed remote='%1' err=%2").arg(remote, detail0));
        if (code == SSH_FX_NO_SUCH_FILE || code == SSH_FX_NO_SUCH_PATH || code == SSH_FX_FAILURE) {
            vlog(QStringLiteral("uploadFile: attempting to create parent dir for '%1' and retry").arg(remote));
            if (ensureParentDirExists(remote, sftp)) {
                file = sftp_open(sftp, remote.toUtf8().constData(), sftpWriteFlags, 0644);
                if (file) {
                    vlog(QStringLiteral("uploadFile: retry sftp_open succeeded after mkdir -p"));
                }
            }
        }
        if (!file) {
            const QString detail = sftpErrorString();
            vlog(QStringLiteral("uploadFile: sftp_open still failed remote='%1' err=%2").arg(remote, detail));
            emit transferFinished(false, QStringLiteral("open remote '%1' failed: %2").arg(remote, detail));
            return;
        }
    }
    vlog(QStringLiteral("uploadFile: sftp_open ok remote='%1'").arg(remote));

    const qint64 total = localSize;
    qint64 done = 0;
    char buffer[32768];
    while (true) {
        if (isCancelled()) {
            sftp_close(file);
            in.close();
            vlog(QStringLiteral("uploadFile: cancelled remote='%1' after %2/%3 bytes").arg(remote).arg(done).arg(total));
            emit transferFinished(false, QStringLiteral("cancelled %1 (%2 bytes)").arg(remote).arg(done));
            emit statusChanged(QStringLiteral("upload cancelled"));
            return;
        }
        const qint64 n = in.read(buffer, sizeof(buffer));
        if (n < 0) {
            vlog(QStringLiteral("uploadFile: local read failed after %1 bytes err=%2").arg(done).arg(in.errorString()));
            sftp_close(file);
            emit transferFinished(false,
                                  QStringLiteral("local read failed for '%1': %2").arg(localPath, in.errorString()));
            return;
        }
        if (n == 0) {
            break;
        }
        qint64 off = 0;
        while (off < n) {
            if (isCancelled()) {
                sftp_close(file);
                in.close();
                vlog(QStringLiteral("uploadFile: cancelled remote='%1' during write").arg(remote));
                emit transferFinished(false, QStringLiteral("cancelled %1 (%2 bytes)").arg(remote).arg(done + off));
                emit statusChanged(QStringLiteral("upload cancelled"));
                return;
            }
            const ssize_t w = sftp_write(file, buffer + off, static_cast<size_t>(n - off));
            if (w < 0) {
                const QString detail = sftpErrorString();
                vlog(QStringLiteral("uploadFile: sftp_write failed at %1/%2 err=%3").arg(done + off).arg(total).arg(detail));
                sftp_close(file);
                emit transferFinished(false,
                                      QStringLiteral("write failed for '%1' at %2/%3 bytes: %4")
                                          .arg(remote)
                                          .arg(done + off)
                                          .arg(total)
                                          .arg(detail));
                return;
            }
            off += w;
        }
        done += n;
        emit transferProgress(remote, done, total);
    }

    const int closeRc = sftp_close(file);
    if (closeRc != SSH_OK) {
        const QString detail = sftpErrorString();
        vlog(QStringLiteral("uploadFile: sftp_close returned %1 err=%2").arg(closeRc).arg(detail));
        emit transferFinished(false, QStringLiteral("close failed for '%1': %2").arg(remote, detail));
        return;
    }
    in.close();
    vlog(QStringLiteral("uploadFile: done local='%1' remote='%2' bytes=%3").arg(localPath, remote).arg(done));
    emit transferFinished(true, QStringLiteral("uploaded %1 (%2 bytes)").arg(QFileInfo(localPath).fileName()).arg(done));
    emit statusChanged(QStringLiteral("upload complete"));
}

void SftpClient::uploadPath(const QString& localPath, const QString& remoteDir)
{
    m_cancelRequested.storeRelaxed(false);
    vlog(QStringLiteral("uploadPath: local='%1' remoteDir='%2' cwd='%3'").arg(localPath, remoteDir, m_cwd));
    QString err;
    if (!ensureConnected(&err)) {
        vlog(QStringLiteral("uploadPath: not connected"));
        emit transferFinished(false, err);
        return;
    }
    auto* sftp = static_cast<sftp_session>(m_sftp);
    const QFileInfo localInfo(localPath);
    if (!localInfo.exists()) {
        const QString msg = QStringLiteral("local path does not exist: '%1'").arg(localPath);
        vlog(msg);
        emit transferFinished(false, msg);
        return;
    }
    const QString dir = resolvePath(remoteDir);
    vlog(QStringLiteral("uploadPath: resolved dir='%1' localIsDir=%2").arg(dir).arg(localInfo.isDir()));
    // Proactively ensure the target directory exists before recursing.
    // If m_cwd is stale (deleted on server) this recovers automatically.
    if (!mkdirP(dir, sftp)) {
        const QString detail = sftpErrorString();
        vlog(QStringLiteral("uploadPath: mkdirP target dir failed dir='%1' err=%2").arg(dir, detail));
        emit transferFinished(false,
                              QStringLiteral("cannot create remote folder '%1': %2 — check permissions or whether '%1' was deleted")
                                  .arg(dir, detail));
        return;
    }
    if (!uploadPathRec(localPath, dir, sftp)) {
        vlog(QStringLiteral("uploadPath: failed"));
        return;
    }
    vlog(QStringLiteral("uploadPath: done local='%1' dir='%2'").arg(localPath, dir));
    emit transferFinished(true, QStringLiteral("uploaded %1").arg(localInfo.fileName()));
    emit statusChanged(QStringLiteral("upload complete"));
}

bool SftpClient::uploadPathRec(const QString& localPath, const QString& remoteDir, void* sftpVoid)
{
    auto* sftp = static_cast<sftp_session>(sftpVoid);
    const QFileInfo localInfo(localPath);
    if (localInfo.isDir()) {
        const QString remote = resolvePath(remoteDir + QLatin1Char('/') + localInfo.fileName());
        vlog(QStringLiteral("uploadPathRec: mkdir '%1' for local dir '%2'").arg(remote, localPath));
        emit statusChanged(QStringLiteral("creating %1").arg(remote));
        if (sftp_mkdir(sftp, remote.toUtf8().constData(), 0755) != SSH_OK
            && sftp_get_error(sftp) != SSH_FX_FILE_ALREADY_EXISTS) {
            const QString detail = sftpErrorString();
            vlog(QStringLiteral("uploadPathRec: mkdir failed remote='%1' err=%2").arg(remote, detail));
            emit transferFinished(false, QStringLiteral("mkdir failed for '%1': %2").arg(remote, detail));
            return false;
        }
        const QDir dir(localPath);
        const QStringList entries = dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
        vlog(QStringLiteral("uploadPathRec: dir '%1' has %2 entries -> remote '%3'").arg(localPath).arg(entries.size()).arg(remote));
        for (const QString& entry : entries) {
            if (isCancelled()) {
                vlog(QStringLiteral("uploadPathRec: dir upload cancelled after '%1'").arg(remote));
                emit transferFinished(false, QStringLiteral("cancelled %1").arg(remote));
                emit statusChanged(QStringLiteral("upload cancelled"));
                return false;
            }
            if (!uploadPathRec(dir.filePath(entry), remote, sftp)) {
                return false;
            }
        }
        return true;
    }

    // File: stream it to <remoteDir>/<fileName>.
    QFile in(localPath);
    if (!in.open(QIODevice::ReadOnly)) {
        const QString detail = fileErrorDetail(in, localPath);
        vlog(QStringLiteral("uploadPathRec: cannot open local file: %1").arg(detail));
        emit transferFinished(false, QStringLiteral("cannot read local file '%1': %2").arg(localPath, in.errorString()));
        return false;
    }
    const qint64 localSize = in.size();
    const QString remote = resolvePath(remoteDir + QLatin1Char('/') + localInfo.fileName());
    vlog(QStringLiteral("uploadPathRec: uploading file local='%1' (%2 bytes) -> remote='%3' (remoteDir='%4')")
             .arg(localPath)
             .arg(localSize)
             .arg(remote, remoteDir));
    emit statusChanged(QStringLiteral("uploading %1").arg(remote));
    // Proactively ensure the parent exists so the initial sftp_open failure
    // can be reported as permission/creation error rather than generic NO_SUCH_FILE.
    if (!ensureParentDirExists(remote, sftp)) {
        const QString detail = sftpErrorString();
        const QString parent = remote.left(remote.lastIndexOf(QLatin1Char('/')));
        if (parent.isEmpty() || parent == remote) {
            vlog(QStringLiteral("uploadPathRec: ensureParentDirExists failed for '%1': %2").arg(remote, detail));
            emit transferFinished(false,
                                  QStringLiteral("cannot create remote folder for '%1': %2 — check permissions")
                                      .arg(remote, detail));
            return false;
        }
        // Diagnostic: is the parent actually there? Report it clearly.
        sftp_attributes pst = sftp_stat(sftp, parent.toUtf8().constData());
        QString diag;
        if (pst) {
            const bool isDir = (pst->type == SSH_FILEXFER_TYPE_DIRECTORY);
            sftp_attributes_free(pst);
            diag = isDir ? QStringLiteral("parent '%1' exists (dir)").arg(parent)
                         : QStringLiteral("parent '%1' exists but is not a directory").arg(parent);
            vlog(QStringLiteral("uploadPathRec: ensureParentDirExists failed but parent stat says: %1 err=%2")
                     .arg(diag, detail));
        } else {
            const int pe = sftp_get_error(sftp);
            diag = QStringLiteral("parent '%1' missing (%2 — %3)").arg(parent, sftpErrorName(pe), sftpErrorString());
            vlog(QStringLiteral("uploadPathRec: ensureParentDirExists failed, parent diagnostic: %1").arg(diag));
        }
        emit transferFinished(false,
                              QStringLiteral("cannot create remote folder for '%1': %2 — %3 — check that '%4' exists and you have write permission")
                                  .arg(remote, detail, diag, parent));
        return false;
    }
    sftp_file file = sftp_open(sftp, remote.toUtf8().constData(), sftpWriteFlags, 0644);
    if (!file) {
        const int code = sftp_get_error(sftp);
        const QString detail0 = sftpErrorString();
        vlog(QStringLiteral("uploadPathRec: sftp_open failed remote='%1' err=%2").arg(remote, detail0));
        if (code == SSH_FX_NO_SUCH_FILE || code == SSH_FX_NO_SUCH_PATH || code == SSH_FX_FAILURE) {
            vlog(QStringLiteral("uploadPathRec: ensuring parents of '%1' then retry").arg(remote));
            if (ensureParentDirExists(remote, sftp)) {
                file = sftp_open(sftp, remote.toUtf8().constData(), sftpWriteFlags, 0644);
                if (file) {
                    vlog(QStringLiteral("uploadPathRec: retry sftp_open ok"));
                }
            }
        }
        if (!file) {
            const QString detail = sftpErrorString();
            vlog(QStringLiteral("uploadPathRec: sftp_open still failed remote='%1' err=%2").arg(remote, detail));
            if (detail.contains(QLatin1String("No such file"), Qt::CaseInsensitive)
                || detail.contains(QLatin1String("NO_SUCH_FILE"))) {
                emit transferFinished(
                    false,
                    QStringLiteral(
                        "open remote '%1' failed: %2 — parent folder doesn't exist or you lack permission to create it in that path")
                        .arg(remote, detail));
            } else {
                emit transferFinished(false, QStringLiteral("open remote '%1' failed: %2").arg(remote, detail));
            }
            return false;
        }
    }

    const qint64 total = localSize;
    qint64 done = 0;
    char buffer[32768];
    while (true) {
        if (isCancelled()) {
            sftp_close(file);
            in.close();
            vlog(QStringLiteral("uploadPathRec: cancelled remote='%1' after %2/%3 bytes").arg(remote).arg(done).arg(total));
            emit transferFinished(false, QStringLiteral("cancelled %1 (%2 bytes)").arg(remote).arg(done));
            emit statusChanged(QStringLiteral("upload cancelled"));
            return false;
        }
        const qint64 n = in.read(buffer, sizeof(buffer));
        if (n < 0) {
            vlog(QStringLiteral("uploadPathRec: local read failed after %1 bytes err=%2").arg(done).arg(in.errorString()));
            sftp_close(file);
            emit transferFinished(false, QStringLiteral("local read failed for '%1': %2").arg(localPath, in.errorString()));
            return false;
        }
        if (n == 0) {
            break;
        }
        qint64 off = 0;
        while (off < n) {
            if (isCancelled()) {
                sftp_close(file);
                in.close();
                vlog(QStringLiteral("uploadPathRec: cancelled remote='%1' during write").arg(remote));
                emit transferFinished(false, QStringLiteral("cancelled %1 (%2 bytes)").arg(remote).arg(done + off));
                emit statusChanged(QStringLiteral("upload cancelled"));
                return false;
            }
            const ssize_t w = sftp_write(file, buffer + off, static_cast<size_t>(n - off));
            if (w < 0) {
                const QString detail = sftpErrorString();
                vlog(QStringLiteral("uploadPathRec: sftp_write failed at %1/%2 err=%3").arg(done + off).arg(total).arg(detail));
                sftp_close(file);
                emit transferFinished(false,
                                      QStringLiteral("write failed for '%1' at %2/%3 bytes: %4")
                                          .arg(remote)
                                          .arg(done + off)
                                          .arg(total)
                                          .arg(detail));
                return false;
            }
            off += w;
        }
        done += n;
        emit transferProgress(remote, done, total);
    }

    const int closeRc = sftp_close(file);
    if (closeRc != SSH_OK) {
        const QString detail = sftpErrorString();
        vlog(QStringLiteral("uploadPathRec: sftp_close failed remote='%1' err=%2").arg(remote, detail));
        emit transferFinished(false, QStringLiteral("close failed for '%1': %2").arg(remote, detail));
        return false;
    }
    in.close();
    vlog(QStringLiteral("uploadPathRec: done remote='%1' bytes=%2").arg(remote).arg(done));
    return true;
}

void SftpClient::makeDirectory(const QString& path)
{
    QString err;
    if (!ensureConnected(&err)) {
        emit operationFinished(false, err);
        return;
    }
    auto* sftp = static_cast<sftp_session>(m_sftp);
    const QString target = resolvePath(path);
    vlog(QStringLiteral("makeDirectory: target='%1' (raw='%2')").arg(target, path));
    if (sftp_mkdir(sftp, target.toUtf8().constData(), 0755) != SSH_OK) {
        const QString detail = sftpErrorString();
        vlog(QStringLiteral("makeDirectory: failed target='%1' err=%2").arg(target, detail));
        emit operationFinished(false, QStringLiteral("mkdir failed for '%1': %2").arg(target, detail));
        return;
    }
    vlog(QStringLiteral("makeDirectory: ok target='%1'").arg(target));
    emit operationFinished(true, QStringLiteral("created %1").arg(target));
}

void SftpClient::removePath(const QString& path, bool isDir)
{
    QString err;
    if (!ensureConnected(&err)) {
        emit operationFinished(false, err);
        return;
    }
    auto* sftp = static_cast<sftp_session>(m_sftp);
    const QString target = resolvePath(path);
    vlog(QStringLiteral("removePath: target='%1' isDir=%2").arg(target).arg(isDir));
    const int rc = isDir ? sftp_rmdir(sftp, target.toUtf8().constData())
                         : sftp_unlink(sftp, target.toUtf8().constData());
    if (rc != SSH_OK) {
        const QString detail = sftpErrorString();
        vlog(QStringLiteral("removePath: failed target='%1' err=%2").arg(target, detail));
        emit operationFinished(false, QStringLiteral("remove failed for '%1': %2").arg(target, detail));
        return;
    }
    vlog(QStringLiteral("removePath: ok target='%1'").arg(target));
    emit operationFinished(true, QStringLiteral("removed %1").arg(target));
}

void SftpClient::renamePath(const QString& from, const QString& to)
{
    QString err;
    if (!ensureConnected(&err)) {
        emit operationFinished(false, err);
        return;
    }
    auto* sftp = static_cast<sftp_session>(m_sftp);
    const QString src = resolvePath(from);
    const QString dst = resolvePath(to);
    vlog(QStringLiteral("renamePath: from='%1' to='%2'").arg(src, dst));
    if (sftp_rename(sftp, src.toUtf8().constData(), dst.toUtf8().constData()) != SSH_OK) {
        const QString detail = sftpErrorString();
        vlog(QStringLiteral("renamePath: failed from='%1' to='%2' err=%3").arg(src, dst, detail));
        emit operationFinished(false, QStringLiteral("rename failed for '%1': %2").arg(src, detail));
        return;
    }
    vlog(QStringLiteral("renamePath: ok from='%1' to='%2'").arg(src, dst));
    emit operationFinished(true, QStringLiteral("renamed %1 → %2").arg(src, dst));
}
