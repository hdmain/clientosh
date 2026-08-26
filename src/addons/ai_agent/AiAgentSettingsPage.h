#pragma once

#include <QWidget>

class QComboBox;
class QLineEdit;
class QLabel;

class AiAgentSettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit AiAgentSettingsPage(QWidget* parent = nullptr);

private:
    void loadFromSettings();
    void persist();

    QComboBox* m_type = nullptr;
    QLineEdit* m_apiBase = nullptr;
    QLineEdit* m_model = nullptr;
    QLineEdit* m_apiKey = nullptr;
};
