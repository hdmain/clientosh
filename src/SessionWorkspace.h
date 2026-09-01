#pragma once

#include "PanelTypes.h"

#include <QHash>
#include <QStringList>
#include <QWidget>

class SessionManager;
class PaneFrame;
class QSplitter;
class QMimeData;

/**
 * In-window split workspace for Terminal and SFTP panes.
 * Drag tabs onto a pane to dock Left/Right/Top/Bottom — no OS windows.
 */
class SessionWorkspace : public QWidget
{
    Q_OBJECT

public:
    explicit SessionWorkspace(SessionManager* sessions, QWidget* parent = nullptr);

    void addTerminal(const QString& sessionId, QWidget* terminal, const QString& title);
    void addSftp(const QString& sessionId, QWidget* sftp, const QString& title);
    void addRdp(const QString& sessionId, QWidget* rdp, const QString& title);
    bool hasSftp(const QString& sessionId) const;
    QString paneTitle(const PanelRef& ref) const;
    QWidget* terminalWidget(const QString& sessionId) const;
    QWidget* sftpWidget(const QString& sessionId) const;
    QWidget* rdpWidget(const QString& sessionId) const;
    QWidget* takeTerminal(const QString& sessionId);
    QWidget* takeSftp(const QString& sessionId);
    QWidget* takeRdp(const QString& sessionId);

    void showPanel(const PanelRef& ref);
    void showSession(const QString& sessionId); // focuses terminal pane for session
    void previewPanel(const PanelRef& ref);    // tab-hover during drag
    void removePanel(const PanelRef& ref, bool deleteContent = false);
    void removeSessionPanels(const QString& sessionId);

    void splitDrop(const PanelRef& moving, DockEdge edge, const PanelRef& target);
    /** Queue moving a split pane back to a normal standalone tab. */
    void unsplitDrop(const PanelRef& ref);
    /** Move a split pane back to a standalone tab without closing its session. */
    void unsplitPanel(const PanelRef& ref);
    bool canSplitPanel(const PanelRef& ref) const;
    bool isPanelSplit(const PanelRef& ref) const;
    /** Split a panel relative to the active/nearest other panel. */
    void splitPanel(const PanelRef& ref, DockEdge edge);
    /** Apply a dock queued by splitDrop — call after QDrag::exec returns. */
    void flushPendingDock();

    bool hasAttachedSessions() const;
    bool containsSession(const QString& id) const;
    bool containsPanel(const PanelRef& ref) const;
    void reorderAttached(const QString& fromId, const QString& beforeId);
    QStringList attachedOrder() const { return m_tabOrder; }
    QList<PanelRef> openPanels() const;
    PanelRef activePanel() const { return m_active; }

signals:
    void sessionSelectRequested(const QString& id);
    void panelSelectRequested(const PanelRef& ref);
    void panelCloseRequested(const PanelRef& ref);
    void layoutChanged();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    PaneFrame* pane(const PanelRef& ref) const;
    PaneFrame* ensurePane(const PanelRef& ref, QWidget* content, const QString& title);
    /** Outermost splitter containing frame, or the frame itself if not docked. */
    QWidget* layoutUnitFor(PaneFrame* frame) const;
    /** Show frame's layout unit as the workspace root (tab-style), hiding the previous unit. */
    void activatePanel(PaneFrame* frame);
    void setRootWidget(QWidget* w);
    void insertPanel(PaneFrame* frame);
    void dockInto(PaneFrame* moving, DockEdge edge, PaneFrame* target);
    bool extractPane(PaneFrame* frame);
    void collapseSplitter(QSplitter* split, QWidget* remaining);
    void focusPane(PaneFrame* frame);
    void clearOverlays();
    void applyDock(const PanelRef& moving, DockEdge edge, const PanelRef& target);
    PanelRef panelFromMime(const QMimeData* mime) const;
    void notifyLayoutChanged();

    SessionManager* m_sessions = nullptr;
    QWidget* m_rootHost = nullptr; // layout container
    QWidget* m_layoutRoot = nullptr;
    QHash<QString, PaneFrame*> m_panes; // PanelRef::key → frame
    QStringList m_tabOrder;             // panel keys
    PanelRef m_active;
    bool m_layoutNotifyQueued = false;

    PanelRef m_pendingMoving;
    PanelRef m_pendingTarget;
    PanelRef m_pendingUnsplit;
    DockEdge m_pendingEdge = DockEdge::None;
    bool m_pendingDock = false;
};
