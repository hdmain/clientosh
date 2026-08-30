#include "PasswordReveal.h"

#include <QAction>
#include <QIcon>
#include <QLineEdit>

namespace ui {

void attachPasswordReveal(QLineEdit* edit)
{
    if (!edit) {
        return;
    }

    edit->setEchoMode(QLineEdit::Password);

    auto* toggle = edit->addAction(QIcon(QStringLiteral(":/icons/filetypes/eye-off.svg")),
                                   QLineEdit::TrailingPosition);
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

} // namespace ui
