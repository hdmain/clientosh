#pragma once

#include <QByteArray>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

class QNetworkAccessManager;

struct AiChatMessage {
    QString role; // system | user | assistant
    QString content;
};

/**
 * OpenAI-compatible chat client with SSE streaming when the gateway supports it.
 */
class AiAgentClient : public QObject
{
    Q_OBJECT

public:
    explicit AiAgentClient(QObject* parent = nullptr);

    void setApiBase(const QString& base);
    void setApiKey(const QString& key);
    void setModel(const QString& model);

    void chat(const QVector<AiChatMessage>& messages);
    void abort();

    bool busy() const { return m_reply != nullptr; }

    static int estimateTokens(const QVector<AiChatMessage>& messages);
    static int estimateTokens(const QString& text);

signals:
    /** Incremental visible/reasoning text while streaming. */
    void streamDelta(const QString& contentDelta, const QString& reasoningDelta);
    void finished(const QString& assistantText);
    void failed(const QString& error);
    void contextTokensChanged(int usedEstimate);

private:
    void onReadyRead();
    void onFinished();
    void processSseBuffer();
    void handleSseDataLine(const QByteArray& dataLine);
    bool tryParseNonStreamJson(const QByteArray& raw);

    QNetworkAccessManager* m_nam = nullptr;
    QPointer<QNetworkReply> m_reply;
    QString m_apiBase;
    QString m_apiKey;
    QString m_model;

    QByteArray m_sseCarry;   // incomplete line fragments
    QString m_assembled;     // full assistant content
    QString m_assembledReasoning;
    bool m_sawStreamEvent = false;
};
