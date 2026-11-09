#include "syncstate.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QtEndian>
#include <cmath>

bool Register::beats(const Register &other) const {
    if (!isSet())
        return false;
    if (!other.isSet())
        return true;
    if (at != other.at)
        return at > other.at;
    return by > other.by;
}

QJsonObject Register::toJson() const {
    if (value.isObject()) {
        QJsonObject object = value.toObject();
        object.insert("at", at);
        object.insert("by", by);
        return object;
    }
    return QJsonObject{{"value", value}, {"at", at}, {"by", by}};
}

Register Register::fromJson(const QJsonValue &json, bool wholeObject) {
    Register r;
    if (!json.isObject())
        return r;
    QJsonObject object = json.toObject();
    r.at = object.value("at").toInteger();
    r.by = object.value("by").toString();
    if (wholeObject) {
        object.remove("at");
        object.remove("by");
        r.value = object;
    } else {
        r.value = object.value("value");
    }
    return r;
}

QJsonObject SyncPosition::toJson() const {
    // Four decimals is a word in a long book; more only adds noise to diffs.
    return QJsonObject{{"chapter", chapter},
                       {"paragraph", paragraph},
                       {"offset", offset},
                       {"progress", std::round(qBound(0.0, progress, 1.0) * 10000) / 10000}};
}

SyncPosition SyncPosition::fromJson(const QJsonObject &json) {
    SyncPosition p;
    p.chapter = qMax(0, json.value("chapter").toInt());
    p.paragraph = qMax(0, json.value("paragraph").toInt());
    p.offset = qMax(0, json.value("offset").toInt());
    p.progress = qBound(0.0, json.value("progress").toDouble(), 1.0);
    return p;
}

QByteArray BookState::toJson() const {
    QJsonObject object{{"format", "omareader-state"},
                       {"version", SyncFormat::Version},
                       {"id", id},
                       {"title", title},
                       {"author", author}};
    if (present.isSet())
        object.insert("present", present.toJson());
    if (hasPosition())
        object.insert("position", position.toJson());
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

bool BookState::fromJson(const QByteArray &json, BookState *state) {
    const QJsonObject object = QJsonDocument::fromJson(json).object();
    if (object.value("format").toString() != "omareader-state" || object.value("version").toInt() > SyncFormat::Version)
        return false;
    BookState s;
    s.id = object.value("id").toString();
    if (!SyncFormat::isValidId(s.id))
        return false;
    s.title = object.value("title").toString();
    s.author = object.value("author").toString();
    s.present = Register::fromJson(object.value("present"));
    if (s.present.isSet() && !s.present.value.isBool())
        s.present = {};
    s.position = Register::fromJson(object.value("position"), true);
    if (s.position.isSet() && s.position.value.toObject().isEmpty())
        s.position = {};
    *state = s;
    return true;
}

BookState BookState::merge(const BookState &a, const BookState &b) {
    BookState merged;
    merged.id = a.id.isEmpty() ? b.id : a.id;
    // Names come from whichever side has them; they are only for people
    // looking in the folder and for a device that lacks the book.
    merged.title = a.title.isEmpty() ? b.title : a.title;
    merged.author = a.author.isEmpty() ? b.author : a.author;
    merged.present = Register::newer(a.present, b.present);
    merged.position = Register::newer(a.position, b.position);
    return merged;
}

bool BookState::operator==(const BookState &other) const {
    return id == other.id && title == other.title && author == other.author
           && present.sameChange(other.present) && position.sameChange(other.position);
}

QByteArray SyncSettings::toJson() const {
    QJsonObject object{{"format", "omareader-settings"}, {"version", SyncFormat::Version}};
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        if (it.value().isSet())
            object.insert(it.key(), it.value().toJson());
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

bool SyncSettings::fromJson(const QByteArray &json, SyncSettings *settings) {
    const QJsonObject object = QJsonDocument::fromJson(json).object();
    if (object.value("format").toString() != "omareader-settings"
        || object.value("version").toInt() > SyncFormat::Version)
        return false;
    SyncSettings s;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.key() == "format" || it.key() == "version")
            continue;
        const Register r = Register::fromJson(it.value());
        if (r.isSet())
            s.values.insert(it.key(), r);
    }
    *settings = s;
    return true;
}

SyncSettings SyncSettings::merge(const SyncSettings &a, const SyncSettings &b) {
    SyncSettings merged = a;
    for (auto it = b.values.cbegin(); it != b.values.cend(); ++it)
        merged.values.insert(it.key(), Register::newer(a.values.value(it.key()), it.value()));
    return merged;
}

bool SyncSettings::operator==(const SyncSettings &other) const {
    if (values.size() != other.values.size())
        return false;
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        if (!other.values.contains(it.key()) || !it.value().sameChange(other.values.value(it.key())))
            return false;
    return true;
}

namespace SyncFormat {

QString contentHash(QIODevice *device) {
    constexpr qint64 Block = 4 * 1024 * 1024;
    QCryptographicHash overall(QCryptographicHash::Sha256);
    while (!device->atEnd()) {
        const QByteArray block = device->read(Block);
        if (block.isEmpty())
            break;
        overall.addData(QCryptographicHash::hash(block, QCryptographicHash::Sha256));
    }
    return QString::fromLatin1(overall.result().toHex());
}

QString contentHash(const QByteArray &data) {
    constexpr qsizetype Block = 4 * 1024 * 1024;
    QCryptographicHash overall(QCryptographicHash::Sha256);
    for (qsizetype i = 0; i < data.size(); i += Block)
        overall.addData(QCryptographicHash::hash(data.mid(i, Block), QCryptographicHash::Sha256));
    return QString::fromLatin1(overall.result().toHex());
}

QByteArray headerJson(const QJsonObject &object) {
    const QString json = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    QByteArray out;
    out.reserve(json.size());
    for (const QChar c : json) {
        const ushort u = c.unicode();
        if (u < 0x7F)
            out.append(char(u));
        else
            out.append(QStringLiteral("\\u%1").arg(u, 4, 16, QChar('0')).toLatin1());
    }
    return out;
}

QString bookId(const QString &title, const QString &author) {
    const QByteArray key = (title.simplified() + '\n' + author.simplified()).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(16));
}

QString newDeviceId() {
    const quint64 value = QRandomGenerator::system()->generate64();
    return QStringLiteral("%1").arg(value, 16, 16, QChar('0'));
}

QString statePath(const QString &id) { return "/state/" + id + ".json"; }
QString bundlePath(const QString &id) { return "/books/" + id + ".omabook"; }

bool isValidId(const QString &id) {
    static const QRegularExpression pattern("^[A-Za-z0-9_-]{1,64}$");
    return pattern.match(id).hasMatch();
}

QJsonObject bundleManifest(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    uchar header[30];
    if (file.read(reinterpret_cast<char *>(header), sizeof header) != qint64(sizeof header)
        || qFromLittleEndian<quint32>(header) != 0x04034b50 || qFromLittleEndian<quint16>(header + 8) != 0)
        return {};
    const quint32 size = qFromLittleEndian<quint32>(header + 18);
    const quint16 nameLength = qFromLittleEndian<quint16>(header + 26);
    const quint16 extraLength = qFromLittleEndian<quint16>(header + 28);
    if (file.read(nameLength) != "manifest.json" || size > 16 * 1024 * 1024)
        return {};
    file.skip(extraLength);
    const QJsonObject manifest = QJsonDocument::fromJson(file.read(size)).object();
    return manifest.value("format").toString() == "omabook" && isValidId(manifest.value("id").toString())
               ? manifest
               : QJsonObject{};
}

QString idFromPath(const QString &path, const QString &folder, const QString &suffix) {
    // Dropbox paths are case-insensitive and list_folder reports them in
    // lower case, which is how ids are written anyway.
    const QString prefix = "/" + folder + "/";
    if (!path.startsWith(prefix, Qt::CaseInsensitive) || !path.endsWith(suffix, Qt::CaseInsensitive))
        return {};
    const QString id = path.mid(prefix.size(), path.size() - prefix.size() - suffix.size());
    return isValidId(id) ? id : QString();
}

}
