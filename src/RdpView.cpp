#include "RdpView.h"

#include "core/RdpFreeRdpIncludes.h"
#include "core/RdpKeyboardCapture.h"
#include "core/RdpSession.h"

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QWheelEvent>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace {

constexpr int kWheelDelta = 120;

quint16 wheelFlagsForDelta(int delta)
{
    if (delta == 0) {
        return 0;
    }

    int units = qAbs(delta) / kWheelDelta;
    if (units == 0) {
        units = 1;
    }
    units = qBound(1, units, 2);

    int rotation = units * kWheelDelta;
    rotation = qBound(kWheelDelta, rotation, 255);

    quint16 flags = PTR_FLAGS_WHEEL;
    if (delta > 0) {
        flags |= static_cast<quint16>(rotation & WheelRotationMask);
    } else {
        flags |= static_cast<quint16>((512 - rotation) & WheelRotationMask);
    }
    return flags;
}

quint16 horizontalWheelFlagsForDelta(int delta)
{
    if (delta == 0) {
        return 0;
    }

    int units = qAbs(delta) / kWheelDelta;
    if (units == 0) {
        units = 1;
    }
    units = qBound(1, units, 2);

    int rotation = units * kWheelDelta;
    rotation = qBound(kWheelDelta, rotation, 255);

    quint16 flags = PTR_FLAGS_HWHEEL;
    if (delta > 0) {
        flags |= static_cast<quint16>(rotation & WheelRotationMask);
    } else {
        flags |= static_cast<quint16>((512 - rotation) & WheelRotationMask);
    }
    return flags;
}

} // namespace

RdpView::RdpView(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("rdpView"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void RdpView::setSession(RdpSession* session)
{
    m_session = session;
}

void RdpView::setFrame(const QImage& frame)
{
    m_frame = frame;
    update();
}

void RdpView::clearFrame()
{
    m_frame = QImage();
    update();
}

QSize RdpView::requestedDesktopSize() const
{
    const qreal ratio = devicePixelRatioF();
    int pixelWidth = qRound(QWidget::width() * ratio);
    int pixelHeight = qRound(QWidget::height() * ratio);

    if (const QScreen* screen = window() ? window()->screen() : nullptr) {
        const QSize physical = screen->size() * screen->devicePixelRatio();
        pixelWidth = qMin(pixelWidth, physical.width());
        pixelHeight = qMin(pixelHeight, physical.height());
    }

    pixelWidth = qBound(640, pixelWidth, 3840) & ~1;
    pixelHeight = qBound(480, pixelHeight, 2160) & ~1;
    return QSize(pixelWidth, pixelHeight);
}

int RdpView::deviceScalePercent() const
{
    return qBound(100, qRound(devicePixelRatioF() * 100.0), 500);
}

void RdpView::refreshInputCapture()
{
    if (!hasFocus()) {
        return;
    }
    if (m_session && m_session->isConnected()) {
        m_session->sendFocusIn(keyboardToggleStates());
    }
    syncKeyboardCapture();
}

void RdpView::releaseInputCapture()
{
    releaseAllKeys();
    if (m_keyboardCaptured) {
        RdpKeyboardCapture::instance().end();
        m_keyboardCaptured = false;
    }
#if !defined(_WIN32)
    releaseKeyboard();
#endif
}

bool RdpView::event(QEvent* event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void RdpView::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x10, 0x10, 0x10));

    if (m_frame.isNull()) {
        painter.setPen(QColor(0x88, 0x88, 0x88));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("RDP desktop"));
        return;
    }

    const QRect target = displayRect();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, m_frame);
}

void RdpView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
}

void RdpView::keyPressEvent(QKeyEvent* event)
{
    if (!m_session || !m_session->isConnected()) {
        QWidget::keyPressEvent(event);
        return;
    }

    if (m_keyboardCaptured) {
        event->accept();
        return;
    }

    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_End && (event->modifiers() & Qt::ControlModifier)
        && (event->modifiers() & Qt::AltModifier)) {
        m_session->sendSecureAttentionSequence();
        event->accept();
        return;
    }

    if (isSystemShortcut(event->key(), event->modifiers())) {
        event->accept();
        return;
    }

    const quint32 nativeScan = static_cast<quint32>(event->nativeScanCode());
    if (nativeScan != 0) {
        sendNativeKeyEvent(nativeScan, event->nativeModifiers() & Qt::KeyboardModifierMask, true);
        event->accept();
        return;
    }

    const QString text = event->text();
    if (!text.isEmpty() && text.at(0).isPrint()) {
        for (const QChar ch : text) {
            m_session->sendUnicodeChar(ch, true);
            m_session->sendUnicodeChar(ch, false);
        }
        event->accept();
        return;
    }

    sendKeyEvent(event->key(), event->modifiers(), true);
    if (scanCodeForKey(event->key(), event->modifiers())) {
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void RdpView::keyReleaseEvent(QKeyEvent* event)
{
    if (!m_session || !m_session->isConnected()) {
        QWidget::keyReleaseEvent(event);
        return;
    }

    if (m_keyboardCaptured) {
        event->accept();
        return;
    }

    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }

    if (isSystemShortcut(event->key(), event->modifiers())) {
        event->accept();
        return;
    }

    const quint32 nativeScan = static_cast<quint32>(event->nativeScanCode());
    if (nativeScan != 0) {
        sendNativeKeyEvent(nativeScan, event->nativeModifiers() & Qt::KeyboardModifierMask, false);
        event->accept();
        return;
    }

    if (!event->text().isEmpty() && event->text().at(0).isPrint()) {
        event->accept();
        return;
    }
    sendKeyEvent(event->key(), event->modifiers(), false);
    if (scanCodeForKey(event->key(), event->modifiers())) {
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void RdpView::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_session || !m_session->isConnected()) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    sendMouse(PTR_FLAGS_MOVE, event->pos());
    event->accept();
}

void RdpView::mousePressEvent(QMouseEvent* event)
{
    if (!m_session || !m_session->isConnected()) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    quint16 flags = PTR_FLAGS_DOWN;
    if (event->button() == Qt::LeftButton) {
        flags |= PTR_FLAGS_BUTTON1;
    } else if (event->button() == Qt::RightButton) {
        flags |= PTR_FLAGS_BUTTON2;
    } else if (event->button() == Qt::MiddleButton) {
        flags |= PTR_FLAGS_BUTTON3;
    } else {
        QWidget::mousePressEvent(event);
        return;
    }
    sendMouse(flags, event->pos());
    event->accept();
}

void RdpView::mouseReleaseEvent(QMouseEvent* event)
{
    if (!m_session || !m_session->isConnected()) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    quint16 flags = 0;
    if (event->button() == Qt::LeftButton) {
        flags |= PTR_FLAGS_BUTTON1;
    } else if (event->button() == Qt::RightButton) {
        flags |= PTR_FLAGS_BUTTON2;
    } else if (event->button() == Qt::MiddleButton) {
        flags |= PTR_FLAGS_BUTTON3;
    } else {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    sendMouse(flags, event->pos());
    event->accept();
}

void RdpView::wheelEvent(QWheelEvent* event)
{
    if (!m_session || !m_session->isConnected()) {
        QWidget::wheelEvent(event);
        return;
    }

    QPoint delta = event->angleDelta();
    if (event->pixelDelta().manhattanLength() > 0) {
        delta = event->pixelDelta() * 8;
    }
    if (event->inverted()) {
        delta = -delta;
    }

    sendWheel(delta.x(), delta.y(), event->position().toPoint());
    event->accept();
}

void RdpView::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    if (m_session && m_session->isConnected()) {
        m_session->sendFocusIn(keyboardToggleStates());
    }
    syncKeyboardCapture();
}

void RdpView::focusOutEvent(QFocusEvent* event)
{
    releaseInputCapture();
    QWidget::focusOutEvent(event);
}

QRect RdpView::displayRect() const
{
    if (m_frame.isNull() || width() <= 0 || height() <= 0) {
        return {};
    }
    const QSize scaled = m_frame.size().scaled(size(), Qt::KeepAspectRatio);
    return QRect((width() - scaled.width()) / 2, (height() - scaled.height()) / 2, scaled.width(),
                 scaled.height());
}

QPoint RdpView::mapToRemote(const QPoint& local) const
{
    const QRect target = displayRect();
    if (m_frame.isNull() || target.isEmpty()) {
        return QPoint(-1, -1);
    }

    const int x = (local.x() - target.x()) * m_frame.width() / qMax(1, target.width());
    const int y = (local.y() - target.y()) * m_frame.height() / qMax(1, target.height());
    if (x < 0 || y < 0 || x >= m_frame.width() || y >= m_frame.height()) {
        return QPoint(-1, -1);
    }
    return QPoint(x, y);
}

QPoint RdpView::wheelPosition(const QPoint& local) const
{
    const QPoint remote = mapToRemote(local);
    if (remote.x() >= 0) {
        return remote;
    }
    if (m_lastMouseRemote.x() >= 0) {
        return m_lastMouseRemote;
    }
    if (!m_frame.isNull()) {
        return QPoint(m_frame.width() / 2, m_frame.height() / 2);
    }
    return QPoint(-1, -1);
}

void RdpView::sendMouse(quint16 flags, const QPoint& local)
{
    const QPoint remote = mapToRemote(local);
    if (remote.x() < 0) {
        return;
    }
    if ((flags & PTR_FLAGS_MOVE) && remote == m_lastMouseRemote && (flags & PTR_FLAGS_DOWN) == 0
        && (flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3)) == 0) {
        return;
    }
    m_lastMouseRemote = remote;
    m_session->sendMouseEvent(flags, remote.x(), remote.y());
}

void RdpView::sendWheel(int deltaX, int deltaY, const QPoint& local)
{
    const QPoint remote = wheelPosition(local);
    if (remote.x() < 0) {
        return;
    }

    if (deltaY != 0) {
        const quint16 flags = wheelFlagsForDelta(deltaY);
        if (flags) {
            m_session->sendWheelEvent(flags);
        }
    }
    if (deltaX != 0) {
        const quint16 flags = horizontalWheelFlagsForDelta(deltaX);
        if (flags) {
            m_session->sendWheelEvent(flags);
        }
    }

    m_lastMouseRemote = remote;
    m_session->sendMouseEvent(PTR_FLAGS_MOVE, remote.x(), remote.y());
}

void RdpView::sendKeyEvent(int key, Qt::KeyboardModifiers mods, bool down)
{
    const quint32 code = scanCodeForKey(key, mods);
    if (!code) {
        return;
    }
    if (down) {
        m_pressedScanCodes.insert(code);
    } else {
        m_pressedScanCodes.remove(code);
    }
    m_session->sendScanCode(code, down);
}

void RdpView::sendNativeKeyEvent(quint32 nativeScan, bool extended, bool down)
{
    const quint32 code = scanCodeForNativeScan(nativeScan, extended);
    if (!code) {
        return;
    }
    if (down) {
        m_pressedScanCodes.insert(code);
    } else {
        m_pressedScanCodes.remove(code);
    }
    m_session->sendScanCode(code, down);
}

void RdpView::syncKeyboardCapture()
{
    if (!m_session || !m_session->isConnected()) {
        if (m_keyboardCaptured) {
            RdpKeyboardCapture::instance().end();
            m_keyboardCaptured = false;
        }
        return;
    }

#if defined(_WIN32)
    if (!m_keyboardCaptured) {
        m_keyboardCaptured = RdpKeyboardCapture::instance().begin(m_session);
    }
#else
    grabKeyboard();
#endif
}

void RdpView::releaseAllKeys()
{
    if (!m_session) {
        m_pressedScanCodes.clear();
        return;
    }
    for (const quint32 code : m_pressedScanCodes) {
        m_session->sendScanCode(code, false);
    }
    m_pressedScanCodes.clear();
}

quint16 RdpView::keyboardToggleStates() const
{
    quint16 states = 0;
#if defined(_WIN32)
    if (GetKeyState(VK_CAPITAL) & 1) {
        states |= KBD_SYNC_CAPS_LOCK;
    }
    if (GetKeyState(VK_NUMLOCK) & 1) {
        states |= KBD_SYNC_NUM_LOCK;
    }
#endif
    return states;
}

bool RdpView::isSystemShortcut(int key, Qt::KeyboardModifiers mods) const
{
#if defined(_WIN32)
    if (m_keyboardCaptured) {
        return false;
    }
    if (key == Qt::Key_Tab && (mods & Qt::AltModifier)) {
        return true;
    }
    if (key == Qt::Key_Escape && (mods & Qt::AltModifier)) {
        return true;
    }
    if (mods & Qt::MetaModifier) {
        return true;
    }
#else
    Q_UNUSED(key)
    Q_UNUSED(mods)
#endif
    return false;
}

quint32 RdpView::scanCodeForNativeScan(quint32 nativeScan, bool extended) const
{
    const BYTE code = static_cast<BYTE>(nativeScan & 0xFF);
    return MAKE_RDP_SCANCODE(code, extended);
}

quint32 RdpView::scanCodeForKey(int key, Qt::KeyboardModifiers mods) const
{
    Q_UNUSED(mods)
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return RDP_SCANCODE_RETURN;
    case Qt::Key_Backspace:
        return RDP_SCANCODE_BACKSPACE;
    case Qt::Key_Tab:
        return RDP_SCANCODE_TAB;
    case Qt::Key_Escape:
        return RDP_SCANCODE_ESCAPE;
    case Qt::Key_Left:
        return RDP_SCANCODE_LEFT;
    case Qt::Key_Right:
        return RDP_SCANCODE_RIGHT;
    case Qt::Key_Up:
        return RDP_SCANCODE_UP;
    case Qt::Key_Down:
        return RDP_SCANCODE_DOWN;
    case Qt::Key_Home:
        return RDP_SCANCODE_HOME;
    case Qt::Key_End:
        return RDP_SCANCODE_END;
    case Qt::Key_PageUp:
        return RDP_SCANCODE_PRIOR;
    case Qt::Key_PageDown:
        return RDP_SCANCODE_NEXT;
    case Qt::Key_Delete:
        return RDP_SCANCODE_DELETE;
    case Qt::Key_Insert:
        return RDP_SCANCODE_INSERT;
    case Qt::Key_Shift:
        return RDP_SCANCODE_LSHIFT;
    case Qt::Key_Control:
        return RDP_SCANCODE_LCONTROL;
    case Qt::Key_Alt:
        return RDP_SCANCODE_LMENU;
    case Qt::Key_Meta:
        return RDP_SCANCODE_LWIN;
    case Qt::Key_CapsLock:
        return RDP_SCANCODE_CAPSLOCK;
    case Qt::Key_F1:
        return RDP_SCANCODE_F1;
    case Qt::Key_F2:
        return RDP_SCANCODE_F2;
    case Qt::Key_F3:
        return RDP_SCANCODE_F3;
    case Qt::Key_F4:
        return RDP_SCANCODE_F4;
    case Qt::Key_F5:
        return RDP_SCANCODE_F5;
    case Qt::Key_F6:
        return RDP_SCANCODE_F6;
    case Qt::Key_F7:
        return RDP_SCANCODE_F7;
    case Qt::Key_F8:
        return RDP_SCANCODE_F8;
    case Qt::Key_F9:
        return RDP_SCANCODE_F9;
    case Qt::Key_F10:
        return RDP_SCANCODE_F10;
    case Qt::Key_F11:
        return RDP_SCANCODE_F11;
    case Qt::Key_F12:
        return RDP_SCANCODE_F12;
    default:
        return 0;
    }
}
