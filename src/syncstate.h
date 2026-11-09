#pragma once

#include <QByteArray>
#include <QHash>
#include <QIODevice>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

// What sync keeps in the user's Dropbox (docs/sync.md): one state file per
// book and one settings file, each made of registers that every device can
// change. Merging keeps the latest change of each register, so two devices
// never have to agree on anything else. Nothing here touches the network.

// A value, when it was set (ms since 1970 UTC) and by which device.
struct Register {
    QJsonValue value;
    qint64 at = 0;
    QString by;

    bool isSet() const { return !value.isUndefined() && !value.isNull(); }
    // True when this change is newer than other's: the later time, then the
    // larger device id. An unset register loses to any set one.
    bool beats(const Register &other) const;
    bool sameChange(const Register &other) const { return at == other.at && by == other.by && value == other.value; }

    QJsonObject toJson() const;
    // The register in an object of the form {"value": ..., "at": ..., "by": ...},
    // or for a position, every field but at and by.
    static Register fromJson(const QJsonValue &json, bool wholeObject = false);
    static Register newer(const Register &a, const Register &b) { return b.beats(a) ? b : a; }
};

// Where reading stopped, in the book's own terms.
struct SyncPosition {
    int chapter = 0;
    int paragraph = 0;
    int offset = 0;
    double progress = 0;

    QJsonObject toJson() const;
    static SyncPosition fromJson(const QJsonObject &json);
};

struct BookState {
    QString id;
    QString title;
    QString author;
    Register present;  // value: bool
    Register position; // value: the SyncPosition object

    bool isPresent() const { return present.isSet() && present.value.toBool(); }
    bool isRemoved() const { return present.isSet() && !present.value.toBool(); }
    bool hasPosition() const { return position.isSet() && position.value.isObject(); }
    SyncPosition syncPosition() const { return SyncPosition::fromJson(position.value.toObject()); }

    QByteArray toJson() const;
    // False for anything that isn't a state file this version can read.
    static bool fromJson(const QByteArray &json, BookState *state);
    static BookState merge(const BookState &a, const BookState &b);
    bool operator==(const BookState &other) const;
    bool operator!=(const BookState &other) const { return !(*this == other); }
};

// Settings that follow the user, by key; keys this version doesn't know are
// kept so a newer device's settings survive an older one's sync.
struct SyncSettings {
    QHash<QString, Register> values;

    Register value(const QString &key) const { return values.value(key); }
    QByteArray toJson() const;
    static bool fromJson(const QByteArray &json, SyncSettings *settings);
    static SyncSettings merge(const SyncSettings &a, const SyncSettings &b);
    bool operator==(const SyncSettings &other) const;
    bool operator!=(const SyncSettings &other) const { return !(*this == other); }

    static constexpr const char *Speed = "speed";
    static constexpr const char *DesktopTextSize = "textSize.desktop";
};

namespace SyncFormat {
constexpr int Version = 1;
// Dropbox's content_hash: SHA-256 of each 4 MiB block, then SHA-256 of those
// digests, in hex. Reads the device from where it is to the end.
QString contentHash(QIODevice *device);
QString contentHash(const QByteArray &data);
// JSON for the Dropbox-API-Arg header, which must stay ASCII: 0x7F and
// everything beyond it become \uXXXX escapes.
QByteArray headerJson(const QJsonObject &object);
// A book's id on every device: the first 16 hex digits of the SHA-1 of its
// title and author, so another file or export of it has the same id.
QString bookId(const QString &title, const QString &author);
// A random id for this install, 16 hex digits.
QString newDeviceId();
QString statePath(const QString &id);
QString bundlePath(const QString &id);
constexpr const char *SettingsPath = "/settings.json";
// The id in "/state/<id>.json" or "/books/<id>.omabook", or empty.
QString idFromPath(const QString &path, const QString &folder, const QString &suffix);
bool isValidId(const QString &id);
// The manifest of a .omabook (docs/omabook.md), which is always its first,
// stored entry; empty when the file isn't a bundle.
QJsonObject bundleManifest(const QString &path);
}
