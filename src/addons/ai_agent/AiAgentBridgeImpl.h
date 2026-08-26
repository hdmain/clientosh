#pragma once

#include "core/addons/AiAgentBridge.h"
#include "core/addons/AddonHostContext.h"

#include <QPointer>

class AiAgentPanel;
class AiAgentSettingsPage;

class AiAgentBridgeImpl : public AiAgentBridge
{
public:
    explicit AiAgentBridgeImpl(AddonHostContext* ctx);
    ~AiAgentBridgeImpl() override;

    QString displayName() const override;
    QIcon navIcon() const override;
    QWidget* createSettingsPage(QWidget* parent) override;
    QWidget* createPanel(QWidget* parent) override;
    void setSessionContext(const QString& host, const QString& user, int port,
                           const QString& cwdHint) override;
    void destroyUi() override;

private:
    AddonHostContext* m_ctx = nullptr;
    QPointer<AiAgentSettingsPage> m_settings;
    QPointer<AiAgentPanel> m_panel;
};
