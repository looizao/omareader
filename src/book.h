#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <memory>

// A book as omareader reads it: chapters of plain-text paragraphs, split into
// sentences and words so the narrator can speak one sentence at a time and the
// reader can highlight the word being spoken. Every format reduces to the same
// reflowable text; the little inline formatting Markdown carries (bold,
// italic, code) rides along as spans over that text, so the words the voice
// reads and the characters the reader highlights stay the same.

// Inline formatting over a range of a paragraph's text.
struct TextStyle {
    enum Flag { Bold = 1, Italic = 2, Code = 4, Strike = 8 };
    int start = 0;
    int length = 0;
    int flags = 0;
    bool operator==(const TextStyle &o) const { return start == o.start && length == o.length && flags == o.flags; }
};

struct Paragraph {
    QString text;
    int level = 0; // 1-6 for headings, 0 for body text
    QList<TextStyle> styles;
    // A footnote found at the foot of a PDF's page: shown, but not read aloud.
    bool note = false;
};

struct Chapter {
    QString title;
    QList<Paragraph> paragraphs;
};

// A word's position inside its paragraph.
struct WordSpan {
    int start = 0;
    int length = 0;
};

struct Sentence {
    int chapter = 0;
    int paragraph = 0;
    int start = 0; // character range inside the paragraph
    int end = 0;
    // The words the voice reads, which leaves out the clutter in skipped.
    QList<WordSpan> words;
    // Character ranges inside the paragraph, within this sentence, that are
    // shown but not read aloud: citation markers, links and the like.
    QList<QPair<int, int>> skipped;
    // False for a piece of a sentence too long for the voice, broken at a
    // comma or a space, which is no place to pause.
    bool ends = true;
    bool speakable() const { return !words.isEmpty(); }
};

struct Book {
    QString path;
    QString format;
    QString title;
    QString author;
    QString source; // the web address a page added from the web came from
    // The language it is written in, as an ISO 639-1 code ("pt"), and where
    // that came from: "metadata" or "text"; both empty when unknown. The
    // loaders put the metadata's in; Language::identify() settles it.
    QString language;
    QString languageSource;
    // A hash of the book's file, which its bookmarks and notes are kept
    // under; filled in when the book is opened, see Annotations.
    QString fingerprint;
    // Whether the text was recognised from pictures of the pages (OCR), as
    // a scanned PDF's is, rather than read from the file.
    bool recognised = false;
    QByteArray cover;
    QList<Chapter> chapters;

    // Filled by segment(): every sentence of the book in reading order, and
    // where each chapter's sentences start.
    QList<Sentence> sentences;
    QList<int> chapterFirstSentence;

    void segment();
    int sentenceCount() const { return int(sentences.size()); }
    // The sentence as the voice reads it, without its skipped clutter.
    QString sentenceText(int index) const;
    QStringList sentenceWords(int index) const;
    // The same for any stretch of a paragraph cut like a sentence, such as
    // the part of one a character speaks.
    QString spokenText(const Sentence &sentence) const;
    QStringList wordsOf(const Sentence &sentence) const;
    // The sentence that holds the character at offset, or the nearest one
    // after it; -1 when the chapter has none.
    int sentenceAt(int chapter, int paragraph, int offset = 0) const;
    int wordCount() const;
};

using BookPtr = std::shared_ptr<const Book>;

namespace Text {
// Sentences as character ranges, with very long ones broken at clause or
// word boundaries so each stays a comfortable size for the voice.
// ends, when given, gets for each range whether a sentence ends there, false
// for every piece of a long one but its last.
QList<QPair<int, int>> sentenceRanges(const QString &text, int maxLength = 320, QList<bool> *ends = nullptr);
QList<WordSpan> words(const QString &text, int start, int end);
// The parts of a paragraph that sound bad spoken and say nothing a listener
// misses: citation markers ("[12]", "[Bro96]"), web addresses, bracketed
// editorial asides ("[sic]") and parenthesised cross-references ("(see
// Figure 2-3)", "(p. 12)"), and with footnoteNumbers the bare footnote
// numbers a PDF's text keeps ("failure.1 While"), which other formats mark
// up and drop. Sorted [start, end) ranges that don't overlap. Code-like
// brackets ("x[2]", "range [0, 1]") and numbers that belong to words
// ("GPT4", "server:443") stay.
QList<QPair<int, int>> clutter(const QString &text, bool footnoteNumbers = false);
// The text with ranges taken out, and the spaces and punctuation left
// around each gap tidied ("see https://x.org." becomes "see.").
QString withoutRanges(const QString &text, const QList<QPair<int, int>> &ranges);
QString normalizeWord(const QString &word);
// The text as a search compares it: lower case, accents dropped, curly
// quotes, dashes and odd spaces made plain. offsets gets, for each character
// of the folded text, where the character it came from starts in text.
QString fold(const QString &text, QList<int> *offsets = nullptr);
// Where needle, already folded, is in text, as [start, end) ranges of text
// that don't overlap; folded and offsets are text as fold() gives them.
QList<QPair<int, int>> find(const QString &text, const QString &folded, const QList<int> &offsets,
                            const QString &needle);
}
