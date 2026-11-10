package io.github.looizao.omareader

import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.ProtocolException
import java.net.URL
import java.net.URLEncoder
import java.security.MessageDigest
import java.security.SecureRandom
import java.time.Instant
import java.time.format.DateTimeParseException

// Google Drive's HTTP API (v3), spoken with HttpURLConnection like
// Dropbox.kt, and giving the sync engine the same calls (docs/sync.md,
// "Google Drive"). Drive has no app folder, no conditional write, and lets
// two files share a name, so this client finds omareader's files by their
// properties and settles duplicates itself. Nothing here touches Android, so
// the tests run it against a local server standing in for Drive.

// Who is connected, to show in the settings.
data class DriveUser(val name: String, val email: String) {
    val label get() = listOf(name, email).filter { it.isNotEmpty() }.joinToString(" · ")
}

class DriveRemote(
    private val tokens: AccessTokens,
    private val api: String = API,
    // Waits between retries; the tests make it instant.
    private val sleep: (Long) -> Unit = { Thread.sleep(it) },
    // Up to a second added to each wait, so devices that failed together
    // don't come back together.
    private val jitter: () -> Long = { random.nextInt(1000).toLong() },
    // How much of a bundle goes up per request; Drive wants a multiple of
    // 256 KiB, and the tests use a few bytes.
    private val piece: Int = PIECE,
) : Remote {

    // A file as the listing gave it, with what the duplicate rule needs.
    private class Listed(val id: String, val created: Instant, val modified: Instant, val file: RemoteFile)

    private sealed class Progress {
        class Done(val listed: Listed) : Progress()
        class At(val offset: Long) : Progress()
    }

    private class NotFoundException : IOException("not found")
    private class SessionExpiredException : IOException("upload session expired")

    // The file kept for each path (lowercase, as the engine compares them),
    // from the last listing and from what this client wrote since.
    private val listed = HashMap<String, Listed>()
    private var folder: String? = null

    override fun list(): List<RemoteFile> {
        val copies = files(FILES).groupBy { it.file.path.lowercase() }
        listed.clear()
        return copies.values.map { keep(it).file }
    }

    override fun read(path: String): Pair<ByteArray, RemoteFile> {
        val f = known(path) ?: throw IOException("$path is not in Google Drive")
        val bytes = call("GET", "$api/drive/v3/files/${f.id}?alt=media") { _, c -> c.inputStream.use { it.readBytes() } }
        return bytes to f.file
    }

    override fun download(path: String, out: File, from: Long, cancelled: () -> Boolean): RemoteFile {
        val f = known(path) ?: throw IOException("$path is not in Google Drive")
        call(
            "GET",
            "$api/drive/v3/files/${f.id}?alt=media",
            { c ->
                c.readTimeout = 120_000
                if (from > 0) c.setRequestProperty("Range", "bytes=$from-")
            },
        ) { code, c ->
            // A server that ignores the range sends the whole file again.
            val append = from > 0 && code == 206
            FileOutputStream(out, append).use { sink -> c.inputStream.use { copy(it, sink, cancelled) } }
        }
        return f.file
    }

    override fun write(path: String, bytes: ByteArray, mode: WriteMode): RemoteFile = when (mode) {
        // Asked fresh rather than taken from the listing, so a file another
        // device added since is a conflict and not a second copy.
        WriteMode.Add -> if (lookup(path) != null) throw ConflictException() else remember(create(path, bytes)).file
        is WriteMode.Update -> remember(update(path, bytes, mode.rev)).file
        WriteMode.Overwrite -> upload(path, bytes.size.toLong(), path.substringAfterLast('/')) { offset, length ->
            bytes.copyOfRange(offset.toInt(), offset.toInt() + length)
        }
    }

    // A bundle, in a resumable upload. The phone doesn't upload bundles; this
    // is the contract the desktop follows, kept here so both sides are
    // tested against the same rules.
    fun upload(path: String, file: File, name: String = path.substringAfterLast('/')): RemoteFile =
        RandomAccessFile(file, "r").use { raf ->
            upload(path, raf.length(), name) { offset, length ->
                ByteArray(length).also {
                    raf.seek(offset)
                    raf.readFully(it)
                }
            }
        }

    // Moves the file to the trash, where the user can get it back for 30
    // days.
    override fun delete(path: String) {
        val f = known(path) ?: return
        trash(f.id)
        listed.remove(path.lowercase())
    }

    // Drive's sha256Checksum is the plain SHA-256 of the whole file.
    override fun contentHash(file: File): String = file.inputStream().use { sha256(it) }

    fun account(): DriveUser {
        val fields = URLEncoder.encode("user(displayName,emailAddress)", "UTF-8")
        val user = call("GET", "$api/drive/v3/about?fields=$fields") { _, c -> json(c) }.optJSONObject("user")
        return DriveUser(user?.optString("displayName").orEmpty(), user?.optString("emailAddress").orEmpty())
    }

    // The folder new files go in: the first one made, should two devices
    // have made one at the same moment, or a new one.
    private fun folder(): String {
        folder?.let { return it }
        val found = query(FOLDERS, "id,createdTime").minWithOrNull(compareBy({ instant(it, "createdTime") }, { it.getString("id") }))
        val id = found?.getString("id") ?: call(
            "POST",
            "$api/drive/v3/files?fields=id",
            body(
                JSONObject()
                    .put("name", "omareader")
                    .put("mimeType", FOLDER_TYPE)
                    .put("parents", JSONArray().put("root"))
                    .put("properties", JSONObject().put(KIND, "folder"))
                    .toString().toByteArray(),
                JSON_TYPE,
            ),
        ) { _, c -> json(c).getString("id") }
        folder = id
        return id
    }

    // The listed file for a path, or the one Drive has now.
    private fun known(path: String) = listed[path.lowercase()] ?: lookup(path)

    private fun lookup(path: String): Listed? {
        val copies = files("$FILES and properties has {key='$PATH' and value='${quote(path)}'}")
        return if (copies.isEmpty()) null else keep(copies)
    }

    // Of the copies of one path, keeps the one docs/sync.md names and trashes
    // the rest: the record created first, since every device merges into it,
    // or the bundle modified last, since it is the newest export.
    private fun keep(copies: List<Listed>): Listed {
        val bundle = copies.first().file.path.lowercase().startsWith("/books/")
        val kept = if (bundle) {
            copies.maxWith(compareBy({ it.modified }, { it.id }))
        } else {
            copies.minWith(compareBy({ it.created }, { it.id }))
        }
        copies.filter { it !== kept }.forEach { trash(it.id) }
        return remember(kept)
    }

    private fun remember(f: Listed): Listed {
        listed[f.file.path.lowercase()] = f
        return f
    }

    private fun files(q: String) = query(q, FILE_FIELDS).mapNotNull { listedOf(it) }

    private fun query(q: String, fields: String): List<JSONObject> {
        val found = ArrayList<JSONObject>()
        var token: String? = null
        do {
            val url = "$api/drive/v3/files?" + form(
                "q" to q,
                "spaces" to "drive",
                "pageSize" to "1000",
                "fields" to "nextPageToken,files($fields)",
                "pageToken" to token,
            )
            val page = call("GET", url) { _, c -> json(c) }
            val files = page.optJSONArray("files") ?: JSONArray()
            for (i in 0 until files.length()) found.add(files.getJSONObject(i))
            token = page.optString("nextPageToken").ifEmpty { null }
        } while (token != null)
        return found
    }

    private fun create(path: String, bytes: ByteArray): Listed {
        val metadata = metadata(path, path.substringAfterLast('/'))
        val boundary = "omareader-" + (1..16).map { "0123456789abcdef"[random.nextInt(16)] }.joinToString("")
        val out = ByteArrayOutputStream()
        out.write("--$boundary\r\nContent-Type: $JSON_TYPE\r\n\r\n".toByteArray())
        out.write(metadata.toString().toByteArray())
        out.write("\r\n--$boundary\r\nContent-Type: $OCTETS\r\n\r\n".toByteArray())
        out.write(bytes)
        out.write("\r\n--$boundary--\r\n".toByteArray())
        return call(
            "POST",
            "$api/upload/drive/v3/files?uploadType=multipart&fields=$ENCODED_FIELDS",
            body(out.toByteArray(), "multipart/related; boundary=$boundary"),
        ) { _, c -> written(json(c), path) }
    }

    // Drive can't make the write itself conditional, so the revision is
    // checked just before it. A write from another device can still land in
    // between; that device finds its rev gone at its next sync and merges
    // again (docs/sync.md).
    private fun update(path: String, bytes: ByteArray, rev: String): Listed {
        val f = known(path) ?: throw ConflictException()
        val head = try {
            call("GET", "$api/drive/v3/files/${f.id}?fields=headRevisionId,modifiedTime,trashed") { _, c -> json(c) }
        } catch (e: NotFoundException) {
            throw ConflictException()
        }
        if (head.optBoolean("trashed") || revOf(head) != rev) throw ConflictException()
        return call(
            "PATCH",
            "$api/upload/drive/v3/files/${f.id}?uploadType=media&fields=$ENCODED_FIELDS",
            body(bytes, OCTETS),
        ) { _, c -> written(json(c), path) }
    }

    private fun upload(path: String, total: Long, name: String, read: (Long, Int) -> ByteArray): RemoteFile {
        repeat(SESSIONS) {
            val session = startUpload(path, name, total)
            try {
                return remember(sendPieces(session, path, total, read)).file
            } catch (e: SessionExpiredException) {
                // Drive forgot the session; the upload starts over.
            }
        }
        throw RetryLaterException(RETRY_LATER_SECONDS)
    }

    // Opens a resumable upload: a new file, or new contents for the file
    // already there for the path.
    private fun startUpload(path: String, name: String, total: Long): String {
        val existing = lookup(path)
        val (method, url, metadata) = if (existing == null) {
            Triple("POST", "$api/upload/drive/v3/files?uploadType=resumable&fields=$ENCODED_FIELDS", metadata(path, name))
        } else {
            Triple("PATCH", "$api/upload/drive/v3/files/${existing.id}?uploadType=resumable&fields=$ENCODED_FIELDS", JSONObject().put("name", name))
        }
        return call(
            method,
            url,
            { c ->
                c.setRequestProperty("X-Upload-Content-Type", OCTETS)
                c.setRequestProperty("X-Upload-Content-Length", total.toString())
                body(metadata.toString().toByteArray(), JSON_TYPE)(c)
            },
        ) { _, c -> c.getHeaderField("Location") ?: throw IOException("Google Drive gave no upload address") }
    }

    private fun sendPieces(session: String, path: String, total: Long, read: (Long, Int) -> ByteArray): Listed {
        var offset = 0L
        var failures = 0
        var ask = total == 0L
        while (true) {
            val asked = ask
            ask = false
            val progress = if (asked) {
                put(session, ByteArray(0), "bytes */$total", path)
            } else {
                val length = minOf(piece.toLong(), total - offset).toInt()
                put(session, read(offset, length), "bytes $offset-${offset + length - 1}/$total", path)
            }
            when (progress) {
                is Progress.Done -> return progress.listed
                is Progress.At -> {
                    if (progress.offset > offset) {
                        failures = 0
                    } else if (!asked || progress.offset >= total) {
                        // Drive took none of the piece.
                        backOff(failures++)
                    }
                    offset = progress.offset
                    if (offset >= total) ask = true
                }
                // The connection dropped or Drive failed: wait, then ask how
                // much arrived before sending more.
                null -> {
                    backOff(failures++)
                    ask = true
                }
            }
        }
    }

    // One piece of a resumable upload, or with no bytes, the question of how
    // much arrived. Null when the connection dropped or Drive asked to try
    // again.
    private fun put(session: String, bytes: ByteArray, range: String, path: String): Progress? {
        val c = open("PUT", session)
        try {
            val code = try {
                c.doOutput = true
                c.setFixedLengthStreamingMode(bytes.size)
                c.setRequestProperty("Content-Range", range)
                c.outputStream.use { it.write(bytes) }
                c.responseCode
            } catch (e: IOException) {
                return null
            }
            return when (code) {
                200, 201 -> Progress.Done(written(json(c), path))
                308 -> Progress.At(received(c.getHeaderField("Range")))
                404, 410 -> throw SessionExpiredException()
                429, 500, 502, 503, 504 -> null
                else -> throw IOException("Google Drive answered $code to an upload")
            }
        } finally {
            c.disconnect()
        }
    }

    private fun backOff(failures: Int) {
        if (failures >= BACKOFF.size) throw RetryLaterException(RETRY_LATER_SECONDS)
        sleep(BACKOFF[failures] * 1000 + jitter())
    }

    private fun trash(id: String) {
        try {
            call("PATCH", "$api/drive/v3/files/$id?fields=id", body("""{"trashed":true}""".toByteArray(), JSON_TYPE)) { _, _ -> }
        } catch (e: NotFoundException) {
            // Already gone, which is what was wanted.
        }
    }

    // One request, with the access token renewed once when Drive refuses
    // it, the waits docs/sync.md names when Drive is busy, and its errors
    // turned into exceptions the sync understands.
    private fun <T> call(
        method: String,
        url: String,
        prepare: (HttpURLConnection) -> Unit = {},
        handle: (Int, HttpURLConnection) -> T,
    ): T {
        var refreshed = false
        var failures = 0
        while (true) {
            val token = tokens.current()
            val c = open(method, url)
            try {
                c.setRequestProperty("Authorization", "Bearer $token")
                prepare(c)
                val code = c.responseCode
                if (code in 200..299) return handle(code, c)
                val reasons = if (code >= 400) reasons(c) else emptySet()
                when {
                    code == 401 && !refreshed -> {
                        refreshed = true
                        tokens.refresh()
                    }
                    code == 401 -> throw RevokedException()
                    code in RETRY_CODES || (code == 403 && reasons.any { it in RATE_LIMITS }) -> {
                        if (failures >= BACKOFF.size) throw RetryLaterException(DropboxRemote.retryAfter(c))
                        sleep(BACKOFF[failures++] * 1000 + jitter())
                    }
                    code == 404 -> throw NotFoundException()
                    else -> throw IOException("Google Drive answered $code ${reasons.joinToString()}".trim())
                }
            } finally {
                c.disconnect()
            }
        }
    }

    private fun open(method: String, url: String): HttpURLConnection {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = 20_000
        c.readTimeout = 60_000
        c.instanceFollowRedirects = false
        try {
            c.requestMethod = method
        } catch (e: ProtocolException) {
            // The JVM's HttpURLConnection knows no PATCH (Android's does).
            // Google's APIs take a POST that names the method instead.
            c.requestMethod = "POST"
            c.setRequestProperty("X-HTTP-Method-Override", method)
        }
        return c
    }

    private fun metadata(path: String, name: String) = JSONObject()
        .put("name", name)
        .put("parents", JSONArray().put(folder()))
        .put("properties", JSONObject().put(KIND, "file").put(PATH, path))

    private fun body(bytes: ByteArray, type: String): (HttpURLConnection) -> Unit = { c ->
        c.doOutput = true
        c.setFixedLengthStreamingMode(bytes.size)
        c.setRequestProperty("Content-Type", type)
        c.outputStream.use { it.write(bytes) }
    }

    private fun copy(input: InputStream, out: FileOutputStream, cancelled: () -> Boolean) {
        val buffer = ByteArray(1 shl 16)
        while (true) {
            if (cancelled()) throw InterruptedException()
            val n = input.read(buffer)
            if (n < 0) return
            out.write(buffer, 0, n)
        }
    }

    companion object {
        const val API = "https://www.googleapis.com"
        const val PIECE = 8 * 1024 * 1024
        const val KIND = "omareader"
        const val PATH = "omareaderPath"
        const val FOLDER_TYPE = "application/vnd.google-apps.folder"
        private const val JSON_TYPE = "application/json; charset=UTF-8"
        private const val OCTETS = "application/octet-stream"
        private const val FILE_FIELDS = "id,name,properties,headRevisionId,sha256Checksum,size,createdTime,modifiedTime"
        private val ENCODED_FIELDS = URLEncoder.encode(FILE_FIELDS, "UTF-8")
        private const val FILES = "properties has {key='$KIND' and value='file'} and trashed=false"
        private const val FOLDERS = "properties has {key='$KIND' and value='folder'} and trashed=false"
        private val BACKOFF = longArrayOf(1, 2, 4)
        private val RETRY_CODES = setOf(429, 500, 502, 503, 504)
        private val RATE_LIMITS = setOf("rateLimitExceeded", "userRateLimitExceeded")
        private const val RETRY_LATER_SECONDS = 60L
        private const val SESSIONS = 3
        private val random = SecureRandom()

        fun sha256(input: InputStream): String {
            val digest = MessageDigest.getInstance("SHA-256")
            val buffer = ByteArray(1 shl 16)
            while (true) {
                val n = input.read(buffer)
                if (n < 0) break
                digest.update(buffer, 0, n)
            }
            return digest.digest().joinToString("") { "%02x".format(it) }
        }

        // Inside a query a value is quoted with ', so ' and \ are escaped.
        fun quote(value: String) = value.replace("\\", "\\\\").replace("'", "\\'")

        // The byte after the last one Drive has, from "bytes=0-42".
        fun received(range: String?): Long =
            range?.substringAfterLast('-', "")?.trim()?.toLongOrNull()?.plus(1) ?: 0

        private fun form(vararg fields: Pair<String, String?>) = fields.filter { it.second != null }
            .joinToString("&") { (k, v) -> k + "=" + URLEncoder.encode(v, "UTF-8") }

        private fun json(c: HttpURLConnection) =
            JSONObject(c.inputStream.use { it.readBytes() }.toString(Charsets.UTF_8))

        private fun instant(o: JSONObject, key: String): Instant = try {
            Instant.parse(o.optString(key))
        } catch (e: DateTimeParseException) {
            Instant.EPOCH
        }

        // headRevisionId changes with every new content. A file without one
        // (Drive leaves it out for files with no content) falls back to its
        // modification time, which changes as often.
        private fun revOf(o: JSONObject) = o.optString("headRevisionId").ifEmpty { "m:" + o.optString("modifiedTime") }

        // A file of the listing, under the Dropbox-style path its properties
        // give (or the one it was written under). Files without a usable
        // path aren't omareader's to sync.
        private fun listedOf(o: JSONObject, path: String? = null): Listed? {
            val p = path ?: o.optJSONObject("properties")?.optString(PATH).orEmpty()
            if (!p.startsWith("/") || p.endsWith("/")) return null
            return Listed(
                o.getString("id"),
                instant(o, "createdTime"),
                instant(o, "modifiedTime"),
                RemoteFile(
                    path = p,
                    name = p.substringAfterLast('/'),
                    rev = revOf(o),
                    size = o.optLong("size"),
                    contentHash = o.optString("sha256Checksum").ifEmpty { null },
                ),
            )
        }

        private fun written(o: JSONObject, path: String) =
            listedOf(o, path) ?: throw IOException("Google Drive answered a write without the file")

        // The reasons in Drive's error, such as userRateLimitExceeded.
        private fun reasons(c: HttpURLConnection): Set<String> {
            val text = runCatching { c.errorStream?.use { it.readBytes() }?.toString(Charsets.UTF_8) }.getOrNull() ?: return emptySet()
            return try {
                val errors = JSONObject(text).optJSONObject("error")?.optJSONArray("errors") ?: return emptySet()
                (0 until errors.length()).mapNotNull { errors.optJSONObject(it)?.optString("reason")?.ifEmpty { null } }.toSet()
            } catch (e: JSONException) {
                emptySet()
            }
        }
    }
}
