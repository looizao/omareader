package io.github.looizao.omareader

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import java.io.File
import java.util.concurrent.Executors

// The books on this phone and where each was left. Books live in the app's
// private storage (one folder per book, named by its id); positions and the
// order they were opened in are small SharedPreferences entries.
class Library(context: Context) {
    private val app = context.applicationContext
    private val importer = BundleImporter(File(app.filesDir, "books"))
    private val prefs = app.getSharedPreferences("library", Context.MODE_PRIVATE)
    private val reader = app.getSharedPreferences("reader", Context.MODE_PRIVATE)

    // Most recently opened first, then newest import.
    fun books(): List<StoredBook> =
        importer.books().sortedWith(
            compareByDescending<StoredBook> { prefs.getLong(OPENED + it.id, 0) }
                .thenByDescending { it.dir.lastModified() },
        )

    fun book(id: String?): StoredBook? {
        if (id.isNullOrEmpty() || !Regex("[A-Za-z0-9_-]{1,64}").matches(id)) return null
        val dir = File(File(app.filesDir, "books"), id)
        return if (dir.isDirectory) runCatching { BundleImporter.readBook(dir) }.getOrNull() else null
    }

    fun position(id: String) = Position.decode(prefs.getString(POSITION + id, null))

    fun savePosition(id: String, position: Position) {
        prefs.edit().putString(POSITION + id, position.encode()).apply()
    }

    fun markOpened(id: String) {
        prefs.edit().putLong(OPENED + id, System.currentTimeMillis()).apply()
    }

    // The reading speed is one setting for every book, kept with the reader's
    // other settings; the car plays at the speed chosen on the phone.
    fun speed(): Float = reader.getFloat(SPEED, 1f).coerceIn(SPEEDS.first(), SPEEDS.last())

    fun setSpeed(speed: Float) {
        reader.edit().putFloat(SPEED, speed).apply()
    }

    // Copies the bundle in. A new export of a book already here replaces it
    // and keeps the place reached, moved onto the new export's timings.
    fun import(uri: Uri): StoredBook {
        val input = app.contentResolver.openInputStream(uri)
            ?: throw BundleException(BundleException.Reason.Unreadable)
        val imported = input.use { importer.import(it) }
        imported.previous?.let { old ->
            val saved = prefs.getString(POSITION + old.id, null)
            if (saved != null) savePosition(old.id, Position.remap(Position.decode(saved), old, imported.book))
            old.dir.deleteRecursively()
        }
        return imported.book
    }

    fun delete(book: StoredBook) {
        prefs.edit().remove(POSITION + book.id).remove(OPENED + book.id).apply()
        book.dir.deleteRecursively()
    }

    fun cleanUp() = importer.cleanUp()

    companion object {
        val SPEEDS = listOf(0.75f, 0.9f, 1f, 1.1f, 1.25f, 1.5f, 1.75f, 2f)
        private const val POSITION = "position:"
        private const val OPENED = "opened:"
        private const val SPEED = "speed"
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
