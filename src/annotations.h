#pragma once

#include "book.h"

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QObject>
#include <QPointer>
#include <QVariantList>

class FilePicker;

// A place or passage the reader marked in a book. A bookmark marks the
// start of a sentence; a highlight runs from one character to another
// within a chapter, possibly across paragraphs, and can carry a note.
struct Annotation {
    QString id;
    QString kind; // "bookmark" or "highlight"
    int chapter = 0;
    int paragraph = 0;
    int start = 0; // a character inside the paragraph
    // Where a highlight ends: its last paragraph, and the character after
    // its last one there. A bookmark ends where it starts.
    int endParagraph = 0;
    int end = 0;
    // The highlighted passage (paragraphs joined by a newline), or the start
    // of a bookmarked sentence. It finds the place again when the book's
    // text comes out differently, such as after an update to how a format
    // is parsed.
    QString text;
    QString note;
    QDateTime created;
    QDateTime modified;
    // Removed, but kept so the removal reaches the other computers too.
    bool deleted = false;
    // Whether the text is where the positions say, in the open book. One
    // that can't be found again is listed but not drawn.
    bool placed = true;

    bool isHighlight() const { return kind == QLatin1String("highlight"); }
};

// The bookmarks, highlights and notes kept for each book, exposed to QML
// as `annotations`. Each book's are saved in
// ~/.local/share/omareader/annotations/<fingerprint>.json, named by a hash
// of the book's file, so the same book matches wherever it is on a computer.
//
// They can be kept in step across computers through a folder the user syncs
// with a tool of their own (Syncthing, Nextcloud, a network share). omareader
// never touches the network for it: each computer writes its own file into
// the folder, omareader/<fingerprint>/<device>.json, and reads the others'.
// No two computers write the same file, so the sync tool never has a
// conflict to settle; omareader merges the files instead, keeping the
// latest change to each annotation, removals included.
class Annotations : public QObject {
    Q_OBJECT
    // Every annotation of the open book in reading order, as maps with the
    // fields of Annotation plus the chapter's title.
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    // The open chapter's annotations by paragraph: for each paragraph, a map
    // with "marks" (its parts of highlights: {id, start, end, note}) and
    // "bookmark" (whether a bookmark starts in it). Paragraphs without either
    // have an empty map.
    Q_PROPERTY(QVariantList chapterMarks READ chapterMarks NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QString syncFolder READ syncFolder WRITE setSyncFolder NOTIFY syncChanged)
    // How many other computers have written to the sync folder for this book.
    Q_PROPERTY(int syncedDevices READ syncedDevices NOTIFY syncChanged)

public:
    // An empty directory means ~/.local/share/omareader/annotations, and an
    // empty device a name made for this computer and remembered.
    explicit Annotations(const QString &directory = {}, const QString &device = {}, QObject *parent = nullptr);

    QVariantList items() const;
    QVariantList chapterMarks() const { return m_chapterMarks; }
    int count() const;
    QString syncFolder() const { return m_syncFolder; }
    void setSyncFolder(const QString &folder);
    int syncedDevices() const { return m_syncedDevices; }
    QString device() const { return m_device; }
    QString directory() const { return m_directory; }
    const QList<Annotation> &all() const { return m_items; }

    // The book the annotations belong to, and the chapter the window shows.
    void setBook(BookPtr book);
    void setChapter(int chapter);

    // Highlights a passage of the open chapter, widened to whole words. A
    // highlight it overlaps is folded into it, notes and all. Returns the
    // highlight's id, or an empty string when there is nothing to highlight.
    Q_INVOKABLE QString addHighlight(int paragraph, int start, int endParagraph, int end);
    // Bookmarks the sentence that holds a character, or removes the bookmark
    // already in its paragraph. Returns the new bookmark's id, or an empty
    // string when one was removed.
    QString toggleBookmark(int chapter, int paragraph, int offset);
    bool hasBookmark(int chapter, int paragraph) const;
    Q_INVOKABLE void setNote(const QString &id, const QString &note);
    Q_INVOKABLE void remove(const QString &id);
    // Where the word around a character of the open chapter starts, or
    // with forward where it ends, so a selection takes in whole words.
    Q_INVOKABLE int wordEdge(int paragraph, int offset, bool forward) const;
    static int wordEdge(const QString &text, int offset, bool forward);
    // The highlight over a character of the open chapter, or an empty string.
    Q_INVOKABLE QString highlightAt(int paragraph, int offset) const;
    // One annotation as items lists it, or an empty map.
    Q_INVOKABLE QVariantMap item(const QString &id) const;

    Q_INVOKABLE void chooseSyncFolder();
    Q_INVOKABLE void stopSyncing() { setSyncFolder({}); }
    void setPicker(FilePicker *picker);
    // Reads what the other computers wrote for the open book.
    Q_INVOKABLE void sync();

    // A book's identity: a hash of its file, the same on every computer.
    static QString fingerprint(const QString &path);
    // Both lists' annotations, the latest change to each.
    static QList<Annotation> merge(const QList<Annotation> &a, const QList<Annotation> &b);
    static QList<Annotation> read(const QString &file);
    // What a book's annotations are on this computer and in the sync folder,
    // without writing anything; the command line reads them this way.
    static QList<Annotation> stored(const QString &fingerprint, const QString &directory, const QString &syncFolder);
    // The sync folder the window was set to, or an empty string.
    static QString savedSyncFolder();
    // Finds each annotation's text in the book, where it was or nearest to it.
    static void place(QList<Annotation> &items, const Book &book);
    // The text of a passage of a chapter, paragraphs joined by a newline.
    static QString passage(const Book &book, int chapter, int paragraph, int start, int endParagraph, int end);
    static QVariantMap toMap(const Annotation &a, const Book *book);
    static QString defaultDirectory();

signals:
    void changed();
    void syncChanged();

private:
    QString localFile() const;
    QString syncDirectory() const;
    QString deviceFile() const;
    // Saves the open book's annotations here and in the sync folder.
    void save();
    bool write(const QString &file, const QList<Annotation> &items) const;
    // Adopts a merged list, saving it if anything changed.
    void adopt(const QList<Annotation> &items, bool save);
    void rebuild();
    void watch();
    void watchBook();
    // Pushes every book's annotations into a newly chosen sync folder.
    void syncAll();
    Annotation *find(const QString &id);

    QString m_directory;
    QString m_device;
    QString m_syncFolder;
    int m_syncedDevices = 0;
    BookPtr m_book;
    QString m_fingerprint;
    int m_chapter = 0;
    QList<Annotation> m_items;
    QVariantList m_chapterMarks;
    QFileSystemWatcher m_watcher;
    QPointer<FilePicker> m_picker;
};
