#include "NotesMarkdown.h"

#include <QAbstractTextDocumentLayout>
#include <QColor>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QVector>

namespace NotesMarkdown {
namespace {

constexpr const char kSpoilerHref[] = "clientosh:spoiler";

bool isSpoilerFormat(const QTextCharFormat& fmt)
{
    if (fmt.hasProperty(kSpoilerProperty) && fmt.property(kSpoilerProperty).toBool()) {
        return true;
    }
    return fmt.isAnchor() && fmt.anchorHref() == QLatin1String(kSpoilerHref);
}

bool looksLikeHtml(const QString& stored)
{
    const QString t = stored.trimmed();
    if (t.isEmpty()) {
        return false;
    }
    return t.startsWith(QLatin1String("<!DOCTYPE"), Qt::CaseInsensitive)
        || t.startsWith(QLatin1String("<html"), Qt::CaseInsensitive)
        || t.startsWith(QLatin1String("<meta"), Qt::CaseInsensitive)
        || t.startsWith(QLatin1String("<!--StartFragment"), Qt::CaseInsensitive)
        || t.startsWith(QLatin1String("<body"), Qt::CaseInsensitive)
        || t.startsWith(QLatin1String("<p"), Qt::CaseInsensitive);
}

struct SpoilerRange {
    int start = 0;
    int end = 0;
};

QVector<SpoilerRange> collectSpoilerRanges(QTextDocument* doc)
{
    QVector<SpoilerRange> out;
    if (!doc) {
        return out;
    }
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !(it.atEnd()); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || !isSpoilerFormat(frag.charFormat())) {
                continue;
            }
            SpoilerRange r{frag.position(), frag.position() + frag.length()};
            if (!out.isEmpty() && out.last().end == r.start) {
                out.last().end = r.end;
            } else {
                out.push_back(r);
            }
        }
    }
    return out;
}

QTextCursor spoilerRangeAt(QTextDocument* doc, int pos)
{
    if (!doc || pos < 0) {
        return {};
    }
    // Strict: only a character *inside* the spoiler run (not after it / nearby).
    for (const SpoilerRange& r : collectSpoilerRanges(doc)) {
        if (pos >= r.start && pos < r.end) {
            QTextCursor sel(doc);
            sel.setPosition(r.start);
            sel.setPosition(r.end, QTextCursor::KeepAnchor);
            return sel;
        }
    }
    return {};
}

void restoreSpoilersFromAnchors(QTextDocument* doc)
{
    if (!doc) {
        return;
    }
    const QVector<SpoilerRange> ranges = collectSpoilerRanges(doc);
    for (const SpoilerRange& r : ranges) {
        QTextCursor cur(doc);
        cur.setPosition(r.start);
        cur.setPosition(r.end, QTextCursor::KeepAnchor);
        // Covered look after reload (anchors alone are not enough).
        cur.mergeCharFormat(spoilerFormat(false));
    }
}

void ensureSpoilerAnchors(QTextDocument* doc)
{
    if (!doc) {
        return;
    }
    for (const SpoilerRange& r : collectSpoilerRanges(doc)) {
        QTextCursor cur(doc);
        cur.setPosition(r.start);
        cur.setPosition(r.end, QTextCursor::KeepAnchor);
        QTextCharFormat fmt = cur.charFormat();
        if (!fmt.isAnchor() || fmt.anchorHref() != QLatin1String(kSpoilerHref)) {
            cur.mergeCharFormat(spoilerFormat(false));
        }
    }
}

void applyLegacyMarkdown(QTextEdit* edit, const QString& markdown)
{
    QString rewritten;
    rewritten.reserve(markdown.size());
    QVector<QString> spoilerTexts;

    static const QRegularExpression re(QStringLiteral(R"(\|\|([\s\S]+?)\|\|)"));
    int last = 0;
    auto it = re.globalMatch(markdown);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        rewritten += markdown.mid(last, m.capturedStart() - last);
        const int idx = spoilerTexts.size();
        spoilerTexts.push_back(m.captured(1));
        rewritten += QChar(0xE000);
        rewritten += QString::number(idx);
        rewritten += QChar(0xE001);
        last = m.capturedEnd();
    }
    rewritten += markdown.mid(last);

    edit->document()->setMarkdown(rewritten, QTextDocument::MarkdownDialectGitHub);

    if (spoilerTexts.isEmpty()) {
        return;
    }

    static const QRegularExpression tokenRe(QStringLiteral("\uE000(\\d+)\uE001"));
    QTextDocument* doc = edit->document();
    QTextCursor findCur(doc);
    while (true) {
        findCur = doc->find(tokenRe, findCur);
        if (findCur.isNull()) {
            break;
        }
        const QRegularExpressionMatch m = tokenRe.match(findCur.selectedText());
        if (!m.hasMatch()) {
            break;
        }
        const int idx = m.captured(1).toInt();
        const QString text =
            (idx >= 0 && idx < spoilerTexts.size()) ? spoilerTexts.at(idx) : QString();
        findCur.insertText(text, spoilerFormat(false));
    }
}

} // namespace

QTextCharFormat spoilerFormat(bool revealed)
{
    QTextCharFormat fmt;
    fmt.setProperty(kSpoilerProperty, true);
    fmt.setAnchor(true);
    fmt.setAnchorHref(QString::fromLatin1(kSpoilerHref));
    if (revealed) {
        fmt.setForeground(QColor(0xe8, 0xe8, 0xe8));
        fmt.setBackground(QColor(0x3a, 0x3a, 0x3a));
        fmt.setToolTip(QStringLiteral("Spoiler (click to hide)"));
    } else {
        fmt.setForeground(QColor(0x1a, 0x1a, 0x1a));
        fmt.setBackground(QColor(0x1a, 0x1a, 0x1a));
        fmt.setToolTip(QStringLiteral("Spoiler (click to reveal)"));
    }
    return fmt;
}

void applyMarkdown(QTextEdit* edit, const QString& stored)
{
    if (!edit) {
        return;
    }
    if (looksLikeHtml(stored)) {
        edit->setHtml(stored);
        restoreSpoilersFromAnchors(edit->document());
    } else {
        applyLegacyMarkdown(edit, stored);
    }
    edit->moveCursor(QTextCursor::Start);
}

QString toMarkdown(const QTextEdit* edit)
{
    if (!edit || !edit->document()) {
        return {};
    }
    // HTML preserves empty paragraphs; Markdown collapses them.
    QTextDocument clone;
    clone.setHtml(edit->document()->toHtml());
    ensureSpoilerAnchors(&clone);
    return clone.toHtml();
}

bool toggleSpoilerAtCursor(QTextEdit* edit, const QPoint& viewportPos)
{
    if (!edit || !edit->document() || !edit->document()->documentLayout()) {
        return false;
    }
    // ExactHit ignores empty background / padding — only true text glyphs count.
    const QPointF docPos(
        viewportPos.x() + edit->horizontalScrollBar()->value(),
        viewportPos.y() + edit->verticalScrollBar()->value());
    const int pos = edit->document()->documentLayout()->hitTest(docPos, Qt::ExactHit);
    if (pos < 0) {
        return false;
    }
    QTextCursor range = spoilerRangeAt(edit->document(), pos);
    if (range.isNull() || !range.hasSelection()) {
        return false;
    }
    const bool revealed = range.charFormat().foreground().color().lightness() > 40;
    range.mergeCharFormat(spoilerFormat(!revealed));
    return true;
}

void applySpoilerToSelection(QTextEdit* edit)
{
    if (!edit) {
        return;
    }
    QTextCursor cursor = edit->textCursor();
    if (!cursor.hasSelection()) {
        return;
    }
    cursor.mergeCharFormat(spoilerFormat(false));
    edit->setTextCursor(cursor);
}

} // namespace NotesMarkdown
