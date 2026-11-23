#include "library.h"

#include "bookloader.h"
#include "titleagent.h"

#include <QCollator>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>

Library::Library(const QString &file, const QString &coverDirectory, QObject *parent)
    : QObject(parent),
      m_file(file.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/library.json" : file),
      m_coverDirectory(coverDirectory.isEmpty()
                           ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/covers"
                           : coverDirectory),
      m_textDirectory(QFileInfo(m_file).absolutePath() + "/texts") {
    load();
}

namespace {

QStringList sortOrders() { return {"recent", "title", "author", "progress"}; }

// Names are told apart regardless of case, so "Fiction" and "fiction" are
// one collection.
int nameIndex(const QStringList &names, const QString &name) {
    const QString folded = name.toCaseFolded();
    for (int i = 0; i < names.size(); ++i)
        if (names.at(i).toCaseFolded() == folded)
            return i;
    return -1;
}

// Adds a name to the list unless it has it already, and returns the name as
// the list keeps it; empty for a blank name.
QString addName(QStringList &names, const QString &name) {
    const QString clean = name.simplified();
    if (clean.isEmpty())
        return {};
    const int i = nameIndex(names, clean);
    if (i >= 0)
        return names.at(i);
    names.append(clean);
    return clean;
}

QCollator collator() {
    QCollator c;
    c.setCaseSensitivity(Qt::CaseInsensitive);
    c.setNumericMode(true); // "Volume 2" before "Volume 10"
    return c;
}

void sortNames(QStringList &names) {
    const QCollator c = collator();
    std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) { return c.compare(a, b) < 0; });
}

// Text as the filter compares it: without case or accents, so "jose" finds
// "José".
QString folded(const QString &text) {
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed)
        if (c.category() != QChar::Mark_NonSpacing)
            out.append(c);
    return out.toCaseFolded();
}

double progressOf(const LibraryEntry &e) {
    const double progress = e.sentenceCount > 1 ? double(e.sentence) / (e.sentenceCount - 1) : 0;
    return qBound(0.0, progress, 1.0);
}

} // namespace

void Library::load() {
    QFile file(m_file);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonArray books = root.value("books").toArray();
    for (const auto &value : books) {
        const QJsonObject o = value.toObject();
        LibraryEntry e;
        e.path = o.value("path").toString();
        if (e.path.isEmpty())
            continue;
        e.title = o.value("title").toString();
        e.author = o.value("author").toString();
        e.format = o.value("format").toString();
        e.cover = o.value("cover").toString();
        e.source = o.value("source").toString();
        e.chapter = o.value("chapter").toInt();
        e.paragraph = o.value("paragraph").toInt();
        e.sentence = o.value("sentence").toInt();
        e.sentenceCount = o.value("sentenceCount").toInt();
        e.opened = QDateTime::fromString(o.value("opened").toString(), Qt::ISODate);
        e.offset = o.value("offset").toInt(-1);
        e.anchor = o.value("anchor").toString();
        for (const auto &name : o.value("collections").toArray()) {
            const QString kept = addName(m_collections, name.toString());
            if (!kept.isEmpty() && !e.collections.contains(kept))
                e.collections.append(kept);
        }
        m_entries.append(e);
    }
    // Collections are listed on their own too, so one with no books yet is
    // kept. Libraries saved before collections have neither list.
    for (const auto &name : root.value("collections").toArray())
        addName(m_collections, name.toString());
    sortNames(m_collections);
    if (sortOrders().contains(root.value("sort").toString()))
        m_sortOrder = root.value("sort").toString();
    sort();
}

bool Library::save() const {
    QJsonArray books;
    for (const auto &e : m_entries) {
        QJsonObject book{{"path", e.path},
                         {"title", e.title},
                         {"author", e.author},
                         {"format", e.format},
                         {"cover", e.cover},
                         {"source", e.source},
                         {"chapter", e.chapter},
                         {"paragraph", e.paragraph},
                         {"sentence", e.sentence},
                         {"sentenceCount", e.sentenceCount},
                         {"offset", e.offset},
                         {"anchor", e.anchor},
                         {"opened", e.opened.toString(Qt::ISODate)}};
        if (!e.collections.isEmpty())
            book.insert("collections", QJsonArray::fromStringList(e.collections));
        books.append(book);
    }
    QJsonObject root{{"version", 1}, {"books", books}, {"sort", m_sortOrder}};
    if (!m_collections.isEmpty())
        root.insert("collections", QJsonArray::fromStringList(m_collections));
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    // Written beside the target and renamed over it, so a crash never leaves
    // half a library behind.
    QSaveFile file(m_file);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(root).toJson());
    return file.commit();
}

QVariantMap Library::bookMap(const LibraryEntry &e) const {
    return QVariantMap{{"path", e.path},
                       {"title", e.title},
                       {"author", e.author},
                       {"format", e.format.toUpper()},
                       {"cover", e.cover.isEmpty() ? QString() : QUrl::fromLocalFile(e.cover).toString()},
                       {"progress", progressOf(e)},
                       {"missing", !QFileInfo::exists(e.path)},
                       {"pasted", isText(e.path)},
                       {"source", e.source},
                       {"site", siteOf(e.source)},
                       {"opened", e.opened},
                       {"collections", e.collections}};
}

QVariantList Library::books() const {
    QVariantList list;
    for (const auto &e : m_entries)
        list.append(bookMap(e));
    return list;
}

QVariantList Library::shelf() const {
    const QStringList words = folded(m_filter).split(' ', Qt::SkipEmptyParts);
    QList<const LibraryEntry *> shown;
    for (const auto &e : m_entries) {
        if (!m_collection.isEmpty() && nameIndex(e.collections, m_collection) < 0)
            continue;
        const QString text = folded(e.title + ' ' + e.author + ' ' + siteOf(e.source));
        if (std::all_of(words.begin(), words.end(), [&](const QString &w) { return text.contains(w); }))
            shown.append(&e);
    }
    // The entries are kept most recent first, and the sort is stable, so
    // ties stay in that order.
    const QCollator c = collator();
    auto byTitle = [&](const LibraryEntry *a, const LibraryEntry *b) { return c.compare(a->title, b->title) < 0; };
    if (m_sortOrder == "title") {
        std::stable_sort(shown.begin(), shown.end(), byTitle);
    } else if (m_sortOrder == "author") {
        // Books with no author go last.
        std::stable_sort(shown.begin(), shown.end(), [&](const LibraryEntry *a, const LibraryEntry *b) {
            if (a->author.isEmpty() != b->author.isEmpty())
                return b->author.isEmpty();
            const int order = c.compare(a->author, b->author);
            return order != 0 ? order < 0 : byTitle(a, b);
        });
    } else if (m_sortOrder == "progress") {
        std::stable_sort(shown.begin(), shown.end(),
                         [](const LibraryEntry *a, const LibraryEntry *b) { return progressOf(*a) > progressOf(*b); });
    }
    QVariantList list;
    for (const LibraryEntry *e : shown)
        list.append(bookMap(*e));
    return list;
}

QVariantList Library::collectionList() const {
    QVariantList list;
    for (const auto &name : m_collections) {
        const auto count = std::count_if(m_entries.begin(), m_entries.end(),
                                         [&](const LibraryEntry &e) { return e.collections.contains(name); });
        list.append(QVariantMap{{"name", name}, {"count", int(count)}});
    }
    return list;
}

int Library::collectionIndex(const QString &name) const { return nameIndex(m_collections, name.simplified()); }

void Library::setCollection(const QString &name) {
    const int i = collectionIndex(name);
    const QString chosen = i >= 0 ? m_collections.at(i) : QString();
    if (chosen == m_collection)
        return;
    m_collection = chosen;
    emit changed();
}

void Library::setSortOrder(const QString &order) {
    if (order == m_sortOrder || !sortOrders().contains(order))
        return;
    m_sortOrder = order;
    save();
    emit changed();
}

void Library::setFilter(const QString &filter) {
    if (filter == m_filter)
        return;
    m_filter = filter;
    emit changed();
}

QString Library::addCollection(const QString &name) {
    const int before = int(m_collections.size());
    const QString kept = addName(m_collections, name);
    if (kept.isEmpty() || m_collections.size() == before)
        return kept;
    sortNames(m_collections);
    save();
    emit changed();
    return kept;
}

bool Library::renameCollection(const QString &name, const QString &newName) {
    const int i = collectionIndex(name);
    const QString clean = newName.simplified();
    if (i < 0 || clean.isEmpty())
        return false;
    const QString old = m_collections.at(i);
    const int taken = nameIndex(m_collections, clean);
    if (taken >= 0 && taken != i)
        return false;
    if (clean == old)
        return true;
    m_collections[i] = clean;
    sortNames(m_collections);
    for (auto &e : m_entries)
        for (auto &n : e.collections)
            if (n == old)
                n = clean;
    if (m_collection == old)
        m_collection = clean;
    save();
    emit changed();
    return true;
}

void Library::removeCollection(const QString &name) {
    const int i = collectionIndex(name);
    if (i < 0)
        return;
    const QString old = m_collections.takeAt(i);
    for (auto &e : m_entries)
        e.collections.removeAll(old);
    if (m_collection == old)
        m_collection.clear();
    save();
    emit changed();
}

void Library::setInCollection(const QString &path, const QString &name, bool in) {
    const int b = indexOf(path);
    if (b < 0)
        return;
    LibraryEntry &e = m_entries[b];
    if (in) {
        const QString kept = addName(m_collections, name);
        if (kept.isEmpty() || e.collections.contains(kept))
            return;
        sortNames(m_collections);
        e.collections.append(kept);
    } else {
        const int i = collectionIndex(name);
        if (i < 0 || !e.collections.removeAll(m_collections.at(i)))
            return;
    }
    save();
    emit changed();
}

QString Library::siteOf(const QString &source) {
    QString host = QUrl(source).host();
    if (host.startsWith("www."))
        host = host.mid(4);
    return host;
}

int Library::indexOf(const QString &path) const {
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries.at(i).path == path)
            return i;
    return -1;
}

bool Library::contains(const QString &path) const { return indexOf(path) >= 0; }

LibraryEntry Library::entry(const QString &path) const {
    const int i = indexOf(path);
    return i >= 0 ? m_entries.at(i) : LibraryEntry{};
}

void Library::sort() {
    std::stable_sort(m_entries.begin(), m_entries.end(),
                     [](const LibraryEntry &a, const LibraryEntry &b) { return a.opened > b.opened; });
}

QString Library::storeCover(const Book &book) const {
    if (book.cover.isEmpty())
        return {};
    const QImage image = QImage::fromData(book.cover);
    if (image.isNull())
        return {};
    QDir().mkpath(m_coverDirectory);
    const QString name = QCryptographicHash::hash(book.path.toUtf8(), QCryptographicHash::Sha1).toHex() + ".jpg";
    const QString path = m_coverDirectory + "/" + name;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    image.scaledToHeight(qMin(480, image.height()), Qt::SmoothTransformation).save(&file, "JPG", 88);
    return file.commit() ? path : QString();
}

void Library::opened(const Book &book) {
    int i = indexOf(book.path);
    if (i < 0) {
        m_entries.append(LibraryEntry{});
        i = int(m_entries.size()) - 1;
        m_entries[i].path = book.path;
    }
    LibraryEntry &e = m_entries[i];
    e.title = book.title;
    e.author = book.author;
    e.format = book.format;
    if (!book.source.isEmpty())
        e.source = book.source;
    e.sentenceCount = book.sentenceCount();
    if (e.cover.isEmpty() || !QFileInfo::exists(e.cover))
        e.cover = storeCover(book);
    e.opened = QDateTime::currentDateTimeUtc();
    sort();
    save();
    emit changed();
}

void Library::savePosition(const QString &path, int chapter, int paragraph, int sentence, int sentenceCount,
                           int offset, const QString &anchor) {
    const int i = indexOf(path);
    if (i < 0)
        return;
    LibraryEntry &e = m_entries[i];
    if (e.chapter == chapter && e.paragraph == paragraph && e.sentence == sentence && e.sentenceCount == sentenceCount
        && e.offset == offset && e.anchor == anchor)
        return;
    e.anchor = anchor;
    e.chapter = chapter;
    e.paragraph = paragraph;
    e.sentence = sentence;
    e.sentenceCount = sentenceCount;
    e.offset = offset;
    save();
    emit changed();
}

bool Library::isText(const QString &path) const {
    return QFileInfo(path).absolutePath() == QFileInfo(m_textDirectory).absoluteFilePath();
}

QString Library::addText(const QString &title, const QString &text, const QString &source, const QString &author) {
    if (text.trimmed().isEmpty())
        return {};
    // Pasted text can be private, so only its owner can read it.
    if (!QDir().mkpath(m_textDirectory))
        return {};
    QFile::setPermissions(m_textDirectory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString path = m_textDirectory + "/" + now.toString("yyyyMMdd-HHmmss") + "-"
                         + QUuid::createUuid().toString(QUuid::Id128).left(8) + ".json";
    const QString name = title.simplified().isEmpty() ? PastedTitle::fromText(text) : title.simplified();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    file.write(BookLoader::pastedText(name, text, now, source, author.simplified()));
    if (!file.commit())
        return {};

    // In the library straight away, even before it is first opened.
    LibraryEntry e;
    e.path = path;
    e.title = name;
    e.author = author.simplified();
    e.source = source;
    e.format = source.isEmpty() ? "text" : "web";
    e.opened = now;
    m_entries.append(e);
    sort();
    save();
    emit changed();
    return path;
}

void Library::remove(const QString &path) {
    const int i = indexOf(path);
    if (i < 0)
        return;
    // Forgets the book only; the file itself is left alone. Pasted text has
    // no other copy, so it goes to the trash, where it can still be restored.
    const QString cover = m_entries.at(i).cover;
    m_entries.removeAt(i);
    if (!cover.isEmpty() && cover.startsWith(m_coverDirectory))
        QFile::remove(cover);
    if (isText(path) && QFileInfo::exists(path))
        QFile::moveToTrash(path);
    save();
    emit changed();
    emit removed(path);
}

// Compiled as part of this file, not on its own; see omareader.pro.
#include "moc_library.cpp"
