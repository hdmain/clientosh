#pragma once

#include "core/SessionProfile.h"

#include <QWidget>

class RdpSession;
class RdpView;

/** Embedded RDP pane (FreeRDP) — connects using profile credentials from the dashboard. */
class RdpPane : public QWidget
{
    Q_OBJECT

public:
    explicit RdpPane(const SessionProfile& profile, QWidget* parent = nullptr);
    ~RdpPane() override;

    SessionProfile profile() const;
    void disconnectSession();

signals:
    void debugLog(const QString& line);
    void windowClosed(const QString& panelId);

private:
    void connectSession();

    SessionProfile m_profile;
    RdpSession* m_session = nullptr;
    RdpView* m_view = nullptr;
};
