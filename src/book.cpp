#include "book.h"

#include <QRegularExpression>
#include <QSet>
#include <QTextBoundaryFinder>

namespace Text {

namespace {
bool isSpace(QChar c) { return c.isSpace() || c == QChar(0x00A0); }

// Breaks [start, end) into pieces no longer than maxLength, preferring the
// last clause mark (comma, semicolon, colon, em or en dash) in the window and falling back to a space.
// Each piece but the last is marked as not ending its sentence in ends.
void splitLong(const QString &text, int start, int end, int maxLength, QList<QPair<int, int>> &out,
               QList<bool> &ends) {
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
        ends.append(false);
        start = cut;
        while (start < end && isSpace(text.at(start)))
            ++start;
    }
    if (end > start) {
        out.append({start, end});
        ends.append(true);
    }
}
}

QList<QPair<int, int>> sentenceRanges(const QString &text, int maxLength, QList<bool> *ends) {
    QList<QPair<int, int>> ranges;
    QList<bool> rangeEnds;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Sentence, text);
    int previous = 0;
    auto add = [&](int start, int end) {
        while (start < end && isSpace(text.at(start)))
            ++start;
        while (end > start && isSpace(text.at(end - 1)))
            --end;
        if (end > start)
            splitLong(text, start, end, maxLength, ranges, rangeEnds);
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
    QList<bool> mergedEnds;
    for (int i = 0; i < ranges.size(); ++i) {
        const auto &range = ranges.at(i);
        if (!merged.isEmpty()) {
            auto &last = merged.last();
            const QString tail = text.mid(last.first, last.second - last.first);
            if (abbreviation.match(tail).hasMatch() && range.second - last.first <= maxLength) {
                last.second = range.second;
                mergedEnds.last() = rangeEnds.at(i);
                continue;
            }
        }
        merged.append(range);
        mergedEnds.append(rangeEnds.at(i));
    }
    if (ends)
        *ends = mergedEnds;
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

namespace {
// The words before " [3]" that make it a value rather than a citation, as
// in "the logits are [1, 2]" or "a shape of [4]", or that point at the
// reference itself ("see [110]").
bool introducesValue(const QString &word) {
    static const QSet<QString> words{
        "a", "am", "an", "and", "are", "array", "arrays", "as", "at", "be", "became", "become", "becomes", "been",
        "between", "by", "called", "contain", "contains", "dimension", "dimensions", "else", "equal", "equals",
        "for", "from", "gave", "get", "gets", "give", "gives", "got", "in", "index", "indexes", "indices", "into",
        "is", "like", "list", "lists", "matrix", "named", "of", "on", "onto", "or", "output", "outputs", "print",
        "prints", "range", "ranges", "return", "returned", "returns", "set", "sets", "shape", "shaped", "shapes",
        "size", "sized", "than", "the", "then", "to", "tensor", "tensors", "tuple", "tuples", "value", "values",
        "vector", "vectors", "was", "were", "where", "with", "yield", "yields", "see"};
    return words.contains(word.toLower());
}

// Where a web address that starts at start ends: past what can be in one,
// without the punctuation that closes the sentence around it, and on past a
// line break a PDF put after a hyphen or inside the last part of its path.
qsizetype urlEnd(const QString &text, qsizetype start) {
    static const QRegularExpression token(R"([^\s<>"“”]+)");
    static const QRegularExpression pathPiece(R"(^[a-z0-9][\w.~%#?=&/-]*\.(?:html?|php|aspx?|pdf|md)\b[\w.~%#?=&/-]*$)");
    qsizetype end = start;
    auto take = [&](qsizetype from) {
        const auto match = token.match(text, from, QRegularExpression::NormalMatch,
                                       QRegularExpression::AnchorAtOffsetMatchOption);
        return match.hasMatch() ? match.capturedEnd() : from;
    };
    end = take(start);
    // Only an address cut short goes on: never one that ends its sentence
    // or clause, nor into a word that merely has a slash ("and/or").
    while (end > start && end < text.size() && text.at(end).isSpace()
           && !QStringLiteral(".,;:!?)]”’\"'").contains(text.at(end - 1))) {
        qsizetype next = end;
        while (next < text.size() && text.at(next).isSpace())
            ++next;
        if (next >= text.size() || next - end > 2)
            break;
        const QString following = text.mid(next, take(next) - next);
        if (following.isEmpty())
            break;
        const bool afterHyphen = text.at(end - 1) == '-' && (following.at(0).isLower() || following.at(0).isDigit());
        if (!afterHyphen && !pathPiece.match(following).hasMatch())
            break;
        end = take(next);
    }
    // A closing bracket belongs to the address only when it opened one.
    while (end > start) {
        const QChar last = text.at(end - 1);
        const QStringView url = QStringView(text).mid(start, end - start);
        if (QStringLiteral(".,;:!?’'”\"").contains(last)
            || (last == ')' && url.count('(') < url.count(')'))
            || (last == ']' && url.count('[') < url.count(']')))
            --end;
        else
            break;
    }
    return end;
}
}

QList<QPair<int, int>> clutter(const QString &text, bool footnoteNumbers) {
    QList<QPair<int, int>> found;
    auto add = [&](qsizetype start, qsizetype end) {
        if (end > start)
            found.append({int(start), int(end)});
    };
    const QString closers = QStringLiteral(".,;:!?”’\"')");

    // A footnote number a PDF leaves after a word's punctuation ("failure.1
    // While"). The word has two letters at least, so "Appendix A.1" keeps
    // its number, and isn't an abbreviation that numbers things ("Fig.1");
    // a colon is left alone, as numbers after one are ports and ratios.
    static const QRegularExpression afterPunctuation(R"((\p{L}+)([.,;!?”’"')\]]+)(\d{1,3})(?=\s|$))");
    static const QSet<QString> numbering{"art", "ch", "chap", "def", "ed", "eq", "eqs", "ex", "fig", "figs",
                                         "no", "nos", "op", "p", "para", "pp", "pt", "ref", "refs", "sec",
                                         "tab", "thm", "v", "ver", "vol", "vols"};
    for (auto it = afterPunctuation.globalMatch(footnoteNumbers ? text : QString()); it.hasNext();) {
        const auto m = it.next();
        if (m.capturedLength(1) < 2 || (m.captured(2).startsWith('.') && numbering.contains(m.captured(1).toLower())))
            continue;
        add(m.capturedStart(3), m.capturedEnd(3));
    }
    // One left on the word itself, where the PDF set the punctuation after
    // it apart ("a common pattern1 . For").
    static const QRegularExpression beforeLonePunctuation(R"(\p{Ll}{3}(\d{1,2})(?=\s[.,;:](?:\s|$)))");
    for (auto it = beforeLonePunctuation.globalMatch(footnoteNumbers ? text : QString()); it.hasNext();) {
        const auto m = it.next();
        add(m.capturedStart(1), m.capturedEnd(1));
    }

    // Numbered citations: "[12]", "[3, 7]", "[4–6]". Code has numbers in
    // brackets too ("argv[2]", "range [0, 1]"), so a citation follows
    // punctuation, or a space after a word that doesn't introduce a value,
    // never counts from 0, and is followed by a space or punctuation.
    static const QRegularExpression numbered(R"(\[(\d{1,3}(?:\s*[,–-]\s*\d{1,3})*)\])");
    static const QRegularExpression numberSplit(R"(\s*[,–-]\s*)");
    for (auto it = numbered.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        const qsizetype start = m.capturedStart(0), end = m.capturedEnd(0);
        // Then the end, punctuation, or a space and a word: never a number
        // or an arrow, as in printed output ("Hello [11] --> ,").
        const QChar after = end < text.size() ? text.at(end) : QChar(' ');
        qsizetype next = end;
        while (next < text.size() && text.at(next).isSpace())
            ++next;
        const bool wordNext = next >= text.size() || text.at(next).isLetter() || QStringLiteral("“\"‘'•(").contains(text.at(next));
        if (!(after.isSpace() && wordNext) && !closers.contains(after) && after != '[')
            continue;
        bool fromZero = false;
        for (const QString &number : m.captured(1).split(numberSplit))
            fromZero = fromZero || number.startsWith('0');
        if (fromZero)
            continue;
        const QChar before = start > 0 ? text.at(start - 1) : QChar();
        bool citation = false;
        if (!found.isEmpty() && found.last().second == start) {
            citation = true; // "[1][2]"
        } else if (closers.contains(before) && before != '\'' && start > 1 && text.at(start - 2).isLetter()) {
            citation = true; // "following,[5]"
        } else if (before.isSpace()) {
            qsizetype wordEnd = start - 1;
            while (wordEnd > 0 && text.at(wordEnd - 1).isSpace())
                --wordEnd;
            qsizetype wordStart = wordEnd;
            while (wordStart > 0 && text.at(wordStart - 1).isLetter())
                --wordStart;
            const bool afterWord = wordEnd - wordStart >= 2 && (wordStart == 0 || !text.at(wordStart - 1).isLetterOrNumber());
            citation = afterWord && !introducesValue(text.mid(wordStart, wordEnd - wordStart));
        }
        if (citation)
            add(start, end);
    }

    // Author-and-year keys ("[Bro96]", "[GHJV95; KP99]"), after anything
    // but a word they would index.
    static const QRegularExpression keys(
        R"((?<![\w\]])\[[A-Z][A-Za-z+]{1,8}\d{2,4}[a-z]?(?:\s*[,;]\s*[A-Z][A-Za-z+]{1,8}\d{2,4}[a-z]?)*\])");
    for (auto it = keys.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        add(m.capturedStart(), m.capturedEnd());
    }

    // An editor's or a wiki's remarks, and lettered notes after punctuation.
    // Words an editor put in brackets ("Children [with ADHD]") are kept.
    static const QRegularExpression asides(
        R"(\[(?i:sic!?|citation needed|clarification needed|edit|page needed|better source needed|)"
        R"(verification needed|failed verification|dubious|disputed|discuss|original research\??|)"
        R"(according to whom\??|by whom\??|who\?|when\?|which\?|where\?|why\?)\]|\[(?:…|\.\.\.)\])");
    for (auto it = asides.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        add(m.capturedStart(), m.capturedEnd());
    }
    static const QRegularExpression letteredNote(R"((?<=[.,;:!?”’"')])\[[a-z]{1,2}\])");
    for (auto it = letteredNote.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        add(m.capturedStart(), m.capturedEnd());
    }

    // Web addresses, and bare ones with a path ("oreil.ly/x8Fb").
    static const QRegularExpression url(
        R"((?<![\w@/.-])(?:(?:https?|ftp)://|www\.|(?:[A-Za-z0-9-]+\.)+)"
        R"((?:com|org|net|io|dev|ly|ai|edu|gov|co|me|app|info|uk|de|br|fr|jp|us)/))");
    for (auto it = url.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        add(m.capturedStart(), urlEnd(text, m.capturedStart()));
    }

    // Cross-references in parentheses, which point at a page, a figure or
    // another chapter: "(see Figure 2-3)", "(chapter 3)", "(pp. 12–15)".
    static const QRegularExpression crossReference(
        R"(\(\s*(?:(?i:see|cf\.?)\s+(?:(?i:also)\s+)?)?)"
        R"((?i:figures?|figs?\.|tables?|listings?|examples?|chapters?|sections?|appendix|appendices|)"
        R"(equations?|eqs?\.|exhibits?|pages?|pp?\.|§)\s*)"
        R"((?:\d[\w.:–-]*|[A-Z](?:[.\d–-][\w.–-]*)?|[ivxlcIVXLC]+))"
        R"((?:\s*(?:,|and|&|–|-|to|or)\s*(?:\d[\w.:–-]*|[A-Z](?:[.\d–-][\w.–-]*)?|[ivxlcIVXLC]+))*\s*\))");
    for (auto it = crossReference.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        add(m.capturedStart(), m.capturedEnd());
    }

    // Author-and-year citations: "(Vaswani et al., 2017)", "(Smith, 2020;
    // Lee and Park, 2019, p. 4)". Without "et al." the year needs a comma
    // before it, so "(Windows 2000)" stays.
    static const QRegularExpression authorYear(
        R"(\((?:[^()]{0,60}\bet al\.?,?\s+(?:19|20)\d{2}[a-z]?|)"
        R"(\s*[A-Z][\p{L}'’-]+(?:\s+(?:and|&)\s+[A-Z][\p{L}'’-]+)?,\s+(?:19|20)\d{2}[a-z]?)"
        R"((?:,\s*pp?\.\s*\d+(?:[–-]\d+)?)?)(?:\s*;[^()]{0,80})?\))");
    for (auto it = authorYear.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        add(m.capturedStart(), m.capturedEnd());
    }

    // A footnote number after a reference that goes ("(see Figure 4-12).49").
    static const QRegularExpression afterBracket(R"([.,;:!?”’"']*(\d{1,3})(?=\s|$))");
    for (int i = 0, count = footnoteNumbers ? int(found.size()) : 0; i < count; ++i) {
        const int end = found.at(i).second;
        if (end < 1 || (text.at(end - 1) != ')' && text.at(end - 1) != ']'))
            continue;
        const auto m = afterBracket.match(text, end, QRegularExpression::NormalMatch,
                                          QRegularExpression::AnchorAtOffsetMatchOption);
        if (m.hasMatch())
            add(m.capturedStart(1), m.capturedEnd(1));
    }

    std::sort(found.begin(), found.end());
    QList<QPair<int, int>> merged;
    for (const auto &range : std::as_const(found)) {
        if (!merged.isEmpty() && range.first <= merged.last().second)
            merged.last().second = qMax(merged.last().second, range.second);
        else
            merged.append(range);
    }
    return merged;
}

QString withoutRanges(const QString &text, const QList<QPair<int, int>> &ranges) {
    if (ranges.isEmpty())
        return text;
    QString out;
    out.reserve(text.size());
    int last = 0;
    for (const auto &range : ranges) {
        const int start = qBound(last, range.first, int(text.size()));
        out += QStringView(text).mid(last, start - last);
        out += ' ';
        last = qBound(start, range.second, int(text.size()));
    }
    out += QStringView(text).mid(last);
    // What a gap leaves behind: brackets with nothing in them, a space
    // before punctuation, and a comma left before the sentence's full stop.
    static const QRegularExpression empty(R"(\(\s*\)|\[\s*\]|<\s*>)");
    static const QRegularExpression spaceBefore(R"(\s+([.,;:!?…)\]]))");
    static const QRegularExpression doubled(R"([,;:]\s*([.,;:!?]))");
    out.remove(empty);
    out.replace(spaceBefore, "\\1");
    out.replace(doubled, "\\1");
    return out.simplified();
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
            // Words and sentences come from the text with its clutter
            // blanked out, so each keeps its place for the highlight.
            const QList<QPair<int, int>> skip = paragraphs.at(p).note ? QList<QPair<int, int>>() : Text::clutter(text, format == "pdf");
            QString spoken = text;
            for (const auto &range : skip)
                spoken.replace(range.first, range.second - range.first, QString(range.second - range.first, ' '));
            // Sentences end where the voice hears them end, so a footnote
            // number doesn't glue two together ("failure.1 While"). A
            // paragraph of nothing but clutter keeps one, unread.
            QList<bool> ends;
            QList<QPair<int, int>> ranges = Text::sentenceRanges(spoken, 320, &ends);
            if (ranges.isEmpty())
                ranges = Text::sentenceRanges(text, 320, &ends);
            for (int r = 0; r < ranges.size(); ++r) {
                const auto &range = ranges.at(r);
                Sentence sentence;
                sentence.ends = ends.at(r);
                sentence.chapter = c;
                sentence.paragraph = p;
                sentence.start = range.first;
                sentence.end = range.second;
                // A footnote is shown but never read aloud.
                if (!paragraphs.at(p).note)
                    sentence.words = Text::words(spoken, range.first, range.second);
                for (const auto &gap : skip)
                    if (gap.first < range.second && gap.second > range.first)
                        sentence.skipped.append({qMax(gap.first, range.first), qMin(gap.second, range.second)});
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
    QList<QPair<int, int>> skipped;
    for (const auto &gap : s.skipped)
        skipped.append({gap.first - s.start, gap.second - s.start});
    text = Text::withoutRanges(text, skipped);
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
