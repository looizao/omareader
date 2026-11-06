#include "zipwriter.h"

#include <QDateTime>
#include <QtEndian>
#include <array>

namespace {
constexpr quint32 MaxSize = 0xFFFFFFFFu;
constexpr int MaxEntries = 0xFFFF;
constexpr qint64 Chunk = 1 << 20;
// Bit 11: names are UTF-8.
constexpr quint16 Utf8Names = 0x0800;

void put16(QByteArray &out, quint16 value) {
    char bytes[2];
    qToLittleEndian(value, bytes);
    out.append(bytes, 2);
}

void put32(QByteArray &out, quint32 value) {
    char bytes[4];
    qToLittleEndian(value, bytes);
    out.append(bytes, 4);
}

const std::array<quint32, 256> &crcTable() {
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    return table;
}
}

ZipWriter::ZipWriter(const QString &path) : m_file(path) {
    // DOS time has two-second resolution and starts in 1980.
    const QDateTime now = QDateTime::currentDateTime();
    const QDate date = now.date();
    const QTime time = now.time();
    m_date = quint16(((qMax(1980, date.year()) - 1980) << 9) | (date.month() << 5) | date.day());
    m_time = quint16((time.hour() << 11) | (time.minute() << 5) | (time.second() / 2));
}

ZipWriter::~ZipWriter() {
    if (m_file.isOpen())
        m_file.close();
}

quint32 ZipWriter::crc32(const QByteArray &data, quint32 crc) {
    const auto &table = crcTable();
    crc = ~crc;
    for (const char byte : data)
        crc = table[(crc ^ quint8(byte)) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool ZipWriter::open() {
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(m_file.errorString());
    return true;
}

bool ZipWriter::fail(const QString &message) {
    if (m_error.isEmpty())
        m_error = message;
    return false;
}

bool ZipWriter::write(const QByteArray &data) {
    if (m_file.write(data) != data.size())
        return fail(m_file.errorString());
    return true;
}

bool ZipWriter::begin(const QString &name, quint32 crc, qint64 size) {
    if (!m_file.isOpen() || !m_error.isEmpty())
        return fail(m_error.isEmpty() ? QStringLiteral("The archive is not open.") : m_error);
    if (m_entries.size() >= MaxEntries)
        return fail(QStringLiteral("Too many files for one archive."));
    const qint64 offset = m_file.pos();
    if (size > MaxSize || offset + size > MaxSize)
        return fail(QStringLiteral("The archive would be larger than 4 GiB."));
    Entry entry;
    entry.name = name.toUtf8();
    entry.crc = crc;
    entry.size = quint32(size);
    entry.offset = quint32(offset);

    QByteArray header;
    put32(header, 0x04034b50);
    put16(header, 10); // version needed: stored entries
    put16(header, Utf8Names);
    put16(header, 0); // stored
    put16(header, m_time);
    put16(header, m_date);
    put32(header, entry.crc);
    put32(header, entry.size);
    put32(header, entry.size);
    put16(header, quint16(entry.name.size()));
    put16(header, 0);
    header += entry.name;
    m_entries.append(entry);
    return write(header);
}

bool ZipWriter::addData(const QString &name, const QByteArray &data) {
    return begin(name, crc32(data), data.size()) && write(data);
}

bool ZipWriter::addFile(const QString &name, const QString &path) {
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly))
        return fail(in.errorString());
    quint32 crc = 0;
    while (!in.atEnd()) {
        const QByteArray chunk = in.read(Chunk);
        if (chunk.isEmpty() && !in.atEnd())
            return fail(in.errorString());
        crc = crc32(chunk, crc);
    }
    const qint64 size = in.size();
    if (!begin(name, crc, size) || !in.seek(0))
        return fail(in.errorString());
    qint64 copied = 0;
    while (copied < size) {
        const QByteArray chunk = in.read(Chunk);
        if (chunk.isEmpty())
            return fail(in.errorString());
        if (!write(chunk))
            return false;
        copied += chunk.size();
    }
    return true;
}

bool ZipWriter::finish() {
    if (!m_file.isOpen() || !m_error.isEmpty())
        return fail(m_error.isEmpty() ? QStringLiteral("The archive is not open.") : m_error);
    const qint64 start = m_file.pos();
    QByteArray directory;
    for (const Entry &entry : std::as_const(m_entries)) {
        put32(directory, 0x02014b50);
        put16(directory, (3 << 8) | 20); // made on Unix, ZIP 2.0
        put16(directory, 10);
        put16(directory, Utf8Names);
        put16(directory, 0);
        put16(directory, m_time);
        put16(directory, m_date);
        put32(directory, entry.crc);
        put32(directory, entry.size);
        put32(directory, entry.size);
        put16(directory, quint16(entry.name.size()));
        put16(directory, 0); // extra
        put16(directory, 0); // comment
        put16(directory, 0); // disk
        put16(directory, 0); // internal attributes
        put32(directory, quint32(0100644) << 16); // a regular file, rw-r--r--
        put32(directory, entry.offset);
        directory += entry.name;
    }
    if (start + directory.size() > MaxSize)
        return fail(QStringLiteral("The archive would be larger than 4 GiB."));
    QByteArray end;
    put32(end, 0x06054b50);
    put16(end, 0);
    put16(end, 0);
    put16(end, quint16(m_entries.size()));
    put16(end, quint16(m_entries.size()));
    put32(end, quint32(directory.size()));
    put32(end, quint32(start));
    put16(end, 0);
    if (!write(directory + end))
        return false;
    if (!m_file.flush())
        return fail(m_file.errorString());
    m_file.close();
    return true;
}
