#include "RdpKeyboardCapture.h"

#include "RdpSession.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

#  include "RdpFreeRdpIncludes.h"

namespace {

LRESULT CALLBACK rdpKeyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && RdpKeyboardCapture::instance().isActive()) {
        RdpKeyboardCapture::instance().handleHookKeyEvent(wParam, lParam);
        return 1;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

} // namespace
#endif

RdpKeyboardCapture& RdpKeyboardCapture::instance()
{
    static RdpKeyboardCapture capture;
    return capture;
}

bool RdpKeyboardCapture::begin(RdpSession* session)
{
    if (!session) {
        return false;
    }
    m_session = session;

#if defined(_WIN32)
    if (m_hook) {
        return true;
    }
    m_hook = SetWindowsHookExW(WH_KEYBOARD_LL, rdpKeyboardHookProc, GetModuleHandleW(nullptr), 0);
    return m_hook != nullptr;
#else
    return true;
#endif
}

void RdpKeyboardCapture::end()
{
#if defined(_WIN32)
    if (m_hook) {
        UnhookWindowsHookEx(static_cast<HHOOK>(m_hook));
        m_hook = nullptr;
    }
#endif
    m_session = nullptr;
}

bool RdpKeyboardCapture::isActive() const
{
    return m_session != nullptr;
}

#if defined(_WIN32)

void RdpKeyboardCapture::handleHookKeyEvent(unsigned long long wParam, long long lParam)
{
    if (!m_session || !m_session->isConnected()) {
        return;
    }

    const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    if (!down && !up) {
        return;
    }

    if (down && kb->vkCode == VK_END && (GetAsyncKeyState(VK_CONTROL) & 0x8000)
        && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
        m_session->sendSecureAttentionSequence();
        return;
    }

    const bool extended = (kb->flags & LLKHF_EXTENDED) != 0;
    const quint32 scanCode = MAKE_RDP_SCANCODE(kb->scanCode & 0xFF, extended);
    m_session->sendScanCode(scanCode, down);
}

#endif
