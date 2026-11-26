#pragma once

#include "book.h"

#include <QObject>
#include <QTimer>
#include <QVariantList>

class Narrator;
class Reader;
class ReadingStats;

// RSVP shows a book one flash at a time, always in the same place: a word,
// or a short group of words, with its recognition point pinned to one spot
// so the eye never has to move. The words are the ones the voice reads,
// each with the punctuation written against it, so citation markers, links
// and footnotes are left out exactly as narration leaves them out.

// One flash: a run of a sentence's words, as [start, end) of its paragraph.
struct Flash {
    int sentence = 0;
    int firstWord = 0; // index into the sentence's words
    int wordCount = 0;
    int start = 0;
    int end = 0;
    // How long it stays up, in units of an ordinary short word: longer for
    // long words, and for the punctuation or the break that follows it.
    double weight = 1;
};

namespace Flashes {

// Every flash of the book in reading order, of up to size words each. A
// flash never runs across the end of a sentence, a paragraph, or a switch
// between the narrator and a character speaking, and punctuation after a
// word ends its flash early.
QList<Flash> of(const Book &book, int size);

// The flash as shown: its words with the punctuation written against them,
// and none of the clutter the voice skips, separated by single spaces.
QString text(const Book &book, const Flash &flash);

// The character of the text pinned in place, counted in letters and digits
// from the first one, as if the text were one long word: the 1st of 1, the
// 2nd of 2 to 5, the 3rd of 6 to 9, the 4th of 10 to 13 and the 5th of
// more. -1 for a text without a letter.
int recognitionPoint(const QString &text);

// How many milliseconds one unit of weight lasts at this many words a
// minute, so that the book as a whole, pauses included, reads at that rate.
double unitMs(const QList<Flash> &flashes, int wpm);

// How long the nth flash since the clock started stays up: the first few
// run slower, from half the rate up to the full one, so they aren't missed.
int durationMs(const Flash &flash, double unitMs, int sinceStart);

// The flashes after a start that are slowed.
constexpr int RampLength = 5;

}

// RSVP in the window, exposed to QML as `rsvp`. It keeps the flash being
// shown, and paces it with one of two clocks: its own, at a rate of words
// a minute, or the voice's, following the word being heard. The reading
// position moves with the flash, so leaving RSVP finds the page there.
class Rsvp : public QObject {
    Q_OBJECT
    // Whether the page is replaced by the flash.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    // Whether the silent clock is running.
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    // Whether the voice reads aloud and paces the flashes; the silent clock
    // paces them otherwise.
    Q_PROPERTY(bool aloud READ aloud WRITE setAloud NOTIFY settingsChanged)
    Q_PROPERTY(int wpm READ wpm WRITE setWpm NOTIFY settingsChanged)
    // Words in a flash, at most.
    Q_PROPERTY(int size READ size WRITE setSize NOTIFY settingsChanged)
    // The flash's text size in pixels, apart from the page's.
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY settingsChanged)
    // The flash shown, cut around its recognition point.
    Q_PROPERTY(int index READ index NOTIFY flashChanged)
    Q_PROPERTY(QString text READ text NOTIFY flashChanged)
    Q_PROPERTY(QString before READ before NOTIFY flashChanged)
    Q_PROPERTY(QString pivot READ pivot NOTIFY flashChanged)
    Q_PROPERTY(QString after READ after NOTIFY flashChanged)
    // Where the flash is on the page, and the word to look up or teach
    // how to say: the one being heard, or else the flash's first.
    Q_PROPERTY(int paragraph READ paragraph NOTIFY flashChanged)
    Q_PROPERTY(int start READ start NOTIFY flashChanged)
    Q_PROPERTY(QString word READ word NOTIFY flashChanged)
    // The flashes of the sentence shown, as {index, text}, to show while
    // paused and to move to with a click.
    Q_PROPERTY(QVariantList context READ context NOTIFY sentenceChanged)
    // How far into the chapter the flash is, from 0 to 1, and the minutes
    // the rest of it takes at the pace of the clock in use.
    Q_PROPERTY(double chapterProgress READ chapterProgress NOTIFY flashChanged)
    Q_PROPERTY(int minutesLeftInChapter READ minutesLeftInChapter NOTIFY flashChanged)

public:
    static constexpr int MinWpm = 100;
    static constexpr int MaxWpm = 1000;
    static constexpr int WpmStep = 25;
    static constexpr int DefaultWpm = 300;
    static constexpr int MaxSize = 3;
    static constexpr int MinFontSize = 24;
    static constexpr int MaxFontSize = 144;

    Rsvp(Reader *reader, Narrator *narrator, ReadingStats *stats, QObject *parent = nullptr);

    bool active() const { return m_active; }
    bool running() const { return m_timer.isActive(); }
    bool aloud() const { return m_aloud; }
    int wpm() const { return m_wpm; }
    int size() const { return m_size; }
    int fontSize() const { return m_fontSize; }
    int index() const { return m_index; }
    QString text() const;
    QString before() const;
    QString pivot() const;
    QString after() const;
    int paragraph() const;
    int start() const;
    QString word() const;
    QVariantList context() const;
    double chapterProgress() const;
    int minutesLeftInChapter() const;

    void setActive(bool active);
    void setAloud(bool aloud);
    void setWpm(int wpm);
    void setSize(int size);
    void setFontSize(int size);

    // Space: starts or pauses the clock in use.
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    // Moves by flashes, pausing first.
    Q_INVOKABLE void step(int flashes);
    // Moves by sentences, keeping the silent clock going if it was.
    Q_INVOKABLE void skip(int sentences);
    // Shows a flash, as a click on one of the context's does.
    Q_INVOKABLE void moveTo(int index);

    const QList<Flash> &flashes() const { return m_flashes; }

signals:
    void activeChanged();
    void runningChanged();
    void settingsChanged();
    void flashChanged();
    void sentenceChanged();

private:
    // Works the flashes out for the open book, if not done yet.
    void prepare();
    void forget();
    // The flash holding a word of a sentence, the nearest one after it when
    // none does; -1 when there are none.
    int flashAt(int sentence, int word = 0) const;
    // Shows a flash and moves the reading position to its sentence.
    void show(int index);
    void advance();
    void startClock();
    void stopClock();
    // The reading position moved under RSVP: a chapter, a bookmark, a
    // search match, or the voice going on to its next sentence.
    void followReader();
    void followVoice();
    bool voicePlaying() const;
    // The flashes [first, end) of the chapter of a flash.
    std::pair<int, int> chapterFlashes(int index) const;

    Reader *m_reader;
    Narrator *m_narrator;
    ReadingStats *m_stats;
    BookPtr m_book;
    QList<Flash> m_flashes;
    // Each sentence's first flash, or the next sentence's for one the voice
    // doesn't read, with the flash count after the last sentence, so a
    // word's flash is found without a search through the book.
    QList<int> m_sentenceFirst;
    // How many words the flashes before each one hold, and in all at the
    // end, so the time left is a subtraction.
    QList<int> m_wordsBefore;
    double m_unitMs = 0;
    int m_index = -1;
    // The flash shown, and its recognition point in it.
    QString m_text;
    int m_point = -1;
    int m_sinceStart = 0;
    bool m_active = false;
    bool m_aloud = false;
    int m_wpm = DefaultWpm;
    int m_size = 1;
    int m_fontSize = 50;
    QTimer m_timer;
};
