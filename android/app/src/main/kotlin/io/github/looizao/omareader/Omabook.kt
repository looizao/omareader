package io.github.looizao.omareader

import org.json.JSONException
import org.json.JSONObject

// The .omabook bundle the desktop app exports (docs/omabook.md): a manifest,
// and per chapter its AAC audio and its text with the time of every word.
// Nothing here touches Android, so it runs in plain JVM tests.

class BundleException(val reason: Reason) : Exception(reason.name) {
    enum class Reason { NotABundle, TooNew, Damaged, Unreadable }
}

// bookChapter is the chapter's index in the desktop book, counting chapters
// the bundle left out for having nothing to read; synced positions name
// chapters that way. Bundles exported before it existed have -1.
class ChapterInfo(
    val title: String,
    val audio: String,
    val text: String,
    val durationMs: Long,
    val bookChapter: Int = -1,
)

class Manifest(
    val id: String,
    val title: String,
    val author: String,
    val cover: String?,
    val chapters: List<ChapterInfo>,
    // The speed the voice was synthesised at; the player divides the
    // listening speed by it, so a book exported fast is not sped up twice.
    val voiceSpeed: Double = 1.0,
) {
    // Whether every chapter says where it sits in the book, which a synced
    // position needs.
    val knowsBookChapters get() = chapters.all { it.bookChapter >= 0 }

    companion object {
        const val VERSION = 1

        // Entry names a bundle may hold; anything else is refused, which also
        // keeps every extracted file inside the book's folder.
        val ENTRY = Regex("""manifest\.json|cover\.jpg|audio/\d{3,6}\.m4a|text/\d{3,6}\.json""")

        // The id names the book's folder, so it must be a plain name.
        private val ID = Regex("""[A-Za-z0-9_-]{1,64}""")

        fun parse(json: String): Manifest = try {
            val root = JSONObject(json)
            if (root.optString("format") != "omabook") throw BundleException(BundleException.Reason.NotABundle)
            if (root.optInt("version", 0) > VERSION) throw BundleException(BundleException.Reason.TooNew)
            val id = root.getString("id")
            if (!ID.matches(id)) throw BundleException(BundleException.Reason.Damaged)
            val list = root.getJSONArray("chapters")
            val chapters = (0 until list.length()).map { i ->
                val c = list.getJSONObject(i)
                val audio = c.getString("audio")
                val text = c.getString("text")
                if (!ENTRY.matches(audio) || !audio.startsWith("audio/") || !ENTRY.matches(text) ||
                    !text.startsWith("text/")
                ) throw BundleException(BundleException.Reason.Damaged)
                ChapterInfo(c.optString("title"), audio, text, c.optLong("duration"), c.optInt("chapter", -1))
            }
            if (chapters.isEmpty()) throw BundleException(BundleException.Reason.Damaged)
            val cover = root.optString("cover").takeIf { it == "cover.jpg" }
            val speed = root.optJSONObject("voice")?.optDouble("speed", 1.0)?.takeIf { it in 0.25..4.0 } ?: 1.0
            Manifest(id, root.optString("title"), root.optString("author"), cover, chapters, speed)
        } catch (e: JSONException) {
            throw BundleException(BundleException.Reason.NotABundle)
        }
    }
}

class Paragraph(val text: String, val level: Int) {
    val isHeading get() = level > 0
}

// A chapter's paragraphs and its spoken words. Words are kept as one flat
// IntArray, five values each (paragraph, offset, length, start ms, end ms),
// which is compact and needs no object per word even for long chapters.
class ChapterText(
    val title: String,
    val paragraphs: List<Paragraph>,
    private val words: IntArray,
    private val sentences: IntArray,
) {
    val wordCount get() = words.size / STRIDE
    val sentenceCount get() = sentences.size

    fun paragraph(word: Int) = words[word * STRIDE]
    fun start(word: Int) = words[word * STRIDE + 1]
    fun end(word: Int) = start(word) + words[word * STRIDE + 2]
    fun startMs(word: Int) = words[word * STRIDE + 3].toLong()
    fun endMs(word: Int) = words[word * STRIDE + 4].toLong()

    // The word being heard at ms: the last one that has started, so a pause
    // keeps the word before it lit. -1 before the first word.
    fun wordAt(ms: Long): Int {
        var low = 0
        var high = wordCount - 1
        var found = -1
        while (low <= high) {
            val mid = (low + high) ushr 1
            if (startMs(mid) <= ms) {
                found = mid
                low = mid + 1
            } else {
                high = mid - 1
            }
        }
        return found
    }

    // The word at a character of a paragraph, or the next one read after it
    // (tapping the space between words starts at the following word). -1
    // when nothing in or after that paragraph is read aloud.
    fun wordAtOffset(paragraph: Int, offset: Int): Int {
        for (w in 0 until wordCount) {
            val p = paragraph(w)
            if (p > paragraph) return w
            if (p == paragraph && end(w) > offset) return w
        }
        return -1
    }

    // The first word heard in a paragraph or after it, for scrolling to a
    // paragraph that has none of its own.
    fun firstWordFrom(paragraph: Int): Int = wordAtOffset(paragraph, 0)

    // The sentence holding a word, by the index of its first word.
    fun sentenceOf(word: Int): Int {
        var low = 0
        var high = sentences.size - 1
        var found = 0
        while (low <= high) {
            val mid = (low + high) ushr 1
            if (sentences[mid] <= word) {
                found = mid
                low = mid + 1
            } else {
                high = mid - 1
            }
        }
        return found
    }

    fun sentenceStart(sentence: Int) = sentences[sentence]

    companion object {
        const val STRIDE = 5

        fun parse(json: String): ChapterText = try {
            val root = JSONObject(json)
            val list = root.getJSONArray("paragraphs")
            val paragraphs = (0 until list.length()).map { i ->
                val p = list.getJSONObject(i)
                Paragraph(p.getString("text"), p.optInt("level"))
            }
            val flat = root.getJSONArray("words")
            if (flat.length() % STRIDE != 0) throw BundleException(BundleException.Reason.Damaged)
            val words = IntArray(flat.length()) { flat.getInt(it) }
            val starts = root.optJSONArray("sentences")
            val sentences = if (starts != null && starts.length() > 0) {
                IntArray(starts.length()) { starts.getInt(it) }
            } else {
                intArrayOf(0)
            }
            ChapterText(root.optString("title"), paragraphs, words, sentences).also { it.check() }
        } catch (e: JSONException) {
            throw BundleException(BundleException.Reason.Damaged)
        }
    }

    // Every word must point inside its paragraph and the times must run in
    // order, or the lookups above would highlight the wrong text.
    private fun check() {
        var previousParagraph = 0
        var previousStart = Long.MIN_VALUE
        for (w in 0 until wordCount) {
            val p = paragraph(w)
            if (p < previousParagraph || p >= paragraphs.size) throw BundleException(BundleException.Reason.Damaged)
            if (start(w) < 0 || words[w * STRIDE + 2] <= 0 || end(w) > paragraphs[p].text.length) {
                throw BundleException(BundleException.Reason.Damaged)
            }
            if (startMs(w) < previousStart || endMs(w) < startMs(w)) {
                throw BundleException(BundleException.Reason.Damaged)
            }
            previousParagraph = p
            previousStart = startMs(w)
        }
        var previous = -1
        for (s in sentences) {
            if (s <= previous || s < 0 || (wordCount > 0 && s >= wordCount)) {
                throw BundleException(BundleException.Reason.Damaged)
            }
            previous = s
        }
    }
}
