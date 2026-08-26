#pragma once

#include "PanelTypes.h"
#include "core/SessionProfile.h"
#include "core/addons/AddonHostContext.h"

#include <QMainWindow>
#include <QHash>
#include <QSet>

class QStackedWidget;
class QShortcut;
class QShowEvent;
class QProgressDialog;
class SessionManager;
class DashboardPage;
class SessionWorkspace;
class TerminalWidget;
class TopNavBar;
class SftpWindow;
class AiAgentBridge;
class QHBoxLayout;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    /** Open an ad-hoc session, e.g. one supplied on the command line. */
    void openSession(const SessionProfile& profile);

    /** Open a session requested from the command line (after the window is shown). */
    void launchFromCli(const SessionProfile& profile, bool openSftpWithSsh);

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void applyTheme();
    void setupShortcuts();
    void rebindShortcuts();
    void adjustAllTerminalFonts(int deltaOrAbsolute, bool absolute);
    void toggleDashboard();
    void showDashboard();
    void showWorkspace();
    void openProfileSession(const SessionProfile& profile);
    void openProfileThenSftp(const SessionProfile& profile);
    void openProfileSftpOnly(const SessionProfile& profile);
    /** Mount terminal, layout for real size, then start SSH with matching PTY. */
    QString beginTerminalSession(const SessionProfile& profile, bool openSftpWhenConnected);
    void openOrFocusSession(const QString& id);
    void openOrFocusPanel(const PanelRef& ref);
    void closePanel(const PanelRef& ref);
    void savePanelProfile(const PanelRef& ref);
    void closeLiveSession(const QString& id);
    void openSftp(const QString& id);
    void openStandaloneSftp(const SessionProfile& profile);
    void openSftpFromSession(const QString& sessionId);
    void createSftpPane(const QString& panelId, const SessionProfile& profile);
    void wireSessionTerminal(const QString& id, TerminalWidget* term);
    void startXmodemTransfer(const QString& id);
    void closeXmodemProgress(const QString& id);
    TerminalWidget* findTerminal(const QString& id) const;
    void applyAlwaysOnTop(bool on);
    void onAiAgentBridgeChanged(AiAgentBridge* bridge);
    void setAiAgentPanelVisible(bool visible);
    void refreshAiAgentSessionContext();

    SessionManager* m_sessions = nullptr;
    TopNavBar* m_topNav = nullptr;
    QStackedWidget* m_rootStack = nullptr;
    QWidget* m_bodyHost = nullptr;
    QHBoxLayout* m_bodyLay = nullptr;
    DashboardPage* m_dashboard = nullptr;
    SessionWorkspace* m_workspace = nullptr;
    AddonHostContext m_addonContext;
    AiAgentBridge* m_aiBridge = nullptr;
    QWidget* m_aiPanel = nullptr;
    QHash<QString, SftpWindow*> m_sftpPanes;
    QHash<QString, QProgressDialog*> m_xmodemProgressDialogs;
    QString m_pendingSftpSessionId;
    QSet<QString> m_sftpOnlySessionIds;

    QShortcut* m_scNewSession = nullptr;
    QShortcut* m_scSettings = nullptr;
    QShortcut* m_scDashboard = nullptr;
    QShortcut* m_scClosePanel = nullptr;
    QShortcut* m_scOpenSftp = nullptr;
    QShortcut* m_scClearTerminal = nullptr;
    QShortcut* m_scFontLarger = nullptr;
    QShortcut* m_scFontSmaller = nullptr;
    QShortcut* m_scFontReset = nullptr;
};
