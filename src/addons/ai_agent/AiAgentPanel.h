#pragma once

#include "AiAgentClient.h"
#include "core/addons/AddonHostContext.h"

#include <QStringList>
#include <QVector>
#include <QWidget>

class QLabel;
class QTextBrowser;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QTimer;

struct AiPendingAction {
    QString command;
    QString reason;
};

class AiAgentPanel : public QWidget
{
    Q_OBJECT

public:
    explicit AiAgentPanel(AddonHostContext* ctx, QWidget* parent = nullptr);

    void setSessionContext(const QString& host, const QString& user, int port,
                           const QString& cwdHint);

private:
    enum class Phase {
        Idle,
        Thinking,
        AwaitingConfirm,
        Running,
        Observing,
    };

    QString buildSystemPrompt() const;
    void refreshContextMeter();
    void refreshTranscript();
    void appendTranscriptBlock(const QString& markdownBlock);
    void setOrUpdateLiveBlock(int* index, const QString& markdownBlock);
    void clearLiveStreamBlocks();
    void setPhase(Phase phase, const QString& detail = QString());
    void submitPrompt();
    void requestModelTurn();
    void beginStreamUi();
    void onStreamDelta(const QString& contentDelta, const QString& reasoningDelta);
    void flushStreamUi();
    void handleAssistant(const QString& text);
    void stopAgent(const QString& reason);
    void showNextPendingAction();
    void runCurrentAction();
    void skipCurrentAction(bool notifyModel);
    void runAllRemaining();
    void beginObserveAfterRun(const QString& command);
    void finishObserve(const QString& command);
    void maybeUpdateCwdFromObservation(const QString& command, const QString& observation);
    void trimHistoryIfNeeded();
    /** True for read-only discovery commands (cat/ls/pwd/…) — run without confirm. */
    static bool isAutoApprovedCommand(const QString& command);

    static QString extractThink(const QString& text, QString* remainder);
    static void splitStreamingThink(const QString& raw, const QString& nativeReasoning,
                                    QString* thinkOut, QString* bodyOut, bool* thinkOpen);
    static QStringList extractCommands(const QString& assistantText);
    static bool marksDone(const QString& text);
    static QString escapeMarkdownFence(const QString& text);
    static QString formatThinkMarkdown(const QString& think, bool stillOpen);

    AddonHostContext* m_ctx = nullptr;
    AiAgentClient* m_client = nullptr;
    QTimer* m_observeTimer = nullptr;
    QTimer* m_streamUiTimer = nullptr;

    QLabel* m_sessionLabel = nullptr;
    QLabel* m_phaseLabel = nullptr;
    QLabel* m_contextLabel = nullptr;
    QProgressBar* m_contextBar = nullptr;
    QTextBrowser* m_chat = nullptr;
    QLineEdit* m_prompt = nullptr;
    QPushButton* m_sendBtn = nullptr;
    QPushButton* m_stopBtn = nullptr;
    QWidget* m_confirmBox = nullptr;
    QLabel* m_confirmLabel = nullptr;
    QPushButton* m_confirmRun = nullptr;
    QPushButton* m_confirmRunAll = nullptr;
    QPushButton* m_confirmSkip = nullptr;

    QString m_host;
    QString m_user;
    int m_port = 22;
    QString m_cwd;
    QVector<AiChatMessage> m_history;
    QStringList m_transcriptBlocks;
    QVector<AiPendingAction> m_actionQueue;
    int m_actionIndex = 0;
    bool m_autoRunRemaining = false;
    Phase m_phase = Phase::Idle;
    int m_agentSteps = 0;
    int m_contextTokens = 0;
    QString m_preRunSnapshot;
    QString m_runningCommand;

    bool m_streamActive = false;
    bool m_streamDisplayed = false; // live UI already showed think/body
    QString m_streamContent;
    QString m_streamReasoning;
    int m_liveThinkIdx = -1;
    int m_liveBodyIdx = -1;

    static constexpr int kContextBudget = 128000;
    static constexpr int kMaxAgentSteps = 16;
    static constexpr int kObserveMs = 2000;
    static constexpr int kCaptureLines = 60;
    static constexpr int kMaxHistoryChars = 90000;
};
