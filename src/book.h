#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <memory>

// A book as omareader reads it: chapters of plain-text paragraphs, split into
// sentences and words so the narrator can speak one sentence at a time and the
// reader can highlight the word being spoken. Formatting is dropped on purpose;
// every format reduces to the same reflowable text.

struct Paragraph {
    QString text;
    int level = 0; // 1-6 for headings, 0 for body text
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
    QList<WordSpan> words;
    bool speakable() const { return !words.isEmpty(); }
};

struct Book {
    QString path;
    QString format;
    QString title;
    QString author;
    QByteArray cover;
    QList<Chapter> chapters;

    // Filled by segment(): every sentence of the book in reading order, and
    // where each chapter's sentences start.
    QList<Sentence> sentences;
    QList<int> chapterFirstSentence;

    void segment();
    int sentenceCount() const { return int(sentences.size()); }
    QString sentenceText(int index) const;
    QStringList sentenceWords(int index) const;
    // The sentence that holds the character at offset, or the nearest one
    // after it; -1 when the chapter has none.
    int sentenceAt(int chapter, int paragraph, int offset = 0) const;
    int wordCount() const;
};

using BookPtr = std::shared_ptr<const Book>;

namespace Text {
// Sentences as character ranges, with very long ones broken at clause or
// word boundaries so each stays a comfortable size for the voice.
QList<QPair<int, int>> sentenceRanges(const QString &text, int maxLength = 320);
QList<WordSpan> words(const QString &text, int start, int end);
QString normalizeWord(const QString &word);
}
