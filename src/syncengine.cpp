#include "syncengine.h"

#include <QFileInfo>
#include <QSet>

namespace {

constexpr int Attempts = 3;

void addError(SyncOutput *out, const QString &error) {
    out->ok = false;
    if (out->error.isEmpty())
        out->error = error;
}

// Reads the state file at its current rev, using what was read last time when
// the rev hasn't changed. False when it can't be read or is from a newer
// version, in which case the book is left alone.
bool readState(SyncRemote &remote, const QString &id, const RemoteFile *file, const SyncCacheEntry &cached,
               BookState *state, QString *rev, bool *exists) {
    *exists = file != nullptr;
    *state = BookState{};
    rev->clear();
    if (!file)
        return true;
    if (!cached.rev.isEmpty() && cached.rev == file->rev) {
        *state = cached.state;
        *rev = file->rev;
        return true;
    }
    QByteArray data;
    const SyncRemote::Status status = remote.download(SyncFormat::statePath(id), &data, rev);
    if (status == SyncRemote::NotFound) {
        *exists = false;
        rev->clear();
        return true;
    }
    if (status != SyncRemote::Ok)
        return false;
    return BookState::fromJson(data, state) && state->id == id;
}

}

namespace SyncEngine {

SyncOutput run(const SyncInput &input, SyncRemote &remote) {
    SyncOutput out;
    out.cache = input.cache;
    out.settingsRev = input.settingsRev;
    out.settingsCache = input.settingsCache;
    out.settings = input.settings;
    QHash<QString, BookState> local = input.local;

    // 1. Exports waiting for the phone. Once one is up, the book is in the
    // library again, even if a device removed it before.
    for (const PendingUpload &upload : input.uploads) {
        if (!QFileInfo::exists(upload.file)) {
            out.uploadsDone.append(upload.id);
            continue;
        }
        QString rev;
        if (remote.uploadFile(SyncFormat::bundlePath(upload.id), upload.file, &rev) != SyncRemote::Ok) {
            addError(&out, remote.error());
            continue;
        }
        out.uploadsDone.append(upload.id);
        ++out.uploadedBundles;
        BookState &state = local[upload.id];
        state.id = upload.id;
        if (state.title.isEmpty())
            state.title = upload.title;
        if (state.author.isEmpty())
            state.author = upload.author;
        state.present = Register::newer(state.present, Register{true, input.now, input.device});
    }

    // 2. What is in the folder now.
    QList<RemoteFile> files;
    if (remote.list(&files) != SyncRemote::Ok) {
        addError(&out, remote.error());
        out.merged = local;
        return out;
    }
    QHash<QString, RemoteFile> states, bundles;
    const RemoteFile *settingsFile = nullptr;
    for (const RemoteFile &file : std::as_const(files)) {
        QString id = SyncFormat::idFromPath(file.path, "state", ".json");
        if (!id.isEmpty()) {
            states.insert(id, file);
            continue;
        }
        id = SyncFormat::idFromPath(file.path, "books", ".omabook");
        if (!id.isEmpty())
            bundles.insert(id, file);
        else if (file.path.compare(SyncFormat::SettingsPath, Qt::CaseInsensitive) == 0)
            settingsFile = &file;
    }

    // 3. Each book: merge with Dropbox and write back what Dropbox lacks.
    QSet<QString> ids;
    for (auto it = local.cbegin(); it != local.cend(); ++it)
        ids.insert(it.key());
    for (auto it = states.cbegin(); it != states.cend(); ++it)
        ids.insert(it.key());
    for (const QString &id : std::as_const(ids)) {
        const BookState mine = local.value(id);
        BookState merged;
        bool settled = false, unreadable = false;
        for (int attempt = 0; attempt < Attempts && !settled && !unreadable; ++attempt) {
            const RemoteFile *file = states.contains(id) ? &states[id] : nullptr;
            BookState theirs;
            QString rev;
            bool exists = false;
            if (!readState(remote, id, file, out.cache.value(id), &theirs, &rev, &exists)) {
                unreadable = true;
                break;
            }
            merged = exists ? BookState::merge(mine, theirs) : mine;
            merged.id = id;
            if (exists && merged == theirs) {
                out.cache.insert(id, {rev, merged});
                settled = true;
                break;
            }
            if (!merged.present.isSet() && !merged.hasPosition()) {
                settled = true; // nothing worth writing
                break;
            }
            QString newRev;
            const SyncRemote::Status status =
                remote.upload(SyncFormat::statePath(id), merged.toJson(), exists ? rev : QString(), &newRev);
            if (status == SyncRemote::Ok) {
                out.cache.insert(id, {newRev, merged});
                states.insert(id, RemoteFile{SyncFormat::statePath(id), newRev, {}, 0});
                ++out.writtenFiles;
                settled = true;
            } else if (status == SyncRemote::Conflict) {
                // Another device wrote in between: read its version and merge
                // again. Forgetting the cached rev forces the download.
                out.cache.remove(id);
                states.insert(id, RemoteFile{SyncFormat::statePath(id), QStringLiteral("?"), {}, 0});
            } else {
                addError(&out, remote.error());
                break;
            }
        }
        if (unreadable) {
            if (!remote.error().isEmpty())
                addError(&out, remote.error());
            // A file from a newer version, or one that couldn't be read: keep
            // the local view and leave Dropbox's alone.
            out.merged.insert(id, mine);
            continue;
        }
        out.merged.insert(id, settled ? merged : mine);
        if (!settled && out.error.isEmpty())
            addError(&out, QStringLiteral("Dropbox kept changing %1").arg(SyncFormat::statePath(id)));
    }

    // 4. The settings, the same way.
    RemoteFile changedSettings;
    for (int attempt = 0; attempt < Attempts; ++attempt) {
        SyncSettings theirs;
        QString rev;
        bool exists = settingsFile != nullptr;
        if (exists && settingsFile->rev == out.settingsRev && !out.settingsRev.isEmpty()) {
            theirs = out.settingsCache;
            rev = out.settingsRev;
        } else if (exists) {
            QByteArray data;
            const SyncRemote::Status status = remote.download(SyncFormat::SettingsPath, &data, &rev);
            if (status == SyncRemote::NotFound) {
                exists = false;
            } else if (status != SyncRemote::Ok) {
                addError(&out, remote.error());
                break;
            } else if (!SyncSettings::fromJson(data, &theirs)) {
                break; // a newer version's settings; leave them alone
            }
        }
        const SyncSettings merged = exists ? SyncSettings::merge(input.settings, theirs) : input.settings;
        out.settings = merged;
        if ((exists && merged == theirs) || (!exists && merged.values.isEmpty())) {
            out.settingsRev = rev;
            out.settingsCache = merged;
            break;
        }
        QString newRev;
        const SyncRemote::Status status =
            remote.upload(SyncFormat::SettingsPath, merged.toJson(), exists ? rev : QString(), &newRev);
        if (status == SyncRemote::Ok) {
            out.settingsRev = newRev;
            out.settingsCache = merged;
            ++out.writtenFiles;
            break;
        }
        if (status != SyncRemote::Conflict) {
            addError(&out, remote.error());
            break;
        }
        changedSettings = RemoteFile{SyncFormat::SettingsPath, QStringLiteral("?"), {}, 0};
        settingsFile = &changedSettings;
        out.settingsRev.clear();
    }

    // 5. Bundles of books nobody has any more.
    for (auto it = bundles.cbegin(); it != bundles.cend(); ++it) {
        if (!out.merged.value(it.key()).isRemoved())
            continue;
        const SyncRemote::Status status = remote.remove(it.value().path);
        if (status == SyncRemote::Ok)
            ++out.removedBundles;
        else if (status != SyncRemote::NotFound)
            addError(&out, remote.error());
    }
    return out;
}

}
