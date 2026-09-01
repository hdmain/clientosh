#pragma once

#include "SessionProfile.h"
#include "NetworkProxyManager.h"

#include <QAtomicInteger>
#include <QObject>
#include <QString>
#include <QVector>

struct SftpCrossEntry {
    QString remotePath;
    QString name;
    bool isDir = false;
};

Q_DECLARE_METATYPE(SftpCrossEntry)
Q_DECLARE_METATYPE(QVector<SftpCrossEntry>)

class SftpCrossTransfer : public QObject
{
    Q_OBJECT

public:
    explicit SftpCrossTransfer(QObject* parent = nullptr);

public slots:
    void startTransfer(const SessionProfile& sourceProfile,
                       const QVector<SftpCrossEntry>& entries,
                       const SessionProfile& destProfile,
                       const QString& destDir,
                       const QString& stagingRoot);
    /** Thread-safe — may be called from the UI thread during an active transfer. */
    void requestCancel();
    void cancel();

signals:
    void progress(const QString& label, qint64 done, qint64 total);
    void fileStarted(const QString& fileName);
    void finished(bool ok, const QString& message);
    void debugLog(const QString& line);
    void verboseProgress(const QString& label);

private:
    bool connectSftp(const SessionProfile& profile, void** sessionOut, void** sftpOut,
                     SshProxyTunnel* tunnelOut, QString* errOut);
    void cleanupSftp(void* session, void* sftp, SshProxyTunnel* tunnel);
    bool downloadFile(void* sftp, const QString& remote, const QString& local);
    bool downloadDir(void* sftp, const QString& remoteDir, const QString& localDir);
    bool uploadFile(void* sftp, const QString& local, const QString& remote);
    bool uploadDirContents(void* destSftp, const QString& localDir, const QString& remoteDir);
    bool mkdirP(void* sftp, const QString& remoteDir);
    bool ensureParentDir(void* sftp, const QString& remoteFilePath);
    QString sftpError(void* session, void* sftp) const;
    void vlog(const QString& msg);
    bool isCancelled() const { return m_cancelled.loadRelaxed(); }

    QAtomicInteger<bool> m_cancelled = false;
    bool m_verbose = false;
};
