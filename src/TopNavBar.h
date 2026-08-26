#pragma once

#include "PanelTypes.h"
#include "core/ServerStatsClient.h"
#include "core/SessionProfile.h"

#include <QIcon>
#include <QWidget>
#include <QStringList>

class SessionManager;
class SessionWorkspace;
class QHBoxLayout;
class QLabel;
class QToolButton;
class QThread;
class QResizeEvent;

class TopNavBar : public QWidget
{
    Q_OBJECT

public:
    explicit TopNavBar(SessionManager* sessions, SessionWorkspace* workspace, QWidget* parent = nullptr);
    ~TopNavBar() override;

    void refresh();
    void syncActiveChip();
    void applySettings();
    void setAlwaysOnTopChecked(bool on);
    /** Controls tab highlighting without changing the active live session. */
    void setWorkspaceActive(bool active);
    /** Show/hide the AI agent robot toggle (only while the addon is loaded). */
    void setAiAgentAvailable(bool available, const QIcon& icon = QIcon());
    void setAiAgentPanelOpen(bool open);
    bool isAiAgentPanelOpen() const;

signals:
    void dashboardRequested();
    void newSessionRequested();
    void panelSelectRequested(const PanelRef& ref);
    void panelCloseRequested(const PanelRef& ref);
    void panelPreviewRequested(const PanelRef& ref);
    void savePanelRequested(const PanelRef& ref);
    void sftpRequested();
    void xmodemRequested(const QString& sessionId);
    void alwaysOnTopToggled(bool on);
    void aiAgentToggled(bool open);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void rebuildTabs();
    void syncStatsProbe();
    void applyStats(const ServerStats& stats);
    void updateStatsPresentation();
    bool shouldCompactStats() const;
    void clearStatsDisplay();
    void hideStatsUntilData();
    void syncXmodemAction();
    void showPanelContextMenu(const PanelRef& ref, const QPoint& globalPos);

    SessionManager* m_sessions = nullptr;
    SessionWorkspace* m_workspace = nullptr;
    QHBoxLayout* m_tabsLay = nullptr;
    QWidget* m_tabsHost = nullptr;
    QToolButton* m_menuBtn = nullptr;
    QToolButton* m_newBtn = nullptr;
    QToolButton* m_pinBtn = nullptr;
    QToolButton* m_aiAgentBtn = nullptr;
    QToolButton* m_sftpBtn = nullptr;
    QToolButton* m_xmodemBtn = nullptr;
    QLabel* m_stats = nullptr;
    QString m_statsText;
    bool m_statsHaveData = false;
    bool m_statsCompact = false;
    bool m_workspaceActive = false;
    QString m_xmodemSessionId;

    QThread* m_statsThread = nullptr;
    ServerStatsClient* m_statsClient = nullptr;
    QString m_statsSessionId;
};
