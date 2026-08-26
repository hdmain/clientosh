#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

/** Lightweight semver helpers for About → GitHub release checks. */
namespace UpdateCheck {

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool valid = false;
};

/** Parse "v1.0.7", "1.0.8-dev", "1.0.8-beta.3" → numeric core version. */
inline SemVer parseSemVer(QString text)
{
    text = text.trimmed();
    if (text.startsWith(QLatin1Char('v')) || text.startsWith(QLatin1Char('V'))) {
        text = text.mid(1);
    }
    const int cut = text.indexOf(QRegularExpression(QStringLiteral("[+-]")));
    if (cut >= 0) {
        text = text.left(cut);
    }
    const QStringList parts = text.split(QLatin1Char('.'));
    if (parts.size() < 2) {
        return {};
    }
    SemVer v;
    bool okMajor = false;
    bool okMinor = false;
    bool okPatch = true;
    v.major = parts.at(0).toInt(&okMajor);
    v.minor = parts.at(1).toInt(&okMinor);
    if (parts.size() >= 3) {
        // Ignore extra build metadata after a 4th component (e.g. 1.0.8.1).
        v.patch = parts.at(2).toInt(&okPatch);
    }
    v.valid = okMajor && okMinor && okPatch && v.major >= 0 && v.minor >= 0 && v.patch >= 0;
    return v;
}

inline int compare(const SemVer& a, const SemVer& b)
{
    if (a.major != b.major) {
        return a.major < b.major ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor ? -1 : 1;
    }
    if (a.patch != b.patch) {
        return a.patch < b.patch ? -1 : 1;
    }
    return 0;
}

inline QString format(const SemVer& v)
{
    return QStringLiteral("%1.%2.%3").arg(v.major).arg(v.minor).arg(v.patch);
}

/**
 * True when the published release should be offered as an update.
 * Same numeric version on a non-stable channel (dev/beta) also qualifies,
 * so users can jump to the matching official release.
 */
inline bool shouldOfferUpdate(const SemVer& currentProduct, const SemVer& latestRelease,
                              const QString& buildChannel)
{
    if (!currentProduct.valid || !latestRelease.valid) {
        return false;
    }
    const int cmp = compare(latestRelease, currentProduct);
    if (cmp > 0) {
        return true;
    }
    if (cmp == 0 && buildChannel != QLatin1String("stable")) {
        return true;
    }
    return false;
}

} // namespace UpdateCheck
