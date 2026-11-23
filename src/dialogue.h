#pragma once

#include "book.h"

#include <QList>
#include <QPair>

// Where a book's characters speak, so the narration can read it in a voice
// of its own. Everything is worked out from the punctuation alone, offline:
// quotation marks (curly, straight, guillemets, German low-high quotes and
// single quotes in books that use them for speech) and the dialogue dash
// of Portuguese, Spanish, French, Russian and other traditions, where a
// paragraph that opens with a dash is speech and each later dash switches
// between speech and the narrator's words ("— Olá — disse ele. — Tudo
// bem?"). A quotation may run over several paragraphs, which by convention
// reopen the quote and close it only at the end.
namespace Dialogue {

using Range = QPair<int, int>;

// For each paragraph of a chapter, the [start, end) character ranges that
// are spoken by a character, quotation marks and dashes included. Headings
// end any open quotation and footnotes are passed over. A straight or single
// quotation mark, which may just as well be an apostrophe or an inch, only
// counts when its quotation closes, in the paragraph or one that reopens it.
QList<QList<Range>> find(const QList<Paragraph> &paragraphs);

// A sentence cut where speech starts and stops: [start, end) inside its
// paragraph, each with at least one of the sentence's words, together
// covering the whole sentence. One part when the sentence is all narration
// or all speech.
struct Part {
    int start = 0;
    int end = 0;
    bool dialogue = false;
    bool operator==(const Part &o) const { return start == o.start && end == o.end && dialogue == o.dialogue; }
};
QList<Part> parts(const Sentence &sentence, const QList<Range> &dialogue);

// The sentence as narrowed to [start, end) of its paragraph: the words and
// skipped ranges inside it.
Sentence slice(const Sentence &sentence, int start, int end);

}
