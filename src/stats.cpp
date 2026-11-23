#include "stats.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

// The file is written at most this often while a book is open, so a crash
// loses little and a long session doesn't write it every tick.
static constexpr qint64 SaveInterval = 2 * 60 * 1000;
// How long a save waits for another window's to finish.
static constexpr int LockWaitMs = 2000;

ReadingStats::ReadingStats(const QString &file, QObject *parent)
    : QObject(parent),
      m_file(file.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/stats.json" : file) {
    m_monotonic.start();
    read(m_file, &m_books, &m_days);
    m_tick.setInterval(TickInterval);
    connect(&m_tick, &QTimer::timeout, this, &ReadingStats::checkpoint);
    // The window in front counts as reading; behind other windows it doesn't.
    if (qGuiApp) {
        m_active = QGuiApplication::applicationState() == Qt::ApplicationActive;
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) { setActive(state == Qt::ApplicationActive); });
    }
}

ReadingStats::~ReadingStats() {
    checkpoint();
    save();
}

void ReadingStats::setClock(std::function<QDateTime()> wall, std::function<qint64()> ticks) {
    if (!ticks)
        ticks = [wall] { return wall().toMSecsSinceEpoch(); };
    m_wall = std::move(wall);
    m_ticks = std::move(ticks);
}

// A file that isn't there reads as empty; one that is there but can't be
// opened is false, so it is never saved over.
bool ReadingStats::read(const QString &file, QHash<QString, BookTime> *books, QMap<QDate, DayTime> *days) {
    QFile in(file);
    if (!in.exists())
        return true;
    if (!in.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
    for (const auto &value : root.value("books").toArray()) {
        const QJsonObject o = value.toObject();
        const QString path = o.value("path").toString();
        if (path.isEmpty())
            continue;
        BookTime b;
        b.title = o.value("title").toString();
        b.reading = qMax<qint64>(0, o.value("reading").toInteger());
        b.listening = qMax<qint64>(0, o.value("listening").toInteger());
        b.days = qMax(0, o.value("days").toInt());
        b.first = QDate::fromString(o.value("first").toString(), Qt::ISODate);
        b.last = QDate::fromString(o.value("last").toString(), Qt::ISODate);
        books->insert(path, b);
    }
    const QJsonObject dayTimes = root.value("days").toObject();
    for (auto it = dayTimes.begin(); it != dayTimes.end(); ++it) {
        const QDate date = QDate::fromString(it.key(), Qt::ISODate);
        if (!date.isValid())
            continue;
        const QJsonObject o = it.value().toObject();
        days->insert(date, {qMax<qint64>(0, o.value("reading").toInteger()),
                            qMax<qint64>(0, o.value("listening").toInteger())});
    }
    return true;
}

bool ReadingStats::save() {
    if (!m_dirty)
        return true;
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    // Another omareader window may have saved since this one read the file:
    // read it again under a lock and add only what this window counted.
    QLockFile lock(m_file + ".lock");
    lock.setStaleLockTime(10000);
    if (!lock.tryLock(LockWaitMs))
        return false;
    QHash<QString, BookTime> books;
    QMap<QDate, DayTime> days;
    if (!read(m_file, &books, &days))
        return false;
    for (const QString &path : std::as_const(m_forgotten))
        books.remove(path);
    for (auto it = m_pendingDays.cbegin(); it != m_pendingDays.cend(); ++it) {
        days[it.key()].reading += it->reading;
        days[it.key()].listening += it->listening;
    }
    for (auto it = m_pendingBooks.cbegin(); it != m_pendingBooks.cend(); ++it) {
        if (!books.contains(it.key()) && it->reading + it->listening <= 0)
            continue;
        BookTime &b = books[it.key()];
        if (!it->title.isEmpty())
            b.title = it->title;
        b.reading += it->reading;
        b.listening += it->listening;
        for (const QDate &date : it->dates) {
            // A day already counted, here or by another window, counts once.
            if (!b.last.isValid() || date > b.last) {
                ++b.days;
                b.last = date;
            }
            if (!b.first.isValid() || date < b.first)
                b.first = date;
        }
    }

    // Times are kept in milliseconds, so the short counts of each tick add
    // up without rounding.
    QJsonArray bookArray;
    for (auto it = books.cbegin(); it != books.cend(); ++it)
        bookArray.append(QJsonObject{{"path", it.key()},
                                     {"title", it->title},
                                     {"reading", it->reading},
                                     {"listening", it->listening},
                                     {"days", it->days},
                                     {"first", it->first.toString(Qt::ISODate)},
                                     {"last", it->last.toString(Qt::ISODate)}});
    QJsonObject dayObject;
    for (auto it = days.cbegin(); it != days.cend(); ++it)
        dayObject.insert(it.key().toString(Qt::ISODate),
                         QJsonObject{{"reading", it->reading}, {"listening", it->listening}});
    // Written beside the target and renamed over it, so a crash never leaves
    // half a file behind.
    QSaveFile file(m_file);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(QJsonObject{{"version", 1}, {"books", bookArray}, {"days", dayObject}})
                   .toJson(QJsonDocument::Compact));
    if (!file.commit())
        return false;
    m_books = books;
    m_days = days;
    m_pendingBooks.clear();
    m_pendingDays.clear();
    m_forgotten.clear();
    m_dirty = false;
    m_lastSave = ticks();
    emit changed(); // with whatever other windows added
    return true;
}

void ReadingStats::add(const QDate &date, qint64 reading, qint64 listening) {
    if (m_path.isEmpty() || (reading <= 0 && listening <= 0))
        return;
    DayTime &day = m_days[date];
    day.reading += reading;
    day.listening += listening;
    DayTime &pendingDay = m_pendingDays[date];
    pendingDay.reading += reading;
    pendingDay.listening += listening;

    BookTime &book = m_books[m_path];
    if (!m_title.isEmpty())
        book.title = m_title;
    book.reading += reading;
    book.listening += listening;
    if (!book.last.isValid() || date > book.last) {
        ++book.days;
        book.last = date;
    }
    if (!book.first.isValid() || date < book.first)
        book.first = date;
    Pending &pending = m_pendingBooks[m_path];
    pending.title = m_title;
    pending.reading += reading;
    pending.listening += listening;
    if (!pending.dates.contains(date)) {
        pending.dates.append(date);
        std::sort(pending.dates.begin(), pending.dates.end());
    }
    m_dirty = true;
}

void ReadingStats::checkpoint() {
    const qint64 t = ticks();
    if (m_lastActivity > t) // only a test's clock goes back
        m_lastActivity = t;
    if (m_path.isEmpty() || m_since < 0 || m_since >= t) {
        m_since = t;
        return;
    }
    // Read time: up to a few minutes after the last input while the window
    // is in front and the voice is quiet. Heard time comes from the narrator.
    const qint64 from = m_since;
    qint64 to = from;
    if (!m_listening && m_active && m_lastActivity >= 0)
        to = qMin(t, m_lastActivity + IdleLimit);
    m_since = t;
    bool added = false;
    if (to > from) {
        // Spans are measured on the clock that only goes forward; the wall
        // clock says which day they belong to, split at midnight.
        const QDateTime wall = wallNow();
        QDateTime start = wall.addMSecs(from - t), end = wall.addMSecs(to - t);
        while (start < end) {
            const QDateTime midnight(start.date().addDays(1), QTime(0, 0));
            const QDateTime stop = midnight > start ? qMin(end, midnight) : end;
            add(start.date(), start.msecsTo(stop), 0);
            start = stop;
        }
        added = true;
    }
    if (added)
        emit changed();
    if (m_dirty && (m_lastSave < 0 || t - m_lastSave >= SaveInterval))
        save();
}

void ReadingStats::addHeard(qint64 ms) {
    if (m_path.isEmpty() || ms <= 0)
        return;
    add(wallNow().date(), 0, ms);
    emit changed();
}

void ReadingStats::restart() {
    m_since = ticks();
    m_lastActivity = m_since;
    if (m_path.isEmpty())
        m_tick.stop();
    else
        m_tick.start();
}

void ReadingStats::setBook(const QString &path, const QString &title) {
    checkpoint();
    const bool other = path != m_path;
    m_path = path;
    m_title = title;
    // A book whose title changed keeps the new one.
    if (!path.isEmpty() && !title.isEmpty() && m_books.contains(path) && m_books.value(path).title != title) {
        m_books[path].title = title;
        m_pendingBooks[path].title = title;
        m_dirty = true;
    }
    restart();
    if (other) {
        save();
        emit changed();
    }
}

void ReadingStats::setListening(bool listening) {
    if (listening == m_listening)
        return;
    checkpoint();
    m_listening = listening;
    // Once the voice stops the reader is still there, reading on.
    m_lastActivity = ticks();
}

void ReadingStats::setActive(bool active) {
    if (active == m_active)
        return;
    checkpoint();
    m_active = active;
    if (active)
        m_lastActivity = ticks();
}

void ReadingStats::noteActivity() {
    const qint64 t = ticks();
    // After a long pause the idle time is settled first, so it never counts.
    if (m_lastActivity >= 0 && t - m_lastActivity > IdleLimit)
        checkpoint();
    m_lastActivity = t;
}

void ReadingStats::forget(const QString &path) {
    if (path == m_path)
        checkpoint();
    m_pendingBooks.remove(path);
    m_forgotten.insert(path);
    m_books.remove(path);
    m_dirty = true;
    save();
    emit changed();
}

QVariantMap ReadingStats::current() const {
    if (m_path.isEmpty())
        return {};
    const BookTime b = m_books.value(m_path);
    return {{"reading", b.reading / 1000},
            {"listening", b.listening / 1000},
            {"total", (b.reading + b.listening) / 1000},
            {"days", b.days},
            {"first", b.first.toString(Qt::ISODate)},
            {"last", b.last.toString(Qt::ISODate)},
            {"since", b.first.isValid() ? QLocale().toString(b.first, QLocale::ShortFormat) : QString()}};
}

QVariantMap ReadingStats::overview() const {
    const QDate today = wallNow().date();
    auto seconds = [](const DayTime &d) { return (d.reading + d.listening) / 1000; };
    qint64 week = 0, total = 0;
    QVariantList days;
    for (int i = 6; i >= 0; --i) {
        const QDate date = today.addDays(-i);
        const DayTime d = m_days.value(date);
        week += seconds(d);
        days.append(QVariantMap{{"date", date.toString(Qt::ISODate)},
                                {"label", QLocale().dayName(date.dayOfWeek(), QLocale::ShortFormat)},
                                {"reading", d.reading / 1000},
                                {"listening", d.listening / 1000}});
    }
    for (const DayTime &d : m_days)
        total += seconds(d);
    // Days in a row with at least a minute of reading, ending today, or
    // yesterday while today has none yet.
    auto counted = [&](const QDate &date) { return m_days.value(date).reading + m_days.value(date).listening >= 60000; };
    int streak = 0;
    QDate date = counted(today) ? today : today.addDays(-1);
    while (counted(date)) {
        ++streak;
        date = date.addDays(-1);
    }
    QList<std::pair<QString, BookTime>> ranked;
    for (auto it = m_books.cbegin(); it != m_books.cend(); ++it)
        if (it->reading + it->listening >= 1000)
            ranked.append({it.key(), *it});
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        const qint64 ta = a.second.reading + a.second.listening, tb = b.second.reading + b.second.listening;
        return ta != tb ? ta > tb : a.first < b.first;
    });
    QVariantList books;
    for (int i = 0; i < qMin(TopBooks, int(ranked.size())); ++i) {
        const BookTime &b = ranked.at(i).second;
        books.append(QVariantMap{{"path", ranked.at(i).first},
                                 {"title", b.title.isEmpty() ? QFileInfo(ranked.at(i).first).completeBaseName() : b.title},
                                 {"reading", b.reading / 1000},
                                 {"listening", b.listening / 1000},
                                 {"total", (b.reading + b.listening) / 1000}});
    }
    return {{"today", seconds(m_days.value(today))},
            {"week", week},
            {"total", total},
            {"streak", streak},
            {"days", days},
            {"books", books}};
}

QString ReadingStats::duration(qint64 seconds) {
    const qint64 minutes = (qMax<qint64>(0, seconds) + 30) / 60;
    const QLocale locale;
    if (minutes < 60)
        return tr("%1 min").arg(locale.toString(minutes));
    if (minutes % 60 == 0)
        return tr("%1 h").arg(locale.toString(minutes / 60));
    return tr("%1 h %2 min").arg(locale.toString(minutes / 60), locale.toString(minutes % 60));
}

// Compiled as part of this file, not on its own; see omareader.pro.
#include "moc_stats.cpp"
