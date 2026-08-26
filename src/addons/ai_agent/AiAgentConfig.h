#pragma once

#include <QSettings>
#include <QString>

namespace AiAgentConfig {

inline constexpr const char* kType = "addons/ai-agent/type";
inline constexpr const char* kApiBase = "addons/ai-agent/apiBase";
inline constexpr const char* kModel = "addons/ai-agent/model";
inline constexpr const char* kApiKey = "addons/ai-agent/apiKey";

inline QString providerType()
{
    return QSettings().value(QLatin1String(kType), QStringLiteral("openai")).toString();
}

inline void setProviderType(const QString& type)
{
    QSettings s;
    s.setValue(QLatin1String(kType), type);
    s.sync();
}

inline QString apiBase()
{
    return QSettings()
        .value(QLatin1String(kApiBase), QStringLiteral("https://api.openai.com/v1"))
        .toString()
        .trimmed();
}

inline void setApiBase(const QString& base)
{
    QSettings s;
    s.setValue(QLatin1String(kApiBase), base.trimmed());
    s.sync();
}

inline QString modelName()
{
    return QSettings()
        .value(QLatin1String(kModel), QStringLiteral("gpt-4o-mini"))
        .toString()
        .trimmed();
}

inline void setModelName(const QString& model)
{
    QSettings s;
    s.setValue(QLatin1String(kModel), model.trimmed());
    s.sync();
}

inline QString apiKey()
{
    return QSettings().value(QLatin1String(kApiKey), QString()).toString();
}

inline void setApiKey(const QString& key)
{
    QSettings s;
    s.setValue(QLatin1String(kApiKey), key);
    s.sync();
}

} // namespace AiAgentConfig
