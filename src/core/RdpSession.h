#pragma once

#include "SessionProfile.h"

#include <QImage>
#include <QMutex>
#include <QSize>
#include <QThread>

/** Embedded RDP session (FreeRDP worker thread). */
class RdpSession : public QThread
{
    Q_OBJECT

public:
    explicit RdpSession(QObject* parent = nullptr);
    ~RdpSession() override;

    void startSession(const SessionProfile& profile, const QSize& desktopSize,
                      int deviceScalePercent = 100);
    void stopSession();
    bool isConnected() const;

    void sendMouseEvent(quint16 flags, int x, int y);
    void sendWheelEvent(quint16 flags);
    void sendUnicodeChar(QChar ch, bool down);
    void sendScanCode(quint32 scanCode, bool down);
    void sendFocusIn(quint16 toggleStates);
    void sendSecureAttentionSequence();

    // Called from FreeRDP paint/resize callbacks (worker thread).
    void handleEndPaint(void* ctx);
    void handleDesktopResize(void* ctx);

signals:
    void connected();
    void disconnected();
    void frameReady(const QImage& frame);
    void errorOccurred(const QString& message);
    void statusChanged(const QString& status);

protected:
    void run() override;

private:
    mutable QMutex m_mutex;
    SessionProfile m_profile;
    QSize m_desktopSize = QSize(1280, 720);
    int m_deviceScalePercent = 100;
    bool m_stopRequested = false;
    bool m_connected = false;
    void* m_instance = nullptr; // freerdp* — opaque here to keep header FreeRDP-free
};
