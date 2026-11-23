#pragma once

#include <QString>

struct Book;

// What language a book is written in, worked out offline: from what the book
// says about itself first, and otherwise from its words. Languages are kept
// as ISO 639-1 codes ("en", "pt"), regions dropped, since a voice that reads
// Portuguese reads it from Lisbon as well as from São Paulo.
namespace Language {

// A language as books, settings and voices name it ("en-US", "por",
// "Português", "pt-br", "cmn", "English") as its code; empty when it names
// no single language omareader knows ("und", "mul", "Auto").
QString fromTag(const QString &tag);

// The language's English name ("Portuguese"), for the window to translate;
// empty for a code it doesn't know.
QString name(const QString &code);

struct Guess {
    QString code; // empty when the text doesn't say clearly enough
    // How sure the guess is, from 0 to 1.
    double confidence = 0;
};

// Guesses from the text alone: the writing system first (Han, kana, Hangul,
// Cyrillic, Devanagari...), and for text in Latin letters, how often each
// language's commonest little words ("the", "de", "und") turn up. A script
// several languages share names one only on letters that are that
// language's own (Russian's ы and э), and none otherwise.
Guess guess(const QString &text);

// A sample of the book's words for guess(), spread across its chapters so a
// foreword in another language doesn't decide; notes are left out.
QString sample(const Book &book, int maxWords = 4000);

// Settles book.language and book.languageSource: the metadata's language
// when it gives one, and otherwise the text's guess.
void identify(Book &book);

}
