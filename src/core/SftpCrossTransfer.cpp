#include "SftpCrossTransfer.h"
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

namespace {

constexpr const char* kKex = "curve25519-sha256,curve25519-sha256@libssh.org,"
                             "ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,"
                             "diffie-hellman-group-exchange-sha256,"
                             "diffie-hellman-group16-sha512,diffie-hellman-group18-sha512,"
                             "diffie-hellman-group14-sha256";

QString normalizeRemote(QString p)
{
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (p.contains(QStringLiteral("//"))) {
        p.replace(QStringLiteral("//"), QStringLiteral("/"));
    }
    if (p.isEmpty()) {
        return QStringLiteral("/");
    }
    return p;
}

QString fxName(int code)
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
    case SSH_FX_NO_SUCH_PATH:
        return QStringLiteral("NO_SUCH_PATH");
    case SSH_FX_FILE_ALREADY_EXISTS:
        return QStringLiteral("FILE_ALREADY_EXISTS");
    default:
        return QStringLiteral("code=%1").arg(code);
    }
}

}

SftpCrossTransfer::SftpCrossTransfer(QObject* parent)
    : QObject(parent)
    , m_verbose(AppSettings::sftpVerboseLogging())
{
    qRegisterMetaType<SessionProfile>("SessionProfile");
    qRegisterMetaType<SftpCrossEntry>("SftpCrossEntry");
    qRegisterMetaType<QVector<SftpCrossEntry>>("QVector<SftpCrossEntry>");
}

void SftpCrossTransfer::vlog(const QString& msg)
{
    if (!m_verbose) {
        return;
    }
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    emit debugLog(QStringLiteral("[xfer %1] %2").arg(ts, msg));
}

QString SftpCrossTransfer::sftpError(void* sessVoid, void* sftpVoid) const
{
    if (!sessVoid || !sftpVoid) {
        return QStringLiteral("no session");
    }
    auto* sess = static_cast<ssh_session>(sessVoid);
    auto* sftp = static_cast<sftp_session>(sftpVoid);
    const int code = sftp_get_error(sftp);
    const QString sshErr = QString::fromUtf8(ssh_get_error(sess)).trimmed();
    const QString fx = fxName(code);
    if (!sshErr.isEmpty()) {
        return QStringLiteral("%1 (%2) — %3").arg(fx).arg(code).arg(sshErr);
    }
    return QStringLiteral("%1 (%2)").arg(fx).arg(code);
}

bool SftpCrossTransfer::connectSftp(const SessionProfile& profile, void** sessionOut,
                                    void** sftpOut, SshProxyTunnel* tunnelOut, QString* errOut)
{
    ssh_session session = ssh_new();
    if (!session) {
        if (errOut) {
            *errOut = QStringLiteral("failed to create ssh session");
        }
        return false;
    }
    const QByteArray host = profile.host.toUtf8();
    const QByteArray user = profile.user.toUtf8();
    int port = profile.port;
    ssh_options_set(session, SSH_OPTIONS_HOST, host.constData());
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session, SSH_OPTIONS_USER, user.constData());
    long timeoutSec = 20;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeoutSec);
    ssh_options_set(session, SSH_OPTIONS_KEY_EXCHANGE, kKex);
    int strict = 0;
    ssh_options_set(session, SSH_OPTIONS_STRICTHOSTKEYCHECK, &strict);
    int logLevel = m_verbose ? SSH_LOG_PROTOCOL : SSH_LOG_NOLOG;
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &logLevel);

    SshProxyTunnel tunnel;
    QString tunnelError;
    if (!NetworkProxy::openSshTunnel(profile.host, port, &tunnel, &tunnelError)) {
        if (errOut) {
            *errOut = tunnelError;
        }
        ssh_free(session);
        return false;
    }
    if (tunnel.socket) {
        NetworkProxy::applyTunnelToSshSession(session, tunnel);
    }

    if (ssh_connect(session) != SSH_OK) {
        const QString e = NetworkProxy::sshConnectErrorMessage(session);
        if (errOut) {
            *errOut = QStringLiteral("ssh_connect to %1:%2 failed: %3").arg(profile.host).arg(port).arg(e);
        }
        ssh_free(session);
        return false;
    }

    const QString keyId = profile.privateKeyId.trimmed();
    const QString keyPath = profile.privateKeyPath.trimmed();
    bool authenticated = false;

    if (profile.authMethod == AuthMethod::SshAgent) {
        authenticated = ssh_userauth_agent(session, nullptr) == SSH_AUTH_SUCCESS;
        if (!authenticated && errOut) {
            *errOut = QStringLiteral("SSH agent authentication failed for %1: %2")
                          .arg(profile.host, QString::fromUtf8(ssh_get_error(session)));
        }
    } else if (profile.authMethod == AuthMethod::StoredKey
               || profile.authMethod == AuthMethod::KeyFile) {
        QString keyLoadErr;
        ssh_key privkey = nullptr;
        if (loadProfilePrivateKey(profile, &privkey, &keyLoadErr) && privkey) {
            const int rc = ssh_userauth_publickey(session, nullptr, privkey);
            ssh_key_free(privkey);
            if (rc == SSH_AUTH_SUCCESS) {
                authenticated = true;
            } else if (errOut) {
                *errOut = QStringLiteral("key auth failed for %1: %2")
                              .arg(profile.host, QString::fromUtf8(ssh_get_error(session)));
            }
        } else if (errOut) {
            *errOut = keyLoadErr;
        }
    } else if (profile.authMethod == AuthMethod::Password && !profile.password.isEmpty()) {
        QString authErr;
        authenticated = sshUserauthPasswordFlexible(session, profile.password, profile.user, &authErr);
        if (!authenticated && errOut) {
            *errOut = QStringLiteral("auth failed for %1@%2: %3")
                          .arg(profile.user, profile.host, authErr);
        }
    } else if (errOut) {
        *errOut = QStringLiteral("no credentials for %1").arg(profile.host);
    }

    if (!authenticated) {
        if (errOut) {
            if (errOut->isEmpty()) {
                *errOut = QStringLiteral("authentication failed for %1").arg(profile.host);
            }
        }
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }

    sftp_session sftp = sftp_new(session);
    if (!sftp || sftp_init(sftp) != SSH_OK) {
        const QString e = sftp ? sftpError(session, sftp) : QString::fromUtf8(ssh_get_error(session));
        if (sftp) {
            sftp_free(sftp);
        }
        if (errOut) {
            *errOut = QStringLiteral("sftp init failed on %1: %2").arg(profile.host, e);
        }
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }
    *sessionOut = session;
    *sftpOut = sftp;
    if (tunnelOut) {
        *tunnelOut = std::move(tunnel);
    }
    return true;
}

void SftpCrossTransfer::cleanupSftp(void* session, void* sftp, SshProxyTunnel* tunnel)
{
    if (sftp) {
        sftp_free(static_cast<sftp_session>(sftp));
    }
    if (session) {
        auto* s = static_cast<ssh_session>(session);
        ssh_disconnect(s);
        ssh_free(s);
    }
    if (tunnel) {
        *tunnel = {};
    }
}

bool SftpCrossTransfer::mkdirP(void* sftpVoid, const QString& remoteDir)
{
    auto* sftp = static_cast<sftp_session>(sftpVoid);
    const QString norm = normalizeRemote(remoteDir);
    if (norm == QStringLiteral("/")) {
        return true;
    }
    sftp_attributes st = sftp_stat(sftp, norm.toUtf8().constData());
    if (st) {
        const bool isDir = (st->type == SSH_FILEXFER_TYPE_DIRECTORY);
        sftp_attributes_free(st);
        if (isDir) {
            return true;
        }
        return false;
    }
    const QStringList parts = norm.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString cur;
    for (const QString& part : parts) {
        cur += QLatin1Char('/') + part;
        sftp_attributes pre = sftp_stat(sftp, cur.toUtf8().constData());
        if (pre) {
            const bool isDir = (pre->type == SSH_FILEXFER_TYPE_DIRECTORY);
            sftp_attributes_free(pre);
            if (isDir) {
                continue;
            }
            return false;
        }
        if (sftp_mkdir(sftp, cur.toUtf8().constData(), 0755) == SSH_OK) {
            continue;
        }
        const int err = sftp_get_error(sftp);
        if (err == SSH_FX_FILE_ALREADY_EXISTS || err == SSH_FX_OK) {
            continue;
        }
        sftp_attributes pst = sftp_stat(sftp, cur.toUtf8().constData());
        if (pst) {
            const bool isDir = (pst->type == SSH_FILEXFER_TYPE_DIRECTORY);
            sftp_attributes_free(pst);
            if (isDir) {
                continue;
            }
            return false;
        }
        return false;
    }
    return true;
}

bool SftpCrossTransfer::ensureParentDir(void* sftp, const QString& remoteFilePath)
{
    const QString norm = normalizeRemote(remoteFilePath);
    const int slash = norm.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0) {
        return true;
    }
    return mkdirP(sftp, norm.left(slash));
}

bool SftpCrossTransfer::downloadFile(void* sftpVoid, const QString& remote, const QString& local)
{
    auto* sftp = static_cast<sftp_session>(sftpVoid);
    const QString r = normalizeRemote(remote);
    sftp_file file = sftp_open(sftp, r.toUtf8().constData(), O_RDONLY, 0);
    if (!file) {
        return false;
    }
    QFile out(local);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        sftp_close(file);
        return false;
    }
    qint64 total = 0;
    if (sftp_attributes a = sftp_fstat(file)) {
        total = static_cast<qint64>(a->size);
        sftp_attributes_free(a);
    }
    char buf[32768];
    qint64 done = 0;
    while (true) {
        if (isCancelled()) {
            sftp_close(file);
            out.close();
            QFile::remove(local);
            return false;
        }
        const ssize_t n = sftp_read(file, buf, sizeof(buf));
        if (n < 0) {
            sftp_close(file);
            out.close();
            return false;
        }
        if (n == 0) {
            break;
        }
        if (out.write(buf, n) != n) {
            sftp_close(file);
            out.close();
            return false;
        }
        done += n;
        emit progress(QFileInfo(local).fileName(), done, total);
    }
    sftp_close(file);
    out.close();
    return true;
}

bool SftpCrossTransfer::downloadDir(void* sftpVoid, const QString& remoteDir, const QString& localDir)
{
    auto* sftp = static_cast<sftp_session>(sftpVoid);
    QDir().mkpath(localDir);
    const QString r = normalizeRemote(remoteDir);
    sftp_dir dir = sftp_opendir(sftp, r.toUtf8().constData());
    if (!dir) {
        return false;
    }
    bool ok = true;
    while (sftp_attributes attr = sftp_readdir(sftp, dir)) {
        const QString name = QString::fromUtf8(attr->name ? attr->name : "");
        const bool isDir = (attr->type == SSH_FILEXFER_TYPE_DIRECTORY);
        const quint64 sz = attr->size;
        sftp_attributes_free(attr);
        if (name == QStringLiteral(".") || name == QStringLiteral("..")) {
            continue;
        }
        if (isCancelled()) {
            ok = false;
            break;
        }
        const QString remoteChild = r + QLatin1Char('/') + name;
        const QString localChild = localDir + QLatin1Char('/') + name;
        if (isDir) {
            if (!downloadDir(sftpVoid, remoteChild, localChild)) {
                ok = false;
                break;
            }
        } else {
            emit fileStarted(name);
            if (!downloadFile(sftpVoid, remoteChild, localChild)) {
                ok = false;
                break;
            }
            Q_UNUSED(sz)
        }
    }
    sftp_closedir(dir);
    return ok && !isCancelled();
}

bool SftpCrossTransfer::uploadFile(void* sftpVoid, const QString& local, const QString& remote)
{
    auto* sftp = static_cast<sftp_session>(sftpVoid);
    QFile in(local);
    if (!in.open(QIODevice::ReadOnly)) {
        return false;
    }
    const qint64 total = in.size();
    const QString r = normalizeRemote(remote);
    if (!ensureParentDir(sftpVoid, r)) {
        return false;
    }
    sftp_file file = sftp_open(sftp, r.toUtf8().constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!file) {
        return false;
    }
    qint64 done = 0;
    char buf[32768];
    while (true) {
        if (isCancelled()) {
            sftp_close(file);
            in.close();
            return false;
        }
        const qint64 n = in.read(buf, sizeof(buf));
        if (n < 0) {
            sftp_close(file);
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
                return false;
            }
            const ssize_t w = sftp_write(file, buf + off, static_cast<size_t>(n - off));
            if (w < 0) {
                sftp_close(file);
                return false;
            }
            off += w;
        }
        done += n;
        emit progress(QFileInfo(local).fileName(), done, total);
    }
    const int rc = sftp_close(file);
    in.close();
    return rc == SSH_OK;
}

bool SftpCrossTransfer::uploadDirContents(void* destSftp, const QString& localDir, const QString& remoteDir)
{
    const QString r = normalizeRemote(remoteDir);
    if (!mkdirP(destSftp, r)) {
        return false;
    }
    QDir dir(localDir);
    const QStringList entries = dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
    for (const QString& name : entries) {
        if (isCancelled()) {
            return false;
        }
        const QString localChild = dir.filePath(name);
        const QFileInfo fi(localChild);
        const QString remoteChild = r + QLatin1Char('/') + name;
        if (fi.isDir()) {
            if (!uploadDirContents(destSftp, localChild, remoteChild)) {
                return false;
            }
        } else {
            emit fileStarted(name);
            if (!uploadFile(destSftp, localChild, remoteChild)) {
                return false;
            }
        }
    }
    return true;
}

void SftpCrossTransfer::requestCancel()
{
    m_cancelled.storeRelaxed(true);
}

void SftpCrossTransfer::cancel()
{
    requestCancel();
}

void SftpCrossTransfer::startTransfer(const SessionProfile& sourceProfile,
                                      const QVector<SftpCrossEntry>& entries,
                                      const SessionProfile& destProfile,
                                      const QString& destDir,
                                      const QString& stagingRoot)
{
    m_cancelled.storeRelaxed(false);
    if (entries.isEmpty()) {
        emit finished(false, QStringLiteral("nothing to transfer"));
        return;
    }
    const QString destNorm = normalizeRemote(destDir);
    vlog(QStringLiteral("xfer: %1 entries from %2 -> %3:%4 staging=%5")
             .arg(entries.size())
             .arg(sourceProfile.host, destProfile.host, destNorm, stagingRoot));

    void* srcSession = nullptr;
    void* srcSftp = nullptr;
    SshProxyTunnel srcTunnel;
    QString err;
    if (!connectSftp(sourceProfile, &srcSession, &srcSftp, &srcTunnel, &err)) {
        emit finished(false, QStringLiteral("source connect failed: %1").arg(err));
        return;
    }
    if (isCancelled()) {
        cleanupSftp(srcSession, srcSftp, &srcTunnel);
        emit finished(false, QStringLiteral("cancelled"));
        return;
    }

    void* dstSession = nullptr;
    void* dstSftp = nullptr;
    SshProxyTunnel dstTunnel;
    if (!connectSftp(destProfile, &dstSession, &dstSftp, &dstTunnel, &err)) {
        cleanupSftp(srcSession, srcSftp, &srcTunnel);
        emit finished(false, QStringLiteral("destination connect failed: %1").arg(err));
        return;
    }

    if (!mkdirP(dstSftp, destNorm)) {
        cleanupSftp(srcSession, srcSftp, &srcTunnel);
        cleanupSftp(dstSession, dstSftp, &dstTunnel);
        emit finished(false, QStringLiteral("cannot create destination folder '%1'").arg(destNorm));
        return;
    }

    QDir().mkpath(stagingRoot);
    bool allOk = true;
    QString failMsg;

    for (const SftpCrossEntry& e : entries) {
        if (isCancelled()) {
            allOk = false;
            failMsg = QStringLiteral("cancelled");
            break;
        }
        emit fileStarted(e.name);
        vlog(QStringLiteral("xfer: file '%1' isDir=%2").arg(e.remotePath).arg(e.isDir));

        if (e.isDir) {
            const QString localDir = stagingRoot + QLatin1Char('/') + e.name;
            const QString remoteDest = destNorm + QLatin1Char('/') + e.name;
            if (!downloadDir(srcSftp, e.remotePath, localDir)) {
                allOk = false;
                failMsg = QStringLiteral("download failed for folder '%1'").arg(e.name);
                vlog(failMsg + QStringLiteral(" err=%1").arg(sftpError(srcSession, srcSftp)));
                break;
            }
            if (isCancelled()) {
                allOk = false;
                failMsg = QStringLiteral("cancelled");
                break;
            }
            if (!uploadDirContents(dstSftp, localDir, remoteDest)) {
                allOk = false;
                failMsg = QStringLiteral("upload failed for folder '%1'").arg(e.name);
                vlog(failMsg + QStringLiteral(" err=%1").arg(sftpError(dstSession, dstSftp)));
                break;
            }
        } else {
            const QString localFile = stagingRoot + QLatin1Char('/') + e.name;
            if (!downloadFile(srcSftp, e.remotePath, localFile)) {
                allOk = false;
                failMsg = QStringLiteral("download failed for '%1': %2").arg(e.name, sftpError(srcSession, srcSftp));
                vlog(failMsg);
                break;
            }
            if (isCancelled()) {
                allOk = false;
                failMsg = QStringLiteral("cancelled");
                break;
            }
            const QString remoteDest = destNorm + QLatin1Char('/') + e.name;
            if (!uploadFile(dstSftp, localFile, remoteDest)) {
                allOk = false;
                failMsg = QStringLiteral("upload failed for '%1': %2").arg(e.name, sftpError(dstSession, dstSftp));
                vlog(failMsg);
                break;
            }
        }
    }

    cleanupSftp(srcSession, srcSftp, &srcTunnel);
    cleanupSftp(dstSession, dstSftp, &dstTunnel);

    // Best-effort cleanup of staging (remove temp files). Keep on failure for debugging? No — always clean.
    {
        QDir d(stagingRoot);
        if (d.exists()) {
            d.removeRecursively();
        }
    }

    if (isCancelled()) {
        emit finished(false, QStringLiteral("cancelled"));
        return;
    }
    if (!allOk) {
        emit finished(false, failMsg);
        return;
    }
    const QString msg = entries.size() == 1
        ? QStringLiteral("transferred %1").arg(entries.first().name)
        : QStringLiteral("transferred %1 items").arg(entries.size());
    emit finished(true, msg);
}
