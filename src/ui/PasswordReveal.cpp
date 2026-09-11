#include "PasswordReveal.h"

#include <QAction>
#include <QIcon>
#include <QLineEdit>

namespace ui {

namespace {

constexpr const char* kRevealActionProp = "passwordReveal";

} // namespace

void attachPasswordReveal(QLineEdit* edit)
{
    if (!edit) {
        return;
    }

    edit->setEchoMode(QLineEdit::Password);

    auto* toggle = edit->addAction(QIcon(QStringLiteral(":/icons/filetypes/eye-off.svg")),
                                   QLineEdit::TrailingPosition);
    toggle->setProperty(kRevealActionProp, true);
    toggle->setToolTip(QStringLiteral("Show password"));

    QObject::connect(toggle, &QAction::triggered, edit, [edit, toggle]() {
        const bool hidden = edit->echoMode() == QLineEdit::Password;
        edit->setEchoMode(hidden ? QLineEdit::Normal : QLineEdit::Password);
        toggle->setIcon(QIcon(hidden ? QStringLiteral(":/icons/filetypes/eye.svg")
                                     : QStringLiteral(":/icons/filetypes/eye-off.svg")));
        toggle->setToolTip(hidden ? QStringLiteral("Hide password")
                                  : QStringLiteral("Show password"));
    });
}

void resetPasswordReveal(QLineEdit* edit)
{
    if (!edit) {
        return;
    }

    edit->setEchoMode(QLineEdit::Password);

    for (QAction* action : edit->actions()) {
        if (action && action->property(kRevealActionProp).toBool()) {
            action->setIcon(QIcon(QStringLiteral(":/icons/filetypes/eye-off.svg")));
            action->setToolTip(QStringLiteral("Show password"));
            break;
        }
    }
}

} // namespace ui
