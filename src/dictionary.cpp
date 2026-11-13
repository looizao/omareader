#include "dictionary.h"

#include "book.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QtConcurrent>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <zlib.h>

namespace {

// No dictionary file comes near this; it stops a damaged or hostile gzip
// file from inflating without end.
constexpr qsizetype MaxInflated = qsizetype(1) << 30;

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A whole gzip file, or an empty array when it doesn't inflate.
QByteArray gunzip(const QByteArray &data) {
    if (data.isEmpty())
        return {};
    z_stream stream{};
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK)
        return {};
    QByteArray out;
    out.resize(qMax<qsizetype>(data.size() * 4, 4096));
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    stream.avail_in = uInt(data.size());
    int status = Z_OK;
    qsizetype produced = 0;
    while (status == Z_OK) {
        if (produced == out.size()) {
            if (out.size() >= MaxInflated)
                break;
            out.resize(qMin(out.size() * 2, MaxInflated));
        }
        stream.next_out = reinterpret_cast<Bytef *>(out.data() + produced);
        stream.avail_out = uInt(out.size() - produced);
        status = inflate(&stream, Z_NO_FLUSH);
        produced = out.size() - stream.avail_out;
    }
    inflateEnd(&stream);
    if (status != Z_STREAM_END)
        return {};
    out.resize(produced);
    return out;
}

// One dictzip chunk: raw deflate data that ends on a full flush, so it
// inflates without what came before it.
QByteArray inflateChunk(const QByteArray &data, quint32 expected) {
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        return {};
    QByteArray out(qsizetype(expected), Qt::Uninitialized);
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    stream.avail_in = uInt(data.size());
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = uInt(out.size());
    const int status = inflate(&stream, Z_SYNC_FLUSH);
    const qsizetype produced = out.size() - stream.avail_out;
    inflateEnd(&stream);
    if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR)
        return {};
    out.resize(produced);
    return out;
}

quint16 le16(const QByteArray &data, qsizetype at) {
    return quint16(uchar(data.at(at))) | quint16(uchar(data.at(at + 1))) << 8;
}

QString decode(const QByteArray &bytes) {
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString text = utf8(bytes);
    return utf8.hasError() ? QString::fromLatin1(bytes) : text;
}

// dictd writes offsets and lengths as base-64 numbers.
quint64 dictdNumber(QByteArrayView digits, bool *ok) {
    static const QByteArray alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    quint64 value = 0;
    *ok = !digits.isEmpty() && digits.size() <= 10;
    for (const char c : digits) {
        const qsizetype digit = alphabet.indexOf(c);
        if (digit < 0) {
            *ok = false;
            return 0;
        }
        value = value * 64 + quint64(digit);
    }
    return value;
}

// Drops the leading line when it only repeats the headword, as many
// dictionaries start their articles, and the blank lines around the text.
QString tidy(QString text, const QString &headword) {
    text.replace(QStringLiteral("\r"), QString());
    QStringList lines = text.split('\n');
    for (QString &line : lines) {
        while (!line.isEmpty() && line.back().isSpace())
            line.chop(1);
    }
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty())
        lines.removeFirst();
    if (!lines.isEmpty() && lines.first().trimmed().compare(headword.trimmed(), Qt::CaseInsensitive) == 0)
        lines.removeFirst();
    QStringList kept;
    for (const QString &line : std::as_const(lines)) {
        if (line.trimmed().isEmpty() && (kept.isEmpty() || kept.last().isEmpty()))
            continue;
        kept.append(line.trimmed().isEmpty() ? QString() : line);
    }
    while (!kept.isEmpty() && kept.last().isEmpty())
        kept.removeLast();
    return kept.join('\n');
}

}

// The data file.

bool DictionaryData::open(const QString &path) {
    m_path = path;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    // The gzip header, with room for the largest chunk table it can carry.
    const QByteArray head = file.read(10 + 2 + 65535);
    if (head.size() < 18 || uchar(head.at(0)) != 0x1f || uchar(head.at(1)) != 0x8b)
        return true; // plain
    m_inflated = true;
    const uchar flags = uchar(head.at(3));
    qsizetype at = 10;
    if (flags & 4) {
        const quint16 length = le16(head, at);
        at += 2;
        const qsizetype extraEnd = at + length;
        if (extraEnd > head.size())
            return false;
        qsizetype field = at;
        while (field + 4 <= extraEnd) {
            const quint16 size = le16(head, field + 2);
            if (head.at(field) == 'R' && head.at(field + 1) == 'A' && size >= 6 && field + 4 + size <= extraEnd) {
                const quint32 chunkLength = le16(head, field + 6);
                const quint16 count = le16(head, field + 8);
                if (chunkLength > 0 && 6 + 2 * qsizetype(count) <= size) {
                    m_chunkLength = chunkLength;
                    for (int i = 0; i < count; ++i)
                        m_chunkStarts.append(le16(head, field + 10 + 2 * i));
                }
            }
            field += 4 + size;
        }
        at = extraEnd;
    }
    for (const uchar named : {uchar(8), uchar(16)})
        if (flags & named) {
            const qsizetype end = head.indexOf('\0', at);
            if (end < 0)
                return false;
            at = end + 1;
        }
    if (flags & 2)
        at += 2;
    if (m_chunkLength == 0 || m_chunkStarts.isEmpty())
        return true; // gzip without a chunk table: inflated whole when first read
    // The table holds each chunk's compressed size; keep where each starts.
    quint64 start = quint64(at);
    for (quint64 &chunk : m_chunkStarts) {
        const quint64 size = chunk;
        chunk = start;
        start += size;
    }
    m_chunkStarts.append(start);
    m_dictzip = true;
    return true;
}

QByteArray DictionaryData::read(quint64 offset, quint32 size) {
    if (size == 0)
        return {};
    if (!m_inflated) {
        QFile file(m_path);
        if (!file.open(QIODevice::ReadOnly) || !file.seek(qint64(offset)))
            return {};
        return file.read(size);
    }
    if (!m_dictzip) {
        if (m_whole.isEmpty())
            m_whole = gunzip(readFile(m_path));
        if (offset + size > quint64(m_whole.size()))
            return {};
        return m_whole.mid(qsizetype(offset), size);
    }
    const quint64 first = offset / m_chunkLength;
    const quint64 last = (offset + size - 1) / m_chunkLength;
    if (last + 1 >= quint64(m_chunkStarts.size()))
        return {};
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QByteArray out;
    for (quint64 chunk = first; chunk <= last; ++chunk) {
        const quint64 start = m_chunkStarts.at(qsizetype(chunk));
        if (!file.seek(qint64(start)))
            return {};
        const QByteArray compressed = file.read(qint64(m_chunkStarts.at(qsizetype(chunk) + 1) - start));
        out += inflateChunk(compressed, m_chunkLength);
    }
    const quint64 skip = offset - first * m_chunkLength;
    if (skip + size > quint64(out.size()))
        return {};
    return out.mid(qsizetype(skip), size);
}

// One dictionary.

std::shared_ptr<Dictionary> Dictionary::open(const QString &path) {
    auto dictionary = std::make_shared<Dictionary>();
    dictionary->m_path = path;
    const QFileInfo info(path);
    dictionary->m_name = info.completeBaseName();
    if (path.endsWith(".ifo")) {
        dictionary->m_format = StarDict;
        const QStringList lines = decode(readFile(path)).split('\n');
        if (lines.isEmpty() || !lines.first().startsWith("StarDict's dict ifo file"))
            return nullptr;
        for (const QString &line : lines) {
            const qsizetype equals = line.indexOf('=');
            if (equals <= 0)
                continue;
            const QString key = line.left(equals).trimmed(), value = line.mid(equals + 1).trimmed();
            if (key == "bookname" && !value.isEmpty())
                dictionary->m_name = value;
            else if (key == "wordcount")
                dictionary->m_declaredWords = value.toInt();
            else if (key == "sametypesequence")
                dictionary->m_types = value;
            else if (key == "idxoffsetbits")
                dictionary->m_offsets64 = value == "64";
        }
        return dictionary;
    }
    if (!path.endsWith(".index"))
        return nullptr;
    dictionary->m_format = Dictd;
    const QString base = path.chopped(6);
    if (!QFileInfo::exists(base + ".dict") && !QFileInfo::exists(base + ".dict.dz"))
        return nullptr;
    // The name is the article dictd keeps under 00-database-short.
    const QByteArray index = readFile(path);
    dictionary->m_declaredWords = int(index.count('\n'));
    for (const char *key : {"00-database-short\t", "00databaseshort\t"}) {
        const qsizetype at = index.startsWith(key) ? 0 : index.indexOf(QByteArray("\n") + key);
        if (at < 0)
            continue;
        const qsizetype start = at == 0 ? 0 : at + 1;
        const QList<QByteArray> fields = index.mid(start, index.indexOf('\n', start) - start).split('\t');
        bool okOffset = false, okSize = false;
        const quint64 offset = dictdNumber(fields.value(1), &okOffset);
        const quint64 size = dictdNumber(fields.value(2), &okSize);
        DictionaryData data;
        if (!okOffset || !okSize || size > 4096 || !data.open(QFileInfo::exists(base + ".dict.dz") ? base + ".dict.dz"
                                                                                                    : base + ".dict"))
            break;
        const QString text = tidy(decode(data.read(offset, quint32(size))), QString::fromUtf8(fields.value(0)));
        const QString name = text.section('\n', 0, 0).trimmed();
        if (!name.isEmpty())
            dictionary->m_name = name;
        break;
    }
    return dictionary;
}

int Dictionary::wordCount() const {
    return m_declaredWords;
}

quint64 Dictionary::keyHash(QByteArrayView utf8) {
    while (!utf8.isEmpty() && (utf8.front() == ' ' || utf8.front() == '\t'))
        utf8 = utf8.sliced(1);
    while (!utf8.isEmpty() && (utf8.back() == ' ' || utf8.back() == '\t'))
        utf8.chop(1);
    bool ascii = true;
    for (const char c : utf8)
        if (uchar(c) >= 0x80) {
            ascii = false;
            break;
        }
    if (ascii) {
        const QByteArray lower = utf8.toByteArray().toLower();
        return quint64(qHashBits(lower.constData(), size_t(lower.size()), 0));
    }
    const QByteArray folded =
        QString::fromUtf8(utf8).toCaseFolded().normalized(QString::NormalizationForm_C).toUtf8();
    return quint64(qHashBits(folded.constData(), size_t(folded.size()), 0));
}

bool Dictionary::loadIndex() {
    if (m_loaded)
        return !m_broken;
    m_loaded = true;
    m_broken = !(m_format == StarDict ? loadStarDict() : loadDictd());
    if (m_broken) {
        m_index.clear();
        m_keys = {};
    } else {
        std::stable_sort(m_keys.begin(), m_keys.end());
    }
    return !m_broken;
}

bool Dictionary::loadStarDict() {
    const QString base = m_path.chopped(4);
    m_index = QFileInfo::exists(base + ".idx") ? readFile(base + ".idx") : gunzip(readFile(base + ".idx.gz"));
    if (m_index.isEmpty())
        return false;
    const qsizetype tail = m_offsets64 ? 12 : 8;
    const char *data = m_index.constData();
    const qsizetype size = m_index.size();
    const bool synonyms = QFileInfo::exists(base + ".syn");
    std::vector<quint32> positions;
    m_keys.reserve(size_t(qMax(m_declaredWords, 0)));
    for (qsizetype at = 0; at < size;) {
        const char *end = static_cast<const char *>(std::memchr(data + at, 0, size_t(size - at)));
        if (!end || end - data + 1 + tail > size)
            break;
        m_keys.push_back({keyHash(QByteArrayView(data + at, end - data - at)), quint32(at)});
        if (synonyms)
            positions.push_back(quint32(at));
        at = end - data + 1 + tail;
    }
    // Synonyms (often a word's other forms) point at an entry by its number.
    if (synonyms) {
        const QByteArray syn = readFile(base + ".syn");
        const char *s = syn.constData();
        for (qsizetype at = 0; at < syn.size();) {
            const char *end = static_cast<const char *>(std::memchr(s + at, 0, size_t(syn.size() - at)));
            if (!end || end - s + 5 > syn.size())
                break;
            const quint32 entry = qFromBigEndian<quint32>(end + 1);
            if (entry < positions.size())
                m_keys.push_back({keyHash(QByteArrayView(s + at, end - s - at)), positions.at(entry)});
            at = end - s + 5;
        }
    }
    return m_data.open(QFileInfo::exists(base + ".dict.dz") ? base + ".dict.dz" : base + ".dict");
}

bool Dictionary::loadDictd() {
    m_index = readFile(m_path);
    if (m_index.isEmpty())
        return false;
    const char *data = m_index.constData();
    const qsizetype size = m_index.size();
    for (qsizetype at = 0; at < size;) {
        const char *newline = static_cast<const char *>(std::memchr(data + at, '\n', size_t(size - at)));
        const qsizetype end = newline ? newline - data : size;
        const char *tab = static_cast<const char *>(std::memchr(data + at, '\t', size_t(end - at)));
        if (tab)
            m_keys.push_back({keyHash(QByteArrayView(data + at, tab - data - at)), quint32(at)});
        at = end + 1;
    }
    const QString base = m_path.chopped(6);
    return m_data.open(QFileInfo::exists(base + ".dict.dz") ? base + ".dict.dz" : base + ".dict");
}

QList<DictionaryEntry> Dictionary::lookup(const QString &word, int limit) {
    QMutexLocker lock(&m_mutex);
    if (word.trimmed().isEmpty() || !loadIndex())
        return {};
    const auto [first, last] = std::equal_range(m_keys.begin(), m_keys.end(), Key{keyHash(word.toUtf8()), 0});
    std::vector<quint32> positions;
    for (auto it = first; it != last; ++it)
        positions.push_back(it->position);
    // In the dictionary's own order, each entry once (a synonym can lead to
    // an entry the word also has).
    std::sort(positions.begin(), positions.end());
    positions.erase(std::unique(positions.begin(), positions.end()), positions.end());

    QList<DictionaryEntry> entries;
    const char *data = m_index.constData();
    const qsizetype size = m_index.size();
    for (const quint32 position : positions) {
        if (entries.size() >= limit)
            break;
        QString headword;
        quint64 offset = 0, length = 0;
        if (m_format == StarDict) {
            const char *end = static_cast<const char *>(std::memchr(data + position, 0, size_t(size - position)));
            if (!end)
                continue;
            headword = QString::fromUtf8(data + position, end - data - position);
            if (m_offsets64) {
                offset = qFromBigEndian<quint64>(end + 1);
                length = qFromBigEndian<quint32>(end + 9);
            } else {
                offset = qFromBigEndian<quint32>(end + 1);
                length = qFromBigEndian<quint32>(end + 5);
            }
        } else {
            const char *newline = static_cast<const char *>(std::memchr(data + position, '\n', size_t(size - position)));
            const QList<QByteArray> fields =
                QByteArray(data + position, (newline ? newline - data : size) - position).split('\t');
            bool okOffset = false, okLength = false;
            headword = QString::fromUtf8(fields.value(0));
            offset = dictdNumber(fields.value(1), &okOffset);
            length = dictdNumber(fields.value(2), &okLength);
            if (!okOffset || !okLength)
                continue;
        }
        if (length == 0 || length > 4 * 1024 * 1024)
            continue;
        const QString text = articleText(m_data.read(offset, quint32(length)), headword);
        if (text.isEmpty())
            continue;
        const bool repeated = std::any_of(entries.cbegin(), entries.cend(), [&](const DictionaryEntry &e) {
            return e.text == text;
        });
        if (!repeated)
            entries.append({m_name, headword, text});
    }
    return entries;
}

QString Dictionary::articleText(const QByteArray &data, const QString &headword) const {
    if (data.isEmpty())
        return {};
    if (m_format == Dictd) {
        QString text = decode(data);
        // WordNet marks its cross-references {like this}.
        text.remove('{').remove('}');
        QStringList lines = text.split('\n');
        while (!lines.isEmpty() && lines.first().trimmed().isEmpty())
            lines.removeFirst();
        // The first line repeats the headword, often with its pronunciation.
        if (!lines.isEmpty()) {
            const QString first = lines.first().trimmed();
            if (first.startsWith(headword, Qt::CaseInsensitive)
                && (first.size() == headword.size() || first.at(headword.size()).isSpace())) {
                const QString rest = first.mid(headword.size()).trimmed();
                if (rest.isEmpty())
                    lines.removeFirst();
                else
                    lines.first() = rest;
            }
        }
        // Without the headword, the rest is indented; take the indent away.
        qsizetype indent = -1;
        for (const QString &line : std::as_const(lines)) {
            if (line.trimmed().isEmpty())
                continue;
            qsizetype spaces = 0;
            while (spaces < line.size() && line.at(spaces) == ' ')
                ++spaces;
            indent = indent < 0 ? spaces : qMin(indent, spaces);
        }
        if (indent > 0)
            for (QString &line : lines)
                line = line.mid(qMin(indent, line.size()));
        return tidy(lines.join('\n'), headword);
    }

    // StarDict: one or more fields, each with a type letter. A lowercase
    // type is text ending with a NUL, an uppercase one binary data led by its
    // size; the last field of a sametypesequence has neither.
    QStringList parts;
    auto add = [&](char type, const QByteArray &bytes) {
        const QString text = decode(bytes);
        switch (type) {
        case 'm': case 'l': case 'y': case 'w': case 'n':
            parts.append(tidy(text, headword));
            break;
        case 'g': case 'x': case 'k':
            parts.append(tidy(plainText(text, false), headword));
            break;
        case 'h':
            parts.append(tidy(plainText(text, true), headword));
            break;
        case 't':
            parts.append("/" + text.trimmed() + "/");
            break;
        default:
            break; // sounds, pictures and other files
        }
    };
    const char *p = data.constData();
    const char *end = p + data.size();
    auto field = [&](char type, bool last) {
        if (QChar::isLower(uchar(type))) {
            const char *stop = last ? end : static_cast<const char *>(std::memchr(p, 0, size_t(end - p)));
            if (!stop)
                stop = end;
            add(type, QByteArray(p, stop - p));
            p = stop == end ? end : stop + 1;
        } else {
            qsizetype size = end - p;
            if (!last) {
                if (end - p < 4) {
                    p = end;
                    return;
                }
                size = qMin<qsizetype>(qFromBigEndian<quint32>(p), end - p - 4);
                p += 4;
            }
            p += size;
        }
    };
    if (!m_types.isEmpty()) {
        for (qsizetype i = 0; i < m_types.size() && p < end; ++i)
            field(m_types.at(i).toLatin1(), i == m_types.size() - 1);
    } else {
        while (p < end) {
            const char type = *p++;
            field(type, false);
        }
    }
    return tidy(parts.join('\n'), headword);
}

QString Dictionary::plainText(const QString &markup, bool html) {
    static const QHash<QString, QString> entities{
        {"lt", "<"}, {"gt", ">"}, {"amp", "&"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
        {"ndash", QString(QChar(0x2013))}, {"mdash", QString(QChar(0x2014))}, {"hellip", QString(QChar(0x2026))},
        {"laquo", QString(QChar(0x00AB))}, {"raquo", QString(QChar(0x00BB))}, {"middot", QString(QChar(0x00B7))},
    };
    static const QStringList blocks{"p", "div", "li", "ol", "ul", "dl", "dt", "dd", "table", "blockquote",
                                    "h1", "h2", "h3", "h4", "h5", "h6", "hr", "pre", "section", "article"};
    QString out;
    bool lineHasText = false;
    QString marker; // a list item's number or bullet, written before its first word
    // Each open list: how many items it has had, -1 for a bulleted one. A
    // list that is all an item holds (FreeDict groups synonyms so) runs on
    // that item's line: "1. try; attempt".
    struct List {
        int count;
        bool inlined;
    };
    QList<List> lists;
    auto inlined = [&] { return std::any_of(lists.cbegin(), lists.cend(), [](const List &l) { return l.inlined; }); };
    // Right after a link, with nothing but space since: a link that follows
    // is the next of a list of them, as related words often come.
    bool afterLink = false;
    auto newline = [&] {
        afterLink = false;
        if (lineHasText && !inlined()) {
            out += '\n';
            lineHasText = false;
        }
    };
    auto put = [&](QChar c) {
        if (!c.isSpace())
            afterLink = false;
        if (c == '\n' && !html) {
            out += '\n';
            lineHasText = false;
            return;
        }
        if (c.isSpace()) {
            if (lineHasText && !out.endsWith(' '))
                out += ' ';
            return;
        }
        if (!marker.isEmpty()) {
            out += marker;
            marker.clear();
        }
        out += c;
        lineHasText = true;
    };

    const qsizetype size = markup.size();
    for (qsizetype i = 0; i < size; ++i) {
        const QChar c = markup.at(i);
        if (c == '&') {
            const qsizetype semicolon = markup.indexOf(';', i);
            if (semicolon > i + 1 && semicolon - i <= 10) {
                const QString name = markup.mid(i + 1, semicolon - i - 1);
                QString decoded;
                if (name.startsWith('#')) {
                    bool ok = false;
                    const uint code = name.startsWith("#x", Qt::CaseInsensitive) ? name.mid(2).toUInt(&ok, 16)
                                                                                  : name.mid(1).toUInt(&ok);
                    if (ok && code > 0 && code <= 0x10FFFF)
                        decoded = QString::fromUcs4(reinterpret_cast<const char32_t *>(&code), 1);
                } else {
                    decoded = entities.value(name);
                }
                if (!decoded.isEmpty()) {
                    for (const QChar d : std::as_const(decoded))
                        put(d);
                    i = semicolon;
                    continue;
                }
            }
            put(c);
            continue;
        }
        if (c != '<') {
            put(c);
            continue;
        }
        if (markup.mid(i, 4) == "<!--") {
            const qsizetype close = markup.indexOf("-->", i + 4);
            i = close < 0 ? size : close + 2;
            continue;
        }
        const qsizetype close = markup.indexOf('>', i);
        if (close < 0) {
            put(c);
            continue;
        }
        const QString tag = markup.mid(i + 1, close - i - 1).trimmed();
        i = close;
        const bool closing = tag.startsWith('/');
        qsizetype nameEnd = closing ? 1 : 0;
        while (nameEnd < tag.size() && (tag.at(nameEnd).isLetterOrNumber()))
            ++nameEnd;
        const QString name = tag.mid(closing ? 1 : 0, nameEnd - (closing ? 1 : 0)).toLower();
        if (name == "script" || name == "style") {
            if (!closing) {
                const qsizetype end = markup.indexOf("</" + name, i, Qt::CaseInsensitive);
                const qsizetype after = end < 0 ? -1 : markup.indexOf('>', end);
                i = after < 0 ? size : after;
            }
            continue;
        }
        if (name == "br") {
            newline();
            continue;
        }
        if (name == "a") {
            if (closing) {
                afterLink = true;
            } else if (afterLink && lineHasText) {
                while (out.endsWith(' '))
                    out.chop(1);
                out += QString(" %1 ").arg(QChar(0x00B7));
            }
            continue;
        }
        if ((name == "ol" || name == "ul") && !closing) {
            const bool runOn = !marker.isEmpty() || inlined();
            newline();
            lists.append({name == "ol" ? 0 : -1, runOn});
            continue;
        }
        if ((name == "ol" || name == "ul") && closing) {
            if (!lists.isEmpty())
                lists.removeLast();
            newline();
            continue;
        }
        if (name == "li" && !closing) {
            if (!lists.isEmpty() && lists.last().inlined) {
                if (lists.last().count++ > 0 && lineHasText)
                    out += "; ";
                continue;
            }
            newline();
            qsizetype depth = 0;
            for (const List &list : std::as_const(lists))
                depth += list.inlined ? 0 : 1;
            const QString indent(qMax<qsizetype>(depth - 1, 0) * 3, ' ');
            if (!lists.isEmpty() && lists.last().count >= 0)
                marker = indent + QString::number(++lists.last().count) + ". ";
            else
                marker = indent + QString(QChar(0x2022)) + " ";
            continue;
        }
        if (blocks.contains(name) || (html && name == "tr"))
            newline();
    }
    return tidy(out, QString());
}

QStringList Dictionary::forms(const QString &word) {
    QString w = word;
    w.replace(QChar(0x2019), '\'').replace(QChar(0x2018), '\'');
    qsizetype from = 0, to = w.size();
    while (from < to && !w.at(from).isLetterOrNumber())
        ++from;
    while (to > from && !w.at(to - 1).isLetterOrNumber() && !w.at(to - 1).isMark())
        --to;
    w = w.mid(from, to - from);
    QStringList out;
    auto add = [&](const QString &form) {
        if (!form.isEmpty() && !out.contains(form, Qt::CaseInsensitive))
            out.append(form);
    };
    if (w.isEmpty())
        return out;
    add(w);
    QString base = w.toLower();
    if (base.endsWith("'s") && base.size() > 3) {
        base.chop(2);
        add(base);
    }
    const qsizetype n = base.size();
    auto ends = [&](const char *suffix) {
        const QString s = QString::fromUtf8(suffix);
        return base.endsWith(s) && n >= s.size() + 2;
    };
    auto stem = [&](qsizetype cut) { return base.left(n - cut); };
    // "running" to "runn" to "run".
    auto undoubled = [](const QString &s) {
        if (s.size() >= 3 && s.at(s.size() - 1) == s.at(s.size() - 2) && !QStringLiteral("aeiou").contains(s.back()))
            return s.chopped(1);
        return QString();
    };

    // English plurals, verb forms and comparatives.
    if (ends("ies"))
        add(stem(3) + "y");
    if (ends("ves")) {
        add(stem(3) + "f");
        add(stem(3) + "fe");
    }
    if (ends("s") && !ends("ss"))
        add(stem(1));
    if (ends("es"))
        add(stem(2));
    if (ends("ied"))
        add(stem(3) + "y");
    if (ends("ed")) {
        add(stem(1));
        add(stem(2));
        add(undoubled(stem(2)));
    }
    if (ends("ing")) {
        add(stem(3));
        add(undoubled(stem(3)));
        add(stem(3) + "e");
    }
    if (ends("ier"))
        add(stem(3) + "y");
    if (ends("iest"))
        add(stem(4) + "y");
    if (ends("er")) {
        add(stem(2));
        add(undoubled(stem(2)));
        add(stem(1));
    }
    if (ends("est")) {
        add(stem(3));
        add(undoubled(stem(3)));
        add(stem(2));
    }
    if (ends("ily"))
        add(stem(3) + "y");
    if (ends("ly"))
        add(stem(2));
    if (ends("men"))
        add(stem(3) + "man");
    if (ends("ren"))
        add(stem(3));

    // Portuguese, Spanish, French and Italian plurals and feminines.
    if (ends("ões") || ends("ães") || ends("ãos"))
        add(stem(3) + "ão");
    if (ends("ais"))
        add(stem(3) + "al");
    if (ends("éis"))
        add(stem(3) + "el");
    if (ends("eis"))
        add(stem(3) + "il");
    if (ends("óis"))
        add(stem(3) + "ol");
    if (ends("ns"))
        add(stem(2) + "m");
    if (ends("aux"))
        add(stem(3) + "al");
    if (ends("x"))
        add(stem(1));
    if (ends("as"))
        add(stem(2) + "o");
    if (ends("a"))
        add(stem(1) + "o");
    if (ends("i")) {
        add(stem(1) + "o");
        add(stem(1) + "e");
    }
    if (ends("e"))
        add(stem(1) + "a");
    return out;
}

// The shelf of installed dictionaries.

DictionaryShelf::DictionaryShelf(const QStringList &directories) : m_directories(directories) {}

static QString dataHome() {
    return qEnvironmentVariableIsEmpty("XDG_DATA_HOME") ? QDir::homePath() + "/.local/share"
                                                        : qEnvironmentVariable("XDG_DATA_HOME");
}

QString DictionaryShelf::installDirectory() {
    return dataHome() + "/omareader/dictionaries";
}

QStringList DictionaryShelf::defaultDirectories() {
    return {installDirectory(), dataHome() + "/stardict/dic", QDir::homePath() + "/.stardict/dic",
            "/usr/share/stardict/dic", "/usr/share/dictd"};
}

QList<std::shared_ptr<Dictionary>> DictionaryShelf::dictionaries() {
    QStringList found;
    for (const QString &directory : std::as_const(m_directories)) {
        if (!QFileInfo(directory).isDir())
            continue;
        QStringList here;
        QDirIterator it(directory, {"*.ifo", "*.index"}, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
        while (it.hasNext())
            here.append(it.next());
        here.sort();
        found += here;
    }
    QMutexLocker lock(&m_mutex);
    QList<std::shared_ptr<Dictionary>> dictionaries;
    QHash<QString, std::shared_ptr<Dictionary>> open;
    for (const QString &path : std::as_const(found)) {
        auto dictionary = m_open.value(path);
        if (!dictionary)
            dictionary = Dictionary::open(path);
        if (!dictionary)
            continue;
        open.insert(path, dictionary);
        dictionaries.append(dictionary);
    }
    m_open = open;
    return dictionaries;
}

DictionaryShelf::Result DictionaryShelf::lookup(const QString &word) {
    Result result;
    result.word = word;
    const auto shelf = dictionaries();
    result.dictionaries = int(shelf.size());
    const QStringList forms = Dictionary::forms(word);
    for (const auto &dictionary : shelf)
        for (const QString &form : forms) {
            const QList<DictionaryEntry> entries = dictionary->lookup(form);
            if (!entries.isEmpty()) {
                result.entries += entries;
                break;
            }
        }
    return result;
}

// FreeDict's catalog.

QString CatalogDictionary::title() const {
    return DictionaryCatalog::languageName(from()) + " → " + DictionaryCatalog::languageName(to());
}

namespace DictionaryCatalog {

QList<CatalogDictionary> all() {
    static const QList<CatalogDictionary> catalog = [] {
        QList<CatalogDictionary> list;
        const QString text = QString::fromUtf8(readFile(":/dictionaries.tsv"));
        for (const QString &line : text.split('\n')) {
            if (line.startsWith('#') || line.trimmed().isEmpty())
                continue;
            const QStringList f = line.split('\t');
            if (f.size() != 6)
                continue;
            list.append({f.at(0), f.at(1), f.at(2).toInt(), f.at(3).toLongLong(), f.at(4), f.at(5)});
        }
        return list;
    }();
    return catalog;
}

QString languageCode(const QString &language) {
    const QString code = language.section('-', 0, 0).section('_', 0, 0).trimmed().toLower();
    if (code.size() == 3)
        return code;
    if (code.size() != 2)
        return {};
    const QLocale::Language l = QLocale::codeToLanguage(code, QLocale::ISO639Part1);
    return l == QLocale::AnyLanguage ? QString() : QLocale::languageToCode(l, QLocale::ISO639Part3);
}

QString languageName(const QString &code) {
    const QLocale::Language l = QLocale::codeToLanguage(code, QLocale::ISO639Part3);
    return l == QLocale::AnyLanguage ? code : QLocale::languageToString(l);
}

CatalogDictionary find(const QString &name) {
    const QStringList pair = name.trimmed().toLower().split('-');
    if (pair.size() != 2)
        return {};
    const QString wanted = languageCode(pair.at(0)) + "-" + languageCode(pair.at(1));
    for (const CatalogDictionary &d : all())
        if (d.name == wanted)
            return d;
    return {};
}

}

// The reader's lookups.

WordLookup::WordLookup(const QStringList &directories, QObject *parent)
    : QObject(parent), m_shelf(std::make_shared<DictionaryShelf>(directories)) {
    connect(&m_watcher, &QFutureWatcher<DictionaryShelf::Result>::finished, this, [this] {
        const DictionaryShelf::Result result = m_watcher.result();
        if (!m_busy || result.word != m_word)
            return;
        m_busy = false;
        m_dictionaryCount = result.dictionaries;
        m_entries.clear();
        QString previous;
        for (const DictionaryEntry &entry : result.entries) {
            m_entries.append(QVariantMap{{"dictionary", entry.dictionary},
                                         {"headword", entry.headword},
                                         {"text", entry.text},
                                         {"firstOfDictionary", entry.dictionary != previous}});
            previous = entry.dictionary;
        }
        emit changed();
    });
}

void WordLookup::lookUp(const QString &word) {
    const QString trimmed = word.trimmed();
    if (trimmed.isEmpty()) {
        clear();
        return;
    }
    m_word = trimmed;
    m_busy = true;
    m_entries.clear();
    emit changed();
    const auto shelf = m_shelf;
    m_watcher.setFuture(QtConcurrent::run([shelf, trimmed] { return shelf->lookup(trimmed); }));
}

void WordLookup::clear() {
    m_word.clear();
    m_busy = false;
    m_entries.clear();
    emit changed();
}

QVariantMap WordLookup::wordAt(const QString &text, int offset) {
    const QList<WordSpan> spans = Text::words(text, 0, int(text.size()));
    const WordSpan *found = nullptr;
    for (const WordSpan &span : spans) {
        if (offset >= span.start && offset < span.start + span.length) {
            found = &span;
            break;
        }
        // A click on a word's last letter can land just past it.
        if (offset == span.start + span.length)
            found = &span;
    }
    if (!found)
        return {};
    return {{"start", found->start}, {"length", found->length}, {"text", text.mid(found->start, found->length)}};
}

QString WordLookup::setupCommand(const QString &language) {
    const QString code = DictionaryCatalog::languageCode(language);
    if (!code.isEmpty() && code != "eng" && !DictionaryCatalog::find("eng-" + code).name.isEmpty())
        return "omareader setup --dictionary eng-" + code;
    return "omareader dictionaries --available";
}

#include "moc_dictionary.cpp"
