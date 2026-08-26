#include "AiAgentBridgeImpl.h"

#include "core/addons/AddonHostContext.h"
#include "core/addons/IClientoshAddon.h"

#include <QObject>

class AiAgentPlugin : public QObject, public IClientoshAddon
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ClientoshAddon_iid)
    Q_INTERFACES(IClientoshAddon)

public:
    QString id() const override { return QStringLiteral("ai-agent"); }
    QString displayName() const override { return QStringLiteral("AI agent"); }

    void activate(AddonHostContext* context) override
    {
        m_ctx = context;
        if (!m_bridge) {
            m_bridge = new AiAgentBridgeImpl(context);
        }
        if (context) {
            context->setAiAgentBridge(m_bridge);
        }
    }

    void deactivate() override
    {
        if (m_bridge) {
            m_bridge->destroyUi();
        }
        if (m_ctx) {
            m_ctx->setAiAgentBridge(nullptr);
        }
        delete m_bridge;
        m_bridge = nullptr;
        m_ctx = nullptr;
    }

private:
    AddonHostContext* m_ctx = nullptr;
    AiAgentBridgeImpl* m_bridge = nullptr;
};

#include "AiAgentPlugin.moc"
