#include "library.h"

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

Library::Library(const QString &file, const QString &coverDirectory, QObject *parent)
    : QObject(parent),
      m_file(file.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/library.json" : file),
      m_coverDirectory(coverDirectory.isEmpty()
                           ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/covers"
                           : coverDirectory) {
    load();
}

void Library::load() {
    QFile file(m_file);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonArray books = QJsonDocument::fromJson(file.readAll()).object().value("books").toArray();
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
        e.chapter = o.value("chapter").toInt();
        e.paragraph = o.value("paragraph").toInt();
        e.sentence = o.value("sentence").toInt();
        e.sentenceCount = o.value("sentenceCount").toInt();
        e.opened = QDateTime::fromString(o.value("opened").toString(), Qt::ISODate);
        m_entries.append(e);
    }
    sort();
}

bool Library::save() const {
    QJsonArray books;
    for (const auto &e : m_entries)
        books.append(QJsonObject{{"path", e.path},
                                 {"title", e.title},
                                 {"author", e.author},
                                 {"format", e.format},
                                 {"cover", e.cover},
                                 {"chapter", e.chapter},
                                 {"paragraph", e.paragraph},
                                 {"sentence", e.sentence},
                                 {"sentenceCount", e.sentenceCount},
                                 {"opened", e.opened.toString(Qt::ISODate)}});
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    // Written beside the target and renamed over it, so a crash never leaves
    // half a library behind.
    QSaveFile file(m_file);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(QJsonObject{{"version", 1}, {"books", books}}).toJson());
    return file.commit();
}

QVariantList Library::books() const {
    QVariantList list;
    for (const auto &e : m_entries) {
        const double progress = e.sentenceCount > 1 ? double(e.sentence) / (e.sentenceCount - 1) : 0;
        list.append(QVariantMap{{"path", e.path},
                                {"title", e.title},
                                {"author", e.author},
                                {"format", e.format.toUpper()},
                                {"cover", e.cover.isEmpty() ? QString() : QUrl::fromLocalFile(e.cover).toString()},
                                {"progress", qBound(0.0, progress, 1.0)},
                                {"missing", !QFileInfo::exists(e.path)},
                                {"opened", e.opened}});
    }
    return list;
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
    e.sentenceCount = book.sentenceCount();
    if (e.cover.isEmpty() || !QFileInfo::exists(e.cover))
        e.cover = storeCover(book);
    e.opened = QDateTime::currentDateTimeUtc();
    sort();
    save();
    emit changed();
}

void Library::savePosition(const QString &path, int chapter, int paragraph, int sentence, int sentenceCount) {
    const int i = indexOf(path);
    if (i < 0)
        return;
    LibraryEntry &e = m_entries[i];
    if (e.chapter == chapter && e.paragraph == paragraph && e.sentence == sentence && e.sentenceCount == sentenceCount)
        return;
    e.chapter = chapter;
    e.paragraph = paragraph;
    e.sentence = sentence;
    e.sentenceCount = sentenceCount;
    save();
    emit changed();
}

void Library::remove(const QString &path) {
    const int i = indexOf(path);
    if (i < 0)
        return;
    // Forgets the book only; the file itself is left alone.
    const QString cover = m_entries.at(i).cover;
    m_entries.removeAt(i);
    if (!cover.isEmpty() && cover.startsWith(m_coverDirectory))
        QFile::remove(cover);
    save();
    emit changed();
}
