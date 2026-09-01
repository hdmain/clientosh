#pragma once

#include "SessionProfile.h"
#include "NetworkProxyManager.h"

#include <QObject>
#include <QtGlobal>

class QTimer;

struct ServerStats {
    bool valid = false;
    double cpuPercent = -1.0; // -1 = unknown / first sample
    qint64 memUsedBytes = -1;
    qint64 memTotalBytes = -1;
    qint64 diskUsedBytes = -1;
    qint64 diskTotalBytes = -1;
};

Q_DECLARE_METATYPE(ServerStats)

/**
 * Opens a dedicated SSH connection and polls remote CPU / RAM / disk.
 * Must live on a worker thread (same pattern as SftpClient).
 */
class ServerStatsClient : public QObject
{
    Q_OBJECT

public:
    explicit ServerStatsClient(QObject* parent = nullptr);
    ~ServerStatsClient() override;

public slots:
    void start(const SessionProfile& profile);
    void stop();
    void poll();

signals:
    void statsUpdated(const ServerStats& stats);
    void failed(const QString& message);

private:
    bool authenticate(const SessionProfile& profile, QString* errorOut);
    void cleanup();
    bool ensureConnected(QString* errorOut);
    QByteArray execCommand(const char* command, QString* errorOut);

    void* m_session = nullptr; // ssh_session
    bool m_connected = false;
    QTimer* m_timer = nullptr;
    SshProxyTunnel m_proxyTunnel;

    bool m_havePrevCpu = false;
    quint64 m_prevIdle = 0;
    quint64 m_prevTotal = 0;
};
