#include "SessionWorkspace.h"
#include "PaneFrame.h"
#include "core/SessionManager.h"
#include "ui/Motion.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

namespace {
Qt::Orientation edgeOrientation(DockEdge edge)
{
    return (edge == DockEdge::Left || edge == DockEdge::Right) ? Qt::Horizontal : Qt::Vertical;
}

bool edgeIsFirst(DockEdge edge)
{
    return edge == DockEdge::Left || edge == DockEdge::Top;
}

bool isSplitEdge(DockEdge edge)
{
    return edge == DockEdge::Left || edge == DockEdge::Right
        || edge == DockEdge::Top || edge == DockEdge::Bottom;
}

bool widgetInTree(QWidget* node, QWidget* root)
{
    if (!node || !root) {
        return false;
    }
    for (QWidget* w = node; w; w = w->parentWidget()) {
        if (w == root) {
            return true;
        }
    }
    return false;
}

void animateSplitOpen(QSplitter* split, bool expandingFirst)
{
    if (!split) {
        return;
    }
    // Wait one event-loop tick so the splitter has a real size after insert.
    QTimer::singleShot(0, split, [split, expandingFirst]() {
        const int span = qMax(200, split->orientation() == Qt::Horizontal ? split->width()
                                                                           : split->height());
        const int half = span / 2;
        const QList<int> from = expandingFirst ? QList<int>{2, span - 2} : QList<int>{span - 2, 2};
        const QList<int> to = {half, span - half};
        Motion::animateSplitterSizes(split, from, to, Motion::kNormalMs);
    });
}
} // namespace

SessionWorkspace::SessionWorkspace(SessionManager* sessions, QWidget* parent)
    : QWidget(parent)
    , m_sessions(sessions)
{
    setObjectName(QStringLiteral("sessionWorkspace"));
    setAcceptDrops(true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_rootHost = new QWidget(this);
    m_rootHost->setObjectName(QStringLiteral("splitHost"));
    auto* hostLay = new QVBoxLayout(m_rootHost);
    hostLay->setContentsMargins(0, 0, 0, 0);
    hostLay->setSpacing(0);
    root->addWidget(m_rootHost, 1);

    connect(m_sessions, &SessionManager::sessionActivated, this, [this](const QString& id) {
        showSession(id);
    });
}

PaneFrame* SessionWorkspace::pane(const PanelRef& ref) const
{
    return m_panes.value(ref.key(), nullptr);
}

QList<PanelRef> SessionWorkspace::openPanels() const
{
    QList<PanelRef> out;
    out.reserve(m_tabOrder.size());
    for (const QString& key : m_tabOrder) {
        out.push_back(PanelRef::fromKey(key));
    }
    return out;
}

void SessionWorkspace::notifyLayoutChanged()
{
    // Defer so tab rebuild cannot destroy a QDrag source mid-drop.
    if (m_layoutNotifyQueued) {
        return;
    }
    m_layoutNotifyQueued = true;
    QTimer::singleShot(0, this, [this]() {
        m_layoutNotifyQueued = false;
        emit layoutChanged();
    });
}

void SessionWorkspace::setRootWidget(QWidget* w)
{
    auto* lay = qobject_cast<QVBoxLayout*>(m_rootHost->layout());
    if (!lay) {
        return;
    }

    // Detach current root from the host layout without destroying it.
    if (m_layoutRoot) {
        lay->removeWidget(m_layoutRoot);
    }

    m_layoutRoot = w;
    if (w) {
        w->setParent(m_rootHost);
        lay->addWidget(w, 1);
        w->show();
    }
}

PaneFrame* SessionWorkspace::ensurePane(const PanelRef& ref, QWidget* content, const QString& title)
{
    if (PaneFrame* existing = pane(ref)) {
        if (content && existing->content() != content) {
            existing->setContent(content);
        }
        if (!title.isEmpty()) {
            existing->setTitle(title);
        }
        return existing;
    }

    auto* frame = new PaneFrame(ref, content, this);
    frame->setTitle(title);
    frame->hide();
    m_panes.insert(ref.key(), frame);
    if (!m_tabOrder.contains(ref.key())) {
        m_tabOrder.push_back(ref.key());
    }

    connect(frame, &PaneFrame::focusRequested, this, [this](const PanelRef& r) {
        showPanel(r);
        emit panelSelectRequested(r);
        if (r.kind == PanelKind::Terminal) {
            emit sessionSelectRequested(r.sessionId);
        }
    });
    connect(frame, &PaneFrame::closeRequested, this, [this, frame](const PanelRef& ref) {
        // In a split, the pane-header close button means "remove from split".
        // The session remains available as a normal tab. Closing that tab from
        // the top navigation still performs the real disconnect.
        if (qobject_cast<QSplitter*>(frame->parentWidget())) {
            unsplitPanel(ref);
        } else {
            emit panelCloseRequested(ref);
        }
    });
    connect(frame, &PaneFrame::panelDropRequested, this,
            [this](const PanelRef& moving, DockEdge edge, const PanelRef& target) {
                splitDrop(moving, edge, target);
            });
    connect(frame, &PaneFrame::headerDragFinished, this, &SessionWorkspace::flushPendingDock);

    return frame;
}

QWidget* SessionWorkspace::layoutUnitFor(PaneFrame* frame) const
{
    if (!frame) {
        return nullptr;
    }
    QWidget* unit = frame;
    // Walk up nested splitters to the outermost dock group for this panel.
    while (auto* split = qobject_cast<QSplitter*>(unit->parentWidget())) {
        unit = split;
    }
    return unit;
}

void SessionWorkspace::activatePanel(PaneFrame* frame)
{
    if (!frame) {
        return;
    }

    QWidget* unit = layoutUnitFor(frame);
    if (!unit) {
        unit = frame;
    }

    if (unit != m_layoutRoot) {
        if (m_layoutRoot) {
            auto* lay = qobject_cast<QVBoxLayout*>(m_rootHost->layout());
            if (lay) {
                lay->removeWidget(m_layoutRoot);
            }
            // Keep the previous layout unit intact (e.g. SSH+SFTP splitter) for later tabs.
            m_layoutRoot->setParent(this);
            m_layoutRoot->hide();
            m_layoutRoot = nullptr;
        }
        setRootWidget(unit);
        unit->show();
        if (auto* split = qobject_cast<QSplitter*>(unit)) {
            for (int i = 0; i < split->count(); ++i) {
                if (QWidget* child = split->widget(i)) {
                    child->show();
                }
            }
        } else {
            frame->show();
        }
    }

    focusPane(frame);
    // Do not notifyLayoutChanged here — tab hover preview must not rebuild chips mid-drag.
}

void SessionWorkspace::focusPane(PaneFrame* frame)
{
    if (!frame) {
        return;
    }
    for (PaneFrame* p : m_panes) {
        if (p) {
            p->setActive(p == frame);
        }
    }
    m_active = frame->panelRef();
    frame->show();
    if (QWidget* c = frame->content()) {
        c->setFocus(Qt::OtherFocusReason);
    }
}

void SessionWorkspace::clearOverlays()
{
    for (PaneFrame* p : m_panes) {
        if (p) {
            p->clearDropHover();
        }
    }
}

void SessionWorkspace::collapseSplitter(QSplitter* split, QWidget* remaining)
{
    if (!split || !remaining) {
        return;
    }

    const bool splitWasRoot = (split == m_layoutRoot);
    auto* parentSplit = qobject_cast<QSplitter*>(split->parentWidget());
    const int parentIdx = parentSplit ? parentSplit->indexOf(split) : -1;
    // Parked layout unit: outermost splitter parented to the workspace (hidden tab).
    const bool splitWasParked = (!splitWasRoot && !parentSplit && split->parentWidget() == this);

    // Pull remaining out before tearing down the splitter.
    remaining->setParent(nullptr);

    if (splitWasRoot) {
        m_layoutRoot = nullptr;
        split->hide();
        split->setParent(nullptr);
        setRootWidget(remaining);
        split->deleteLater();
        return;
    }

    if (parentSplit && parentIdx >= 0) {
        split->hide();
        split->setParent(nullptr);
        parentSplit->insertWidget(parentIdx, remaining);
        split->deleteLater();
        return;
    }

    split->hide();
    split->setParent(nullptr);
    if (splitWasParked) {
        // Keep the sibling as a parked tab unit — do not steal the visible root.
        remaining->setParent(this);
        remaining->hide();
    } else {
        setRootWidget(remaining);
    }
    split->deleteLater();
}

bool SessionWorkspace::extractPane(PaneFrame* frame)
{
    if (!frame) {
        return false;
    }

    auto* parentSplit = qobject_cast<QSplitter*>(frame->parentWidget());
    if (!parentSplit) {
        if (frame == m_layoutRoot) {
            auto* lay = qobject_cast<QVBoxLayout*>(m_rootHost->layout());
            if (lay) {
                lay->removeWidget(frame);
            }
            m_layoutRoot = nullptr;
        }
        frame->setParent(this);
        frame->hide();
        return true;
    }

    QWidget* sibling = nullptr;
    for (int i = 0; i < parentSplit->count(); ++i) {
        if (parentSplit->widget(i) != frame) {
            sibling = parentSplit->widget(i);
            break;
        }
    }

    frame->setParent(this);
    frame->hide();

    if (sibling) {
        collapseSplitter(parentSplit, sibling);
    } else if (parentSplit == m_layoutRoot) {
        auto* lay = qobject_cast<QVBoxLayout*>(m_rootHost->layout());
        if (lay) {
            lay->removeWidget(parentSplit);
        }
        m_layoutRoot = nullptr;
        parentSplit->deleteLater();
    } else {
        // Empty parked splitter (or orphan).
        if (parentSplit->parentWidget() == this) {
            parentSplit->setParent(nullptr);
        }
        parentSplit->deleteLater();
    }
    return true;
}

void SessionWorkspace::dockInto(PaneFrame* moving, DockEdge edge, PaneFrame* target)
{
    if (!moving || !target || moving == target || !isSplitEdge(edge)) {
        return;
    }

    // Snapshot target placement before mutating the tree.
    const bool targetIsRoot = (target == m_layoutRoot);
    auto* targetParentSplit = qobject_cast<QSplitter*>(target->parentWidget());
    const int targetIndex = targetParentSplit ? targetParentSplit->indexOf(target) : -1;

    extractPane(moving);

    // Target may have moved if extracting `moving` collapsed a splitter onto it.
    const bool targetStillRoot = (target == m_layoutRoot);
    targetParentSplit = qobject_cast<QSplitter*>(target->parentWidget());
    const int freshIndex = targetParentSplit ? targetParentSplit->indexOf(target) : -1;

    auto* newSplit = new QSplitter(edgeOrientation(edge));
    newSplit->setChildrenCollapsible(false);
    newSplit->setHandleWidth(3);
    newSplit->setOpaqueResize(true);

    if (targetStillRoot || targetIsRoot) {
        auto* lay = qobject_cast<QVBoxLayout*>(m_rootHost->layout());
        if (lay) {
            lay->removeWidget(target);
        }
        m_layoutRoot = nullptr;
    } else if (targetParentSplit && freshIndex >= 0) {
        Q_UNUSED(targetIndex);
    }

    if (edgeIsFirst(edge)) {
        newSplit->addWidget(moving);
        newSplit->addWidget(target);
    } else {
        newSplit->addWidget(target);
        newSplit->addWidget(moving);
    }
    newSplit->setSizes({1000, 1000});

    if (!targetParentSplit || targetStillRoot || targetIsRoot) {
        setRootWidget(newSplit);
    } else {
        const int insertAt = (freshIndex >= 0) ? freshIndex : targetIndex;
        if (insertAt >= 0) {
            targetParentSplit->insertWidget(insertAt, newSplit);
        } else {
            targetParentSplit->addWidget(newSplit);
        }
    }

    moving->show();
    target->show();
    animateSplitOpen(newSplit, edgeIsFirst(edge));
    focusPane(moving);
    notifyLayoutChanged();
}

void SessionWorkspace::insertPanel(PaneFrame* frame)
{
    if (!frame) {
        return;
    }
    if (!m_layoutRoot) {
        setRootWidget(frame);
        frame->show();
        focusPane(frame);
        notifyLayoutChanged();
        return;
    }

    PaneFrame* target = pane(m_active);
    if (!target || !widgetInTree(target, m_layoutRoot)) {
        target = qobject_cast<PaneFrame*>(m_layoutRoot);
    }
    if (target && target != frame) {
        dockInto(frame, DockEdge::Right, target);
        return;
    }

    QWidget* oldRoot = m_layoutRoot;
    auto* lay = qobject_cast<QVBoxLayout*>(m_rootHost->layout());
    if (lay) {
        lay->removeWidget(oldRoot);
    }
    m_layoutRoot = nullptr;

    auto* split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(3);
    split->setOpaqueResize(true);
    split->addWidget(oldRoot);
    split->addWidget(frame);
    split->setSizes({1000, 1000});
    setRootWidget(split);
    frame->show();
    animateSplitOpen(split, false);
    focusPane(frame);
    notifyLayoutChanged();
}

void SessionWorkspace::addTerminal(const QString& sessionId, QWidget* terminal, const QString& title)
{
    if (!terminal || sessionId.isEmpty()) {
        return;
    }
    const PanelRef ref = PanelRef::terminal(sessionId);
    PaneFrame* frame = ensurePane(ref, terminal, title.isEmpty() ? sessionId : title);
    // Always open as its own tab view — never auto-dock into another session's split.
    activatePanel(frame);
    notifyLayoutChanged();
}

void SessionWorkspace::addSftp(const QString& sessionId, QWidget* sftp, const QString& title)
{
    if (!sftp || sessionId.isEmpty()) {
        return;
    }
    const PanelRef ref = PanelRef::sftp(sessionId);
    PaneFrame* frame = ensurePane(ref, sftp, title);

    if (widgetInTree(frame, m_layoutRoot)) {
        focusPane(frame);
        notifyLayoutChanged();
        return;
    }

    PaneFrame* termPane = pane(PanelRef::terminal(sessionId));
    if (termPane) {
        // Bring this session's layout to front, then dock SFTP beside its terminal.
        activatePanel(termPane);
        if (!widgetInTree(frame, m_layoutRoot) && widgetInTree(termPane, m_layoutRoot)) {
            dockInto(frame, DockEdge::Right, termPane);
            return;
        }
    }

    activatePanel(frame);
    notifyLayoutChanged();
}

void SessionWorkspace::addRdp(const QString& sessionId, QWidget* rdp, const QString& title)
{
    if (!rdp || sessionId.isEmpty()) {
        return;
    }
    const PanelRef ref = PanelRef::rdp(sessionId);
    PaneFrame* frame = ensurePane(ref, rdp, title.isEmpty() ? sessionId : title);
    activatePanel(frame);
    notifyLayoutChanged();
}

void SessionWorkspace::showPanel(const PanelRef& ref)
{
    PaneFrame* frame = pane(ref);
    if (!frame) {
        return;
    }
    activatePanel(frame);
}

bool SessionWorkspace::hasSftp(const QString& sessionId) const
{
    return m_panes.contains(PanelRef::sftp(sessionId).key());
}

QString SessionWorkspace::paneTitle(const PanelRef& ref) const
{
    if (PaneFrame* f = pane(ref)) {
        return f->title();
    }
    return {};
}

QWidget* SessionWorkspace::terminalWidget(const QString& sessionId) const
{
    if (PaneFrame* f = pane(PanelRef::terminal(sessionId))) {
        return f->content();
    }
    return nullptr;
}

QWidget* SessionWorkspace::sftpWidget(const QString& sessionId) const
{
    if (PaneFrame* f = pane(PanelRef::sftp(sessionId))) {
        return f->content();
    }
    return nullptr;
}

QWidget* SessionWorkspace::takeTerminal(const QString& sessionId)
{
    const PanelRef ref = PanelRef::terminal(sessionId);
    PaneFrame* f = pane(ref);
    if (!f) {
        return nullptr;
    }
    extractPane(f);
    QWidget* content = f->takeContent();
    m_panes.remove(ref.key());
    m_tabOrder.removeAll(ref.key());
    if (m_active == ref) {
        m_active = {};
    }
    f->deleteLater();
    notifyLayoutChanged();
    return content;
}

QWidget* SessionWorkspace::takeSftp(const QString& sessionId)
{
    const PanelRef ref = PanelRef::sftp(sessionId);
    PaneFrame* f = pane(ref);
    if (!f) {
        return nullptr;
    }
    extractPane(f);
    QWidget* content = f->takeContent();
    m_panes.remove(ref.key());
    m_tabOrder.removeAll(ref.key());
    if (m_active == ref) {
        m_active = {};
    }
    f->deleteLater();
    notifyLayoutChanged();
    return content;
}

QWidget* SessionWorkspace::rdpWidget(const QString& sessionId) const
{
    if (PaneFrame* f = pane(PanelRef::rdp(sessionId))) {
        return f->content();
    }
    return nullptr;
}

QWidget* SessionWorkspace::takeRdp(const QString& sessionId)
{
    const PanelRef ref = PanelRef::rdp(sessionId);
    PaneFrame* f = pane(ref);
    if (!f) {
        return nullptr;
    }
    extractPane(f);
    QWidget* content = f->takeContent();
    m_panes.remove(ref.key());
    m_tabOrder.removeAll(ref.key());
    if (m_active == ref) {
        m_active = {};
    }
    f->deleteLater();
    notifyLayoutChanged();
    return content;
}

void SessionWorkspace::showSession(const QString& sessionId)
{
    showPanel(PanelRef::terminal(sessionId));
}

void SessionWorkspace::previewPanel(const PanelRef& ref)
{
    // Tab-hover during drag: activate target viewport. Do not rebuild tabs here.
    showPanel(ref);
}

void SessionWorkspace::removePanel(const PanelRef& ref, bool deleteContent)
{
    PaneFrame* f = pane(ref);
    if (!f) {
        return;
    }
    extractPane(f);
    QWidget* content = f->takeContent();
    m_panes.remove(ref.key());
    m_tabOrder.removeAll(ref.key());
    if (m_active == ref) {
        m_active = m_tabOrder.isEmpty() ? PanelRef{} : PanelRef::fromKey(m_tabOrder.last());
        if (m_active.isValid()) {
            showPanel(m_active);
        }
    }
    f->deleteLater();
    if (deleteContent && content) {
        content->deleteLater();
    } else if (content) {
        content->setParent(nullptr);
    }
    notifyLayoutChanged();
}

void SessionWorkspace::removeSessionPanels(const QString& sessionId)
{
    removePanel(PanelRef::sftp(sessionId), true);
    removePanel(PanelRef::rdp(sessionId), true);
    removePanel(PanelRef::terminal(sessionId), true);
}

void SessionWorkspace::splitDrop(const PanelRef& moving, DockEdge edge, const PanelRef& target)
{
    clearOverlays();
    if (!moving.isValid() || !target.isValid() || moving == target || edge == DockEdge::None) {
        m_pendingDock = false;
        return;
    }

    if (!pane(moving) || !pane(target)) {
        m_pendingDock = false;
        return;
    }

    // Queue only — applying inside QDrag::exec's nested loop reparents/destroys
    // the drag source and crashes. flushPendingDock runs after exec returns.
    m_pendingMoving = moving;
    m_pendingTarget = target;
    m_pendingEdge = edge;
    m_pendingUnsplit = {};
    m_pendingDock = true;
}

void SessionWorkspace::unsplitDrop(const PanelRef& ref)
{
    if (!ref.isValid() || !pane(ref)) {
        return;
    }
    // As with splitDrop, do not reparent the drag source until QDrag::exec exits.
    m_pendingDock = false;
    m_pendingMoving = {};
    m_pendingTarget = {};
    m_pendingEdge = DockEdge::None;
    m_pendingUnsplit = ref;
}

void SessionWorkspace::unsplitPanel(const PanelRef& ref)
{
    PaneFrame* frame = pane(ref);
    if (!frame) {
        return;
    }

    if (qobject_cast<QSplitter*>(frame->parentWidget())) {
        extractPane(frame);
    }
    activatePanel(frame);
    notifyLayoutChanged();
    emit panelSelectRequested(ref);
    if (ref.kind == PanelKind::Terminal) {
        emit sessionSelectRequested(ref.sessionId);
    }
}

bool SessionWorkspace::canSplitPanel(const PanelRef& ref) const
{
    return ref.isValid() && pane(ref) && m_panes.size() > 1;
}

bool SessionWorkspace::isPanelSplit(const PanelRef& ref) const
{
    PaneFrame* frame = pane(ref);
    return frame && qobject_cast<QSplitter*>(frame->parentWidget());
}

void SessionWorkspace::splitPanel(const PanelRef& ref, DockEdge edge)
{
    if (!canSplitPanel(ref) || !isSplitEdge(edge)) {
        return;
    }
    PaneFrame* moving = pane(ref);
    PaneFrame* target = pane(m_active);
    if (!target || target == moving) {
        // Prefer another panel already visible in the current layout.
        for (const QString& key : m_tabOrder) {
            PaneFrame* candidate = pane(PanelRef::fromKey(key));
            if (candidate && candidate != moving && widgetInTree(candidate, m_layoutRoot)) {
                target = candidate;
                break;
            }
        }
    }
    if (!target || target == moving) {
        // Otherwise use the most recently opened other tab.
        for (auto it = m_tabOrder.crbegin(); it != m_tabOrder.crend(); ++it) {
            PaneFrame* candidate = pane(PanelRef::fromKey(*it));
            if (candidate && candidate != moving) {
                target = candidate;
                break;
            }
        }
    }
    if (!target || target == moving) {
        return;
    }
    applyDock(ref, edge, target->panelRef());
}

void SessionWorkspace::flushPendingDock()
{
    if (m_pendingUnsplit.isValid()) {
        const PanelRef ref = m_pendingUnsplit;
        m_pendingUnsplit = {};
        unsplitPanel(ref);
        return;
    }
    if (!m_pendingDock) {
        return;
    }
    m_pendingDock = false;
    const PanelRef moving = m_pendingMoving;
    const PanelRef target = m_pendingTarget;
    const DockEdge edge = m_pendingEdge;
    m_pendingMoving = {};
    m_pendingTarget = {};
    m_pendingEdge = DockEdge::None;
    applyDock(moving, edge, target);
}

void SessionWorkspace::applyDock(const PanelRef& moving, DockEdge edge, const PanelRef& target)
{
    PaneFrame* movingPane = pane(moving);
    PaneFrame* targetPane = pane(target);
    if (!movingPane || !targetPane || movingPane == targetPane) {
        return;
    }
    if (!widgetInTree(targetPane, m_layoutRoot)) {
        showPanel(target);
        targetPane = pane(target);
        if (!targetPane) {
            return;
        }
    }

    if (isSplitEdge(edge)) {
        dockInto(movingPane, edge, targetPane);
    }

    emit panelSelectRequested(moving);
    if (moving.kind == PanelKind::Terminal) {
        emit sessionSelectRequested(moving.sessionId);
    }
}

bool SessionWorkspace::hasAttachedSessions() const
{
    return !m_panes.isEmpty();
}

bool SessionWorkspace::containsSession(const QString& id) const
{
    return m_panes.contains(PanelRef::terminal(id).key())
        || m_panes.contains(PanelRef::sftp(id).key());
}

bool SessionWorkspace::containsPanel(const PanelRef& ref) const
{
    return m_panes.contains(ref.key());
}

void SessionWorkspace::reorderAttached(const QString& fromId, const QString& beforeId)
{
    const QString fromKey = PanelRef::terminal(fromId).key();
    const QString beforeKey = PanelRef::terminal(beforeId).key();
    if (!m_tabOrder.contains(fromKey) || !m_tabOrder.contains(beforeKey) || fromKey == beforeKey) {
        return;
    }
    m_tabOrder.removeAll(fromKey);
    const int idx = m_tabOrder.indexOf(beforeKey);
    if (idx < 0) {
        m_tabOrder.push_back(fromKey);
    } else {
        m_tabOrder.insert(idx, fromKey);
    }
    notifyLayoutChanged();
}

PanelRef SessionWorkspace::panelFromMime(const QMimeData* mime) const
{
    if (!mime) {
        return {};
    }
    if (mime->hasFormat(QLatin1String(kClientoshPanelMime))) {
        return PanelRef::fromMime(mime->data(QLatin1String(kClientoshPanelMime)));
    }
    if (mime->hasFormat(QLatin1String(kClientoshSessionMime))) {
        return PanelRef::terminal(QString::fromUtf8(mime->data(QLatin1String(kClientoshSessionMime))));
    }
    return {};
}

void SessionWorkspace::dragEnterEvent(QDragEnterEvent* event)
{
    if (panelFromMime(event->mimeData()).isValid()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void SessionWorkspace::dragMoveEvent(QDragMoveEvent* event)
{
    if (panelFromMime(event->mimeData()).isValid()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void SessionWorkspace::dropEvent(QDropEvent* event)
{
    const PanelRef ref = panelFromMime(event->mimeData());
    if (!ref.isValid()) {
        event->ignore();
        return;
    }
    showPanel(ref);
    emit panelSelectRequested(ref);
    if (ref.kind == PanelKind::Terminal) {
        emit sessionSelectRequested(ref.sessionId);
    }
    event->acceptProposedAction();
}
