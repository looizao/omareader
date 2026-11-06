package io.github.looizao.omareader

import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.UUID
import java.util.zip.ZipException
import java.util.zip.ZipInputStream

// A book as it sits on the phone: its folder and its manifest.
class StoredBook(val dir: File, val manifest: Manifest) {
    val id get() = manifest.id
    fun audio(chapter: Int) = File(dir, manifest.chapters[chapter].audio)
    fun text(chapter: Int) = File(dir, manifest.chapters[chapter].text)
    fun cover() = manifest.cover?.let { File(dir, it) }
    fun loadChapter(chapter: Int) = ChapterText.parse(text(chapter).readText())
}

// What an import produced: the book now in place, and the copy it replaced
// (still on disk so its reading position can be carried over), if any.
class Imported(val book: StoredBook, val previous: StoredBook?)

// Copies a bundle from a stream (a file picked in Google Drive, Dropbox or
// a file manager) into booksDir/<id>. It is unpacked into a hidden staging
// folder, checked completely, and only then renamed into place, so a
// cancelled or damaged import never leaves half a book behind and never
// breaks the copy already there.
class BundleImporter(private val booksDir: File) {

    fun import(input: InputStream): Imported {
        if (!booksDir.isDirectory && !booksDir.mkdirs()) throw BundleException(BundleException.Reason.Unreadable)
        val staging = File(booksDir, ".import-" + UUID.randomUUID())
        // The old copy and where it belongs, while it is moved aside.
        var displaced: Pair<File, File>? = null
        try {
            unpack(input, staging)
            val manifest = Manifest.parse(File(staging, "manifest.json").readText())
            for (chapter in manifest.chapters) {
                if (!File(staging, chapter.audio).isFile) throw BundleException(BundleException.Reason.Damaged)
                val text = File(staging, chapter.text)
                if (!text.isFile) throw BundleException(BundleException.Reason.Damaged)
                ChapterText.parse(text.readText())
            }
            val target = File(booksDir, manifest.id)
            val previous = if (target.exists()) {
                File(booksDir, ".previous-" + UUID.randomUUID()).also {
                    if (!target.renameTo(it)) throw BundleException(BundleException.Reason.Unreadable)
                    displaced = it to target
                }
            } else {
                null
            }
            if (!staging.renameTo(target)) throw BundleException(BundleException.Reason.Unreadable)
            displaced = null
            val old = previous?.let { dir -> runCatching { readBook(dir) }.getOrNull() ?: StoredBook(dir, manifest) }
            return Imported(StoredBook(target, manifest), old)
        } catch (e: ZipException) {
            throw BundleException(BundleException.Reason.NotABundle)
        } catch (e: IOException) {
            throw BundleException(BundleException.Reason.Unreadable)
        } finally {
            // Put the old copy back if the new one never made it into place.
            displaced?.let { (aside, home) -> aside.renameTo(home) }
            staging.deleteRecursively()
        }
    }

    private fun unpack(input: InputStream, staging: File) {
        if (!staging.mkdirs()) throw BundleException(BundleException.Reason.Unreadable)
        ZipInputStream(input.buffered(1 shl 16)).use { zip ->
            val seen = HashSet<String>()
            while (true) {
                val entry = zip.nextEntry ?: break
                if (entry.isDirectory) continue
                val name = entry.name
                // The manifest comes first, so any other ZIP is turned away
                // before anything of it is copied.
                if (seen.isEmpty() && name != "manifest.json") throw BundleException(BundleException.Reason.NotABundle)
                if (!Manifest.ENTRY.matches(name) || !seen.add(name)) {
                    throw BundleException(BundleException.Reason.Damaged)
                }
                val out = File(staging, name)
                out.parentFile?.mkdirs()
                out.outputStream().use { zip.copyTo(it, 1 shl 16) }
            }
            if (seen.isEmpty()) throw BundleException(BundleException.Reason.NotABundle)
        }
    }

    // Every finished book in the folder, skipping anything half-imported.
    fun books(): List<StoredBook> =
        booksDir.listFiles().orEmpty()
            .filter { it.isDirectory && !it.name.startsWith(".") }
            .mapNotNull { runCatching { readBook(it) }.getOrNull() }

    // Leftovers from an import the system stopped halfway.
    fun cleanUp() {
        booksDir.listFiles().orEmpty()
            .filter { it.name.startsWith(".import-") || it.name.startsWith(".previous-") }
            .forEach { it.deleteRecursively() }
    }

    companion object {
        fun readBook(dir: File) = StoredBook(dir, Manifest.parse(File(dir, "manifest.json").readText()))
    }
}

// Where listening stopped: a chapter and a time in its audio.
data class Position(val chapter: Int, val ms: Long) {
    fun encode() = "$chapter:$ms"

    companion object {
        val START = Position(0, 0)

        fun decode(value: String?): Position {
            val parts = value?.split(':') ?: return START
            val chapter = parts.getOrNull(0)?.toIntOrNull() ?: return START
            val ms = parts.getOrNull(1)?.toLongOrNull() ?: return START
            return Position(chapter.coerceAtLeast(0), ms.coerceAtLeast(0))
        }

        // The same place in a new export of the book. Another voice or speed
        // moves every word in time, so the place is carried over by word:
        // the word being heard in the old copy, found by the chapter's title
        // (or its number) in the new one, and that word's new start.
        fun remap(position: Position, old: StoredBook, new: StoredBook): Position {
            val oldChapters = old.manifest.chapters
            val newChapters = new.manifest.chapters
            val title = oldChapters.getOrNull(position.chapter)?.title
            val byTitle = newChapters.indexOfFirst { it.title == title && title.isNotEmpty() }
            val chapter = if (byTitle >= 0) byTitle else position.chapter.coerceIn(0, newChapters.size - 1)
            return try {
                val oldText = old.loadChapter(position.chapter)
                val newText = new.loadChapter(chapter)
                val word = oldText.wordAt(position.ms)
                if (word < 0 || word >= newText.wordCount) Position(chapter, 0)
                else Position(chapter, newText.startMs(word))
            } catch (e: Exception) {
                Position(chapter, 0)
            }
        }
    }
}
