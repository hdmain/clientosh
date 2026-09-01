#include "CliLaunch.h"

#include "AppSettings.h"
#include "platform/ConsoleAttach.h"

#include "version.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QSettings>
#include <QUuid>

#include <cstdio>
#include <cstring>

namespace {

struct ParsedTarget {
    QString user;
    QString host;
    int port = -1;
    QString error;
};

ParsedTarget parseTarget(const QString& raw)
{
    ParsedTarget out;
    QString t = raw.trimmed();
    if (t.isEmpty()) {
        out.error = QStringLiteral("target host is empty");
        return out;
    }

    QString hostPort = t;
    const int at = t.lastIndexOf(QLatin1Char('@'));
    if (at >= 0) {
        out.user = t.left(at).trimmed();
        hostPort = t.mid(at + 1).trimmed();
        if (hostPort.isEmpty()) {
            out.error = QStringLiteral("missing host after '@'");
            return out;
        }
    }

    // Bracketed IPv6: [::1]:port
    if (hostPort.startsWith(QLatin1Char('['))) {
        const int close = hostPort.indexOf(QLatin1Char(']'));
        if (close < 0) {
            out.error = QStringLiteral("invalid IPv6 address (missing ']')");
            return out;
        }
        out.host = hostPort.mid(1, close - 1);
        if (close + 1 < hostPort.size()) {
            if (hostPort.at(close + 1) != QLatin1Char(':')) {
                out.error = QStringLiteral("invalid target after IPv6 address");
                return out;
            }
            bool ok = false;
            const int p = hostPort.mid(close + 2).toInt(&ok);
            if (!ok || p < 1 || p > 65535) {
                out.error = QStringLiteral("invalid port");
                return out;
            }
            out.port = p;
        }
        return out;
    }

    const int colon = hostPort.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        bool ok = false;
        const int p = hostPort.mid(colon + 1).toInt(&ok);
        if (ok && p >= 1 && p <= 65535) {
            out.host = hostPort.left(colon);
            out.port = p;
            return out;
        }
    }

    out.host = hostPort;
    return out;
}

ConnectionMode modeFromCommand(const QString& cmd, QString* errorOut)
{
    const QString c = cmd.trimmed().toLower();
    if (c == QLatin1String("ssh") || c == QLatin1String("connect")) {
        return ConnectionMode::Ssh;
    }
    if (c == QLatin1String("telnet")) {
        return ConnectionMode::Telnet;
    }
    if (c == QLatin1String("sftp")) {
        return ConnectionMode::SftpOnly;
    }
    if (c == QLatin1String("serial") || c == QLatin1String("com")) {
        return ConnectionMode::Serial;
    }
    if (c == QLatin1String("rdp") || c == QLatin1String("desktop")) {
        return ConnectionMode::Rdp;
    }
    if (errorOut) {
        *errorOut = QStringLiteral("unknown command %1 (expected ssh, telnet, sftp, serial, or rdp)").arg(cmd);
    }
    return ConnectionMode::Ssh;
}

int defaultPortFor(ConnectionMode mode)
{
    switch (mode) {
    case ConnectionMode::Telnet:
        return 23;
    case ConnectionMode::Serial:
        return 0;
    case ConnectionMode::Rdp:
        return 3389;
    case ConnectionMode::SftpOnly:
    case ConnectionMode::Ssh:
    default:
        return AppSettings::defaultPort();
    }
}

bool argvHasHelp(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0 || std::strcmp(a, "-?") == 0
            || std::strcmp(a, "--help-all") == 0) {
            return true;
        }
    }
    return false;
}

bool argvHasVersion(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--version") == 0 || std::strcmp(a, "-v") == 0) {
            return true;
        }
    }
    return false;
}

bool argvHasRemovedNameOption(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-name") == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

namespace CliLaunch {

void addStandardOptions(QCommandLineParser& parser)
{
    // Do not use parser.addHelpOption() / addVersionOption() — on Windows GUI builds Qt
    // shows a message box instead of printing to the terminal. We handle both manually.
    parser.addOption(QCommandLineOption(
        QStringLiteral("verbose"),
        QStringLiteral("Enable verbose SFTP debug logging (also: Settings > SFTP).")));
}

void configureParser(QCommandLineParser& parser)
{
    parser.setApplicationDescription(
        QStringLiteral(
            "clientosh - SSH, SFTP, Telnet, serial, and RDP client\n"
            "\n"
            "Quick connect:\n"
            "  clientosh telnet 192.168.0.1:23 --name Router\n"
            "  clientosh ssh user@prod.example.com --name Production\n"
            "  clientosh ssh 10.0.0.5:2222 -u admin --name Jump\n"
            "  clientosh sftp deploy@files.example.com --name Deploy\n"
            "  clientosh rdp admin@win-server.example.com --name Windows\n"
            "  clientosh ssh host -i ~/.ssh/id_ed25519 -u root --sftp\n"
            "  clientosh serial COM3 --baud 115200 --name Console\n"
            "  clientosh serial /dev/ttyUSB0 --baud 9600 --name Console\n"
            "\n"
            "Target: host, host:port, user@host, user@host:port, or a serial device\n"
            "Run without a command to open the dashboard."));

    parser.addPositionalArgument(
        QStringLiteral("command"),
        QStringLiteral("Connection type: ssh, telnet, sftp, serial, or rdp."));
    parser.addPositionalArgument(
        QStringLiteral("target"),
        QStringLiteral("Remote host or serial device (see examples above)."));

    parser.addOption(QCommandLineOption(
        {QStringLiteral("n"), QStringLiteral("name")},
        QStringLiteral("Tab / pane title."),
        QStringLiteral("title")));
    parser.addOption(QCommandLineOption(
        {QStringLiteral("u"), QStringLiteral("user")},
        QStringLiteral("Username (overrides user@ in target)."),
        QStringLiteral("user")));
    parser.addOption(QCommandLineOption(
        {QStringLiteral("P"), QStringLiteral("port")},
        QStringLiteral("Port (overrides :port in target)."),
        QStringLiteral("port")));
    parser.addOption(QCommandLineOption(
        {QStringLiteral("p"), QStringLiteral("password")},
        QStringLiteral("Password (session only, not saved)."),
        QStringLiteral("password")));
    parser.addOption(QCommandLineOption(
        {QStringLiteral("i"), QStringLiteral("identity")},
        QStringLiteral("Private key file path."),
        QStringLiteral("path")));
    parser.addOption(QCommandLineOption(
        {QStringLiteral("stored-key"), QStringLiteral("keyring")},
        QStringLiteral("Stored SSH key id (--keyring is kept for compatibility)."),
        QStringLiteral("id")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("agent"),
        QStringLiteral("Authenticate using SSH agent.")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("key-passphrase"),
        QStringLiteral("Passphrase for an encrypted private key."),
        QStringLiteral("passphrase")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("sftp"),
        QStringLiteral("With ssh, also open an SFTP pane for the same host.")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("baud"),
        QStringLiteral("Serial baud rate (default: 115200)."),
        QStringLiteral("rate"),
        QStringLiteral("115200")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("domain"),
        QStringLiteral("Windows domain for RDP sessions."),
        QStringLiteral("domain")));

    parser.addOption(QCommandLineOption(
        {QStringLiteral("h"), QStringLiteral("help"), QStringLiteral("?")},
        QStringLiteral("Show help and exit.")));
    parser.addOption(QCommandLineOption(
        {QStringLiteral("v"), QStringLiteral("version")},
        QStringLiteral("Show version and exit.")));
}

Request parse(const QCommandLineParser& parser)
{
    Request req;
    const QStringList pos = parser.positionalArguments();
    if (pos.isEmpty()) {
        return req;
    }

    req.launch = true;

    if (pos.size() < 2) {
        req.error = QStringLiteral("missing target (example: clientosh telnet 192.168.0.1:23 --name Router)");
        return req;
    }

    const ConnectionMode mode = modeFromCommand(pos.at(0), &req.error);
    if (!req.error.isEmpty()) {
        return req;
    }

    SessionProfile profile;
    profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    profile.connectionMode = mode;

    if (mode == ConnectionMode::Serial) {
        profile.host = pos.at(1).trimmed();
        if (profile.host.isEmpty()) {
            req.error = QStringLiteral("serial device is required");
            return req;
        }
        if (parser.isSet(QStringLiteral("port"))) {
            req.error = QStringLiteral("--port is not valid with serial; use --baud");
            return req;
        }
        bool baudOk = false;
        const int baud = parser.value(QStringLiteral("baud")).toInt(&baudOk);
        if (!baudOk || baud <= 0) {
            req.error = QStringLiteral("invalid --baud value");
            return req;
        }
        profile.serialBaudRate = baud;
        profile.system = QStringLiteral("Serial");
    } else {
        const ParsedTarget target = parseTarget(pos.at(1));
        if (!target.error.isEmpty()) {
            req.error = target.error;
            return req;
        }
        profile.host = target.host.trimmed();
        profile.user = target.user.trimmed();

        if (parser.isSet(QStringLiteral("user"))) {
            profile.user = parser.value(QStringLiteral("user")).trimmed();
        }

        int port = target.port >= 0 ? target.port : defaultPortFor(mode);
        if (parser.isSet(QStringLiteral("port"))) {
            bool ok = false;
            const int cliPort = parser.value(QStringLiteral("port")).toInt(&ok);
            if (!ok || cliPort < 1 || cliPort > 65535) {
                req.error = QStringLiteral("invalid --port value");
                return req;
            }
            port = cliPort;
        }
        profile.port = port;
    }

    if (parser.isSet(QStringLiteral("name"))) {
        profile.name = parser.value(QStringLiteral("name")).trimmed();
    }

    const int authOptions = int(parser.isSet(QStringLiteral("password")))
        + int(parser.isSet(QStringLiteral("identity")))
        + int(parser.isSet(QStringLiteral("keyring")))
        + int(parser.isSet(QStringLiteral("agent")));
    if (authOptions > 1) {
        req.error = QStringLiteral("choose only one authentication option: --password, --identity, --stored-key, or --agent");
        return req;
    }
    if (parser.isSet(QStringLiteral("password"))) {
        profile.password = parser.value(QStringLiteral("password"));
        profile.authMethod = AuthMethod::Password;
    }
    if (parser.isSet(QStringLiteral("identity"))) {
        profile.privateKeyPath = parser.value(QStringLiteral("identity")).trimmed();
        profile.authMethod = AuthMethod::KeyFile;
    }
    if (parser.isSet(QStringLiteral("keyring"))) {
        profile.privateKeyId = parser.value(QStringLiteral("keyring")).trimmed();
        profile.authMethod = AuthMethod::StoredKey;
    }
    if (parser.isSet(QStringLiteral("agent"))) {
        profile.authMethod = AuthMethod::SshAgent;
    }
    if (parser.isSet(QStringLiteral("key-passphrase"))) {
        profile.keyPassphrase = parser.value(QStringLiteral("key-passphrase"));
    }
    if (profile.isRdp()) {
        if (parser.isSet(QStringLiteral("identity")) || parser.isSet(QStringLiteral("keyring"))
            || parser.isSet(QStringLiteral("agent"))) {
            req.error = QStringLiteral("RDP supports only password authentication");
            return req;
        }
        if (parser.isSet(QStringLiteral("domain"))) {
            profile.rdpDomain = parser.value(QStringLiteral("domain")).trimmed();
        }
        profile.authMethod = AuthMethod::Password;
    }

    req.openSftpWithSsh = parser.isSet(QStringLiteral("sftp"));

    if (profile.host.isEmpty()) {
        req.error = QStringLiteral("host is required");
        return req;
    }

    if (!profile.isTelnet() && !profile.isSerial() && !profile.isRdp() && profile.user.isEmpty()) {
        profile.user = AppSettings::defaultUser().trimmed();
    }

    if (!profile.isTelnet() && !profile.isSerial() && profile.user.isEmpty()) {
        req.error = QStringLiteral("username required (use user@host or -u/--user)");
        return req;
    }

    if (req.openSftpWithSsh && profile.connectionMode != ConnectionMode::Ssh) {
        req.error = QStringLiteral("--sftp is only valid with ssh");
        return req;
    }

    req.profile = profile;
    req.error.clear();
    return req;
}

int runHeadlessPhase(int argc, char* argv[], Request* outRequest, bool* outLaunch, bool* verboseOut)
{
    if (outRequest) {
        *outRequest = {};
    }
    if (outLaunch) {
        *outLaunch = false;
    }
    if (verboseOut) {
        *verboseOut = false;
    }

    const bool wantHelp = argvHasHelp(argc, argv);
    const bool wantVersion = argvHasVersion(argc, argv);

    QCoreApplication core(argc, argv);
    core.setOrganizationName(QStringLiteral("clientosh"));
    core.setApplicationName(QStringLiteral("clientosh"));
    core.setApplicationVersion(QStringLiteral(CLIENTOSH_VERSION));
    QSettings::setDefaultFormat(QSettings::IniFormat);

    QCommandLineParser parser;
    configureParser(parser);
    addStandardOptions(parser);

    if (wantHelp) {
        clientoshCliWrite(parser.helpText());
        return 0;
    }
    if (wantVersion) {
        clientoshCliWrite(QStringLiteral("clientosh %1\n").arg(QStringLiteral(CLIENTOSH_VERSION)));
        return 0;
    }
    if (argvHasRemovedNameOption(argc, argv)) {
        clientoshCliWrite(
            QStringLiteral("clientosh: unknown option '-name' (use '-n' or '--name')\n"));
        return 1;
    }

    const QStringList args = core.arguments();
    parser.parse(args);

    if (!parser.errorText().isEmpty()) {
        clientoshCliWrite(
            QStringLiteral("clientosh: %1\nTry '%2 --help' for usage.\n")
                .arg(parser.errorText(), QString::fromLocal8Bit(argv[0])));
        return 1;
    }

    if (parser.isSet(QStringLiteral("help")) || parser.isSet(QStringLiteral("h"))
        || parser.isSet(QStringLiteral("?"))) {
        clientoshCliWrite(parser.helpText());
        return 0;
    }
    if (parser.isSet(QStringLiteral("version")) || parser.isSet(QStringLiteral("v"))) {
        clientoshCliWrite(QStringLiteral("clientosh %1\n").arg(QStringLiteral(CLIENTOSH_VERSION)));
        return 0;
    }

    if (verboseOut && parser.isSet(QStringLiteral("verbose"))) {
        *verboseOut = true;
    }

    const Request req = parse(parser);
    if (outRequest) {
        *outRequest = req;
    }
    if (outLaunch) {
        *outLaunch = req.launch;
    }

    if (req.launch && !req.error.isEmpty()) {
        clientoshCliWrite(
            QStringLiteral("clientosh: %1\nTry '%2 --help' for usage.\n")
                .arg(req.error, QString::fromLocal8Bit(argv[0])));
        return 1;
    }

    return -1;
}

} // namespace CliLaunch
