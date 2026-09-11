#pragma once

class QLineEdit;

namespace ui {

/** Password echo + trailing eye toggle (show / hide). */
void attachPasswordReveal(QLineEdit* edit);

/** Force hidden echo and restore the eye-off icon / tooltip. */
void resetPasswordReveal(QLineEdit* edit);

} // namespace ui
