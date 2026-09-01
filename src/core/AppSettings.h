#pragma once

#include <QColor>
#include <QCoreApplication>
#include <QHash>
#include <QKeySequence>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

/** Central QSettings keys for clientosh preferences. */
namespace AppSettings {

/**
 * Force all QSettings() usage onto an INI file under the app config dir
 * (e.g. %APPDATA%/clientosh/clientosh.ini on Windows) instead of the
 * Windows registry. Safe to call before QApplication exists.
 *
 * Migrates non-secret NativeFormat settings once when the INI is empty.
 * Always strips leftover plaintext passwords / passphrases from both stores —
 * secrets belong only in the encrypted vault (dbvault), never in INI/registry.
 */
inline bool isSecretSettingsKey(const QString& key)
{
    const QString lower = key.toLower();
    return lower == QLatin1String("password")
        || lower == QLatin1String("keypassphrase")
        || lower.endsWith(QLatin1String("/password"))
        || lower.endsWith(QLatin1String("\\password"))
        || lower.endsWith(QLatin1String("/keypassphrase"))
        || lower.endsWith(QLatin1String("\\keypassphrase"));
}

inline void purgePlaintextSecrets(QSettings& s)
{
    const QStringList keys = s.allKeys();
    bool changed = false;
    for (const QString& key : keys) {
        if (!isSecretSettingsKey(key)) {
            continue;
        }
        s.remove(key);
        changed = true;
    }
    // Legacy profile array must not live beside the vault — wipe the whole group.
    if (s.childGroups().contains(QLatin1String("profiles"))
        || keys.contains(QLatin1String("profiles/size"))
        || !s.value(QLatin1String("profiles/size")).isNull()) {
        s.beginGroup(QLatin1String("profiles"));
        s.remove(QString());
        s.endGroup();
        s.remove(QLatin1String("profiles"));
        changed = true;
    }
    if (changed) {
        s.sync();
    }
}

inline void purgeAllPlaintextSecrets()
{
    QSettings ini(QSettings::IniFormat, QSettings::UserScope,
                  QStringLiteral("clientosh"), QStringLiteral("clientosh"));
    purgePlaintextSecrets(ini);

    QSettings native(QSettings::NativeFormat, QSettings::UserScope,
                     QStringLiteral("clientosh"), QStringLiteral("clientosh"));
    purgePlaintextSecrets(native);
}

inline void configureFileStorage()
{
    QCoreApplication::setOrganizationName(QStringLiteral("clientosh"));
    QCoreApplication::setApplicationName(QStringLiteral("clientosh"));
    QSettings::setDefaultFormat(QSettings::IniFormat);

    static bool migrated = false;
    if (migrated) {
        return;
    }
    migrated = true;

    QSettings ini(QSettings::IniFormat, QSettings::UserScope,
                  QStringLiteral("clientosh"), QStringLiteral("clientosh"));
    if (ini.allKeys().isEmpty()) {
        QSettings native(QSettings::NativeFormat, QSettings::UserScope,
                         QStringLiteral("clientosh"), QStringLiteral("clientosh"));
        const QStringList keys = native.allKeys();
        for (const QString& key : keys) {
            // Never copy secrets or the legacy profiles array into the INI —
            // profiles/secrets are migrated into the encrypted vault by loadProfiles().
            if (isSecretSettingsKey(key)
                || key.startsWith(QLatin1String("profiles/"))) {
                continue;
            }
            ini.setValue(key, native.value(key));
        }
        ini.sync();
    }

    // Always scrub leftover plaintext passwords from INI + Native stores.
    purgeAllPlaintextSecrets();
}

inline constexpr const char* kSavePasswordDefault = "settings/savePasswordDefault";
inline constexpr const char* kHideDotfiles = "settings/hideDotfiles";
inline constexpr const char* kAnimationsEnabled = "settings/animationsEnabled";
inline constexpr const char* kFontSize = "settings/fontSize";
inline constexpr const char* kFontFamily = "settings/fontFamily";
inline constexpr const char* kUiFontSize = "settings/uiFontSize";
inline constexpr const char* kUiFontFamily = "settings/uiFontFamily";
inline constexpr const char* kTheme = "settings/theme";
inline constexpr const char* kTerminalFg = "settings/terminalFg";
inline constexpr const char* kTerminalBg = "settings/terminalBg";
inline constexpr const char* kTerminalBgImage = "settings/terminalBgImage";
inline constexpr const char* kTerminalBgOpacity = "settings/terminalBgOpacity";
inline constexpr const char* kTerminalBgBlur = "settings/terminalBgBlur";
inline constexpr const char* kDefaultHost = "settings/defaultHost";
inline constexpr const char* kDefaultUser = "settings/defaultUser";
inline constexpr const char* kDefaultPort = "settings/defaultPort";
inline constexpr const char* kLastAuthMethod = "settings/lastAuthMethod";
inline constexpr const char* kStatsIntervalSec = "settings/statsIntervalSec";
inline constexpr const char* kShowServerStats = "settings/showServerStats";
inline constexpr const char* kSftpDefaultView = "settings/sftpDefaultView";
inline constexpr const char* kSftpVerboseLogging = "settings/sftpVerboseLogging";
inline constexpr const char* kHighlightAddresses = "settings/highlightAddresses";
inline constexpr const char* kHighlightLogKeywords = "settings/highlightLogKeywords";
inline constexpr const char* kHighlightCiscoCli = "settings/highlightCiscoCli";
inline constexpr const char* kCtrlScrollFontZoom = "settings/ctrlScrollFontZoom";
inline constexpr const char* kScrollSensitivity = "settings/scrollSensitivity";
inline constexpr const char* kCopyPasteMode = "settings/copyPasteMode";
inline constexpr const char* kShortcutNewSession = "settings/shortcutNewSession";
inline constexpr const char* kShortcutSettings = "settings/shortcutSettings";
inline constexpr const char* kShortcutDashboard = "settings/shortcutDashboard";
inline constexpr const char* kShortcutClosePanel = "settings/shortcutClosePanel";
inline constexpr const char* kShortcutOpenSftp = "settings/shortcutOpenSftp";
inline constexpr const char* kShortcutClearTerminal = "settings/shortcutClearTerminal";
inline constexpr const char* kShortcutFontLarger = "settings/shortcutFontLarger";
inline constexpr const char* kShortcutFontSmaller = "settings/shortcutFontSmaller";
inline constexpr const char* kShortcutFontReset = "settings/shortcutFontReset";
inline constexpr const char* kShortcutNewSessionEnabled = "settings/shortcutNewSessionEnabled";
inline constexpr const char* kShortcutSettingsEnabled = "settings/shortcutSettingsEnabled";
inline constexpr const char* kShortcutDashboardEnabled = "settings/shortcutDashboardEnabled";
inline constexpr const char* kShortcutClosePanelEnabled = "settings/shortcutClosePanelEnabled";
inline constexpr const char* kShortcutOpenSftpEnabled = "settings/shortcutOpenSftpEnabled";
inline constexpr const char* kShortcutClearTerminalEnabled = "settings/shortcutClearTerminalEnabled";
inline constexpr const char* kShortcutFontLargerEnabled = "settings/shortcutFontLargerEnabled";
inline constexpr const char* kShortcutFontSmallerEnabled = "settings/shortcutFontSmallerEnabled";
inline constexpr const char* kShortcutFontResetEnabled = "settings/shortcutFontResetEnabled";
inline constexpr const char* kAlwaysOnTop = "window/alwaysOnTop";
inline constexpr const char* kProxyEnabled = "settings/proxyEnabled";
inline constexpr const char* kProxyProtocol = "settings/proxyProtocol";
inline constexpr const char* kProxyHost = "settings/proxyHost";
inline constexpr const char* kProxyPort = "settings/proxyPort";
inline constexpr const char* kProxyAuthEnabled = "settings/proxyAuthEnabled";
inline constexpr const char* kProxyUsername = "settings/proxyUsername";

enum class ProxyProtocol {
    Http = 0,
    Socks4 = 1,
    Socks5 = 2
};

inline QColor colorFromSetting(const char* key, const QColor& fallback)
{
    const QString hex = QSettings().value(QLatin1String(key)).toString().trimmed();
    if (hex.isEmpty()) {
        return fallback;
    }
    const QColor c(hex);
    return c.isValid() ? c : fallback;
}

inline void setValueSync(const char* key, const QVariant& value)
{
    QSettings s;
    s.setValue(QLatin1String(key), value);
    s.sync();
}

inline void setColorSetting(const char* key, const QColor& color)
{
    setValueSync(key, color.name(QColor::HexRgb));
}

inline bool savePasswordDefault()
{
    return QSettings().value(QLatin1String(kSavePasswordDefault), false).toBool();
}

inline bool hideDotfiles()
{
    return QSettings().value(QLatin1String(kHideDotfiles), true).toBool();
}

inline bool animationsEnabled()
{
    return QSettings().value(QLatin1String(kAnimationsEnabled), true).toBool();
}

inline int fontSize()
{
    return qBound(9, QSettings().value(QLatin1String(kFontSize), 11).toInt(), 22);
}

inline void setFontSize(int points)
{
    const int next = qBound(9, points, 22);
    QSettings s;
    s.setValue(QLatin1String(kFontSize), next);
    s.sync();
}

/** Empty = auto-pick a monospace face. */
inline QString fontFamily()
{
    return QSettings().value(QLatin1String(kFontFamily), QString()).toString().trimmed();
}

inline int uiFontSize()
{
    return qBound(9, QSettings().value(QLatin1String(kUiFontSize), 10).toInt(), 22);
}

/** Empty = system UI default. */
inline QString uiFontFamily()
{
    return QSettings().value(QLatin1String(kUiFontFamily), QString()).toString().trimmed();
}

inline QString theme()
{
    return QSettings().value(QLatin1String(kTheme), QStringLiteral("dark")).toString();
}

inline bool isLightTheme()
{
    return theme().compare(QLatin1String("light"), Qt::CaseInsensitive) == 0;
}

inline QColor defaultTerminalFgForTheme(bool light)
{
    return light ? QColor(0x1a, 0x1a, 0x1a) : QColor(0xc8, 0xc8, 0xc8);
}

inline QColor defaultTerminalBgForTheme(bool light)
{
    return light ? QColor(0xff, 0xff, 0xff) : QColor(0x1a, 0x1a, 0x1a);
}

inline QColor terminalFg()
{
    return colorFromSetting(kTerminalFg, defaultTerminalFgForTheme(isLightTheme()));
}

inline QColor terminalBg()
{
    return colorFromSetting(kTerminalBg, defaultTerminalBgForTheme(isLightTheme()));
}

inline QString terminalBgImage()
{
    return QSettings().value(QLatin1String(kTerminalBgImage), QString()).toString();
}

inline void setTerminalBgImage(const QString& imagePath)
{
    setValueSync(kTerminalBgImage, imagePath);
}

inline qreal terminalBgOpacity()
{
    return qBound(0.0, QSettings().value(QLatin1String(kTerminalBgOpacity), 0.5).toReal(), 1.0);
}

inline void setTerminalBgOpacity(qreal opacity)
{
    setValueSync(kTerminalBgOpacity, qBound(0.0, opacity, 1.0));
}

inline int terminalBgBlur()
{
    return qBound(0, QSettings().value(QLatin1String(kTerminalBgBlur), 0).toInt(), 100);
}

inline void setTerminalBgBlur(int radius)
{
    setValueSync(kTerminalBgBlur, qBound(0, radius, 100));
}

inline void resetTerminalAppearance()
{
    QSettings s;
    s.remove(QLatin1String(kTerminalFg));
    s.remove(QLatin1String(kTerminalBg));
    s.remove(QLatin1String(kTerminalBgImage));
    s.remove(QLatin1String(kTerminalBgOpacity));
    s.remove(QLatin1String(kTerminalBgBlur));
    s.sync();
}

inline QString defaultHost()
{
    return QSettings().value(QLatin1String(kDefaultHost), QStringLiteral("127.0.0.1")).toString();
}

inline QString defaultUser()
{
    return QSettings().value(QLatin1String(kDefaultUser), QString()).toString();
}

inline int defaultPort()
{
    return qBound(1, QSettings().value(QLatin1String(kDefaultPort), 22).toInt(), 65535);
}

/** AuthMethod value last selected while creating an SSH/SFTP session. */
inline int lastAuthMethod()
{
    const int method = QSettings().value(QLatin1String(kLastAuthMethod), 0).toInt();
    return (method >= 0 && method <= 3) ? method : 0;
}

inline void setLastAuthMethod(int method)
{
    setValueSync(kLastAuthMethod, (method >= 0 && method <= 3) ? method : 0);
}

inline int statsIntervalSec()
{
    return qBound(1, QSettings().value(QLatin1String(kStatsIntervalSec), 2).toInt(), 30);
}

inline bool showServerStats()
{
    return QSettings().value(QLatin1String(kShowServerStats), true).toBool();
}

inline bool alwaysOnTop()
{
    return QSettings().value(QLatin1String(kAlwaysOnTop), false).toBool();
}

inline void setAlwaysOnTop(bool on)
{
    setValueSync(kAlwaysOnTop, on);
}

/** "details" (name/size/type) or "compact" (name/size). */
inline QString sftpDefaultView()
{
    const QString v = QSettings().value(QLatin1String(kSftpDefaultView), QStringLiteral("details")).toString();
    return (v == QLatin1String("compact")) ? QStringLiteral("compact") : QStringLiteral("details");
}

inline bool sftpCompactView()
{
    return sftpDefaultView() == QLatin1String("compact");
}

inline bool sftpVerboseLogging()
{
    return QSettings().value(QLatin1String(kSftpVerboseLogging), false).toBool();
}

inline void setSftpVerboseLogging(bool enabled)
{
    setValueSync(kSftpVerboseLogging, enabled);
}

inline bool highlightAddresses()
{
    return QSettings().value(QLatin1String(kHighlightAddresses), true).toBool();
}

inline void setHighlightAddresses(bool enabled)
{
    setValueSync(kHighlightAddresses, enabled);
}

inline bool highlightLogKeywords()
{
    return QSettings().value(QLatin1String(kHighlightLogKeywords), true).toBool();
}

inline void setHighlightLogKeywords(bool enabled)
{
    setValueSync(kHighlightLogKeywords, enabled);
}

inline bool highlightCiscoCli()
{
    return QSettings().value(QLatin1String(kHighlightCiscoCli), false).toBool();
}

inline void setHighlightCiscoCli(bool enabled)
{
    setValueSync(kHighlightCiscoCli, enabled);
}

inline bool ctrlScrollFontZoom()
{
    return QSettings().value(QLatin1String(kCtrlScrollFontZoom), true).toBool();
}

/** Lines scrolled per wheel notch (1–20). Default 1 line per notch. */
inline int scrollSensitivity()
{
    return qBound(1, QSettings().value(QLatin1String(kScrollSensitivity), 1).toInt(), 20);
}

inline void setScrollSensitivity(int lines)
{
    setValueSync(kScrollSensitivity, qBound(1, lines, 20));
}

/** "standard" or "menu". */
inline QString copyPasteMode()
{
    const QString v = QSettings().value(QLatin1String(kCopyPasteMode), QStringLiteral("standard")).toString();
    return (v == QLatin1String("menu")) ? QStringLiteral("menu") : QStringLiteral("standard");
}

inline bool copyPasteMenu()
{
    return copyPasteMode() == QLatin1String("menu");
}

inline void setCopyPasteMode(const QString& mode)
{
    const QString next = (mode == QLatin1String("menu")) ? QStringLiteral("menu")
                                                         : QStringLiteral("standard");
    setValueSync(kCopyPasteMode, next);
}

inline QKeySequence shortcutFromSetting(const char* key, const QKeySequence& fallback)
{
    const QString stored = QSettings().value(QLatin1String(key)).toString().trimmed();
    if (stored.isEmpty()) {
        return fallback;
    }
    const QKeySequence seq = QKeySequence::fromString(stored, QKeySequence::PortableText);
    return seq.isEmpty() ? fallback : seq;
}

inline void setShortcutSetting(const char* key, const QKeySequence& seq)
{
    setValueSync(key, seq.toString(QKeySequence::PortableText));
}

inline void setShortcutEnabled(const char* key, bool enabled)
{
    setValueSync(key, enabled);
}

inline bool shortcutEnabled(const char* key)
{
    // A shortcut is enabled by default unless explicitly disabled by the user.
    return QSettings().value(QLatin1String(key), true).toBool();
}

inline QKeySequence shortcutNewSession()
{
    if (!shortcutEnabled(kShortcutNewSessionEnabled)) {
        return {};
    }
    return shortcutFromSetting(kShortcutNewSession, QKeySequence(QStringLiteral("Ctrl+N")));
}

inline QKeySequence shortcutSettings()
{
    if (!shortcutEnabled(kShortcutSettingsEnabled)) {
        return {};
    }
    return shortcutFromSetting(kShortcutSettings, QKeySequence(QStringLiteral("Ctrl+,")));
}

inline QKeySequence shortcutDashboard()
{
    if (!shortcutEnabled(kShortcutDashboardEnabled)) {
        return {};
    }
    return shortcutFromSetting(kShortcutDashboard, QKeySequence(QStringLiteral("Ctrl+Shift+D")));
}

inline QKeySequence shortcutClosePanel()
{
    if (!shortcutEnabled(kShortcutClosePanelEnabled)) {
        return {};
    }
    return shortcutFromSetting(kShortcutClosePanel, QKeySequence(QStringLiteral("Ctrl+W")));
}

inline QKeySequence shortcutOpenSftp()
{
    if (!shortcutEnabled(kShortcutOpenSftpEnabled)) {
        return {};
    }
    return shortcutFromSetting(kShortcutOpenSftp, QKeySequence(QStringLiteral("Ctrl+Shift+S")));
}

inline QKeySequence shortcutClearTerminal()
{
    if (!shortcutEnabled(kShortcutClearTerminalEnabled)) {
        return {};
    }
    return shortcutFromSetting(kShortcutClearTerminal,
                               QKeySequence(QStringLiteral("Ctrl+Shift+K")));
}

inline QKeySequence shortcutFontLarger()
{
    if (!shortcutEnabled(kShortcutFontLargerEnabled)) {
        return {};
    }
    return shortcutFromSetting(kShortcutFontLarger, QKeySequence(QStringLiteral("Ctrl+=")));
}

inline QKeySequence shortcutFontSmaller()
{
    if (!shortcutEnabled(kShortcutFontSmallerEnabled)) {
        return {};
    }
    return shortcutFromSetting(kShortcutFontSmaller, QKeySequence(QStringLiteral("Ctrl+-")));
}

inline QKeySequence shortcutFontReset()
{
    if (!shortcutEnabled(kShortcutFontResetEnabled)) {
        return {};
    }
    return shortcutFromSetting(kShortcutFontReset, QKeySequence(QStringLiteral("Ctrl+0")));
}

// ---- Tags (host organization) -----------------------------------------------
inline constexpr const char* kTagsDefinitions = "tags/definitions";
inline constexpr const char* kTagsAssignments = "tags/assignments";
inline constexpr const char* kTagsCollapsed = "tags/collapsed";

/** Tags are stored as a list of names under tags/definitions. */
inline QStringList tagDefinitions()
{
    return QSettings().value(QLatin1String(kTagsDefinitions), QStringList()).toStringList();
}

inline void setTagDefinitions(const QStringList& tags)
{
    setValueSync(kTagsDefinitions, tags);
}

/** Host-to-tag assignments: tag name → list of profile IDs. */
inline QHash<QString, QStringList> tagAssignments()
{
    const QVariantMap m = QSettings().value(QLatin1String(kTagsAssignments)).toMap();
    QHash<QString, QStringList> out;
    for (auto it = m.begin(); it != m.end(); ++it) {
        out.insert(it.key(), it.value().toStringList());
    }
    return out;
}

inline void setTagAssignments(const QHash<QString, QStringList>& assign)
{
    QVariantMap m;
    for (auto it = assign.begin(); it != assign.end(); ++it) {
        m.insert(it.key(), QVariant(it.value()));
    }
    QSettings s;
    s.setValue(QLatin1String(kTagsAssignments), m);
    s.sync();
}

/** Which tags are collapsed (not expanded). */
inline QStringList tagCollapsed()
{
    return QSettings().value(QLatin1String(kTagsCollapsed), QStringList()).toStringList();
}

inline void setTagCollapsed(const QStringList& collapsed)
{
    setValueSync(kTagsCollapsed, collapsed);
}

inline bool proxyEnabled()
{
    return QSettings().value(QLatin1String(kProxyEnabled), false).toBool();
}

inline void setProxyEnabled(bool on)
{
    setValueSync(kProxyEnabled, on);
}

inline ProxyProtocol proxyProtocol()
{
    const int raw = QSettings().value(QLatin1String(kProxyProtocol),
                                      int(ProxyProtocol::Socks5)).toInt();
    if (raw >= int(ProxyProtocol::Http) && raw <= int(ProxyProtocol::Socks5)) {
        return static_cast<ProxyProtocol>(raw);
    }
    return ProxyProtocol::Socks5;
}

inline void setProxyProtocol(ProxyProtocol protocol)
{
    setValueSync(kProxyProtocol, int(protocol));
}

inline QString proxyHost()
{
    return QSettings().value(QLatin1String(kProxyHost), QString()).toString();
}

inline void setProxyHost(const QString& host)
{
    setValueSync(kProxyHost, host.trimmed());
}

inline int proxyPort()
{
    return qBound(1, QSettings().value(QLatin1String(kProxyPort), 8080).toInt(), 65535);
}

inline void setProxyPort(int port)
{
    setValueSync(kProxyPort, qBound(1, port, 65535));
}

inline bool proxyAuthEnabled()
{
    return QSettings().value(QLatin1String(kProxyAuthEnabled), false).toBool();
}

inline void setProxyAuthEnabled(bool on)
{
    setValueSync(kProxyAuthEnabled, on);
}

inline QString proxyUsername()
{
    return QSettings().value(QLatin1String(kProxyUsername), QString()).toString();
}

inline void setProxyUsername(const QString& user)
{
    setValueSync(kProxyUsername, user.trimmed());
}

} // namespace AppSettings
