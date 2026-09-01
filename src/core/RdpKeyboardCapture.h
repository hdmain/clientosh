#pragma once

class RdpSession;

/** Captures system-level keyboard shortcuts (Alt+Tab, Win, etc.) for RDP forwarding. */
class RdpKeyboardCapture
{
public:
    static RdpKeyboardCapture& instance();

    bool begin(RdpSession* session);
    void end();
    bool isActive() const;

#if defined(_WIN32)
    void handleHookKeyEvent(unsigned long long wParam, long long lParam);
#endif

private:
    RdpKeyboardCapture() = default;

    RdpSession* m_session = nullptr;
#if defined(_WIN32)
    void* m_hook = nullptr;
#endif
};
