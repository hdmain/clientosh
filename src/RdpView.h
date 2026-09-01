#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QWidget>

class RdpSession;

/** Displays an embedded RDP framebuffer and forwards local input. */
class RdpView : public QWidget
{
    Q_OBJECT

public:
    explicit RdpView(QWidget* parent = nullptr);

    void setSession(RdpSession* session);
    void setFrame(const QImage& frame);
    void clearFrame();
    QSize requestedDesktopSize() const;
    int deviceScalePercent() const;
    void refreshInputCapture();
    void releaseInputCapture();

signals:
    void statusMessage(const QString& text);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    QRect displayRect() const;
    QPoint mapToRemote(const QPoint& local) const;
    QPoint wheelPosition(const QPoint& local) const;
    void sendMouse(quint16 flags, const QPoint& local);
    void sendWheel(int deltaX, int deltaY, const QPoint& local);
    quint32 scanCodeForKey(int key, Qt::KeyboardModifiers mods) const;
    quint32 scanCodeForNativeScan(quint32 nativeScan, bool extended) const;
    void sendKeyEvent(int key, Qt::KeyboardModifiers mods, bool down);
    void sendNativeKeyEvent(quint32 nativeScan, bool extended, bool down);
    void syncKeyboardCapture();
    void releaseAllKeys();
    quint16 keyboardToggleStates() const;
    bool isSystemShortcut(int key, Qt::KeyboardModifiers mods) const;

    RdpSession* m_session = nullptr;
    QImage m_frame;
    QPoint m_lastMouseRemote{-1, -1};
    QSet<quint32> m_pressedScanCodes;
    bool m_keyboardCaptured = false;
};
