#include "AiAgentClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

AiAgentClient::AiAgentClient(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

void AiAgentClient::setApiBase(const QString& base)
{
    m_apiBase = base.trimmed();
    while (m_apiBase.endsWith(QLatin1Char('/'))) {
        m_apiBase.chop(1);
    }
}

void AiAgentClient::setApiKey(const QString& key)
{
    m_apiKey = key.trimmed();
}

void AiAgentClient::setModel(const QString& model)
{
    m_model = model.trimmed();
}

void AiAgentClient::abort()
{
    if (!m_reply) {
        return;
    }
    QNetworkReply* reply = m_reply;
    m_reply.clear();
    m_sseCarry.clear();
    m_assembled.clear();
    m_assembledReasoning.clear();
    m_sawStreamEvent = false;
    reply->abort();
    reply->deleteLater();
}

int AiAgentClient::estimateTokens(const QString& text)
{
    return qMax(1, (text.size() + 3) / 4);
}

int AiAgentClient::estimateTokens(const QVector<AiChatMessage>& messages)
{
    int total = 0;
    for (const AiChatMessage& m : messages) {
        total += estimateTokens(m.role) + estimateTokens(m.content) + 4;
    }
    return total;
}

void AiAgentClient::chat(const QVector<AiChatMessage>& messages)
{
    abort();
    m_sseCarry.clear();
    m_assembled.clear();
    m_assembledReasoning.clear();
    m_sawStreamEvent = false;

    emit contextTokensChanged(estimateTokens(messages));

    if (m_apiBase.isEmpty() || m_model.isEmpty()) {
        emit failed(QStringLiteral("Set API base and model name in Settings → AI agent."));
        return;
    }

    QUrl url(m_apiBase + QStringLiteral("/chat/completions"));
    if (!url.isValid()) {
        emit failed(QStringLiteral("Invalid API base URL."));
        return;
    }

    QJsonArray msgs;
    for (const AiChatMessage& m : messages) {
        QJsonObject o;
        o.insert(QStringLiteral("role"), m.role);
        o.insert(QStringLiteral("content"), m.content);
        msgs.append(o);
    }

    QJsonObject body;
    body.insert(QStringLiteral("model"), m_model);
    body.insert(QStringLiteral("messages"), msgs);
    body.insert(QStringLiteral("temperature"), 0.2);
    body.insert(QStringLiteral("stream"), true);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("clientosh-ai-agent/1.2"));
    req.setRawHeader("Accept", "text/event-stream, application/json");
    if (!m_apiKey.isEmpty()) {
        req.setRawHeader("Authorization",
                         QByteArray("Bearer ") + m_apiKey.toUtf8());
    }
    req.setTransferTimeout(180000);

    QNetworkReply* reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, &AiAgentClient::onReadyRead);
    connect(reply, &QNetworkReply::finished, this, &AiAgentClient::onFinished);
}

void AiAgentClient::onReadyRead()
{
    if (!m_reply) {
        return;
    }
    m_sseCarry.append(m_reply->readAll());
    processSseBuffer();
}

void AiAgentClient::processSseBuffer()
{
    while (true) {
        const int nl = m_sseCarry.indexOf('\n');
        if (nl < 0) {
            break;
        }
        QByteArray line = m_sseCarry.left(nl);
        m_sseCarry.remove(0, nl + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith("data:")) {
            QByteArray payload = line.mid(5);
            if (payload.startsWith(' ')) {
                payload = payload.mid(1);
            }
            handleSseDataLine(payload);
        }
        // ignore event:/id:/comment lines
    }
}

void AiAgentClient::handleSseDataLine(const QByteArray& dataLine)
{
    if (dataLine == "[DONE]") {
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(dataLine);
    if (!doc.isObject()) {
        return;
    }
    m_sawStreamEvent = true;

    const QJsonObject root = doc.object();
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return;
    }
    const QJsonObject choice0 = choices.at(0).toObject();
    QJsonObject delta = choice0.value(QStringLiteral("delta")).toObject();
    if (delta.isEmpty()) {
        // Some gateways put the chunk under "message"
        delta = choice0.value(QStringLiteral("message")).toObject();
    }

    QString contentDelta = delta.value(QStringLiteral("content")).toString();
    QString reasoningDelta = delta.value(QStringLiteral("reasoning_content")).toString();
    if (reasoningDelta.isEmpty()) {
        reasoningDelta = delta.value(QStringLiteral("reasoning")).toString();
    }

    if (!contentDelta.isEmpty()) {
        m_assembled += contentDelta;
    }
    if (!reasoningDelta.isEmpty()) {
        m_assembledReasoning += reasoningDelta;
    }
    if (!contentDelta.isEmpty() || !reasoningDelta.isEmpty()) {
        emit streamDelta(contentDelta, reasoningDelta);
    }
}

bool AiAgentClient::tryParseNonStreamJson(const QByteArray& raw)
{
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        return false;
    }
    const QJsonArray choices = doc.object().value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return false;
    }
    const QJsonObject message =
        choices.at(0).toObject().value(QStringLiteral("message")).toObject();
    QString content = message.value(QStringLiteral("content")).toString();
    QString reasoning = message.value(QStringLiteral("reasoning_content")).toString();
    if (reasoning.isEmpty()) {
        reasoning = message.value(QStringLiteral("reasoning")).toString();
    }
    if (content.trimmed().isEmpty() && reasoning.trimmed().isEmpty()) {
        return false;
    }

    // Normalize: wrap native reasoning into <think> so the panel parser stays unified.
    if (!reasoning.trimmed().isEmpty()) {
        m_assembled = QStringLiteral("<think>\n%1\n</think>\n%2").arg(reasoning, content);
        emit streamDelta(m_assembled, QString());
    } else {
        m_assembled = content;
        emit streamDelta(content, QString());
    }
    return true;
}

void AiAgentClient::onFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        return;
    }
    reply->deleteLater();
    if (m_reply != reply) {
        return;
    }
    m_reply.clear();

    // Flush any trailing SSE without a final newline.
    if (!m_sseCarry.isEmpty()) {
        processSseBuffer();
        if (!m_sseCarry.isEmpty() && m_sseCarry.startsWith("data:")) {
            QByteArray payload = m_sseCarry.mid(5);
            if (payload.startsWith(' ')) {
                payload = payload.mid(1);
            }
            handleSseDataLine(payload.trimmed());
            m_sseCarry.clear();
        }
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() == QNetworkReply::OperationCanceledError) {
        return;
    }

    // Leftover bytes that weren't consumed as SSE (non-stream JSON body).
    QByteArray leftover = m_sseCarry;
    m_sseCarry.clear();
    // Also anything still unread (usually empty after readyRead).
    leftover += reply->readAll();

    if (reply->error() != QNetworkReply::NoError && !m_sawStreamEvent && m_assembled.isEmpty()) {
        QString err = reply->errorString();
        if (status > 0) {
            err += QStringLiteral(" (HTTP %1)").arg(status);
        }
        if (!leftover.isEmpty()) {
            err += QStringLiteral("\n") + QString::fromUtf8(leftover.left(400));
        }
        emit failed(err);
        return;
    }

    if (!m_sawStreamEvent && m_assembled.isEmpty() && !leftover.isEmpty()) {
        if (!tryParseNonStreamJson(leftover)) {
            emit failed(QStringLiteral("Could not parse model response."));
            return;
        }
    }

    QString full = m_assembled;
    if (!m_assembledReasoning.trimmed().isEmpty()) {
        // Prefer embedding native reasoning for history/display consistency.
        full = QStringLiteral("<think>\n%1\n</think>\n%2").arg(m_assembledReasoning, m_assembled);
    }

    if (full.trimmed().isEmpty()) {
        emit failed(QStringLiteral("Model returned no content."));
        return;
    }

    emit finished(full);
}
