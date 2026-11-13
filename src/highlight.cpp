#include "highlight.h"

#include "book.h"

#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QFontMetricsF>
#include <QRawFont>
#include <QtMath>
#include <QTextBlock>
#include <QTextLayout>
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

void ReadingHighlight::setMarks(const QVariantList &marks) {
    if (m_marks == marks)
        return;
    m_marks = marks;
    emit stylesChanged();
    refresh();
}

void ReadingHighlight::setMarkColor(const QColor &color) {
    if (m_markColor == color)
        return;
    m_markColor = color;
    emit stylesChanged();
    refresh();
}

void ReadingHighlight::setNoteColor(const QColor &color) {
    if (m_noteColor == color)
        return;
    m_noteColor = color;
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
    // The reader's highlights belong to the page like the styles do, so
    // the voice's highlight passes over them and leaves them as they were.
    for (const QVariant &value : std::as_const(m_marks)) {
        const QVariantMap mark = value.toMap();
        const int from = qMax(start, mark.value("start").toInt());
        const int to = qMin(end, mark.value("end").toInt());
        if (to <= from)
            continue;
        QTextCharFormat format;
        if (m_markColor.isValid() && m_markColor.alpha() > 0)
            format.setBackground(m_markColor);
        if (mark.value("note").toBool() && m_noteColor.isValid()) {
            format.setUnderlineStyle(QTextCharFormat::DotLine);
            format.setUnderlineColor(m_noteColor);
        }
        cursor.setPosition(from);
        cursor.setPosition(to, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
    // A character the font lacks is drawn from a fallback font, often a
    // taller one, and Qt sizes every line that shares its formatting with
    // it by that font. Formatting it apart keeps that to its own line, and
    // the same however the highlight colours the text around it.
    QTextCharFormat apart;
    apart.setProperty(QTextFormat::UserProperty, true);
    for (const auto &[from, to] : std::as_const(m_fallback)) {
        if (to <= start || from >= end)
            continue;
        cursor.setPosition(qMax(start, from));
        cursor.setPosition(qMin(end, to), QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(apart);
    }
}

QFont ReadingHighlight::textFont() const {
    QTextDocument *doc = textDocument();
    const bool set = !m_font.family().isEmpty() || m_font.pixelSize() > 0 || m_font.pointSizeF() > 0;
    return set || !doc ? m_font : doc->defaultFont();
}

void ReadingHighlight::refresh() {
    QTextDocument *doc = textDocument();
    if (!doc)
        return;
    m_applying = true;
    QTextCursor cursor(doc);
    cursor.select(QTextCursor::Document);
    const QFontMetricsF metrics(textFont());
    QTextBlockFormat block;
    // The height Qt gives a line of this font, rounded the way it rounds it,
    // so the spacing is what a proportional line height would give.
    const qreal natural = qCeil(metrics.ascent()) + qCeil(metrics.descent());
    block.setLineHeight(natural * m_lineHeight, QTextBlockFormat::FixedHeight);
    cursor.mergeBlockFormat(block);
    m_fallback.clear();
    // The font itself, without the fallbacks QFontMetrics would count.
    const QRawFont raw = QRawFont::fromFont(textFont());
    const QString text = doc->toPlainText();
    for (int i = 0; i < text.size();) {
        const char32_t code = text.at(i).isHighSurrogate() && i + 1 < text.size()
                                  ? QChar::surrogateToUcs4(text.at(i), text.at(i + 1)) : text.at(i).unicode();
        const int size = code > 0xFFFF ? 2 : 1;
        if (!QChar::isSpace(code) && QChar::isPrint(code) && raw.isValid() && !raw.supportsCharacter(code))
            m_fallback.append({i, i + size});
        i += size;
    }
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
    if (m_selectionStart >= 0 && m_selectionEnd > m_selectionStart && shows(m_selectionColor)) {
        QTextCharFormat selection;
        selection.setBackground(m_selectionColor);
        mark(m_selectionStart, m_selectionEnd, selection);
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

QVariantList ReadingHighlight::lines() const {
    QVariantList found;
    QTextDocument *doc = textDocument();
    if (!doc)
        return found;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        const QTextLayout *layout = block.layout();
        if (!layout)
            continue;
        const qreal top = layout->position().y();
        for (int i = 0; i < layout->lineCount(); ++i) {
            const QTextLine line = layout->lineAt(i);
            QVariantMap entry;
            entry.insert("y", top + line.y());
            entry.insert("baseline", top + line.y() + line.ascent());
            entry.insert("start", block.position() + line.textStart());
            found.append(entry);
        }
    }
    return found;
}

// Compiled as part of this file, not on its own; see omareader.pro.
#include "moc_highlight.cpp"
