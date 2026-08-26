#include "DashboardPage.h"
#include "PlatformFonts.h"
#include "version.h"
#include "core/AppSettings.h"
#include "core/FontManager.h"
#include "core/SessionManager.h"
#include "core/SerialSession.h"
#include "core/sync/SyncConfig.h"
#include "core/sync/SyncController.h"
#include "core/sync/SyncKey.h"
#include "core/sync/SyncPayload.h"
#include "core/addons/AddonConfig.h"
#include "core/addons/AddonHost.h"
#include "core/addons/AddonStore.h"
#include "core/addons/AddonTypes.h"
#include "core/VaultManager.h"
#include "core/UpdateCheck.h"
#include "ui/Motion.h"

#include <libssh/libssh.h>

#include <QAbstractItemView>
#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QFrame>
#include <QSysInfo>
#include <QUuid>
#include <QHeaderView>
#include <QHoverEvent>
#include <QTreeWidget>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QTableWidget>
#include <QToolButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace {
constexpr int kHostFolderStateRole = Qt::UserRole + 2;

QString hostTagFolderKey(const QString& tagName)
{
    return tagName.isEmpty() ? QStringLiteral("folder:untagged")
                             : QStringLiteral("folder:tag:%1").arg(tagName);
}

QString hostLiveFolderKey()
{
    return QStringLiteral("folder:current-sessions");
}

class HostTreeWidget final : public QTreeWidget
{
    struct DropLocation {
        QTreeWidgetItem* folder = nullptr;
        QTreeWidgetItem* anchor = nullptr;
        bool before = false;
        QString beforeProfileId;
    };

public:
    using ProfileDropHandler =
        std::function<void(const QString&, const QString&, const QString&)>;

    explicit HostTreeWidget(QWidget* parent = nullptr)
        : QTreeWidget(parent)
    {
        setDragEnabled(false);
        setDropIndicatorShown(false);
        viewport()->setMouseTracking(true);
    }

    void setProfileDropHandler(ProfileDropHandler handler)
    {
        m_profileDropHandler = std::move(handler);
    }

protected:
    bool viewportEvent(QEvent* event) override
    {
        if (m_dragging
            && (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverMove)) {
            event->accept();
            return true;
        }
        return QTreeWidget::viewportEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        QTreeWidget::mousePressEvent(event);
        m_dragProfileId.clear();
        if (event->button() != Qt::LeftButton) {
            return;
        }

        QTreeWidgetItem* item = itemAt(event->position().toPoint());
        if (!isSavedHost(item)) {
            return;
        }
        m_dragStartPosition = event->position().toPoint();
        m_dragProfileId = item->data(0, Qt::UserRole).toString();
        const QRect rowRect = visualItemRect(item);
        m_dragHotSpot = QPoint(m_dragStartPosition.x(), m_dragStartPosition.y() - rowRect.top());
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        const QPoint position = event->position().toPoint();
        if (!(event->buttons() & Qt::LeftButton)) {
            if (m_dragging) {
                finishInternalDrag();
            }
            QTreeWidget::mouseMoveEvent(event);
            return;
        }

        if (!m_dragging) {
            if (m_dragProfileId.isEmpty()
                || (position - m_dragStartPosition).manhattanLength()
                    < QApplication::startDragDistance()) {
                QTreeWidget::mouseMoveEvent(event);
                return;
            }

            QTreeWidgetItem* item = findSavedHost(m_dragProfileId);
            if (!item) {
                m_dragProfileId.clear();
                return;
            }

            QRect rowRect = visualItemRect(item);
            rowRect.setLeft(0);
            rowRect.setRight(viewport()->width() - 1);
            m_dragPixmap = viewport()->grab(rowRect);
            m_dragging = true;
            viewport()->setCursor(Qt::ClosedHandCursor);

            // QAbstractItemView stores the hovered index independently of the
            // item delegate. Clear that state and suppress subsequent hover
            // events in viewportEvent() until the drag finishes.
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
            QHoverEvent hoverLeave(QEvent::HoverLeave, QPointF(-1, -1), QPointF(-1, -1),
                                   QPointF(position));
#else
            // Qt 6.2 (used by the Ubuntu 22.04 package build) only provides
            // the legacy position/old-position constructor.
            QHoverEvent hoverLeave(QEvent::HoverLeave, QPointF(-1, -1),
                                   QPointF(position));
#endif
            QTreeWidget::viewportEvent(&hoverLeave);
        }

        m_dragPosition = position;
        const DropLocation location = dropLocationAt(position, m_dragProfileId);
        if (location.folder && wouldChangeOrder(m_dragProfileId, location)) {
            setDropTarget(location);
        } else {
            clearDropTarget();
        }
        viewport()->update();
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_dragging) {
            const QString profileId = m_dragProfileId;
            const DropLocation location = dropLocationAt(event->position().toPoint(), profileId);
            const bool canMove = location.folder && wouldChangeOrder(profileId, location);
            const QString targetTag = canMove
                ? location.folder->data(0, Qt::UserRole).toString()
                : QString();
            const QString beforeProfileId = canMove ? location.beforeProfileId : QString();

            finishInternalDrag();
            event->accept();
            if (canMove && m_profileDropHandler) {
                m_profileDropHandler(profileId, targetTag, beforeProfileId);
            }
            return;
        }

        m_dragProfileId.clear();
        QTreeWidget::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent* event) override
    {
        QTreeWidget::paintEvent(event);
        QPainter painter(viewport());
        if (m_dropFolder) {
            QColor outline = palette().color(QPalette::Highlight);
            outline.setAlpha(210);

            if (m_dropAnchor) {
                const QRect anchorRect = visualItemRect(m_dropAnchor);
                const int y = m_dropBefore ? anchorRect.top() : anchorRect.bottom();
                painter.setPen(QPen(outline, 2));
                painter.drawLine(1, y, viewport()->width() - 2, y);
            } else {
                QRect targetRect = visualItemRect(m_dropFolder);
                targetRect.setLeft(1);
                targetRect.setRight(viewport()->width() - 2);
                targetRect.adjust(0, 1, 0, -1);
                QColor fill = palette().color(QPalette::Highlight);
                fill.setAlpha(44);
                painter.fillRect(targetRect, fill);
                painter.setPen(QPen(outline, 1));
                painter.drawRect(targetRect.adjusted(0, 0, -1, -1));
            }
        }

        if (m_dragging && !m_dragPixmap.isNull()) {
            painter.setOpacity(0.68);
            painter.drawPixmap(m_dragPosition - m_dragHotSpot, m_dragPixmap);
        }
    }

private:
    static bool isSavedHost(const QTreeWidgetItem* item)
    {
        return item && item->parent()
            && item->data(0, Qt::UserRole + 1).toString() != QLatin1String("live")
            && !item->data(0, Qt::UserRole).toString().isEmpty();
    }

    QTreeWidgetItem* findSavedHost(const QString& profileId) const
    {
        for (int i = 0; i < topLevelItemCount(); ++i) {
            QTreeWidgetItem* folder = topLevelItem(i);
            for (int c = 0; c < folder->childCount(); ++c) {
                QTreeWidgetItem* child = folder->child(c);
                if (isSavedHost(child)
                    && child->data(0, Qt::UserRole).toString() == profileId) {
                    return child;
                }
            }
        }
        return nullptr;
    }

    DropLocation dropLocationAt(const QPoint& position, const QString& profileId) const
    {
        QTreeWidgetItem* item = itemAt(position);
        if (!item) {
            return {};
        }

        QTreeWidgetItem* folder = item->parent() ? item->parent() : item;
        if (folder->data(0, Qt::UserRole + 1).toString() == QLatin1String("live-header")
            || folder->data(0, kHostFolderStateRole).toString().isEmpty()) {
            return {};
        }

        DropLocation location;
        location.folder = folder;
        if (!item->parent()) {
            return location; // dropping on a folder header appends to that folder
        }
        if (!isSavedHost(item)) {
            return {};
        }

        location.anchor = item;
        const QRect anchorRect = visualItemRect(item);
        location.before = position.y() < anchorRect.center().y();
        if (location.before) {
            location.beforeProfileId = item->data(0, Qt::UserRole).toString();
        } else {
            const int anchorIndex = folder->indexOfChild(item);
            for (int i = anchorIndex + 1; i < folder->childCount(); ++i) {
                QTreeWidgetItem* next = folder->child(i);
                const QString nextId = next->data(0, Qt::UserRole).toString();
                if (isSavedHost(next) && nextId != profileId) {
                    location.beforeProfileId = nextId;
                    break;
                }
            }
        }
        return location;
    }

    QString currentTagForProfile(const QString& profileId) const
    {
        if (QTreeWidgetItem* item = findSavedHost(profileId)) {
            return item->parent()->data(0, Qt::UserRole).toString();
        }
        return {};
    }

    bool wouldChangeOrder(const QString& profileId, const DropLocation& location) const
    {
        const QString sourceTag = currentTagForProfile(profileId);
        const QString targetTag = location.folder->data(0, Qt::UserRole).toString();
        if (sourceTag != targetTag) {
            return true;
        }

        QStringList currentOrder;
        for (int i = 0; i < location.folder->childCount(); ++i) {
            QTreeWidgetItem* child = location.folder->child(i);
            if (isSavedHost(child)) {
                currentOrder.append(child->data(0, Qt::UserRole).toString());
            }
        }
        QStringList reordered = currentOrder;
        reordered.removeAll(profileId);
        const int beforeIndex = location.beforeProfileId.isEmpty()
            ? reordered.size()
            : reordered.indexOf(location.beforeProfileId);
        if (beforeIndex < 0) {
            return false;
        }
        reordered.insert(beforeIndex, profileId);
        return reordered != currentOrder;
    }

    void setDropTarget(const DropLocation& location)
    {
        if (m_dropFolder == location.folder && m_dropAnchor == location.anchor
            && m_dropBefore == location.before) {
            return;
        }
        m_dropFolder = location.folder;
        m_dropAnchor = location.anchor;
        m_dropBefore = location.before;
        viewport()->update();
    }

    void clearDropTarget()
    {
        m_dropFolder = nullptr;
        m_dropAnchor = nullptr;
        m_dropBefore = false;
        viewport()->update();
    }

    void finishInternalDrag()
    {
        m_dragging = false;
        m_dragPixmap = QPixmap();
        m_dragProfileId.clear();
        viewport()->unsetCursor();
        clearDropTarget();
    }

    QPoint m_dragStartPosition;
    QPoint m_dragHotSpot;
    QPoint m_dragPosition;
    QPixmap m_dragPixmap;
    QString m_dragProfileId;
    bool m_dragging = false;
    QTreeWidgetItem* m_dropFolder = nullptr;
    QTreeWidgetItem* m_dropAnchor = nullptr;
    bool m_dropBefore = false;
    ProfileDropHandler m_profileDropHandler;
};

int detectPrivateKeyPassphrase(const char* /*prompt*/, char* /*buf*/, size_t /*len*/,
                               int /*echo*/, int /*verify*/, void* userdata)
{
    *static_cast<bool*>(userdata) = true;
    return -1; // Detection only; the UI asks for the passphrase explicitly.
}

void wipeBytes(QByteArray& bytes)
{
    if (!bytes.isNull()) {
        bytes.fill('\0');
        bytes.clear();
    }
}

QToolButton* makeSidebarNav(const QString& iconPath, const QString& text, QWidget* parent)
{
    auto* btn = new Motion::HoverFillButton(parent);
    btn->setText(text);
    btn->setIcon(QIcon(iconPath));
    btn->setIconSize(QSize(14, 14));
    btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    btn->setCheckable(true);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setObjectName(QStringLiteral("sideNavBtn"));
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn->setFixedHeight(28);
    btn->setHoverFill(QColor(0x2c, 0x2c, 0x2c));
    return btn;
}

QToolButton* makeRowIcon(const QString& iconPath, const QString& tip, QWidget* parent)
{
    auto* btn = new Motion::HoverFillButton(parent);
    btn->setIcon(QIcon(iconPath));
    btn->setIconSize(QSize(13, 13));
    btn->setToolTip(tip);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setObjectName(QStringLiteral("dashRowAction"));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFixedSize(22, 22);
    btn->setHoverFill(QColor(0x3c, 0x3c, 0x3c));
    return btn;
}

class HoverRowDelegate final : public QStyledItemDelegate
{
public:
    explicit HoverRowDelegate(QTableWidget* table)
        : QStyledItemDelegate(table)
        , m_table(table)
    {
        table->viewport()->setMouseTracking(true);
        table->viewport()->installEventFilter(this);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyleOptionViewItem rowOption(option);
        // Selection has its own stronger visual state. Otherwise make every
        // cell in the hovered row share the hover state.
        if (!(rowOption.state & QStyle::State_Selected) && index.row() == m_hoveredRow) {
            rowOption.state |= QStyle::State_MouseOver;
        } else {
            rowOption.state &= ~QStyle::State_MouseOver;
        }
        QStyledItemDelegate::paint(painter, rowOption, index);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (m_table && watched == m_table->viewport()) {
            int row = m_hoveredRow;
            if (event->type() == QEvent::MouseMove) {
                const auto* mouseEvent = static_cast<QMouseEvent*>(event);
                row = m_table->indexAt(mouseEvent->position().toPoint()).row();
            } else if (event->type() == QEvent::Leave || event->type() == QEvent::Hide) {
                row = -1;
            }
            if (row != m_hoveredRow) {
                m_hoveredRow = row;
                m_table->viewport()->update();
            }
        }
        return QStyledItemDelegate::eventFilter(watched, event);
    }

private:
    QTableWidget* m_table = nullptr;
    int m_hoveredRow = -1;
};

void styleTable(QTableWidget* table)
{
    table->setObjectName(QStringLiteral("dashTable"));
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(34);
    table->horizontalHeader()->setStretchLastSection(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    table->setFocusPolicy(Qt::NoFocus);
    table->setAlternatingRowColors(false);
    table->setWordWrap(false);
    table->setIconSize(QSize(14, 14));
}
}

DashboardPage::DashboardPage(SessionManager* sessions, QWidget* parent)
    : QWidget(parent)
    , m_sessions(sessions)
{
    setObjectName(QStringLiteral("dashboardPage"));
    m_profiles = loadProfiles();

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- Sidebar ----
    m_sidebar = new QWidget(this);
    m_sidebar->setObjectName(QStringLiteral("dashSidebar"));
    m_sidebar->setFixedWidth(168);
    auto* sideLay = new QVBoxLayout(m_sidebar);
    sideLay->setContentsMargins(8, 12, 8, 10);
    sideLay->setSpacing(2);

    auto* brand = new QLabel(QStringLiteral("clientosh"), m_sidebar);
    brand->setObjectName(QStringLiteral("sideBrand"));
    sideLay->addWidget(brand);

    auto* brandSub = new QLabel(QStringLiteral("ssh client"), m_sidebar);
    brandSub->setObjectName(QStringLiteral("sideBrandSub"));
    sideLay->addWidget(brandSub);
    sideLay->addSpacing(12);

    m_navGroup = new QButtonGroup(this);
    m_navGroup->setExclusive(true);

    m_navHosts = makeSidebarNav(QStringLiteral(":/icons/hosts.svg"), QStringLiteral("Hosts"), m_sidebar);
    m_navKeys = makeSidebarNav(QStringLiteral(":/icons/key.svg"), QStringLiteral("SSH Keys"), m_sidebar);
    m_navLogs = makeSidebarNav(QStringLiteral(":/icons/logs.svg"), QStringLiteral("Logs"), m_sidebar);
    m_navSettings = makeSidebarNav(QStringLiteral(":/icons/settings.svg"), QStringLiteral("Settings"), m_sidebar);

    m_navGroup->addButton(m_navHosts, static_cast<int>(NavPage::Hosts));
    m_navGroup->addButton(m_navKeys, static_cast<int>(NavPage::Keychain));
    m_navGroup->addButton(m_navLogs, static_cast<int>(NavPage::Logs));
    m_navGroup->addButton(m_navSettings, static_cast<int>(NavPage::Settings));

    sideLay->addWidget(m_navHosts);

    m_activeBadge = new QLabel(QStringLiteral(""), m_sidebar);
    m_activeBadge->setObjectName(QStringLiteral("sideBadge"));
    m_activeBadge->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_activeBadge->hide();
    sideLay->addWidget(m_activeBadge);

    sideLay->addSpacing(6);
    auto* sideRule = new QFrame(m_sidebar);
    sideRule->setObjectName(QStringLiteral("sideRule"));
    sideRule->setFrameShape(QFrame::HLine);
    sideRule->setFixedHeight(1);
    sideLay->addWidget(sideRule);
    sideLay->addSpacing(6);

    sideLay->addWidget(m_navKeys);
    sideLay->addWidget(m_navLogs);
    sideLay->addStretch(1);
    sideLay->addWidget(m_navSettings);

    root->addWidget(m_sidebar);

    // ---- Main column ----
    auto* mainCol = new QWidget(this);
    mainCol->setObjectName(QStringLiteral("dashMain"));
    auto* mainLay = new QVBoxLayout(mainCol);
    mainLay->setContentsMargins(0, 0, 0, 0);
    mainLay->setSpacing(0);

    m_topBar = new QWidget(mainCol);
    m_topBar->setObjectName(QStringLiteral("dashTopBar"));
    m_topBar->setFixedHeight(44);
    auto* topLay = new QHBoxLayout(m_topBar);
    topLay->setContentsMargins(16, 6, 16, 6);
    topLay->setSpacing(10);

    auto* titleCol = new QVBoxLayout;
    titleCol->setContentsMargins(0, 0, 0, 0);
    titleCol->setSpacing(0);
    m_pageTitle = new QLabel(QStringLiteral("Hosts"), m_topBar);
    m_pageTitle->setObjectName(QStringLiteral("dashPageTitle"));
    m_pageSub = new QLabel(QStringLiteral("saved connection profiles"), m_topBar);
    m_pageSub->setObjectName(QStringLiteral("dashPageSub"));
    titleCol->addWidget(m_pageTitle);
    titleCol->addWidget(m_pageSub);
    topLay->addLayout(titleCol);
    topLay->addStretch(1);

    m_searchEdit = new QLineEdit(m_topBar);
    m_searchEdit->setObjectName(QStringLiteral("dashSearch"));
    m_searchEdit->setPlaceholderText(QStringLiteral("filter hosts…"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setMinimumWidth(200);
    m_searchEdit->setMaximumWidth(320);
    m_searchEdit->setFixedHeight(30);
    topLay->addWidget(m_searchEdit, 1);

    m_newSessionBtn = new QPushButton(QIcon(QStringLiteral(":/icons/plus.svg")),
                                      QStringLiteral("New Session"), m_topBar);
    m_newSessionBtn->setObjectName(QStringLiteral("dashPrimary"));
    m_newSessionBtn->setIconSize(QSize(13, 13));
    m_newSessionBtn->setFocusPolicy(Qt::NoFocus);
    m_newSessionBtn->setFixedHeight(30);
    topLay->addWidget(m_newSessionBtn);

    mainLay->addWidget(m_topBar);

    auto* topRule = new QFrame(mainCol);
    topRule->setObjectName(QStringLiteral("dashTabLine"));
    topRule->setFrameShape(QFrame::HLine);
    topRule->setFixedHeight(1);
    mainLay->addWidget(topRule);

    m_stack = new QStackedWidget(mainCol);
    mainLay->addWidget(m_stack, 1);

    // Hallmark · component: hosts table · genre: modern-minimal · theme: app palette
    // Pre-emit critique: P5 H5 E5 S5 R5 V4
    // ---- Hosts page ----
    m_hostsPage = new QWidget;
    auto* hostsLay = new QVBoxLayout(m_hostsPage);
    hostsLay->setContentsMargins(16, 12, 16, 10);
    hostsLay->setSpacing(8);

    auto* hostTree = new HostTreeWidget(m_hostsPage);
    m_savedTree = hostTree;
    m_savedTree->setObjectName(QStringLiteral("dashTable"));
    m_savedTree->setColumnCount(5);
    m_savedTree->setHeaderLabels({QStringLiteral("name"), QStringLiteral("type"), QStringLiteral("auth"),
                                  QStringLiteral("system"), QStringLiteral("")});
    for (int column = 1; column <= 3; ++column) {
        m_savedTree->headerItem()->setTextAlignment(column, Qt::AlignLeft | Qt::AlignVCenter);
    }
    m_savedTree->header()->setStretchLastSection(false);
    m_savedTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_savedTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_savedTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_savedTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_savedTree->header()->setSectionResizeMode(4, QHeaderView::Fixed);
    m_savedTree->setColumnWidth(4, 34);
    m_savedTree->setRootIsDecorated(true);
    m_savedTree->setIndentation(18);
    m_savedTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_savedTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_savedTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_savedTree->setFocusPolicy(Qt::NoFocus);
    m_savedTree->setContextMenuPolicy(Qt::CustomContextMenu);
    hostTree->setProfileDropHandler([this](const QString& profileId, const QString& tagName,
                                           const QString& beforeProfileId) {
        const int profileIndex = profileIndexById(profileId);
        if (profileIndex < 0) {
            return;
        }

        const QString profileTitle = m_profiles[profileIndex].displayTitle();
        const QString targetLabel = tagName.isEmpty() ? QStringLiteral("Untagged") : tagName;

        if (!moveProfileToTagAt(profileId, tagName, beforeProfileId)) {
            m_hint->setText(QStringLiteral("could not move %1").arg(profileTitle));
            return;
        }

        // Expand the destination so the successful move confirms itself visually.
        for (int i = 0; i < m_savedTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* folder = m_savedTree->topLevelItem(i);
            if (folder->data(0, Qt::UserRole + 1).toString() != QLatin1String("live-header")
                && folder->data(0, Qt::UserRole).toString() == tagName) {
                folder->setExpanded(true);
                break;
            }
        }
        m_hint->setText(QStringLiteral("moved %1 to %2").arg(profileTitle, targetLabel));
        appendLog(QStringLiteral("moved host %1 to %2").arg(profileTitle, targetLabel));
    });
    connect(m_savedTree, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                auto* item = m_savedTree->itemAt(pos);
                if (item && item->parent()) {
                    const QString rowKind = item->data(0, Qt::UserRole + 1).toString();
                    const QString id = item->data(0, Qt::UserRole).toString();
                    if (rowKind == QLatin1String("live")) {
                        showLiveSessionContextMenu(m_savedTree->viewport()->mapToGlobal(pos), id);
                    } else {
                        showHostContextMenu(m_savedTree->viewport()->mapToGlobal(pos), id);
                    }
                } else if (item) {
                    if (item->data(0, Qt::UserRole + 1).toString() != QLatin1String("live-header")) {
                        const QString tag = item->data(0, Qt::UserRole).toString();
                        showTagContextMenu(m_savedTree->viewport()->mapToGlobal(pos), tag);
                    }
                } else {
                    // Empty area → Add Tag
                    showPageContextMenu(m_savedTree->viewport()->mapToGlobal(pos));
                }
            });
    connect(m_savedTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* it) {
        if (it->parent() == nullptr) {
            const bool liveFolder = it->data(0, Qt::UserRole + 1).toString()
                == QLatin1String("live-header");
            m_tagCollapsed.removeAll(it->data(0, kHostFolderStateRole).toString());
            // Remove entries written by older versions, which stored raw tag names.
            if (!liveFolder) {
                m_tagCollapsed.removeAll(it->data(0, Qt::UserRole).toString());
            }
            AppSettings::setTagCollapsed(m_tagCollapsed);
        }
    });
    connect(m_savedTree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* it) {
        if (it->parent() == nullptr) {
            const bool liveFolder = it->data(0, Qt::UserRole + 1).toString()
                == QLatin1String("live-header");
            const QString key = it->data(0, kHostFolderStateRole).toString();
            if (!liveFolder) {
                m_tagCollapsed.removeAll(it->data(0, Qt::UserRole).toString());
            }
            if (!key.isEmpty() && !m_tagCollapsed.contains(key)) {
                m_tagCollapsed.append(key);
            }
            AppSettings::setTagCollapsed(m_tagCollapsed);
        }
    });
    hostsLay->addWidget(m_savedTree, 1);

    m_savedEmpty = new QLabel(QStringLiteral("no hosts yet — create a session to get started"), m_hostsPage);
    m_savedEmpty->setObjectName(QStringLiteral("dashHint"));
    m_savedEmpty->setAlignment(Qt::AlignCenter);
    m_savedEmpty->hide();
    // Let the empty state take over the table's flexible area. This keeps the
    // status hint anchored at the bottom when the last host is removed.
    hostsLay->addWidget(m_savedEmpty, 1);

    m_hint = new QLabel(QStringLiteral(""), m_hostsPage);
    m_hint->setObjectName(QStringLiteral("dashHint"));
    hostsLay->addWidget(m_hint);
    m_stack->addWidget(m_hostsPage);

    // ---- SSH keys page ----
    m_keysPage = new QWidget;
    auto* keysLay = new QVBoxLayout(m_keysPage);
    keysLay->setContentsMargins(16, 12, 16, 10);
    keysLay->setSpacing(8);

    auto* keysToolbar = new QWidget(m_keysPage);
    auto* keysToolbarLay = new QHBoxLayout(keysToolbar);
    keysToolbarLay->setContentsMargins(0, 0, 0, 0);
    keysToolbarLay->setSpacing(6);
    m_agentStatus = new QLabel(m_keysPage);
    m_agentStatus->setObjectName(QStringLiteral("dashHint"));
    m_importKeyBtn = new QPushButton(QStringLiteral("Import key…"), m_keysPage);
    m_renameKeyBtn = new QPushButton(QStringLiteral("Rename"), m_keysPage);
    m_passphraseKeyBtn = new QPushButton(QStringLiteral("Passphrase…"), m_keysPage);
    m_removeKeyBtn = new QPushButton(QStringLiteral("Remove"), m_keysPage);
    for (QPushButton* button : {m_importKeyBtn, m_renameKeyBtn, m_passphraseKeyBtn, m_removeKeyBtn}) {
        button->setObjectName(QStringLiteral("dashButton"));
        button->setFocusPolicy(Qt::NoFocus);
    }
    keysToolbarLay->addWidget(m_agentStatus, 1);
    keysToolbarLay->addWidget(m_importKeyBtn);
    keysToolbarLay->addWidget(m_renameKeyBtn);
    keysToolbarLay->addWidget(m_passphraseKeyBtn);
    keysToolbarLay->addWidget(m_removeKeyBtn);
    keysLay->addWidget(keysToolbar);

    m_keysTable = new QTableWidget(0, 4, m_keysPage);
    styleTable(m_keysTable);
    m_keysTable->setItemDelegate(new HoverRowDelegate(m_keysTable));
    m_keysTable->setHorizontalHeaderLabels(
        {QStringLiteral("name"), QStringLiteral("type"), QStringLiteral("fingerprint"),
         QStringLiteral("used by")});
    m_keysTable->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_keysTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_keysTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_keysTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_keysTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    keysLay->addWidget(m_keysTable, 1);

    m_keysEmpty = new QLabel(QStringLiteral("no stored SSH keys — import one to reuse it across hosts"), m_keysPage);
    m_keysEmpty->setObjectName(QStringLiteral("dashHint"));
    m_keysEmpty->setAlignment(Qt::AlignCenter);
    // Match the Hosts empty-state geometry by filling the table area.
    keysLay->addWidget(m_keysEmpty, 1);
    m_keysStatus = new QLabel(m_keysPage);
    m_keysStatus->setObjectName(QStringLiteral("dashHint"));
    keysLay->addWidget(m_keysStatus);
    m_stack->addWidget(m_keysPage);

    // ---- Logs page ----
    m_logsPage = new QWidget;
    auto* logsLay = new QVBoxLayout(m_logsPage);
    logsLay->setContentsMargins(16, 12, 16, 10);
    logsLay->setSpacing(8);

    m_logsView = new QPlainTextEdit(m_logsPage);
    m_logsView->setObjectName(QStringLiteral("dashLogs"));
    m_logsView->setReadOnly(true);
    m_logsView->setMaximumBlockCount(500);
    m_logsView->setPlaceholderText(QStringLiteral("session events appear here…"));
    logsLay->addWidget(m_logsView, 1);

    auto* clearLogs = new QPushButton(QStringLiteral("clear"), m_logsPage);
    clearLogs->setObjectName(QStringLiteral("dashButton"));
    clearLogs->setFocusPolicy(Qt::NoFocus);
    clearLogs->setFixedWidth(72);
    auto* logsRow = new QHBoxLayout;
    logsRow->addStretch(1);
    logsRow->addWidget(clearLogs);
    logsLay->addLayout(logsRow);
    m_stack->addWidget(m_logsPage);

    // ---- Settings page (categorized) ----
    m_settingsPage = new QWidget;
    auto* setOuter = new QVBoxLayout(m_settingsPage);
    setOuter->setContentsMargins(0, 0, 0, 0);
    setOuter->setSpacing(0);

    auto* setBody = new QWidget(m_settingsPage);
    auto* setBodyLay = new QHBoxLayout(setBody);
    setBodyLay->setContentsMargins(0, 0, 0, 0);
    setBodyLay->setSpacing(0);

    m_settingsNav = new QListWidget(setBody);
    m_settingsNav->setObjectName(QStringLiteral("settingsNav"));
    m_settingsNav->setFixedWidth(148);
    m_settingsNav->setFocusPolicy(Qt::NoFocus);
    m_settingsNav->viewport()->setCursor(Qt::PointingHandCursor);
    m_settingsNav->setSpacing(1);
    m_settingsNav->addItems({QStringLiteral("General"), QStringLiteral("Appearance"),
                             QStringLiteral("Performance"), QStringLiteral("SSH / Sessions"),
                             QStringLiteral("SFTP"), QStringLiteral("Shortcuts"),
                             QStringLiteral("Sync"), QStringLiteral("Addons"),
                             QStringLiteral("About")});
    m_settingsNav->setCurrentRow(0);

    m_settingsStack = new QStackedWidget(setBody);

    auto makeScrollPage = [&](QWidget* section) {
        auto* page = new QWidget;
        auto* pageLay = new QVBoxLayout(page);
        pageLay->setContentsMargins(0, 0, 0, 0);
        pageLay->setSpacing(0);

        auto* scroll = new QScrollArea(page);
        scroll->setObjectName(QStringLiteral("settingsScroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(section);

        pageLay->addWidget(scroll);
        m_settingsStack->addWidget(page);
    };

    // General
    {
        QWidget* sec = buildSettingsSection(QStringLiteral("General"), m_settingsStack);
        m_settingsSavePassDefault = new QCheckBox(QStringLiteral("Save passwords by default for new profiles"), sec);
        m_settingsDefaultHost = new QLineEdit(sec);
        m_settingsDefaultHost->setPlaceholderText(QStringLiteral("127.0.0.1"));
        m_settingsDefaultUser = new QLineEdit(sec);
        m_settingsDefaultUser->setPlaceholderText(QStringLiteral("optional"));
        auto* gLay = qobject_cast<QVBoxLayout*>(sec->layout());
        gLay->addWidget(m_settingsSavePassDefault);
        addSettingsField(sec, QStringLiteral("Default host"), m_settingsDefaultHost);
        addSettingsField(sec, QStringLiteral("Default username"), m_settingsDefaultUser);
        gLay->addSpacing(10);

        // Scroll sensitivity slider
        m_settingsScrollSensitivity = new QSlider(Qt::Horizontal, sec);
        m_settingsScrollSensitivity->setRange(1, 20);
        m_settingsScrollSensitivity->setSingleStep(1);
        m_settingsScrollSensitivity->setPageStep(1);
        m_settingsScrollSensitivity->setFixedWidth(220);
        m_settingsScrollSensitivityValue = new QLabel(QStringLiteral("1 line"), sec);
        m_settingsScrollSensitivityValue->setObjectName(QStringLiteral("dashHint"));

        auto* scrollRow = new QWidget(sec);
        auto* scrollRowLay = new QHBoxLayout(scrollRow);
        scrollRowLay->setContentsMargins(0, 0, 0, 0);
        scrollRowLay->setSpacing(8);
        auto* scrollLab = new QLabel(QStringLiteral("Scroll Sensitivity"), scrollRow);
        scrollLab->setObjectName(QStringLiteral("fieldLabel"));
        scrollRowLay->addWidget(scrollLab);
        scrollRowLay->addStretch(1);
        scrollRowLay->addWidget(m_settingsScrollSensitivityValue);
        scrollRowLay->addWidget(m_settingsScrollSensitivity);
        gLay->addWidget(scrollRow);

        auto* scrollHint = new QLabel(
            QStringLiteral("Lines scrolled per wheel notch when reviewing terminal history."), sec);
        scrollHint->setObjectName(QStringLiteral("dashHint"));
        scrollHint->setWordWrap(true);
        gLay->addWidget(scrollHint);
        gLay->addSpacing(6);

        // Copy & Paste mode
        m_settingsCopyPaste = new QComboBox(sec);
        m_settingsCopyPaste->addItem(QStringLiteral("Standard Copy & Paste"), QStringLiteral("standard"));
        m_settingsCopyPaste->addItem(QStringLiteral("Copy & Paste Menu"), QStringLiteral("menu"));
        addSettingsField(sec, QStringLiteral("Copy & Paste"), m_settingsCopyPaste);
        auto* pasteHint = new QLabel(
            QStringLiteral("\"Copy & Paste Menu\" opens a right-click menu with Copy, Paste, and Copy & Paste. "
                           "Standard pastes on right-click."), sec);
        pasteHint->setObjectName(QStringLiteral("dashHint"));
        pasteHint->setWordWrap(true);
        gLay->addWidget(pasteHint);
        gLay->addSpacing(6);

        connect(m_settingsScrollSensitivity, &QSlider::valueChanged, this, [this](int v) {
            m_settingsScrollSensitivityValue->setText(QStringLiteral("%1 lines").arg(v));
            AppSettings::setScrollSensitivity(v);
            emit settingsApplied();
        });
        connect(m_settingsCopyPaste, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) {
                    AppSettings::setCopyPasteMode(m_settingsCopyPaste->currentData().toString());
                    emit settingsApplied();
                });

        gLay->addWidget(new QWidget(sec)); // spacing filler
        makeScrollPage(sec);
    }

    // Appearance
    {
        QWidget* sec = buildSettingsSection(QStringLiteral("Appearance"), m_settingsStack);
        auto* secLay = qobject_cast<QVBoxLayout*>(sec->layout());

        auto addSubsection = [sec, secLay](const QString& title, bool withDivider) {
            if (withDivider) {
                secLay->addSpacing(6);
                auto* div = new QFrame(sec);
                div->setObjectName(QStringLiteral("settingsDivider"));
                div->setFrameShape(QFrame::HLine);
                secLay->addWidget(div);
            }
            auto* lab = new QLabel(title, sec);
            lab->setObjectName(QStringLiteral("settingsSubsection"));
            secLay->addWidget(lab);
            secLay->addSpacing(2);
        };

        auto addFieldLabel = [sec, secLay](const QString& text) {
            auto* lab = new QLabel(text, sec);
            lab->setObjectName(QStringLiteral("fieldLabel"));
            secLay->addSpacing(4);
            secLay->addWidget(lab);
        };

        auto makeInlineRow = [sec]() {
            auto* row = new QWidget(sec);
            auto* lay = new QHBoxLayout(row);
            lay->setContentsMargins(0, 0, 0, 0);
            lay->setSpacing(8);
            return std::pair<QWidget*, QHBoxLayout*>{row, lay};
        };

        // ---- App ----
        addSubsection(QStringLiteral("App"), false);

        m_settingsTheme = new QComboBox(sec);
        m_settingsTheme->addItem(QStringLiteral("Dark"), QStringLiteral("dark"));
        m_settingsTheme->addItem(QStringLiteral("Light"), QStringLiteral("light"));
        m_settingsTheme->setMaximumWidth(220);
        addFieldLabel(QStringLiteral("Theme"));
        secLay->addWidget(m_settingsTheme);

        m_settingsUiFontFamily = new QComboBox(sec);
        m_settingsUiFontSize = new QSpinBox(sec);
        m_settingsUiFontSize->setRange(9, 22);
        m_settingsUiFontSize->setSuffix(QStringLiteral(" pt"));
        m_settingsUiFontSize->setFixedWidth(92);
        {
            auto [row, lay] = makeInlineRow();
            m_settingsUiFontFamily->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            lay->addWidget(m_settingsUiFontFamily, 1);
            lay->addWidget(m_settingsUiFontSize);
            addFieldLabel(QStringLiteral("UI font"));
            secLay->addWidget(row);
        }

        auto* uiPreview = new QLabel(QStringLiteral("Hosts · Settings · Connect"), sec);
        uiPreview->setObjectName(QStringLiteral("settingsFontPreview"));
        uiPreview->setContentsMargins(8, 8, 8, 8);
        secLay->addSpacing(4);
        secLay->addWidget(uiPreview);

        // ---- Terminal ----
        addSubsection(QStringLiteral("Terminal"), true);

        m_settingsFontFamily = new QComboBox(sec);
        m_settingsFontSize = new QSpinBox(sec);
        m_settingsFontSize->setRange(9, 22);
        m_settingsFontSize->setSuffix(QStringLiteral(" pt"));
        m_settingsFontSize->setFixedWidth(92);
        {
            auto [row, lay] = makeInlineRow();
            m_settingsFontFamily->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            lay->addWidget(m_settingsFontFamily, 1);
            lay->addWidget(m_settingsFontSize);
            addFieldLabel(QStringLiteral("Font"));
            secLay->addWidget(row);
        }

        m_settingsTermFgBtn = new QToolButton(sec);
        m_settingsTermFgBtn->setObjectName(QStringLiteral("colorSwatchBtn"));
        m_settingsTermFgBtn->setFixedSize(88, 26);
        m_settingsTermFgBtn->setFocusPolicy(Qt::NoFocus);
        m_settingsTermBgBtn = new QToolButton(sec);
        m_settingsTermBgBtn->setObjectName(QStringLiteral("colorSwatchBtn"));
        m_settingsTermBgBtn->setFixedSize(88, 26);
        m_settingsTermBgBtn->setFocusPolicy(Qt::NoFocus);
        {
            auto [row, lay] = makeInlineRow();
            auto* fgLab = new QLabel(QStringLiteral("Text"), row);
            fgLab->setObjectName(QStringLiteral("fieldLabel"));
            auto* bgLab = new QLabel(QStringLiteral("Background"), row);
            bgLab->setObjectName(QStringLiteral("fieldLabel"));
            lay->addWidget(fgLab);
            lay->addWidget(m_settingsTermFgBtn);
            lay->addSpacing(12);
            lay->addWidget(bgLab);
            lay->addWidget(m_settingsTermBgBtn);
            lay->addStretch(1);
            addFieldLabel(QStringLiteral("Colors"));
            secLay->addWidget(row);
        }

        m_settingsTermBgImage = new QLabel(QStringLiteral("None"), sec);
        m_settingsTermBgImage->setObjectName(QStringLiteral("dashHint"));
        m_settingsTermBgImage->setWordWrap(false);
        m_settingsTermBgImage->setMinimumWidth(80);
        m_settingsTermBgImageBtn = new QToolButton(sec);
        m_settingsTermBgImageBtn->setObjectName(QStringLiteral("dashSecondary"));
        m_settingsTermBgImageBtn->setText(QStringLiteral("Choose…"));
        m_settingsTermBgImageBtn->setFocusPolicy(Qt::NoFocus);
        {
            auto [row, lay] = makeInlineRow();
            m_settingsTermBgImage->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            lay->addWidget(m_settingsTermBgImage, 1);
            lay->addWidget(m_settingsTermBgImageBtn);
            addFieldLabel(QStringLiteral("Background image"));
            secLay->addWidget(row);
        }

        m_settingsTermBgOpacity = new QSpinBox(sec);
        m_settingsTermBgOpacity->setRange(0, 100);
        m_settingsTermBgOpacity->setSuffix(QStringLiteral("%"));
        m_settingsTermBgOpacity->setSingleStep(5);
        m_settingsTermBgOpacity->setFixedWidth(92);
        m_settingsTermBgBlur = new QSpinBox(sec);
        m_settingsTermBgBlur->setRange(0, 100);
        m_settingsTermBgBlur->setSuffix(QStringLiteral(" px"));
        m_settingsTermBgBlur->setSingleStep(5);
        m_settingsTermBgBlur->setFixedWidth(100);
        {
            auto [row, lay] = makeInlineRow();
            auto* opLab = new QLabel(QStringLiteral("Opacity"), row);
            opLab->setObjectName(QStringLiteral("fieldLabel"));
            auto* blurLab = new QLabel(QStringLiteral("Blur"), row);
            blurLab->setObjectName(QStringLiteral("fieldLabel"));
            lay->addWidget(opLab);
            lay->addWidget(m_settingsTermBgOpacity);
            lay->addSpacing(12);
            lay->addWidget(blurLab);
            lay->addWidget(m_settingsTermBgBlur);
            lay->addStretch(1);
            addFieldLabel(QStringLiteral("Image effects"));
            secLay->addWidget(row);
        }

        m_settingsTermPreview = new QLabel(QStringLiteral("Abc 123 → ~/src $"), sec);
        m_settingsTermPreview->setObjectName(QStringLiteral("settingsTermPreview"));
        m_settingsTermPreview->setContentsMargins(8, 8, 8, 8);
        secLay->addSpacing(4);
        secLay->addWidget(m_settingsTermPreview);

        m_settingsFontStatus = new QLabel(QStringLiteral(""), sec);
        m_settingsFontStatus->setObjectName(QStringLiteral("dashHint"));
        m_settingsFontStatus->setWordWrap(true);
        secLay->addWidget(m_settingsFontStatus);

        // ---- Highlighting ----
        addSubsection(QStringLiteral("Syntax highlighting"), true);

        m_settingsHighlightAddresses = new QCheckBox(QStringLiteral("Colorize IP and MAC addresses"), sec);
        m_settingsHighlightKeywords = new QCheckBox(
            QStringLiteral("Colorize log keywords (ERROR, WARN, OK, INFO, DEBUG)"), sec);
        m_settingsHighlightCiscoCli = new QCheckBox(
            QStringLiteral("Colorize Cisco CLI insights (interfaces, states, routing)"), sec);
        m_settingsHighlightCiscoCli->setToolTip(QStringLiteral(
            "Highlights Cisco interface names, operational states, routing protocols, "
            "configuration prompts, and common faults."));
        // Hydrate before connecting toggles so a live-persist cannot write the
        // unchecked construction defaults into the INI.
        m_settingsHighlightAddresses->setChecked(AppSettings::highlightAddresses());
        m_settingsHighlightKeywords->setChecked(AppSettings::highlightLogKeywords());
        m_settingsHighlightCiscoCli->setChecked(AppSettings::highlightCiscoCli());
        secLay->addWidget(m_settingsHighlightAddresses);
        secLay->addWidget(m_settingsHighlightKeywords);
        secLay->addWidget(m_settingsHighlightCiscoCli);

        secLay->addSpacing(10);
        auto* resetAppearanceBtn = new QPushButton(QStringLiteral("Reset appearance to defaults"), sec);
        resetAppearanceBtn->setObjectName(QStringLiteral("dashSecondary"));
        resetAppearanceBtn->setMaximumWidth(240);
        connect(resetAppearanceBtn, &QPushButton::clicked, this, [this]() {
            AppSettings::resetTerminalAppearance();
            loadSettingsUi();
            emit settingsApplied();
        });
        secLay->addWidget(resetAppearanceBtn);

        auto refreshPreviews = [this, uiPreview]() {
            uiPreview->setFont(
                clientoshUiFont(m_settingsUiFontSize->value(), m_settingsUiFontFamily->currentData().toString()));
            if (m_settingsTermPreview) {
                m_settingsTermPreview->setFont(
                    clientoshMonospaceFont(m_settingsFontSize->value(),
                                           m_settingsFontFamily->currentData().toString()));
                m_settingsTermPreview->setStyleSheet(
                    QStringLiteral("QLabel#settingsTermPreview { color: %1; background: %2; padding: 8px; }")
                        .arg(m_termFg.name(), m_termBg.name()));
            }
        };

        connect(m_settingsTheme, &QComboBox::currentIndexChanged, this, [this, refreshPreviews](int) {
            const bool light = m_settingsTheme->currentData().toString() == QLatin1String("light");
            m_termFg = AppSettings::defaultTerminalFgForTheme(light);
            m_termBg = AppSettings::defaultTerminalBgForTheme(light);
            syncColorSwatch(m_settingsTermFgBtn, m_termFg);
            syncColorSwatch(m_settingsTermBgBtn, m_termBg);
            refreshPreviews();
            persistAppearanceLive();
        });
        connect(m_settingsTermFgBtn, &QToolButton::clicked, this, [this]() { pickTerminalColor(true); });
        connect(m_settingsTermBgBtn, &QToolButton::clicked, this, [this]() { pickTerminalColor(false); });
        connect(m_settingsTermBgImageBtn, &QToolButton::clicked, this, [this]() {
            const QString path = QFileDialog::getOpenFileName(this,
                QStringLiteral("Select background image"),
                QString(),
                QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.webp);;All files (*.*)"));
            if (!path.isEmpty()) {
                m_settingsTermBgImage->setText(QFileInfo(path).fileName());
                // Path must be stored before persistAppearanceLive reads it.
                AppSettings::setTerminalBgImage(path);
                persistAppearanceLive();
            }
        });
        connect(m_settingsTermBgOpacity, qOverload<int>(&QSpinBox::valueChanged), this,
                [this](int) { persistAppearanceLive(); });
        connect(m_settingsTermBgBlur, qOverload<int>(&QSpinBox::valueChanged), this,
                [this](int) { persistAppearanceLive(); });

        connect(m_settingsUiFontFamily, &QComboBox::currentIndexChanged, this, [this, refreshPreviews](int) {
            ensureSelectedFonts();
            refreshPreviews();
            persistAppearanceLive();
        });
        connect(m_settingsFontFamily, &QComboBox::currentIndexChanged, this, [this, refreshPreviews](int) {
            ensureSelectedFonts();
            refreshPreviews();
            persistAppearanceLive();
        });
        connect(m_settingsUiFontSize, qOverload<int>(&QSpinBox::valueChanged), this,
                [this, refreshPreviews](int) {
                    refreshPreviews();
                    persistAppearanceLive();
                });
        connect(m_settingsFontSize, qOverload<int>(&QSpinBox::valueChanged), this,
                [this, refreshPreviews](int) {
                    refreshPreviews();
                    persistAppearanceLive();
                });
        connect(m_settingsHighlightAddresses, &QCheckBox::toggled, this, [this](bool on) {
            if (m_applyingAppearance) {
                return;
            }
            AppSettings::setHighlightAddresses(on);
            notifyHighlightSettingsChanged();
        });
        connect(m_settingsHighlightKeywords, &QCheckBox::toggled, this, [this](bool on) {
            if (m_applyingAppearance) {
                return;
            }
            AppSettings::setHighlightLogKeywords(on);
            notifyHighlightSettingsChanged();
        });
        connect(m_settingsHighlightCiscoCli, &QCheckBox::toggled, this, [this](bool on) {
            if (m_applyingAppearance) {
                return;
            }
            AppSettings::setHighlightCiscoCli(on);
            notifyHighlightSettingsChanged();
        });

        connect(FontManager::instance(), &FontManager::downloadStarted, this,
                [this](const QString& family) {
                    m_settingsFontStatus->setText(QStringLiteral("Downloading %1…").arg(family));
                });
        connect(FontManager::instance(), &FontManager::downloadProgress, this,
                [this](const QString& family, qint64 received, qint64 total) {
                    if (total > 0) {
                        m_settingsFontStatus->setText(
                            QStringLiteral("Downloading %1… %2%")
                                .arg(family)
                                .arg(int(100.0 * double(received) / double(total))));
                    }
                });
        connect(FontManager::instance(), &FontManager::fontReady, this,
                [this, refreshPreviews](const QString& family) {
                    if (!family.isEmpty()) {
                        m_settingsFontStatus->setText(QStringLiteral("%1 ready").arg(family));
                    } else {
                        m_settingsFontStatus->clear();
                    }
                    const QString ui = m_settingsUiFontFamily->currentData().toString();
                    const QString term = m_settingsFontFamily->currentData().toString();
                    populateFontCombos();
                    auto reselect = [](QComboBox* box, const QString& fam) {
                        const int idx = box->findData(fam);
                        box->blockSignals(true);
                        box->setCurrentIndex(idx >= 0 ? idx : 0);
                        box->blockSignals(false);
                    };
                    reselect(m_settingsUiFontFamily, ui);
                    reselect(m_settingsFontFamily, term);
                    refreshPreviews();
                });
        connect(FontManager::instance(), &FontManager::fontFailed, this,
                [this](const QString& family, const QString& error) {
                    m_settingsFontStatus->setText(QStringLiteral("%1: %2").arg(family, error));
                });

        populateFontCombos();
        m_termFg = AppSettings::terminalFg();
        m_termBg = AppSettings::terminalBg();
        syncColorSwatch(m_settingsTermFgBtn, m_termFg);
        syncColorSwatch(m_settingsTermBgBtn, m_termBg);
        refreshPreviews();
        makeScrollPage(sec);
    }

    // Performance
    {
        QWidget* sec = buildSettingsSection(QStringLiteral("Performance"), m_settingsStack);
        m_settingsAnimations = new QCheckBox(QStringLiteral("Enable UI animations"), sec);
        auto* animHint = new QLabel(
            QStringLiteral("Off = low memory mode — transitions snap and animation caches are released."),
            sec);
        animHint->setObjectName(QStringLiteral("dashHint"));
        animHint->setWordWrap(true);
        m_settingsShowStats = new QCheckBox(QStringLiteral("Show live server stats in session header"), sec);
        m_settingsStatsInterval = new QSpinBox(sec);
        m_settingsStatsInterval->setRange(1, 30);
        m_settingsStatsInterval->setSuffix(QStringLiteral(" s"));
        auto* pLay = qobject_cast<QVBoxLayout*>(sec->layout());
        pLay->addWidget(m_settingsAnimations);
        pLay->addWidget(animHint);
        pLay->addSpacing(6);
        pLay->addWidget(m_settingsShowStats);
        addSettingsField(sec, QStringLiteral("Server stats refresh interval"), m_settingsStatsInterval);
        makeScrollPage(sec);
    }

    // SSH / Sessions
    {
        QWidget* sec = buildSettingsSection(QStringLiteral("SSH / Sessions"), m_settingsStack);
        m_settingsDefaultPort = new QSpinBox(sec);
        m_settingsDefaultPort->setRange(1, 65535);
        addSettingsField(sec, QStringLiteral("Default SSH port"), m_settingsDefaultPort);
        auto* sshHint = new QLabel(QStringLiteral("Used when creating new session profiles."), sec);
        sshHint->setObjectName(QStringLiteral("dashHint"));
        qobject_cast<QVBoxLayout*>(sec->layout())->addWidget(sshHint);
        makeScrollPage(sec);
    }

    // SFTP
    {
        QWidget* sec = buildSettingsSection(QStringLiteral("SFTP"), m_settingsStack);
        m_settingsHideDotfiles = new QCheckBox(QStringLiteral("Hide dotfiles by default"), sec);
        m_settingsSftpView = new QComboBox(sec);
        m_settingsSftpView->addItem(QStringLiteral("Details (name, size, type)"), QStringLiteral("details"));
        m_settingsSftpView->addItem(QStringLiteral("Compact (name, size)"), QStringLiteral("compact"));
        m_settingsSftpVerbose = new QCheckBox(QStringLiteral("Verbose / debug logging for transfers and connection"), sec);
        auto* verboseHint = new QLabel(
            QStringLiteral("When enabled, SFTP writes detailed trace lines to Logs and to the console (qWarning). "
                           "Also enables libssh packet logging. Useful to diagnose upload/download failures — "
                           "turn off for normal use."),
            sec);
        verboseHint->setObjectName(QStringLiteral("dashHint"));
        verboseHint->setWordWrap(true);
        auto* sLay = qobject_cast<QVBoxLayout*>(sec->layout());
        sLay->addWidget(m_settingsHideDotfiles);
        addSettingsField(sec, QStringLiteral("Default file list view"), m_settingsSftpView);
        sLay->addSpacing(6);
        sLay->addWidget(m_settingsSftpVerbose);
        sLay->addWidget(verboseHint);
        connect(m_settingsSftpVerbose, &QCheckBox::toggled, this, [this](bool on) {
            AppSettings::setSftpVerboseLogging(on);
            emit settingsApplied();
            appendLog(on ? QStringLiteral("sftp verbose logging enabled")
                         : QStringLiteral("sftp verbose logging disabled"));
        });
        makeScrollPage(sec);
    }

    // Shortcuts
    {
        QWidget* sec = buildSettingsSection(QStringLiteral("Shortcuts"), m_settingsStack);
        m_settingsCtrlScrollZoom = new QCheckBox(
            QStringLiteral("Ctrl+Scroll zooms terminal font (when terminal is focused)"), sec);

        m_shortcutNewSession = new QKeySequenceEdit(sec);
        m_shortcutSettings = new QKeySequenceEdit(sec);
        m_shortcutDashboard = new QKeySequenceEdit(sec);
        m_shortcutClosePanel = new QKeySequenceEdit(sec);
        m_shortcutOpenSftp = new QKeySequenceEdit(sec);
        m_shortcutClearTerminal = new QKeySequenceEdit(sec);
        m_shortcutFontLarger = new QKeySequenceEdit(sec);
        m_shortcutFontSmaller = new QKeySequenceEdit(sec);
        m_shortcutFontReset = new QKeySequenceEdit(sec);

        m_enableNewSession = new QCheckBox(sec);
        m_enableSettings = new QCheckBox(sec);
        m_enableDashboard = new QCheckBox(sec);
        m_enableClosePanel = new QCheckBox(sec);
        m_enableOpenSftp = new QCheckBox(sec);
        m_enableClearTerminal = new QCheckBox(sec);
        m_enableFontLarger = new QCheckBox(sec);
        m_enableFontSmaller = new QCheckBox(sec);
        m_enableFontReset = new QCheckBox(sec);

        auto* sLay = qobject_cast<QVBoxLayout*>(sec->layout());
        sLay->addWidget(m_settingsCtrlScrollZoom);
        sLay->addSpacing(6);

        // Each shortcut gets an enable toggle next to its key field.
        auto addShortcutRow = [this, sLay](const QString& label, QKeySequenceEdit* edit, QCheckBox* enable) {
            auto* row = new QWidget(this);
            auto* rowLay = new QHBoxLayout(row);
            rowLay->setContentsMargins(0, 0, 0, 0);
            rowLay->setSpacing(8);
            auto* lab = new QLabel(label, row);
            lab->setObjectName(QStringLiteral("fieldLabel"));
            rowLay->addWidget(lab);
            rowLay->addStretch(1);
            rowLay->addWidget(enable);
            rowLay->addWidget(edit);
            edit->setFixedWidth(180);
            enable->setText(QStringLiteral("enabled"));
            enable->setFocusPolicy(Qt::NoFocus);

            connect(enable, &QCheckBox::toggled, this, [this, edit](bool on) {
                edit->setEnabled(on);
                if (!on) {
                    edit->clearFocus();
                }
                persistShortcutsLive();
            });
            sLay->addWidget(row);
        };
        addShortcutRow(QStringLiteral("New session"), m_shortcutNewSession, m_enableNewSession);
        addShortcutRow(QStringLiteral("Open settings"), m_shortcutSettings, m_enableSettings);
        addShortcutRow(QStringLiteral("Show dashboard"), m_shortcutDashboard, m_enableDashboard);
        addShortcutRow(QStringLiteral("Close active panel"), m_shortcutClosePanel, m_enableClosePanel);
        addShortcutRow(QStringLiteral("Open SFTP"), m_shortcutOpenSftp, m_enableOpenSftp);
        addShortcutRow(QStringLiteral("Clear active terminal"), m_shortcutClearTerminal,
                       m_enableClearTerminal);
        addShortcutRow(QStringLiteral("Terminal font larger"), m_shortcutFontLarger, m_enableFontLarger);
        addShortcutRow(QStringLiteral("Terminal font smaller"), m_shortcutFontSmaller, m_enableFontSmaller);
        addShortcutRow(QStringLiteral("Terminal font reset"), m_shortcutFontReset, m_enableFontReset);

        auto* resetBtn = new QPushButton(QStringLiteral("Reset shortcuts to defaults"), sec);
        resetBtn->setObjectName(QStringLiteral("dashButton"));
        resetBtn->setFocusPolicy(Qt::NoFocus);
        sLay->addSpacing(8);
        sLay->addWidget(resetBtn);

        auto* hint = new QLabel(
            QStringLiteral("Untick a shortcut to disable it. Click a field and press the new keys to rebind."),
            sec);
        hint->setObjectName(QStringLiteral("dashHint"));
        hint->setWordWrap(true);
        sLay->addWidget(hint);

        connect(m_settingsCtrlScrollZoom, &QCheckBox::toggled, this, [this](bool) {
            persistShortcutsLive();
        });
        const auto wireEdit = [this](QKeySequenceEdit* edit) {
            connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [this](const QKeySequence&) {
                persistShortcutsLive();
            });
        };
        wireEdit(m_shortcutNewSession);
        wireEdit(m_shortcutSettings);
        wireEdit(m_shortcutDashboard);
        wireEdit(m_shortcutClosePanel);
        wireEdit(m_shortcutOpenSftp);
        wireEdit(m_shortcutClearTerminal);
        wireEdit(m_shortcutFontLarger);
        wireEdit(m_shortcutFontSmaller);
        wireEdit(m_shortcutFontReset);
        connect(resetBtn, &QPushButton::clicked, this, &DashboardPage::resetShortcutsToDefaults);

        makeScrollPage(sec);
    }

    // Sync (GitHub Gist)
    {
        QWidget* sec = buildSettingsSection(QStringLiteral("Sync · GitHub Gist"), m_settingsStack);
        auto* sLay = qobject_cast<QVBoxLayout*>(sec->layout());
        if (!sLay) {
            makeScrollPage(sec);
        } else {
            auto* statusHint = new QLabel(
                QStringLiteral(
                    "Synchronize saved sessions between machines via a private GitHub Gist.\n"
                    "Everything is encrypted locally before upload — GitHub only stores ciphertext."),
                sec);
            statusHint->setObjectName(QStringLiteral("dashHint"));
            statusHint->setWordWrap(true);
            sLay->addWidget(statusHint);

            m_syncEnabledCheck = new QCheckBox(QStringLiteral("Enable synchronization"), sec);
            m_syncEnabledHint = new QLabel(
                QStringLiteral("When enabled, changes are encrypted and pushed to the gist; other devices "
                               "pull new revisions automatically. Uncheck to pause on this machine without "
                               "forgetting the sync key."),
                sec);
            m_syncEnabledHint->setObjectName(QStringLiteral("dashHint"));
            m_syncEnabledHint->setWordWrap(true);

            m_syncTokenEdit = new QLineEdit(sec);
            m_syncTokenEdit->setPlaceholderText(QStringLiteral("GitHub token (classic or fine-grained, `gist` scope)"));
            m_syncTokenEdit->setEchoMode(QLineEdit::Password);
            m_syncTestBtn = new QPushButton(QStringLiteral("Test token"), sec);
            m_syncTestBtn->setObjectName(QStringLiteral("dashButton"));
            m_syncTestBtn->setCursor(Qt::PointingHandCursor);
            m_syncTestBtn->setFocusPolicy(Qt::NoFocus);
            m_syncTokenStatusLabel = new QLabel(sec);
            m_syncTokenStatusLabel->setObjectName(QStringLiteral("dashHint"));
            m_syncTokenStatusLabel->setWordWrap(true);

            m_syncCreateBtn = new QPushButton(QStringLiteral("Create sync · Computer 1"), sec);
            m_syncCreateBtn->setObjectName(QStringLiteral("dashPrimary"));
            m_syncCreateBtn->setCursor(Qt::PointingHandCursor);
            m_syncCreateBtn->setFocusPolicy(Qt::NoFocus);
            m_syncCreateBtn->setIcon(QIcon(QStringLiteral(":/icons/cloud.svg")));
            m_syncCreateBtn->setIconSize(QSize(13, 13));

            // Display the current portable key (what gets shared to Computer 2)
            m_syncKeyDisplay = new QLineEdit(sec);
            m_syncKeyDisplay->setReadOnly(true);
            m_syncKeyDisplay->setObjectName(QStringLiteral("syncKeyDisplay"));
            m_syncCopyKeyBtn = new QPushButton(QStringLiteral("Copy key"), sec);
            m_syncCopyKeyBtn->setObjectName(QStringLiteral("dashButton"));
            m_syncCopyKeyBtn->setCursor(Qt::PointingHandCursor);
            m_syncCopyKeyBtn->setFocusPolicy(Qt::NoFocus);

            m_syncKeyEdit = new QLineEdit(sec);
            m_syncKeyEdit->setPlaceholderText(QStringLiteral("Paste sync key received from Computer 1"));
            m_syncJoinBtn = new QPushButton(QStringLiteral("Connect · Computer 2"), sec);
            m_syncJoinBtn->setObjectName(QStringLiteral("dashButton"));
            m_syncJoinBtn->setCursor(Qt::PointingHandCursor);
            m_syncJoinBtn->setFocusPolicy(Qt::NoFocus);

            m_syncPollInterval = new QSpinBox(sec);
            m_syncPollInterval->setRange(10, 3600);
            m_syncPollInterval->setSuffix(QStringLiteral(" s"));
            m_syncPollInterval->setValue(60);

            m_syncSyncNowBtn = new QPushButton(QStringLiteral("Sync now"), sec);
            m_syncSyncNowBtn->setObjectName(QStringLiteral("dashButton"));
            m_syncSyncNowBtn->setCursor(Qt::PointingHandCursor);
            m_syncSyncNowBtn->setFocusPolicy(Qt::NoFocus);
            m_syncSyncNowBtn->setIcon(QIcon(QStringLiteral(":/icons/sync.svg")));
            m_syncSyncNowBtn->setIconSize(QSize(13, 13));

            m_syncDisableBtn = new QPushButton(QStringLiteral("Disable synchronization"), sec);
            m_syncDisableBtn->setObjectName(QStringLiteral("dashButton"));
            m_syncDisableBtn->setCursor(Qt::PointingHandCursor);
            m_syncDisableBtn->setFocusPolicy(Qt::NoFocus);

            m_syncGistIdLabel = new QLabel(sec);
            m_syncGistIdLabel->setObjectName(QStringLiteral("dashHint"));
            m_syncGistIdLabel->setWordWrap(true);
            m_syncGistIdLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

            m_syncStatus = new QLabel(QStringLiteral("Sync is disabled. Enable it to share sessions between devices."), sec);
            m_syncStatus->setObjectName(QStringLiteral("dashHint"));
            m_syncStatus->setWordWrap(true);

            // Layout the fields
            auto* enabledRow = new QHBoxLayout;
            enabledRow->addWidget(m_syncEnabledCheck, 1);
            sLay->addSpacing(6);
            sLay->addLayout(enabledRow);
            sLay->addWidget(m_syncEnabledHint);

            auto* tokenRowTop = new QHBoxLayout;
            tokenRowTop->addWidget(m_syncTokenEdit, 1);
            tokenRowTop->addWidget(m_syncTestBtn);
            sLay->addSpacing(6);
            sLay->addWidget(new QLabel(QStringLiteral("GitHub token (kept only on this machine)"), sec));
            sLay->addLayout(tokenRowTop);
            sLay->addWidget(m_syncTokenStatusLabel);

            sLay->addSpacing(8);
            sLay->addWidget(m_syncCreateBtn);

            auto* keyDisplayRow = new QHBoxLayout;
            keyDisplayRow->addWidget(m_syncKeyDisplay, 1);
            keyDisplayRow->addWidget(m_syncCopyKeyBtn);
            sLay->addSpacing(6);
            sLay->addWidget(new QLabel(QStringLiteral("Current sync key — copy this to Computer 2"), sec));
            sLay->addLayout(keyDisplayRow);

            auto* keyInputRow = new QHBoxLayout;
            keyInputRow->addWidget(m_syncKeyEdit, 1);
            keyInputRow->addWidget(m_syncJoinBtn);
            sLay->addSpacing(8);
            sLay->addWidget(new QLabel(QStringLiteral("Join from Computer 2 (paste the key from Computer 1)"), sec));
            sLay->addLayout(keyInputRow);

            addSettingsField(sec, QStringLiteral("Poll interval (check for newer revisions)"), m_syncPollInterval);
            sLay->addSpacing(4);

            auto* actions = new QHBoxLayout;
            actions->addWidget(m_syncSyncNowBtn);
            actions->addStretch(1);
            actions->addWidget(m_syncDisableBtn);
            sLay->addLayout(actions);

            sLay->addWidget(m_syncGistIdLabel);
            sLay->addWidget(m_syncStatus);

            makeScrollPage(sec);
        }
    }

    // Addons (downloadable; plugin binaries not loaded yet)
    {
        QWidget* sec = buildSettingsSection(QStringLiteral("Addons"), m_settingsStack);
        auto* aLay = qobject_cast<QVBoxLayout*>(sec->layout());

        auto* intro = new QLabel(
            QStringLiteral(
                "Browse optional addons from a remote catalog. Installed addons are stored on disk "
                "and use no RAM until plugin loading is enabled in a future release."),
            sec);
        intro->setObjectName(QStringLiteral("dashHint"));
        intro->setWordWrap(true);
        aLay->addWidget(intro);
        aLay->addSpacing(6);

        m_addonsAbiLabel = new QLabel(sec);
        m_addonsAbiLabel->setObjectName(QStringLiteral("dashHint"));
        m_addonsAbiLabel->setText(
            QStringLiteral("This build ABI: %1").arg(clientoshAddonAbi()));
        aLay->addWidget(m_addonsAbiLabel);

        m_addonsRepoEdit = new QLineEdit(sec);
        m_addonsRepoEdit->setPlaceholderText(AddonConfig::defaultRepositoryUrl());
        m_addonsRepoEdit->setText(AddonConfig::repositoryUrl());
        addSettingsField(sec, QStringLiteral("Catalog URL (index.json)"), m_addonsRepoEdit);

        auto* repoRow = new QWidget(sec);
        auto* repoLay = new QHBoxLayout(repoRow);
        repoLay->setContentsMargins(0, 0, 0, 0);
        repoLay->setSpacing(8);
        m_addonsRefreshBtn = new QPushButton(QStringLiteral("Refresh catalog"), repoRow);
        m_addonsRefreshBtn->setObjectName(QStringLiteral("dashPrimary"));
        m_addonsRefreshBtn->setFocusPolicy(Qt::NoFocus);
        m_addonsRefreshBtn->setCursor(Qt::PointingHandCursor);
        auto* openFolderBtn = new QPushButton(QStringLiteral("Open folder"), repoRow);
        openFolderBtn->setObjectName(QStringLiteral("dashButton"));
        openFolderBtn->setFocusPolicy(Qt::NoFocus);
        openFolderBtn->setCursor(Qt::PointingHandCursor);
        repoLay->addWidget(m_addonsRefreshBtn);
        repoLay->addWidget(openFolderBtn);
        repoLay->addStretch(1);
        aLay->addWidget(repoRow);

        m_addonsStatus = new QLabel(QStringLiteral("Refresh the catalog to see available addons."), sec);
        m_addonsStatus->setObjectName(QStringLiteral("dashHint"));
        m_addonsStatus->setWordWrap(true);
        aLay->addWidget(m_addonsStatus);
        aLay->addSpacing(8);

        auto* listTitle = new QLabel(QStringLiteral("Available"), sec);
        listTitle->setObjectName(QStringLiteral("settingsSubsection"));
        aLay->addWidget(listTitle);

        m_addonsListHost = new QWidget(sec);
        m_addonsListLay = new QVBoxLayout(m_addonsListHost);
        m_addonsListLay->setContentsMargins(0, 0, 0, 0);
        m_addonsListLay->setSpacing(8);
        aLay->addWidget(m_addonsListHost);
        aLay->addStretch(1);

        connect(m_addonsRepoEdit, &QLineEdit::editingFinished, this, [this]() {
            persistAddonsRepoUrl();
        });
        connect(m_addonsRefreshBtn, &QPushButton::clicked, this, [this]() {
            persistAddonsRepoUrl();
            if (m_addonStore) {
                m_addonStore->refreshCatalog();
            }
        });
        connect(openFolderBtn, &QPushButton::clicked, this, []() {
            QDir().mkpath(AddonStore::addonsRoot());
            QDesktopServices::openUrl(QUrl::fromLocalFile(AddonStore::addonsRoot()));
        });

        makeScrollPage(sec);
    }

    // About
    {
        QWidget* sec = buildSettingsSection(QStringLiteral("About"), m_settingsStack);
        auto* aLay = qobject_cast<QVBoxLayout*>(sec->layout());

        // Logo — the app's terminal mark, rendered large.
        auto* logo = new QLabel(sec);
        logo->setPixmap(QIcon(QStringLiteral(":/icons/terminal.svg")).pixmap(72, 72));
        logo->setAlignment(Qt::AlignHCenter);
        logo->setObjectName(QStringLiteral("settingsAboutLogo"));

        // Name
        auto* appName = new QLabel(QStringLiteral("Clientosh"), sec);
        appName->setObjectName(QStringLiteral("settingsAboutName"));
        appName->setAlignment(Qt::AlignHCenter);

        // Version — pulled directly from CMake via the generated version.h.
        m_aboutCurrentVersion = new QLabel(
            QStringLiteral("Version v%1").arg(CLIENTOSH_VERSION), sec);
        m_aboutCurrentVersion->setObjectName(QStringLiteral("settingsAboutVersion"));
        m_aboutCurrentVersion->setAlignment(Qt::AlignHCenter);

        m_aboutUpdateStatus = new QLabel(QStringLiteral("Checking for updates…"), sec);
        m_aboutUpdateStatus->setObjectName(QStringLiteral("settingsAboutUpdate"));
        m_aboutUpdateStatus->setAlignment(Qt::AlignHCenter);
        m_aboutUpdateStatus->setWordWrap(true);

        m_aboutDownloadBtn = new QPushButton(QStringLiteral("Download"), sec);
        m_aboutDownloadBtn->setObjectName(QStringLiteral("dashPrimary"));
        m_aboutDownloadBtn->setCursor(Qt::PointingHandCursor);
        m_aboutDownloadBtn->setFocusPolicy(Qt::NoFocus);
        m_aboutDownloadBtn->hide();
        connect(m_aboutDownloadBtn, &QPushButton::clicked, this, [this]() {
            const QUrl url = m_aboutReleaseUrl.isValid()
                ? m_aboutReleaseUrl
                : QUrl(QStringLiteral("https://github.com/hdmain/clientosh/releases/latest"));
            QDesktopServices::openUrl(url);
        });

        auto* starBtn = new QPushButton(QStringLiteral("★ Star on GitHub"), sec);
        starBtn->setObjectName(QStringLiteral("dashPrimary"));
        starBtn->setCursor(Qt::PointingHandCursor);
        starBtn->setFocusPolicy(Qt::NoFocus);
        connect(starBtn, &QPushButton::clicked, this, []() {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/hdmain/clientosh")));
        });

        aLay->addStretch(1);
        aLay->addWidget(logo);
        aLay->addSpacing(6);
        aLay->addWidget(appName);
        aLay->addSpacing(6);
        aLay->addWidget(m_aboutCurrentVersion);
        aLay->addSpacing(8);
        aLay->addWidget(m_aboutUpdateStatus);
        aLay->addSpacing(10);
        aLay->addWidget(m_aboutDownloadBtn, 0, Qt::AlignHCenter);
        aLay->addSpacing(12);
        aLay->addWidget(starBtn, 0, Qt::AlignHCenter);
        aLay->addStretch(1);

        makeScrollPage(sec);
    }

    setBodyLay->addWidget(m_settingsNav);
    auto* setRule = new QFrame(setBody);
    setRule->setObjectName(QStringLiteral("sideRule"));
    setRule->setFrameShape(QFrame::VLine);
    setRule->setFixedWidth(1);
    setBodyLay->addWidget(setRule);
    setBodyLay->addWidget(m_settingsStack, 1);

    setOuter->addWidget(setBody, 1);

    auto* setFooter = new QWidget(m_settingsPage);
    setFooter->setObjectName(QStringLiteral("settingsFooter"));
    auto* setFootLay = new QHBoxLayout(setFooter);
    setFootLay->setContentsMargins(16, 8, 16, 10);
    setFootLay->setSpacing(8);
    auto* setSave = new QPushButton(QIcon(QStringLiteral(":/icons/settings.svg")),
                                    QStringLiteral("Save settings"), setFooter);
    setSave->setObjectName(QStringLiteral("dashPrimary"));
    setSave->setIconSize(QSize(13, 13));
    setSave->setFocusPolicy(Qt::NoFocus);
    setFootLay->addStretch(1);
    setFootLay->addWidget(setSave);
    setOuter->addWidget(setFooter);

    m_stack->addWidget(m_settingsPage);

    // ---- Form page ----
    m_formPage = new QWidget;
    auto* formOuter = new QVBoxLayout(m_formPage);
    formOuter->setContentsMargins(0, 0, 0, 0);
    formOuter->setSpacing(0);

    auto* formScroll = new QScrollArea(m_formPage);
    formScroll->setWidgetResizable(true);
    formScroll->setFrameShape(QFrame::NoFrame);
    formScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* formInner = new QWidget(formScroll);
    formInner->setObjectName(QStringLiteral("dashMain"));
    auto* formLay = new QVBoxLayout(formInner);
    formLay->setContentsMargins(20, 16, 20, 16);
    formLay->setSpacing(10);
    formScroll->setWidget(formInner);

    // Title lives in the top bar only (updateTopBar) — keep these off-screen
    // as the source of the page heading text.
    m_formTitle = new QLabel(QStringLiteral("New Session"), formInner);
    m_formTitle->hide();
    m_formSub = new QLabel(QStringLiteral("Connection details for a saved host"), formInner);
    m_formSub->hide();

    auto addSection = [&](const QString& title) {
        auto* lab = new QLabel(title, formInner);
        lab->setObjectName(QStringLiteral("settingsSectionTitle"));
        formLay->addWidget(lab);
        auto* rule = new QFrame(formInner);
        rule->setObjectName(QStringLiteral("settingsDivider"));
        rule->setFrameShape(QFrame::NoFrame);
        rule->setFixedHeight(1);
        formLay->addWidget(rule);
    };

    auto addLabeled = [&](QVBoxLayout* into, const QString& label, QWidget* field) {
        auto* lab = new QLabel(label, formInner);
        lab->setObjectName(QStringLiteral("fieldLabel"));
        into->addWidget(lab);
        into->addWidget(field);
    };

    // ---- Connection ----
    addSection(QStringLiteral("Connection"));

    m_nameEdit = new QLineEdit(formInner);
    m_nameEdit->setPlaceholderText(QStringLiteral("e.g. production, home lab"));
    m_connectionModeCombo = new QComboBox(formInner);
    m_connectionModeCombo->addItem(QStringLiteral("SSH - terminal + SFTP"),
                                   static_cast<int>(ConnectionMode::Ssh));
    m_connectionModeCombo->addItem(QStringLiteral("Telnet - terminal"),
                                   static_cast<int>(ConnectionMode::Telnet));
    m_connectionModeCombo->addItem(QStringLiteral("Serial / COM - terminal"),
                                   static_cast<int>(ConnectionMode::Serial));
    m_connectionModeCombo->addItem(QStringLiteral("SFTP only - file manager"),
                                   static_cast<int>(ConnectionMode::SftpOnly));

    m_hostEdit = new QLineEdit(formInner);
    m_hostEdit->setPlaceholderText(QStringLiteral("hostname or IP"));
    m_hostEdit->setText(AppSettings::defaultHost());
    m_portSpin = new QSpinBox(formInner);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(AppSettings::defaultPort());
    m_portSpin->setMinimumWidth(90);
    m_portSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_portSpin->setFixedHeight(m_hostEdit->sizeHint().height());
    m_userEdit = new QLineEdit(formInner);
    m_userEdit->setPlaceholderText(QStringLiteral("username"));
    m_userEdit->setText(AppSettings::defaultUser());

    addLabeled(formLay, QStringLiteral("Display name (optional)"), m_nameEdit);
    addLabeled(formLay, QStringLiteral("Type"), m_connectionModeCombo);

    m_networkFieldsPanel = new QWidget(formInner);
    auto* networkLay = new QVBoxLayout(m_networkFieldsPanel);
    networkLay->setContentsMargins(0, 0, 0, 0);
    networkLay->setSpacing(10);
    auto* hostPortRow = new QWidget(m_networkFieldsPanel);
    auto* hostPortLay = new QHBoxLayout(hostPortRow);
    hostPortLay->setContentsMargins(0, 0, 0, 0);
    hostPortLay->setSpacing(10);
    auto* hostCol = new QVBoxLayout;
    hostCol->setContentsMargins(0, 0, 0, 0);
    hostCol->setSpacing(4);
    auto* portCol = new QVBoxLayout;
    portCol->setContentsMargins(0, 0, 0, 0);
    portCol->setSpacing(4);
    auto* hostLab = new QLabel(QStringLiteral("Host"), formInner);
    hostLab->setObjectName(QStringLiteral("fieldLabel"));
    auto* portLab = new QLabel(QStringLiteral("Port"), formInner);
    portLab->setObjectName(QStringLiteral("fieldLabel"));
    hostCol->addWidget(hostLab);
    hostCol->addWidget(m_hostEdit);
    portCol->addWidget(portLab);
    portCol->addWidget(m_portSpin);
    hostPortLay->addLayout(hostCol, 1);
    hostPortLay->addLayout(portCol, 0);
    networkLay->addWidget(hostPortRow);
    addLabeled(networkLay, QStringLiteral("User"), m_userEdit);
    formLay->addWidget(m_networkFieldsPanel);

    m_serialFieldsPanel = new QWidget(formInner);
    auto* serialLay = new QVBoxLayout(m_serialFieldsPanel);
    serialLay->setContentsMargins(0, 0, 0, 0);
    serialLay->setSpacing(8);
    m_serialPortCombo = new QComboBox(m_serialFieldsPanel);
    m_serialPortCombo->setEditable(true);
    m_serialPortCombo->setInsertPolicy(QComboBox::NoInsert);
    auto refreshSerialPorts = [this]() {
        const QString previous = m_serialPortCombo->currentText().trimmed();
        m_serialPortCombo->clear();
        m_serialPortCombo->addItems(SerialSession::availablePorts());
        const int index = m_serialPortCombo->findText(previous);
        if (!previous.isEmpty()) m_serialPortCombo->setCurrentText(previous);
        else if (index >= 0) m_serialPortCombo->setCurrentIndex(index);
#ifdef Q_OS_WIN
        else if (m_serialPortCombo->count() == 0) m_serialPortCombo->setCurrentText(QStringLiteral("COM1"));
#endif
    };
    auto* serialPortRow = new QWidget(m_serialFieldsPanel);
    auto* serialPortRowLay = new QHBoxLayout(serialPortRow);
    serialPortRowLay->setContentsMargins(0, 0, 0, 0);
    serialPortRowLay->setSpacing(6);
    auto* refreshPortsBtn = new QPushButton(QStringLiteral("Refresh"), serialPortRow);
    refreshPortsBtn->setObjectName(QStringLiteral("dashButton"));
    refreshPortsBtn->setFocusPolicy(Qt::NoFocus);
    serialPortRowLay->addWidget(m_serialPortCombo, 1);
    serialPortRowLay->addWidget(refreshPortsBtn);
    addLabeled(serialLay, QStringLiteral("Serial port"), serialPortRow);
    connect(refreshPortsBtn, &QPushButton::clicked, this, refreshSerialPorts);
    refreshSerialPorts();

    auto* serialSettingsRow = new QWidget(m_serialFieldsPanel);
    auto* serialSettingsLay = new QHBoxLayout(serialSettingsRow);
    serialSettingsLay->setContentsMargins(0, 0, 0, 0);
    serialSettingsLay->setSpacing(8);
    auto addSerialCombo = [&](const QString& label, QComboBox** combo, const QStringList& values) {
        auto* column = new QVBoxLayout;
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(4);
        auto* lab = new QLabel(label, m_serialFieldsPanel);
        lab->setObjectName(QStringLiteral("fieldLabel"));
        *combo = new QComboBox(m_serialFieldsPanel);
        (*combo)->addItems(values);
        column->addWidget(lab);
        column->addWidget(*combo);
        serialSettingsLay->addLayout(column, 1);
    };
    addSerialCombo(QStringLiteral("Baud"), &m_serialBaudCombo,
                   {QStringLiteral("1200"), QStringLiteral("2400"), QStringLiteral("4800"),
                    QStringLiteral("9600"), QStringLiteral("19200"), QStringLiteral("38400"),
                    QStringLiteral("57600"), QStringLiteral("115200"), QStringLiteral("230400"),
                    QStringLiteral("460800"), QStringLiteral("921600")});
    m_serialBaudCombo->setCurrentText(QStringLiteral("115200"));
    addSerialCombo(QStringLiteral("Data bits"), &m_serialDataBitsCombo,
                   {QStringLiteral("5"), QStringLiteral("6"), QStringLiteral("7"), QStringLiteral("8")});
    m_serialDataBitsCombo->setCurrentText(QStringLiteral("8"));
    addSerialCombo(QStringLiteral("Parity"), &m_serialParityCombo,
                   {QStringLiteral("none"), QStringLiteral("even"), QStringLiteral("odd")});
    addSerialCombo(QStringLiteral("Stop bits"), &m_serialStopBitsCombo,
                   {QStringLiteral("1"), QStringLiteral("2")});
    serialLay->addWidget(serialSettingsRow);
    addSerialCombo(QStringLiteral("Flow control"), &m_serialFlowCombo,
                   {QStringLiteral("none"), QStringLiteral("hardware"), QStringLiteral("software")});
    formLay->addWidget(m_serialFieldsPanel);
    formLay->addSpacing(6);

    // ---- Authentication ----
    m_authSectionTitle = new QLabel(QStringLiteral("Authentication"), formInner);
    m_authSectionTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
    formLay->addWidget(m_authSectionTitle);
    m_authSectionRule = new QFrame(formInner);
    m_authSectionRule->setObjectName(QStringLiteral("settingsDivider"));
    qobject_cast<QFrame*>(m_authSectionRule)->setFrameShape(QFrame::NoFrame);
    m_authSectionRule->setFixedHeight(1);
    formLay->addWidget(m_authSectionRule);

    m_authMethodCombo = new QComboBox(formInner);
    m_authMethodCombo->addItem(QStringLiteral("Password"), static_cast<int>(AuthMethod::Password));
    m_authMethodCombo->addItem(QStringLiteral("SSH Agent"), static_cast<int>(AuthMethod::SshAgent));
    m_authMethodCombo->addItem(QStringLiteral("Stored key"), static_cast<int>(AuthMethod::StoredKey));
    m_authMethodCombo->addItem(QStringLiteral("Key file"), static_cast<int>(AuthMethod::KeyFile));
    m_authMethodLabel = new QLabel(QStringLiteral("Method"), formInner);
    m_authMethodLabel->setObjectName(QStringLiteral("fieldLabel"));
    formLay->addWidget(m_authMethodLabel);
    formLay->addWidget(m_authMethodCombo);

    m_authPasswordPanel = new QWidget(formInner);
    {
        auto* lay = new QVBoxLayout(m_authPasswordPanel);
        lay->setContentsMargins(0, 4, 0, 0);
        lay->setSpacing(6);
        m_passEdit = new QLineEdit(m_authPasswordPanel);
        m_passEdit->setPlaceholderText(QStringLiteral("password"));
        m_passEdit->setEchoMode(QLineEdit::Password);
        m_savePass = new QCheckBox(QStringLiteral("Save password with this profile"), m_authPasswordPanel);
        addLabeled(lay, QStringLiteral("Password"), m_passEdit);
        lay->addWidget(m_savePass);
    }

    m_authKeyringPanel = new QWidget(formInner);
    {
        auto* lay = new QVBoxLayout(m_authKeyringPanel);
        lay->setContentsMargins(0, 4, 0, 0);
        lay->setSpacing(6);
        m_keyringCombo = new QComboBox(m_authKeyringPanel);
        m_manageKeysBtn = new QPushButton(QStringLiteral("Manage keys…"), m_authKeyringPanel);
        m_manageKeysBtn->setObjectName(QStringLiteral("dashButton"));
        m_manageKeysBtn->setFocusPolicy(Qt::NoFocus);
        auto* keyringRow = new QWidget(m_authKeyringPanel);
        auto* keyringLay = new QHBoxLayout(keyringRow);
        keyringLay->setContentsMargins(0, 0, 0, 0);
        keyringLay->setSpacing(6);
        keyringLay->addWidget(m_keyringCombo, 1);
        keyringLay->addWidget(m_manageKeysBtn);
        addLabeled(lay, QStringLiteral("Saved key"), keyringRow);
        auto* hint = new QLabel(
            QStringLiteral("Keys are managed centrally in SSH Keys and encrypted in the local vault."),
            m_authKeyringPanel);
        hint->setObjectName(QStringLiteral("dashHint"));
        hint->setWordWrap(true);
        lay->addWidget(hint);
    }

    m_authKeyFilePanel = new QWidget(formInner);
    {
        auto* lay = new QVBoxLayout(m_authKeyFilePanel);
        lay->setContentsMargins(0, 4, 0, 0);
        lay->setSpacing(6);
        m_keyPathEdit = new QLineEdit(m_authKeyFilePanel);
        m_keyPathEdit->setPlaceholderText(QStringLiteral("path to private key"));
        m_browseKeyBtn = new QPushButton(QStringLiteral("Browse…"), m_authKeyFilePanel);
        m_browseKeyBtn->setObjectName(QStringLiteral("dashButton"));
        m_browseKeyBtn->setFocusPolicy(Qt::NoFocus);
        auto* keyRow = new QWidget(m_authKeyFilePanel);
        auto* keyLay = new QHBoxLayout(keyRow);
        keyLay->setContentsMargins(0, 0, 0, 0);
        keyLay->setSpacing(6);
        keyLay->addWidget(m_keyPathEdit, 1);
        keyLay->addWidget(m_browseKeyBtn);
        addLabeled(lay, QStringLiteral("Private key file"), keyRow);
    }

    m_authPassphrasePanel = new QWidget(formInner);
    {
        auto* lay = new QVBoxLayout(m_authPassphrasePanel);
        lay->setContentsMargins(0, 4, 0, 0);
        lay->setSpacing(6);
        m_keyPassEdit = new QLineEdit(m_authPassphrasePanel);
        m_keyPassEdit->setPlaceholderText(QStringLiteral("leave empty if the key is not encrypted"));
        m_keyPassEdit->setEchoMode(QLineEdit::Password);
        m_saveKeyPass = new QCheckBox(QStringLiteral("Save passphrase"),
                                     m_authPassphrasePanel);
        addLabeled(lay, QStringLiteral("Key passphrase (optional)"), m_keyPassEdit);
        lay->addWidget(m_saveKeyPass);
    }

    formLay->addWidget(m_authPasswordPanel);
    formLay->addWidget(m_authKeyringPanel);
    formLay->addWidget(m_authKeyFilePanel);
    formLay->addWidget(m_authPassphrasePanel);
    formLay->addStretch(1);

    // ---- Footer actions ----
    auto* formFooter = new QWidget(m_formPage);
    formFooter->setObjectName(QStringLiteral("settingsFooter"));
    auto* formRow = new QHBoxLayout(formFooter);
    formRow->setContentsMargins(16, 8, 16, 10);
    formRow->setSpacing(8);
    auto* backBtn = new QPushButton(QStringLiteral("Cancel"), formFooter);
    auto* saveBtn = new QPushButton(QStringLiteral("Save"), formFooter);
    auto* connectBtn = new QPushButton(QIcon(QStringLiteral(":/icons/connect.svg")),
                                       QStringLiteral("Connect"), formFooter);
    m_saveProfileBtn = saveBtn;
    m_connectProfileBtn = connectBtn;
    backBtn->setObjectName(QStringLiteral("dashButton"));
    saveBtn->setObjectName(QStringLiteral("dashButton"));
    connectBtn->setObjectName(QStringLiteral("dashPrimary"));
    for (auto* b : {backBtn, saveBtn, connectBtn}) {
        b->setIconSize(QSize(13, 13));
        b->setFocusPolicy(Qt::NoFocus);
    }
    formRow->addWidget(backBtn);
    formRow->addStretch(1);
    formRow->addWidget(saveBtn);
    formRow->addWidget(connectBtn);

    formOuter->addWidget(formScroll, 1);
    formOuter->addWidget(formFooter);
    m_stack->addWidget(m_formPage);

    root->addWidget(mainCol, 1);

    // ---- Wiring ----
    connect(m_navGroup, &QButtonGroup::idClicked, this, [this](int id) {
        setNavPage(static_cast<NavPage>(id));
    });
    connect(m_newSessionBtn, &QPushButton::clicked, this, &DashboardPage::showNewSessionForm);
    connect(backBtn, &QPushButton::clicked, this, &DashboardPage::showHome);
    connect(saveBtn, &QPushButton::clicked, this, &DashboardPage::saveCurrentFormAsProfile);
    connect(connectBtn, &QPushButton::clicked, this, &DashboardPage::connectFromForm);
    connect(m_browseKeyBtn, &QPushButton::clicked, this, &DashboardPage::browsePrivateKey);
    connect(m_importKeyBtn, &QPushButton::clicked, this, &DashboardPage::importKeyIntoKeyring);
    connect(m_renameKeyBtn, &QPushButton::clicked, this, &DashboardPage::renameSelectedStoredKey);
    connect(m_passphraseKeyBtn, &QPushButton::clicked,
            this, &DashboardPage::editSelectedStoredKeyPassphrase);
    connect(m_removeKeyBtn, &QPushButton::clicked, this, &DashboardPage::removeSelectedKeyringKey);
    connect(m_manageKeysBtn, &QPushButton::clicked, this, [this]() { setNavPage(NavPage::Keychain); });
    connect(m_keysTable, &QTableWidget::itemSelectionChanged,
            this, &DashboardPage::updateStoredKeyActions);
    connect(m_keyringCombo, &QComboBox::currentIndexChanged, this, &DashboardPage::onKeyringSelectionChanged);
    connect(m_authMethodCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        updateAuthMethodUi();
        const auto mode = static_cast<ConnectionMode>(m_connectionModeCombo->currentData().toInt());
        if (m_editingId.isEmpty()
            && (mode == ConnectionMode::Ssh || mode == ConnectionMode::SftpOnly)) {
            AppSettings::setLastAuthMethod(m_authMethodCombo->currentData().toInt());
        }
    });
    connect(m_connectionModeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        updateConnectionModeUi();
    });
    reloadKeyringCombo();
    updateConnectionModeUi();
    connect(setSave, &QPushButton::clicked, this, &DashboardPage::saveSettingsUi);
    connect(m_settingsNav, &QListWidget::currentRowChanged, this, &DashboardPage::setSettingsCategory);
    connect(m_settingsAnimations, &QCheckBox::toggled, this, [](bool on) {
        Motion::setEnabled(on);
    });
    connect(m_settingsSavePassDefault, &QCheckBox::toggled, this, &DashboardPage::persistPrefsLive);
    connect(m_settingsDefaultHost, &QLineEdit::editingFinished, this, &DashboardPage::persistPrefsLive);
    connect(m_settingsDefaultUser, &QLineEdit::editingFinished, this, &DashboardPage::persistPrefsLive);
    connect(m_settingsShowStats, &QCheckBox::toggled, this, [this](bool visible) {
        m_settingsStatsInterval->setEnabled(visible);
        persistPrefsLive();
        emit settingsApplied();
    });
    connect(m_settingsStatsInterval, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int) {
                persistPrefsLive();
                emit settingsApplied();
            });
    connect(m_settingsDefaultPort, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int) { persistPrefsLive(); });
    connect(m_settingsHideDotfiles, &QCheckBox::toggled, this, &DashboardPage::persistPrefsLive);
    connect(m_settingsSftpView, &QComboBox::currentIndexChanged, this,
            [this](int) { persistPrefsLive(); });
    // ---- Sync (background controller, never blocks UI) ---------------
    m_sync = new SyncController(this);
    {
        const QString deviceId = SyncConfig::deviceId();
        const QString raw = QString::fromLatin1(qgetenv("COMPUTERNAME"));
        const QString fallback = QSysInfo::machineHostName().trimmed();
        const QString hostLabel = raw.isEmpty() ? fallback : raw;
        m_sync->setDeviceId(deviceId, hostLabel.isEmpty() ? QStringLiteral("computer") : hostLabel);
    }
    m_sync->setDataProvider(
        // serialize the current in-memory profile set (including secrets) — use
        // m_profiles directly so a just-saved local change is not lost to
        // a stale vault read, then re-load secrets from the vault as needed.
        [this]() -> QByteArray {
            VaultManager v;
            const QVector<StoredKey> keys = v.listStoredKeys();
            // Bring the PEM for every key into the snapshot
            QVector<StoredKey> fullKeys;
            fullKeys.reserve(keys.size());
            for (StoredKey k : keys) {
                if (!k.id.isEmpty()) {
                    StoredKey out;
                    if (v.retrieveStoredKey(k.id, out)) {
                        fullKeys.push_back(out);
                    }
                }
            }
            SyncPayload p;
            p.profiles = m_profiles;
            p.keys = fullKeys;
            // Let the controller frame rev/timestamp/device
            return SyncPayloadCodec::toJson(p);
        },
        // apply a synced payload back to the local store
        [this](const QByteArray& jsonBytes) -> bool {
            bool ok = false;
            SyncPayload p = SyncPayloadCodec::fromJson(jsonBytes, &ok);
            if (!ok) {
                return false;
            }
            // Persist sessions + keyring
            if (!saveProfiles(p.profiles)) {
                return false;
            }
            VaultManager v;
            for (const StoredKey& k : p.keys) {
                if (!v.storeStoredKey(k)) {
                    return false;
                }
            }
            m_profiles = p.profiles;
            rebuildSavedList();
            rebuildKeychainList();
            appendLog(QStringLiteral("sync: applied %1 profiles from remote").arg(p.profiles.size()));
            return true;
        });

    connect(m_syncTestBtn, &QPushButton::clicked, this, [this]() { syncTestToken(); });
    connect(m_syncCreateBtn, &QPushButton::clicked, this, [this]() { syncCreateSetup(); });
    connect(m_syncJoinBtn, &QPushButton::clicked, this, [this]() { syncJoinFromInput(); });
    connect(m_syncDisableBtn, &QPushButton::clicked, this, [this]() { syncDisable(); });
    connect(m_syncSyncNowBtn, &QPushButton::clicked, this, [this]() { syncPullNow(); });
    connect(m_syncCopyKeyBtn, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_syncKeyDisplay->text());
        m_syncStatus->setText(QStringLiteral("Sync key copied to the clipboard."));
        appendLog(QStringLiteral("sync: sync key copied"));
    });
    connect(m_syncPollInterval, &QSpinBox::valueChanged, this, [this](int) { persistSyncLive(); });
    connect(m_syncEnabledCheck, &QCheckBox::toggled, this, [this](bool on) {
        persistSyncLive();
        if (!m_sync) {
            return;
        }
        if (!on) {
            m_deferredSyncKey.clear();
            m_deferredSyncToken.clear();
            m_sync->setPaused(true);
            syncRefreshUiFromSyncState();
            return;
        }
        m_sync->setPaused(false);
        if (m_sync->state() == SyncController::State::Disabled) {
            const QString keyText = SyncConfig::syncKeyText();
            if (!keyText.isEmpty()) {
                QString token = m_syncTokenEdit->text().trimmed();
                const SyncKey key = SyncKeyCodec::decode(keyText);
                if (token.isEmpty() && !key.token.isEmpty()) {
                    token = QString::fromUtf8(key.token);
                }
                if (token.isEmpty() && key.isValid()) {
                    token = SyncConfig::loadToken(QString::fromLatin1(key.syncUuid.toHex()));
                }
                if (!token.isEmpty()) {
                    m_syncTokenEdit->setText(token);
                    m_deferredSyncKey.clear();
                    m_deferredSyncToken.clear();
                    m_sync->restoreExisting(keyText, token);
                }
            }
        }
        syncRefreshUiFromSyncState();
    });

    // Controller → UI status
    connect(m_sync, &SyncController::stateChanged, this, &DashboardPage::syncOnStateChanged);
    connect(m_sync, &SyncController::statusMessage, this, &DashboardPage::syncOnStatus);
    connect(m_sync, &SyncController::errorOccurred, this, &DashboardPage::syncOnError);
    connect(m_sync, &SyncController::dataUpdated, this, &DashboardPage::syncOnDataUpdated);

    // Pull-after-save debounce (buffer bursts from profile edits)
    m_syncSaveDebounce = new QTimer(this);
    m_syncSaveDebounce->setSingleShot(true);
    m_syncSaveDebounce->setInterval(1800);
    connect(m_syncSaveDebounce, &QTimer::timeout, this, [this]() { syncPushNow(); });

    applyStoredSyncState();
    syncRefreshUiFromSyncState();

    // ---- Addons marketplace (install files only; no plugin load yet) ----
    m_addonStore = new AddonStore(this);
    m_addonHost = new AddonHost(m_addonStore, this);
    connect(m_addonStore, &AddonStore::catalogUpdated, this, &DashboardPage::rebuildAddonsList);
    connect(m_addonStore, &AddonStore::statusMessage, this, [this](const QString& msg) {
        if (m_addonsStatus) {
            m_addonsStatus->setText(msg);
        }
        appendLog(QStringLiteral("addons: %1").arg(msg));
    });
    connect(m_addonStore, &AddonStore::errorOccurred, this, [this](const QString& err) {
        if (m_addonsStatus) {
            m_addonsStatus->setText(err);
        }
        appendLog(QStringLiteral("addons: %1").arg(err));
    });
    connect(m_addonStore, &AddonStore::installFinished, this,
            [this](const QString& id, bool ok, const QString& err) {
                if (!ok && m_addonsStatus) {
                    m_addonsStatus->setText(err);
                }
                Q_UNUSED(id);
                rebuildAddonsList();
            });
    connect(m_addonStore, &AddonStore::removeFinished, this,
            [this](const QString&, bool ok, const QString& err) {
                if (!ok && m_addonsStatus) {
                    m_addonsStatus->setText(err);
                }
                rebuildAddonsList();
            });
    connect(m_addonHost, &AddonHost::installedChanged, this, &DashboardPage::rebuildAddonsList);
    rebuildAddonsList();

    connect(clearLogs, &QPushButton::clicked, this, [this]() { m_logsView->clear(); });
    connect(m_passEdit, &QLineEdit::returnPressed, this, &DashboardPage::connectFromForm);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString&) { applySavedFilter(); });

    connect(m_savedTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item && item->parent()) {
            const QString id = item->data(0, Qt::UserRole).toString();
            if (item->data(0, Qt::UserRole + 1).toString() == QLatin1String("live")) {
                emit openLiveSession(id);
            } else {
                openSavedProfile(id);
            }
        }
    });
    connect(m_sessions, &SessionManager::sessionOpened, this, [this](const QString& id) {
        rebuildActiveList();
        rebuildSavedList();
        if (const auto* live = m_sessions->session(id)) {
            appendLog(QStringLiteral("opened %1").arg(live->profile.displayTitle()));
        }
    });
    connect(m_sessions, &SessionManager::sessionClosed, this, [this](const QString&) {
        rebuildActiveList();
        rebuildSavedList();
        appendLog(QStringLiteral("session closed"));
    });
    connect(m_sessions, &SessionManager::sessionStatusChanged, this,
            [this](const QString& id, const QString& status) {
                rebuildActiveList();
                rebuildSavedList();
                if (const auto* live = m_sessions->session(id)) {
                    appendLog(QStringLiteral("%1 · %2").arg(live->profile.displayTitle(), status));
                }
            });
    connect(m_sessions, &SessionManager::sessionConnectionChanged, this,
            [this](const QString&, bool) {
                rebuildActiveList();
                rebuildSavedList();
            });
    connect(m_sessions, &SessionManager::sessionSystemDetected, this,
            [this](const QString& sessionId, const QString& system) {
                if (const auto* live = m_sessions->session(sessionId)) {
                    setProfileSystem(live->profile.id, system);
                }
                rebuildSavedList();
            });

    loadSettingsUi();
    m_navHosts->setChecked(true);
    setNavPage(NavPage::Hosts);
    // Profiles were loaded once in the constructor — rebuild UI without re-reading the vault.
    rebuildLists();

    // Sync network restore after the rest of startup (MainWindow finish + first show) settles.
    QTimer::singleShot(0, this, &DashboardPage::startDeferredSyncRestore);
}

QToolButton* DashboardPage::makeNavButton(const QString& iconPath, const QString& text, QWidget* parent)
{
    return makeSidebarNav(iconPath, text, parent);
}

QToolButton* DashboardPage::makeRowAction(const QString& iconPath, const QString& tip, QWidget* parent)
{
    return makeRowIcon(iconPath, tip, parent);
}

QWidget* DashboardPage::buildSettingsSection(const QString& title, QWidget* parent)
{
    auto* sec = new QWidget(parent);
    auto* lay = new QVBoxLayout(sec);
    lay->setContentsMargins(16, 12, 16, 12);
    lay->setSpacing(6);
    // Keep the section at its natural height (scrolls when it overflows) instead
    // of letting the scroll area stretch it and pad every row far apart.
    sec->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto* heading = new QLabel(title, sec);
    heading->setObjectName(QStringLiteral("settingsSectionTitle"));
    lay->addWidget(heading);
    return sec;
}

void DashboardPage::addSettingsField(QWidget* section, const QString& label, QWidget* field)
{
    auto* lay = qobject_cast<QVBoxLayout*>(section->layout());
    if (!lay || !field) {
        return;
    }
    auto* lab = new QLabel(label, section);
    lab->setObjectName(QStringLiteral("fieldLabel"));
    field->setParent(section);
    lay->addSpacing(4);
    lay->addWidget(lab);
    lay->addWidget(field);
}

void DashboardPage::setSettingsCategory(int index)
{
    if (!m_settingsStack || index < 0 || index >= m_settingsStack->count()) {
        return;
    }
    m_settingsStack->setCurrentIndex(index);
    // About is the last settings category — refresh GitHub latest-release status.
    if (m_settingsNav && index == m_settingsNav->count() - 1) {
        checkForUpdates();
    }
}

void DashboardPage::checkForUpdates()
{
    if (!m_aboutUpdateStatus || !m_aboutDownloadBtn) {
        return;
    }
    if (m_updateCheckInFlight) {
        return;
    }
    if (!m_updateNam) {
        m_updateNam = new QNetworkAccessManager(this);
    }

    m_updateCheckInFlight = true;
    m_aboutDownloadBtn->hide();
    m_aboutReleaseUrl = QUrl();
    m_aboutUpdateStatus->setText(QStringLiteral("Checking for updates…"));

    QNetworkRequest req(
        QUrl(QStringLiteral("https://api.github.com/repos/hdmain/clientosh/releases/latest")));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("clientosh/%1").arg(QStringLiteral(CLIENTOSH_VERSION)));
    req.setRawHeader("Accept", "application/vnd.github+json");
    // Avoid hanging the About page forever on a stalled connection.
    req.setTransferTimeout(12000);

    QNetworkReply* reply = m_updateNam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleUpdateCheckReply(reply);
    });
}

void DashboardPage::handleUpdateCheckReply(QNetworkReply* reply)
{
    m_updateCheckInFlight = false;
    if (!reply) {
        return;
    }
    reply->deleteLater();
    if (!m_aboutUpdateStatus || !m_aboutDownloadBtn) {
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        m_aboutUpdateStatus->setText(QStringLiteral("Could not check for updates"));
        m_aboutDownloadBtn->hide();
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) {
        m_aboutUpdateStatus->setText(QStringLiteral("Could not check for updates"));
        m_aboutDownloadBtn->hide();
        return;
    }

    const QJsonObject obj = doc.object();
    const QString tagName = obj.value(QStringLiteral("tag_name")).toString().trimmed();
    const QString htmlUrl = obj.value(QStringLiteral("html_url")).toString().trimmed();
    const UpdateCheck::SemVer latest = UpdateCheck::parseSemVer(tagName);
    const UpdateCheck::SemVer current =
        UpdateCheck::parseSemVer(QStringLiteral(CLIENTOSH_PRODUCT_VERSION));

    if (!latest.valid) {
        m_aboutUpdateStatus->setText(QStringLiteral("Could not check for updates"));
        m_aboutDownloadBtn->hide();
        return;
    }

    const QString latestLabel = UpdateCheck::format(latest);
    if (UpdateCheck::shouldOfferUpdate(current, latest,
                                       QStringLiteral(CLIENTOSH_BUILD_CHANNEL))) {
        m_aboutReleaseUrl = QUrl(htmlUrl);
        if (!m_aboutReleaseUrl.isValid()) {
            m_aboutReleaseUrl =
                QUrl(QStringLiteral("https://github.com/hdmain/clientosh/releases/tag/%1")
                         .arg(tagName));
        }
        m_aboutUpdateStatus->setText(
            QStringLiteral("New version v%1 is available").arg(latestLabel));
        m_aboutDownloadBtn->setText(QStringLiteral("Download v%1").arg(latestLabel));
        m_aboutDownloadBtn->show();
    } else {
        m_aboutUpdateStatus->setText(QStringLiteral("You're up to date"));
        m_aboutDownloadBtn->hide();
        m_aboutReleaseUrl = QUrl();
    }
}

void DashboardPage::populateFontCombos()
{
    auto* fonts = FontManager::instance();

    auto fillUi = [&](QComboBox* box) {
        const QString current = box->currentData().toString();
        const bool wasBlocked = box->signalsBlocked();
        box->blockSignals(true);
        box->clear();
        box->addItem(QStringLiteral("Auto (system UI)"), QString());
        for (const FontCatalogEntry& e : fonts->uiCatalog()) {
            const QString suffix = fonts->isFamilyLoaded(e.family)
                ? QString()
                : QStringLiteral("  · download");
            box->addItem(e.label + suffix, e.family);
        }
        // Common system UI faces
        const QStringList systemUi = {
#ifdef Q_OS_WIN
            QStringLiteral("Segoe UI"), QStringLiteral("Calibri"), QStringLiteral("Arial"),
#elif defined(Q_OS_MACOS)
            QStringLiteral("SF Pro Text"), QStringLiteral("Helvetica Neue"), QStringLiteral("Arial"),
#else
            QStringLiteral("Ubuntu"), QStringLiteral("Noto Sans"), QStringLiteral("DejaVu Sans"),
#endif
        };
        for (const QString& family : systemUi) {
            if (box->findData(family) < 0 && QFontDatabase::hasFamily(family)) {
                box->addItem(family, family);
            }
        }
        int idx = box->findData(current);
        box->setCurrentIndex(idx >= 0 ? idx : 0);
        box->blockSignals(wasBlocked);
    };

    auto fillTerm = [&](QComboBox* box) {
        const QString current = box->currentData().toString();
        const bool wasBlocked = box->signalsBlocked();
        box->blockSignals(true);
        box->clear();
        box->addItem(QStringLiteral("Auto (system monospace)"), QString());
        for (const FontCatalogEntry& e : fonts->terminalCatalog()) {
            const QString suffix = fonts->isFamilyLoaded(e.family)
                ? QString()
                : QStringLiteral("  · download");
            box->addItem(e.label + suffix, e.family);
        }
        for (const QString& family : clientoshMonospaceFamilies()) {
            if (box->findData(family) < 0) {
                box->addItem(family, family);
            }
        }
        int idx = box->findData(current);
        box->setCurrentIndex(idx >= 0 ? idx : 0);
        box->blockSignals(wasBlocked);
    };

    fillUi(m_settingsUiFontFamily);
    fillTerm(m_settingsFontFamily);
}

void DashboardPage::ensureSelectedFonts()
{
    auto* fonts = FontManager::instance();
    const QString ui = m_settingsUiFontFamily->currentData().toString();
    const QString term = m_settingsFontFamily->currentData().toString();
    if (fonts->needsDownload(ui)) {
        fonts->ensureFamily(ui);
    }
    if (fonts->needsDownload(term)) {
        fonts->ensureFamily(term);
    }
}

void DashboardPage::refreshFontPreviews()
{
    // Previews refresh via connected lambdas on the appearance page.
}

void DashboardPage::syncColorSwatch(QAbstractButton* btn, const QColor& color)
{
    if (!btn) {
        return;
    }
    btn->setStyleSheet(QStringLiteral(
                           "QToolButton#colorSwatchBtn {"
                           "  background: %1;"
                           "  border: 1px solid #555555;"
                           "  color: %2;"
                           "  font-size: 10px;"
                           "}")
                           .arg(color.name(),
                                (color.lightness() > 140) ? QStringLiteral("#111111")
                                                         : QStringLiteral("#eeeeee")));
    btn->setText(color.name(QColor::HexRgb).toUpper());
    btn->setToolTip(color.name(QColor::HexRgb));
}

void DashboardPage::pickTerminalColor(bool foreground)
{
    const QColor current = foreground ? m_termFg : m_termBg;
    const QColor chosen = QColorDialog::getColor(current, this,
                                                 foreground ? QStringLiteral("Terminal text color")
                                                            : QStringLiteral("Terminal background"),
                                                 QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) {
        return;
    }
    QColor solid = chosen;
    solid.setAlpha(255);
    if (foreground) {
        m_termFg = solid;
        syncColorSwatch(m_settingsTermFgBtn, m_termFg);
    } else {
        m_termBg = solid;
        syncColorSwatch(m_settingsTermBgBtn, m_termBg);
    }
    if (m_settingsTermPreview) {
        m_settingsTermPreview->setStyleSheet(
            QStringLiteral("QLabel#settingsTermPreview { color: %1; background: %2; padding: 8px; }")
                .arg(m_termFg.name(), m_termBg.name()));
    }
    persistAppearanceLive();
}

void DashboardPage::persistHighlightSettings()
{
    if (!m_settingsHighlightAddresses || !m_settingsHighlightKeywords || !m_settingsHighlightCiscoCli) {
        return;
    }
    AppSettings::setHighlightAddresses(m_settingsHighlightAddresses->isChecked());
    AppSettings::setHighlightLogKeywords(m_settingsHighlightKeywords->isChecked());
    AppSettings::setHighlightCiscoCli(m_settingsHighlightCiscoCli->isChecked());
}

void DashboardPage::notifyHighlightSettingsChanged()
{
    // Refresh terminals only — do not reopen a shared QSettings object that could
    // clobber the highlight keys we just wrote.
    m_applyingAppearance = true;
    const QList<QCheckBox*> boxes = {m_settingsHighlightAddresses, m_settingsHighlightKeywords,
                                     m_settingsHighlightCiscoCli};
    for (QCheckBox* b : boxes) {
        if (b) {
            b->blockSignals(true);
        }
    }
    emit settingsApplied();
    for (QCheckBox* b : boxes) {
        if (b) {
            b->blockSignals(false);
        }
    }
    m_applyingAppearance = false;
}

void DashboardPage::persistAppearanceLive()
{
    if (m_applyingAppearance) {
        return;
    }
    m_applyingAppearance = true;

    // Read path before opening the writer — never mix setValueSync() with a
    // long-lived QSettings (stale sync can wipe sibling keys).
    const QString bgImagePath = AppSettings::terminalBgImage();

    // One QSettings instance for the whole write. Mixing a long-lived QSettings
    // with AppSettings::setValueSync() caused s.sync() to rewrite the INI from a
    // stale snapshot and wipe highlightAddresses / highlightLogKeywords back to false.
    QSettings s;
    s.setValue(QLatin1String(AppSettings::kTheme), m_settingsTheme->currentData().toString());
    s.setValue(QLatin1String(AppSettings::kUiFontFamily), m_settingsUiFontFamily->currentData().toString());
    s.setValue(QLatin1String(AppSettings::kUiFontSize), m_settingsUiFontSize->value());
    s.setValue(QLatin1String(AppSettings::kFontFamily), m_settingsFontFamily->currentData().toString());
    s.setValue(QLatin1String(AppSettings::kFontSize),
               qBound(9, m_settingsFontSize->value(), 22));
    s.setValue(QLatin1String(AppSettings::kTerminalFg), m_termFg.name(QColor::HexRgb));
    s.setValue(QLatin1String(AppSettings::kTerminalBg), m_termBg.name(QColor::HexRgb));
    s.setValue(QLatin1String(AppSettings::kTerminalBgImage), bgImagePath);
    if (m_settingsTermBgOpacity) {
        s.setValue(QLatin1String(AppSettings::kTerminalBgOpacity),
                   qBound(0.0, qreal(m_settingsTermBgOpacity->value()) / 100.0, 1.0));
    }
    if (m_settingsTermBgBlur) {
        s.setValue(QLatin1String(AppSettings::kTerminalBgBlur),
                   qBound(0, m_settingsTermBgBlur->value(), 100));
    }
    // Do NOT rewrite highlight* here — those keys are owned by the checkbox toggles.
    s.sync();

    const QList<QCheckBox*> boxes = {m_settingsHighlightAddresses, m_settingsHighlightKeywords,
                                     m_settingsHighlightCiscoCli};
    for (QCheckBox* b : boxes) {
        if (b) {
            b->blockSignals(true);
        }
    }
    emit settingsApplied();
    for (QCheckBox* b : boxes) {
        if (b) {
            b->blockSignals(false);
        }
    }
    m_applyingAppearance = false;
}

void DashboardPage::persistPrefsLive()
{
    // General / Performance / SSH / SFTP prefs that previously required Save.
    // One QSettings batch — never call setValueSync while this object lives.
    QSettings s;
    if (m_settingsSavePassDefault) {
        s.setValue(QLatin1String(AppSettings::kSavePasswordDefault),
                   m_settingsSavePassDefault->isChecked());
    }
    if (m_settingsDefaultHost) {
        const QString host = m_settingsDefaultHost->text().trimmed();
        s.setValue(QLatin1String(AppSettings::kDefaultHost),
                   host.isEmpty() ? QStringLiteral("127.0.0.1") : host);
    }
    if (m_settingsDefaultUser) {
        s.setValue(QLatin1String(AppSettings::kDefaultUser),
                   m_settingsDefaultUser->text().trimmed());
    }
    if (m_settingsShowStats) {
        s.setValue(QLatin1String(AppSettings::kShowServerStats), m_settingsShowStats->isChecked());
    }
    if (m_settingsStatsInterval) {
        s.setValue(QLatin1String(AppSettings::kStatsIntervalSec), m_settingsStatsInterval->value());
    }
    if (m_settingsDefaultPort) {
        s.setValue(QLatin1String(AppSettings::kDefaultPort), m_settingsDefaultPort->value());
    }
    if (m_settingsHideDotfiles) {
        s.setValue(QLatin1String(AppSettings::kHideDotfiles), m_settingsHideDotfiles->isChecked());
    }
    if (m_settingsSftpView) {
        s.setValue(QLatin1String(AppSettings::kSftpDefaultView),
                   m_settingsSftpView->currentData().toString());
    }
    s.sync();
}

void DashboardPage::persistShortcutsLive()
{
    QSettings s;
    s.setValue(QLatin1String(AppSettings::kCtrlScrollFontZoom), m_settingsCtrlScrollZoom->isChecked());
    s.setValue(QLatin1String(AppSettings::kScrollSensitivity), m_settingsScrollSensitivity->value());
    s.setValue(QLatin1String(AppSettings::kCopyPasteMode), m_settingsCopyPaste->currentData().toString());
    s.setValue(QLatin1String(AppSettings::kShortcutNewSession),
               m_shortcutNewSession->keySequence().toString(QKeySequence::PortableText));
    s.setValue(QLatin1String(AppSettings::kShortcutSettings),
               m_shortcutSettings->keySequence().toString(QKeySequence::PortableText));
    s.setValue(QLatin1String(AppSettings::kShortcutDashboard),
               m_shortcutDashboard->keySequence().toString(QKeySequence::PortableText));
    s.setValue(QLatin1String(AppSettings::kShortcutClosePanel),
               m_shortcutClosePanel->keySequence().toString(QKeySequence::PortableText));
    s.setValue(QLatin1String(AppSettings::kShortcutOpenSftp),
               m_shortcutOpenSftp->keySequence().toString(QKeySequence::PortableText));
    s.setValue(QLatin1String(AppSettings::kShortcutClearTerminal),
               m_shortcutClearTerminal->keySequence().toString(QKeySequence::PortableText));
    s.setValue(QLatin1String(AppSettings::kShortcutFontLarger),
               m_shortcutFontLarger->keySequence().toString(QKeySequence::PortableText));
    s.setValue(QLatin1String(AppSettings::kShortcutFontSmaller),
               m_shortcutFontSmaller->keySequence().toString(QKeySequence::PortableText));
    s.setValue(QLatin1String(AppSettings::kShortcutFontReset),
               m_shortcutFontReset->keySequence().toString(QKeySequence::PortableText));
    s.setValue(QLatin1String(AppSettings::kShortcutNewSessionEnabled), m_enableNewSession->isChecked());
    s.setValue(QLatin1String(AppSettings::kShortcutSettingsEnabled), m_enableSettings->isChecked());
    s.setValue(QLatin1String(AppSettings::kShortcutDashboardEnabled), m_enableDashboard->isChecked());
    s.setValue(QLatin1String(AppSettings::kShortcutClosePanelEnabled), m_enableClosePanel->isChecked());
    s.setValue(QLatin1String(AppSettings::kShortcutOpenSftpEnabled), m_enableOpenSftp->isChecked());
    s.setValue(QLatin1String(AppSettings::kShortcutClearTerminalEnabled),
               m_enableClearTerminal->isChecked());
    s.setValue(QLatin1String(AppSettings::kShortcutFontLargerEnabled), m_enableFontLarger->isChecked());
    s.setValue(QLatin1String(AppSettings::kShortcutFontSmallerEnabled), m_enableFontSmaller->isChecked());
    s.setValue(QLatin1String(AppSettings::kShortcutFontResetEnabled), m_enableFontReset->isChecked());
    s.sync();
    emit settingsApplied();
}

void DashboardPage::resetShortcutsToDefaults()
{
    const QList<QKeySequenceEdit*> edits = {
        m_shortcutNewSession, m_shortcutSettings, m_shortcutDashboard, m_shortcutClosePanel,
        m_shortcutOpenSftp,   m_shortcutClearTerminal, m_shortcutFontLarger,
        m_shortcutFontSmaller, m_shortcutFontReset};
    for (QKeySequenceEdit* e : edits) {
        if (e) {
            e->blockSignals(true);
        }
    }
    m_settingsCtrlScrollZoom->blockSignals(true);
    m_settingsCtrlScrollZoom->setChecked(true);
    const QList<QCheckBox*> enables = {
        m_enableNewSession, m_enableSettings, m_enableDashboard, m_enableClosePanel,
        m_enableOpenSftp,   m_enableClearTerminal, m_enableFontLarger,
        m_enableFontSmaller, m_enableFontReset};
    for (QCheckBox* c : enables) {
        if (c) {
            c->blockSignals(true);
            c->setChecked(true);
        }
    }
    m_shortcutNewSession->setKeySequence(QKeySequence(QStringLiteral("Ctrl+N")));
    m_shortcutSettings->setKeySequence(QKeySequence(QStringLiteral("Ctrl+,")));
    m_shortcutDashboard->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Shift+D")));
    m_shortcutClosePanel->setKeySequence(QKeySequence(QStringLiteral("Ctrl+W")));
    m_shortcutOpenSftp->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    m_shortcutClearTerminal->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Shift+K")));
    m_shortcutFontLarger->setKeySequence(QKeySequence(QStringLiteral("Ctrl+=")));
    m_shortcutFontSmaller->setKeySequence(QKeySequence(QStringLiteral("Ctrl+-")));
    m_shortcutFontReset->setKeySequence(QKeySequence(QStringLiteral("Ctrl+0")));
    m_settingsCtrlScrollZoom->blockSignals(false);
    for (QKeySequenceEdit* e : edits) {
        if (e) {
            e->blockSignals(false);
        }
    }
    for (QCheckBox* c : enables) {
        if (c) {
            c->blockSignals(false);
        }
    }
    persistShortcutsLive();
}

void DashboardPage::syncTerminalFontSizeUi(int points)
{
    if (!m_settingsFontSize) {
        return;
    }
    const int clamped = qBound(9, points, 22);
    AppSettings::setFontSize(clamped);
    if (m_settingsFontSize->value() == clamped) {
        return;
    }
    m_settingsFontSize->blockSignals(true);
    m_settingsFontSize->setValue(clamped);
    m_settingsFontSize->blockSignals(false);
}

void DashboardPage::updateTopBar()
{
    const bool hosts = (m_currentNav == NavPage::Hosts);
    const bool form = (m_currentNav == NavPage::Form);
    m_searchEdit->setVisible(hosts);
    m_newSessionBtn->setVisible(!form);

    switch (m_currentNav) {
    case NavPage::Hosts:
        m_pageTitle->setText(QStringLiteral("Hosts"));
        m_pageSub->setText(QStringLiteral("saved connection profiles"));
        break;
    case NavPage::Active: // unreachable (entry removed)
    case NavPage::Keychain:
        m_pageTitle->setText(QStringLiteral("SSH Keys"));
        m_pageSub->setText(QStringLiteral("stored keys and SSH agent status"));
        break;
    case NavPage::Logs:
        m_pageTitle->setText(QStringLiteral("Logs"));
        m_pageSub->setText(QStringLiteral("recent session events"));
        break;
    case NavPage::Settings:
        m_pageTitle->setText(QStringLiteral("Settings"));
        m_pageSub->setText(QStringLiteral("preferences by category"));
        break;
    case NavPage::Form:
        m_pageTitle->setText(m_formTitle->text());
        m_pageSub->setText(m_formSub ? m_formSub->text() : QStringLiteral("connection details"));
        break;
    }
}

void DashboardPage::setNavPage(NavPage page)
{
    m_currentNav = page;
    switch (page) {
    case NavPage::Hosts:
    case NavPage::Active: // deprecated entry; fall back to the Hosts page
        m_stack->setCurrentWidget(m_hostsPage);
        m_navHosts->setChecked(true);
        break;
    case NavPage::Keychain:
        rebuildKeychainList();
        m_stack->setCurrentWidget(m_keysPage);
        m_navKeys->setChecked(true);
        break;
    case NavPage::Logs:
        m_stack->setCurrentWidget(m_logsPage);
        m_navLogs->setChecked(true);
        break;
    case NavPage::Settings:
        loadSettingsUi();
        m_stack->setCurrentWidget(m_settingsPage);
        m_navSettings->setChecked(true);
        break;
    case NavPage::Form:
        m_stack->setCurrentWidget(m_formPage);
        break;
    }
    updateTopBar();
}

void DashboardPage::appendLog(const QString& line)
{
    if (!m_logsView) {
        return;
    }
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    m_logsView->appendPlainText(QStringLiteral("[%1] %2").arg(stamp, line));
}

void DashboardPage::rebuildLists()
{
    rebuildSavedList();
    rebuildActiveList();
    if (m_currentNav == NavPage::Keychain) {
        rebuildKeychainList();
    }
}

void DashboardPage::refresh()
{
    m_profiles = loadProfiles();
    rebuildLists();
}

void DashboardPage::showHome()
{
    refresh();
    setNavPage(NavPage::Hosts);
}

void DashboardPage::showNewSessionForm()
{
    clearForm();
    m_editingId.clear();
    m_formTitle->setText(QStringLiteral("New Session"));
    if (m_formSub) {
        m_formSub->setText(QStringLiteral("Connection details for a saved host"));
    }
    m_saveProfileBtn->setText(QStringLiteral("Save"));
    m_saveProfileBtn->setObjectName(QStringLiteral("dashButton"));
    m_saveProfileBtn->style()->unpolish(m_saveProfileBtn);
    m_saveProfileBtn->style()->polish(m_saveProfileBtn);
    m_connectProfileBtn->show();
    m_savePass->setChecked(AppSettings::savePasswordDefault());
    setNavPage(NavPage::Form);
    m_hostEdit->setFocus();
}

void DashboardPage::showEditSessionForm(const QString& profileId)
{
    const int row = profileIndexById(profileId);
    if (row < 0) {
        m_hint->setText(QStringLiteral("session not found"));
        return;
    }
    loadProfileIntoForm(m_profiles[row]);
    m_editingId = m_profiles[row].id;
    m_formTitle->setText(QStringLiteral("Edit Session"));
    if (m_formSub) {
        m_formSub->setText(QStringLiteral("Update connection details"));
    }
    m_saveProfileBtn->setText(QStringLiteral("Save"));
    m_saveProfileBtn->setObjectName(QStringLiteral("dashPrimary"));
    m_saveProfileBtn->style()->unpolish(m_saveProfileBtn);
    m_saveProfileBtn->style()->polish(m_saveProfileBtn);
    m_connectProfileBtn->hide();
    setNavPage(NavPage::Form);
    m_nameEdit->setFocus();
}

void DashboardPage::clearForm()
{
    m_nameEdit->clear();
    m_connectionModeCombo->setCurrentIndex(0);
    m_hostEdit->setText(AppSettings::defaultHost());
    m_portSpin->setValue(AppSettings::defaultPort());
    m_userEdit->setText(AppSettings::defaultUser());
    if (m_serialBaudCombo) m_serialBaudCombo->setCurrentText(QStringLiteral("115200"));
    if (m_serialDataBitsCombo) m_serialDataBitsCombo->setCurrentText(QStringLiteral("8"));
    if (m_serialParityCombo) m_serialParityCombo->setCurrentText(QStringLiteral("none"));
    if (m_serialStopBitsCombo) m_serialStopBitsCombo->setCurrentText(QStringLiteral("1"));
    if (m_serialFlowCombo) m_serialFlowCombo->setCurrentText(QStringLiteral("none"));
    m_passEdit->clear();
    m_savePass->setChecked(AppSettings::savePasswordDefault());
    m_keyPathEdit->clear();
    reloadKeyringCombo();
    if (m_keyringCombo->count() > 0) {
        m_keyringCombo->setCurrentIndex(0);
    }
    onKeyringSelectionChanged(m_keyringCombo->currentIndex());
    m_keyPassEdit->clear();
    m_saveKeyPass->setChecked(false);
    if (m_authMethodCombo) {
        const int authIdx = m_authMethodCombo->findData(AppSettings::lastAuthMethod());
        m_authMethodCombo->setCurrentIndex(authIdx >= 0 ? authIdx : 0);
    }
    updateConnectionModeUi();
}

void DashboardPage::loadProfileIntoForm(const SessionProfile& profile)
{
    m_nameEdit->setText(profile.name);
    const int modeIdx = m_connectionModeCombo->findData(static_cast<int>(profile.connectionMode));
    m_connectionModeCombo->setCurrentIndex(modeIdx >= 0 ? modeIdx : 0);
    m_hostEdit->setText(profile.host);
    m_portSpin->setValue(profile.port);
    m_userEdit->setText(profile.user);
    if (m_serialPortCombo && profile.isSerial()) m_serialPortCombo->setCurrentText(profile.host);
    if (m_serialBaudCombo) m_serialBaudCombo->setCurrentText(QString::number(profile.serialBaudRate));
    if (m_serialDataBitsCombo) m_serialDataBitsCombo->setCurrentText(QString::number(profile.serialDataBits));
    if (m_serialParityCombo) m_serialParityCombo->setCurrentText(profile.serialParity);
    if (m_serialStopBitsCombo) m_serialStopBitsCombo->setCurrentText(QString::number(profile.serialStopBits));
    if (m_serialFlowCombo) m_serialFlowCombo->setCurrentText(profile.serialFlowControl);
    m_passEdit->setText(profile.password);
    m_savePass->setChecked(profile.savePassword);
    m_keyPathEdit->setText(profile.privateKeyPath);
    reloadKeyringCombo();

    const int authMethod = static_cast<int>(profile.authMethod);
    if (profile.authMethod == AuthMethod::StoredKey) {
        const int idx = m_keyringCombo->findData(profile.privateKeyId);
        m_keyringCombo->setCurrentIndex(idx >= 0 ? idx : 0);
        m_keyPathEdit->clear();
    } else if (profile.authMethod == AuthMethod::KeyFile) {
        if (m_keyringCombo->count() > 0) {
            m_keyringCombo->setCurrentIndex(0);
        }
    } else if (m_keyringCombo->count() > 0) {
        m_keyringCombo->setCurrentIndex(0);
    }
    onKeyringSelectionChanged(m_keyringCombo->currentIndex());
    m_keyPassEdit->setText(profile.keyPassphrase);
    m_saveKeyPass->setChecked(profile.saveKeyPassphrase);
    if (m_authMethodCombo) {
        const int authIdx = m_authMethodCombo->findData(authMethod);
        m_authMethodCombo->setCurrentIndex(authIdx >= 0 ? authIdx : 0);
    }
    updateConnectionModeUi();
}

void DashboardPage::browsePrivateKey()
{
    const QString startDir = m_keyPathEdit->text().trimmed().isEmpty()
        ? QDir::homePath() + QStringLiteral("/.ssh")
        : QFileInfo(m_keyPathEdit->text()).absolutePath();

    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select private key"),
        startDir,
        QStringLiteral("Private keys (id_rsa id_ed25519 id_ecdsa id_dsa *);;All files (*)"));
    if (!path.isEmpty()) {
        m_keyPathEdit->setText(QDir::toNativeSeparators(path));
        if (m_authMethodCombo) {
            const int idx = m_authMethodCombo->findData(static_cast<int>(AuthMethod::KeyFile));
            if (idx >= 0) {
                m_authMethodCombo->setCurrentIndex(idx);
            }
            updateAuthMethodUi();
        }
    }
}

void DashboardPage::reloadKeyringCombo()
{
    if (!m_keyringCombo) {
        return;
    }
    const QString prevId = m_keyringCombo->currentData().toString();
    m_keyringCombo->blockSignals(true);
    m_keyringCombo->clear();
    VaultManager vault;
    if (!vault.usingNativeKeyring()) {
        m_keysStatus->setText(QStringLiteral(
            "Security warning: OS keyring is unavailable; the encrypted machine-bound fallback is active."));
    } else if (m_keysStatus->text().startsWith(QStringLiteral("Security warning:"))) {
        m_keysStatus->clear();
    }
    const QVector<StoredKey> keys = vault.listStoredKeys();
    if (keys.isEmpty()) {
        m_keyringCombo->addItem(QStringLiteral("No keys yet - import one"), QString());
    } else {
        for (const StoredKey& k : keys) {
            QString label = k.name.trimmed().isEmpty() ? QStringLiteral("(unnamed)") : k.name;
            if (!k.type.trimmed().isEmpty()) {
                label += QStringLiteral("  ·  %1").arg(k.type);
            }
            if (k.hasPassphrase) {
                label += QStringLiteral("  ·  passphrase");
            }
            m_keyringCombo->addItem(label, k.id);
        }
    }
    const int idx = m_keyringCombo->findData(prevId);
    m_keyringCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    m_keyringCombo->blockSignals(false);
}

void DashboardPage::updateConnectionModeUi()
{
    if (!m_connectionModeCombo) {
        return;
    }
    const auto mode = static_cast<ConnectionMode>(m_connectionModeCombo->currentData().toInt());
    const bool telnet = (mode == ConnectionMode::Telnet);
    const bool serial = (mode == ConnectionMode::Serial);

    if (telnet && m_portSpin->value() == AppSettings::defaultPort()) {
        m_portSpin->setValue(23);
    }

    if (m_networkFieldsPanel) m_networkFieldsPanel->setVisible(!serial);
    if (m_serialFieldsPanel) m_serialFieldsPanel->setVisible(serial);
    if (m_authSectionTitle) m_authSectionTitle->setVisible(!serial);
    if (m_authSectionRule) m_authSectionRule->setVisible(!serial);
    if (m_authMethodLabel) m_authMethodLabel->setVisible(!serial);
    if (m_authMethodCombo) {
        m_authMethodCombo->setVisible(!telnet && !serial);
    }
    if ((telnet || serial) && m_authMethodCombo) {
        m_authMethodCombo->setCurrentIndex(0);
    }
    updateAuthMethodUi();
}

void DashboardPage::updateAuthMethodUi()
{
    if (!m_authMethodCombo) {
        return;
    }
    const auto mode = m_connectionModeCombo
        ? static_cast<ConnectionMode>(m_connectionModeCombo->currentData().toInt())
        : ConnectionMode::Ssh;
    const bool telnet = (mode == ConnectionMode::Telnet);
    const bool serial = (mode == ConnectionMode::Serial);
    const int method = m_authMethodCombo->currentData().toInt();
    const bool password = !serial && (telnet || (method == static_cast<int>(AuthMethod::Password)));
    const bool keyring = !telnet && !serial && (method == static_cast<int>(AuthMethod::StoredKey));
    const bool keyFile = !telnet && !serial && (method == static_cast<int>(AuthMethod::KeyFile));
    if (m_authPasswordPanel) {
        m_authPasswordPanel->setVisible(password);
    }
    if (m_authKeyringPanel) {
        m_authKeyringPanel->setVisible(keyring);
    }
    if (m_authKeyFilePanel) {
        m_authKeyFilePanel->setVisible(keyFile);
    }
    if (m_authPassphrasePanel) {
        m_authPassphrasePanel->setVisible(keyring || keyFile);
    }
    if (m_saveKeyPass) {
        m_saveKeyPass->setText(keyring
            ? QStringLiteral("Save passphrase with this key")
            : QStringLiteral("Save passphrase with this profile"));
    }
}

void DashboardPage::onKeyringSelectionChanged(int /*index*/)
{
    if (!m_keyringCombo || !m_keyPassEdit || !m_saveKeyPass) return;
    const QString id = m_keyringCombo->currentData().toString();
    QByteArray passphrase;
    VaultManager vault;
    const bool stored = !id.isEmpty() && vault.retrieveStoredKeyPassphrase(id, passphrase);
    m_keyPassEdit->setText(stored ? QString::fromUtf8(passphrase) : QString());
    m_saveKeyPass->setChecked(stored);
    passphrase.fill('\0');
}

void DashboardPage::fillProfileFromForm(SessionProfile* profile) const
{
    if (!profile) {
        return;
    }
    profile->name = m_nameEdit->text().trimmed();
    profile->connectionMode = static_cast<ConnectionMode>(m_connectionModeCombo->currentData().toInt());
    profile->host = m_hostEdit->text().trimmed();
    profile->port = m_portSpin->value();
    profile->user = m_userEdit->text().trimmed();

    if (profile->isSerial()) {
        profile->host = m_serialPortCombo->currentText().trimmed();
        profile->port = 0;
        profile->user.clear();
        profile->password.clear();
        profile->savePassword = false;
        profile->privateKeyId.clear();
        profile->privateKeyPath.clear();
        profile->keyPassphrase.clear();
        profile->saveKeyPassphrase = false;
        profile->authMethod = AuthMethod::Password;
        profile->serialBaudRate = m_serialBaudCombo->currentText().toInt();
        profile->serialDataBits = m_serialDataBitsCombo->currentText().toInt();
        profile->serialParity = m_serialParityCombo->currentText();
        profile->serialStopBits = m_serialStopBitsCombo->currentText().toInt();
        profile->serialFlowControl = m_serialFlowCombo->currentText();
        profile->system = QStringLiteral("Serial");
        return;
    }

    if (profile->isTelnet()) {
        profile->privateKeyId.clear();
        profile->privateKeyPath.clear();
        profile->password = m_passEdit->text();
        profile->savePassword = m_savePass->isChecked();
        profile->keyPassphrase.clear();
        profile->saveKeyPassphrase = false;
        profile->authMethod = AuthMethod::Password;
        if (!profile->savePassword) {
            profile->password.clear();
        }
        return;
    }

    const int method = m_authMethodCombo ? m_authMethodCombo->currentData().toInt() : 0;
    profile->authMethod = static_cast<AuthMethod>(method);
    if (profile->authMethod == AuthMethod::StoredKey) {
        profile->privateKeyId = m_keyringCombo->currentData().toString();
        profile->privateKeyPath.clear();
        profile->password.clear();
        profile->savePassword = false;
        profile->keyPassphrase = m_keyPassEdit->text();
        profile->saveKeyPassphrase = m_saveKeyPass->isChecked();
    } else if (profile->authMethod == AuthMethod::KeyFile) {
        // Key file
        profile->privateKeyId.clear();
        profile->privateKeyPath = m_keyPathEdit->text().trimmed();
        profile->password.clear();
        profile->savePassword = false;
        profile->keyPassphrase = m_keyPassEdit->text();
        profile->saveKeyPassphrase = m_saveKeyPass->isChecked();
    } else if (profile->authMethod == AuthMethod::Password) {
        // Password
        profile->privateKeyId.clear();
        profile->privateKeyPath.clear();
        profile->password = m_passEdit->text();
        profile->savePassword = m_savePass->isChecked();
        profile->keyPassphrase.clear();
        profile->saveKeyPassphrase = false;
        if (!profile->savePassword) {
            profile->password.clear();
        }
    } else {
        // SSH agent owns the key material and passphrase.
        profile->privateKeyId.clear();
        profile->privateKeyPath.clear();
        profile->password.clear();
        profile->savePassword = false;
        profile->keyPassphrase.clear();
        profile->saveKeyPassphrase = false;
    }
}

void DashboardPage::importKeyIntoKeyring()
{
    const QString startDir = QDir::homePath() + QStringLiteral("/.ssh");
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Import SSH private key"),
        startDir,
        QStringLiteral("Private keys (id_rsa id_ed25519 id_ecdsa id_dsa *);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_keysStatus->setText(QStringLiteral("cannot read key file: %1").arg(path));
        return;
    }
    QByteArray pem = f.readAll();
    f.close();
    const QString baseName = QFileInfo(path).fileName();

    bool passphraseRequired = false;
    QByteArray validatedPassphrase;
    ssh_key parsedKey = nullptr;
    int importRc = ssh_pki_import_privkey_base64(pem.constData(), nullptr,
                                                  detectPrivateKeyPassphrase,
                                                  &passphraseRequired,
                                                  &parsedKey);
    if (importRc != SSH_OK || !parsedKey) {
        if (parsedKey) {
            ssh_key_free(parsedKey);
            parsedKey = nullptr;
        }
        if (!passphraseRequired) {
            wipeBytes(pem);
            m_keysStatus->setText(QStringLiteral("selected file is not a valid private key"));
            return;
        }

        bool passphraseAccepted = false;
        QString passphrase = QInputDialog::getText(
            this,
            QStringLiteral("Encrypted private key"),
            QStringLiteral("Enter the key passphrase to validate it:"),
            QLineEdit::Password,
            QString(),
            &passphraseAccepted);
        if (!passphraseAccepted) {
            wipeBytes(pem);
            return;
        }

        QByteArray passphraseUtf8 = passphrase.toUtf8();
        importRc = ssh_pki_import_privkey_base64(
            pem.constData(), passphraseUtf8.constData(), nullptr, nullptr, &parsedKey);
        if (importRc == SSH_OK && parsedKey) {
            validatedPassphrase = passphraseUtf8;
        }
        passphrase.fill(QChar(u'\0'));
        wipeBytes(passphraseUtf8);
        if (importRc != SSH_OK || !parsedKey) {
            wipeBytes(pem);
            m_keysStatus->setText(QStringLiteral("wrong passphrase or invalid private key"));
            return;
        }
    }

    const char* parsedType = ssh_key_type_to_char(ssh_key_type(parsedKey));

    bool ok = false;
    const QString displayName = QInputDialog::getText(
        this,
        QStringLiteral("SSH key name"),
        QStringLiteral("Give this key a memorable name:"),
        QLineEdit::Normal,
        baseName,
        &ok);
    if (!ok) {
        ssh_key_free(parsedKey);
        wipeBytes(pem);
        wipeBytes(validatedPassphrase);
        return;
    }

    StoredKey key;
    key.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    key.name = displayName.trimmed().isEmpty() ? baseName : displayName.trimmed();
    key.type = parsedType ? QString::fromLatin1(parsedType) : QStringLiteral("unknown");
    unsigned char* hash = nullptr;
    size_t hashLen = 0;
    if (ssh_get_publickey_hash(parsedKey, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLen) == SSH_OK) {
        char* fingerprint = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLen);
        if (fingerprint) {
            key.fingerprint = QString::fromLatin1(fingerprint);
            ssh_string_free_char(fingerprint);
        }
        ssh_clean_pubkey_hash(&hash);
    }
    key.pem = pem;
    key.hasPassphrase = passphraseRequired;
    ssh_key_free(parsedKey);

    VaultManager vault;
    const bool stored = vault.storeStoredKey(key);
    wipeBytes(key.pem);
    wipeBytes(pem);
    if (!stored) {
        wipeBytes(validatedPassphrase);
        m_keysStatus->setText(QStringLiteral("failed to store key in the encrypted vault"));
        return;
    }
    bool passphraseStoreFailed = false;
    if (key.hasPassphrase && !validatedPassphrase.isEmpty()) {
        const auto remember = QMessageBox::question(
            this, QStringLiteral("Remember passphrase"),
            QStringLiteral("Save this passphrase in the encrypted vault for every profile using this key?"));
        if (remember == QMessageBox::Yes
            && !vault.storeStoredKeyPassphrase(key.id, validatedPassphrase)) {
            passphraseStoreFailed = true;
            m_keysStatus->setText(QStringLiteral("key imported, but its passphrase could not be saved"));
        }
    }
    wipeBytes(validatedPassphrase);
    reloadKeyringCombo();
    const int idx = m_keyringCombo->findData(key.id);
    if (idx >= 0) {
        m_keyringCombo->setCurrentIndex(idx);
    }
    if (m_authMethodCombo) {
        const int authIdx = m_authMethodCombo->findData(static_cast<int>(AuthMethod::StoredKey));
        if (authIdx >= 0) {
            m_authMethodCombo->setCurrentIndex(authIdx);
        }
        updateAuthMethodUi();
    }
    onKeyringSelectionChanged(m_keyringCombo->currentIndex());
    rebuildKeychainList();
    if (!passphraseStoreFailed) {
        m_keysStatus->setText(QStringLiteral("key '%1' saved to the encrypted vault").arg(key.name));
    }
    appendLog(QStringLiteral("imported SSH key '%1'").arg(key.name));
}

void DashboardPage::renameSelectedStoredKey()
{
    const int row = m_keysTable ? m_keysTable->currentRow() : -1;
    if (row < 0 || !m_keysTable->item(row, 0)) return;
    const QString id = m_keysTable->item(row, 0)->data(Qt::UserRole).toString();
    StoredKey key;
    VaultManager vault;
    if (!vault.retrieveStoredKey(id, key)) {
        m_keysStatus->setText(QStringLiteral("selected key is missing from the vault"));
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename SSH key"),
                                                QStringLiteral("Key name:"), QLineEdit::Normal,
                                                key.name, &accepted).trimmed();
    if (!accepted || name.isEmpty() || name == key.name) {
        key.pem.fill('\0');
        return;
    }
    key.name = name;
    const bool saved = vault.storeStoredKey(key);
    key.pem.fill('\0');
    if (!saved) {
        m_keysStatus->setText(QStringLiteral("failed to rename the key"));
        return;
    }
    reloadKeyringCombo();
    rebuildKeychainList();
    m_keysStatus->setText(QStringLiteral("key renamed to '%1'").arg(name));
}

void DashboardPage::editSelectedStoredKeyPassphrase()
{
    const int row = m_keysTable ? m_keysTable->currentRow() : -1;
    if (row < 0 || !m_keysTable->item(row, 0)) return;
    const QString id = m_keysTable->item(row, 0)->data(Qt::UserRole).toString();
    StoredKey key;
    VaultManager vault;
    if (!vault.retrieveStoredKey(id, key) || !key.hasPassphrase) {
        key.pem.fill('\0');
        m_keysStatus->setText(QStringLiteral("this key does not require a passphrase"));
        return;
    }

    bool accepted = false;
    QString passphrase = QInputDialog::getText(
        this, QStringLiteral("Stored key passphrase"),
        QStringLiteral("Enter a passphrase to save, or leave empty to forget the saved passphrase:"),
        QLineEdit::Password, QString(), &accepted);
    if (!accepted) {
        key.pem.fill('\0');
        return;
    }
    if (passphrase.isEmpty()) {
        key.pem.fill('\0');
        bool removed = vault.removeStoredKeyPassphrase(id);
        for (SessionProfile& profile : m_profiles) {
            if (profile.authMethod == AuthMethod::StoredKey && profile.privateKeyId == id) {
                profile.keyPassphrase.clear();
                profile.saveKeyPassphrase = false;
            }
        }
        removed = saveProfiles(m_profiles) && removed;
        if (removed) {
            m_keysStatus->setText(QStringLiteral("saved passphrase removed"));
            rebuildKeychainList();
        } else {
            m_keysStatus->setText(QStringLiteral("failed to remove the saved passphrase"));
        }
        return;
    }

    QByteArray passphraseUtf8 = passphrase.toUtf8();
    passphrase.fill(QChar(u'\0'));
    ssh_key parsed = nullptr;
    const int rc = ssh_pki_import_privkey_base64(
        key.pem.constData(), passphraseUtf8.constData(), nullptr, nullptr, &parsed);
    key.pem.fill('\0');
    if (parsed) ssh_key_free(parsed);
    if (rc != SSH_OK) {
        wipeBytes(passphraseUtf8);
        m_keysStatus->setText(QStringLiteral("wrong passphrase"));
        return;
    }
    bool saved = vault.storeStoredKeyPassphrase(id, passphraseUtf8);
    if (saved) {
        const QString runtimePassphrase = QString::fromUtf8(passphraseUtf8);
        for (SessionProfile& profile : m_profiles) {
            if (profile.authMethod == AuthMethod::StoredKey && profile.privateKeyId == id) {
                profile.keyPassphrase = runtimePassphrase;
                profile.saveKeyPassphrase = true;
            }
        }
        saved = saveProfiles(m_profiles);
    }
    wipeBytes(passphraseUtf8);
    m_keysStatus->setText(saved ? QStringLiteral("passphrase saved with this key")
                               : QStringLiteral("failed to save the passphrase"));
    if (saved) rebuildKeychainList();
}

void DashboardPage::removeSelectedKeyringKey()
{
    const int row = m_keysTable ? m_keysTable->currentRow() : -1;
    if (row < 0 || !m_keysTable->item(row, 0)) return;
    const QString id = m_keysTable->item(row, 0)->data(Qt::UserRole).toString();
    const QString label = m_keysTable->item(row, 0)->text();
    QStringList usedBy;
    for (const SessionProfile& profile : m_profiles) {
        if (profile.authMethod == AuthMethod::StoredKey && profile.privateKeyId == id) {
            usedBy.push_back(profile.displayTitle());
        }
    }
    if (!usedBy.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("SSH key is in use"),
                             QStringLiteral("This key is used by: %1. Change those profiles before removing it.")
                                 .arg(usedBy.join(QStringLiteral(", "))));
        return;
    }
    const auto answer = QMessageBox::question(
        this,
        QStringLiteral("Remove SSH key"),
        QStringLiteral("Permanently remove \"%1\" from the encrypted vault?").arg(label));
    if (answer != QMessageBox::Yes) {
        return;
    }
    VaultManager vault;
    if (!vault.removeStoredKey(id)) {
        m_keysStatus->setText(QStringLiteral("failed to remove the key"));
        return;
    }
    reloadKeyringCombo();
    rebuildKeychainList();
    m_keysStatus->setText(QStringLiteral("key removed from the vault"));
    appendLog(QStringLiteral("removed SSH key '%1'").arg(label));
}

void DashboardPage::showSettings()
{
    setNavPage(NavPage::Settings);
}

int DashboardPage::profileIndexById(const QString& id) const
{
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id == id) {
            return i;
        }
    }
    return -1;
}

void DashboardPage::openSavedProfile(const QString& profileId)
{
    const int row = profileIndexById(profileId);
    if (row < 0) {
        return;
    }
    const SessionProfile& p = m_profiles[row];
    if (p.isSftpOnly()) {
        emit openSftpForProfile(p);
    } else {
        emit openProfile(p);
    }
}

void DashboardPage::sftpSavedProfile(const QString& profileId)
{
    const int row = profileIndexById(profileId);
    if (row < 0) {
        return;
    }
    // Standalone SFTP: always open a new independent pane (own SSH session inside
    // SftpClient). Never require or reuse a live SSH tab — multiple SFTPs to the
    // same host/port/user can coexist.
    emit openSftpForProfile(m_profiles[row]);
}

void DashboardPage::deleteSavedProfile(const QString& profileId)
{
    const int row = profileIndexById(profileId);
    if (row < 0) {
        return;
    }
    const QString title = m_profiles[row].displayTitle();
    const SessionProfile removed = m_profiles[row];
    m_profiles.removeAt(row);
    if (!saveProfiles(m_profiles)) {
        m_profiles.insert(row, removed);
        m_hint->setText(QStringLiteral("failed to persist profile deletion"));
        return;
    }
    if (m_syncSaveDebounce && m_sync && m_sync->state() == SyncController::State::Active
        && !m_sync->isPaused()) {
        m_syncSaveDebounce->start();
    }
    rebuildSavedList();
    rebuildKeychainList();
    m_hint->setText(QStringLiteral("deleted %1").arg(title));
    appendLog(QStringLiteral("deleted profile %1").arg(title));
}

void DashboardPage::setProfileSystem(const QString& profileId, const QString& system)
{
    const int row = profileIndexById(profileId);
    if (row < 0) {
        return;
    }
    if (m_profiles[row].system == system) {
        return;
    }
    m_profiles[row].system = system;
    saveProfiles(m_profiles);
    rebuildSavedList();
}

bool DashboardPage::isProfileSaved(const SessionProfile& profile) const
{
    for (const SessionProfile& saved : m_profiles) {
        if (!profile.id.isEmpty() && saved.id == profile.id) {
            return true;
        }
        if (saved.host.compare(profile.host, Qt::CaseInsensitive) == 0
            && saved.port == profile.port
            && saved.user.compare(profile.user, Qt::CaseSensitive) == 0
            && saved.connectionMode == profile.connectionMode) {
            return true;
        }
    }
    return false;
}

void DashboardPage::saveSessionProfile(const SessionProfile& profile)
{
    if (profile.host.trimmed().isEmpty()) {
        return;
    }
    if (isProfileSaved(profile)) {
        m_hint->setText(QStringLiteral("profile already saved"));
        rebuildSavedList();
        return;
    }

    SessionProfile saved = profile;
    if (saved.id.isEmpty()) {
        saved.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!saved.savePassword) {
        saved.password.clear();
    }
    if (!saved.saveKeyPassphrase) {
        saved.keyPassphrase.clear();
    }
    m_profiles.push_back(saved);
    if (!saveProfiles(m_profiles)) {
        m_profiles.removeLast();
        m_hint->setText(QStringLiteral("failed to save profile secrets"));
        return;
    }
    if (m_syncSaveDebounce && m_sync && m_sync->state() == SyncController::State::Active
        && !m_sync->isPaused()) {
        m_syncSaveDebounce->start();
    }
    rebuildSavedList();
    rebuildKeychainList();
    m_hint->setText(QStringLiteral("saved %1").arg(saved.displayTitle()));
    appendLog(QStringLiteral("saved live session %1").arg(saved.displayTitle()));
}

void DashboardPage::saveLiveSession(const QString& sessionId)
{
    if (const auto* live = m_sessions->session(sessionId)) {
        saveSessionProfile(live->profile);
    }
}

void DashboardPage::showLiveSessionContextMenu(const QPoint& globalPos, const QString& sessionId)
{
    const auto* live = m_sessions->session(sessionId);
    if (!live) {
        return;
    }
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("dashContextMenu"));
    auto* focus = menu.addAction(QStringLiteral("Open session"));
    connect(focus, &QAction::triggered, this, [this, sessionId]() {
        emit openLiveSession(sessionId);
    });
    auto* save = menu.addAction(QStringLiteral("Save connection profile"));
    save->setEnabled(!isProfileSaved(live->profile));
    connect(save, &QAction::triggered, this, [this, sessionId]() {
        saveLiveSession(sessionId);
    });
    menu.exec(globalPos);
}

void DashboardPage::rebuildSavedList()
{
    if (!m_savedTree) {
        return;
    }
    // Rebuilding and filtering change expansion programmatically; only direct
    // user actions should update the persisted folder state.
    const QSignalBlocker rebuildSignals(m_savedTree);
    m_savedTree->clear();

    const int baseFontSize = AppSettings::uiFontSize();
    const int folderFontSize = qMax(8, baseFontSize - 1);
    const int nameFontSize = qMax(8, baseFontSize - 1);
    const int metadataFontSize = qMax(8, baseFontSize - 2);
    const QColor primaryGray = AppSettings::isLightTheme() ? QColor(0x2a, 0x2a, 0x2a)
                                                           : QColor(0xb8, 0xb8, 0xb8);
    const QColor secondaryGray = AppSettings::isLightTheme() ? QColor(0x5a, 0x5a, 0x5a)
                                                             : QColor(0x8a, 0x8a, 0x8a);
    const auto applyFolderFont = [this, folderFontSize](QTreeWidgetItem* item) {
        QFont font = item->font(0);
        font.setPointSize(folderFontSize);
        font.setBold(false);
        for (int column = 0; column < m_savedTree->columnCount(); ++column) {
            item->setFont(column, font);
        }
        item->setSizeHint(0, QSize(0, QFontMetrics(font).height() + 10));
    };
    const auto applyEntryStyle = [nameFontSize, metadataFontSize, secondaryGray](QTreeWidgetItem* item) {
        QFont nameFont = item->font(0);
        nameFont.setPointSize(nameFontSize);
        item->setFont(0, nameFont);

        QFont metadataFont = nameFont;
        metadataFont.setPointSize(metadataFontSize);
        for (int column = 1; column <= 3; ++column) {
            item->setFont(column, metadataFont);
            item->setForeground(column, secondaryGray);
            item->setTextAlignment(column, Qt::AlignLeft | Qt::AlignVCenter);
        }
        item->setSizeHint(0, QSize(0, QFontMetrics(nameFont).height() + 6));
    };

    QHash<QString, QString> storedKeyNames;
    {
        VaultManager vault;
        for (const StoredKey& key : vault.listStoredKeys()) {
            storedKeyNames.insert(key.id, key.name.trimmed());
        }
    }
    const auto authLabel = [&storedKeyNames](const SessionProfile& profile) {
        if (profile.isSerial()) return QStringLiteral("—");
        if (profile.isTelnet()) return QStringLiteral("Password / prompt");
        switch (profile.authMethod) {
        case AuthMethod::SshAgent:
            return QStringLiteral("SSH Agent");
        case AuthMethod::StoredKey: {
            const QString name = storedKeyNames.value(profile.privateKeyId);
            return name.isEmpty() ? QStringLiteral("Stored key")
                                  : QStringLiteral("Key · %1").arg(name);
        }
        case AuthMethod::KeyFile: {
            const QString name = QFileInfo(profile.privateKeyPath).fileName();
            return name.isEmpty() ? QStringLiteral("Key file")
                                  : QStringLiteral("File · %1").arg(name);
        }
        case AuthMethod::Password:
        default:
            return QStringLiteral("Password");
        }
    };

    // Reload tag state from disk (assignments + collapse).
    m_tags = AppSettings::tagDefinitions();
    m_tagAssignments = AppSettings::tagAssignments();
    m_tagCollapsed = AppSettings::tagCollapsed();

    const QStringList liveIds = m_sessions->sessionIds();
    if (!liveIds.isEmpty()) {
        auto* liveHeader = new QTreeWidgetItem(m_savedTree);
        liveHeader->setText(0, QStringLiteral("Current sessions  (%1)").arg(liveIds.size()));
        liveHeader->setData(0, Qt::UserRole + 1, QStringLiteral("live-header"));
        liveHeader->setData(0, kHostFolderStateRole, hostLiveFolderKey());
        liveHeader->setFlags(Qt::ItemIsEnabled);
        applyFolderFont(liveHeader);
        liveHeader->setForeground(0, primaryGray);

        for (const QString& sessionId : liveIds) {
            const auto* live = m_sessions->session(sessionId);
            if (!live) {
                continue;
            }
            auto* child = new QTreeWidgetItem(liveHeader);
            child->setText(0, live->profile.displayTitle());
            child->setData(0, Qt::UserRole, sessionId);
            child->setData(0, Qt::UserRole + 1, QStringLiteral("live"));
            child->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            child->setText(1, live->profile.connectionTypeLabel());
            child->setText(2, authLabel(live->profile));
            child->setText(3, live->profile.systemLabel());
            applyEntryStyle(child);

            auto* save = makeRowAction(QStringLiteral(":/icons/plus.svg"),
                                       QStringLiteral("save connection profile"), m_savedTree);
            const bool alreadySaved = isProfileSaved(live->profile);
            save->setEnabled(!alreadySaved);
            if (alreadySaved) {
                save->setToolTip(QStringLiteral("connection profile already saved"));
            }
            connect(save, &QToolButton::clicked, this, [this, sessionId]() {
                saveLiveSession(sessionId);
            });
            m_savedTree->setItemWidget(child, 4, save);
        }
        liveHeader->setExpanded(!m_tagCollapsed.contains(hostLiveFolderKey()));
    }

    // Map profileId → tag (first tag that contains it).
    QHash<QString, QString> profileTag;
    for (const QString& tag : m_tags) {
        for (const QString& id : m_tagAssignments.value(tag)) {
            if (!profileTag.contains(id)) {
                profileTag.insert(id, tag);
            }
        }
    }

    // Build one top-level section per tag, then an "Untagged" section.
    struct Section {
        QString title;
        QString tagName; // empty for Untagged
        QVector<SessionProfile> profiles;
    };
    QVector<Section> sections;
    for (const QString& tag : m_tags) {
        Section s;
        s.title = tag;
        s.tagName = tag;
        sections.append(s);
    }
    Section untagged;
    untagged.title = QStringLiteral("Untagged");

    for (const SessionProfile& p : m_profiles) {
        const QString tag = profileTag.value(p.id);
        bool placed = false;
        for (Section& s : sections) {
            if (s.tagName == tag) {
                s.profiles.append(p);
                placed = true;
                break;
            }
        }
        if (!placed) {
            untagged.profiles.append(p);
        }
    }

    if (!untagged.profiles.isEmpty()) {
        sections.append(untagged);
    }

    // Only show sections that have hosts (or, for tags, keep empty tags visible
    // so the user can manage them).
    for (const Section& s : sections) {
        if (s.profiles.isEmpty() && s.tagName.isEmpty()) {
            continue;
        }
        auto* header = new QTreeWidgetItem(m_savedTree);
        header->setText(0, s.profiles.isEmpty()
                               ? QStringLiteral("%1").arg(s.title)
                               : QStringLiteral("%1  (%2)").arg(s.title).arg(s.profiles.size()));
        header->setData(0, Qt::UserRole, s.tagName);
        const QString folderKey = hostTagFolderKey(s.tagName);
        header->setData(0, kHostFolderStateRole, folderKey);
        header->setFlags(Qt::ItemIsEnabled);
        applyFolderFont(header);
        header->setForeground(0, primaryGray);

        for (const SessionProfile& p : s.profiles) {
            auto* child = new QTreeWidgetItem(header);
            child->setText(0, p.displayTitle());
            child->setData(0, Qt::UserRole, p.id);
            child->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            child->setToolTip(0, QStringLiteral("Drag to reorder or move this host to another tag"));

            child->setText(1, p.connectionTypeLabel());

            child->setText(2, authLabel(p));
            child->setText(3, p.systemLabel());
            applyEntryStyle(child);

            auto* edit = makeRowAction(QStringLiteral(":/icons/edit.svg"),
                                       QStringLiteral("edit connection profile"), m_savedTree);
            connect(edit, &QToolButton::clicked, this, [this, profileId = p.id]() {
                showEditSessionForm(profileId);
            });
            m_savedTree->setItemWidget(child, 4, edit);
        }

        // Restore collapsed state.
        // Accept the legacy raw-tag format and migrate it on the next click.
        const bool collapsed = m_tagCollapsed.contains(folderKey)
            || m_tagCollapsed.contains(s.tagName);
        header->setExpanded(!collapsed);
    }

    applySavedFilter();
}

void DashboardPage::applySavedFilter()
{
    if (!m_savedTree) {
        return;
    }
    const QString q = m_searchEdit->text().trimmed();
    int visible = 0;
    const QSignalBlocker filterSignals(m_savedTree);

    for (int i = 0; i < m_savedTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* header = m_savedTree->topLevelItem(i);
        bool anyMatch = q.isEmpty();
        int childVisible = 0;

        for (int c = 0; c < header->childCount(); ++c) {
            QTreeWidgetItem* child = header->child(c);
            const QString id = child->data(0, Qt::UserRole).toString();
            const bool liveRow = child->data(0, Qt::UserRole + 1).toString()
                == QLatin1String("live");
            bool match = q.isEmpty();
            if (!match) {
                if (liveRow) {
                    if (const auto* live = m_sessions->session(id)) {
                        const SessionProfile& p = live->profile;
                        match = p.displayTitle().contains(q, Qt::CaseInsensitive)
                            || p.connectionTypeLabel().contains(q, Qt::CaseInsensitive)
                            || p.host.contains(q, Qt::CaseInsensitive)
                            || p.user.contains(q, Qt::CaseInsensitive)
                            || p.endpoint().contains(q, Qt::CaseInsensitive)
                            || live->status.contains(q, Qt::CaseInsensitive);
                    }
                } else if (const int idx = profileIndexById(id); idx >= 0) {
                    const SessionProfile& p = m_profiles[idx];
                    if (p.displayTitle().contains(q, Qt::CaseInsensitive)
                        || p.connectionTypeLabel().contains(q, Qt::CaseInsensitive)
                        || p.host.contains(q, Qt::CaseInsensitive)
                        || p.user.contains(q, Qt::CaseInsensitive)
                        || p.endpoint().contains(q, Qt::CaseInsensitive)) {
                        match = true;
                    }
                }
            }
            child->setHidden(!match);
            if (match) {
                ++childVisible;
                ++visible;
            }
        }

        // A tag header stays visible if it has any matching host, or if there is
        // no active filter (so empty tags remain manageable).
        const bool headerVisible = (q.isEmpty() && header->childCount() >= 0) || childVisible > 0;
        header->setHidden(!headerVisible);
        if (headerVisible) {
            anyMatch = childVisible > 0;
        }
        if (!q.isEmpty()) {
            // Temporarily expand matching folders without overwriting the
            // expansion state chosen by the user.
            if (anyMatch && headerVisible) {
                header->setExpanded(true);
            }
        } else {
            const bool liveFolder = header->data(0, Qt::UserRole + 1).toString()
                == QLatin1String("live-header");
            const QString folderKey = header->data(0, kHostFolderStateRole).toString();
            const QString legacyTag = header->data(0, Qt::UserRole).toString();
            const bool collapsed = m_tagCollapsed.contains(folderKey)
                || (!liveFolder && m_tagCollapsed.contains(legacyTag));
            header->setExpanded(!collapsed);
        }
    }

    if (m_profiles.isEmpty() && m_sessions->count() == 0) {
        m_savedTree->hide();
        m_savedEmpty->setText(QStringLiteral("no hosts yet — create a session to get started"));
        m_savedEmpty->show();
    } else if (visible == 0 && !q.isEmpty()) {
        m_savedTree->show();
        m_savedEmpty->setText(QStringLiteral("no hosts match the filter"));
        m_savedEmpty->show();
    } else {
        m_savedTree->show();
        m_savedEmpty->hide();
    }
}

QStringList DashboardPage::currentTags() const
{
    return m_tags;
}

void DashboardPage::persistTagCollapseState()
{
    AppSettings::setTagCollapsed(m_tagCollapsed);
}

void DashboardPage::addTagDialog()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Add Tag"), QStringLiteral("Tag name:"), QLineEdit::Normal, QString(), &ok);
    const QString trimmed = name.trimmed();
    if (!ok || trimmed.isEmpty()) {
        return;
    }
    if (m_tags.contains(trimmed)) {
        m_hint->setText(QStringLiteral("tag '%1' already exists").arg(trimmed));
        return;
    }
    m_tags.append(trimmed);
    AppSettings::setTagDefinitions(m_tags);
    m_tagAssignments.insert(trimmed, QStringList());
    AppSettings::setTagAssignments(m_tagAssignments);
    rebuildSavedList();
    m_hint->setText(QStringLiteral("added tag '%1'").arg(trimmed));
    appendLog(QStringLiteral("added tag %1").arg(trimmed));
}

void DashboardPage::renameTagDialog(const QString& tagName)
{
    if (tagName.isEmpty()) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Rename Tag"), QStringLiteral("New name:"), QLineEdit::Normal, tagName, &ok);
    const QString trimmed = name.trimmed();
    if (!ok || trimmed.isEmpty() || trimmed == tagName) {
        return;
    }
    if (m_tags.contains(trimmed)) {
        m_hint->setText(QStringLiteral("tag '%1' already exists").arg(trimmed));
        return;
    }
    const int idx = m_tags.indexOf(tagName);
    if (idx < 0) {
        return;
    }
    m_tags[idx] = trimmed;
    AppSettings::setTagDefinitions(m_tags);
    m_tagAssignments.insert(trimmed, m_tagAssignments.take(tagName));
    AppSettings::setTagAssignments(m_tagAssignments);
    const QString oldFolderKey = hostTagFolderKey(tagName);
    if (m_tagCollapsed.contains(oldFolderKey) || m_tagCollapsed.contains(tagName)) {
        m_tagCollapsed.removeAll(oldFolderKey);
        m_tagCollapsed.removeAll(tagName);
        m_tagCollapsed.append(hostTagFolderKey(trimmed));
        AppSettings::setTagCollapsed(m_tagCollapsed);
    }
    rebuildSavedList();
    m_hint->setText(QStringLiteral("renamed tag to '%1'").arg(trimmed));
    appendLog(QStringLiteral("renamed tag %1 → %2").arg(tagName, trimmed));
}

void DashboardPage::deleteTag(const QString& tagName)
{
    if (tagName.isEmpty()) {
        return;
    }
    const auto ret = QMessageBox::question(
        this, QStringLiteral("Delete Tag"),
        QStringLiteral("Delete tag '%1'? Hosts in this tag will become untagged.").arg(tagName),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) {
        return;
    }
    m_tags.removeAll(tagName);
    AppSettings::setTagDefinitions(m_tags);
    m_tagAssignments.remove(tagName);
    AppSettings::setTagAssignments(m_tagAssignments);
    m_tagCollapsed.removeAll(hostTagFolderKey(tagName));
    m_tagCollapsed.removeAll(tagName);
    AppSettings::setTagCollapsed(m_tagCollapsed);
    rebuildSavedList();
    m_hint->setText(QStringLiteral("deleted tag '%1'").arg(tagName));
    appendLog(QStringLiteral("deleted tag %1").arg(tagName));
}

void DashboardPage::moveProfileToTag(const QString& profileId, const QString& tagName)
{
    if (profileId.isEmpty()) {
        return;
    }
    // Remove the profile from every tag.
    for (auto it = m_tagAssignments.begin(); it != m_tagAssignments.end(); ++it) {
        it.value().removeAll(profileId);
    }
    if (!tagName.isEmpty()) {
        if (!m_tags.contains(tagName)) {
            return;
        }
        QStringList& list = m_tagAssignments[tagName];
        if (!list.contains(profileId)) {
            list.append(profileId);
        }
    }
    AppSettings::setTagAssignments(m_tagAssignments);
    rebuildSavedList();
}

bool DashboardPage::moveProfileToTagAt(const QString& profileId, const QString& tagName,
                                       const QString& beforeProfileId)
{
    const int sourceIndex = profileIndexById(profileId);
    if (sourceIndex < 0 || (!tagName.isEmpty() && !m_tags.contains(tagName))
        || profileId == beforeProfileId) {
        return false;
    }

    const QVector<SessionProfile> previousProfiles = m_profiles;
    const QHash<QString, QStringList> previousAssignments = m_tagAssignments;

    for (auto it = m_tagAssignments.begin(); it != m_tagAssignments.end(); ++it) {
        it.value().removeAll(profileId);
    }
    if (!tagName.isEmpty()) {
        QStringList& targetIds = m_tagAssignments[tagName];
        if (!targetIds.contains(profileId)) {
            targetIds.append(profileId);
        }
    }

    const auto tagForProfile = [this](const QString& id) {
        for (const QString& tag : m_tags) {
            if (m_tagAssignments.value(tag).contains(id)) {
                return tag;
            }
        }
        return QString();
    };

    const SessionProfile movedProfile = m_profiles.takeAt(sourceIndex);
    int insertionIndex = m_profiles.size();
    if (!beforeProfileId.isEmpty()) {
        insertionIndex = profileIndexById(beforeProfileId);
        if (insertionIndex < 0 || tagForProfile(beforeProfileId) != tagName) {
            m_profiles = previousProfiles;
            m_tagAssignments = previousAssignments;
            return false;
        }
    } else {
        int lastTargetIndex = -1;
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (tagForProfile(m_profiles[i].id) == tagName) {
                lastTargetIndex = i;
            }
        }
        insertionIndex = lastTargetIndex + 1;
        if (lastTargetIndex < 0) {
            insertionIndex = m_profiles.size();
        }
    }
    m_profiles.insert(insertionIndex, movedProfile);

    if (!saveProfiles(m_profiles)) {
        m_profiles = previousProfiles;
        m_tagAssignments = previousAssignments;
        return false;
    }

    AppSettings::setTagAssignments(m_tagAssignments);
    if (m_syncSaveDebounce && m_sync && m_sync->state() == SyncController::State::Active
        && !m_sync->isPaused()) {
        m_syncSaveDebounce->start();
    }
    rebuildSavedList();
    return true;
}

void DashboardPage::removeProfileFromTag(const QString& profileId)
{
    moveProfileToTag(profileId, QString());
}

void DashboardPage::showPageContextMenu(const QPoint& globalPos)
{
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("dashContextMenu"));
    auto* addTag = menu.addAction(QStringLiteral("Add Tag"));
    connect(addTag, &QAction::triggered, this, [this]() { addTagDialog(); });
    menu.exec(globalPos);
}

void DashboardPage::showTagContextMenu(const QPoint& globalPos, const QString& tagName)
{
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("dashContextMenu"));
    auto* addHost = menu.addAction(QStringLiteral("Add Host to Tag"));
    connect(addHost, &QAction::triggered, this, [this, tagName]() {
        // Let the user pick an existing host to add to this tag.
        QStringList hostNames;
        for (const SessionProfile& p : m_profiles) {
            hostNames << QStringLiteral("%1 (%2)").arg(p.displayTitle(), p.host);
        }
        if (hostNames.isEmpty()) {
            m_hint->setText(QStringLiteral("no hosts to add"));
            return;
        }
        bool ok = false;
        const QString choice = QInputDialog::getItem(
            this, QStringLiteral("Add Host to Tag"),
            QStringLiteral("Choose a host to add to '%1':").arg(tagName), hostNames, 0, false, &ok);
        if (!ok || choice.isEmpty()) {
            return;
        }
        const int idx = hostNames.indexOf(choice);
        if (idx < 0 || idx >= m_profiles.size()) {
            return;
        }
        const QString id = m_profiles[idx].id;
        QStringList& list = m_tagAssignments[tagName];
        if (!list.contains(id)) {
            list.append(id);
        }
        AppSettings::setTagAssignments(m_tagAssignments);
        rebuildSavedList();
    });
    menu.addSeparator();
    auto* rename = menu.addAction(QStringLiteral("Rename Tag"));
    connect(rename, &QAction::triggered, this, [this, tagName]() { renameTagDialog(tagName); });
    auto* del = menu.addAction(QStringLiteral("Delete Tag"));
    connect(del, &QAction::triggered, this, [this, tagName]() { deleteTag(tagName); });
    menu.exec(globalPos);
}

void DashboardPage::showHostContextMenu(const QPoint& globalPos, const QString& profileId)
{
    const int row = profileIndexById(profileId);
    if (row < 0) {
        return;
    }
    const SessionProfile& p = m_profiles[row];

    QMenu menu(this);
    menu.setObjectName(QStringLiteral("dashContextMenu"));

    // Move → submenu of tags (plus Untagged).
    auto* moveMenu = menu.addMenu(QStringLiteral("Move"));
    QStringList tags = m_tags;
    for (const QString& tag : tags) {
        auto* act = moveMenu->addAction(tag);
        connect(act, &QAction::triggered, this, [this, profileId, tag]() {
            moveProfileToTag(profileId, tag);
        });
    }
    moveMenu->addSeparator();
    auto* untag = moveMenu->addAction(QStringLiteral("Untagged"));
    connect(untag, &QAction::triggered, this, [this, profileId]() {
        removeProfileFromTag(profileId);
    });

    menu.addSeparator();

    auto* edit = menu.addAction(QStringLiteral("Edit"));
    connect(edit, &QAction::triggered, this, [this, profileId]() { showEditSessionForm(profileId); });

    auto* del = menu.addAction(QStringLiteral("Delete"));
    connect(del, &QAction::triggered, this, [this, profileId]() { deleteSavedProfile(profileId); });

    menu.addSeparator();

    auto* connectAct = menu.addAction(p.isTelnet() ? QStringLiteral("Connect via Telnet")
                                      : p.isSerial() ? QStringLiteral("Open serial port")
                                                     : QStringLiteral("Connect via SSH"));
    connect(connectAct, &QAction::triggered, this, [this, profileId]() { openSavedProfile(profileId); });

    if (!p.isTelnet() && !p.isSerial()) {
        auto* sftp = menu.addAction(QStringLiteral("Connect via SFTP"));
        connect(sftp, &QAction::triggered, this, [this, profileId]() { sftpSavedProfile(profileId); });
    }

    menu.addSeparator();

    auto* copyHost = menu.addAction(p.isSerial() ? QStringLiteral("Copy Port Name")
                                                  : QStringLiteral("Copy Hostname"));
    connect(copyHost, &QAction::triggered, this, [p]() {
        QApplication::clipboard()->setText(p.host);
    });

    menu.exec(globalPos);
}

void DashboardPage::rebuildActiveList()
{
    const QStringList ids = m_sessions->sessionIds();
    const int n = ids.size();

    // With the dedicated "Active" page removed, this only drives the small
    // "N live" badge shown under the Hosts entry in the sidebar.
    if (n == 0) {
        m_activeBadge->hide();
    } else {
        m_activeBadge->setText(QStringLiteral("%1 live").arg(n));
        m_activeBadge->show();
    }
}

void DashboardPage::rebuildKeychainList()
{
    m_keysTable->setRowCount(0);
    const int baseFontSize = AppSettings::uiFontSize();
    const int nameFontSize = qMax(8, baseFontSize - 1);
    const int metadataFontSize = qMax(8, baseFontSize - 2);
    const QColor metadataColor = AppSettings::isLightTheme() ? QColor(0x5a, 0x5a, 0x5a)
                                                              : QColor(0x8a, 0x8a, 0x8a);
    const QByteArray agentSocket = qgetenv("SSH_AUTH_SOCK");
#if defined(Q_OS_WIN)
    m_agentStatus->setText(agentSocket.isEmpty()
        ? QStringLiteral("SSH Agent · checked when connecting")
        : QStringLiteral("SSH Agent · available"));
#else
    m_agentStatus->setText(agentSocket.isEmpty()
        ? QStringLiteral("SSH Agent · unavailable (SSH_AUTH_SOCK is not set)")
        : QStringLiteral("SSH Agent · available"));
#endif

    QHash<QString, int> usage;
    for (const SessionProfile& profile : m_profiles) {
        if (profile.authMethod == AuthMethod::StoredKey && !profile.privateKeyId.isEmpty()) {
            usage[profile.privateKeyId] += 1;
        }
    }

    VaultManager vault;
    const QVector<StoredKey> keys = vault.listStoredKeys();
    for (const StoredKey& key : keys) {
        const int row = m_keysTable->rowCount();
        m_keysTable->insertRow(row);

        auto* nameItem = new QTableWidgetItem(
            key.name.trimmed().isEmpty() ? QStringLiteral("(unnamed)") : key.name);
        nameItem->setData(Qt::UserRole, key.id);
        nameItem->setData(Qt::UserRole + 1, key.hasPassphrase);
        nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        QFont nameFont = nameItem->font();
        nameFont.setPointSize(nameFontSize);
        nameItem->setFont(nameFont);
        nameItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        auto* typeItem = new QTableWidgetItem(key.type.isEmpty() ? QStringLiteral("—") : key.type);
        typeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        QFont metadataFont = nameFont;
        metadataFont.setPointSize(metadataFontSize);
        typeItem->setFont(metadataFont);
        typeItem->setForeground(metadataColor);

        QString fingerprint = key.fingerprint;
        if (fingerprint.isEmpty()) fingerprint = QStringLiteral("— (legacy key)");
        if (key.hasPassphrase) {
            QByteArray savedPassphrase;
            const bool passphraseSaved = vault.retrieveStoredKeyPassphrase(key.id, savedPassphrase);
            savedPassphrase.fill('\0');
            fingerprint += passphraseSaved ? QStringLiteral("  ·  passphrase saved")
                                           : QStringLiteral("  ·  passphrase required");
        }
        auto* fingerprintItem = new QTableWidgetItem(fingerprint);
        fingerprintItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        fingerprintItem->setFont(metadataFont);
        fingerprintItem->setForeground(metadataColor);

        auto* usageItem = new QTableWidgetItem(QString::number(usage.value(key.id)));
        usageItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        usageItem->setFont(metadataFont);
        usageItem->setForeground(metadataColor);
        usageItem->setTextAlignment(Qt::AlignCenter);

        m_keysTable->setItem(row, 0, nameItem);
        m_keysTable->setItem(row, 1, typeItem);
        m_keysTable->setItem(row, 2, fingerprintItem);
        m_keysTable->setItem(row, 3, usageItem);
    }

    if (keys.isEmpty()) {
        m_keysTable->hide();
        m_keysEmpty->show();
    } else {
        m_keysTable->show();
        m_keysEmpty->hide();
    }
    updateStoredKeyActions();
}

void DashboardPage::updateStoredKeyActions()
{
    const bool selected = m_keysTable && m_keysTable->currentRow() >= 0;
    if (m_renameKeyBtn) m_renameKeyBtn->setEnabled(selected);
    if (m_passphraseKeyBtn) {
        const bool encrypted = selected && m_keysTable->item(m_keysTable->currentRow(), 0)
            && m_keysTable->item(m_keysTable->currentRow(), 0)->data(Qt::UserRole + 1).toBool();
        m_passphraseKeyBtn->setEnabled(encrypted);
    }
    if (m_removeKeyBtn) m_removeKeyBtn->setEnabled(selected);
}

void DashboardPage::saveCurrentFormAsProfile()
{
    const auto mode = static_cast<ConnectionMode>(m_connectionModeCombo->currentData().toInt());
    const bool serial = (mode == ConnectionMode::Serial);
    const QString host = m_hostEdit->text().trimmed();
    const QString user = m_userEdit->text().trimmed();
    if (serial && m_serialPortCombo->currentText().trimmed().isEmpty()) {
        m_hint->setText(QStringLiteral("serial port required"));
        return;
    }
    if (!serial && (host.isEmpty() || user.isEmpty())) {
        m_hint->setText(QStringLiteral("host and user required"));
        return;
    }

    const int method = m_authMethodCombo ? m_authMethodCombo->currentData().toInt() : 0;
    if (!serial && method == 0 && m_passEdit->text().isEmpty()) {
        m_hint->setText(QStringLiteral("enter a password"));
        return;
    }
    if (!serial && method == 1 && m_keyringCombo->currentData().toString().isEmpty()) {
        m_hint->setText(QStringLiteral("import or select a stored SSH key"));
        return;
    }
    if (!serial && method == 2 && m_keyPathEdit->text().trimmed().isEmpty()) {
        m_hint->setText(QStringLiteral("choose a private key file"));
        return;
    }

    SessionProfile p;
    if (!m_editingId.isEmpty()) {
        p.id = m_editingId;
    } else {
        p = makeProfile(host, m_portSpin->value(), user, QString(), m_nameEdit->text());
    }
    fillProfileFromForm(&p);
    if (!serial) {
        p.host = host;
        p.user = user;
    }
    if (!p.savePassword) {
        p.password.clear();
    }
    if (!p.saveKeyPassphrase) {
        p.keyPassphrase.clear();
    }

    if (!m_editingId.isEmpty()) {
        bool updated = false;
        for (SessionProfile& existing : m_profiles) {
            if (existing.id == m_editingId) {
                if (method == 0 && p.savePassword && m_passEdit->text().isEmpty()
                    && !existing.password.isEmpty()) {
                    p.password = existing.password;
                }
                if (method != 0 && p.saveKeyPassphrase && m_keyPassEdit->text().isEmpty()
                    && !existing.keyPassphrase.isEmpty()) {
                    p.keyPassphrase = existing.keyPassphrase;
                }
                existing = p;
                updated = true;
                break;
            }
        }
        if (!updated) {
            m_profiles.push_back(p);
        }
        m_hint->setText(QStringLiteral("session updated"));
        appendLog(QStringLiteral("updated profile %1").arg(p.displayTitle()));
    } else {
        m_profiles.push_back(p);
        m_hint->setText(QStringLiteral("profile saved"));
        appendLog(QStringLiteral("saved profile %1").arg(p.displayTitle()));
    }

    if (!saveProfiles(m_profiles)) {
        m_hint->setText(QStringLiteral("failed to save profile or its secrets"));
        appendLog(QStringLiteral("failed to persist profile %1").arg(p.displayTitle()));
        return;
    }
    if (m_syncSaveDebounce && m_sync && m_sync->state() == SyncController::State::Active
        && !m_sync->isPaused()) {
        m_syncSaveDebounce->start();
    }
    m_editingId.clear();
    showHome();
}

void DashboardPage::connectFromForm()
{
    const auto mode = static_cast<ConnectionMode>(m_connectionModeCombo->currentData().toInt());
    const bool serial = (mode == ConnectionMode::Serial);
    const QString host = m_hostEdit->text().trimmed();
    const QString user = m_userEdit->text().trimmed();
    if (serial && m_serialPortCombo->currentText().trimmed().isEmpty()) {
        m_hint->setText(QStringLiteral("serial port required"));
        return;
    }
    if (!serial && (host.isEmpty() || user.isEmpty())) {
        m_hint->setText(QStringLiteral("host and user required"));
        return;
    }

    const bool telnet = (mode == ConnectionMode::Telnet);

    const int method = m_authMethodCombo ? m_authMethodCombo->currentData().toInt() : 0;
    if (!telnet && !serial) {
        if (method == 0 && m_passEdit->text().isEmpty() && m_editingId.isEmpty()) {
            m_hint->setText(QStringLiteral("enter a password"));
            return;
        }
        if (method == 1 && m_keyringCombo->currentData().toString().isEmpty()) {
            m_hint->setText(QStringLiteral("import or select a stored SSH key"));
            return;
        }
        if (method == 2 && m_keyPathEdit->text().trimmed().isEmpty()) {
            m_hint->setText(QStringLiteral("choose a private key file"));
            return;
        }
    }

    SessionProfile p = makeProfile(host, m_portSpin->value(), user, QString(), m_nameEdit->text());
    fillProfileFromForm(&p);

    if (!serial && method == 0 && p.password.isEmpty() && !m_editingId.isEmpty()) {
        for (const SessionProfile& existing : m_profiles) {
            if (existing.id == m_editingId && !existing.password.isEmpty()) {
                p.password = existing.password;
                break;
            }
        }
    }
    if (!serial && method != 0 && p.keyPassphrase.isEmpty() && !m_editingId.isEmpty()) {
        for (const SessionProfile& existing : m_profiles) {
            if (existing.id == m_editingId && !existing.keyPassphrase.isEmpty()) {
                p.keyPassphrase = existing.keyPassphrase;
                break;
            }
        }
    }

    if (p.isSftpOnly()) {
        emit openSftpForProfile(p);
    } else {
        emit openProfile(p);
    }
}

void DashboardPage::loadSettingsUi()
{
    // Do not keep a long-lived QSettings here — its destructor sync can overwrite
    // concurrent writes (e.g. Motion::setEnabled) with a stale snapshot.
    {
        const QSignalBlocker b1(m_settingsSavePassDefault);
        const QSignalBlocker b2(m_settingsDefaultHost);
        const QSignalBlocker b3(m_settingsDefaultUser);
        m_settingsSavePassDefault->setChecked(AppSettings::savePasswordDefault());
        m_settingsDefaultHost->setText(AppSettings::defaultHost());
        m_settingsDefaultUser->setText(AppSettings::defaultUser());
    }

    // Block live-persist handlers while hydrating Appearance controls.
    const QList<QObject*> appearanceBlocks = {
        m_settingsTheme,           m_settingsUiFontFamily, m_settingsUiFontSize,
        m_settingsFontFamily,      m_settingsFontSize,     m_settingsHighlightAddresses,
        m_settingsHighlightKeywords, m_settingsHighlightCiscoCli,
        m_settingsTermBgImageBtn, m_settingsTermBgOpacity, m_settingsTermBgBlur};
    for (QObject* o : appearanceBlocks) {
        if (auto* w = qobject_cast<QWidget*>(o)) {
            w->blockSignals(true);
        }
    }

    const QString theme = AppSettings::theme();
    const int themeIdx = m_settingsTheme->findData(theme);
    m_settingsTheme->setCurrentIndex(themeIdx >= 0 ? themeIdx : 0);

    populateFontCombos();

    auto selectFamily = [](QComboBox* box, const QString& family) {
        int idx = box->findData(family);
        if (idx < 0 && !family.isEmpty()) {
            box->addItem(family, family);
            idx = box->findData(family);
        }
        box->setCurrentIndex(idx >= 0 ? idx : 0);
    };
    selectFamily(m_settingsUiFontFamily, AppSettings::uiFontFamily());
    selectFamily(m_settingsFontFamily, AppSettings::fontFamily());
    m_settingsUiFontSize->setValue(AppSettings::uiFontSize());
    m_settingsFontSize->setValue(AppSettings::fontSize());
    m_settingsHighlightAddresses->setChecked(AppSettings::highlightAddresses());
    m_settingsHighlightKeywords->setChecked(AppSettings::highlightLogKeywords());
    m_settingsHighlightCiscoCli->setChecked(AppSettings::highlightCiscoCli());

    m_termFg = AppSettings::terminalFg();
    m_termBg = AppSettings::terminalBg();
    syncColorSwatch(m_settingsTermFgBtn, m_termFg);
    syncColorSwatch(m_settingsTermBgBtn, m_termBg);

    const QString bgImagePath = AppSettings::terminalBgImage();
    if (!bgImagePath.isEmpty()) {
        m_settingsTermBgImage->setText(QFileInfo(bgImagePath).fileName());
    } else {
        m_settingsTermBgImage->setText(QStringLiteral("None"));
    }
    m_settingsTermBgOpacity->setValue(int(AppSettings::terminalBgOpacity() * 100));
    m_settingsTermBgBlur->setValue(AppSettings::terminalBgBlur());

    if (m_settingsTermPreview) {
        m_settingsTermPreview->setFont(
            clientoshMonospaceFont(m_settingsFontSize->value(), m_settingsFontFamily->currentData().toString()));
        m_settingsTermPreview->setStyleSheet(
            QStringLiteral("QLabel#settingsTermPreview { color: %1; background: %2; padding: 8px; }")
                .arg(m_termFg.name(), m_termBg.name()));
    }

    for (QObject* o : appearanceBlocks) {
        if (auto* w = qobject_cast<QWidget*>(o)) {
            w->blockSignals(false);
        }
    }
    ensureSelectedFonts();

    const bool animations = AppSettings::animationsEnabled();
    m_settingsAnimations->blockSignals(true);
    m_settingsAnimations->setChecked(animations);
    m_settingsAnimations->blockSignals(false);
    Motion::setEnabled(animations);

    {
        const QSignalBlocker b1(m_settingsShowStats);
        const QSignalBlocker b2(m_settingsStatsInterval);
        const QSignalBlocker b3(m_settingsDefaultPort);
        const QSignalBlocker b4(m_settingsHideDotfiles);
        const QSignalBlocker b5(m_settingsSftpView);
        m_settingsShowStats->setChecked(AppSettings::showServerStats());
        m_settingsStatsInterval->setValue(AppSettings::statsIntervalSec());
        m_settingsStatsInterval->setEnabled(m_settingsShowStats->isChecked());
        m_settingsDefaultPort->setValue(AppSettings::defaultPort());
        m_settingsHideDotfiles->setChecked(AppSettings::hideDotfiles());
        const int viewIdx = m_settingsSftpView->findData(AppSettings::sftpDefaultView());
        m_settingsSftpView->setCurrentIndex(viewIdx >= 0 ? viewIdx : 0);
    }
    if (m_settingsSftpVerbose) {
        m_settingsSftpVerbose->blockSignals(true);
        m_settingsSftpVerbose->setChecked(AppSettings::sftpVerboseLogging());
        m_settingsSftpVerbose->blockSignals(false);
    }

    const QList<QKeySequenceEdit*> shortcutEdits = {
        m_shortcutNewSession, m_shortcutSettings, m_shortcutDashboard, m_shortcutClosePanel,
        m_shortcutOpenSftp,   m_shortcutClearTerminal, m_shortcutFontLarger,
        m_shortcutFontSmaller, m_shortcutFontReset};
    for (QKeySequenceEdit* e : shortcutEdits) {
        if (e) {
            e->blockSignals(true);
        }
    }
    m_settingsCtrlScrollZoom->blockSignals(true);
    m_settingsCtrlScrollZoom->setChecked(AppSettings::ctrlScrollFontZoom());

    if (m_settingsScrollSensitivity) {
        m_settingsScrollSensitivity->blockSignals(true);
        const int sens = AppSettings::scrollSensitivity();
        m_settingsScrollSensitivity->setValue(sens);
        m_settingsScrollSensitivityValue->setText(QStringLiteral("%1 lines").arg(sens));
        m_settingsScrollSensitivity->blockSignals(false);
    }
    if (m_settingsCopyPaste) {
        m_settingsCopyPaste->blockSignals(true);
        const int cpIdx = m_settingsCopyPaste->findData(AppSettings::copyPasteMode());
        m_settingsCopyPaste->setCurrentIndex(cpIdx >= 0 ? cpIdx : 0);
        m_settingsCopyPaste->blockSignals(false);
    }

    // Hydrate enable toggles independently of the key fields.
    const QList<QCheckBox*> shortcutEnables = {
        m_enableNewSession, m_enableSettings, m_enableDashboard, m_enableClosePanel,
        m_enableOpenSftp,   m_enableClearTerminal, m_enableFontLarger,
        m_enableFontSmaller, m_enableFontReset};
    const QList<const char*> shortcutEnableKeys = {
        AppSettings::kShortcutNewSessionEnabled, AppSettings::kShortcutSettingsEnabled,
        AppSettings::kShortcutDashboardEnabled,  AppSettings::kShortcutClosePanelEnabled,
        AppSettings::kShortcutOpenSftpEnabled,    AppSettings::kShortcutClearTerminalEnabled,
        AppSettings::kShortcutFontLargerEnabled,
        AppSettings::kShortcutFontSmallerEnabled, AppSettings::kShortcutFontResetEnabled};
    for (int i = 0; i < shortcutEnables.size(); ++i) {
        QCheckBox* c = shortcutEnables[i];
        if (c) {
            c->blockSignals(true);
            const bool on = AppSettings::shortcutEnabled(shortcutEnableKeys[i]);
            c->setChecked(on);
            c->blockSignals(false);
        }
    }
    // Reflect enable state on the key fields.
    const QList<QCheckBox*> enableWidgets = shortcutEnables;
    const QList<QKeySequenceEdit*> editWidgets = shortcutEdits;
    for (int i = 0; i < enableWidgets.size(); ++i) {
        if (enableWidgets[i] && editWidgets[i]) {
            editWidgets[i]->setEnabled(enableWidgets[i]->isChecked());
        }
    }

    // Load stored bindings even when disabled — shortcutX() returns empty when
    // off, and persisting that would wipe the user's key sequences.
    m_shortcutNewSession->setKeySequence(
        AppSettings::shortcutFromSetting(AppSettings::kShortcutNewSession,
                                         QKeySequence(QStringLiteral("Ctrl+N"))));
    m_shortcutSettings->setKeySequence(
        AppSettings::shortcutFromSetting(AppSettings::kShortcutSettings,
                                         QKeySequence(QStringLiteral("Ctrl+,"))));
    m_shortcutDashboard->setKeySequence(
        AppSettings::shortcutFromSetting(AppSettings::kShortcutDashboard,
                                         QKeySequence(QStringLiteral("Ctrl+Shift+D"))));
    m_shortcutClosePanel->setKeySequence(
        AppSettings::shortcutFromSetting(AppSettings::kShortcutClosePanel,
                                         QKeySequence(QStringLiteral("Ctrl+W"))));
    m_shortcutOpenSftp->setKeySequence(
        AppSettings::shortcutFromSetting(AppSettings::kShortcutOpenSftp,
                                         QKeySequence(QStringLiteral("Ctrl+Shift+S"))));
    m_shortcutClearTerminal->setKeySequence(
        AppSettings::shortcutFromSetting(AppSettings::kShortcutClearTerminal,
                                         QKeySequence(QStringLiteral("Ctrl+Shift+K"))));
    m_shortcutFontLarger->setKeySequence(
        AppSettings::shortcutFromSetting(AppSettings::kShortcutFontLarger,
                                         QKeySequence(QStringLiteral("Ctrl+="))));
    m_shortcutFontSmaller->setKeySequence(
        AppSettings::shortcutFromSetting(AppSettings::kShortcutFontSmaller,
                                         QKeySequence(QStringLiteral("Ctrl+-"))));
    m_shortcutFontReset->setKeySequence(
        AppSettings::shortcutFromSetting(AppSettings::kShortcutFontReset,
                                         QKeySequence(QStringLiteral("Ctrl+0"))));
    m_settingsCtrlScrollZoom->blockSignals(false);
    for (QKeySequenceEdit* e : shortcutEdits) {
        if (e) {
            e->blockSignals(false);
        }
    }

    if (m_settingsNav && m_settingsNav->currentRow() < 0) {
        m_settingsNav->setCurrentRow(0);
    }
    setSettingsCategory(m_settingsNav ? m_settingsNav->currentRow() : 0);
}

void DashboardPage::saveSettingsUi()
{
    const QString bgImagePath = AppSettings::terminalBgImage();

    // Single QSettings write pass — destroy it before any other writer
    // (persistSyncLive / Motion::setEnabled), or ~QSettings will restore a
    // stale snapshot and wipe those keys.
    {
        QSettings s;
        s.setValue(QLatin1String(AppSettings::kSavePasswordDefault), m_settingsSavePassDefault->isChecked());
        s.setValue(QLatin1String(AppSettings::kDefaultHost), m_settingsDefaultHost->text().trimmed().isEmpty()
                                                                 ? QStringLiteral("127.0.0.1")
                                                                 : m_settingsDefaultHost->text().trimmed());
        s.setValue(QLatin1String(AppSettings::kDefaultUser), m_settingsDefaultUser->text().trimmed());
        s.setValue(QLatin1String(AppSettings::kTheme), m_settingsTheme->currentData().toString());
        s.setValue(QLatin1String(AppSettings::kUiFontFamily), m_settingsUiFontFamily->currentData().toString());
        s.setValue(QLatin1String(AppSettings::kUiFontSize), m_settingsUiFontSize->value());
        s.setValue(QLatin1String(AppSettings::kFontFamily), m_settingsFontFamily->currentData().toString());
        s.setValue(QLatin1String(AppSettings::kFontSize), qBound(9, m_settingsFontSize->value(), 22));
        s.setValue(QLatin1String(AppSettings::kTerminalFg), m_termFg.name(QColor::HexRgb));
        s.setValue(QLatin1String(AppSettings::kTerminalBg), m_termBg.name(QColor::HexRgb));
        s.setValue(QLatin1String(AppSettings::kTerminalBgImage), bgImagePath);
        if (m_settingsTermBgOpacity) {
            s.setValue(QLatin1String(AppSettings::kTerminalBgOpacity),
                       qBound(0.0, qreal(m_settingsTermBgOpacity->value()) / 100.0, 1.0));
        }
        if (m_settingsTermBgBlur) {
            s.setValue(QLatin1String(AppSettings::kTerminalBgBlur),
                       qBound(0, m_settingsTermBgBlur->value(), 100));
        }
        if (m_settingsHighlightAddresses) {
            s.setValue(QLatin1String(AppSettings::kHighlightAddresses),
                       m_settingsHighlightAddresses->isChecked());
        }
        if (m_settingsHighlightKeywords) {
            s.setValue(QLatin1String(AppSettings::kHighlightLogKeywords),
                       m_settingsHighlightKeywords->isChecked());
        }
        if (m_settingsHighlightCiscoCli) {
            s.setValue(QLatin1String(AppSettings::kHighlightCiscoCli),
                       m_settingsHighlightCiscoCli->isChecked());
        }
        s.setValue(QLatin1String(AppSettings::kAnimationsEnabled), m_settingsAnimations->isChecked());
        s.setValue(QLatin1String(AppSettings::kShowServerStats), m_settingsShowStats->isChecked());
        s.setValue(QLatin1String(AppSettings::kStatsIntervalSec), m_settingsStatsInterval->value());
        s.setValue(QLatin1String(AppSettings::kDefaultPort), m_settingsDefaultPort->value());
        s.setValue(QLatin1String(AppSettings::kHideDotfiles), m_settingsHideDotfiles->isChecked());
        s.setValue(QLatin1String(AppSettings::kSftpDefaultView), m_settingsSftpView->currentData().toString());
        if (m_settingsSftpVerbose) {
            s.setValue(QLatin1String(AppSettings::kSftpVerboseLogging), m_settingsSftpVerbose->isChecked());
        }

        s.setValue(QLatin1String(AppSettings::kCtrlScrollFontZoom), m_settingsCtrlScrollZoom->isChecked());
        s.setValue(QLatin1String(AppSettings::kScrollSensitivity), m_settingsScrollSensitivity->value());
        s.setValue(QLatin1String(AppSettings::kCopyPasteMode), m_settingsCopyPaste->currentData().toString());
        s.setValue(QLatin1String(AppSettings::kShortcutNewSession),
                   m_shortcutNewSession->keySequence().toString(QKeySequence::PortableText));
        s.setValue(QLatin1String(AppSettings::kShortcutSettings),
                   m_shortcutSettings->keySequence().toString(QKeySequence::PortableText));
        s.setValue(QLatin1String(AppSettings::kShortcutDashboard),
                   m_shortcutDashboard->keySequence().toString(QKeySequence::PortableText));
        s.setValue(QLatin1String(AppSettings::kShortcutClosePanel),
                   m_shortcutClosePanel->keySequence().toString(QKeySequence::PortableText));
        s.setValue(QLatin1String(AppSettings::kShortcutOpenSftp),
                   m_shortcutOpenSftp->keySequence().toString(QKeySequence::PortableText));
        s.setValue(QLatin1String(AppSettings::kShortcutClearTerminal),
                   m_shortcutClearTerminal->keySequence().toString(QKeySequence::PortableText));
        s.setValue(QLatin1String(AppSettings::kShortcutFontLarger),
                   m_shortcutFontLarger->keySequence().toString(QKeySequence::PortableText));
        s.setValue(QLatin1String(AppSettings::kShortcutFontSmaller),
                   m_shortcutFontSmaller->keySequence().toString(QKeySequence::PortableText));
        s.setValue(QLatin1String(AppSettings::kShortcutFontReset),
                   m_shortcutFontReset->keySequence().toString(QKeySequence::PortableText));
        s.setValue(QLatin1String(AppSettings::kShortcutNewSessionEnabled), m_enableNewSession->isChecked());
        s.setValue(QLatin1String(AppSettings::kShortcutSettingsEnabled), m_enableSettings->isChecked());
        s.setValue(QLatin1String(AppSettings::kShortcutDashboardEnabled), m_enableDashboard->isChecked());
        s.setValue(QLatin1String(AppSettings::kShortcutClosePanelEnabled), m_enableClosePanel->isChecked());
        s.setValue(QLatin1String(AppSettings::kShortcutOpenSftpEnabled), m_enableOpenSftp->isChecked());
        s.setValue(QLatin1String(AppSettings::kShortcutClearTerminalEnabled),
                   m_enableClearTerminal->isChecked());
        s.setValue(QLatin1String(AppSettings::kShortcutFontLargerEnabled), m_enableFontLarger->isChecked());
        s.setValue(QLatin1String(AppSettings::kShortcutFontSmallerEnabled), m_enableFontSmaller->isChecked());
        s.setValue(QLatin1String(AppSettings::kShortcutFontResetEnabled), m_enableFontReset->isChecked());
        s.sync();
    }

    persistSyncLive();

    Motion::setEnabled(m_settingsAnimations->isChecked());
    ensureSelectedFonts();

    m_applyingAppearance = true;
    const QList<QCheckBox*> boxes = {m_settingsHighlightAddresses, m_settingsHighlightKeywords,
                                     m_settingsHighlightCiscoCli};
    for (QCheckBox* b : boxes) {
        if (b) {
            b->blockSignals(true);
        }
    }
    emit settingsApplied();
    for (QCheckBox* b : boxes) {
        if (b) {
            b->blockSignals(false);
        }
    }
    m_applyingAppearance = false;

    m_hint->setText(QStringLiteral("settings saved"));
    appendLog(QStringLiteral("settings saved"));
    showHome();
}

// ---- Addons marketplace -------------------------------------------------

void DashboardPage::persistAddonsRepoUrl()
{
    if (!m_addonsRepoEdit) {
        return;
    }
    AddonConfig::setRepositoryUrl(m_addonsRepoEdit->text().trimmed());
}

void DashboardPage::rebuildAddonsList()
{
    if (!m_addonsListLay) {
        return;
    }
    while (QLayoutItem* item = m_addonsListLay->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }

    QHash<QString, AddonInstallRecord> installedById;
    if (m_addonStore) {
        for (const AddonInstallRecord& r : m_addonStore->installed()) {
            installedById.insert(r.id, r);
        }
    }

    QVector<AddonCatalogEntry> catalog;
    if (m_addonStore && m_addonStore->hasCatalog()) {
        catalog = m_addonStore->catalog().addons;
    }

    // Also show installed addons that are no longer in the catalog.
    QSet<QString> seen;
    auto addRow = [&](const QString& id, const QString& name, const QString& version,
                      const QString& description, const QString& author, bool compatible,
                      bool fromCatalog) {
        seen.insert(id);
        const bool installed = installedById.contains(id);
        const AddonInstallRecord rec = installedById.value(id);
        const bool enabled = installed && AddonConfig::isEnabled(id);

        auto* row = new QWidget(m_addonsListHost);
        auto* lay = new QVBoxLayout(row);
        lay->setContentsMargins(8, 8, 8, 8);
        lay->setSpacing(4);

        auto* title = new QLabel(name.isEmpty() ? id : name, row);
        title->setObjectName(QStringLiteral("settingsSubsection"));
        QString meta = version;
        if (!author.isEmpty()) {
            meta += QStringLiteral(" · %1").arg(author);
        }
        if (installed) {
            meta += QStringLiteral(" · installed");
            if (!rec.version.isEmpty() && rec.version != version && fromCatalog) {
                meta += QStringLiteral(" (local %1)").arg(rec.version);
            }
        } else if (!compatible) {
            meta += QStringLiteral(" · not available for this platform");
        }
        auto* metaLab = new QLabel(meta, row);
        metaLab->setObjectName(QStringLiteral("dashHint"));

        auto* desc = new QLabel(description.isEmpty() ? QStringLiteral("No description.") : description,
                                row);
        desc->setObjectName(QStringLiteral("dashHint"));
        desc->setWordWrap(true);

        auto* actions = new QHBoxLayout;
        actions->setContentsMargins(0, 0, 0, 0);
        actions->setSpacing(6);

        if (installed) {
            auto* enable = new QCheckBox(QStringLiteral("Enabled"), row);
            enable->setChecked(enabled);
            enable->setFocusPolicy(Qt::NoFocus);
            connect(enable, &QCheckBox::toggled, this, [this, id](bool on) {
                if (m_addonHost) {
                    m_addonHost->setAddonEnabled(id, on);
                }
            });
            actions->addWidget(enable);

            auto* remove = new QPushButton(QStringLiteral("Remove"), row);
            remove->setObjectName(QStringLiteral("dashButton"));
            remove->setFocusPolicy(Qt::NoFocus);
            remove->setCursor(Qt::PointingHandCursor);
            connect(remove, &QPushButton::clicked, this, [this, id]() {
                if (m_addonStore) {
                    m_addonStore->removeAddon(id);
                }
            });
            actions->addWidget(remove);
        } else {
            auto* install = new QPushButton(QStringLiteral("Install"), row);
            install->setObjectName(QStringLiteral("dashPrimary"));
            install->setFocusPolicy(Qt::NoFocus);
            install->setCursor(Qt::PointingHandCursor);
            install->setEnabled(compatible && m_addonStore && !m_addonStore->busy());
            connect(install, &QPushButton::clicked, this, [this, id]() {
                if (m_addonStore) {
                    m_addonStore->installAddon(id);
                }
            });
            actions->addWidget(install);
        }
        actions->addStretch(1);

        lay->addWidget(title);
        lay->addWidget(metaLab);
        lay->addWidget(desc);
        lay->addLayout(actions);
        m_addonsListLay->addWidget(row);
    };

    for (const AddonCatalogEntry& e : catalog) {
        addRow(e.id, e.name, e.version, e.description, e.author, e.hasCompatibleArtifact(), true);
    }
    for (auto it = installedById.cbegin(); it != installedById.cend(); ++it) {
        if (seen.contains(it.key())) {
            continue;
        }
        const AddonInstallRecord& r = it.value();
        addRow(r.id, r.name, r.version, r.description, r.author, true, false);
    }

    if (m_addonsListLay->count() == 0) {
        auto* empty = new QLabel(
            QStringLiteral("No addons to show. Refresh the catalog or install from a valid index.json."),
            m_addonsListHost);
        empty->setObjectName(QStringLiteral("dashHint"));
        empty->setWordWrap(true);
        m_addonsListLay->addWidget(empty);
    }
}

// ---- Sync (GitHub Gist, end-to-end encrypted) ----------------------------

void DashboardPage::persistSyncLive()
{
    if (!m_syncEnabledCheck) {
        return;
    }
    SyncConfig::setEnabled(m_syncEnabledCheck->isChecked());
    if (m_syncPollInterval) {
        SyncConfig::setPollIntervalSec(m_syncPollInterval->value());
    }
    if (m_sync) {
        m_sync->setPollIntervalSec(SyncConfig::pollIntervalSec());
    }
}

void DashboardPage::applyStoredSyncState()
{
    if (!m_syncEnabledCheck) {
        return;
    }
    // Load stored values into the widgets before any signal can persist them,
    // otherwise the default spinbox/checkbox values overwrite QSettings.
    if (m_syncPollInterval) {
        const QSignalBlocker block(m_syncPollInterval);
        m_syncPollInterval->setValue(SyncConfig::pollIntervalSec());
    }
    if (m_sync) {
        m_sync->setPollIntervalSec(SyncConfig::pollIntervalSec());
    }
    {
        const QSignalBlocker block(m_syncEnabledCheck);
        m_syncEnabledCheck->setChecked(SyncConfig::enabled());
    }

    const QString keyText = SyncConfig::syncKeyText();
    if (keyText.isEmpty() || !m_sync) {
        m_deferredSyncKey.clear();
        m_deferredSyncToken.clear();
        syncRefreshUiFromSyncState();
        return;
    }

    SyncKey key = SyncKeyCodec::decode(keyText);
    if (!key.isValid()) {
        m_deferredSyncKey.clear();
        m_deferredSyncToken.clear();
        syncRefreshUiFromSyncState();
        return;
    }
    const QString uuidHex = QString::fromLatin1(key.syncUuid.toHex());
    QString token;
    if (!key.token.isEmpty()) {
        token = QString::fromUtf8(key.token);
    } else {
        token = SyncConfig::loadToken(uuidHex);
    }
    if (!token.isEmpty() && m_syncTokenEdit) {
        m_syncTokenEdit->setText(token);
    }
    syncRefreshUiFromSyncState();
    // Network restore is deferred via startDeferredSyncRestore() so it does not
    // compete with vault load, theme, and first paint.
    if (SyncConfig::enabled() && !token.isEmpty()) {
        m_deferredSyncKey = keyText;
        m_deferredSyncToken = token;
    } else {
        m_deferredSyncKey.clear();
        m_deferredSyncToken.clear();
    }
}

void DashboardPage::startDeferredSyncRestore()
{
    if (!m_sync || m_deferredSyncKey.isEmpty() || m_deferredSyncToken.isEmpty()) {
        return;
    }
    if (!SyncConfig::enabled()) {
        m_deferredSyncKey.clear();
        m_deferredSyncToken.clear();
        return;
    }
    const QString keyText = m_deferredSyncKey;
    const QString token = m_deferredSyncToken;
    m_deferredSyncKey.clear();
    m_deferredSyncToken.clear();
    m_sync->restoreExisting(keyText, token);
}

void DashboardPage::syncRefreshUiFromSyncState()
{
    if (!m_syncEnabledCheck || !m_sync) {
        return;
    }
    const bool want = m_syncEnabledCheck->isChecked();
    QString keyText = m_sync->syncKeyString();
    if (keyText.isEmpty()) {
        keyText = SyncConfig::syncKeyText();
    }

    m_syncKeyDisplay->setText(keyText);
    const QString gistId = SyncKeyCodec::decode(keyText).gistId;
    m_syncGistIdLabel->setText(gistId.isEmpty() ? QString() : QStringLiteral("Gist: %1").arg(gistId));

    const bool active = (m_sync->state() == SyncController::State::Active);
    const bool connecting = (m_sync->state() == SyncController::State::Connecting);
    const bool paused = m_sync->isPaused();
    const bool configured = !keyText.isEmpty();

    m_syncEnabledHint->setVisible(true);
    m_syncSyncNowBtn->setEnabled(active && !connecting && !paused);
    m_syncDisableBtn->setEnabled(configured || want || active || connecting);
    m_syncCreateBtn->setEnabled(!connecting && !active);
    m_syncJoinBtn->setEnabled(!connecting && !active);
    m_syncCopyKeyBtn->setEnabled(!keyText.isEmpty());
    m_syncTestBtn->setEnabled(!connecting);
    m_syncPollInterval->setEnabled(want || active);
}

void DashboardPage::syncCreateSetup()
{
    const QString token = m_syncTokenEdit->text().trimmed();
    if (token.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Sync token required"),
                             QStringLiteral("Enter your GitHub token before creating a sync."));
        return;
    }
    if (!SyncConfig::syncKeyText().isEmpty() || m_sync->state() == SyncController::State::Active) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Replace existing sync?"),
            QStringLiteral("This will create a new gist and a new sync key. The current key will stop working on other devices."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    if (!m_syncEnabledCheck->isChecked()) {
        const QSignalBlocker block(m_syncEnabledCheck);
        m_syncEnabledCheck->setChecked(true);
        SyncConfig::setEnabled(true);
    }
    if (m_sync) {
        m_sync->setPaused(false);
    }
    m_syncStatus->setText(QStringLiteral("Creating an encrypted gist on GitHub…"));
    syncRefreshUiFromSyncState();
    m_sync->createSync(token, QStringLiteral("clientosh saved-sessions sync"));
}

void DashboardPage::syncJoinFromInput()
{
    const QString keyText = m_syncKeyEdit->text().trimmed();
    if (keyText.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Sync key required"),
                             QStringLiteral("Paste the sync key received from Computer 1."));
        return;
    }
    const QString token = m_syncTokenEdit->text().trimmed();
    if (token.isEmpty() && SyncKeyCodec::decode(keyText).token.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("GitHub token required"),
                             QStringLiteral("Enter the GitHub token for this machine (or use a sync key that already contains it)."));
        return;
    }
    if (!m_syncEnabledCheck->isChecked()) {
        const QSignalBlocker block(m_syncEnabledCheck);
        m_syncEnabledCheck->setChecked(true);
        SyncConfig::setEnabled(true);
    }
    if (m_sync) {
        m_sync->setPaused(false);
    }
    m_syncStatus->setText(QStringLiteral("Joining sync and pulling encrypted data…"));
    syncRefreshUiFromSyncState();
    m_sync->joinSync(keyText, token);
}

void DashboardPage::syncDisable()
{
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Disable synchronization?"),
        QStringLiteral("This forgets the sync key on this machine. Local sessions are kept. "
                       "Other devices can keep using the gist until you revoke the GitHub token."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    const QString keyText = SyncConfig::syncKeyText();
    if (!keyText.isEmpty()) {
        SyncKey key = SyncKeyCodec::decode(keyText);
        if (key.isValid()) {
            const QString uuidHex = QString::fromLatin1(key.syncUuid.toHex());
            SyncConfig::clearToken(uuidHex);
        }
    }
    SyncConfig::setSyncKeyText(QString());
    SyncConfig::setLastKnownRev(0);
    SyncConfig::setEnabled(false);
    if (m_syncEnabledCheck) {
        const QSignalBlocker block(m_syncEnabledCheck);
        m_syncEnabledCheck->setChecked(false);
    }
    if (m_sync) {
        m_sync->disable();
    }
    if (m_syncKeyDisplay) {
        m_syncKeyDisplay->clear();
    }
    if (m_syncKeyEdit) {
        m_syncKeyEdit->clear();
    }
    appendLog(QStringLiteral("sync: disabled"));
    syncRefreshUiFromSyncState();
}

void DashboardPage::syncTestToken()
{
    const QString token = m_syncTokenEdit->text().trimmed();
    if (token.isEmpty()) {
        m_syncTokenStatusLabel->setText(QStringLiteral("Enter a token to test."));
        return;
    }
    m_syncTokenStatusLabel->setText(QStringLiteral("Checking token…"));
    m_sync->testToken(token);
}

void DashboardPage::syncPushNow()
{
    if (!m_sync || m_sync->state() != SyncController::State::Active || m_sync->isPaused()) {
        return;
    }
    m_sync->pushNow();
}

void DashboardPage::syncPullNow()
{
    if (!m_sync || m_sync->state() != SyncController::State::Active || m_sync->isPaused()) {
        m_syncStatus->setText(QStringLiteral("Sync is not connected. Create or join a sync first."));
        return;
    }
    m_syncStatus->setText(QStringLiteral("Synchronizing with GitHub…"));
    m_sync->syncNow();
}

void DashboardPage::syncOnStateChanged(SyncController::State state)
{
    if (state == SyncController::State::Active) {
        if (!m_sync->syncKeyString().isEmpty()) {
            SyncConfig::setSyncKeyText(m_sync->syncKeyString());
            SyncKey key = SyncKeyCodec::decode(m_sync->syncKeyString());
            QString token = m_syncTokenEdit->text().trimmed();
            if (token.isEmpty() && !key.token.isEmpty()) {
                token = QString::fromUtf8(key.token);
                m_syncTokenEdit->setText(token);
            }
            if (key.isValid() && !token.isEmpty()) {
                const QString uuidHex = QString::fromLatin1(key.syncUuid.toHex());
                SyncConfig::storeToken(uuidHex, token);
            }
        }
        SyncConfig::setEnabled(true);
        if (m_syncEnabledCheck && !m_syncEnabledCheck->isChecked()) {
            const QSignalBlocker block(m_syncEnabledCheck);
            m_syncEnabledCheck->setChecked(true);
        }
    }
    syncRefreshUiFromSyncState();
}

void DashboardPage::syncOnStatus(const QString& message)
{
    const QString lower = message.toLower();
    if (lower.contains(QStringLiteral("token"))) {
        m_syncTokenStatusLabel->setText(message);
        if (lower.contains(QStringLiteral("valid")) || lower.contains(QStringLiteral("rejected"))) {
            syncRefreshUiFromSyncState();
            return;
        }
    }
    m_syncStatus->setText(message);
    syncRefreshUiFromSyncState();
}

void DashboardPage::syncOnError(const QString& message)
{
    m_syncStatus->setText(message);
    if (message.toLower().contains(QStringLiteral("token"))) {
        m_syncTokenStatusLabel->setText(message);
    }
    appendLog(QStringLiteral("sync error: %1").arg(message));
    syncRefreshUiFromSyncState();
}

void DashboardPage::syncOnDataUpdated()
{
    refresh();
    appendLog(QStringLiteral("sync: profiles updated from remote"));
}
