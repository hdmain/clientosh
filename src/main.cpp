#include "MainWindow.h"
#include "PlatformFonts.h"
#include "core/AppSettings.h"
#include "core/CliLaunch.h"
#include "core/FontManager.h"
#include "core/NetworkProxyManager.h"
#include "ui/Motion.h"

#include <libssh/libssh.h>

#include <QApplication>
#include <QCryptographicHash>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTimer>

#ifdef Q_OS_WIN
#include <winsock2.h>
#endif

namespace {

QString commandServerName()
{
    const QByteArray home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation).toUtf8();
    const QByteArray userKey = QCryptographicHash::hash(home, QCryptographicHash::Sha256).toHex().left(12);
    return QStringLiteral("clientosh-%1").arg(QString::fromLatin1(userKey));
}

bool forwardCommand(const QJsonObject& request)
{
    if (request.isEmpty()) {
        return false;
    }

    QLocalSocket socket;
    socket.connectToServer(commandServerName(), QIODevice::WriteOnly);
    if (!socket.waitForConnected(500)) {
        return false;
    }

    QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    payload.append('\n');
    if (socket.write(payload) != payload.size() || !socket.waitForBytesWritten(1000)) {
        return false;
    }
    socket.disconnectFromServer();
    return true;
}

QJsonObject commandFromCliRequest(const CliLaunch::Request& req)
{
    QJsonObject request;
    const SessionProfile& profile = req.profile;

    request.insert(QStringLiteral("protocol"), connectionModeToString(profile.connectionMode));
    request.insert(QStringLiteral("host"), profile.host);
    request.insert(QStringLiteral("port"), profile.port);
    if (profile.isSerial()) {
        request.insert(QStringLiteral("baud"), profile.serialBaudRate);
    }
    if (!profile.name.isEmpty()) {
        request.insert(QStringLiteral("hasName"), true);
        request.insert(QStringLiteral("name"), profile.name);
    }
    if (!profile.user.isEmpty()) {
        request.insert(QStringLiteral("user"), profile.user);
    }
    if (!profile.password.isEmpty()) {
        request.insert(QStringLiteral("password"), profile.password);
    }
    if (!profile.privateKeyPath.isEmpty()) {
        request.insert(QStringLiteral("identity"), profile.privateKeyPath);
    }
    if (!profile.privateKeyId.isEmpty()) {
        request.insert(QStringLiteral("keyring"), profile.privateKeyId);
    }
    if (!profile.keyPassphrase.isEmpty()) {
        request.insert(QStringLiteral("keyPassphrase"), profile.keyPassphrase);
    }
    if (profile.authMethod == AuthMethod::SshAgent) {
        request.insert(QStringLiteral("authMethod"), authMethodToString(profile.authMethod));
    }
    if (req.openSftpWithSsh) {
        request.insert(QStringLiteral("openSftp"), true);
    }
    return request;
}

SessionProfile profileFromCommand(const QJsonObject& request)
{
    SessionProfile profile;
    const QString host = request.value(QStringLiteral("host")).toString().trimmed();
    const int port = request.value(QStringLiteral("port")).toInt();
    const ConnectionMode mode = connectionModeFromString(
        request.value(QStringLiteral("protocol")).toString());
    const bool serial = mode == ConnectionMode::Serial;
    if (host.isEmpty() || (!serial && (port < 1 || port > 65535))) {
        return profile;
    }

    const QVector<SessionProfile> savedProfiles = loadProfiles();
    for (const SessionProfile& saved : savedProfiles) {
        if (saved.host.compare(host, Qt::CaseInsensitive) == 0
            && (serial || saved.port == port) && saved.connectionMode == mode) {
            profile = saved;
            break;
        }
    }

    profile.host = host;
    profile.port = port;
    profile.connectionMode = mode;
    if (serial) {
        profile.serialBaudRate = request.value(QStringLiteral("baud")).toInt(115200);
        profile.system = QStringLiteral("Serial");
    } else if (request.contains(QStringLiteral("user"))) {
        profile.user = request.value(QStringLiteral("user")).toString().trimmed();
    } else if (profile.user.trimmed().isEmpty()) {
        profile.user = AppSettings::defaultUser();
    }

    if (request.value(QStringLiteral("hasName")).toBool()) {
        profile.name = request.value(QStringLiteral("name")).toString().trimmed();
    }
    if (request.contains(QStringLiteral("password"))) {
        profile.password = request.value(QStringLiteral("password")).toString();
        profile.authMethod = AuthMethod::Password;
    }
    if (request.contains(QStringLiteral("identity"))) {
        profile.privateKeyPath = request.value(QStringLiteral("identity")).toString().trimmed();
        profile.authMethod = AuthMethod::KeyFile;
    }
    if (request.contains(QStringLiteral("keyring"))) {
        profile.privateKeyId = request.value(QStringLiteral("keyring")).toString().trimmed();
        profile.authMethod = AuthMethod::StoredKey;
    }
    if (request.contains(QStringLiteral("authMethod"))) {
        profile.authMethod = authMethodFromString(
            request.value(QStringLiteral("authMethod")).toString(),
            profile.privateKeyId, profile.privateKeyPath);
    }
    if (request.contains(QStringLiteral("keyPassphrase"))) {
        profile.keyPassphrase = request.value(QStringLiteral("keyPassphrase")).toString();
    }
    profile.normalizeAuthentication();
    return profile;
}

bool openSftpFromCommand(const QJsonObject& request)
{
    return request.value(QStringLiteral("openSftp")).toBool();
}

void focusMainWindow(MainWindow& window)
{
    if (window.isMinimized()) {
        window.showNormal();
    } else {
        window.show();
    }
    window.raise();
    window.activateWindow();
}

} // namespace

int main(int argc, char* argv[])
{
    // File-backed settings (INI) — must run before any QSettings() usage.
    AppSettings::configureFileStorage();

#ifdef Q_OS_WIN
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    CliLaunch::Request cliRequest;
    bool cliConnect = false;
    bool verbose = false;

    if (argc > 1) {
        const int headlessCode =
            CliLaunch::runHeadlessPhase(argc, argv, &cliRequest, &cliConnect, &verbose);
        if (headlessCode >= 0) {
#ifdef Q_OS_WIN
            WSACleanup();
#endif
            return headlessCode;
        }
    }

    const QJsonObject commandLineRequest = cliConnect ? commandFromCliRequest(cliRequest) : QJsonObject();

    if (forwardCommand(commandLineRequest)) {
#ifdef Q_OS_WIN
        WSACleanup();
#endif
        return 0;
    }

    ssh_init();

    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("clientosh"));
    app.setOrganizationName(QStringLiteral("clientosh"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/terminal.svg")));

    NetworkProxy::applyApplicationProxy();

    if (verbose) {
        AppSettings::setSftpVerboseLogging(true);
    }

    QLocalServer commandServer;
    commandServer.setSocketOptions(QLocalServer::UserAccessOption);
    bool ownsCommandServer = commandServer.listen(commandServerName());
    if (!ownsCommandServer && !commandLineRequest.isEmpty()
        && forwardCommand(commandLineRequest)) {
        ssh_finalize();
#ifdef Q_OS_WIN
        WSACleanup();
#endif
        return 0;
    }
    if (!ownsCommandServer) {
        QLocalSocket probe;
        probe.connectToServer(commandServerName());
        if (!probe.waitForConnected(250)) {
            QLocalServer::removeServer(commandServerName());
            ownsCommandServer = commandServer.listen(commandServerName());
        }
    }

    FontManager::instance()->loadCachedFonts();
    app.setFont(clientoshUiFont(AppSettings::uiFontSize(), AppSettings::uiFontFamily()));

    Motion::loadFromSettings();

    MainWindow window;
    window.show();

    if (ownsCommandServer) {
        QObject::connect(&commandServer, &QLocalServer::newConnection, &window,
                         [&commandServer, &window]() {
            while (QLocalSocket* socket = commandServer.nextPendingConnection()) {
                const auto processRequests = [socket, &window]() {
                    while (socket->canReadLine()) {
                        const QJsonDocument doc = QJsonDocument::fromJson(socket->readLine().trimmed());
                        if (!doc.isObject()) {
                            continue;
                        }
                        const QJsonObject request = doc.object();
                        const SessionProfile profile = profileFromCommand(request);
                        if (profile.host.isEmpty()) {
                            continue;
                        }
                        focusMainWindow(window);
                        window.launchFromCli(profile, openSftpFromCommand(request));
                    }
                };
                QObject::connect(socket, &QLocalSocket::readyRead, socket, processRequests);
                QObject::connect(socket, &QLocalSocket::disconnected,
                                 socket, &QObject::deleteLater);
                processRequests();
            }
        });
    }

    if (cliConnect) {
        const SessionProfile profile = cliRequest.profile;
        const bool withSftp = cliRequest.openSftpWithSsh;
        QTimer::singleShot(0, &window, [&window, profile, withSftp]() {
            window.launchFromCli(profile, withSftp);
        });
    }

    const int code = app.exec();

    ssh_finalize();

#ifdef Q_OS_WIN
    WSACleanup();
#endif
    return code;
}
