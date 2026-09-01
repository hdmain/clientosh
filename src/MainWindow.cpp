#include "MainWindow.h"
#include "DashboardPage.h"
#include "PanelTypes.h"
#include "PlatformFonts.h"
#include "core/AppSettings.h"
#include "core/SessionManager.h"
#include "core/SessionProfile.h"
#include "core/addons/AddonHostContext.h"
#include "core/addons/AiAgentBridge.h"
#include "platform/WindowStayOnTop.h"
#include "RdpPane.h"
#include "SessionWorkspace.h"
#include "SftpWindow.h"
#include "TerminalWidget.h"
#include "TopNavBar.h"

#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("clientosh"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/terminal.svg")));
    resize(1100, 700);
    setMinimumSize(720, 480);

    m_sessions = new SessionManager(this);

    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_workspace = new SessionWorkspace(m_sessions);
    m_topNav = new TopNavBar(m_sessions, m_workspace, central);
    root->addWidget(m_topNav);

    m_bodyHost = new QWidget(central);
    m_bodyLay = new QHBoxLayout(m_bodyHost);
    m_bodyLay->setContentsMargins(0, 0, 0, 0);
    m_bodyLay->setSpacing(0);
    root->addWidget(m_bodyHost, 1);

    m_rootStack = new QStackedWidget(m_bodyHost);
    m_bodyLay->addWidget(m_rootStack, 1);

    m_addonContext.setSessionContextProvider([this](QString* host, QString* user, int* port,
                                                     QString* cwdHint) {
        const QString id = m_sessions ? m_sessions->activeId() : QString();
        if (id.isEmpty() || !m_sessions) {
            return;
        }
        if (const auto* live = m_sessions->session(id)) {
            if (host) {
                *host = live->profile.host;
            }
            if (user) {
                *user = live->profile.user;
            }
            if (port) {
                *port = live->profile.port;
            }
            if (cwdHint) {
                cwdHint->clear(); // filled when the agent runs `pwd`
            }
        }
    });
    m_addonContext.setInjectInput([this](const QByteArray& data) -> bool {
        const QString id = m_sessions ? m_sessions->activeId() : QString();
        TerminalWidget* term = findTerminal(id);
        if (!term) {
            return false;
        }
        // Visually echo the command in the terminal buffer, then send to PTY.
        term->appendOutput(QByteArray("\r\n\x1b[90m# ai ▸ \x1b[0m") + data);
        term->injectInput(data);
        return true;
    });
    m_addonContext.setCaptureTerminal([this](int maxLines) -> QString {
        const QString id = m_sessions ? m_sessions->activeId() : QString();
        TerminalWidget* term = findTerminal(id);
        if (!term) {
            return {};
        }
        return term->captureRecentText(maxLines);
    });
    m_addonContext.setBridgeChangedHandler([this](AiAgentBridge* bridge) {
        onAiAgentBridgeChanged(bridge);
    });

    m_dashboard = new DashboardPage(m_sessions);
    m_dashboard->bindAddonHostContext(&m_addonContext);

    m_rootStack->addWidget(m_dashboard);
    m_rootStack->addWidget(m_workspace);

    connect(m_dashboard, &DashboardPage::openProfile, this, &MainWindow::openProfileSession);
    connect(m_dashboard, &DashboardPage::openLiveSession, this, &MainWindow::openOrFocusSession);
    connect(m_dashboard, &DashboardPage::openSftpForSession, this, &MainWindow::openSftpFromSession);
    connect(m_dashboard, &DashboardPage::openSftpForProfile, this, &MainWindow::openStandaloneSftp);
    connect(m_dashboard, &DashboardPage::closeLiveSession, this, &MainWindow::closeLiveSession);

    connect(m_dashboard, &DashboardPage::settingsApplied, this, [this]() {
        // Theme stylesheet first (may set font-family), then app font so size/family stick.
        applyTheme();
        qApp->setFont(clientoshUiFont(AppSettings::uiFontSize(), AppSettings::uiFontFamily()));
        for (const QString& id : m_sessions->sessionIds()) {
            if (TerminalWidget* term = findTerminal(id)) {
                term->applyAppearanceFromSettings();
                // Font metrics affect both the local grid and the remote PTY/NAWS size.
                term->syncPtySize(true);
            }
        }
        for (auto it = m_sftpPanes.begin(); it != m_sftpPanes.end(); ++it) {
            if (it.value()) {
                it.value()->applyAppSettings();
            }
        }
        m_topNav->applySettings();
        rebindShortcuts();
    });

    connect(m_topNav, &TopNavBar::dashboardRequested, this, &MainWindow::toggleDashboard);
    connect(m_topNav, &TopNavBar::newSessionRequested, this, [this]() {
        showDashboard();
        m_dashboard->showNewSessionForm();
    });
    connect(m_topNav, &TopNavBar::panelSelectRequested, this, &MainWindow::openOrFocusPanel);
    connect(m_topNav, &TopNavBar::panelCloseRequested, this, &MainWindow::closePanel);
    connect(m_topNav, &TopNavBar::savePanelRequested, this, &MainWindow::savePanelProfile);
    connect(m_topNav, &TopNavBar::xmodemRequested, this,
            [this](const QString& id) { startXmodemTransfer(id); });
    connect(m_topNav, &TopNavBar::panelPreviewRequested, this, [this](const PanelRef& ref) {
        // Drag-hover over a tab: activate that viewport. Do not rebuild tabs —
        // destroying the drag-source chip mid-drag crashes.
        m_workspace->previewPanel(ref);
        showWorkspace();
        m_topNav->syncActiveChip();
    });
    connect(m_topNav, &TopNavBar::sftpRequested, this, [this]() {
        const PanelRef active = m_workspace->activePanel();
        if (active.isValid() && active.kind == PanelKind::Sftp) {
            if (SftpWindow* src = m_sftpPanes.value(active.sessionId)) {
                openStandaloneSftp(src->profile());
                return;
            }
        }
        const QString id = m_sessions->activeId();
        if (!id.isEmpty()) {
            openSftpFromSession(id);
            return;
        }
        // No SSH session at all: pick the first saved profile to SFTP into by duplicating
        // an existing standalone pane, or do nothing and let the user use the dashboard.
        if (!m_sftpPanes.isEmpty()) {
            if (SftpWindow* any = m_sftpPanes.constBegin().value()) {
                openStandaloneSftp(any->profile());
            }
        }
    });
    connect(m_topNav, &TopNavBar::alwaysOnTopToggled, this, [this](bool on) {
        AppSettings::setAlwaysOnTop(on);
        applyAlwaysOnTop(on);
    });
    connect(m_topNav, &TopNavBar::aiAgentToggled, this, [this](bool on) {
        setAiAgentPanelVisible(on);
    });

    connect(m_workspace, &SessionWorkspace::sessionSelectRequested, this, &MainWindow::openOrFocusSession);
    connect(m_workspace, &SessionWorkspace::panelSelectRequested, this, &MainWindow::openOrFocusPanel);
    connect(m_workspace, &SessionWorkspace::panelCloseRequested, this, &MainWindow::closePanel);

    connect(m_sessions, &SessionManager::sessionActivated, this, [this](const QString&) {
        refreshAiAgentSessionContext();
    });
    connect(m_sessions, &SessionManager::sessionDataReceived, this,
            [this](const QString& id, const QByteArray& data) {
                if (TerminalWidget* term = findTerminal(id)) {
                    term->appendOutput(data);
                }
            });
    connect(m_sessions, &SessionManager::sessionConnectionChanged, this,
            [this](const QString& id, bool connected) {
                if (TerminalWidget* term = findTerminal(id)) {
                    term->setInteractive(connected);
                    const auto* liveForTransfer = m_sessions->session(id);
                    term->setXmodemAvailable(connected && liveForTransfer
                                             && liveForTransfer->profile.isSerial());
                    if (connected) {
                        // Replace the temporary startup message instead of leaving
                        // a stale "connecting..." line after the socket is ready.
                        term->appendOutput(QByteArray("\r\x1b[2Kconnected\r\n"));
                        term->setFocus(Qt::OtherFocusReason);
                        term->scrollViewToBottom();
                        // After layout/paint, push the real cols/rows to the PTY.
                        const QPointer<TerminalWidget> termGuard(term);
                        QTimer::singleShot(0, this, [this, id, termGuard]() {
                            if (!termGuard) {
                                return;
                            }
                            termGuard->syncPtySize(true);
                            termGuard->scrollViewToBottom();
                            if (auto* s = m_sessions->session(id); s && s->connected) {
                                int cols = 0;
                                int rows = 0;
                                termGuard->estimatePtySize(&cols, &rows);
                                m_sessions->resizePty(id, cols, rows);
                            }
                        });
                        // One more pass after the workspace has finished settling.
                        QTimer::singleShot(50, this, [this, id, termGuard]() {
                            if (!termGuard) {
                                return;
                            }
                            termGuard->syncPtySize(true);
                            termGuard->scrollViewToBottom();
                            if (auto* s = m_sessions->session(id); s && s->connected) {
                                int cols = 0;
                                int rows = 0;
                                termGuard->estimatePtySize(&cols, &rows);
                                m_sessions->resizePty(id, cols, rows);
                            }
                        });
                    }
                }
                if (connected && m_pendingSftpSessionId == id) {
                    m_pendingSftpSessionId.clear();
                    openSftp(id);
                }
                // Dashboard lists are owned by DashboardPage's SessionManager handlers.
                m_topNav->refresh();
            });

    connect(m_sessions, &SessionManager::xmodemStarted, this,
            [this](const QString& id, qint64 totalBytes) {
                if (QProgressDialog* dialog = m_xmodemProgressDialogs.value(id)) {
                    dialog->setLabelText(QStringLiteral("Waiting for the receiver · %1 MiB")
                                             .arg(double(totalBytes) / (1024.0 * 1024.0), 0, 'f', 1));
                }
                m_topNav->refresh();
            });
    connect(m_sessions, &SessionManager::xmodemProgress, this,
            [this](const QString& id, qint64 sentBytes, qint64 totalBytes, int retries) {
                if (QProgressDialog* dialog = m_xmodemProgressDialogs.value(id)) {
                    const int progress = totalBytes > 0
                        ? int(qMin<qint64>(1000, sentBytes * 1000 / totalBytes)) : 0;
                    dialog->setValue(progress);
                    dialog->setLabelText(QStringLiteral("Sending via XMODEM · %1 / %2 MiB · retries %3")
                                             .arg(double(sentBytes) / (1024.0 * 1024.0), 0, 'f', 1)
                                             .arg(double(totalBytes) / (1024.0 * 1024.0), 0, 'f', 1)
                                             .arg(retries));
                }
            });
    connect(m_sessions, &SessionManager::xmodemFinished, this, [this](const QString& id) {
        closeXmodemProgress(id);
        if (TerminalWidget* term = findTerminal(id)) {
            const auto* live = m_sessions->session(id);
            term->setInteractive(live && live->connected);
            term->setXmodemAvailable(live && live->connected && live->profile.isSerial());
            term->appendOutput(QByteArray("\r\n[XMODEM transfer completed]\r\n"));
        }
        m_topNav->refresh();
    });
    connect(m_sessions, &SessionManager::xmodemError, this,
            [this](const QString& id, const QString& message) {
                const bool hadDialog = m_xmodemProgressDialogs.contains(id);
                closeXmodemProgress(id);
                if (TerminalWidget* term = findTerminal(id)) {
                    const auto* live = m_sessions->session(id);
                    term->setInteractive(live && live->connected);
                    term->setXmodemAvailable(live && live->connected && live->profile.isSerial());
                    term->appendOutput((QStringLiteral("\r\n[XMODEM error] ") + message
                                        + QStringLiteral("\r\n")).toUtf8());
                }
                if (hadDialog && message != QLatin1String("transfer cancelled")) {
                    QMessageBox::warning(this, QStringLiteral("XMODEM"), message);
                }
                m_topNav->refresh();
            });

    connect(m_sessions, &SessionManager::sessionClosed, this, [this](const QString& id) {
        closeXmodemProgress(id);
        if (m_pendingSftpSessionId == id) {
            m_pendingSftpSessionId.clear();
        }
        m_sftpOnlySessionIds.remove(id);
        m_workspace->removeSessionPanels(id);
        if (SftpWindow* sftp = m_sftpPanes.take(id)) {
            sftp->deleteLater();
        }
        if (!m_workspace->hasAttachedSessions()) {
            showDashboard();
        } else {
            showWorkspace();
        }
        // Dashboard lists are owned by DashboardPage's SessionManager handlers.
        m_topNav->refresh();
    });

    applyTheme();
    setupShortcuts();
    m_topNav->setAlwaysOnTopChecked(AppSettings::alwaysOnTop());
    applyAlwaysOnTop(AppSettings::alwaysOnTop());
    showDashboard();
}

void MainWindow::applyAlwaysOnTop(bool on)
{
    setWindowStayOnTop(this, on);

    // Keep any other clientosh top-level windows in sync (detached panes, etc.).
    const auto tops = QApplication::topLevelWidgets();
    for (QWidget* w : tops) {
        if (!w || w == this || !w->isWindow()) {
            continue;
        }
        const Qt::WindowFlags f = w->windowFlags();
        if (f & Qt::Popup) {
            continue;
        }
        if (qobject_cast<QMenu*>(w)) {
            continue;
        }
        const QString name = w->objectName();
        if (name == QLatin1String("terminalWindow")
            || name == QLatin1String("sftpWindow")
            || name.startsWith(QLatin1String("clientosh"))) {
            setWindowStayOnTop(w, on);
        }
    }
}

void MainWindow::onAiAgentBridgeChanged(AiAgentBridge* bridge)
{
    if (m_aiBridge == bridge) {
        return;
    }

    // Tear down previous UI.
    m_dashboard->setAiAgentSettingsPage(nullptr);
    setAiAgentPanelVisible(false);
    if (m_aiPanel) {
        m_bodyLay->removeWidget(m_aiPanel);
        m_aiPanel->hide();
        m_aiPanel = nullptr;
    }
    m_topNav->setAiAgentAvailable(false);
    m_aiBridge = bridge;

    if (!m_aiBridge) {
        return;
    }

    QWidget* settingsPage = m_aiBridge->createSettingsPage(m_dashboard);
    m_dashboard->setAiAgentSettingsPage(settingsPage);

    m_aiPanel = m_aiBridge->createPanel(m_bodyHost);
    if (m_aiPanel) {
        m_aiPanel->hide();
        m_bodyLay->addWidget(m_aiPanel, 0);
    }
    m_topNav->setAiAgentAvailable(true, m_aiBridge->navIcon());
    refreshAiAgentSessionContext();
}

void MainWindow::setAiAgentPanelVisible(bool visible)
{
    if (!m_aiPanel) {
        m_topNav->setAiAgentPanelOpen(false);
        return;
    }
    m_aiPanel->setVisible(visible);
    m_topNav->setAiAgentPanelOpen(visible);
    if (visible) {
        refreshAiAgentSessionContext();
        showWorkspace();
    }
}

void MainWindow::refreshAiAgentSessionContext()
{
    if (!m_aiBridge) {
        return;
    }
    QString host;
    QString user;
    int port = 22;
    QString cwd;
    if (m_addonContext.sessionContextProvider()) {
        m_addonContext.sessionContextProvider()(&host, &user, &port, &cwd);
    }
    m_aiBridge->setSessionContext(host, user, port, cwd);
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    // Native windows are sometimes recreated on show (esp. macOS / Wayland);
    // re-assert the hint so the setting survives across platforms.
    if (AppSettings::alwaysOnTop()) {
        setWindowStayOnTop(this, true);
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_dashboard) {
        m_dashboard->flushNotesOnExit();
    }
    const auto sftpPanes = m_sftpPanes;
    for (auto it = sftpPanes.begin(); it != sftpPanes.end(); ++it) {
        if (it.value()) {
            m_workspace->takeSftp(it.key());
            it.value()->deleteLater();
        }
    }
    m_sftpPanes.clear();
    QMainWindow::closeEvent(event);
}

void MainWindow::showDashboard()
{
    m_topNav->setWorkspaceActive(false);
    // Do not reload the vault here — profiles are already in memory and live
    // session rows are kept current by DashboardPage's SessionManager handlers.
    m_rootStack->setCurrentWidget(m_dashboard);
    m_topNav->setVisible(m_workspace->hasAttachedSessions());
}

void MainWindow::toggleDashboard()
{
    if (m_rootStack->currentWidget() == m_dashboard && m_workspace->hasAttachedSessions()) {
        const PanelRef previous = m_workspace->activePanel();
        if (previous.isValid()) {
            openOrFocusPanel(previous);
        } else {
            showWorkspace();
        }
        return;
    }
    showDashboard();
}

void MainWindow::showWorkspace()
{
    if (!m_workspace->hasAttachedSessions()) {
        showDashboard();
        return;
    }
    m_topNav->setWorkspaceActive(true);
    m_topNav->setVisible(true);
    m_rootStack->setCurrentWidget(m_workspace);
}

void MainWindow::openSession(const SessionProfile& profile)
{
    openProfileSession(profile);
}

void MainWindow::openProfileSession(const SessionProfile& profile)
{
    if (profile.isSftpOnly()) {
        openProfileSftpOnly(profile);
        return;
    }
    if (profile.isRdp()) {
        openRdpSession(profile);
        return;
    }
    if (profile.port == 3389 && profile.connectionMode == ConnectionMode::Ssh) {
        QMessageBox::information(this, QStringLiteral("rdp"),
                                 QStringLiteral("Port 3389 is used for Remote Desktop.\n"
                                                "Edit the profile and set connection type to RDP."));
        return;
    }
    beginTerminalSession(profile, false);
}

void MainWindow::openProfileThenSftp(const SessionProfile& profile)
{
    if (profile.isSftpOnly()) {
        openStandaloneSftp(profile);
        return;
    }
    if (profile.isTelnet() || profile.isSerial()) {
        beginTerminalSession(profile, false);
        return;
    }
    if (profile.isRdp()) {
        openRdpSession(profile);
        return;
    }
    // SSH + SFTP: open both independently so SFTP never blocks on SSH.
    beginTerminalSession(profile, false);
    openStandaloneSftp(profile);
}

QString MainWindow::beginTerminalSession(const SessionProfile& profile, bool openSftpWhenConnected)
{
    auto* term = new TerminalWidget;
    term->setInteractive(false);
    term->clearTerminal();
    // Apply appearance settings immediately so background image loads on new terminal tabs.
    term->applyAppearanceFromSettings();

    // Create session bookkeeping first so the pane can layout, but do not start SSH yet.
    const QString id = m_sessions->createSession(profile);
    if (openSftpWhenConnected) {
        m_pendingSftpSessionId = id;
    }
    wireSessionTerminal(id, term);
    m_workspace->addTerminal(id, term, profile.displayTitle());
    m_sessions->activateSession(id);
    showWorkspace();
    m_topNav->refresh();

    // Realize the pane geometry so estimatePtySize matches what the user sees.
    term->show();
    m_workspace->updateGeometry();
    QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    // A couple of layout passes — first show from the dashboard can leave 0×0 for a tick.
    for (int i = 0; i < 3; ++i) {
        term->syncPtySize(true);
        if (term->width() >= 80 && term->height() >= 40) {
            break;
        }
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    int cols = 80;
    int rows = 24;
    term->estimatePtySize(&cols, &rows);
    term->syncPtySize(true);
    term->clearTerminal();
    term->appendOutput(QByteArray("connecting..."));
    term->scrollViewToBottom();

    // PTY is requested with this size — MOTD/shell output will wrap correctly.
    m_sessions->connectSession(id, cols, rows);
    return id;
}

void MainWindow::openProfileSftpOnly(const SessionProfile& profile)
{
    // Standalone SFTP: no SessionManager session — SftpClient owns its SSH session.
    openStandaloneSftp(profile);
}

void MainWindow::launchFromCli(const SessionProfile& profile, bool openSftpWithSsh)
{
    if (profile.isSftpOnly()) {
        openStandaloneSftp(profile);
        showWorkspace();
        m_topNav->refresh();
        return;
    }
    if (profile.isRdp()) {
        openRdpSession(profile);
        showWorkspace();
        m_topNav->refresh();
        return;
    }

    if (openSftpWithSsh && !profile.isTelnet()) {
        openProfileThenSftp(profile);
        return;
    }

    beginTerminalSession(profile, false);
}

void MainWindow::wireSessionTerminal(const QString& id, TerminalWidget* term)
{
    if (!term) {
        return;
    }
    connect(term, &TerminalWidget::inputReady, this, [this, id](const QByteArray& data) {
        m_sessions->sendData(id, data);
    });
    connect(term, &TerminalWidget::xmodemSendRequested, this,
            [this, id]() { startXmodemTransfer(id); });
    connect(term, &TerminalWidget::ptySizeChanged, this, [this, id](int cols, int rows) {
        // Always update pending size so auth/PTY open use the laid-out dimensions.
        m_sessions->resizePty(id, cols, rows);
    });
    connect(term, &TerminalWidget::terminalFontSizeChanged, this, [this](int points) {
        for (const QString& sid : m_sessions->sessionIds()) {
            if (TerminalWidget* other = findTerminal(sid)) {
                if (other != sender()) {
                    other->applyAppearanceFromSettings();
                }
            }
        }
        m_dashboard->syncTerminalFontSizeUi(points);
    });
}

void MainWindow::startXmodemTransfer(const QString& id)
{
    const auto* live = m_sessions->session(id);
    if (!live || !live->serial || !live->connected) {
        QMessageBox::warning(this, QStringLiteral("XMODEM"),
                             QStringLiteral("Connect a serial session before starting XMODEM."));
        return;
    }

    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select file to send via XMODEM"), QString(),
        QStringLiteral("Firmware files (*.bin *.tar);;All files (*)"));
    if (path.isEmpty()) return;

    const QFileInfo info(path);
    const QString prompt = QStringLiteral(
        "Make sure the device is already waiting for XMODEM input.\n\n"
        "File: %1\nSize: %2 MiB\n\nStart sending?")
        .arg(info.fileName())
        .arg(double(info.size()) / (1024.0 * 1024.0), 0, 'f', 1);
    if (QMessageBox::question(this, QStringLiteral("Start XMODEM transfer"), prompt,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    closeXmodemProgress(id);
    auto* progress = new QProgressDialog(QStringLiteral("Waiting for the XMODEM receiver…"),
                                         QStringLiteral("Cancel"), 0, 1000, this);
    progress->setWindowTitle(QStringLiteral("XMODEM · %1").arg(info.fileName()));
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);
    m_xmodemProgressDialogs.insert(id, progress);
    connect(progress, &QProgressDialog::canceled, this,
            [this, id]() { m_sessions->cancelXmodem(id); });

    if (TerminalWidget* term = findTerminal(id)) {
        term->setInteractive(false);
        term->setXmodemAvailable(false);
        term->appendOutput(QByteArray("\r\n[XMODEM transfer starting]\r\n"));
    }
    m_sessions->startXmodem(id, path);
    m_topNav->refresh();
}

void MainWindow::closeXmodemProgress(const QString& id)
{
    if (QProgressDialog* dialog = m_xmodemProgressDialogs.take(id)) {
        dialog->blockSignals(true);
        dialog->close();
        dialog->deleteLater();
    }
}

void MainWindow::setupShortcuts()
{
    auto make = [this](QShortcut*& sc, const auto& slot) {
        if (!sc) {
            sc = new QShortcut(this);
            sc->setContext(Qt::ApplicationShortcut);
            connect(sc, &QShortcut::activated, this, slot);
        }
    };

    make(m_scNewSession, [this]() {
        showDashboard();
        m_dashboard->showNewSessionForm();
    });
    make(m_scSettings, [this]() {
        showDashboard();
        m_dashboard->showSettings();
    });
    make(m_scDashboard, [this]() {
        showDashboard();
        m_dashboard->showHome();
    });
    make(m_scClosePanel, [this]() {
        const PanelRef active = m_workspace->activePanel();
        if (active.isValid()) {
            closePanel(active);
        }
    });
    make(m_scOpenSftp, [this]() {
        const PanelRef active = m_workspace->activePanel();
        if (active.isValid() && active.kind == PanelKind::Sftp) {
            if (SftpWindow* src = m_sftpPanes.value(active.sessionId)) {
                openStandaloneSftp(src->profile());
                return;
            }
        }
        const QString id = m_sessions->activeId();
        if (!id.isEmpty()) {
            openSftpFromSession(id);
        }
    });
    make(m_scClearTerminal, [this]() {
        TerminalWidget* term = qobject_cast<TerminalWidget*>(QApplication::focusWidget());
        if (!term && m_rootStack->currentWidget() == m_workspace) {
            const PanelRef active = m_workspace->activePanel();
            if (active.isValid() && active.kind == PanelKind::Terminal) {
                term = findTerminal(active.sessionId);
            }
        }
        if (term) {
            term->clearTerminal();
            term->setFocus(Qt::ShortcutFocusReason);
        }
    });
    make(m_scFontLarger, [this]() { adjustAllTerminalFonts(1, false); });
    make(m_scFontSmaller, [this]() { adjustAllTerminalFonts(-1, false); });
    make(m_scFontReset, [this]() { adjustAllTerminalFonts(11, true); });

    rebindShortcuts();
}

void MainWindow::rebindShortcuts()
{
    auto bind = [](QShortcut* sc, const QKeySequence& seq) {
        if (!sc) {
            return;
        }
        sc->setKey(seq);
        sc->setEnabled(!seq.isEmpty());
    };
    bind(m_scNewSession, AppSettings::shortcutNewSession());
    bind(m_scSettings, AppSettings::shortcutSettings());
    bind(m_scDashboard, AppSettings::shortcutDashboard());
    bind(m_scClosePanel, AppSettings::shortcutClosePanel());
    bind(m_scOpenSftp, AppSettings::shortcutOpenSftp());
    bind(m_scClearTerminal, AppSettings::shortcutClearTerminal());
    bind(m_scFontLarger, AppSettings::shortcutFontLarger());
    bind(m_scFontSmaller, AppSettings::shortcutFontSmaller());
    bind(m_scFontReset, AppSettings::shortcutFontReset());
}

void MainWindow::adjustAllTerminalFonts(int deltaOrAbsolute, bool absolute)
{
    const int next = absolute ? qBound(9, deltaOrAbsolute, 22)
                              : qBound(9, AppSettings::fontSize() + deltaOrAbsolute, 22);
    AppSettings::setFontSize(next);
    TerminalWidget* overlayTerm = qobject_cast<TerminalWidget*>(QApplication::focusWidget());
    if (!overlayTerm) {
        const PanelRef active = m_workspace->activePanel();
        if (active.isValid() && active.kind == PanelKind::Terminal) {
            overlayTerm = findTerminal(active.sessionId);
        }
    }
    for (const QString& id : m_sessions->sessionIds()) {
        if (TerminalWidget* term = findTerminal(id)) {
            term->applyAppearanceFromSettings();
        }
    }
    if (overlayTerm) {
        overlayTerm->showFontSizeOverlay(next);
    }
    m_dashboard->syncTerminalFontSizeUi(next);
}

TerminalWidget* MainWindow::findTerminal(const QString& id) const
{
    return qobject_cast<TerminalWidget*>(m_workspace->terminalWidget(id));
}

void MainWindow::openOrFocusSession(const QString& id)
{
    if (!m_sessions->session(id)) {
        return;
    }
    m_sessions->activateSession(id);
    m_workspace->showSession(id);
    showWorkspace();
    m_topNav->refresh();
}

void MainWindow::openOrFocusPanel(const PanelRef& ref)
{
    if (!ref.isValid()) {
        return;
    }
    if (ref.kind == PanelKind::Terminal && !m_sessions->session(ref.sessionId)) {
        return;
    }
    if (ref.kind == PanelKind::Sftp && !m_workspace->containsPanel(ref)
        && !m_sessions->session(ref.sessionId)) {
        return;
    }
    if (ref.kind == PanelKind::Rdp && !m_workspace->containsPanel(ref)) {
        return;
    }
    if (ref.kind == PanelKind::Terminal) {
        m_sessions->activateSession(ref.sessionId);
    }
    m_workspace->showPanel(ref);
    showWorkspace();
    m_topNav->refresh();
}

void MainWindow::openSftp(const QString& id)
{
    // Legacy SSH-bound path: kept for wiring to SessionManager::sessionConnectionChanged.
    // New standalone flow uses openStandaloneSftp / openSftpFromSession and never reuses ids.
    if (m_workspace->hasSftp(id)) {
        m_workspace->showPanel(PanelRef::sftp(id));
        showWorkspace();
        m_topNav->refresh();
        return;
    }
    auto* live = m_sessions->session(id);
    if (!live || !live->connected) {
        return;
    }
    createSftpPane(id, live->profile);
}

void MainWindow::openStandaloneSftp(const SessionProfile& profile)
{
    if (profile.host.trimmed().isEmpty() || profile.user.trimmed().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("sftp"),
                                 QStringLiteral("Profile needs a host and user to open SFTP."));
        return;
    }
    // Each standalone SFTP gets its own panel key so two SFTPs to the same host can coexist.
    const QString panelId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    createSftpPane(panelId, profile);
    m_dashboard->appendLog(QStringLiteral("sftp · %1").arg(profile.displayTitle()));
}

void MainWindow::openSftpFromSession(const QString& sessionId)
{
    auto* live = m_sessions->session(sessionId);
    if (!live) {
        return;
    }
    if (live->profile.isTelnet() || live->profile.isSerial() || live->profile.isRdp()) {
        QMessageBox::information(this, QStringLiteral("sftp"),
                                 QStringLiteral("SFTP is not available for this session type."));
        return;
    }
    if (!live->connected) {
        QMessageBox::information(this, QStringLiteral("sftp"),
                                 QStringLiteral("Connect the session before opening SFTP."));
        return;
    }
    // Focus the session-bound pane if already open, otherwise open a fresh standalone pane
    // so that the same SSH session can own multiple SFTP views.
    if (m_workspace->hasSftp(sessionId)) {
        // Keep the first one focused, but still allow a second one on repeated requests.
        // The toolbar reuses the same action to duplicate the view, so always create new.
    }
    openStandaloneSftp(live->profile);
}

void MainWindow::createSftpPane(const QString& panelId, const SessionProfile& profile)
{
    auto* pane = new SftpWindow(panelId, profile, m_workspace);
    pane->setStyleSheet(styleSheet());
    m_sftpPanes.insert(panelId, pane);
    connect(pane, &SftpWindow::debugLog, this, [this](const QString& line) {
        m_dashboard->appendLog(line);
    });
    connect(pane, &SftpWindow::windowClosed, this, [this](const QString& sid) {
        m_sftpPanes.remove(sid);
        m_workspace->removePanel(PanelRef::sftp(sid), false);
        if (!m_workspace->hasAttachedSessions()) {
            showDashboard();
        }
        m_topNav->refresh();
    });

    const QString title = QStringLiteral("sftp · %1").arg(profile.displayTitle());
    m_workspace->addSftp(panelId, pane, title);
    showWorkspace();
    m_topNav->refresh();
}

void MainWindow::openRdpSession(const SessionProfile& profile)
{
    if (profile.host.trimmed().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("rdp"),
                                 QStringLiteral("Profile needs a host to open RDP."));
        return;
    }
    const QString panelId = profile.id.trimmed().isEmpty()
                                ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                                : profile.id;
    createRdpPane(panelId, profile);
}

void MainWindow::createRdpPane(const QString& panelId, const SessionProfile& profile)
{
    auto* pane = new RdpPane(profile, m_workspace);
    pane->setStyleSheet(styleSheet());
    m_rdpPanes.insert(panelId, pane);
    connect(pane, &RdpPane::debugLog, this, [this](const QString& line) {
        m_dashboard->appendLog(line);
    });
    connect(pane, &RdpPane::windowClosed, this, [this](const QString& sid) {
        m_rdpPanes.remove(sid);
        m_workspace->removePanel(PanelRef::rdp(sid), false);
        if (!m_workspace->hasAttachedSessions()) {
            showDashboard();
        }
        m_topNav->refresh();
    });

    const QString title = QStringLiteral("rdp · %1").arg(profile.displayTitle());
    m_workspace->addRdp(panelId, pane, title);
    showWorkspace();
    m_topNav->refresh();
}

void MainWindow::closePanel(const PanelRef& ref)
{
    if (!ref.isValid()) {
        return;
    }

    if (ref.kind == PanelKind::Sftp) {
        if (QWidget* w = m_workspace->takeSftp(ref.sessionId)) {
            if (SftpWindow* sftp = m_sftpPanes.take(ref.sessionId)) {
                Q_UNUSED(w);
                sftp->deleteLater();
            } else {
                w->deleteLater();
            }
        } else if (SftpWindow* sftp = m_sftpPanes.take(ref.sessionId)) {
            sftp->deleteLater();
        }
        if (!m_workspace->hasAttachedSessions()) {
            showDashboard();
        }
        m_topNav->refresh();
        return;
    }

    if (ref.kind == PanelKind::Rdp) {
        if (QWidget* w = m_workspace->takeRdp(ref.sessionId)) {
            if (RdpPane* rdp = m_rdpPanes.take(ref.sessionId)) {
                Q_UNUSED(w);
                rdp->disconnectSession();
                rdp->deleteLater();
            } else {
                w->deleteLater();
            }
        } else if (RdpPane* rdp = m_rdpPanes.take(ref.sessionId)) {
            rdp->disconnectSession();
            rdp->deleteLater();
        }
        if (!m_workspace->hasAttachedSessions()) {
            showDashboard();
        }
        m_topNav->refresh();
        return;
    }

    closeLiveSession(ref.sessionId);
}

void MainWindow::savePanelProfile(const PanelRef& ref)
{
    if (!ref.isValid()) {
        return;
    }
    if (ref.kind == PanelKind::Terminal) {
        if (const auto* live = m_sessions->session(ref.sessionId)) {
            m_dashboard->saveSessionProfile(live->profile);
        }
        return;
    }
    if (SftpWindow* sftp = m_sftpPanes.value(ref.sessionId)) {
        m_dashboard->saveSessionProfile(sftp->profile());
        return;
    }
    if (RdpPane* rdp = m_rdpPanes.value(ref.sessionId)) {
        m_dashboard->saveSessionProfile(rdp->profile());
    }
}

void MainWindow::closeLiveSession(const QString& id)
{
    if (QWidget* sftp = m_workspace->takeSftp(id)) {
        sftp->deleteLater();
    }
    if (SftpWindow* pane = m_sftpPanes.take(id)) {
        pane->deleteLater();
    }
    if (QWidget* rdp = m_workspace->takeRdp(id)) {
        rdp->deleteLater();
    }
    if (RdpPane* rdpPane = m_rdpPanes.take(id)) {
        rdpPane->disconnectSession();
        rdpPane->deleteLater();
    }
    if (QWidget* term = m_workspace->takeTerminal(id)) {
        term->deleteLater();
    }
    m_sessions->closeSession(id);
}

void MainWindow::applyTheme()
{
    struct Palette {
        const char* windowBg;
        const char* sidebarBg;
        const char* surfaceBg;
        const char* tableBg;
        const char* headerBg;
        const char* text;
        const char* textBright;
        const char* textMuted;
        const char* textDim;
        const char* border;
        const char* borderStrong;
        const char* hoverBg;
        const char* selectedBg;
        const char* buttonBg;
        const char* buttonHover;
        const char* buttonPressed;
        const char* primaryBg;
        const char* accentChunk;
        const char* scrollHandle;
        const char* scrollTrack;
        const char* checkChecked;
    };

    static const Palette dark = {
        "#2a2a2a", "#222222", "#1e1e1e", "#1a1a1a", "#242424",
        "#b8b8b8", "#e8e8e8", "#8a8a8a", "#666666",
        "#3a3a3a", "#555555", "#2c2c2c", "#333333",
        "#333333", "#3c3c3c", "#252525", "#4a4a4a",
        "#5a5a5a", "#4f4f4f", "#262626", "#707070",
    };
    static const Palette light = {
        "#f2f2f2", "#e8e8e8", "#ffffff", "#ffffff", "#ececec",
        "#2a2a2a", "#111111", "#5a5a5a", "#777777",
        "#d0d0d0", "#b0b0b0", "#e6e6e6", "#d8d8d8",
        "#e0e0e0", "#d4d4d4", "#c8c8c8", "#c0c0c0",
        "#a8a8a8", "#b5b5b5", "#e0e0e0", "#888888",
    };

    const Palette& p = AppSettings::isLightTheme() ? light : dark;

    // Hardcoded monospace on chrome overwrote the UI font setting. When empty,
    // omit font-family so QApplication::font() (clientoshUiFont) applies.
    QString uiFontFamilyCss;
    {
        const QString family = AppSettings::uiFontFamily().trimmed();
        if (!family.isEmpty()) {
            QString escaped = family;
            escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
            escaped.replace(QLatin1Char('"'), QLatin1String("\\\""));
            uiFontFamilyCss = QStringLiteral("  font-family: \"%1\";").arg(escaped);
        }
    }

    auto fill = [&](QString qss) {
        qss.replace(QLatin1String("{{windowBg}}"), QLatin1String(p.windowBg));
        qss.replace(QLatin1String("{{sidebarBg}}"), QLatin1String(p.sidebarBg));
        qss.replace(QLatin1String("{{surfaceBg}}"), QLatin1String(p.surfaceBg));
        qss.replace(QLatin1String("{{tableBg}}"), QLatin1String(p.tableBg));
        qss.replace(QLatin1String("{{headerBg}}"), QLatin1String(p.headerBg));
        qss.replace(QLatin1String("{{text}}"), QLatin1String(p.text));
        qss.replace(QLatin1String("{{textBright}}"), QLatin1String(p.textBright));
        qss.replace(QLatin1String("{{textMuted}}"), QLatin1String(p.textMuted));
        qss.replace(QLatin1String("{{textDim}}"), QLatin1String(p.textDim));
        qss.replace(QLatin1String("{{border}}"), QLatin1String(p.border));
        qss.replace(QLatin1String("{{borderStrong}}"), QLatin1String(p.borderStrong));
        qss.replace(QLatin1String("{{hoverBg}}"), QLatin1String(p.hoverBg));
        qss.replace(QLatin1String("{{selectedBg}}"), QLatin1String(p.selectedBg));
        qss.replace(QLatin1String("{{buttonBg}}"), QLatin1String(p.buttonBg));
        qss.replace(QLatin1String("{{buttonHover}}"), QLatin1String(p.buttonHover));
        qss.replace(QLatin1String("{{buttonPressed}}"), QLatin1String(p.buttonPressed));
        qss.replace(QLatin1String("{{primaryBg}}"), QLatin1String(p.primaryBg));
        qss.replace(QLatin1String("{{accentChunk}}"), QLatin1String(p.accentChunk));
        qss.replace(QLatin1String("{{scrollHandle}}"), QLatin1String(p.scrollHandle));
        qss.replace(QLatin1String("{{scrollTrack}}"), QLatin1String(p.scrollTrack));
        qss.replace(QLatin1String("{{checkChecked}}"), QLatin1String(p.checkChecked));
        qss.replace(QLatin1String("{{uiFontFamily}}"), uiFontFamilyCss);
        return qss;
    };

    QString qss = fill(QStringLiteral(
        "QMainWindow, QWidget#dashboardPage, QWidget#sessionWorkspace, QWidget#terminalWindow, QWidget#sftpWindow {"
        "  background-color: {{windowBg}};"
        "  color: {{text}};"
        "{{uiFontFamily}}"
        "}"
        "QWidget#dashMain { background: {{windowBg}}; }"
        "QWidget#dashSidebar {"
        "  background: {{sidebarBg}};"
        "  border-right: 1px solid {{border}};"
        "}"
        "QLabel#sideBrand {"
        "  color: {{textBright}};"
        "  font-size: 14px;"
        "  font-weight: 600;"
        "  padding: 0 4px;"
        "}"
        "QLabel#sideBrandSub {"
        "  color: {{textDim}};"
        "  font-size: 10px;"
        "  padding: 0 4px 4px 4px;"
        "}"
        "QLabel#sideBadge {"
        "  color: {{textMuted}};"
        "  font-size: 10px;"
        "  padding: 0 6px 4px 6px;"
        "}"
        "QFrame#sideRule {"
        "  background: {{border}};"
        "  border: none;"
        "  max-height: 1px;"
        "}"
        "QToolButton#sideNavBtn {"
        "  background: transparent;"
        "  border: none;"
        "  color: {{textMuted}};"
        "  text-align: left;"
        "  padding: 4px 8px;"
        "  font-size: 11px;"
        "}"
        "QToolButton#sideNavBtn:checked {"
        "  background: {{selectedBg}};"
        "  color: {{textBright}};"
        "  border-left: 2px solid {{borderStrong}};"
        "  padding-left: 6px;"
        "}"
        "QWidget#dashTopBar { background: {{windowBg}}; }"
        "QLabel#dashPageTitle {"
        "  color: {{textBright}};"
        "  font-size: 13px;"
        "  font-weight: 600;"
        "}"
        "QLabel#dashPageSub {"
        "  color: {{textDim}};"
        "  font-size: 10px;"
        "}"
        "QPlainTextEdit#dashLogs {"
        "  background: {{surfaceBg}};"
        "  color: {{text}};"
        "  border: 1px solid {{border}};"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  padding: 6px;"
        "}"
        "QTextEdit#dashNotes {"
        "  background: {{surfaceBg}};"
        "  color: {{text}};"
        "  border: 1px solid {{border}};"
        "  font-size: 13px;"
        "  padding: 10px;"
        "}"
        "QWidget#splitHost { background: {{windowBg}}; }"
        "QFrame#paneFrame { background: {{windowBg}}; border: none; }"
        "QLabel#paneTitle {"
        "  color: {{textMuted}};"
        "  font-size: 10px;"
        "  background: transparent;"
        "}"
        "QSplitter::handle { background: {{border}}; }"
        "QSplitter::handle:hover { background: {{borderStrong}}; }"
        "QSplitter::handle:horizontal { width: 3px; }"
        "QSplitter::handle:vertical { height: 3px; }"
        "QWidget#sftpPathBar {"
        "  background: {{windowBg}};"
        "  border-bottom: 1px solid {{border}};"
        "}"
        "QProgressBar#sftpProgress {"
        "  background: {{surfaceBg}};"
        "  border: none;"
        "  border-top: 1px solid {{border}};"
        "  text-align: center;"
        "  color: {{textMuted}};"
        "  font-size: 10px;"
        "}"
        "QProgressBar#sftpProgress::chunk { background: {{accentChunk}}; }"
        "QWidget#sessionNav {"
        "  background-color: {{windowBg}};"
        "  border-bottom: 1px solid {{border}};"
        "  min-height: 28px;"
        "  max-height: 28px;"
        "}"
        "QLabel#dashHint { color: {{textMuted}}; font-size: 11px; }"
        "QLabel#fieldLabel { color: {{textMuted}}; font-size: 11px; }"
        "QLabel#settingsSectionTitle {"
        "  color: {{textBright}};"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "  padding-bottom: 4px;"
        "}"
        "QLabel#settingsSubsection {"
        "  color: {{textMuted}};"
        "  background: transparent;"
        "  font-size: 11px;"
        "  font-weight: 600;"
        "  padding: 2px 0 2px 0;"
        "}"
        "QFrame#settingsDivider {"
        "  background: {{borderStrong}};"
        "  border: none;"
        "  max-height: 1px;"
        "  margin: 4px 0 6px 0;"
        "}"
        "QListWidget#settingsNav {"
        "  background: {{sidebarBg}};"
        "  border: none;"
        "  border-right: none;"
        "  outline: none;"
        "  padding: 8px 6px;"
        "  font-size: 11px;"
        "}"
        "QListWidget#settingsNav::item {"
        "  color: {{textMuted}};"
        "  padding: 6px 8px;"
        "  margin: 1px 0;"
        "}"
        "QListWidget#settingsNav::item:selected {"
        "  background: {{selectedBg}};"
        "  color: {{textBright}};"
        "  border-left: 2px solid {{borderStrong}};"
        "  padding-left: 6px;"
        "}"
        "QListWidget#settingsNav::item:hover {"
        "  background: {{hoverBg}};"
        "  color: {{text}};"
        "}"
        "QWidget#settingsFooter {"
        "  background: {{windowBg}};"
        "  border-top: 1px solid {{border}};"
        "}"
        "QComboBox {"
        "  background-color: {{surfaceBg}};"
        "  color: {{text}};"
        "  border: 1px solid {{border}};"
        "  border-radius: 0;"
        "  padding: 4px 28px 4px 6px;"
        "  font-size: 12px;"
        "  min-height: 22px;"
        "}"
        "QComboBox:hover { border-color: {{borderStrong}}; }"
        "QComboBox::drop-down {"
        "  subcontrol-origin: border;"
        "  subcontrol-position: top right;"
        "  width: 22px;"
        "  border: none;"
        "  border-left: 1px solid {{border}};"
        "  background: {{buttonBg}};"
        "}"
        "QComboBox::drop-down:hover { background: {{buttonHover}}; }"
        "QComboBox::down-arrow {"
        "  image: url(:/icons/chevron-down.svg);"
        "  width: 10px;"
        "  height: 10px;"
        "}"
        "QComboBox QAbstractItemView {"
        "  background: {{surfaceBg}};"
        "  color: {{text}};"
        "  border: 1px solid {{border}};"
        "  selection-background-color: {{selectedBg}};"
        "}"
        "QLabel#settingsFontPreview {"
        "  color: {{text}};"
        "  background: {{surfaceBg}};"
        "  border: 1px solid {{border}};"
        "  padding: 8px;"
        "}"
        "QLabel#settingsTermPreview {"
        "  border: 1px solid {{border}};"
        "  padding: 8px;"
        "}"
        "QLabel#settingsAboutName {"
        "  color: {{textBright}};"
        "  font-size: 16px;"
        "  font-weight: 700;"
        "  letter-spacing: 0.5px;"
        "}"
        "QLabel#settingsAboutVersion {"
        "  color: {{textBright}};"
        "  font-size: 12px;"
        "  border: 1px solid {{border}};"
        "  background: {{surfaceBg}};"
        "  padding: 8px 10px;"
        "}"
        "QLabel#settingsAboutUpdate {"
        "  color: {{textMuted}};"
        "  font-size: 11px;"
        "}"
        "QToolButton#colorSwatchBtn {"
        "  border: 1px solid {{borderStrong}};"
        "  border-radius: 0;"
        "  padding: 2px 4px;"
        "  font-size: 10px;"
        "}"
        "QLabel#statusLabel {"
        "  color: {{textMuted}};"
        "  font-size: 10px;"
        "  padding-left: 4px;"
        "}"
        "QLabel#serverStatsLabel {"
        "  color: {{textMuted}};"
        "  font-size: 10px;"
        "  font-family: Consolas, 'Cascadia Mono', 'Courier New', monospace;"
        "  padding-left: 8px;"
        "  padding-right: 4px;"
        "}"
        "QFrame#dashTabLine {"
        "  background: {{border}};"
        "  border: none;"
        "  max-height: 1px;"
        "}"
        "QLineEdit, QSpinBox, QListWidget, QTableWidget, QPlainTextEdit {"
        "  background-color: {{surfaceBg}};"
        "  color: {{text}};"
        "  border: 1px solid {{border}};"
        "  border-radius: 0;"
        "  padding: 4px 6px;"
        "  font-size: 12px;"
        "  selection-background-color: {{selectedBg}};"
        "}"
        "QLineEdit#dashSearch { padding: 5px 8px; min-height: 18px; }"
        "QTableWidget#dashTable, QTreeWidget#dashTable {"
        "  outline: none;"
        "  gridline-color: transparent;"
        "  border: 1px solid {{border}};"
        "  background: {{tableBg}};"
        "  padding: 0;"
        "}"
        "QTreeWidget#dashTable {"
        "  show-decoration-selected: 1;"
        "  font-size: 14px;"
        "}"
        "QTableWidget#dashTable::item, QTreeWidget#dashTable::item {"
        "  padding: 8px 8px;"
        "  font-size: 14px;"
        "  border: none;"
        "  outline: none;"
        "}"
        "QTableWidget#dashTable::item:selected, QTreeWidget#dashTable::item:selected,"
        "QTreeWidget#dashTable::item:selected:active, QTreeWidget#dashTable::item:selected:!active {"
        "  background: {{selectedBg}};"
        "  color: {{textBright}};"
        "}"
        "QTreeWidget#dashTable::item:focus { border: none; outline: none; }"
        "QTableWidget#dashTable::item:hover, QTreeWidget#dashTable::item:hover { background: {{hoverBg}}; }"
        "QTreeWidget#dashTable QHeaderView::section { font-size: 10px; }"
        "QMenu, QMenu#dashContextMenu, QMenu#termContextMenu, QMenu#sftpContextMenu {"
        "  background-color: {{surfaceBg}};"
        "  color: {{text}};"
        "  border: 1px solid {{borderStrong}};"
        "  border-radius: 6px;"
        "  padding: 4px 0;"
        "  font-size: 12px;"
        "}"
        "QMenu::item {"
        "  background: transparent;"
        "  padding: 6px 28px 6px 16px;"
        "}"
        "QMenu::item:selected {"
        "  background: {{hoverBg}};"
        "  color: {{textBright}};"
        "}"
        "QMenu::item:disabled { color: {{textDim}}; }"
        "QMenu::separator {"
        "  height: 1px;"
        "  background: {{border}};"
        "  margin: 4px 8px;"
        "}"
        "QMenu::indicator { width: 0; height: 0; }"
        "QHeaderView::section {"
        "  background: {{headerBg}};"
        "  color: {{textMuted}};"
        "  border: none;"
        "  border-bottom: 1px solid {{border}};"
        "  padding: 5px 8px;"
        "  font-size: 10px;"
        "  font-weight: 600;"
        "}"
        "QToolButton#dashRowAction {"
        "  background: transparent;"
        "  border: none;"
        "  padding: 2px;"
        "}"
        "QToolButton#dashRowAction:disabled { color: {{textDim}}; }"
        "QWidget#dashRowActions { background: transparent; }"
        "QLineEdit:focus, QSpinBox:focus { border: 1px solid {{borderStrong}}; }"
        "QSpinBox {"
        "  padding-right: 22px;"
        "  min-height: 22px;"
        "}"
        "QSpinBox::up-button, QSpinBox::down-button {"
        "  subcontrol-origin: border;"
        "  width: 20px;"
        "  background: {{buttonBg}};"
        "  border: none;"
        "  border-left: 1px solid {{border}};"
        "}"
        "QSpinBox::up-button {"
        "  subcontrol-position: top right;"
        "  border-bottom: 1px solid {{border}};"
        "}"
        "QSpinBox::down-button {"
        "  subcontrol-position: bottom right;"
        "}"
        "QSpinBox::up-button:hover, QSpinBox::down-button:hover {"
        "  background: {{buttonHover}};"
        "}"
        "QSpinBox::up-button:pressed, QSpinBox::down-button:pressed {"
        "  background: {{buttonPressed}};"
        "}"
        "QSpinBox::up-arrow {"
        "  image: url(:/icons/chevron-up.svg);"
        "  width: 9px;"
        "  height: 9px;"
        "}"
        "QSpinBox::down-arrow {"
        "  image: url(:/icons/chevron-down.svg);"
        "  width: 9px;"
        "  height: 9px;"
        "}"
        "QPushButton, QToolButton#navIconBtn {"
        "  background-color: {{buttonBg}};"
        "  color: {{text}};"
        "  border: 1px solid {{border}};"
        "  border-radius: 0;"
        "  padding: 3px 10px;"
        "  font-size: 11px;"
        "}"
        "QPushButton, QToolButton, QCheckBox, QRadioButton { cursor: pointing-hand; }"
        "QPushButton#dashButton {"
        "  background-color: {{buttonBg}};"
        "  border: 1px solid {{border}};"
        "  color: {{text}};"
        "}"
        "QToolButton#navIconBtn { padding: 3px; border: none; background: transparent; }"
        "QToolButton#navIconBtn:checked { background-color: {{buttonHover}}; border: 1px solid {{borderStrong}}; }"
        "QPushButton#dashPrimary {"
        "  background-color: {{primaryBg}};"
        "  border-color: {{borderStrong}};"
        "  color: {{textBright}};"
        "  padding: 4px 12px;"
        "}"
        "QPushButton:hover { background-color: {{buttonHover}}; border-color: {{borderStrong}}; }"
        "QPushButton:pressed { background-color: {{buttonPressed}}; }"
        "QPushButton:disabled { color: {{textDim}}; border-color: {{border}}; background: {{windowBg}}; }"
        "QCheckBox { color: {{text}}; spacing: 8px; font-size: 11px; }"
        "QCheckBox::indicator {"
        "  width: 16px; height: 16px;"
        "  border: 2px solid {{borderStrong}};"
        "  border-radius: 3px;"
        "  background: {{surfaceBg}};"
        "}"
        "QCheckBox::indicator:hover { border-color: {{accentChunk}}; background: {{hoverBg}}; }"
        "QCheckBox::indicator:checked {"
        "  background: {{checkChecked}};"
        "  border: 2px solid {{checkChecked}};"
        "  image: url(:/icons/check-white.svg);"
        "}"
        "QCheckBox::indicator:checked:hover { background: {{primaryBg}}; border-color: {{primaryBg}}; }"
        "QCheckBox::indicator:disabled {"
        "  border-color: {{border}};"
        "  background: {{tableBg}};"
        "}"
        "QCheckBox::indicator:checked:disabled {"
        "  background: {{borderStrong}};"
        "  border-color: {{borderStrong}};"
        "  image: url(:/icons/check-disabled.svg);"
        "}"
        "QCheckBox:disabled { color: {{textDim}}; }"
        "QFrame#sessionChip {"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QPushButton#sessionChipBtn {"
        "  background: transparent;"
        "  border: none;"
        "  color: {{text}};"
        "  padding: 1px 3px;"
        "  font-size: 11px;"
        "  text-align: left;"
        "}"
        "QToolButton#sessionChipClose {"
        "  background: transparent;"
        "  border: none;"
        "  padding: 1px;"
        "}"
        "QScrollBar#termScrollBar:vertical {"
        "  background: {{scrollTrack}};"
        "  width: 10px;"
        "  margin: 0;"
        "  border: none;"
        "}"
        "QScrollBar#termScrollBar::handle:vertical {"
        "  background: {{scrollHandle}};"
        "  min-height: 24px;"
        "  border: none;"
        "}"
        "QScrollBar#termScrollBar::handle:vertical:hover { background: {{borderStrong}}; }"
        "QScrollBar#termScrollBar::add-line:vertical,"
        "QScrollBar#termScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar#termScrollBar::add-page:vertical,"
        "QScrollBar#termScrollBar::sub-page:vertical { background: transparent; }"
    ));

    // The stylesheet contains deliberate relative size differences, but fixed
    // pixel values used to make the UI font-size setting ineffective. Scale all
    // of those values from the original 10 pt baseline.
    const qreal fontScale = qreal(AppSettings::uiFontSize()) / 10.0;
    const QRegularExpression fontSizePattern(QStringLiteral("font-size:\\s*(\\d+)px"));
    qsizetype offset = 0;
    while (true) {
        const QRegularExpressionMatch match = fontSizePattern.match(qss, offset);
        if (!match.hasMatch()) {
            break;
        }
        const int scaled = qMax(1, qRound(match.captured(1).toInt() * fontScale));
        const QString replacement = QStringLiteral("font-size: %1px").arg(scaled);
        qss.replace(match.capturedStart(), match.capturedLength(), replacement);
        offset = match.capturedStart() + replacement.size();
    }
    setStyleSheet(qss);
}
