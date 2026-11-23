#pragma once

#include "book.h"

#include <QDateTime>
#include <QObject>
#include <QStringList>
#include <QVariantList>

// The books you've opened and where you stopped in each, kept in
// ~/.local/share/omareader/library.json. It only remembers paths: books stay
// where they are and are never copied or modified. Text pasted into the
// library is the exception: having no file of its own, it is saved beside
// the library in texts/, as is the text of a web page added from its
// address.
struct LibraryEntry {
    QString path;
    QString title;
    QString author;
    QString format;
    QString cover; // a thumbnail in the cache, if the book has a cover
    QString source; // a web page's address
    int chapter = 0;
    int paragraph = 0;
    int sentence = 0;
    int sentenceCount = 0;
    // The start of the sentence reached in its paragraph, which finds the
    // place again even when the sentences are split differently; -1 for
    // positions saved before it was kept.
    int offset = -1;
    // The text at the place, which finds it again when a newer omareader
    // splits the book's paragraphs differently; empty for older positions.
    QString anchor;
    QDateTime opened;
    // The collections the book is in, by name.
    QStringList collections;
};

class Library : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList books READ books NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    // The books the library shows: those in the chosen collection that
    // match the filter, in the chosen order.
    Q_PROPERTY(QVariantList shelf READ shelf NOTIFY changed)
    // Every collection, in alphabetical order, each with its name and how
    // many books it holds.
    Q_PROPERTY(QVariantList collections READ collectionList NOTIFY changed)
    // The collection shown; empty for every book.
    Q_PROPERTY(QString collection READ collection WRITE setCollection NOTIFY changed)
    // recent, title, author or progress; kept with the library.
    Q_PROPERTY(QString sortOrder READ sortOrder WRITE setSortOrder NOTIFY changed)
    // Words the shown books' titles, authors or sites must contain.
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY changed)

public:
    explicit Library(const QString &file = {}, const QString &coverDirectory = {}, QObject *parent = nullptr);

    // Every book, most recently opened first.
    QVariantList books() const;
    QVariantList shelf() const;
    QStringList collectionNames() const { return m_collections; }
    QVariantList collectionList() const;
    QString collection() const { return m_collection; }
    void setCollection(const QString &name);
    QString sortOrder() const { return m_sortOrder; }
    void setSortOrder(const QString &order);
    QString filter() const { return m_filter; }
    void setFilter(const QString &filter);

    // Adds a collection and returns its name as kept, or the name of the one
    // that already has it; empty when the name is blank or the library can't
    // be saved. Every change to the collections is undone when it can't be
    // saved, and reports false then.
    Q_INVOKABLE QString addCollection(const QString &name);
    // Renames a collection, taking its books along. Fails for a blank name
    // or one another collection has.
    Q_INVOKABLE bool renameCollection(const QString &name, const QString &newName);
    // Deletes a collection. Its books stay in the library.
    Q_INVOKABLE bool removeCollection(const QString &name);
    // Puts a book in a collection, or takes it out.
    Q_INVOKABLE bool setInCollection(const QString &path, const QString &name, bool in);
    const QList<LibraryEntry> &entries() const { return m_entries; }
    int count() const { return int(m_entries.size()); }
    LibraryEntry entry(const QString &path) const;
    bool contains(const QString &path) const;

    // Records an opened book (metadata and cover), keeping its saved position.
    void opened(const Book &book);
    void savePosition(const QString &path, int chapter, int paragraph, int sentence, int sentenceCount,
                      int offset = -1, const QString &anchor = {});
    // Saves pasted text as a library item and returns its path, or an empty
    // string when there is no text to save or it can't be written. A web
    // page's text comes with the address it was fetched from and its byline.
    QString addText(const QString &title, const QString &text, const QString &source = {},
                    const QString &author = {});
    QString textDirectory() const { return m_textDirectory; }
    bool isText(const QString &path) const;
    // Forgets a book. A book's file stays; a pasted text goes to the trash.
    Q_INVOKABLE void remove(const QString &path);
    bool save() const;
    // A web page's site, as its card shows it.
    static QString siteOf(const QString &source);

signals:
    void changed();
    // A book left the library.
    void removed(const QString &path);

private:
    void load();
    QString storeCover(const Book &book) const;
    int indexOf(const QString &path) const;
    int collectionIndex(const QString &name) const;
    void sort();
    QVariantMap bookMap(const LibraryEntry &e) const;
    // How the library is organised, to put back when a change can't be saved.
    struct Organisation {
        QStringList collections;
        QList<QStringList> memberships;
        QString collection;
        QString sortOrder;
    };
    Organisation organisation() const;
    bool commit(const Organisation &before);

    QString m_file;
    QString m_coverDirectory;
    QString m_textDirectory;
    QList<LibraryEntry> m_entries;
    QStringList m_collections;
    QString m_collection;
    QString m_sortOrder = "recent";
    QString m_filter;
};
