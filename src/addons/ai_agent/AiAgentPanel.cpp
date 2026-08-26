#include "AiAgentPanel.h"
#include "AiAgentConfig.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QString defaultSystemPrompt()
{
    return QStringLiteral(
        R"(You are the Clientosh AI agent — an autonomous SSH/terminal coding agent embedded in an SSH client (similar in spirit to Cursor Agent / Claude Code).

## Mission
Solve the user's goal on the live remote session by planning, running shell commands (with user confirmation), reading real terminal output, and iterating until done.

## Always-available facts
Treat Live session context below as ground truth: host/IP, user, port, working directory when known.
If cwd is unknown, your first tool step should usually be `pwd`.

## Response format (strict)
Every reply MUST follow this structure:

1) Optional reasoning in a think block (always preferred on non-trivial turns):
<think>
Short plan: what you know, what you will check next, risks.
</think>

2) Visible Markdown for the user (headings, lists, bold, code). Be concise.

3) Zero or more executable tool steps as fenced shell blocks. Each block is ONE shell command (or a short intentional pipeline). Example:
```shell
ls -la
```
Put multiple commands as SEPARATE fences when you need a multi-step plan. The host queues them;
safe read-only ones run automatically, others ask the user to confirm. After each run you receive
an Observation with real terminal text.

4) When the goal is complete (or blocked and you need the user), end with a line containing only:
DONE

## Tool / observation loop
- Never invent command output. If you need data, emit a shell block and wait for Observation.
- After an Observation arrives, continue: think → maybe more commands → or DONE.
- Prefer small reversible steps. Call out destructive risk in Markdown before those commands.
- Do not chain unrelated commands with `&&` unless the user asked for a single compound action.
- Prefer read-only discovery first (`pwd`, `ls`, `uname -a`, `ip -br a`, `systemctl status`, `cat`, `rg`/`grep`).
- The host auto-runs safe read-only commands (cat, ls, pwd, grep, head, …) without asking;
  write/destructive commands still require user confirmation.
- Redact secrets. Never echo passwords/tokens into chat.

## Style
- Reply in the user's language when clear; otherwise English.
- Use Markdown formatting liberally for readability.
- Keep think blocks short (bullets). Keep user-facing Markdown focused.
)");
}

} // namespace

AiAgentPanel::AiAgentPanel(AddonHostContext* ctx, QWidget* parent)
    : QWidget(parent)
    , m_ctx(ctx)
    , m_client(new AiAgentClient(this))
    , m_observeTimer(new QTimer(this))
    , m_streamUiTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("aiAgentPanel"));
    setMinimumWidth(340);
    setMaximumWidth(520);

    m_observeTimer->setSingleShot(true);
    connect(m_observeTimer, &QTimer::timeout, this, [this]() {
        finishObserve(m_runningCommand);
    });

    m_streamUiTimer->setSingleShot(true);
    m_streamUiTimer->setInterval(60);
    connect(m_streamUiTimer, &QTimer::timeout, this, &AiAgentPanel::flushStreamUi);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("AI agent"), this);
    title->setObjectName(QStringLiteral("dashPageTitle"));

    m_sessionLabel = new QLabel(QStringLiteral("No active session"), this);
    m_sessionLabel->setObjectName(QStringLiteral("dashHint"));
    m_sessionLabel->setWordWrap(true);

    m_phaseLabel = new QLabel(QStringLiteral("Idle"), this);
    m_phaseLabel->setObjectName(QStringLiteral("dashHint"));

    m_contextLabel = new QLabel(QStringLiteral("Context: —"), this);
    m_contextLabel->setObjectName(QStringLiteral("dashHint"));
    m_contextBar = new QProgressBar(this);
    m_contextBar->setRange(0, 100);
    m_contextBar->setValue(0);
    m_contextBar->setTextVisible(false);
    m_contextBar->setFixedHeight(6);

    m_chat = new QTextBrowser(this);
    m_chat->setOpenExternalLinks(true);
    m_chat->setReadOnly(true);
    m_chat->setObjectName(QStringLiteral("aiAgentChat"));
    m_chat->setPlaceholderText(QStringLiteral("Ask the agent to inspect or change the remote…"));
    m_chat->document()->setDefaultStyleSheet(
        QStringLiteral("body{font-size:12px;} "
                       "code,pre{font-family:Consolas,monospace;font-size:11px;} "
                       "h3{margin-top:12px;margin-bottom:4px;} "
                       "blockquote{color:#888;border-left:3px solid #555;margin:4px 0;padding-left:8px;}"));

    m_confirmBox = new QWidget(this);
    auto* confirmLay = new QVBoxLayout(m_confirmBox);
    confirmLay->setContentsMargins(0, 0, 0, 0);
    confirmLay->setSpacing(6);
    m_confirmLabel = new QLabel(m_confirmBox);
    m_confirmLabel->setObjectName(QStringLiteral("dashHint"));
    m_confirmLabel->setWordWrap(true);
    auto* confirmRow = new QHBoxLayout;
    m_confirmRun = new QPushButton(QStringLiteral("Run"), m_confirmBox);
    m_confirmRunAll = new QPushButton(QStringLiteral("Run all"), m_confirmBox);
    m_confirmSkip = new QPushButton(QStringLiteral("Skip"), m_confirmBox);
    m_confirmRun->setObjectName(QStringLiteral("dashPrimary"));
    m_confirmRunAll->setObjectName(QStringLiteral("dashButton"));
    m_confirmSkip->setObjectName(QStringLiteral("dashButton"));
    confirmRow->addWidget(m_confirmRun);
    confirmRow->addWidget(m_confirmRunAll);
    confirmRow->addWidget(m_confirmSkip);
    confirmLay->addWidget(m_confirmLabel);
    confirmLay->addLayout(confirmRow);
    m_confirmBox->hide();

    auto* promptRow = new QHBoxLayout;
    m_prompt = new QLineEdit(this);
    m_prompt->setPlaceholderText(QStringLiteral("Goal or follow-up…"));
    m_sendBtn = new QPushButton(QStringLiteral("Send"), this);
    m_stopBtn = new QPushButton(QStringLiteral("Stop"), this);
    m_sendBtn->setObjectName(QStringLiteral("dashPrimary"));
    m_stopBtn->setObjectName(QStringLiteral("dashButton"));
    m_stopBtn->setEnabled(false);
    promptRow->addWidget(m_prompt, 1);
    promptRow->addWidget(m_sendBtn);
    promptRow->addWidget(m_stopBtn);

    lay->addWidget(title);
    lay->addWidget(m_sessionLabel);
    lay->addWidget(m_phaseLabel);
    lay->addWidget(m_contextLabel);
    lay->addWidget(m_contextBar);
    lay->addWidget(m_chat, 1);
    lay->addWidget(m_confirmBox);
    lay->addLayout(promptRow);

    connect(m_sendBtn, &QPushButton::clicked, this, &AiAgentPanel::submitPrompt);
    connect(m_prompt, &QLineEdit::returnPressed, this, &AiAgentPanel::submitPrompt);
    connect(m_stopBtn, &QPushButton::clicked, this, [this]() {
        stopAgent(QStringLiteral("Stopped by user."));
    });
    connect(m_client, &AiAgentClient::streamDelta, this, &AiAgentPanel::onStreamDelta);
    connect(m_client, &AiAgentClient::finished, this, &AiAgentPanel::handleAssistant);
    connect(m_client, &AiAgentClient::failed, this, [this](const QString& err) {
        m_streamUiTimer->stop();
        m_streamActive = false;
        m_streamDisplayed = false;
        clearLiveStreamBlocks();
        appendTranscriptBlock(QStringLiteral("### Error\n\n```\n%1\n```\n").arg(err));
        setPhase(Phase::Idle);
        m_sendBtn->setEnabled(true);
        m_stopBtn->setEnabled(false);
        m_autoRunRemaining = false;
    });
    connect(m_client, &AiAgentClient::contextTokensChanged, this, [this](int used) {
        m_contextTokens = used;
        refreshContextMeter();
    });

    connect(m_confirmRun, &QPushButton::clicked, this, [this]() {
        m_autoRunRemaining = false;
        runCurrentAction();
    });
    connect(m_confirmRunAll, &QPushButton::clicked, this, &AiAgentPanel::runAllRemaining);
    connect(m_confirmSkip, &QPushButton::clicked, this, [this]() { skipCurrentAction(true); });

    m_history.push_back({QStringLiteral("system"), buildSystemPrompt()});
    appendTranscriptBlock(
        QStringLiteral("_Agent ready. Describe a goal — it will think, queue shell steps, "
                       "and continue after each confirmed command._\n"));
    refreshContextMeter();
    setPhase(Phase::Idle);
}

void AiAgentPanel::setSessionContext(const QString& host, const QString& user, int port,
                                     const QString& cwdHint)
{
    m_host = host;
    m_user = user;
    m_port = port;
    if (!cwdHint.isEmpty()) {
        m_cwd = cwdHint;
    }

    QString line;
    if (host.isEmpty()) {
        line = QStringLiteral("No active session — open an SSH session for terminal actions.");
    } else {
        line = QStringLiteral("%1@%2:%3")
                   .arg(user.isEmpty() ? QStringLiteral("?") : user, host)
                   .arg(port > 0 ? port : 22);
        if (!m_cwd.isEmpty()) {
            line += QStringLiteral("\ncwd: %1").arg(m_cwd);
        } else {
            line += QStringLiteral("\ncwd: unknown (agent may run `pwd`)");
        }
    }
    m_sessionLabel->setText(line);

    if (!m_history.isEmpty() && m_history.first().role == QLatin1String("system")) {
        m_history[0].content = buildSystemPrompt();
    } else {
        m_history.prepend({QStringLiteral("system"), buildSystemPrompt()});
    }
    refreshContextMeter();
}

QString AiAgentPanel::buildSystemPrompt() const
{
    QString prompt = defaultSystemPrompt();
    prompt += QStringLiteral("\n## Live session context\n");
    prompt += QStringLiteral("- Host / IP: %1\n")
                  .arg(m_host.isEmpty() ? QStringLiteral("(none)") : m_host);
    prompt += QStringLiteral("- User: %1\n")
                  .arg(m_user.isEmpty() ? QStringLiteral("(none)") : m_user);
    prompt += QStringLiteral("- Port: %1\n").arg(m_port > 0 ? m_port : 22);
    prompt += QStringLiteral("- Working directory: %1\n")
                  .arg(m_cwd.isEmpty() ? QStringLiteral("(unknown)") : m_cwd);
    prompt += QStringLiteral(
        "- Agent step budget this run: remaining turns are enforced by the host "
        "(%1 max tool rounds).\n")
                  .arg(kMaxAgentSteps);
    return prompt;
}

void AiAgentPanel::setPhase(Phase phase, const QString& detail)
{
    m_phase = phase;
    QString label;
    switch (phase) {
    case Phase::Idle:
        label = QStringLiteral("Idle");
        break;
    case Phase::Thinking:
        label = QStringLiteral("Thinking…");
        break;
    case Phase::AwaitingConfirm:
        label = QStringLiteral("Awaiting confirmation");
        break;
    case Phase::Running:
        label = QStringLiteral("Running in terminal…");
        break;
    case Phase::Observing:
        label = QStringLiteral("Observing terminal…");
        break;
    }
    if (!detail.isEmpty()) {
        label += QStringLiteral(" — ") + detail;
    }
    m_phaseLabel->setText(label);
    const bool busy = phase != Phase::Idle;
    m_stopBtn->setEnabled(busy);
    m_sendBtn->setEnabled(phase == Phase::Idle || phase == Phase::AwaitingConfirm);
}

void AiAgentPanel::refreshContextMeter()
{
    const int used = m_contextTokens > 0 ? m_contextTokens
                                         : AiAgentClient::estimateTokens(m_history);
    m_contextTokens = used;
    const int pct = qBound(0, int((qreal(used) / qreal(kContextBudget)) * 100.0), 100);
    m_contextBar->setValue(pct);
    m_contextLabel->setText(QStringLiteral("Context: ~%1 / %2 tokens (%3%) · step %4/%5")
                                .arg(used)
                                .arg(kContextBudget)
                                .arg(pct)
                                .arg(m_agentSteps)
                                .arg(kMaxAgentSteps));
}

void AiAgentPanel::refreshTranscript()
{
    const QString md = m_transcriptBlocks.join(QStringLiteral("\n"));
    const int scroll = m_chat->verticalScrollBar()->value();
    const bool atBottom =
        scroll >= m_chat->verticalScrollBar()->maximum() - 24;
    m_chat->setMarkdown(md);
    if (atBottom) {
        m_chat->verticalScrollBar()->setValue(m_chat->verticalScrollBar()->maximum());
    }
}

void AiAgentPanel::appendTranscriptBlock(const QString& markdownBlock)
{
    m_transcriptBlocks.push_back(markdownBlock);
    // Cap UI transcript length.
    while (m_transcriptBlocks.size() > 80) {
        m_transcriptBlocks.removeFirst();
        if (m_liveThinkIdx > 0) {
            --m_liveThinkIdx;
        } else if (m_liveThinkIdx == 0) {
            m_liveThinkIdx = -1;
        }
        if (m_liveBodyIdx > 0) {
            --m_liveBodyIdx;
        } else if (m_liveBodyIdx == 0) {
            m_liveBodyIdx = -1;
        }
    }
    refreshTranscript();
}

void AiAgentPanel::setOrUpdateLiveBlock(int* index, const QString& markdownBlock)
{
    if (!index) {
        return;
    }
    if (*index >= 0 && *index < m_transcriptBlocks.size()) {
        m_transcriptBlocks[*index] = markdownBlock;
    } else {
        m_transcriptBlocks.push_back(markdownBlock);
        *index = m_transcriptBlocks.size() - 1;
    }
}

void AiAgentPanel::clearLiveStreamBlocks()
{
    // Leave finalized content in place; only reset indices.
    m_liveThinkIdx = -1;
    m_liveBodyIdx = -1;
    m_streamContent.clear();
    m_streamReasoning.clear();
}

QString AiAgentPanel::formatThinkMarkdown(const QString& think, bool stillOpen)
{
    QString t = think.trimmed();
    if (t.isEmpty() && stillOpen) {
        t = QStringLiteral("…");
    }
    t.replace(QLatin1Char('\n'), QStringLiteral("\n> "));
    QString md = QStringLiteral("### Thinking\n\n> %1").arg(t);
    if (stillOpen) {
        md += QStringLiteral(" ▍");
    }
    md += QLatin1Char('\n');
    return md;
}

void AiAgentPanel::splitStreamingThink(const QString& raw, const QString& nativeReasoning,
                                       QString* thinkOut, QString* bodyOut, bool* thinkOpen)
{
    QString think;
    QString body = raw;
    bool open = false;

    if (!nativeReasoning.isEmpty()) {
        think = nativeReasoning;
    }

    // Incremental <think>…</think> in content (may be incomplete while streaming).
    static const QRegularExpression openTag(QStringLiteral("<think>"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression closeTag(QStringLiteral("</think>"),
                                             QRegularExpression::CaseInsensitiveOption);

    int searchFrom = 0;
    while (true) {
        const QRegularExpressionMatch om = openTag.match(body, searchFrom);
        if (!om.hasMatch()) {
            break;
        }
        const int start = om.capturedStart();
        const int afterOpen = om.capturedEnd();
        const QRegularExpressionMatch cm = closeTag.match(body, afterOpen);
        if (cm.hasMatch()) {
            const QString chunk = body.mid(afterOpen, cm.capturedStart() - afterOpen).trimmed();
            if (!think.isEmpty() && !chunk.isEmpty()) {
                think += QLatin1Char('\n');
            }
            think += chunk;
            body.remove(start, cm.capturedEnd() - start);
            searchFrom = start;
            open = false;
        } else {
            // Still streaming inside think.
            const QString chunk = body.mid(afterOpen);
            if (!think.isEmpty() && !chunk.isEmpty()) {
                think += QLatin1Char('\n');
            }
            think += chunk;
            body = body.left(start);
            open = true;
            break;
        }
    }

    if (thinkOut) {
        *thinkOut = think;
    }
    if (bodyOut) {
        *bodyOut = body.trimmed();
    }
    if (thinkOpen) {
        // Native reasoning arrives first with empty content ⇒ still thinking.
        *thinkOpen = open || (!nativeReasoning.isEmpty() && raw.trimmed().isEmpty());
    }
}

void AiAgentPanel::beginStreamUi()
{
    m_streamActive = true;
    m_streamDisplayed = false;
    m_streamContent.clear();
    m_streamReasoning.clear();
    m_liveThinkIdx = -1;
    m_liveBodyIdx = -1;
    setPhase(Phase::Thinking, QStringLiteral("streaming"));
    // Seed an empty thinking block so the user sees activity immediately.
    setOrUpdateLiveBlock(&m_liveThinkIdx, formatThinkMarkdown(QString(), true));
    refreshTranscript();
}

void AiAgentPanel::onStreamDelta(const QString& contentDelta, const QString& reasoningDelta)
{
    if (!m_streamActive) {
        beginStreamUi();
    }
    if (!contentDelta.isEmpty()) {
        m_streamContent += contentDelta;
    }
    if (!reasoningDelta.isEmpty()) {
        m_streamReasoning += reasoningDelta;
    }
    m_streamDisplayed = true;
    if (!m_streamUiTimer->isActive()) {
        m_streamUiTimer->start();
    }
}

void AiAgentPanel::flushStreamUi()
{
    if (!m_streamActive && m_streamContent.isEmpty() && m_streamReasoning.isEmpty()) {
        return;
    }

    QString think;
    QString body;
    bool thinkOpen = false;
    splitStreamingThink(m_streamContent, m_streamReasoning, &think, &body, &thinkOpen);

    if (!think.isEmpty() || thinkOpen) {
        setOrUpdateLiveBlock(&m_liveThinkIdx, formatThinkMarkdown(think, thinkOpen));
        setPhase(Phase::Thinking, thinkOpen ? QStringLiteral("streaming")
                                            : QStringLiteral("writing reply"));
    }

    static const QRegularExpression doneLine(
        QStringLiteral("(?:^|\\n)\\s*DONE\\s*$"),
        QRegularExpression::MultilineOption);
    QString bodyShow = body;
    bodyShow.remove(doneLine);
    if (!bodyShow.trimmed().isEmpty()) {
        QString md = QStringLiteral("### Agent\n\n%1").arg(bodyShow.trimmed());
        if (m_streamActive) {
            md += QStringLiteral(" ▍");
        }
        md += QLatin1Char('\n');
        setOrUpdateLiveBlock(&m_liveBodyIdx, md);
    }

    refreshTranscript();
}

QString AiAgentPanel::escapeMarkdownFence(const QString& text)
{
    QString t = text;
    t.replace(QStringLiteral("```"), QStringLiteral("``\\`"));
    return t;
}

void AiAgentPanel::submitPrompt()
{
    const QString text = m_prompt->text().trimmed();
    if (text.isEmpty()) {
        return;
    }
    if (m_phase == Phase::Thinking || m_phase == Phase::Running || m_phase == Phase::Observing) {
        return;
    }

    m_prompt->clear();
    m_actionQueue.clear();
    m_actionIndex = 0;
    m_autoRunRemaining = false;
    m_agentSteps = 0;
    m_confirmBox->hide();

    appendTranscriptBlock(QStringLiteral("### You\n\n%1\n").arg(text));
    m_history.push_back({QStringLiteral("user"), text});

    if (m_ctx && m_ctx->sessionContextProvider()) {
        QString host;
        QString user;
        int port = 22;
        QString cwd;
        m_ctx->sessionContextProvider()(&host, &user, &port, &cwd);
        setSessionContext(host, user, port, cwd.isEmpty() ? m_cwd : cwd);
    }

    requestModelTurn();
}

void AiAgentPanel::requestModelTurn()
{
    if (m_agentSteps >= kMaxAgentSteps) {
        appendTranscriptBlock(
            QStringLiteral("### Agent\n\nReached step limit (%1). Send another prompt to continue.\n")
                .arg(kMaxAgentSteps));
        setPhase(Phase::Idle);
        return;
    }

    trimHistoryIfNeeded();
    m_client->setApiBase(AiAgentConfig::apiBase());
    m_client->setApiKey(AiAgentConfig::apiKey());
    m_client->setModel(AiAgentConfig::modelName());
    beginStreamUi();
    m_sendBtn->setEnabled(false);
    ++m_agentSteps;
    refreshContextMeter();
    m_client->chat(m_history);
}

void AiAgentPanel::handleAssistant(const QString& text)
{
    m_streamUiTimer->stop();
    m_streamActive = false;
    // Final paint without cursor.
    m_streamContent.clear();
    m_streamReasoning.clear();
    // Re-parse full text into live blocks (or create if stream never painted).
    {
        QString visible;
        const QString think = extractThink(text, &visible);
        if (!think.trimmed().isEmpty()) {
            setOrUpdateLiveBlock(&m_liveThinkIdx, formatThinkMarkdown(think, false));
        }
        QString body = visible.trimmed();
        static const QRegularExpression doneLine(
            QStringLiteral("(?:^|\\n)\\s*DONE\\s*$"),
            QRegularExpression::MultilineOption);
        body.remove(doneLine);
        if (!body.trimmed().isEmpty()) {
            setOrUpdateLiveBlock(&m_liveBodyIdx,
                                 QStringLiteral("### Agent\n\n%1\n").arg(body.trimmed()));
        }
        refreshTranscript();
    }
    m_liveThinkIdx = -1;
    m_liveBodyIdx = -1;
    m_streamDisplayed = false;

    m_history.push_back({QStringLiteral("assistant"), text});
    refreshContextMeter();

    const QStringList cmds = extractCommands(text);
    m_actionQueue.clear();
    m_actionIndex = 0;
    for (const QString& c : cmds) {
        AiPendingAction a;
        a.command = c;
        a.reason = QStringLiteral("Queued by agent");
        m_actionQueue.push_back(a);
    }

    if (m_actionQueue.isEmpty()) {
        setPhase(Phase::Idle);
        m_sendBtn->setEnabled(true);
        return;
    }

    if (m_autoRunRemaining) {
        runCurrentAction();
    } else {
        showNextPendingAction();
    }
}

void AiAgentPanel::showNextPendingAction()
{
    if (m_actionIndex >= m_actionQueue.size()) {
        m_confirmBox->hide();
        // Ask model to continue with observations already in history if we finished a batch mid-loop;
        // if we just displayed fresh actions and somehow empty, idle.
        setPhase(Phase::Idle);
        m_sendBtn->setEnabled(true);
        return;
    }

    const AiPendingAction& a = m_actionQueue.at(m_actionIndex);
    if (isAutoApprovedCommand(a.command)) {
        m_confirmBox->hide();
        appendTranscriptBlock(
            QStringLiteral("_Auto-ran read-only:_ `%1`\n").arg(a.command));
        runCurrentAction();
        return;
    }

    const int left = m_actionQueue.size() - m_actionIndex;
    m_confirmLabel->setText(
        QStringLiteral("Step %1/%2 — confirm shell command:\n\n$ %3")
            .arg(m_actionIndex + 1)
            .arg(m_actionQueue.size())
            .arg(a.command));
    m_confirmBox->show();
    m_confirmRunAll->setEnabled(left > 1);
    setPhase(Phase::AwaitingConfirm, QStringLiteral("%1 left").arg(left));
    m_sendBtn->setEnabled(true);
}

void AiAgentPanel::runAllRemaining()
{
    m_autoRunRemaining = true;
    runCurrentAction();
}

void AiAgentPanel::runCurrentAction()
{
    if (m_actionIndex < 0 || m_actionIndex >= m_actionQueue.size() || !m_ctx) {
        m_confirmBox->hide();
        setPhase(Phase::Idle);
        return;
    }

    const QString cmd = m_actionQueue.at(m_actionIndex).command;
    m_confirmBox->hide();
    setPhase(Phase::Running, cmd.left(48));

    if (m_ctx->captureTerminal()) {
        m_preRunSnapshot = m_ctx->captureTerminal()(kCaptureLines);
    } else {
        m_preRunSnapshot.clear();
    }

    const QByteArray payload = cmd.toUtf8() + '\n';
    if (!m_ctx->injectInput() || !m_ctx->injectInput()(payload)) {
        appendTranscriptBlock(
            QStringLiteral("### Error\n\nCould not inject into the active terminal.\n"));
        m_autoRunRemaining = false;
        setPhase(Phase::Idle);
        m_sendBtn->setEnabled(true);
        return;
    }

    appendTranscriptBlock(QStringLiteral("### Ran\n\n```shell\n%1\n```\n").arg(cmd));
    beginObserveAfterRun(cmd);
}

void AiAgentPanel::beginObserveAfterRun(const QString& command)
{
    m_runningCommand = command;
    setPhase(Phase::Observing, QStringLiteral("waiting for output"));
    m_observeTimer->start(kObserveMs);
}

void AiAgentPanel::finishObserve(const QString& command)
{
    QString observation;
    if (m_ctx && m_ctx->captureTerminal()) {
        observation = m_ctx->captureTerminal()(kCaptureLines);
    }
    // Prefer a simple tail; if identical to pre-run, still send it with a note.
    if (observation == m_preRunSnapshot && !observation.isEmpty()) {
        observation = QStringLiteral("(terminal text unchanged after wait)\n") + observation;
    }
    if (observation.isEmpty()) {
        observation = QStringLiteral("(no terminal capture available)");
    }

    maybeUpdateCwdFromObservation(command, observation);

    const QString truncated = observation.size() > 8000 ? observation.right(8000) : observation;
    appendTranscriptBlock(QStringLiteral("### Observation\n\n```text\n%1\n```\n")
                              .arg(escapeMarkdownFence(truncated)));

    m_history.push_back(
        {QStringLiteral("user"),
         QStringLiteral(
             "Observation after confirmed command.\n"
             "Command:\n```shell\n%1\n```\n"
             "Terminal capture (newest lines):\n```text\n%2\n```\n"
             "Continue the agent loop: think, queue more shell steps if needed, or finish with DONE.")
             .arg(command, truncated)});

    ++m_actionIndex;

    if (m_actionIndex < m_actionQueue.size()) {
        // More queued commands from the same model turn — confirm/run next without another model call.
        if (m_autoRunRemaining) {
            runCurrentAction();
        } else {
            showNextPendingAction();
        }
        return;
    }

    // Finished all queued actions from this turn — ask the model again.
    m_actionQueue.clear();
    m_actionIndex = 0;
    m_autoRunRemaining = false;
    requestModelTurn();
}

void AiAgentPanel::skipCurrentAction(bool notifyModel)
{
    if (m_actionIndex < 0 || m_actionIndex >= m_actionQueue.size()) {
        m_confirmBox->hide();
        setPhase(Phase::Idle);
        return;
    }

    const QString cmd = m_actionQueue.at(m_actionIndex).command;
    appendTranscriptBlock(QStringLiteral("### Skipped\n\n`%1`\n").arg(cmd));
    ++m_actionIndex;
    m_autoRunRemaining = false;

    if (m_actionIndex < m_actionQueue.size()) {
        showNextPendingAction();
        return;
    }

    m_confirmBox->hide();
    m_actionQueue.clear();
    m_actionIndex = 0;

    if (notifyModel) {
        m_history.push_back(
            {QStringLiteral("user"),
             QStringLiteral(
                 "The user skipped the remaining queued command(s), including:\n```shell\n%1\n```\n"
                 "Continue without that step, or ask what to do next. Use DONE if finished.")
                 .arg(cmd)});
        requestModelTurn();
    } else {
        setPhase(Phase::Idle);
        m_sendBtn->setEnabled(true);
    }
}

void AiAgentPanel::stopAgent(const QString& reason)
{
    m_observeTimer->stop();
    m_streamUiTimer->stop();
    m_streamActive = false;
    m_streamDisplayed = false;
    m_liveThinkIdx = -1;
    m_liveBodyIdx = -1;
    m_streamContent.clear();
    m_streamReasoning.clear();
    m_client->abort();
    m_actionQueue.clear();
    m_actionIndex = 0;
    m_autoRunRemaining = false;
    m_confirmBox->hide();
    m_runningCommand.clear();
    appendTranscriptBlock(QStringLiteral("### Stopped\n\n%1\n").arg(reason));
    setPhase(Phase::Idle);
    m_sendBtn->setEnabled(true);
}

void AiAgentPanel::maybeUpdateCwdFromObservation(const QString& command, const QString& observation)
{
    const QString trimmedCmd = command.trimmed();
    if (trimmedCmd != QLatin1String("pwd") && !trimmedCmd.startsWith(QLatin1String("pwd "))) {
        return;
    }
    const QStringList lines = observation.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString line = lines.at(i).trimmed();
        if (line.startsWith(QLatin1Char('/')) || line.startsWith(QLatin1Char('~'))) {
            m_cwd = line;
            setSessionContext(m_host, m_user, m_port, m_cwd);
            return;
        }
    }
}

void AiAgentPanel::trimHistoryIfNeeded()
{
    int chars = 0;
    for (const AiChatMessage& m : m_history) {
        chars += m.content.size();
    }
    // Keep system + recent turns.
    while (chars > kMaxHistoryChars && m_history.size() > 4) {
        // Drop oldest non-system message.
        int dropIdx = 1;
        if (dropIdx >= m_history.size()) {
            break;
        }
        chars -= m_history.at(dropIdx).content.size();
        m_history.removeAt(dropIdx);
    }
}

bool AiAgentPanel::isAutoApprovedCommand(const QString& command)
{
    const QString cmd = command.trimmed();
    if (cmd.isEmpty() || cmd.size() > 500) {
        return false;
    }

    // Anything that can mutate state or escalate — always confirm.
    static const QRegularExpression dangerous(
        QStringLiteral(
            R"([>]{1,2}|<<?|\btee\b|\bsudo\b|\bsu\b|\bdoas\b|\brm\b|\bmv\b|\bcp\b|\bchmod\b|\bchown\b|\bchgrp\b|\bkill\b|\bkillall\b|\bpkill\b|\breboot\b|\bshutdown\b|\bhalt\b|\bpoweroff\b|\bdd\b|\bmkfs\b|\bmount\b|\bumount\b|\buseradd\b|\buserdel\b|\bpasswd\b|\bwget\b|\bcurl\b.+\s-o\b|\b>\s*/|\bnano\b|\bvi\b|\bvim\b|\bless\b|\bmore\b|\btop\b|\bhtop\b|\bwatch\b)"),
        QRegularExpression::CaseInsensitiveOption);
    if (dangerous.match(cmd).hasMatch()) {
        return false;
    }

    // Split pipelines; every stage must be an allowlisted reader.
    const QStringList stages = cmd.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    if (stages.isEmpty()) {
        return false;
    }
    // No ; && || compounds for auto (too easy to sneak a write).
    if (cmd.contains(QLatin1String("&&")) || cmd.contains(QLatin1String("||"))
        || cmd.contains(QLatin1Char(';'))) {
        return false;
    }

    auto baseName = [](QString token) -> QString {
        token = token.trimmed();
        if (token.startsWith(QLatin1Char('/'))) {
            token = token.section(QLatin1Char('/'), -1);
        }
        return token.toLower();
    };

    static const QSet<QString> allow = {
        QStringLiteral("cat"),       QStringLiteral("head"),      QStringLiteral("tail"),
        QStringLiteral("ls"),        QStringLiteral("ll"),        QStringLiteral("dir"),
        QStringLiteral("tree"),      QStringLiteral("pwd"),       QStringLiteral("whoami"),
        QStringLiteral("id"),        QStringLiteral("hostname"),  QStringLiteral("uname"),
        QStringLiteral("date"),      QStringLiteral("uptime"),    QStringLiteral("df"),
        QStringLiteral("du"),        QStringLiteral("free"),      QStringLiteral("env"),
        QStringLiteral("printenv"),  QStringLiteral("which"),     QStringLiteral("type"),
        QStringLiteral("file"),      QStringLiteral("stat"),      QStringLiteral("wc"),
        QStringLiteral("md5sum"),    QStringLiteral("sha1sum"),   QStringLiteral("sha256sum"),
        QStringLiteral("grep"),      QStringLiteral("egrep"),     QStringLiteral("fgrep"),
        QStringLiteral("rg"),        QStringLiteral("ag"),        QStringLiteral("ack"),
        QStringLiteral("find"),      QStringLiteral("locate"),    QStringLiteral("realpath"),
        QStringLiteral("readlink"),  QStringLiteral("basename"),  QStringLiteral("dirname"),
        QStringLiteral("ip"),        QStringLiteral("ifconfig"),  QStringLiteral("ss"),
        QStringLiteral("netstat"),   QStringLiteral("ps"),        QStringLiteral("pgrep"),
        QStringLiteral("getent"),    QStringLiteral("dig"),       QStringLiteral("nslookup"),
        QStringLiteral("host"),      QStringLiteral("ping"),      QStringLiteral("traceroute"),
        QStringLiteral("tracepath"), QStringLiteral("echo"),      QStringLiteral("printf"),
        QStringLiteral("true"),      QStringLiteral("false"),     QStringLiteral("test"),
        QStringLiteral("journalctl"),QStringLiteral("lsblk"),     QStringLiteral("lscpu"),
        QStringLiteral("lsusb"),     QStringLiteral("lspci"),     QStringLiteral("dmesg"),
        QStringLiteral("sysctl"),    QStringLiteral("arch"),      QStringLiteral("nproc"),
    };

    for (const QString& stageRaw : stages) {
        const QString stage = stageRaw.trimmed();
        if (stage.isEmpty()) {
            return false;
        }
        // Tokenize roughly on whitespace.
        const QStringList toks = stage.split(QRegularExpression(QStringLiteral("\\s+")),
                                             Qt::SkipEmptyParts);
        if (toks.isEmpty()) {
            return false;
        }
        const QString bin = baseName(toks.first());

        if (bin == QLatin1String("systemctl")) {
            if (toks.size() < 2) {
                return false;
            }
            const QString sub = toks.at(1).toLower();
            static const QSet<QString> ok = {
                QStringLiteral("status"), QStringLiteral("show"), QStringLiteral("cat"),
                QStringLiteral("list-units"), QStringLiteral("list-unit-files"),
                QStringLiteral("is-active"), QStringLiteral("is-enabled"),
                QStringLiteral("is-failed"), QStringLiteral("list-timers"),
            };
            if (!ok.contains(sub)) {
                return false;
            }
            continue;
        }
        if (bin == QLatin1String("git")) {
            if (toks.size() < 2) {
                return false;
            }
            const QString sub = toks.at(1).toLower();
            static const QSet<QString> ok = {
                QStringLiteral("status"), QStringLiteral("log"), QStringLiteral("diff"),
                QStringLiteral("show"), QStringLiteral("branch"), QStringLiteral("remote"),
                QStringLiteral("rev-parse"), QStringLiteral("ls-files"),
            };
            if (!ok.contains(sub)) {
                return false;
            }
            continue;
        }
        if (bin == QLatin1String("docker")) {
            if (toks.size() < 2) {
                return false;
            }
            const QString sub = toks.at(1).toLower();
            static const QSet<QString> ok = {
                QStringLiteral("ps"), QStringLiteral("images"), QStringLiteral("logs"),
                QStringLiteral("inspect"), QStringLiteral("version"), QStringLiteral("info"),
            };
            if (!ok.contains(sub)) {
                return false;
            }
            continue;
        }

        if (!allow.contains(bin)) {
            return false;
        }
    }
    return true;
}

QString AiAgentPanel::extractThink(const QString& text, QString* remainder)
{
    static const QRegularExpression thinkRe(
        QStringLiteral("<think>\\s*([\\s\\S]*?)\\s*</think>"),
        QRegularExpression::CaseInsensitiveOption);
    QString rest = text;
    QString thinking;
    auto it = thinkRe.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (!thinking.isEmpty()) {
            thinking += QLatin1Char('\n');
        }
        thinking += m.captured(1).trimmed();
        rest.remove(m.captured(0));
    }
    // Also accept markdown-style ### Thinking sections lightly? stick to tags.
    if (remainder) {
        *remainder = rest.trimmed();
    }
    return thinking;
}

QStringList AiAgentPanel::extractCommands(const QString& assistantText)
{
    QStringList out;
    static const QRegularExpression fence(
        QStringLiteral("```(?:shell|bash|sh|zsh)\\s*\\n([\\s\\S]*?)```"),
        QRegularExpression::CaseInsensitiveOption);
    auto it = fence.globalMatch(assistantText);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString block = m.captured(1).trimmed();
        // One command per fence preferred; if multiple lines, take non-comment lines as separate steps.
        const QStringList lines = block.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QString joined;
        for (QString line : lines) {
            line = line.trimmed();
            if (line.startsWith(QLatin1Char('#'))) {
                continue;
            }
            if (line.startsWith(QLatin1String("$ "))) {
                line = line.mid(2).trimmed();
            }
            if (line.isEmpty()) {
                continue;
            }
            // If the model put multiple commands in one fence, split into queue entries.
            out.push_back(line);
        }
        Q_UNUSED(joined);
    }
    return out;
}

bool AiAgentPanel::marksDone(const QString& text)
{
    static const QRegularExpression doneRe(
        QStringLiteral("(?:^|\\n)\\s*DONE\\s*(?:\\n|$)"),
        QRegularExpression::CaseInsensitiveOption);
    return doneRe.match(text).hasMatch();
}
