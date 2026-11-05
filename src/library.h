#pragma once

#include "book.h"

#include <QDateTime>
#include <QObject>
#include <QVariantList>

// The books you've opened and where you stopped in each, kept in
// ~/.local/share/omareader/library.json. It only remembers paths: books stay
// where they are and are never copied or modified. Text pasted into the
// library is the exception: having no file of its own, it is saved beside
// the library in texts/.
struct LibraryEntry {
    QString path;
    QString title;
    QString author;
    QString format;
    QString cover; // a thumbnail in the cache, if the book has a cover
    int chapter = 0;
    int paragraph = 0;
    int sentence = 0;
    int sentenceCount = 0;
    QDateTime opened;
};

class Library : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList books READ books NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)

public:
    explicit Library(const QString &file = {}, const QString &coverDirectory = {}, QObject *parent = nullptr);

    QVariantList books() const;
    int count() const { return int(m_entries.size()); }
    LibraryEntry entry(const QString &path) const;
    bool contains(const QString &path) const;

    // Records an opened book (metadata and cover), keeping its saved position.
    void opened(const Book &book);
    void savePosition(const QString &path, int chapter, int paragraph, int sentence, int sentenceCount);
    // Saves pasted text as a library item and returns its path, or an empty
    // string when there is no text to save or it can't be written.
    QString addText(const QString &title, const QString &text);
    QString textDirectory() const { return m_textDirectory; }
    bool isText(const QString &path) const;
    // Forgets a book. A book's file stays; a pasted text goes to the trash.
    Q_INVOKABLE void remove(const QString &path);
    bool save() const;

signals:
    void changed();

private:
    void load();
    QString storeCover(const Book &book) const;
    int indexOf(const QString &path) const;
    void sort();

    QString m_file;
    QString m_coverDirectory;
    QString m_textDirectory;
    QList<LibraryEntry> m_entries;
};
