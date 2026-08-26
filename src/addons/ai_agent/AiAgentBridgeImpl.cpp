#include "AiAgentBridgeImpl.h"
#include "AiAgentPanel.h"
#include "AiAgentSettingsPage.h"

#include <QIcon>

AiAgentBridgeImpl::AiAgentBridgeImpl(AddonHostContext* ctx)
    : m_ctx(ctx)
{
}

AiAgentBridgeImpl::~AiAgentBridgeImpl()
{
    destroyUi();
}

QString AiAgentBridgeImpl::displayName() const
{
    return QStringLiteral("AI agent");
}

QIcon AiAgentBridgeImpl::navIcon() const
{
    return QIcon(QStringLiteral(":/icons/ai-agent.svg"));
}

QWidget* AiAgentBridgeImpl::createSettingsPage(QWidget* parent)
{
    if (!m_settings) {
        m_settings = new AiAgentSettingsPage(parent);
    } else if (m_settings->parent() != parent) {
        m_settings->setParent(parent);
    }
    return m_settings;
}

QWidget* AiAgentBridgeImpl::createPanel(QWidget* parent)
{
    if (!m_panel) {
        m_panel = new AiAgentPanel(m_ctx, parent);
    } else if (m_panel->parent() != parent) {
        m_panel->setParent(parent);
    }
    return m_panel;
}

void AiAgentBridgeImpl::setSessionContext(const QString& host, const QString& user, int port,
                                          const QString& cwdHint)
{
    if (m_panel) {
        m_panel->setSessionContext(host, user, port, cwdHint);
    }
}

void AiAgentBridgeImpl::destroyUi()
{
    if (m_settings) {
        m_settings->deleteLater();
        m_settings.clear();
    }
    if (m_panel) {
        m_panel->deleteLater();
        m_panel.clear();
    }
}
