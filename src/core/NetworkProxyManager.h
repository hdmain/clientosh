#pragma once

#include <QNetworkProxy>
#include <QString>
#include <QTcpSocket>

#include <memory>

struct ssh_session_struct;
typedef struct ssh_session_struct* ssh_session;

struct SshProxyTunnel {
    std::unique_ptr<QTcpSocket> socket;
};

class QNetworkAccessManager;

/** Reads proxy preferences from AppSettings/keyring and applies them app-wide. */
namespace NetworkProxy {

void applyApplicationProxy();
QNetworkProxy networkProxyFromSettings();

/** Open a TCP connection to host:port through the configured proxy (or direct when off). */
bool openSshTunnel(const QString& host, int port, SshProxyTunnel* tunnelOut, QString* errorOut);

/** Attach a tunnel socket to a session before ssh_connect(). */
void applyTunnelToSshSession(ssh_session session, const SshProxyTunnel& tunnel);

/** Human-readable libssh connect error (never empty). */
QString sshConnectErrorMessage(ssh_session session);

/** Force direct connection — used by GitHub Gist sync (bypass rule). */
void configureDirectAccess(QNetworkAccessManager* nam);

void storePassword(const QString& password);
QString loadPassword();
void clearPassword();

} // namespace NetworkProxy
