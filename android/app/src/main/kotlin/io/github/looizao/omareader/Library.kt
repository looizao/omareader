package io.github.looizao.omareader

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import java.io.File
import java.io.InputStream
import java.security.SecureRandom
import java.util.concurrent.Executors

// The books on this phone and where each was left. Books live in the app's
// private storage (one folder per book, named by its id); positions and the
// order they were opened in are small SharedPreferences entries.
//
// What syncs (docs/sync.md) is kept as registers: each value with when it
// was set and by which device. A change made here is stamped with the time
// and this phone's id, but only when the value really changes, since the
// player saves its place every few seconds. A change taken from Dropbox
// keeps the stamp it came with.
class Library(context: Context) {
    private val app = context.applicationContext
    private val importer = BundleImporter(File(app.filesDir, "books"))
    private val prefs = app.getSharedPreferences("library", Context.MODE_PRIVATE)
    private val reader = app.getSharedPreferences("reader", Context.MODE_PRIVATE)

    // This install's id in sync records, made once.
    val device: String
        get() = synchronized(LOCK) {
            reader.getString(DEVICE, null) ?: (1..16).map { "0123456789abcdef"[RANDOM.nextInt(16)] }
                .joinToString("")
                .also { reader.edit().putString(DEVICE, it).apply() }
        }

    // Most recently opened first, then newest import.
    fun books(): List<StoredBook> =
        importer.books().sortedWith(
            compareByDescending<StoredBook> { prefs.getLong(OPENED + it.id, 0) }
                .thenByDescending { it.dir.lastModified() },
        )

    fun book(id: String?): StoredBook? {
        if (id.isNullOrEmpty() || !ID.matches(id)) return null
        val dir = File(File(app.filesDir, "books"), id)
        return if (dir.isDirectory) runCatching { BundleImporter.readBook(dir) }.getOrNull() else null
    }

    fun position(id: String) = Position.decode(prefs.getString(POSITION + id, null))

    // The place the player or the reader reached.
    fun savePosition(id: String, position: Position) {
        if (prefs.getString(POSITION + id, null) == position.encode()) return
        setPosition(id, Register(position, System.currentTimeMillis(), device))
        SyncScheduler.soon(app, SyncScheduler.AFTER_CHANGE_MS)
    }

    fun positionRegister(id: String): Register<Position>? {
        val saved = prefs.getString(POSITION + id, null) ?: return null
        // A place saved before sync existed counts as set when the book was
        // last opened.
        val at = prefs.getLong(POSITION_AT + id, prefs.getLong(OPENED + id, 0))
        return Register(Position.decode(saved), at, prefs.getString(POSITION_BY + id, null) ?: device)
    }

    fun setPosition(id: String, register: Register<Position>) {
        prefs.edit()
            .putString(POSITION + id, register.value.encode())
            .putLong(POSITION_AT + id, register.at)
            .putString(POSITION_BY + id, register.by)
            .apply()
    }

    // Whether the book belongs in the library: true once added, false once
    // removed, kept after removal so the removal reaches other devices.
    fun presentRegister(id: String): Register<Boolean>? {
        if (!prefs.contains(PRESENT + id)) {
            val book = book(id) ?: return null
            // Books added before sync existed count as added when imported.
            return Register(true, book.dir.lastModified(), device)
        }
        return Register(
            prefs.getBoolean(PRESENT + id, true),
            prefs.getLong(PRESENT_AT + id, 0),
            prefs.getString(PRESENT_BY + id, null) ?: device,
        )
    }

    fun setPresent(id: String, register: Register<Boolean>) {
        prefs.edit()
            .putBoolean(PRESENT + id, register.value)
            .putLong(PRESENT_AT + id, register.at)
            .putString(PRESENT_BY + id, register.by)
            .apply()
    }

    // Every book this phone has a record of, including removed ones.
    fun knownIds(): Set<String> =
        importer.books().map { it.id }.toSet() +
            prefs.all.keys.filter { it.startsWith(PRESENT) }.map { it.removePrefix(PRESENT) }.filter { ID.matches(it) }

    // The title and author of a book, kept after it is removed so its record
    // in Dropbox still says which book it was.
    fun name(id: String): Pair<String, String> {
        book(id)?.let { return it.manifest.title to it.manifest.author }
        val saved = prefs.getString(NAME + id, null) ?: return "" to ""
        return saved.substringBefore('\n') to saved.substringAfter('\n', "")
    }

    fun markOpened(id: String) {
        prefs.edit().putLong(OPENED + id, System.currentTimeMillis()).apply()
    }

    // The listening speed is one setting for every book, kept with the
    // reader's other settings; the car plays at the speed chosen on the
    // phone. It is how fast the user listens relative to the voice's natural
    // pace, so the player divides it by the speed the book was exported at.
    fun speed(): Float = reader.getFloat(SPEED, 1f).coerceIn(MIN_SPEED, MAX_SPEED)

    fun playbackSpeed(book: StoredBook): Float =
        (speed() / book.manifest.voiceSpeed.toFloat()).coerceIn(0.25f, 4f)

    fun setSpeed(speed: Float) {
        if (reader.getFloat(SPEED, 1f) == speed && reader.contains(SPEED_AT)) return
        setSpeed(Register(speed.toDouble(), System.currentTimeMillis(), device))
        SyncScheduler.soon(app, SyncScheduler.AFTER_CHANGE_MS)
    }

    fun speedRegister(): Register<Double>? {
        if (!reader.contains(SPEED)) return null
        return Register(speed().toDouble(), reader.getLong(SPEED_AT, 0), reader.getString(SPEED_BY, null) ?: device)
    }

    fun setSpeed(register: Register<Double>) {
        reader.edit()
            .putFloat(SPEED, register.value.toFloat().coerceIn(MIN_SPEED, MAX_SPEED))
            .putLong(SPEED_AT, register.at)
            .putString(SPEED_BY, register.by)
            .apply()
    }

    // The reader's text size, as an index into TEXT_SIZES.
    fun textSize(): Int = reader.getInt(TEXT_SIZE, 2).coerceIn(TEXT_SIZES.indices)

    fun setTextSize(index: Int) {
        if (reader.getInt(TEXT_SIZE, 2) == index && reader.contains(TEXT_SIZE_AT)) return
        setTextSize(Register(TEXT_SIZES[index].toDouble(), System.currentTimeMillis(), device))
        SyncScheduler.soon(app, SyncScheduler.AFTER_CHANGE_MS)
    }

    // Synced in sp, the nearest size this phone offers.
    fun textSizeRegister(): Register<Double>? {
        if (!reader.contains(TEXT_SIZE)) return null
        return Register(
            TEXT_SIZES[textSize()].toDouble(),
            reader.getLong(TEXT_SIZE_AT, 0),
            reader.getString(TEXT_SIZE_BY, null) ?: device,
        )
    }

    fun setTextSize(register: Register<Double>) {
        val index = TEXT_SIZES.indices.minByOrNull { kotlin.math.abs(TEXT_SIZES[it] - register.value) } ?: 2
        reader.edit()
            .putInt(TEXT_SIZE, index)
            .putLong(TEXT_SIZE_AT, register.at)
            .putString(TEXT_SIZE_BY, register.by)
            .apply()
    }

    // Copies the bundle in. A new export of a book already here replaces it
    // and keeps the place reached, moved onto the new export's timings.
    // Adding a book by hand puts it back in the library everywhere, even if
    // it was removed before.
    fun import(uri: Uri): StoredBook {
        val input = app.contentResolver.openInputStream(uri)
            ?: throw BundleException(BundleException.Reason.Unreadable)
        val book = input.use { importStream(it) }
        setPresent(book.id, Register(true, System.currentTimeMillis(), device))
        SyncScheduler.soon(app, SyncScheduler.AFTER_CHANGE_MS)
        return book
    }

    // A bundle sync downloaded. Its presence already came from Dropbox.
    fun importFile(file: File): StoredBook = file.inputStream().use { importStream(it) }

    private fun importStream(input: InputStream): StoredBook {
        val imported = importer.import(input)
        imported.previous?.let { old ->
            // The same place on the new timings is the same change, so it
            // keeps its stamp.
            positionRegister(old.id)?.let { saved ->
                setPosition(old.id, saved.copy(value = Position.remap(saved.value, old, imported.book)))
            }
            old.dir.deleteRecursively()
        }
        val manifest = imported.book.manifest
        prefs.edit().putString(NAME + manifest.id, manifest.title + "\n" + manifest.author).apply()
        return imported.book
    }

    // Removed by the user: gone from every device once it syncs.
    fun delete(book: StoredBook) {
        setPresent(book.id, Register(false, System.currentTimeMillis(), device))
        removeFiles(book.id)
        SyncScheduler.soon(app, SyncScheduler.AFTER_CHANGE_MS)
    }

    // The book's files and its place in the list; its registers stay.
    fun removeFiles(id: String) {
        prefs.edit().remove(OPENED + id).apply()
        book(id)?.dir?.deleteRecursively()
    }

    fun cleanUp() = importer.cleanUp()

    companion object {
        val SPEEDS = listOf(0.75f, 0.9f, 1f, 1.1f, 1.25f, 1.5f, 1.75f, 2f)
        val TEXT_SIZES = listOf(15f, 17f, 19f, 22f, 26f)

        // The desktop's range; a speed set there may fall between the
        // phone's steps.
        const val MIN_SPEED = 0.5f
        const val MAX_SPEED = 2f
        private val ID = Regex("[A-Za-z0-9_-]{1,64}")
        private val RANDOM = SecureRandom()
        private val LOCK = Any()
        private const val POSITION = "position:"
        private const val POSITION_AT = "positionAt:"
        private const val POSITION_BY = "positionBy:"
        private const val PRESENT = "present:"
        private const val PRESENT_AT = "presentAt:"
        private const val PRESENT_BY = "presentBy:"
        private const val NAME = "name:"
        private const val OPENED = "opened:"
        private const val SPEED = "speed"
        private const val SPEED_AT = "speedAt"
        private const val SPEED_BY = "speedBy"
        private const val TEXT_SIZE = "textSize"
        private const val TEXT_SIZE_AT = "textSizeAt"
        private const val TEXT_SIZE_BY = "textSizeBy"
        private const val DEVICE = "device"
    }
}

// One background thread for disk work (imports, chapter text, covers), so
// the main thread only ever draws.
object Background {
    private val executor = Executors.newSingleThreadExecutor { Thread(it, "omareader-io").apply { isDaemon = true } }
    private val main = Handler(Looper.getMainLooper())

    fun <T> run(work: () -> T, done: (Result<T>) -> Unit) {
        executor.execute {
            val result = runCatching(work)
            main.post { done(result) }
        }
    }
}
