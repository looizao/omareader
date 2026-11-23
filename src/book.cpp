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

// The accents a search ignores: the combining marks Latin, Greek and
// Cyrillic letters take. Marks that are letters' vowels in other scripts
// stay, so they still tell words apart.
static bool ignoredMark(char32_t c) {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF)
           || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F);
}

// A character taken apart as far as its canonical decomposition goes, so a
// precomposed letter and the same letter with separate marks come out alike.
static void decompose(char32_t c, QList<char32_t> &out) {
    if (c < 0xC0 || QChar::decompositionTag(c) != QChar::Canonical) {
        out.append(c);
        return;
    }
    const QString parts = QChar::decomposition(c);
    if (parts.isEmpty()) {
        out.append(c);
        return;
    }
    for (const char32_t part : parts.toUcs4())
        decompose(part, out);
}

QString fold(const QString &text, QList<int> *offsets) {
    QString out;
    out.reserve(text.size());
    QList<int> map;
    map.reserve(text.size());
    QList<char32_t> parts;
    for (int i = 0; i < text.size();) {
        char32_t c = text.at(i).unicode();
        int width = 1;
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
            c = QChar::surrogateToUcs4(text.at(i), text.at(i + 1));
            width = 2;
        }
        parts.clear();
        decompose(c, parts);
        for (char32_t part : parts) {
            if (ignoredMark(part))
                continue;
            switch (part) {
            case 0x2018: case 0x2019: case 0x201B: case 0x02BC: case 0x2032:
                part = U'\'';
                break;
            case 0x201C: case 0x201D: case 0x201F: case 0x2033:
                part = U'"';
                break;
            case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212:
                part = U'-';
                break;
            default:
                part = QChar::isSpace(part) ? U' ' : QChar::toCaseFolded(part);
            }
            if (QChar::requiresSurrogates(part)) {
                out.append(QChar(QChar::highSurrogate(part)));
                out.append(QChar(QChar::lowSurrogate(part)));
                map.append(i);
                map.append(i);
            } else {
                out.append(QChar(char16_t(part)));
                map.append(i);
            }
        }
        i += width;
    }
    if (offsets)
        *offsets = map;
    return out;
}

QList<QPair<int, int>> find(const QString &text, const QString &folded, const QList<int> &offsets,
                            const QString &needle) {
    QList<QPair<int, int>> found;
    if (needle.isEmpty())
        return found;
    const int size = int(folded.size());
    for (int at = int(folded.indexOf(needle)); at >= 0; at = int(folded.indexOf(needle, at + 1))) {
        const int end = at + int(needle.size());
        // A match has to begin and end on whole characters of the text, not
        // on a part of one, as a Korean syllable's first letter is.
        if ((at > 0 && offsets.at(at - 1) == offsets.at(at)) || (end < size && offsets.at(end) == offsets.at(end - 1)))
            continue;
        if (!found.isEmpty() && offsets.at(at) < found.last().second)
            continue;
        // The marks a match's last letter carries belong to it.
        found.append({offsets.at(at), end < size ? offsets.at(end) : int(text.size())});
    }
    return found;
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
