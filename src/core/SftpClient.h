#pragma once

#include "SessionProfile.h"
#include "NetworkProxyManager.h"

#include <QAtomicInteger>
#include <QObject>
#include <QString>
#include <QVector>
#include <QMetaType>

struct SftpEntry {
    QString name;
    quint64 size = 0;
    bool isDir = false;
    bool isLink = false;
    QString longName; // permissions-ish display
};

Q_DECLARE_METATYPE(SftpEntry)
Q_DECLARE_METATYPE(QVector<SftpEntry>)

class SftpClient : public QObject
{
    Q_OBJECT

public:
    explicit SftpClient(QObject* parent = nullptr);
    ~SftpClient() override;

public slots:
    void connectHost(const SessionProfile& profile);
    void disconnectHost();
    void listDirectory(const QString& path);
    void downloadFile(const QString& remotePath, const QString& localPath);
    void uploadFile(const QString& localPath, const QString& remotePath);
    void uploadPath(const QString& localPath, const QString& remotePath);
    void makeDirectory(const QString& path);
    void removePath(const QString& path, bool isDir);
    void renamePath(const QString& from, const QString& to);
    void setVerboseEnabled(bool enabled);
    /** Thread-safe — may be called from the UI thread during an active transfer. */
    void requestCancelTransfer();
    void cancelTransfer();

signals:
    void connected(const QString& homePath);
    void disconnected();
    void directoryListed(const QString& path, const QVector<SftpEntry>& entries);
    void transferProgress(const QString& label, qint64 done, qint64 total);
    void transferFinished(bool ok, const QString& message);
    void operationFinished(bool ok, const QString& message);
    void errorOccurred(const QString& message);
    void statusChanged(const QString& status);
    void debugLog(const QString& message);

private:
    bool ensureConnected(QString* errorOut);
    bool authenticate(const SessionProfile& profile, QString* errorOut);
    void cleanup();
    QString resolvePath(const QString& path) const;
    bool uploadPathRec(const QString& localPath, const QString& remoteDir, void* sftp);
    bool mkdirP(const QString& remoteDir, void* sftp);
    bool ensureParentDirExists(const QString& remoteFilePath, void* sftp);
    void vlog(const QString& msg);
    QString sftpErrorString() const;
    bool isCancelled() const { return m_cancelRequested.loadRelaxed(); }

    void* m_session = nullptr; // ssh_session
    void* m_sftp = nullptr;    // sftp_session
    SshProxyTunnel m_proxyTunnel;
    QString m_cwd;
    bool m_connected = false;
    bool m_verbose = false;
    QAtomicInteger<bool> m_cancelRequested = false;
};
