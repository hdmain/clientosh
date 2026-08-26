#pragma once

#include <QtPlugin>
#include <QString>

class AddonHostContext;

/**
 * Qt plugin contract for clientosh addons.
 * Implementations live in separate MODULE libraries and are loaded by
 * AddonHost only while the addon is installed and enabled.
 */
class IClientoshAddon
{
public:
    virtual ~IClientoshAddon() = default;

    virtual QString id() const = 0;
    virtual QString displayName() const = 0;

    /** Build UI / wire services. Must be cheap until the user opens AI UI. */
    virtual void activate(AddonHostContext* context) = 0;
    /** Tear down UI and release memory. */
    virtual void deactivate() = 0;
};

#define ClientoshAddon_iid "com.clientosh.IClientoshAddon/1.0"
Q_DECLARE_INTERFACE(IClientoshAddon, ClientoshAddon_iid)
