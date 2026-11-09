package io.github.looizao.omareader

import java.text.Normalizer

// What Android Auto, the assistant and other media browsers see of the
// library. The decisions live here, apart from Media3, so they can be tested
// on the JVM; PlaybackService turns them into media items.
object CarCatalog {
    // The browse tree: the root holds one folder of books, and each book
    // plays from where it was left. Chapters are reached from the queue in
    // the car. Book ids are hexadecimal and chapters are "<book>/<n>", so
    // neither can be mistaken for these.
    const val ROOT = "root"
    const val RECENT = "recent"
    const val BOOKS = "books"

    // Apps besides this one and the system's own (the notification, the lock
    // screen and Bluetooth, which Android marks as trusted) that may list the
    // books: Android Auto, the assistant for "play ... on omareader", and the
    // maps apps that show a player while navigating. Any other app can still
    // play, pause and see what is playing, as with every media app, but it
    // cannot read the library.
    private val BROWSERS = setOf(
        "com.google.android.projection.gearhead",
        "com.google.android.googlequicksearchbox",
        "com.google.android.apps.maps",
        "com.waze",
    )

    fun mayBrowse(packageName: String, trusted: Boolean, ownPackage: String) =
        trusted || packageName == ownPackage || packageName in BROWSERS

    // How far into the book a place is, from 0 to 1, by the audio's length.
    fun progress(manifest: Manifest, place: Position): Double {
        val chapters = manifest.chapters
        val total = chapters.sumOf { it.durationMs }
        if (chapters.isEmpty() || total <= 0) return 0.0
        val chapter = place.chapter.coerceIn(chapters.indices)
        val before = chapters.take(chapter).sumOf { it.durationMs }
        val into = place.ms.coerceIn(0, chapters[chapter].durationMs)
        return ((before + into) / total.toDouble()).coerceIn(0.0, 1.0)
    }

    // The book a voice request names, from books listed most recent first.
    // Every word said must be in the title, or failing that in the title and
    // author together, ignoring case, accents and punctuation; a tie goes to
    // the book opened last. "Play omareader", with no title, resumes the
    // most recent book.
    fun find(books: List<StoredBook>, query: String?): StoredBook? {
        val words = normalize(query.orEmpty()).split(' ').filter { it.isNotEmpty() }
        if (words.isEmpty()) return books.firstOrNull()
        fun matches(text: String) = normalize(text).let { t -> words.all { it in t } }
        return books.firstOrNull { matches(it.manifest.title) }
            ?: books.firstOrNull { matches(it.manifest.title + " " + it.manifest.author) }
    }

    private val MARKS = Regex("\\p{Mn}+")
    private val SEPARATORS = Regex("[^\\p{L}\\p{N}]+")

    fun normalize(text: String): String =
        Normalizer.normalize(text.lowercase(), Normalizer.Form.NFD)
            .replace(MARKS, "")
            .replace(SEPARATORS, " ")
            .trim()
}
