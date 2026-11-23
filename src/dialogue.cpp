#include "dialogue.h"

#include <QString>

namespace Dialogue {

namespace {

const QChar LeftDouble(0x201C);   // “
const QChar RightDouble(0x201D);  // ”
const QChar LowDouble(0x201E);    // „
const QChar LeftSingle(0x2018);   // ‘
const QChar RightSingle(0x2019);  // ’
const QChar LeftGuillemet(0x00AB);  // «
const QChar RightGuillemet(0x00BB); // »

bool isDash(QChar c) { return c == QChar(0x2014) || c == QChar(0x2015) || c == QChar(0x2013); }

// Which quotation marks a chapter writes its speech with. Books that quote
// speech in double marks keep single ones for quotes inside it, and the
// other way round, so only the kind that opens more quotations is followed.
enum class Marks { None, Double, Single };

// A quotation mark can open one where a word starts: after a space, an
// opening bracket, a dash or another quotation mark, with a word after it.
bool atWordStart(const QString &t, int i) {
    if (i + 1 >= t.size() || t.at(i + 1).isSpace())
        return false;
    if (i == 0)
        return true;
    const QChar before = t.at(i - 1);
    return before.isSpace() || isDash(before) || before == '(' || before == '[' || before == '-'
           || before == LeftDouble || before == LowDouble || before == LeftSingle || before == LeftGuillemet
           || before == '"';
}

// A straight mark or a right single quote closes one only where a word ends,
// since inside a word it is an apostrophe ("don’t").
bool atWordEnd(const QString &t, int i) {
    if (i == 0 || t.at(i - 1).isSpace())
        return false;
    return i + 1 >= t.size() || !t.at(i + 1).isLetterOrNumber();
}

// The mark that closes a quotation opened with open, or a null QChar when
// open can't open one at i in a chapter quoting with these marks.
QChar closerFor(const QString &t, int i, Marks marks) {
    const QChar c = t.at(i);
    if (marks == Marks::Double) {
        if (c == LeftDouble || c == LowDouble || c == LeftGuillemet)
            return c == LeftGuillemet ? RightGuillemet : RightDouble;
        // »Danish and German« and ”Swedish” quotes open with what closes
        // elsewhere, so only where a word starts.
        if (c == RightGuillemet && atWordStart(t, i))
            return LeftGuillemet;
        if ((c == RightDouble || c == '"') && atWordStart(t, i))
            return c;
    } else if (marks == Marks::Single) {
        if ((c == LeftSingle || c == '\'') && atWordStart(t, i))
            return c == LeftSingle ? RightSingle : QChar('\'');
    }
    return {};
}

// Whether the mark at i closes the quotation opened with open.
bool closes(const QString &t, int i, QChar open, QChar closer) {
    const QChar c = t.at(i);
    if (closer == RightDouble) {
        // „German“ quotes close with what opens English ones.
        if (open == LowDouble && c == LeftDouble)
            return true;
        if (c == RightDouble)
            return open != RightDouble || atWordEnd(t, i);
        // Some books close “quotes'' with two apostrophes, as TeX writes them.
        if (c == '\'' && i + 1 < t.size() && t.at(i + 1) == '\'' && i > 0 && !t.at(i - 1).isSpace())
            return true;
        return c == '"' && atWordEnd(t, i);
    }
    if (closer == '"')
        return (c == '"' || c == RightDouble) && atWordEnd(t, i);
    if (closer == RightSingle || closer == '\'')
        return (c == RightSingle || c == '\'') && atWordEnd(t, i);
    return c == closer;
}

// Whether an apostrophe that could close a single quotation at i is more
// likely a plural possessive ("the dogs’ bowls"), because a closing mark
// after punctuation, as speech ends, comes later before any new quotation.
bool closesLater(const QString &t, int i, QChar open, QChar closer, Marks marks) {
    if (closer != RightSingle && closer != '\'')
        return false;
    if (i == 0 || !t.at(i - 1).isLetterOrNumber())
        return false;
    for (int j = i + 1; j < t.size(); ++j) {
        if (!closerFor(t, j, marks).isNull())
            return false;
        if (closes(t, j, open, closer) && !t.at(j - 1).isLetterOrNumber())
            return true;
    }
    return false;
}

// Straight marks and single quotes are also apostrophes, inches and feet,
// so a quotation they open is only believed once it closes.
bool ambiguous(QChar open) { return open == '"' || open == '\'' || open == LeftSingle || open == RightDouble; }

int firstVisible(const QString &t) {
    int i = 0;
    while (i < t.size() && t.at(i).isSpace())
        ++i;
    return i;
}

Marks marksOf(const QList<Paragraph> &paragraphs) {
    int doubles = 0, singles = 0;
    for (const Paragraph &p : paragraphs) {
        if (p.note || p.level > 0)
            continue;
        const QString &t = p.text;
        for (int i = 0; i < t.size(); ++i) {
            if (!closerFor(t, i, Marks::Double).isNull())
                ++doubles;
            else if (!closerFor(t, i, Marks::Single).isNull())
                ++singles;
        }
    }
    if (doubles == 0 && singles == 0)
        return Marks::None;
    return doubles >= singles ? Marks::Double : Marks::Single;
}

// Whether a quotation left open at the end of a paragraph goes on in this
// one: it opens with the same mark again, as a quotation running over
// several paragraphs does, or it closes the quotation before opening any
// other, as where a page break split a paragraph in two.
bool continues(const QString &t, QChar open, QChar closer, Marks marks) {
    const int first = firstVisible(t);
    if (first < t.size() && t.at(first) == open)
        return true;
    if (first < t.size() && isDash(t.at(first)))
        return false;
    for (int i = first; i < t.size(); ++i) {
        if (closes(t, i, open, closer))
            return true;
        if (!closerFor(t, i, marks).isNull())
            return false;
    }
    return false;
}

// Speech set off by dashes: the paragraph opens with one, and every later
// dash that isn't inside a word ("well—known", "1990–2000") hands over
// between speaker and narrator.
QList<Range> dashed(const QString &t, int first) {
    QList<Range> ranges;
    bool speaking = true;
    int from = first;
    for (int i = first + 1; i < t.size(); ++i) {
        if (!isDash(t.at(i)))
            continue;
        const bool inWord = i > 0 && i + 1 < t.size() && t.at(i - 1).isLetterOrNumber() && t.at(i + 1).isLetterOrNumber();
        if (inWord)
            continue;
        if (speaking && i > from)
            ranges.append({from, i});
        from = i;
        speaking = !speaking;
    }
    if (speaking && t.size() > from)
        ranges.append({from, int(t.size())});
    return ranges;
}

}

QList<QList<Range>> find(const QList<Paragraph> &paragraphs) {
    QList<QList<Range>> found(paragraphs.size());
    const Marks marks = marksOf(paragraphs);
    bool open = false;
    QChar opener, closer;

    // The next paragraph a quotation could go on in, or -1.
    auto nextBody = [&](int p) {
        for (int n = p + 1; n < paragraphs.size(); ++n) {
            if (paragraphs.at(n).note)
                continue;
            return paragraphs.at(n).level > 0 ? -1 : n;
        }
        return -1;
    };

    for (int p = 0; p < paragraphs.size(); ++p) {
        const Paragraph &paragraph = paragraphs.at(p);
        if (paragraph.note)
            continue;
        if (paragraph.level > 0) {
            open = false;
            continue;
        }
        const QString &t = paragraph.text;
        const int first = firstVisible(t);
        if (!open && first < t.size() && isDash(t.at(first))) {
            found[p] = dashed(t, first);
            continue;
        }
        QList<Range> &ranges = found[p];
        int from = 0;
        int i = first;
        if (open) {
            from = first;
            // The reopening mark belongs to the quotation, not a new one.
            if (first < t.size() && t.at(first) == opener)
                i = first + 1;
        }
        for (; i < t.size(); ++i) {
            if (open) {
                if (closes(t, i, opener, closer) && !closesLater(t, i, opener, closer, marks)) {
                    // Both apostrophes of a '' close.
                    if (t.at(i) == '\'' && closer != '\'')
                        ++i;
                    ranges.append({from, i + 1});
                    open = false;
                }
                continue;
            }
            const QChar close = closerFor(t, i, marks);
            if (!close.isNull()) {
                open = true;
                opener = t.at(i);
                closer = close;
                from = i;
            }
        }
        if (!open)
            continue;
        const int next = nextBody(p);
        if (next >= 0 && continues(paragraphs.at(next).text, opener, closer, marks)) {
            ranges.append({from, int(t.size())});
            continue;
        }
        // Never closed: a clear quotation mark still opened speech, which
        // runs to the paragraph's end; an apostrophe or an inch did not.
        if (!ambiguous(opener))
            ranges.append({from, int(t.size())});
        open = false;
    }
    return found;
}

QList<Part> parts(const Sentence &sentence, const QList<Range> &dialogue) {
    QList<Part> raw;
    int at = sentence.start;
    for (const Range &range : dialogue) {
        if (range.second <= sentence.start)
            continue;
        if (range.first >= sentence.end)
            break;
        const int from = qMax(range.first, sentence.start);
        const int to = qMin(range.second, sentence.end);
        if (from > at)
            raw.append({at, from, false});
        raw.append({from, to, true});
        at = to;
    }
    if (at < sentence.end)
        raw.append({at, sentence.end, false});

    // A stretch with no word of its own (a closing mark, a full stop after
    // the quotation) goes with the part before it, or else the one after.
    QList<Part> out;
    int carried = -1;
    for (Part part : std::as_const(raw)) {
        const bool hasWord = std::any_of(sentence.words.begin(), sentence.words.end(), [&](const WordSpan &w) {
            return w.start >= part.start && w.start < part.end;
        });
        if (!hasWord) {
            if (!out.isEmpty())
                out.last().end = part.end;
            else if (carried < 0)
                carried = part.start;
            continue;
        }
        if (carried >= 0) {
            part.start = carried;
            carried = -1;
        }
        if (!out.isEmpty() && out.last().dialogue == part.dialogue)
            out.last().end = part.end;
        else
            out.append(part);
    }
    if (out.isEmpty())
        return {{sentence.start, sentence.end, false}};
    out.last().end = sentence.end;
    return out;
}

Sentence slice(const Sentence &sentence, int start, int end) {
    Sentence out = sentence;
    out.start = start;
    out.end = end;
    out.words.clear();
    for (const WordSpan &w : sentence.words)
        if (w.start >= start && w.start < end)
            out.words.append(w);
    out.skipped.clear();
    for (const auto &gap : sentence.skipped)
        if (gap.first < end && gap.second > start)
            out.skipped.append({qMax(gap.first, start), qMin(gap.second, end)});
    return out;
}

}
