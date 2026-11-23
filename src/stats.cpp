#include "stats.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

// The file is written at most this often while a book is open, so a crash
// loses little and a long session doesn't write it every tick.
static constexpr qint64 SaveInterval = 2 * 60 * 1000;

ReadingStats::ReadingStats(const QString &file, QObject *parent)
    : QObject(parent),
      m_file(file.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/stats.json" : file) {
    load();
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

void ReadingStats::load() {
    QFile file(m_file);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
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
        m_books.insert(path, b);
    }
    const QJsonObject days = root.value("days").toObject();
    for (auto it = days.begin(); it != days.end(); ++it) {
        const QDate date = QDate::fromString(it.key(), Qt::ISODate);
        if (!date.isValid())
            continue;
        const QJsonObject o = it.value().toObject();
        m_days.insert(date, {qMax<qint64>(0, o.value("reading").toInteger()),
                             qMax<qint64>(0, o.value("listening").toInteger())});
    }
}

bool ReadingStats::save() {
    if (!m_dirty)
        return true;
    // Times are kept in milliseconds, so the short counts of each tick add
    // up without rounding.
    QJsonArray books;
    for (auto it = m_books.cbegin(); it != m_books.cend(); ++it)
        books.append(QJsonObject{{"path", it.key()},
                                 {"title", it->title},
                                 {"reading", it->reading},
                                 {"listening", it->listening},
                                 {"days", it->days},
                                 {"first", it->first.toString(Qt::ISODate)},
                                 {"last", it->last.toString(Qt::ISODate)}});
    QJsonObject days;
    for (auto it = m_days.cbegin(); it != m_days.cend(); ++it)
        days.insert(it.key().toString(Qt::ISODate), QJsonObject{{"reading", it->reading}, {"listening", it->listening}});
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    // Written beside the target and renamed over it, so a crash never leaves
    // half a file behind.
    QSaveFile file(m_file);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(QJsonObject{{"version", 1}, {"books", books}, {"days", days}}).toJson(QJsonDocument::Compact));
    if (!file.commit())
        return false;
    m_dirty = false;
    m_lastSave = now();
    return true;
}

void ReadingStats::add(const QDate &date, qint64 reading, qint64 listening) {
    if (reading <= 0 && listening <= 0)
        return;
    DayTime &day = m_days[date];
    day.reading += reading;
    day.listening += listening;
    if (!m_path.isEmpty()) {
        BookTime &book = m_books[m_path];
        book.reading += reading;
        book.listening += listening;
        if (book.last != date) {
            ++book.days;
            book.last = date;
        }
        if (!book.first.isValid())
            book.first = date;
    }
    m_dirty = true;
}

void ReadingStats::checkpoint() {
    const QDateTime t = now();
    if (m_path.isEmpty() || !m_since.isValid() || m_since >= t) {
        m_since = t;
        return;
    }
    // The span that counts: all of it while the voice plays, up to a few
    // minutes after the last input while the window is in front. A machine
    // asleep with the voice paused by it never counts for long.
    QDateTime from = m_since, to = t;
    if (m_listening)
        from = qMax(from, t.addMSecs(-IdleLimit));
    else if (m_active && m_lastActivity.isValid())
        to = qMin(t, m_lastActivity.addMSecs(IdleLimit));
    else
        to = from;
    m_since = t;
    bool added = false;
    // Split at midnight, so each day gets its own share.
    while (from < to) {
        const QDateTime midnight(from.date().addDays(1), QTime(0, 0));
        const QDateTime end = midnight > from ? qMin(to, midnight) : to;
        const qint64 ms = from.msecsTo(end);
        add(from.date(), m_listening ? 0 : ms, m_listening ? ms : 0);
        added = added || ms > 0;
        from = end;
    }
    if (added)
        emit changed();
    if (m_dirty && (!m_lastSave.isValid() || m_lastSave.msecsTo(t) >= SaveInterval))
        save();
}

void ReadingStats::restart() {
    m_since = now();
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
    if (!path.isEmpty() && m_books.contains(path) && !title.isEmpty() && m_books[path].title != title) {
        m_books[path].title = title;
        m_dirty = true;
    } else if (!path.isEmpty() && !m_books.contains(path)) {
        // Kept from the start, so the title is there once time is counted.
        m_books[path].title = title;
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
    m_lastActivity = now();
}

void ReadingStats::setActive(bool active) {
    if (active == m_active)
        return;
    checkpoint();
    m_active = active;
    if (active)
        m_lastActivity = now();
}

void ReadingStats::noteActivity() {
    const QDateTime t = now();
    // After a long pause the idle time is settled first, so it never counts.
    if (m_lastActivity.isValid() && m_lastActivity.msecsTo(t) > IdleLimit)
        checkpoint();
    m_lastActivity = t;
}

void ReadingStats::forget(const QString &path) {
    if (path == m_path)
        checkpoint();
    if (!m_books.remove(path))
        return;
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
    const QDate today = now().date();
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
