#include "annotations.h"

#include "filepicker.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>
#include <QUuid>
#include <algorithm>

namespace {

const auto FolderPurpose = QStringLiteral("sync");

// Letters, digits and the marks over them make up words; an apostrophe
// inside a word ("don't") does too.
bool wordChar(const QString &text, int i) {
    if (i < 0 || i >= text.size())
        return false;
    const QChar c = text.at(i);
    if (c.isLetterOrNumber() || c.isMark())
        return true;
    return (c == u'\'' || c == u'’') && i > 0 && i + 1 < text.size() && text.at(i - 1).isLetterOrNumber()
           && text.at(i + 1).isLetterOrNumber();
}

bool before(int paragraphA, int offsetA, int paragraphB, int offsetB) {
    return paragraphA < paragraphB || (paragraphA == paragraphB && offsetA < offsetB);
}

QJsonObject toJson(const Annotation &a) {
    QJsonObject o{{"id", a.id},
                  {"kind", a.kind},
                  {"chapter", a.chapter},
                  {"paragraph", a.paragraph},
                  {"start", a.start},
                  {"text", a.text},
                  {"created", a.created.toString(Qt::ISODateWithMs)},
                  {"modified", a.modified.toString(Qt::ISODateWithMs)}};
    if (a.isHighlight()) {
        o.insert("endParagraph", a.endParagraph);
        o.insert("end", a.end);
    }
    if (!a.note.isEmpty())
        o.insert("note", a.note);
    if (a.deleted)
        o.insert("deleted", true);
    return o;
}

QByteArray serialize(QList<Annotation> items) {
    std::sort(items.begin(), items.end(), [](const Annotation &a, const Annotation &b) { return a.id < b.id; });
    QJsonArray list;
    for (const Annotation &a : std::as_const(items))
        list.append(toJson(a));
    return QJsonDocument(QJsonObject{{"version", 1}, {"annotations", list}}).toJson();
}

QDateTime now() { return QDateTime::currentDateTimeUtc(); }

// A chapter's paragraphs as one text, joined by newlines, so a passage
// that spans paragraphs can be searched for like any other.
struct ChapterText {
    QString text;
    QList<int> starts; // where each paragraph starts in text

    ChapterText(const Chapter &chapter) {
        for (const Paragraph &p : chapter.paragraphs) {
            if (!starts.isEmpty())
                text += u'\n';
            starts.append(int(text.size()));
            text += p.text;
        }
    }
    int at(int paragraph, int offset) const { return starts.value(paragraph) + offset; }
    std::pair<int, int> position(int index) const {
        const auto it = std::upper_bound(starts.begin(), starts.end(), index);
        const int paragraph = qMax(0, int(it - starts.begin()) - 1);
        return {paragraph, index - starts.value(paragraph)};
    }
};

} // namespace

Annotations::Annotations(const QString &directory, const QString &device, QObject *parent)
    : QObject(parent), m_directory(directory.isEmpty() ? defaultDirectory() : directory), m_device(device),
      m_syncFolder(savedSyncFolder()) {
    if (m_device.isEmpty()) {
        QSettings settings;
        m_device = settings.value("annotations/device").toString();
        if (m_device.isEmpty()) {
            // The computer's name, so the files in the sync folder say whose
            // they are, and a few random characters, so two computers with
            // the same name stay apart.
            QString host = QSysInfo::machineHostName().toLower();
            host.replace(QRegularExpression("[^a-z0-9-]+"), "-");
            host = host.left(32);
            m_device = (host.isEmpty() ? QStringLiteral("computer") : host) + "-"
                       + QUuid::createUuid().toString(QUuid::Id128).left(6);
            settings.setValue("annotations/device", m_device);
        }
    }
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &Annotations::sync);
}

QString Annotations::defaultDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/annotations";
}

QString Annotations::savedSyncFolder() { return QSettings().value("annotations/syncFolder").toString(); }

QString Annotations::fingerprint(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return QString::fromLatin1(hash.result().toHex().left(32));
}

QString Annotations::localFile() const {
    return m_fingerprint.isEmpty() ? QString() : m_directory + "/" + m_fingerprint + ".json";
}

QString Annotations::syncDirectory() const {
    return m_syncFolder.isEmpty() || m_fingerprint.isEmpty() ? QString()
                                                             : m_syncFolder + "/omareader/" + m_fingerprint;
}

QString Annotations::deviceFile() const {
    const QString folder = syncDirectory();
    return folder.isEmpty() ? QString() : folder + "/" + m_device + ".json";
}

QList<Annotation> Annotations::read(const QString &file) {
    QList<Annotation> items;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return items;
    const QJsonArray list = QJsonDocument::fromJson(f.readAll()).object().value("annotations").toArray();
    for (const auto &value : list) {
        const QJsonObject o = value.toObject();
        Annotation a;
        a.id = o.value("id").toString();
        a.kind = o.value("kind").toString();
        if (a.id.isEmpty() || (a.kind != "bookmark" && a.kind != "highlight"))
            continue;
        a.chapter = qMax(0, o.value("chapter").toInt());
        a.paragraph = qMax(0, o.value("paragraph").toInt());
        a.start = qMax(0, o.value("start").toInt());
        a.endParagraph = a.isHighlight() ? qMax(a.paragraph, o.value("endParagraph").toInt()) : a.paragraph;
        a.end = a.isHighlight() ? qMax(0, o.value("end").toInt()) : a.start;
        a.text = o.value("text").toString();
        a.note = o.value("note").toString();
        a.created = QDateTime::fromString(o.value("created").toString(), Qt::ISODateWithMs);
        a.modified = QDateTime::fromString(o.value("modified").toString(), Qt::ISODateWithMs);
        a.deleted = o.value("deleted").toBool();
        items.append(a);
    }
    return items;
}

bool Annotations::write(const QString &file, const QList<Annotation> &items) const {
    // A book with no notes leaves no file behind, here or in the sync folder.
    if (items.isEmpty() && !QFileInfo::exists(file))
        return true;
    const QByteArray data = serialize(items);
    QFile existing(file);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == data)
        return true;
    QDir().mkpath(QFileInfo(file).absolutePath());
    // Notes can be private, so only their owner can read them.
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    out.write(data);
    return out.commit();
}

QList<Annotation> Annotations::merge(const QList<Annotation> &a, const QList<Annotation> &b) {
    QList<Annotation> merged = a;
    QHash<QString, int> index;
    for (int i = 0; i < merged.size(); ++i)
        index.insert(merged.at(i).id, i);
    for (const Annotation &theirs : b) {
        const auto it = index.constFind(theirs.id);
        if (it == index.cend()) {
            index.insert(theirs.id, int(merged.size()));
            merged.append(theirs);
            continue;
        }
        Annotation &ours = merged[*it];
        // The latest change wins. Two changes at the same moment are rare;
        // then a removal wins, and otherwise what is here stays, so two
        // computers never trade the same change back and forth.
        if (theirs.modified > ours.modified || (theirs.modified == ours.modified && theirs.deleted && !ours.deleted))
            ours = theirs;
    }
    return merged;
}

QList<Annotation> Annotations::stored(const QString &fingerprint, const QString &directory, const QString &syncFolder) {
    if (fingerprint.isEmpty())
        return {};
    QList<Annotation> items = read((directory.isEmpty() ? defaultDirectory() : directory) + "/" + fingerprint + ".json");
    if (!syncFolder.isEmpty()) {
        const QDir folder(syncFolder + "/omareader/" + fingerprint);
        for (const QString &name : folder.entryList({"*.json"}, QDir::Files, QDir::Name))
            items = merge(items, read(folder.filePath(name)));
    }
    return items;
}

QString Annotations::passage(const Book &book, int chapter, int paragraph, int start, int endParagraph, int end) {
    if (chapter < 0 || chapter >= book.chapters.size())
        return {};
    const auto &paragraphs = book.chapters.at(chapter).paragraphs;
    if (paragraph < 0 || endParagraph >= paragraphs.size() || before(endParagraph, end, paragraph, start))
        return {};
    QStringList parts;
    for (int p = paragraph; p <= endParagraph; ++p) {
        const QString &text = paragraphs.at(p).text;
        const int from = p == paragraph ? qBound(0, start, int(text.size())) : 0;
        const int to = p == endParagraph ? qBound(from, end, int(text.size())) : int(text.size());
        parts.append(text.mid(from, to - from));
    }
    return parts.join(u'\n');
}

void Annotations::place(QList<Annotation> &items, const Book &book) {
    QList<ChapterText> chapters;
    for (const Chapter &c : book.chapters)
        chapters.append(ChapterText(c));
    for (Annotation &a : items) {
        if (a.deleted)
            continue;
        // Still where it was?
        if (a.chapter < chapters.size()) {
            const auto &paragraphs = book.chapters.at(a.chapter).paragraphs;
            if (a.isHighlight()) {
                if (a.endParagraph < paragraphs.size() && a.end <= paragraphs.at(a.endParagraph).text.size()
                    && passage(book, a.chapter, a.paragraph, a.start, a.endParagraph, a.end) == a.text) {
                    a.placed = true;
                    continue;
                }
            } else if (a.paragraph < paragraphs.size() && a.start <= paragraphs.at(a.paragraph).text.size()
                       && QStringView(paragraphs.at(a.paragraph).text).mid(a.start).startsWith(a.text)) {
                a.placed = true;
                continue;
            }
        }
        // Otherwise the same text nearest to where it was: in its chapter
        // first, then in the chapters closest to it.
        a.placed = false;
        if (a.text.isEmpty())
            continue;
        const int home = qBound(0, a.chapter, int(chapters.size()) - 1);
        QList<int> order{home};
        for (int distance = 1; distance < chapters.size(); ++distance)
            for (int c : {home - distance, home + distance})
                if (c >= 0 && c < chapters.size())
                    order.append(c);
        for (int c : std::as_const(order)) {
            if (a.placed)
                break;
            const ChapterText &chapter = chapters.at(c);
            const int origin = c == a.chapter ? chapter.at(qMin(a.paragraph, int(chapter.starts.size()) - 1), 0) : 0;
            int best = -1;
            for (int i = chapter.text.indexOf(a.text); i >= 0; i = chapter.text.indexOf(a.text, i + 1))
                if (best < 0 || qAbs(i - origin) < qAbs(best - origin))
                    best = i;
            if (best < 0)
                continue;
            const auto [paragraph, start] = chapter.position(best);
            a.chapter = c;
            a.paragraph = paragraph;
            a.start = start;
            if (a.isHighlight()) {
                const auto [endParagraph, end] = chapter.position(best + int(a.text.size()));
                a.endParagraph = endParagraph;
                a.end = end;
            } else {
                a.endParagraph = paragraph;
                a.end = start;
            }
            a.placed = true;
        }
    }
}

QVariantMap Annotations::toMap(const Annotation &a, const Book *book) {
    const QString chapterTitle = book && a.chapter < book->chapters.size() ? book->chapters.at(a.chapter).title : QString();
    return QVariantMap{{"id", a.id},
                       {"kind", a.kind},
                       {"chapter", a.chapter},
                       {"chapterTitle", chapterTitle},
                       {"paragraph", a.paragraph},
                       {"start", a.start},
                       {"endParagraph", a.endParagraph},
                       {"end", a.end},
                       {"text", a.text},
                       {"note", a.note},
                       {"placed", a.placed},
                       {"created", a.created},
                       {"modified", a.modified}};
}

QVariantList Annotations::items() const {
    QList<const Annotation *> shown;
    for (const Annotation &a : m_items)
        if (!a.deleted)
            shown.append(&a);
    std::stable_sort(shown.begin(), shown.end(), [](const Annotation *a, const Annotation *b) {
        if (a->chapter != b->chapter)
            return a->chapter < b->chapter;
        return before(a->paragraph, a->start, b->paragraph, b->start);
    });
    QVariantList list;
    for (const Annotation *a : std::as_const(shown))
        list.append(toMap(*a, m_book.get()));
    return list;
}

int Annotations::count() const {
    return int(std::count_if(m_items.begin(), m_items.end(), [](const Annotation &a) { return !a.deleted; }));
}

void Annotations::setBook(BookPtr book) {
    if (m_book == book)
        return;
    m_book = std::move(book);
    m_chapter = 0;
    m_items.clear();
    m_fingerprint.clear();
    m_syncedDevices = 0;
    if (m_book) {
        m_fingerprint = m_book->fingerprint.isEmpty() ? fingerprint(m_book->path) : m_book->fingerprint;
        m_items = read(localFile());
        place(m_items, *m_book);
    }
    watch();
    rebuild();
    emit changed();
    emit syncChanged();
    sync();
}

void Annotations::setChapter(int chapter) {
    if (chapter == m_chapter && !m_chapterMarks.isEmpty())
        return;
    m_chapter = chapter;
    rebuild();
    emit changed();
}

void Annotations::rebuild() {
    m_chapterMarks.clear();
    if (!m_book || m_chapter < 0 || m_chapter >= m_book->chapters.size())
        return;
    const auto &paragraphs = m_book->chapters.at(m_chapter).paragraphs;
    QList<QVariantList> marks(paragraphs.size());
    QList<bool> bookmarks(paragraphs.size(), false);
    for (const Annotation &a : std::as_const(m_items)) {
        if (a.deleted || !a.placed || a.chapter != m_chapter || a.paragraph >= paragraphs.size())
            continue;
        if (!a.isHighlight()) {
            bookmarks[a.paragraph] = true;
            continue;
        }
        for (int p = a.paragraph; p <= qMin(a.endParagraph, int(paragraphs.size()) - 1); ++p) {
            const int start = p == a.paragraph ? a.start : 0;
            const int end = p == a.endParagraph ? a.end : int(paragraphs.at(p).text.size());
            if (end > start)
                marks[p].append(QVariantMap{{"id", a.id}, {"start", start}, {"end", end}, {"note", !a.note.isEmpty()}});
        }
    }
    for (int p = 0; p < paragraphs.size(); ++p) {
        QVariantMap entry;
        if (!marks.at(p).isEmpty())
            entry.insert("marks", marks.at(p));
        if (bookmarks.at(p))
            entry.insert("bookmark", true);
        m_chapterMarks.append(entry);
    }
}

Annotation *Annotations::find(const QString &id) {
    for (Annotation &a : m_items)
        if (a.id == id && !a.deleted)
            return &a;
    return nullptr;
}

void Annotations::save() {
    if (m_fingerprint.isEmpty())
        return;
    write(localFile(), m_items);
    const QString file = deviceFile();
    if (!file.isEmpty()) {
        write(file, m_items);
        watchBook();
    }
}

void Annotations::adopt(const QList<Annotation> &items, bool saveThem) {
    m_items = items;
    if (m_book)
        place(m_items, *m_book);
    if (saveThem)
        save();
    rebuild();
    emit changed();
}

QString Annotations::addHighlight(int paragraph, int start, int endParagraph, int end) {
    if (!m_book || m_chapter >= m_book->chapters.size())
        return {};
    const auto &paragraphs = m_book->chapters.at(m_chapter).paragraphs;
    if (before(endParagraph, end, paragraph, start)) {
        std::swap(paragraph, endParagraph);
        std::swap(start, end);
    }
    if (paragraph < 0 || endParagraph >= paragraphs.size())
        return {};
    auto text = [&](int p) -> const QString & { return paragraphs.at(p).text; };
    start = qBound(0, start, int(text(paragraph).size()));
    end = qBound(0, end, int(text(endParagraph).size()));
    // Leave out the spaces at either end, then take in the whole of the
    // words the passage starts and ends inside.
    while (before(paragraph, start, endParagraph, end)) {
        if (start >= text(paragraph).size()) {
            ++paragraph;
            start = 0;
        } else if (text(paragraph).at(start).isSpace()) {
            ++start;
        } else {
            break;
        }
    }
    while (before(paragraph, start, endParagraph, end)) {
        if (end <= 0) {
            --endParagraph;
            end = int(text(endParagraph).size());
        } else if (text(endParagraph).at(end - 1).isSpace()) {
            --end;
        } else {
            break;
        }
    }
    if (!before(paragraph, start, endParagraph, end))
        return {};
    start = wordEdge(text(paragraph), start, false);
    end = wordEdge(text(endParagraph), end, true);

    const QDateTime time = now();
    Annotation highlight;
    highlight.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    highlight.kind = QStringLiteral("highlight");
    highlight.chapter = m_chapter;
    highlight.created = highlight.modified = time;
    // A highlight this one touches is folded into it, so a passage is never
    // marked twice, and its note is kept.
    QStringList notes;
    for (Annotation &other : m_items) {
        if (other.deleted || !other.placed || !other.isHighlight() || other.chapter != m_chapter)
            continue;
        if (!before(other.paragraph, other.start, endParagraph, end) || !before(paragraph, start, other.endParagraph, other.end))
            continue;
        if (before(other.paragraph, other.start, paragraph, start)) {
            paragraph = other.paragraph;
            start = other.start;
        }
        if (before(endParagraph, end, other.endParagraph, other.end)) {
            endParagraph = other.endParagraph;
            end = other.end;
        }
        if (!other.note.isEmpty())
            notes.append(other.note);
        if (other.created < highlight.created)
            highlight.created = other.created;
        other.deleted = true;
        other.modified = time;
    }
    highlight.paragraph = paragraph;
    highlight.start = start;
    highlight.endParagraph = endParagraph;
    highlight.end = end;
    highlight.text = passage(*m_book, m_chapter, paragraph, start, endParagraph, end);
    highlight.note = notes.join("\n\n");
    m_items.append(highlight);
    save();
    rebuild();
    emit changed();
    return highlight.id;
}

bool Annotations::hasBookmark(int chapter, int paragraph) const {
    return std::any_of(m_items.begin(), m_items.end(), [&](const Annotation &a) {
        return !a.deleted && a.placed && !a.isHighlight() && a.chapter == chapter && a.paragraph == paragraph;
    });
}

QString Annotations::toggleBookmark(int chapter, int paragraph, int offset) {
    if (!m_book)
        return {};
    const int sentence = m_book->sentenceAt(chapter, paragraph, offset);
    if (sentence < 0)
        return {};
    const Sentence &s = m_book->sentences.at(sentence);
    const QDateTime time = now();
    bool removed = false;
    for (Annotation &a : m_items) {
        if (!a.deleted && a.placed && !a.isHighlight() && a.chapter == s.chapter && a.paragraph == s.paragraph) {
            a.deleted = true;
            a.modified = time;
            removed = true;
        }
    }
    QString id;
    if (!removed) {
        Annotation bookmark;
        bookmark.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        bookmark.kind = QStringLiteral("bookmark");
        bookmark.chapter = s.chapter;
        bookmark.paragraph = bookmark.endParagraph = s.paragraph;
        bookmark.start = bookmark.end = s.start;
        // The sentence's start, enough to find it again.
        bookmark.text = m_book->chapters.at(s.chapter).paragraphs.at(s.paragraph).text.mid(s.start, s.end - s.start).left(160);
        bookmark.created = bookmark.modified = time;
        m_items.append(bookmark);
        id = bookmark.id;
    }
    save();
    rebuild();
    emit changed();
    return id;
}

void Annotations::setNote(const QString &id, const QString &note) {
    Annotation *a = find(id);
    const QString trimmed = note.trimmed();
    if (!a || a->note == trimmed)
        return;
    a->note = trimmed;
    a->modified = now();
    save();
    rebuild();
    emit changed();
}

void Annotations::remove(const QString &id) {
    Annotation *a = find(id);
    if (!a)
        return;
    a->deleted = true;
    a->modified = now();
    save();
    rebuild();
    emit changed();
}

int Annotations::wordEdge(const QString &text, int offset, bool forward) {
    offset = qBound(0, offset, int(text.size()));
    if (forward) {
        while (wordChar(text, offset) && wordChar(text, offset - 1))
            ++offset;
    } else {
        while (wordChar(text, offset - 1) && wordChar(text, offset))
            --offset;
    }
    return offset;
}

int Annotations::wordEdge(int paragraph, int offset, bool forward) const {
    if (!m_book || m_chapter >= m_book->chapters.size() || paragraph < 0
        || paragraph >= m_book->chapters.at(m_chapter).paragraphs.size())
        return offset;
    return wordEdge(m_book->chapters.at(m_chapter).paragraphs.at(paragraph).text, offset, forward);
}

QString Annotations::highlightAt(int paragraph, int offset) const {
    for (const Annotation &a : m_items)
        if (!a.deleted && a.placed && a.isHighlight() && a.chapter == m_chapter
            && !before(paragraph, offset, a.paragraph, a.start) && before(paragraph, offset, a.endParagraph, a.end))
            return a.id;
    return {};
}

QVariantMap Annotations::item(const QString &id) const {
    for (const Annotation &a : m_items)
        if (a.id == id && !a.deleted)
            return toMap(a, m_book.get());
    return {};
}

void Annotations::setPicker(FilePicker *picker) {
    m_picker = picker;
    if (m_picker)
        connect(m_picker, &FilePicker::folderSelected, this, [this](const QUrl &url, const QString &purpose) {
            if (purpose == FolderPurpose && url.isLocalFile())
                setSyncFolder(url.toLocalFile());
        });
}

void Annotations::chooseSyncFolder() {
    if (m_picker)
        m_picker->chooseFolder(m_syncFolder.isEmpty() ? QDir::homePath() : m_syncFolder, FolderPurpose);
}

void Annotations::setSyncFolder(const QString &folder) {
    const QString cleaned = folder.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(folder).absoluteFilePath());
    if (cleaned == m_syncFolder)
        return;
    m_syncFolder = cleaned;
    QSettings settings;
    if (cleaned.isEmpty())
        settings.remove("annotations/syncFolder");
    else
        settings.setValue("annotations/syncFolder", cleaned);
    m_syncedDevices = 0;
    watch();
    emit syncChanged();
    syncAll();
    sync();
}

void Annotations::syncAll() {
    if (m_syncFolder.isEmpty())
        return;
    const QDir local(m_directory);
    for (const QString &name : local.entryList({"*.json"}, QDir::Files)) {
        const QString fingerprint = QFileInfo(name).completeBaseName();
        if (fingerprint == m_fingerprint)
            continue; // sync() merges the open book's
        const QList<Annotation> merged = stored(fingerprint, m_directory, m_syncFolder);
        write(local.filePath(name), merged);
        write(m_syncFolder + "/omareader/" + fingerprint + "/" + m_device + ".json", merged);
    }
}

void Annotations::sync() {
    const QString folder = syncDirectory();
    if (folder.isEmpty())
        return;
    watchBook();
    QList<Annotation> merged = m_items;
    int devices = 0;
    const QDir dir(folder);
    for (const QString &name : dir.entryList({"*.json"}, QDir::Files, QDir::Name)) {
        if (name == m_device + ".json")
            continue;
        ++devices;
        merged = merge(merged, read(dir.filePath(name)));
    }
    if (devices != m_syncedDevices) {
        m_syncedDevices = devices;
        emit syncChanged();
    }
    if (serialize(merged) != serialize(m_items))
        adopt(merged, true);
    else
        save(); // makes sure this computer's file is there
}

void Annotations::watch() {
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    if (syncDirectory().isEmpty())
        return;
    // The folder of every book's notes tells when another computer starts
    // on this book; the book's own folder, once there is one, when it
    // changes its notes.
    const QString books = m_syncFolder + "/omareader";
    QDir().mkpath(books);
    m_watcher.addPath(books);
    watchBook();
}

void Annotations::watchBook() {
    const QString folder = syncDirectory();
    if (!folder.isEmpty() && QFileInfo(folder).isDir() && !m_watcher.directories().contains(folder))
        m_watcher.addPath(folder);
}

// Compiled as part of this file, not on its own; see omareader.pro.
#include "moc_annotations.cpp"
