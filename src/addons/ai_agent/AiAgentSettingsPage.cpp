#include "AiAgentSettingsPage.h"
#include "AiAgentConfig.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

AiAgentSettingsPage::AiAgentSettingsPage(QWidget* parent)
    : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 12, 16, 12);
    lay->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("AI agent"), this);
    title->setObjectName(QStringLiteral("dashPageTitle"));
    auto* hint = new QLabel(
        QStringLiteral("OpenAI-compatible providers only for now (OpenAI, Azure OpenAI-compatible "
                       "gateways, Ollama /v1, LiteLLM, etc.)."),
        this);
    hint->setObjectName(QStringLiteral("dashHint"));
    hint->setWordWrap(true);

    auto* formHost = new QWidget(this);
    auto* form = new QFormLayout(formHost);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(8);

    m_type = new QComboBox(formHost);
    m_type->addItem(QStringLiteral("OpenAI-compatible API"), QStringLiteral("openai"));

    m_apiBase = new QLineEdit(formHost);
    m_apiBase->setPlaceholderText(QStringLiteral("https://api.openai.com/v1"));

    m_model = new QLineEdit(formHost);
    m_model->setPlaceholderText(QStringLiteral("gpt-4o-mini"));

    m_apiKey = new QLineEdit(formHost);
    m_apiKey->setEchoMode(QLineEdit::Password);
    m_apiKey->setPlaceholderText(QStringLiteral("sk-… (optional for local gateways)"));

    form->addRow(QStringLiteral("Provider type"), m_type);
    form->addRow(QStringLiteral("API base"), m_apiBase);
    form->addRow(QStringLiteral("Model name"), m_model);
    form->addRow(QStringLiteral("API key"), m_apiKey);

    lay->addWidget(title);
    lay->addWidget(hint);
    lay->addWidget(formHost);
    lay->addStretch(1);

    loadFromSettings();

    connect(m_type, &QComboBox::currentIndexChanged, this, [this](int) { persist(); });
    connect(m_apiBase, &QLineEdit::editingFinished, this, [this]() { persist(); });
    connect(m_model, &QLineEdit::editingFinished, this, [this]() { persist(); });
    connect(m_apiKey, &QLineEdit::editingFinished, this, [this]() { persist(); });
}

void AiAgentSettingsPage::loadFromSettings()
{
    const QString type = AiAgentConfig::providerType();
    const int idx = m_type->findData(type);
    m_type->setCurrentIndex(idx >= 0 ? idx : 0);
    m_apiBase->setText(AiAgentConfig::apiBase());
    m_model->setText(AiAgentConfig::modelName());
    m_apiKey->setText(AiAgentConfig::apiKey());
}

void AiAgentSettingsPage::persist()
{
    AiAgentConfig::setProviderType(m_type->currentData().toString());
    AiAgentConfig::setApiBase(m_apiBase->text());
    AiAgentConfig::setModelName(m_model->text());
    AiAgentConfig::setApiKey(m_apiKey->text());
}
