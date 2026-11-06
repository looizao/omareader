#pragma once

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QString>

// Writes a ZIP archive whose entries are stored, not compressed. The phone
// bundle is mostly AAC audio, which compression can't shrink, and storing
// keeps every entry readable at a known offset without a zlib dependency.
// Archives stay under 4 GiB and 65535 entries (no ZIP64), far beyond any
// book read aloud at speech bitrates.
class ZipWriter {
public:
    explicit ZipWriter(const QString &path);
    ~ZipWriter();

    bool open();
    bool addData(const QString &name, const QByteArray &data);
    // Reads the file twice, once for its checksum and once to copy it, so a
    // large file never has to fit in memory.
    bool addFile(const QString &name, const QString &path);
    // Writes the central directory and closes the file.
    bool finish();
    QString error() const { return m_error; }

    static quint32 crc32(const QByteArray &data, quint32 crc = 0);

private:
    struct Entry {
        QByteArray name;
        quint32 crc = 0;
        quint32 size = 0;
        quint32 offset = 0;
    };

    bool begin(const QString &name, quint32 crc, qint64 size);
    bool write(const QByteArray &data);
    bool fail(const QString &message);

    QFile m_file;
    QList<Entry> m_entries;
    quint16 m_time = 0;
    quint16 m_date = 0;
    QString m_error;
};
