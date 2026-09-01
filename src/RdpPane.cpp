#include "RdpPane.h"

#include "RdpView.h"
#include "core/RdpSession.h"

#include <QTimer>
#include <QVBoxLayout>

RdpPane::RdpPane(const SessionProfile& profile, QWidget* parent)
    : QWidget(parent)
    , m_profile(profile)
{
    setObjectName(QStringLiteral("rdpPane"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_session = new RdpSession(this);
    m_view = new RdpView(this);
    m_view->setSession(m_session);
    lay->addWidget(m_view, 1);

    connect(m_session, &RdpSession::frameReady, m_view, &RdpView::setFrame, Qt::QueuedConnection);
    connect(m_session, &RdpSession::disconnected, m_view, &RdpView::clearFrame);
    connect(m_session, &RdpSession::disconnected, m_view, &RdpView::releaseInputCapture);
    connect(m_session, &RdpSession::connected, m_view, &RdpView::refreshInputCapture);
    connect(m_session, &RdpSession::errorOccurred, this, [this](const QString& err) {
        emit debugLog(QStringLiteral("rdp · %1 — %2").arg(m_profile.displayTitle(), err));
    });

    if (m_profile.port <= 0) {
        m_profile.port = 3389;
    }

    QTimer::singleShot(0, this, [this]() { connectSession(); });
}

RdpPane::~RdpPane()
{
    disconnectSession();
}

SessionProfile RdpPane::profile() const
{
    return m_profile;
}

void RdpPane::connectSession()
{
    if (m_profile.host.trimmed().isEmpty() || m_profile.user.trimmed().isEmpty()) {
        emit debugLog(QStringLiteral("rdp · %1 — missing host or user in profile")
                          .arg(m_profile.displayTitle()));
        return;
    }

    m_view->clearFrame();
    m_session->startSession(m_profile, m_view->requestedDesktopSize(),
                            m_view->deviceScalePercent());
}

void RdpPane::disconnectSession()
{
    if (m_session) {
        m_session->stopSession();
    }
}
