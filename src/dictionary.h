#pragma once

#include <QByteArray>
#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <vector>

// Offline dictionaries for looking up a word on the page: StarDict
// (.ifo, .idx, .dict, as sdcv, GoldenDict and KOReader use) and dictd
// (.index, .dict, as the dict-* packages install). omareader reads their
// files itself, so no dictionary server runs and nothing touches the
// network. A bilingual dictionary, such as FreeDict's English-Portuguese,
// is what translates a word.

// One article of a dictionary, as plain text: markup is reduced to lines,
// so nothing in a dictionary can load an image or follow a link.
struct DictionaryEntry {
    QString dictionary; // the dictionary's name
    QString headword;   // the word as the dictionary spells it
    QString text;
};

// Reads a dictionary's data file, plain or dictzip (gzip cut into chunks
// that each decompress alone, so an article is found without inflating the
// whole file). A gzip file without dictzip's chunk table is inflated whole.
class DictionaryData {
public:
    bool open(const QString &path);
    QByteArray read(quint64 offset, quint32 size);

private:
    QString m_path;
    QByteArray m_whole;
    bool m_dictzip = false;
    bool m_inflated = false;
    quint32 m_chunkLength = 0;
    QList<quint64> m_chunkStarts; // where each chunk's compressed bytes begin, and one past the last
};

class Dictionary {
public:
    enum Format { StarDict, Dictd };

    // From a StarDict .ifo or a dictd .index; null when it isn't a
    // dictionary omareader can read. Only the name is read here: the index
    // loads on the first lookup.
    static std::shared_ptr<Dictionary> open(const QString &path);

    QString name() const { return m_name; }
    QString path() const { return m_path; }
    Format format() const { return m_format; }
    // How many headwords the dictionary has, as its files say.
    int wordCount() const;

    // The articles for the word, ignoring case. Safe to call from any thread.
    QList<DictionaryEntry> lookup(const QString &word, int limit = 4);

    // Markup as plain text: HTML lists become numbered lines, other tags
    // go, entities are decoded. HTML's spacing is collapsed when html is set.
    static QString plainText(const QString &markup, bool html);
    // The word, then the base forms worth trying when the dictionary has no
    // article for it as written ("Running" also tries "run", "houses"
    // "house", "animais" "animal").
    static QStringList forms(const QString &word);
    // The key the index is searched by: case-folded, composed UTF-8.
    static quint64 keyHash(QByteArrayView utf8);

private:
    struct Key {
        quint64 hash;
        quint32 position; // where the entry starts in m_index
        bool operator<(const Key &o) const { return hash < o.hash; }
    };

    bool loadIndex();
    bool loadStarDict();
    bool loadDictd();
    QString articleText(const QByteArray &data, const QString &headword) const;

    QString m_path;
    QString m_name;
    Format m_format = StarDict;
    QString m_types; // StarDict's sametypesequence
    bool m_offsets64 = false;
    int m_declaredWords = 0;
    QMutex m_mutex;
    bool m_loaded = false;
    bool m_broken = false;
    QByteArray m_index;
    std::vector<Key> m_keys;
    DictionaryData m_data;
};

// The installed dictionaries: found under a list of folders, opened once
// and kept, so a dictionary added while the app runs turns up on the next
// lookup. Safe to use from any thread.
class DictionaryShelf {
public:
    explicit DictionaryShelf(const QStringList &directories);

    // omareader's own folder, where setup puts dictionaries, then where
    // StarDict, sdcv and the dictd packages keep theirs.
    static QStringList defaultDirectories();
    static QString installDirectory();

    QStringList directories() const { return m_directories; }
    QList<std::shared_ptr<Dictionary>> dictionaries();

    struct Result {
        QString word;
        int dictionaries = 0; // how many were consulted
        QList<DictionaryEntry> entries;
    };
    // Each dictionary answers with the first form of the word it has.
    Result lookup(const QString &word);

private:
    QStringList m_directories;
    QMutex m_mutex;
    QHash<QString, std::shared_ptr<Dictionary>> m_open;
};

// FreeDict's dictionaries, which omareader setup --dictionary downloads,
// pinned to a release and its SHA-512 (src/dictionaries.tsv).
struct CatalogDictionary {
    QString name; // FreeDict's language pair, such as eng-por
    QString version;
    int headwords = 0;
    qint64 bytes = 0;
    QString url;
    QString sha512;
    QString from() const { return name.section('-', 0, 0); }
    QString to() const { return name.section('-', 1, 1); }
    // "English → Portuguese"
    QString title() const;
};

namespace DictionaryCatalog {
QList<CatalogDictionary> all();
// eng-por, or en-pt and the like with two-letter codes; one with an empty
// name when FreeDict has no such dictionary.
CatalogDictionary find(const QString &name);
// A language's three-letter code from a two- or three-letter one or an
// Omarchy language such as pt-BR; empty when unknown.
QString languageCode(const QString &language);
QString languageName(const QString &code);
}

// Looking up a word from the reader. Exposed to QML as `lookup`.
class WordLookup : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString word READ word NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
    // How many dictionaries the last lookup consulted.
    Q_PROPERTY(int dictionaryCount READ dictionaryCount NOTIFY changed)

public:
    explicit WordLookup(const QStringList &directories = DictionaryShelf::defaultDirectories(),
                        QObject *parent = nullptr);

    QString word() const { return m_word; }
    bool busy() const { return m_busy; }
    QVariantList entries() const { return m_entries; }
    int dictionaryCount() const { return m_dictionaryCount; }

    // Looks the word up on a worker thread; entries follow when done.
    Q_INVOKABLE void lookUp(const QString &word);
    Q_INVOKABLE void clear();
    // The word in text at or just before offset, as {start, length, text};
    // empty when there is none there.
    Q_INVOKABLE static QVariantMap wordAt(const QString &text, int offset);
    // The command that installs a dictionary from English into the
    // window's language, or that lists them all.
    Q_INVOKABLE static QString setupCommand(const QString &language);

signals:
    void changed();

private:
    std::shared_ptr<DictionaryShelf> m_shelf;
    QFutureWatcher<DictionaryShelf::Result> m_watcher;
    QString m_word;
    bool m_busy = false;
    QVariantList m_entries;
    int m_dictionaryCount = 0;
};
