package io.github.looizao.omareader

import org.json.JSONException
import org.json.JSONObject
import java.io.File
import java.io.InputStream
import java.security.MessageDigest

// The records Dropbox sync keeps in the app folder (docs/sync.md) and how two
// copies of them merge. Nothing here touches Android or the network, so the
// rules run in plain JVM tests against the same cases the desktop checks.

// A value with when it was set and by which device. The most recent change
// wins; a tie in time goes to the larger device id, and a tie in both is the
// same change.
data class Register<T>(val value: T, val at: Long, val by: String) {
    fun beats(other: Register<*>) = at > other.at || (at == other.at && by > other.by)

    // The same change, whatever its value looks like after being converted
    // to and from this device's own terms.
    fun sameChange(other: Register<*>?) = other != null && at == other.at && by == other.by

    companion object {
        // Of two copies, the one that stays. A tie of both time and device
        // keeps the remote copy, so a change taken from Dropbox is never sent
        // back as if it were new.
        fun <T> merge(local: Register<T>?, remote: Register<T>?): Register<T>? = when {
            local == null -> remote
            remote == null -> local
            local.beats(remote) -> local
            else -> remote
        }
    }
}

// Where reading stopped, in the desktop book's terms: the chapter's index in
// the book, the paragraph in it and the UTF-16 offset of the word, with how
// far through the book that is.
data class Anchor(val chapter: Int, val paragraph: Int, val offset: Int, val progress: Double)

class SyncFormatException(val tooNew: Boolean) : Exception(if (tooNew) "too new" else "not a sync record")

// state/<id>.json: whether a book is in the library and where it was left.
data class BookState(
    val id: String,
    val title: String = "",
    val author: String = "",
    val present: Register<Boolean>? = null,
    val position: Register<Anchor>? = null,
) {
    fun toJson(): JSONObject = JSONObject().apply {
        put("format", FORMAT)
        put("version", VERSION)
        put("id", id)
        put("title", title)
        put("author", author)
        present?.let { put("present", JSONObject().put("value", it.value).put("at", it.at).put("by", it.by)) }
        position?.let {
            put(
                "position",
                JSONObject()
                    .put("chapter", it.value.chapter)
                    .put("paragraph", it.value.paragraph)
                    .put("offset", it.value.offset)
                    .put("progress", it.value.progress)
                    .put("at", it.at)
                    .put("by", it.by),
            )
        }
    }

    // The registers of both copies merged; the book's name comes from
    // whichever copy has one, this device's first.
    fun mergedWith(remote: BookState?): BookState {
        if (remote == null) return this
        return BookState(
            id = id,
            title = title.ifEmpty { remote.title },
            author = author.ifEmpty { remote.author },
            present = Register.merge(present, remote.present),
            position = Register.merge(position, remote.position),
        )
    }

    // Whether uploading this over remote would change anything that syncs.
    fun differsFrom(remote: BookState?) =
        remote == null || present != remote.present || position != remote.position ||
            (remote.title.isEmpty() && title.isNotEmpty())

    companion object {
        const val FORMAT = "omareader-state"
        const val VERSION = 1

        fun parse(text: String): BookState = try {
            val root = JSONObject(text)
            if (root.optString("format") != FORMAT) throw SyncFormatException(tooNew = false)
            if (root.optInt("version", 0) > VERSION) throw SyncFormatException(tooNew = true)
            val present = root.optJSONObject("present")?.let { o ->
                if (!o.has("value")) null else Register(o.getBoolean("value"), o.getLong("at"), o.getString("by"))
            }
            val position = root.optJSONObject("position")?.let { o ->
                val anchor = Anchor(
                    o.getInt("chapter").coerceAtLeast(0),
                    o.getInt("paragraph").coerceAtLeast(0),
                    o.optInt("offset").coerceAtLeast(0),
                    o.optDouble("progress", 0.0).takeIf { !it.isNaN() }?.coerceIn(0.0, 1.0) ?: 0.0,
                )
                Register(anchor, o.getLong("at"), o.getString("by"))
            }
            BookState(root.getString("id"), root.optString("title"), root.optString("author"), present, position)
        } catch (e: JSONException) {
            throw SyncFormatException(tooNew = false)
        }
    }
}

// settings.json: settings that follow the user. Values stay as JSON values,
// so a key this version doesn't know passes through unchanged.
data class SyncSettings(val registers: Map<String, Register<Any>> = emptyMap()) {
    operator fun get(key: String) = registers[key]

    fun with(key: String, register: Register<Any>?) =
        if (register == null) this else SyncSettings(registers + (key to register))

    fun mergedWith(remote: SyncSettings?): SyncSettings {
        if (remote == null) return this
        val keys = registers.keys + remote.registers.keys
        return SyncSettings(keys.associateWith { Register.merge(registers[it], remote.registers[it])!! })
    }

    fun toJson(): JSONObject = JSONObject().apply {
        put("format", FORMAT)
        put("version", VERSION)
        for ((key, r) in registers.toSortedMap()) put(key, JSONObject().put("value", r.value).put("at", r.at).put("by", r.by))
    }

    // Numbers compare by value, since 22 and 22.0 are the same setting
    // whichever side wrote them.
    fun differsFrom(remote: SyncSettings?): Boolean {
        if (remote == null) return registers.isNotEmpty()
        if (registers.keys != remote.registers.keys) return true
        return registers.any { (key, r) ->
            val o = remote.registers.getValue(key)
            r.at != o.at || r.by != o.by || !sameValue(r.value, o.value)
        }
    }

    companion object {
        const val FORMAT = "omareader-settings"
        const val VERSION = 1
        const val SPEED = "speed"
        const val TEXT_SIZE_PHONE = "textSize.phone"
        private val HEADER = setOf("format", "version")

        // Android's org.json reads decimals as Double, others as BigDecimal;
        // settings are plain numbers either way.
        private fun plain(value: Any): Any = when (value) {
            is java.math.BigDecimal -> value.toDouble()
            is java.math.BigInteger -> value.toLong()
            is Float -> value.toDouble()
            else -> value
        }

        fun sameValue(a: Any, b: Any) =
            if (a is Number && b is Number) a.toDouble() == b.toDouble() else a == b

        fun parse(text: String): SyncSettings = try {
            val root = JSONObject(text)
            if (root.optString("format") != FORMAT) throw SyncFormatException(tooNew = false)
            if (root.optInt("version", 0) > VERSION) throw SyncFormatException(tooNew = true)
            val registers = HashMap<String, Register<Any>>()
            for (key in root.keys()) {
                if (key in HEADER) continue
                val o = root.optJSONObject(key) ?: continue
                if (!o.has("value") || !o.has("at")) continue
                registers[key] = Register(plain(o.get("value")), o.getLong("at"), o.optString("by"))
            }
            SyncSettings(registers)
        } catch (e: JSONException) {
            throw SyncFormatException(tooNew = false)
        }
    }
}

// Dropbox's content_hash: SHA-256 of each 4 MiB block, then SHA-256 of
// those digests one after another, in hex.
object ContentHash {
    const val BLOCK = 4 * 1024 * 1024

    fun of(input: InputStream): String {
        val total = MessageDigest.getInstance("SHA-256")
        val block = MessageDigest.getInstance("SHA-256")
        val buffer = ByteArray(1 shl 16)
        var inBlock = 0
        while (true) {
            val wanted = minOf(buffer.size, BLOCK - inBlock)
            val read = input.read(buffer, 0, wanted)
            if (read < 0) break
            block.update(buffer, 0, read)
            inBlock += read
            if (inBlock == BLOCK) {
                total.update(block.digest())
                inBlock = 0
            }
        }
        if (inBlock > 0) total.update(block.digest())
        return total.digest().joinToString("") { "%02x".format(it) }
    }

    fun of(file: File) = file.inputStream().use { of(it) }
}

// Dropbox-API-Arg is an HTTP header, so the JSON in it escapes 0x7F and
// every non-ASCII character as \uXXXX, by UTF-16 code unit.
object DropboxArg {
    fun escape(json: String): String {
        val out = StringBuilder(json.length)
        for (c in json) {
            if (c.code >= 0x7F) out.append("\\u%04x".format(c.code)) else out.append(c)
        }
        return out.toString()
    }
}

// Turns this phone's place in a bundle (a chapter of the bundle and a time
// in its audio) into a synced Anchor and back. Bundles from before the
// manifest named each chapter's place in the book can't be mapped, so their
// positions don't sync.
object Anchors {
    fun fromPosition(book: StoredBook, position: Position): Anchor? {
        val manifest = book.manifest
        if (!manifest.knowsBookChapters || position.chapter !in manifest.chapters.indices) return null
        val progress = CarCatalog.progress(manifest, position)
        val text = book.loadChapter(position.chapter)
        val word = text.wordAt(position.ms)
        val bookChapter = manifest.chapters[position.chapter].bookChapter
        return if (word < 0) {
            Anchor(bookChapter, 0, 0, progress)
        } else {
            Anchor(bookChapter, text.paragraph(word), text.start(word), progress)
        }
    }

    // The start of the word at the anchor, or the next one read after it. A
    // chapter the bundle left out (nothing to read) resumes at the next one.
    fun toPosition(book: StoredBook, anchor: Anchor): Position? {
        val chapters = book.manifest.chapters
        if (!book.manifest.knowsBookChapters) return null
        val exact = chapters.indexOfFirst { it.bookChapter == anchor.chapter }
        if (exact < 0) {
            val next = chapters.indexOfFirst { it.bookChapter > anchor.chapter }
            return if (next >= 0) Position(next, 0) else Position(chapters.size - 1, chapters.last().durationMs)
        }
        val text = book.loadChapter(exact)
        val word = text.wordAtOffset(anchor.paragraph, anchor.offset)
        return if (word < 0) Position(exact, chapters[exact].durationMs) else Position(exact, text.startMs(word))
    }
}
