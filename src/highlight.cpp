#include "highlight.h"

#include "book.h"

#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>

ReadingHighlight::ReadingHighlight(QObject *parent) : QObject(parent) {}

QTextDocument *ReadingHighlight::textDocument() const {
    return m_document ? m_document->textDocument() : nullptr;
}

void ReadingHighlight::setDocument(QQuickTextDocument *document) {
    if (m_document == document)
        return;
    disconnect(m_contents);
    m_document = document;
    m_appliedStart = m_appliedEnd = -1;
    if (QTextDocument *doc = textDocument()) {
        // Highlight changes are formatting edits; keep them off the undo stack.
        doc->setUndoRedoEnabled(false);
        // New text replaces the formatting, so lay it all on again.
        m_contents = connect(doc, &QTextDocument::contentsChanged, this, [this] {
            if (!m_applying)
                refresh();
        });
    }
    emit documentChanged();
    refresh();
}

void ReadingHighlight::setStyles(const QVariantList &styles) {
    if (m_styles == styles)
        return;
    m_styles = styles;
    emit stylesChanged();
    refresh();
}

void ReadingHighlight::restyle(int start, int end) {
    QTextDocument *doc = textDocument();
    const int length = doc->characterCount() - 1;
    start = qBound(0, start, length);
    end = qBound(0, end, length);
    if (end <= start)
        return;
    QTextCursor cursor(doc);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.setCharFormat(QTextCharFormat());
    for (const QVariant &value : std::as_const(m_styles)) {
        const QVariantMap style = value.toMap();
        const int from = qMax(start, style.value("start").toInt());
        const int to = qMin(end, style.value("start").toInt() + style.value("length").toInt());
        const int flags = style.value("flags").toInt();
        if (to <= from || !flags)
            continue;
        QTextCharFormat format;
        if (flags & TextStyle::Bold)
            format.setFontWeight(QFont::Bold);
        if (flags & TextStyle::Italic)
            format.setFontItalic(true);
        if (flags & TextStyle::Strike)
            format.setFontStrikeOut(true);
        if (flags & TextStyle::Code)
            format.setFontFamilies({QStringLiteral("monospace")});
        cursor.setPosition(from);
        cursor.setPosition(to, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
}

void ReadingHighlight::refresh() {
    QTextDocument *doc = textDocument();
    if (!doc)
        return;
    m_applying = true;
    QTextCursor cursor(doc);
    cursor.select(QTextCursor::Document);
    QTextBlockFormat block;
    block.setLineHeight(m_lineHeight * 100, QTextBlockFormat::ProportionalHeight);
    cursor.mergeBlockFormat(block);
    restyle(0, doc->characterCount() - 1);
    m_applying = false;
    m_appliedStart = m_appliedEnd = -1;
    apply();
}

void ReadingHighlight::apply() {
    QTextDocument *doc = textDocument();
    if (!doc)
        return;
    m_applying = true;
    const int length = doc->characterCount() - 1;
    if (m_appliedStart >= 0)
        restyle(m_appliedStart, m_appliedEnd);
    m_appliedStart = m_appliedEnd = -1;
    auto mark = [&](int start, int end, const QTextCharFormat &format) {
        start = qBound(0, start, length);
        end = qBound(0, end, length);
        if (end <= start)
            return;
        QTextCursor cursor(doc);
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
        m_appliedStart = m_appliedStart < 0 ? start : qMin(m_appliedStart, start);
        m_appliedEnd = qMax(m_appliedEnd, end);
    };
    // A colour that shows; a transparent one means "leave this alone".
    auto shows = [](const QColor &c) { return c.isValid() && c.alpha() > 0; };

    const bool inSentence = m_sentenceStart >= 0 && m_sentenceEnd > m_sentenceStart;
    if (inSentence && shows(m_quietColor)) {
        QTextCharFormat quiet;
        quiet.setForeground(m_quietColor);
        mark(0, m_sentenceStart, quiet);
        mark(m_sentenceEnd, length, quiet);
    }
    if (m_readEnd > 0 && shows(m_readColor)) {
        QTextCharFormat read;
        read.setForeground(m_readColor);
        mark(0, m_readEnd, read);
    }
    if (inSentence && shows(m_sentenceColor)) {
        QTextCharFormat sentence;
        sentence.setBackground(m_sentenceColor);
        mark(m_sentenceStart, m_sentenceEnd, sentence);
    }
    if (m_wordStart >= 0 && m_wordEnd > m_wordStart && (shows(m_wordColor) || shows(m_wordTextColor))) {
        QTextCharFormat word;
        if (shows(m_wordColor))
            word.setBackground(m_wordColor);
        if (shows(m_wordTextColor))
            word.setForeground(m_wordTextColor);
        mark(m_wordStart, m_wordEnd, word);
    }
    m_applying = false;
}

// Compiled as part of this file, not on its own; see omareader.pro.
#include "moc_highlight.cpp"
