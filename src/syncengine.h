#pragma once

#include "syncstate.h"

#include <QHash>
#include <QList>
#include <QString>

// The files sync reads and writes in the app's Dropbox folder, behind an
// interface so the engine can be tested against a folder kept in memory.
// Every call blocks; the engine runs on a worker thread.
struct RemoteFile {
    QString path; // lower case, from the folder's root, such as "/state/abc.json"
    QString rev;
    QString contentHash;
    qint64 size = 0;
};

class SyncRemote {
public:
    enum Status { Ok, Conflict, NotFound, Failed };

    virtual ~SyncRemote() = default;
    virtual Status list(QList<RemoteFile> *files) = 0;
    virtual Status download(const QString &path, QByteArray *data, QString *rev) = 0;
    // Writes a small file. An empty rev adds it and fails with Conflict if
    // something is already there; otherwise it replaces exactly that rev and
    // fails with Conflict if anything else is there, even nothing.
    virtual Status upload(const QString &path, const QByteArray &data, const QString &rev, QString *newRev) = 0;
    // Writes a local file of any size, replacing whatever is there.
    virtual Status uploadFile(const QString &path, const QString &localFile, QString *newRev) = 0;
    // NotFound when there was nothing to delete.
    virtual Status remove(const QString &path) = 0;
    virtual QString error() const = 0;
};

// The state file last read for a book: its rev, and what it held.
struct SyncCacheEntry {
    QString rev;
    BookState state;
};

// An export waiting to go to the phone.
struct PendingUpload {
    QString id;
    QString title;
    QString author;
    QString file;
};

struct SyncInput {
    QString device;
    qint64 now = 0;
    // The device's own view of every book it has a register for.
    QHash<QString, BookState> local;
    SyncSettings settings;
    // What was in Dropbox at the last sync.
    QHash<QString, SyncCacheEntry> cache;
    QString settingsRev;
    SyncSettings settingsCache;
    QList<PendingUpload> uploads;
};

struct SyncOutput {
    bool ok = true;
    QString error;
    // Every book after merging with Dropbox, and the settings likewise.
    QHash<QString, BookState> merged;
    SyncSettings settings;
    QHash<QString, SyncCacheEntry> cache;
    QString settingsRev;
    SyncSettings settingsCache;
    // Exports that went up, or whose file is gone; the rest stay queued.
    QStringList uploadsDone;
    int uploadedBundles = 0;
    int writtenFiles = 0;
    int removedBundles = 0;
};

namespace SyncEngine {
// One sync, steps 1 to 5 of docs/sync.md. Whatever fails is reported and left
// for the next sync; what succeeded is in the output either way.
SyncOutput run(const SyncInput &input, SyncRemote &remote);
}
