#include "SyncGitHub.h"

#include "NetworkProxyManager.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QStringList>
#include <QUrl>

namespace {
const char* kGistApiBase = "https://api.github.com/gists";

QString githubErrorMessage(int status, const QByteArray& resp, const QString& networkError)
{
    QString githubMsg;
    const QJsonDocument doc = QJsonDocument::fromJson(resp);
    if (doc.isObject()) {
        githubMsg = doc.object().value(QStringLiteral("message")).toString().trimmed();
    }
    if (!githubMsg.isEmpty() && status > 0) {
        return QStringLiteral("%1 (HTTP %2)").arg(githubMsg).arg(status);
    }
    if (!githubMsg.isEmpty()) {
        return githubMsg;
    }
    if (status > 0 && !networkError.isEmpty()) {
        return QStringLiteral("%1 (HTTP %2)").arg(networkError).arg(status);
    }
    if (!networkError.isEmpty()) {
        return networkError;
    }
    if (status > 0) {
        return QStringLiteral("GitHub request failed (HTTP %1)").arg(status);
    }
    return QStringLiteral("GitHub request failed");
}

QByteArray bearerHeader(const QString& token)
{
    return QByteArray("Bearer ") + token.trimmed().toUtf8();
}

QByteArray fetchGistFileContent(const QJsonObject& file, const QString& token,
                                QString* errorOut)
{
    const bool truncated = file.value(QStringLiteral("truncated")).toBool(false);
    const QString rawUrl = file.value(QStringLiteral("raw_url")).toString();
    if (truncated && !rawUrl.isEmpty()) {
        QNetworkAccessManager nam;
        NetworkProxy::configureDirectAccess(&nam);
        QNetworkRequest req{QUrl(rawUrl)};
        req.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("clientosh-sync/1.0"));
        req.setRawHeader("Accept", "application/vnd.github.raw");
        req.setRawHeader("Authorization", bearerHeader(token));
        req.setTransferTimeout(30000);
        QNetworkReply* reply = nam.get(req);
        if (!reply->isFinished()) {
            QEventLoop loop;
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec();
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray resp = reply->readAll();
        const bool ok = (reply->error() == QNetworkReply::NoError
                         && status >= 200 && status < 300);
        if (!ok) {
            if (errorOut) {
                *errorOut = githubErrorMessage(status, resp, reply->errorString());
            }
            delete reply;
            return {};
        }
        delete reply;
        return resp;
    }
    return file.value(QStringLiteral("content")).toString().toUtf8();
}
} // namespace

bool SyncGitHub::perform(const QByteArray& method, const QString& url,
                         const QString& token, const QByteArray& payload,
                         int* httpStatusOut, QByteArray* responseBodyOut,
                         QByteArray& errorOut)
{
    QNetworkAccessManager nam;
    NetworkProxy::configureDirectAccess(&nam);
    QUrl qurl(url);
    QNetworkRequest req(qurl);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("clientosh-sync/1.0"));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    req.setRawHeader("Authorization", bearerHeader(token));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(30000);

    QNetworkReply* reply = nullptr;
    if (method == "POST") {
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = nam.post(req, payload);
    } else if (method == "PATCH") {
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = nam.sendCustomRequest(req, "PATCH", payload);
    } else { // GET
        reply = nam.get(req);
    }

    QString sslError;
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [&sslError](const QList<QSslError>& errors) {
                         QStringList parts;
                         for (const QSslError& e : errors) {
                             parts.append(e.errorString());
                         }
                         sslError = parts.join(QStringLiteral("; "));
                     });

    // If the reply already finished (cached / instant failure), skip the wait
    // so we cannot miss the finished signal and hang the worker thread.
    if (!reply->isFinished()) {
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray resp = reply->readAll();
    const bool ok = (reply->error() == QNetworkReply::NoError
                     && status >= 200 && status < 300);

    if (httpStatusOut) {
        *httpStatusOut = status;
    }
    if (responseBodyOut) {
        *responseBodyOut = resp;
    }

    if (!ok) {
        QString reason = sslError;
        if (reason.isEmpty()) {
            reason = githubErrorMessage(status, resp, reply->errorString());
        }
        errorOut = reason.toUtf8();
    }
    delete reply;
    return ok;
}

SyncGitHub::CreateResult SyncGitHub::createGist(const QString& token,
                                                const QString& description,
                                                const QString& filename,
                                                const QByteArray& body)
{
    CreateResult result;

    QJsonObject files;
    files.insert(filename, QJsonObject{{QStringLiteral("content"),
                                        QString::fromUtf8(body)}});
    QJsonObject reqObj;
    reqObj.insert(QStringLiteral("description"), description);
    reqObj.insert(QStringLiteral("public"), false);
    reqObj.insert(QStringLiteral("files"), files);
    const QByteArray payload = QJsonDocument(reqObj).toJson(QJsonDocument::Compact);

    int status = 0;
    QByteArray resp;
    QByteArray error;
    const bool ok = perform("POST", QLatin1String(kGistApiBase), token, payload,
                            &status, &resp, error);
    if (!ok) {
        result.error = QString::fromUtf8(error);
        return result;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(resp);
    if (doc.isObject()) {
        result.gistId = doc.object().value(QStringLiteral("id")).toString();
    }
    result.ok = !result.gistId.isEmpty();
    if (!result.ok) {
        result.error = QStringLiteral("GitHub did not return a gist id");
    }
    return result;
}

bool SyncGitHub::checkToken(const QString& token, QString* errorOut)
{
    // Probe gist access, not GET /user — fine-grained PATs with only the gist
    // permission are rejected by /user even when they can read/write gists.
    int status = 0;
    QByteArray resp;
    QByteArray error;
    const bool ok = perform("GET",
                            QLatin1String("https://api.github.com/gists?per_page=1"),
                            token, QByteArray(), &status, &resp, error);
    if (!ok && errorOut) {
        *errorOut = QString::fromUtf8(error);
    }
    return ok;
}

SyncGitHub::WriteResult SyncGitHub::updateGist(const QString& token,
                                               const QString& gistId,
                                               const QString& filename,
                                               const QByteArray& body)
{
    WriteResult result;

    QJsonObject files;
    files.insert(filename, QJsonObject{{QStringLiteral("content"),
                                        QString::fromUtf8(body)}});
    QJsonObject reqObj;
    reqObj.insert(QStringLiteral("files"), files);
    const QByteArray payload = QJsonDocument(reqObj).toJson(QJsonDocument::Compact);

    const QString url = QStringLiteral("%1/%2").arg(QLatin1String(kGistApiBase), gistId);
    int status = 0;
    QByteArray resp;
    QByteArray error;
    const bool ok = perform("PATCH", url, token, payload, &status, &resp, error);
    result.ok = ok;
    if (!ok) {
        result.error = QString::fromUtf8(error);
    }
    return result;
}

SyncGitHub::ReadResult SyncGitHub::readGist(const QString& token,
                                            const QString& gistId,
                                            const QString& filename)
{
    ReadResult result;

    const QString url = QStringLiteral("%1/%2").arg(QLatin1String(kGistApiBase), gistId);
    int status = 0;
    QByteArray resp;
    QByteArray error;
    const bool ok = perform("GET", url, token, QByteArray(), &status, &resp, error);
    if (!ok) {
        if (status == 404) {
            result.notFound = true;
            result.error = QStringLiteral("Gist not found (id %1)").arg(gistId);
        } else {
            result.error = QString::fromUtf8(error);
        }
        return result;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(resp);
    const QJsonObject files = doc.object().value(QStringLiteral("files")).toObject();

    auto takeFile = [&](const QJsonObject& file) -> bool {
        QString fetchError;
        const QByteArray body = fetchGistFileContent(file, token, &fetchError);
        if (body.isEmpty() && !fetchError.isEmpty()) {
            result.error = fetchError;
            return false;
        }
        result.body = QString::fromUtf8(body);
        result.ok = true;
        return true;
    };

    const QJsonObject named = files.value(filename).toObject();
    if (!named.isEmpty()) {
        takeFile(named);
        return result;
    }

    // Fallback: accept the first file (gist rename resilience).
    for (const QJsonValue& v : files) {
        const QJsonObject f = v.toObject();
        if (f.contains(QStringLiteral("content")) || f.contains(QStringLiteral("raw_url"))) {
            takeFile(f);
            return result;
        }
    }

    result.error = QStringLiteral("Sync file not found in gist");
    return result;
}
