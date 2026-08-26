#pragma once

#include <QString>
#include <functional>

class AiAgentBridge;

/**
 * Host services exposed to loaded plugins.
 * Owned by MainWindow; AddonHost passes this into IClientoshAddon::activate.
 */
class AddonHostContext
{
public:
    using SessionContextFn = std::function<void(QString* host, QString* user, int* port,
                                                QString* cwdHint)>;
    using InjectInputFn = std::function<bool(const QByteArray& data)>;
    using CaptureTerminalFn = std::function<QString(int maxLines)>;
    using BridgeChangedFn = std::function<void(AiAgentBridge* bridge)>;

    void setSessionContextProvider(SessionContextFn fn) { m_sessionContext = std::move(fn); }
    void setInjectInput(InjectInputFn fn) { m_injectInput = std::move(fn); }
    void setCaptureTerminal(CaptureTerminalFn fn) { m_captureTerminal = std::move(fn); }
    void setBridgeChangedHandler(BridgeChangedFn fn) { m_bridgeChanged = std::move(fn); }

    SessionContextFn sessionContextProvider() const { return m_sessionContext; }
    InjectInputFn injectInput() const { return m_injectInput; }
    CaptureTerminalFn captureTerminal() const { return m_captureTerminal; }

    /** Plugin registers / clears the AI agent bridge (null = unload). */
    void setAiAgentBridge(AiAgentBridge* bridge)
    {
        if (m_aiBridge == bridge) {
            return;
        }
        m_aiBridge = bridge;
        if (m_bridgeChanged) {
            m_bridgeChanged(bridge);
        }
    }

    AiAgentBridge* aiAgentBridge() const { return m_aiBridge; }

private:
    SessionContextFn m_sessionContext;
    InjectInputFn m_injectInput;
    BridgeChangedFn m_bridgeChanged;
    AiAgentBridge* m_aiBridge = nullptr;
    // Append-only for binary compatibility with older plugins where possible.
    CaptureTerminalFn m_captureTerminal;
};
