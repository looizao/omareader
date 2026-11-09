package io.github.looizao.omareader

import java.io.File
import java.io.IOException

// A file in the app folder, as Dropbox lists it.
data class RemoteFile(val path: String, val name: String, val rev: String, val size: Long, val contentHash: String?)

sealed class WriteMode {
    object Add : WriteMode()
    object Overwrite : WriteMode()
    data class Update(val rev: String) : WriteMode()
}

// Another device wrote the file since it was read.
class ConflictException : IOException("conflict")

// The token no longer works: the user disconnected the app in Dropbox, or
// it was revoked. Syncing again needs a new connection.
class RevokedException : IOException("revoked")

// Dropbox asked to come back later (rate limits, a server error).
class RetryLaterException(val seconds: Long) : IOException("retry in $seconds s")

// The app folder, as the engine needs it. DropboxRemote speaks to Dropbox;
// the tests use an in-memory one.
interface Remote {
    // Every file in the app folder, at any depth.
    fun list(): List<RemoteFile>

    // A small file's bytes and its metadata.
    fun read(path: String): Pair<ByteArray, RemoteFile>

    // Appends the file to out from byte `from`, for downloads that pick up
    // where they stopped. Returns the metadata of what was downloaded.
    fun download(path: String, out: File, from: Long, cancelled: () -> Boolean): RemoteFile

    // Writes a small file; throws ConflictException when mode is Update with
    // a rev that is no longer current, or Add over an existing file.
    fun write(path: String, bytes: ByteArray, mode: WriteMode): RemoteFile

    // Deletes a file; one that is already gone is not an error.
    fun delete(path: String)
}

// What a sync reads from and changes on this device.
interface SyncStore {
    val device: String

    // Ids of books this device has a record of: books on it, and books it
    // removed (whose removal it may still need to send).
    fun localIds(): Set<String>

    // This device's registers for a book, or null when it knows nothing.
    fun localState(id: String): BookState?

    // Takes the registers Dropbox won. What it can't take yet (the position
    // of a book playing now) it skips; the device's own change will be newer.
    fun applyState(merged: BookState)

    fun localSettings(): SyncSettings
    fun applySettings(merged: SyncSettings)

    // The last state file read from or written to Dropbox for a path, so an
    // unchanged file is not downloaded again.
    fun cached(path: String): Pair<String, String>? // rev to text
    fun cache(path: String, rev: String, text: String)
    fun forget(path: String)

    // The rev of the bundle a book was imported from, if it came from sync.
    fun bundleRev(id: String): String?

    // Where a bundle being downloaded is kept until it is complete.
    fun partialFile(id: String, rev: String): File

    // Imports a downloaded bundle, as if the user had opened it.
    fun importBundle(id: String, file: File, rev: String)
}

class SyncReport(
    var downloaded: Int = 0,
    var uploaded: Int = 0,
    var applied: Int = 0,
    var removed: Int = 0,
    // Bundles that would download but are waiting for an unmetered network.
    var waiting: Int = 0,
    var changedLibrary: Boolean = false,
)

// One sync, steps 2 to 6 of docs/sync.md. Step 1, uploading exports, is the
// desktop's.
class SyncEngine(private val remote: Remote, private val store: SyncStore) {

    fun sync(downloadBundles: Boolean, cancelled: () -> Boolean = { false }): SyncReport {
        val report = SyncReport()
        val files = remote.list()
        val states = HashMap<String, RemoteFile>()
        val bundles = HashMap<String, RemoteFile>()
        var settingsFile: RemoteFile? = null
        for (file in files) {
            val lower = file.path.lowercase()
            when {
                lower == SETTINGS -> settingsFile = file
                lower.startsWith("/state/") -> stateId(file.name)?.let { states[it] = file }
                lower.startsWith("/books/") -> bundleId(file.name)?.let { bundles[it] = file }
            }
        }

        val merged = HashMap<String, BookState>()
        for (id in (store.localIds() + states.keys).toSortedSet()) {
            if (cancelled()) return report
            mergeBook(id, states[id], report)?.let { merged[id] = it }
        }
        mergeSettings(settingsFile)

        for ((id, bundle) in bundles) {
            if (merged[id]?.present?.value == false) {
                remote.delete(bundle.path)
                store.forget(bundle.path)
                report.removed++
            }
        }

        for ((id, bundle) in bundles.toSortedMap()) {
            if (merged[id]?.present?.value != true || store.bundleRev(id) == bundle.rev) continue
            if (!downloadBundles) {
                report.waiting++
                continue
            }
            if (cancelled()) return report
            download(id, bundle, cancelled)
            report.downloaded++
            report.changedLibrary = true
        }
        return report
    }

    // Merges one book's registers both ways, retrying when another device
    // writes the same file at the same time. Returns the merged record, or
    // null when the file in Dropbox can't be read by this version.
    private fun mergeBook(id: String, file: RemoteFile?, report: SyncReport): BookState? {
        val path = "/state/$id.json"
        var current = file
        repeat(ATTEMPTS) {
            val remoteState = try {
                current?.let { readState(it) }
            } catch (e: SyncFormatException) {
                return null
            }
            val local = store.localState(id)
            val merged = (local ?: BookState(id)).mergedWith(remoteState)
            // Compared by change rather than value: a position converted to
            // this phone's terms and back may name the word a little
            // differently and still be the same change.
            val changed = (merged.present != null && !merged.present.sameChange(local?.present)) ||
                (merged.position != null && !merged.position.sameChange(local?.position))
            if (changed) {
                store.applyState(merged)
                report.applied++
                report.changedLibrary = true
            }
            if (!merged.differsFrom(remoteState)) return merged
            val text = merged.toJson().toString()
            val mode = current?.let { WriteMode.Update(it.rev) } ?: WriteMode.Add
            try {
                val written = remote.write(path, text.toByteArray(), mode)
                store.cache(path, written.rev, text)
                report.uploaded++
                return merged
            } catch (e: ConflictException) {
                // Someone wrote in between: read theirs and merge again.
                current = remote.list().firstOrNull { it.path.equals(path, ignoreCase = true) }
            }
        }
        throw ConflictException()
    }

    private fun readState(file: RemoteFile): BookState {
        val cached = store.cached(file.path.lowercase())
        val text = if (cached != null && cached.first == file.rev) {
            cached.second
        } else {
            val (bytes, meta) = remote.read(file.path)
            String(bytes, Charsets.UTF_8).also { store.cache(file.path.lowercase(), meta.rev, it) }
        }
        return BookState.parse(text)
    }

    private fun mergeSettings(file: RemoteFile?) {
        var current = file
        repeat(ATTEMPTS) {
            val remoteSettings = try {
                current?.let { f ->
                    val cached = store.cached(SETTINGS)
                    val text = if (cached != null && cached.first == f.rev) {
                        cached.second
                    } else {
                        val (bytes, meta) = remote.read(f.path)
                        String(bytes, Charsets.UTF_8).also { store.cache(SETTINGS, meta.rev, it) }
                    }
                    SyncSettings.parse(text)
                }
            } catch (e: SyncFormatException) {
                return
            }
            val merged = store.localSettings().mergedWith(remoteSettings)
            store.applySettings(merged)
            if (!merged.differsFrom(remoteSettings)) return
            val text = merged.toJson().toString()
            try {
                val written = remote.write(SETTINGS, text.toByteArray(), current?.let { WriteMode.Update(it.rev) } ?: WriteMode.Add)
                store.cache(SETTINGS, written.rev, text)
                return
            } catch (e: ConflictException) {
                current = remote.list().firstOrNull { it.path.equals(SETTINGS, ignoreCase = true) }
            }
        }
        throw ConflictException()
    }

    // Downloads into a partial file that survives an interrupted job, checks
    // it against Dropbox's hash, and imports it.
    private fun download(id: String, bundle: RemoteFile, cancelled: () -> Boolean) {
        val partial = store.partialFile(id, bundle.rev)
        partial.parentFile?.mkdirs()
        // Leftovers of older revisions of this book are useless now.
        partial.parentFile?.listFiles()?.filter { it.name.startsWith("$id-") && it != partial }?.forEach { it.delete() }
        if (partial.length() > bundle.size) partial.delete()
        if (partial.length() < bundle.size) remote.download(bundle.path, partial, partial.length(), cancelled)
        if (cancelled()) throw InterruptedException()
        val expected = bundle.contentHash
        if (partial.length() != bundle.size || (expected != null && ContentHash.of(partial) != expected)) {
            partial.delete()
            throw IOException("The download of $id did not match Dropbox's copy")
        }
        try {
            store.importBundle(id, partial, bundle.rev)
        } finally {
            partial.delete()
        }
    }

    companion object {
        const val SETTINGS = "/settings.json"
        private const val ATTEMPTS = 3
        private val ID = Regex("""[A-Za-z0-9_-]{1,64}""")

        fun stateId(name: String) = name.removeSuffix(".json").takeIf { name.endsWith(".json") && ID.matches(it) }
        fun bundleId(name: String) = name.removeSuffix(".omabook").takeIf { name.endsWith(".omabook") && ID.matches(it) }
    }
}
