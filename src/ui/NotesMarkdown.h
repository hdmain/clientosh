#pragma once

#include <QPoint>
#include <QString>

class QTextCharFormat;
class QTextEdit;

namespace NotesMarkdown {

/** UserProperty id marking spoiler runs in the rich-text document. */
constexpr int kSpoilerProperty = 0x434E5350; // 'CNSP'

QTextCharFormat spoilerFormat(bool revealed);

/**
 * Load notes into the editor.
 * Prefers HTML (preserves blank lines). Legacy Markdown (incl. ||spoilers||) still loads.
 */
void applyMarkdown(QTextEdit* edit, const QString& stored);

/**
 * Export editor content for vault/sync.
 * Returns HTML so empty paragraphs and formatting round-trip faithfully.
 */
QString toMarkdown(const QTextEdit* edit);

/** Toggle covered/revealed look for the spoiler under the cursor. */
bool toggleSpoilerAtCursor(QTextEdit* edit, const QPoint& viewportPos);

/** Apply spoiler formatting on the current selection. */
void applySpoilerToSelection(QTextEdit* edit);

} // namespace NotesMarkdown
