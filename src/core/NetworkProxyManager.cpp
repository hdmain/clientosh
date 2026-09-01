#include "NetworkProxyManager.h"

#include "AppSettings.h"
#include "KeyringAdapter.h"

#include <libssh/libssh.h>

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QTcpSocket>

#ifdef Q_OS_WIN
#  include <winsock2.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace {

const char* kProxyPasswordKey = "proxy.password";

QString proxyPasswordKeyName()
{
    return KeyringAdapter::servicePrefix() + QStringLiteral(".") + QLatin1String(kProxyPasswordKey);
}

void setSocketBlocking(QTcpSocket& socket)
{
    const qintptr handle = socket.socketDescriptor();
    if (handle == -1) {
        return;
    }
#ifdef Q_OS_WIN
    u_long mode = 0;
    ioctlsocket(static_cast<SOCKET>(handle), FIONBIO, &mode);
#else
    const int fd = static_cast<int>(handle);
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
#endif
}

bool waitForData(QTcpSocket& socket, int timeoutMs, QString* errorOut)
{
    if (socket.bytesAvailable() > 0) {
        return true;
    }
    if (!socket.waitForReadyRead(timeoutMs)) {
        if (errorOut) {
            const QString err = socket.errorString();
            *errorOut = err.isEmpty() ? QStringLiteral("timed out waiting for proxy response")
                                      : err;
        }
        return false;
    }
    return true;
}

bool readExact(QTcpSocket& socket, int nbytes, QByteArray* out, int timeoutMs, QString* errorOut)
{
    out->clear();
    while (out->size() < nbytes) {
        if (!waitForData(socket, timeoutMs, errorOut)) {
            return false;
        }
        out->append(socket.read(nbytes - out->size()));
    }
    return true;
}

bool writeAll(QTcpSocket& socket, const QByteArray& data, int timeoutMs, QString* errorOut)
{
    if (socket.write(data) != data.size() || !socket.waitForBytesWritten(timeoutMs)) {
        if (errorOut) {
            const QString err = socket.errorString();
            *errorOut = err.isEmpty() ? QStringLiteral("failed to write to proxy")
                                      : err;
        }
        return false;
    }
    return true;
}

bool connectToProxyServer(QTcpSocket& socket, QString* errorOut)
{
    const QString proxyHost = AppSettings::proxyHost().trimmed();
    const int proxyPort = AppSettings::proxyPort();
    socket.setProxy(QNetworkProxy::NoProxy);
    socket.connectToHost(proxyHost, quint16(proxyPort));
    if (!socket.waitForConnected(15000)) {
        if (errorOut) {
            *errorOut = QStringLiteral("could not reach proxy %1:%2 — %3")
                            .arg(proxyHost)
                            .arg(proxyPort)
                            .arg(socket.errorString());
        }
        return false;
    }
    return true;
}

bool socks5Tunnel(QTcpSocket& socket, const QString& host, int port, QString* errorOut)
{
    const bool auth = AppSettings::proxyAuthEnabled();
    const QString user = AppSettings::proxyUsername();
    const QString pass = NetworkProxy::loadPassword();

    QByteArray greeting;
    greeting.append(char(0x05));
    if (auth && !user.isEmpty()) {
        greeting.append(char(0x02));
        greeting.append(char(0x00)); // no auth
        greeting.append(char(0x02)); // username/password
    } else {
        greeting.append(char(0x01));
        greeting.append(char(0x00)); // no auth
    }
    if (!writeAll(socket, greeting, 10000, errorOut)) {
        return false;
    }

    QByteArray methodReply;
    if (!readExact(socket, 2, &methodReply, 10000, errorOut)) {
        return false;
    }
    if (methodReply.at(0) != char(0x05)) {
        if (errorOut) {
            *errorOut = QStringLiteral("SOCKS5 proxy returned an invalid greeting");
        }
        return false;
    }
    const char method = methodReply.at(1);
    if (method == char(0xFF)) {
        if (errorOut) {
            *errorOut = QStringLiteral("SOCKS5 proxy rejected all authentication methods");
        }
        return false;
    }
    if (method == char(0x02)) {
        QByteArray authReq;
        authReq.append(char(0x01));
        const QByteArray userBytes = user.toUtf8();
        const QByteArray passBytes = pass.toUtf8();
        authReq.append(char(userBytes.size()));
        authReq.append(userBytes);
        authReq.append(char(passBytes.size()));
        authReq.append(passBytes);
        if (!writeAll(socket, authReq, 10000, errorOut)) {
            return false;
        }
        QByteArray authReply;
        if (!readExact(socket, 2, &authReply, 10000, errorOut)) {
            return false;
        }
        if (authReply.size() < 2 || authReply.at(1) != char(0x00)) {
            if (errorOut) {
                *errorOut = QStringLiteral("SOCKS5 proxy authentication failed");
            }
            return false;
        }
    }

    const QByteArray hostBytes = host.toUtf8();
    QByteArray request;
    request.append(char(0x05)); // VER
    request.append(char(0x01)); // CONNECT
    request.append(char(0x00)); // RSV
    request.append(char(0x03)); // ATYP = domain
    request.append(char(hostBytes.size()));
    request.append(hostBytes);
    request.append(char((port >> 8) & 0xff));
    request.append(char(port & 0xff));
    if (!writeAll(socket, request, 10000, errorOut)) {
        return false;
    }

    QByteArray head;
    if (!readExact(socket, 4, &head, 10000, errorOut)) {
        return false;
    }
    if (head.at(1) != char(0x00)) {
        if (errorOut) {
            *errorOut = QStringLiteral("SOCKS5 proxy refused the tunnel (code %1)")
                            .arg(static_cast<quint8>(head.at(1)));
        }
        return false;
    }
    const char atyp = head.at(3);
    int tailBytes = 0;
    if (atyp == char(0x01)) {
        tailBytes = 4 + 2;
    } else if (atyp == char(0x03)) {
        QByteArray lenByte;
        if (!readExact(socket, 1, &lenByte, 10000, errorOut)) {
            return false;
        }
        tailBytes = static_cast<quint8>(lenByte.at(0)) + 2;
    } else if (atyp == char(0x04)) {
        tailBytes = 16 + 2;
    }
    if (tailBytes > 0) {
        QByteArray tail;
        if (!readExact(socket, tailBytes, &tail, 10000, errorOut)) {
            return false;
        }
    }
    return true;
}

bool socks4Tunnel(QTcpSocket& socket, const QString& host, int port, QString* errorOut)
{
    QHostAddress addr;
    const bool useHostname = !addr.setAddress(host);

    QByteArray request;
    request.append(char(0x04)); // VN
    request.append(char(0x01)); // CONNECT
    request.append(char((port >> 8) & 0xff));
    request.append(char(port & 0xff));
    if (useHostname) {
        request.append(char(0x00));
        request.append(char(0x00));
        request.append(char(0x00));
        request.append(char(0x01)); // SOCKS4a
    } else {
        const quint32 ip = addr.toIPv4Address();
        request.append(char((ip >> 24) & 0xff));
        request.append(char((ip >> 16) & 0xff));
        request.append(char((ip >> 8) & 0xff));
        request.append(char(ip & 0xff));
    }
    const QByteArray user = AppSettings::proxyUsername().toUtf8();
    request.append(user);
    request.append('\0');
    if (useHostname) {
        request.append(host.toUtf8());
        request.append('\0');
    }
    if (!writeAll(socket, request, 10000, errorOut)) {
        return false;
    }

    QByteArray reply;
    if (!readExact(socket, 8, &reply, 10000, errorOut)) {
        return false;
    }
    if (reply.at(1) != char(0x5a)) {
        if (errorOut) {
            *errorOut = QStringLiteral("SOCKS4 proxy refused the tunnel (code %1)")
                            .arg(static_cast<quint8>(reply.at(1)));
        }
        return false;
    }
    return true;
}

bool httpConnectTunnel(QTcpSocket& socket, const QString& host, int port, QString* errorOut)
{
    QString request = QStringLiteral("CONNECT %1:%2 HTTP/1.1\r\n"
                                     "Host: %1:%2\r\n")
                          .arg(host)
                          .arg(port);
    if (AppSettings::proxyAuthEnabled()) {
        const QString user = AppSettings::proxyUsername();
        const QString pass = NetworkProxy::loadPassword();
        const QByteArray creds = (user + QLatin1Char(':') + pass).toUtf8().toBase64();
        request += QStringLiteral("Proxy-Authorization: Basic %1\r\n").arg(QString::fromLatin1(creds));
    }
    request += QStringLiteral("\r\n");
    if (!writeAll(socket, request.toUtf8(), 10000, errorOut)) {
        return false;
    }

    if (!waitForData(socket, 15000, errorOut)) {
        return false;
    }
    const QByteArray response = socket.readAll();
    const int headerEnd = response.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("HTTP proxy returned an incomplete CONNECT response");
        }
        return false;
    }
    const QByteArray statusLine = response.left(response.indexOf("\r\n"));
    if (!statusLine.contains(" 200 ")) {
        if (errorOut) {
            *errorOut = QStringLiteral("HTTP proxy CONNECT failed: %1")
                            .arg(QString::fromUtf8(statusLine));
        }
        return false;
    }
    return true;
}

bool openManualTunnel(const QString& host, int port, SshProxyTunnel* tunnelOut, QString* errorOut)
{
    auto socket = std::make_unique<QTcpSocket>();
    if (!connectToProxyServer(*socket, errorOut)) {
        return false;
    }

    bool ok = false;
    switch (AppSettings::proxyProtocol()) {
    case AppSettings::ProxyProtocol::Http:
        ok = httpConnectTunnel(*socket, host, port, errorOut);
        break;
    case AppSettings::ProxyProtocol::Socks4:
        ok = socks4Tunnel(*socket, host, port, errorOut);
        break;
    case AppSettings::ProxyProtocol::Socks5:
        ok = socks5Tunnel(*socket, host, port, errorOut);
        break;
    }
    if (!ok) {
        return false;
    }

    setSocketBlocking(*socket);
    if (tunnelOut) {
        tunnelOut->socket = std::move(socket);
    }
    return true;
}

} // namespace

namespace NetworkProxy {

void storePassword(const QString& password)
{
    KeyringAdapter::store(proxyPasswordKeyName(), password.toUtf8());
}

QString loadPassword()
{
    QByteArray data;
    if (KeyringAdapter::retrieve(proxyPasswordKeyName(), data)) {
        const QString password = QString::fromUtf8(data);
        data.fill('\0');
        return password;
    }
    return QString();
}

void clearPassword()
{
    KeyringAdapter::remove(proxyPasswordKeyName());
}

QNetworkProxy networkProxyFromSettings()
{
    if (!AppSettings::proxyEnabled()) {
        return QNetworkProxy(QNetworkProxy::NoProxy);
    }
    const QString host = AppSettings::proxyHost().trimmed();
    if (host.isEmpty()) {
        return QNetworkProxy(QNetworkProxy::NoProxy);
    }

    QNetworkProxy::ProxyType type = QNetworkProxy::HttpProxy;
    switch (AppSettings::proxyProtocol()) {
    case AppSettings::ProxyProtocol::Http:
        type = QNetworkProxy::HttpProxy;
        break;
    case AppSettings::ProxyProtocol::Socks4:
    case AppSettings::ProxyProtocol::Socks5:
        type = QNetworkProxy::Socks5Proxy;
        break;
    }
    QNetworkProxy proxy(type, host, AppSettings::proxyPort());
    if (AppSettings::proxyAuthEnabled()) {
        proxy.setUser(AppSettings::proxyUsername());
        proxy.setPassword(loadPassword());
    }
    return proxy;
}

void applyApplicationProxy()
{
    QNetworkProxy::setApplicationProxy(networkProxyFromSettings());
}

bool openSshTunnel(const QString& host, int port, SshProxyTunnel* tunnelOut, QString* errorOut)
{
    if (!AppSettings::proxyEnabled() || AppSettings::proxyHost().trimmed().isEmpty()) {
        return true;
    }
    return openManualTunnel(host, port, tunnelOut, errorOut);
}

void applyTunnelToSshSession(ssh_session session, const SshProxyTunnel& tunnel)
{
    if (!session || !tunnel.socket) {
        return;
    }
    const socket_t fd = static_cast<socket_t>(tunnel.socket->socketDescriptor());
    if (fd == SSH_INVALID_SOCKET) {
        return;
    }
    ssh_options_set(session, SSH_OPTIONS_FD, &fd);
    ssh_set_blocking(session, 1);
}

void configureDirectAccess(QNetworkAccessManager* nam)
{
    if (!nam) {
        return;
    }
    nam->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
}

QString sshConnectErrorMessage(ssh_session session)
{
    if (!session) {
        return QStringLiteral("unknown SSH error");
    }
    QString err = QString::fromUtf8(ssh_get_error(session)).trimmed();
    if (err.isEmpty()) {
        err = QStringLiteral("SSH handshake failed (code %1)").arg(ssh_get_error_code(session));
    }
    return err;
}

} // namespace NetworkProxy
