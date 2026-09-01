#pragma once

#include <QByteArray>
#include <QString>

inline const char kClientoshSessionMime[] = "application/x-clientosh-session-id";
inline const char kClientoshPanelMime[] = "application/x-clientosh-panel";

enum class PanelKind {
    Terminal,
    Sftp,
    Rdp
};

enum class DockEdge {
    Left,
    Right,
    Top,
    Bottom,
    None
};

struct PanelRef {
    PanelKind kind = PanelKind::Terminal;
    QString sessionId;

    bool isValid() const { return !sessionId.isEmpty(); }

    QString key() const
    {
        switch (kind) {
        case PanelKind::Terminal:
            return QStringLiteral("term:") + sessionId;
        case PanelKind::Sftp:
            return QStringLiteral("sftp:") + sessionId;
        case PanelKind::Rdp:
            return QStringLiteral("rdp:") + sessionId;
        }
        return {};
    }

    QString titleHint() const
    {
        switch (kind) {
        case PanelKind::Sftp:
            return QStringLiteral("sftp");
        case PanelKind::Rdp:
            return QStringLiteral("rdp");
        case PanelKind::Terminal:
        default:
            return QStringLiteral("term");
        }
    }

    static PanelRef terminal(const QString& sessionId)
    {
        return {PanelKind::Terminal, sessionId};
    }

    static PanelRef sftp(const QString& sessionId)
    {
        return {PanelKind::Sftp, sessionId};
    }

    static PanelRef rdp(const QString& sessionId)
    {
        return {PanelKind::Rdp, sessionId};
    }

    static PanelRef fromKey(const QString& key)
    {
        if (key.startsWith(QStringLiteral("term:"))) {
            return terminal(key.mid(5));
        }
        if (key.startsWith(QStringLiteral("sftp:"))) {
            return sftp(key.mid(5));
        }
        if (key.startsWith(QStringLiteral("rdp:"))) {
            return rdp(key.mid(4));
        }
        return {};
    }

    static PanelRef fromMime(const QByteArray& bytes)
    {
        return fromKey(QString::fromUtf8(bytes));
    }

    QByteArray toMime() const { return key().toUtf8(); }

    bool operator==(const PanelRef& o) const
    {
        return kind == o.kind && sessionId == o.sessionId;
    }

    bool operator!=(const PanelRef& o) const { return !(*this == o); }
};
