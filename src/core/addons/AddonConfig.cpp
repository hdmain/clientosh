#include "AddonConfig.h"

#include <QSettings>
#include <QStringList>

namespace {

const char* kRepoUrl = "addons/repositoryUrl";
const char* kEnabledPrefix = "addons/enabled/";

} // namespace

namespace AddonConfig {

QString defaultRepositoryUrl()
{
    // Official catalog shipped in this repository (addons/index.json).
    return QStringLiteral(
        "https://raw.githubusercontent.com/hdmain/clientosh/main/addons/index.json");
}

QString repositoryUrl()
{
    const QString stored = QSettings().value(QLatin1String(kRepoUrl)).toString().trimmed();
    return stored.isEmpty() ? defaultRepositoryUrl() : stored;
}

void setRepositoryUrl(const QString& url)
{
    QSettings s;
    const QString trimmed = url.trimmed();
    if (trimmed.isEmpty() || trimmed == defaultRepositoryUrl()) {
        s.remove(QLatin1String(kRepoUrl));
    } else {
        s.setValue(QLatin1String(kRepoUrl), trimmed);
    }
    s.sync();
}

bool isEnabled(const QString& addonId)
{
    if (addonId.trimmed().isEmpty()) {
        return false;
    }
    // Default to enabled once installed.
    return QSettings()
        .value(QLatin1String(kEnabledPrefix) + addonId, true)
        .toBool();
}

void setEnabled(const QString& addonId, bool on)
{
    if (addonId.trimmed().isEmpty()) {
        return;
    }
    QSettings s;
    s.setValue(QLatin1String(kEnabledPrefix) + addonId, on);
    s.sync();
}

QStringList enabledAddonIds()
{
    QSettings s;
    s.beginGroup(QStringLiteral("addons/enabled"));
    const QStringList keys = s.childKeys();
    QStringList out;
    for (const QString& k : keys) {
        if (s.value(k, true).toBool()) {
            out.append(k);
        }
    }
    s.endGroup();
    return out;
}

} // namespace AddonConfig
