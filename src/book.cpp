#include "book.h"

#include <QRegularExpression>
#include <QTextBoundaryFinder>

namespace Text {

namespace {
bool isSpace(QChar c) { return c.isSpace() || c == QChar(0x00A0); }

// Breaks [start, end) into pieces no longer than maxLength, preferring the
// last clause mark (comma, semicolon, colon, em or en dash) in the window and falling back to a space.
void splitLong(const QString &text, int start, int end, int maxLength, QList<QPair<int, int>> &out) {
    while (end - start > maxLength) {
        int cut = -1;
        for (int i = start + maxLength; i > start + maxLength / 3; --i) {
            const QChar c = text.at(i - 1);
            if (c == ',' || c == ';' || c == ':' || c == QChar(0x2014) || c == QChar(0x2013)) {
                cut = i;
                break;
            }
        }
        if (cut < 0)
            for (int i = start + maxLength; i > start + maxLength / 3; --i)
                if (isSpace(text.at(i - 1))) {
                    cut = i;
                    break;
                }
        if (cut < 0)
            cut = start + maxLength;
        out.append({start, cut});
        start = cut;
        while (start < end && isSpace(text.at(start)))
            ++start;
    }
    if (end > start)
        out.append({start, end});
}
}

QList<QPair<int, int>> sentenceRanges(const QString &text, int maxLength) {
    QList<QPair<int, int>> ranges;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Sentence, text);
    int previous = 0;
    auto add = [&](int start, int end) {
        while (start < end && isSpace(text.at(start)))
            ++start;
        while (end > start && isSpace(text.at(end - 1)))
            --end;
        if (end > start)
            splitLong(text, start, end, maxLength, ranges);
    };
    while (finder.toNextBoundary() != -1) {
        const int position = finder.position();
        add(previous, position);
        previous = position;
    }
    add(previous, int(text.size()));

    // Unicode sentence rules end a sentence at every abbreviation ("Mr.",
    // "e.g."); glue those back onto the sentence that follows.
    static const QRegularExpression abbreviation(
        R"((?:^|\s)(?:Mr|Mrs|Ms|Dr|Prof|Sr|Jr|St|vs|etc|e\.g|i\.e|No|Sra|Srta)\.$)",
        QRegularExpression::CaseInsensitiveOption);
    QList<QPair<int, int>> merged;
    for (const auto &range : ranges) {
        if (!merged.isEmpty()) {
            auto &last = merged.last();
            const QString tail = text.mid(last.first, last.second - last.first);
            if (abbreviation.match(tail).hasMatch() && range.second - last.first <= maxLength) {
                last.second = range.second;
                continue;
            }
        }
        merged.append(range);
    }
    return merged;
}

QList<WordSpan> words(const QString &text, int start, int end) {
    // Letters and digits, keeping apostrophes and hyphens inside a word
    // ("don't", "well-known") the way Kokoro reports them.
    static const QRegularExpression word(R"([\p{L}\p{N}\p{M}]+(?:['’\-][\p{L}\p{N}\p{M}]+)*)");
    QList<WordSpan> spans;
    auto it = word.globalMatch(text.mid(start, end - start));
    while (it.hasNext()) {
        const auto match = it.next();
        spans.append({start + int(match.capturedStart()), int(match.capturedLength())});
    }
    return spans;
}

QString normalizeWord(const QString &word) {
    QString out;
    out.reserve(word.size());
    for (const QChar c : word.normalized(QString::NormalizationForm_D))
        if (c.isLetterOrNumber())
            out.append(c.toLower());
    return out;
}

}

void Book::segment() {
    sentences.clear();
    chapterFirstSentence.clear();
    for (int c = 0; c < chapters.size(); ++c) {
        chapterFirstSentence.append(int(sentences.size()));
        const auto &paragraphs = chapters.at(c).paragraphs;
        for (int p = 0; p < paragraphs.size(); ++p) {
            const QString &text = paragraphs.at(p).text;
            for (const auto &range : Text::sentenceRanges(text)) {
                Sentence sentence;
                sentence.chapter = c;
                sentence.paragraph = p;
                sentence.start = range.first;
                sentence.end = range.second;
                sentence.words = Text::words(text, range.first, range.second);
                sentences.append(sentence);
            }
        }
    }
}

QString Book::sentenceText(int index) const {
    if (index < 0 || index >= sentences.size())
        return {};
    const Sentence &s = sentences.at(index);
    QString text = chapters.at(s.chapter).paragraphs.at(s.paragraph).text.mid(s.start, s.end - s.start);
    text.replace('\n', ' ');
    return text.simplified();
}

QStringList Book::sentenceWords(int index) const {
    QStringList out;
    if (index < 0 || index >= sentences.size())
        return out;
    const Sentence &s = sentences.at(index);
    const QString &text = chapters.at(s.chapter).paragraphs.at(s.paragraph).text;
    for (const auto &w : s.words)
        out.append(text.mid(w.start, w.length));
    return out;
}

int Book::sentenceAt(int chapter, int paragraph, int offset) const {
    if (chapter < 0 || chapter >= chapterFirstSentence.size())
        return -1;
    const int first = chapterFirstSentence.at(chapter);
    const int last = chapter + 1 < chapterFirstSentence.size() ? chapterFirstSentence.at(chapter + 1)
                                                                : int(sentences.size());
    for (int i = first; i < last; ++i) {
        const Sentence &s = sentences.at(i);
        if (s.paragraph > paragraph || (s.paragraph == paragraph && offset < s.end))
            return i;
    }
    return last > first ? last - 1 : -1;
}

int Book::wordCount() const {
    int count = 0;
    for (const auto &s : sentences)
        count += int(s.words.size());
    return count;
}
