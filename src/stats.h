#pragma once

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

// How much time goes into each book, read on screen and heard aloud, and
// into reading on each day, kept in ~/.local/share/omareader/stats.json.
// It stays on this computer: nothing here is ever sent anywhere.
//
// Time counts while a book is open and either the voice is playing (heard)
// or the window is in front and the reader has done something lately
// (read). A book left open in a window nobody looks at stops counting a few
// minutes after the last scroll, key or mouse movement.
class ReadingStats : public QObject {
    Q_OBJECT
    // The open book's time: {reading, listening, total, days, first, last,
    // since}, the times in seconds, first and last as ISO dates and since
    // as the first shown in the language's short form; empty with no book.
    Q_PROPERTY(QVariantMap current READ current NOTIFY changed)
    // Reading habits across every book: {today, week, total, streak, days,
    // books}, the times in seconds, with `days` the last seven days, oldest
    // first, as {date, label, reading, listening}, and `books` the books
    // with the most time, as {path, title, reading, listening, total}.
    Q_PROPERTY(QVariantMap overview READ overview NOTIFY changed)

public:
    struct BookTime {
        QString title;
        qint64 reading = 0; // milliseconds
        qint64 listening = 0;
        int days = 0; // days with any time on the book
        QDate first;
        QDate last;
    };
    struct DayTime {
        qint64 reading = 0;
        qint64 listening = 0;
    };

    // No input for this long and the window's time stops counting as read.
    static constexpr qint64 IdleLimit = 5 * 60 * 1000;
    // How often the time so far is counted while a book is open.
    static constexpr int TickInterval = 30 * 1000;
    // How many books the overview lists.
    static constexpr int TopBooks = 5;

    explicit ReadingStats(const QString &file = {}, QObject *parent = nullptr);
    ~ReadingStats() override;

    QVariantMap current() const;
    QVariantMap overview() const;
    BookTime book(const QString &path) const { return m_books.value(path); }
    DayTime day(const QDate &date) const { return m_days.value(date); }

    // The book that is open, or none with an empty path. Time up to now
    // goes to the book open until now.
    void setBook(const QString &path, const QString &title = {});
    // Whether the voice is playing.
    void setListening(bool listening);
    // Whether the window is in front.
    void setActive(bool active);
    // The reader scrolled, pressed a key or moved the mouse.
    Q_INVOKABLE void noteActivity();
    // Counts the time since the last count.
    Q_INVOKABLE void checkpoint();
    // Forgets a book's time, as removing it from the library does. The days
    // it was read on keep their time.
    void forget(const QString &path);
    // Writes the file now if anything changed since it was last written.
    bool save();

    // A time as the panel shows it: "12 min", "3 h 5 min".
    Q_INVOKABLE static QString duration(qint64 seconds);

    // Tests set the time.
    void setClock(std::function<QDateTime()> clock) { m_clock = std::move(clock); }

signals:
    void changed();

private:
    void load();
    QDateTime now() const { return m_clock ? m_clock() : QDateTime::currentDateTime(); }
    void add(const QDate &date, qint64 reading, qint64 listening);
    // Counts the time so far, restarts the count and the tick.
    void restart();

    QString m_file;
    QHash<QString, BookTime> m_books;
    QMap<QDate, DayTime> m_days;
    std::function<QDateTime()> m_clock;

    QString m_path;
    bool m_listening = false;
    bool m_active = true;
    QDateTime m_since;
    QDateTime m_lastActivity;
    QDateTime m_lastSave;
    bool m_dirty = false;
    QTimer m_tick;
};
