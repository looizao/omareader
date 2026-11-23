#pragma once

#include "book.h"
#include "speech.h"

#include <QList>
#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <functional>
#include <memory>

// How the reader wants names, places and jargon said: each entry is a word
// or phrase as the book writes it and a respelling of how it sounds
// ("Hermione" said as "Her-my-oh-nee"). The respelling replaces the written
// form in what the voice is asked to say, in narration and exports alike,
// while the page keeps the book's own words and the highlight still lands
// on them. Respellings rather than phonemes, so they work with every engine
// and every voice.
struct Respelling {
    QString written;
    QString spoken;
};

class Lexicon {
public:
    struct Match {
        int start = 0; // [start, end) in the text searched
        int end = 0;
        QString spoken;
    };

    // ~/.local/share/omareader/omareader/pronunciations.json
    static QString defaultFile();
    // An empty lexicon when the file is missing or unreadable.
    static Lexicon load(const QString &file = {});
    // The same, but false (with why) when the file is there and can't be
    // read or parsed, as opposed to not there yet.
    static bool read(const QString &file, Lexicon *lexicon, QString *error = nullptr);
    bool save(const QString &file = {}) const;
    // Reads the file, applies change and saves the result, all under a lock
    // so changes from the window and the command line never lose one
    // another. Fails without saving when the file can't be read, when the
    // lock stays taken, or when change returns false.
    static bool update(const QString &file, const std::function<bool(Lexicon &)> &change, Lexicon *result = nullptr,
                       QString *error = nullptr);

    // Entries match whole words, ignoring case, except an entry written in
    // capitals ("US", "SQL"), which matches only in capitals so it leaves
    // the ordinary word alone. Two entries are the same when they would
    // match the same text.
    static QString key(const QString &written);
    // Whether a written form and a respelling can make an entry: each needs
    // a letter or digit, and the written form at most a few words.
    static bool valid(const QString &written, const QString &spoken);

    const QList<Respelling> &entries() const { return m_entries; }
    bool isEmpty() const { return m_entries.isEmpty(); }
    int indexOf(const QString &written) const;
    // Adds the entry or replaces the one with the same key.
    bool set(const QString &written, const QString &spoken);
    bool remove(const QString &written);

    // Every place in text an entry matches, in order and not overlapping;
    // at any one place the longest entry wins.
    QList<Match> find(const QString &text) const;

private:
    // A compiled share of the entries: capture group k matches
    // m_entries[entries[k - 1]].
    struct Chunk {
        QRegularExpression pattern;
        QList<int> entries;
    };
    void compile() const;

    QList<Respelling> m_entries;
    // Built on first use and shared by copies until either changes.
    mutable std::shared_ptr<const QList<Chunk>> m_chunks;
};

// A sentence as the voice is asked to say it.
struct Utterance {
    QString text;      // what the engine speaks
    QStringList words; // the words in it, in order
    // For each word on the page, the first and last of words it is heard
    // as. Page words a respelled phrase covers share the same range.
    QList<QPair<int, int>> heard;
};

namespace Pronounce {
// The sentence's spoken text (Book::sentenceText) with the lexicon's
// respellings in place of the written forms.
Utterance utterance(const Book &book, int sentence, const Lexicon &lexicon);
// One timing per word on the page, from the engine's timings of the words
// it said: a respelled word is lit for as long as its respelling is heard,
// and a respelled phrase shares that time among its words by length.
QList<WordTiming> timings(const Utterance &utterance, const QStringList &pageWords,
                          const QList<SpokenWord> &spoken, double duration);
}

// The lexicon as the window edits it. Every change is saved at once, and the
// file is read again first, so an entry added from the command line while
// the window is open is kept.
class Pronunciations : public QObject {
    Q_OBJECT
    // [{written, spoken}], sorted by the written form.
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)

public:
    explicit Pronunciations(const QString &file = {}, QObject *parent = nullptr);

    QVariantList entries() const;
    int count() const { return int(m_lexicon.entries().size()); }
    const Lexicon &lexicon() const { return m_lexicon; }

    // The respelling saved for this written form, or an empty string.
    Q_INVOKABLE QString spokenFor(const QString &written) const;
    Q_INVOKABLE bool valid(const QString &written, const QString &spoken) const {
        return Lexicon::valid(written, spoken);
    }
    // Saves an entry; with original, the entry being edited, which is
    // replaced even if its written form changed.
    Q_INVOKABLE bool set(const QString &written, const QString &spoken, const QString &original = {});
    Q_INVOKABLE bool remove(const QString &written);
    Q_INVOKABLE void reload();

signals:
    void changed();

private:
    QString m_file;
    Lexicon m_lexicon;
};
