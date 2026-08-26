#pragma once

#include <QIcon>
#include <QString>

class QWidget;

/**
 * Abstract AI agent surface implemented inside the ai-agent plugin DLL.
 * The main app never links the plugin — it only holds this pointer while loaded.
 */
class AiAgentBridge
{
public:
    virtual ~AiAgentBridge() = default;

    virtual QString displayName() const = 0;
    virtual QIcon navIcon() const = 0;

    /** Settings category page (parented by the settings stack). */
    virtual QWidget* createSettingsPage(QWidget* parent) = 0;
    /** Side panel shown when the robot toggle is on. */
    virtual QWidget* createPanel(QWidget* parent) = 0;

    virtual void setSessionContext(const QString& host, const QString& user, int port,
                                   const QString& cwdHint) = 0;

    /** Release widgets owned by the bridge (called before unload). */
    virtual void destroyUi() = 0;
};
