package io.github.looizao.omareader

import org.json.JSONException
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64

// Dropbox's HTTP API, spoken directly with the platform's HttpURLConnection
// so the app needs no SDK. Nothing here touches Android, so the tests run it
// against a local HTTP server standing in for Dropbox.

// Gives the access token to send, refreshing it when Dropbox says it has
// expired. Access tokens live only in memory.
interface AccessTokens {
    fun current(): String
    fun refresh(): String
}

// A 409 from Dropbox: the endpoint's own error, in its JSON.
class DropboxError(val error: JSONObject, summary: String) : IOException(summary) {
    private fun tag(o: JSONObject?) = o?.optString(".tag").orEmpty()

    // A write that found someone else's newer copy (the rev didn't match, or
    // the file appeared). Upload puts the WriteError in "reason", an upload
    // session in "path".
    val isConflict
        get() = tag(error) == "path" &&
            (tag(error.optJSONObject("reason")) == "conflict" || tag(error.optJSONObject("path")) == "conflict")

    val isNotFound
        get() = (tag(error) == "path_lookup" && tag(error.optJSONObject("path_lookup")) == "not_found") ||
            (tag(error) == "path" && tag(error.optJSONObject("path")) == "not_found")
}

class DropboxRemote(
    private val tokens: AccessTokens,
    private val api: String = API,
    private val content: String = CONTENT,
) : Remote {

    private class Response(val code: Int, val connection: HttpURLConnection)

    // One request, with the access token refreshed once if it has expired,
    // and Dropbox's errors turned into exceptions the sync understands.
    private fun <T> request(
        url: String,
        prepare: (HttpURLConnection) -> Unit,
        handle: (Response) -> T,
    ): T {
        var refreshed = false
        while (true) {
            val token = tokens.current()
            val c = URL(url).openConnection() as HttpURLConnection
            try {
                c.connectTimeout = 20_000
                c.readTimeout = 60_000
                c.instanceFollowRedirects = false
                c.setRequestProperty("Authorization", "Bearer $token")
                prepare(c)
                val code = c.responseCode
                when {
                    code == 401 && !refreshed -> {
                        tokens.refresh()
                        refreshed = true
                        continue
                    }
                    code == 401 -> throw RevokedException()
                    code == 429 || code >= 500 -> throw RetryLaterException(retryAfter(c))
                    code == 409 -> throw dropboxError(c)
                    code >= 400 -> throw IOException("Dropbox answered $code: ${errorText(c)}")
                }
                return handle(Response(code, c))
            } finally {
                c.disconnect()
            }
        }
    }

    private fun rpc(path: String, body: JSONObject?): JSONObject = request(
        api + path,
        { c ->
            c.requestMethod = "POST"
            c.doOutput = true
            c.setRequestProperty("Content-Type", "application/json")
            c.outputStream.use { it.write((body?.toString() ?: "null").toByteArray()) }
        },
    ) { r -> JSONObject(r.connection.inputStream.use { it.readBytes() }.toString(Charsets.UTF_8)) }

    override fun list(): List<RemoteFile> {
        val files = ArrayList<RemoteFile>()
        var page = rpc(
            "/2/files/list_folder",
            JSONObject().put("path", "").put("recursive", true).put("include_deleted", false).put("limit", 2000),
        )
        while (true) {
            val entries = page.getJSONArray("entries")
            for (i in 0 until entries.length()) {
                val e = entries.getJSONObject(i)
                if (e.optString(".tag") == "file") files.add(metadata(e))
            }
            if (!page.optBoolean("has_more")) return files
            page = rpc("/2/files/list_folder/continue", JSONObject().put("cursor", page.getString("cursor")))
        }
    }

    override fun read(path: String): Pair<ByteArray, RemoteFile> = request(
        "$content/2/files/download",
        { c ->
            c.requestMethod = "GET"
            c.setRequestProperty("Dropbox-API-Arg", arg(JSONObject().put("path", path)))
        },
    ) { r -> r.connection.inputStream.use { it.readBytes() } to result(r.connection) }

    override fun download(path: String, out: File, from: Long, cancelled: () -> Boolean): RemoteFile = request(
        "$content/2/files/download",
        { c ->
            c.requestMethod = "GET"
            c.readTimeout = 120_000
            c.setRequestProperty("Dropbox-API-Arg", arg(JSONObject().put("path", path)))
            if (from > 0) c.setRequestProperty("Range", "bytes=$from-")
        },
    ) { r ->
        // A server that ignores the range sends the whole file again.
        val append = from > 0 && r.code == 206
        FileOutputStream(out, append).use { sink ->
            r.connection.inputStream.use { copy(it, sink, cancelled) }
        }
        result(r.connection)
    }

    override fun write(path: String, bytes: ByteArray, mode: WriteMode): RemoteFile = try {
        request("$content/2/files/upload", { c -> send(c, commit(path, mode), bytes) }, ::bodyMetadata)
    } catch (e: DropboxError) {
        throw if (e.isConflict) ConflictException() else e
    }

    // A file of any size: one upload up to 150 MiB, an upload session in
    // 64 MiB pieces beyond that. The phone only writes small records, but
    // this is the same contract the desktop follows for bundles.
    fun upload(path: String, file: File, mode: WriteMode): RemoteFile {
        val size = file.length()
        if (size <= SINGLE_UPLOAD) return write(path, file.readBytes(), mode)
        file.inputStream().buffered(1 shl 16).use { input ->
            var offset = 0L
            var session = ""
            for ((index, length) in UploadPieces.of(size).withIndex()) {
                val piece = ByteArray(length.toInt())
                var read = 0
                while (read < piece.size) {
                    val n = input.read(piece, read, piece.size - read)
                    if (n < 0) throw IOException("The file shrank while uploading")
                    read += n
                }
                if (index == 0) {
                    session = request("$content/2/files/upload_session/start", { c -> send(c, JSONObject(), piece) }) { r ->
                        bodyJson(r).getString("session_id")
                    }
                } else {
                    val cursor = JSONObject().put("session_id", session).put("offset", offset)
                    request("$content/2/files/upload_session/append_v2", { c -> send(c, JSONObject().put("cursor", cursor), piece) }) {}
                }
                offset += length
            }
            val finish = JSONObject()
                .put("cursor", JSONObject().put("session_id", session).put("offset", offset))
                .put("commit", commit(path, mode))
            return request("$content/2/files/upload_session/finish", { c -> send(c, finish, ByteArray(0)) }, ::bodyMetadata)
        }
    }

    override fun delete(path: String) {
        try {
            rpc("/2/files/delete_v2", JSONObject().put("path", path))
        } catch (e: DropboxError) {
            if (!e.isNotFound) throw e
        }
    }

    // Who is connected, to show in the settings.
    fun account(): String {
        val account = rpc("/2/users/get_current_account", null)
        val name = account.optJSONObject("name")?.optString("display_name").orEmpty()
        val email = account.optString("email")
        return listOf(name, email).filter { it.isNotEmpty() }.joinToString(" · ")
    }

    // Ends the token on Dropbox's side, so a phone that is lost or sold no
    // longer reaches the folder.
    fun revoke() {
        rpc("/2/auth/token/revoke", null)
    }

    private fun send(c: HttpURLConnection, argument: JSONObject, body: ByteArray) {
        c.requestMethod = "POST"
        c.doOutput = true
        c.setFixedLengthStreamingMode(body.size)
        c.setRequestProperty("Content-Type", "application/octet-stream")
        c.setRequestProperty("Dropbox-API-Arg", arg(argument))
        c.outputStream.use { it.write(body) }
    }

    private fun bodyJson(r: Response) =
        JSONObject(r.connection.inputStream.use { it.readBytes() }.toString(Charsets.UTF_8))

    private fun bodyMetadata(r: Response) = metadata(bodyJson(r))

    private fun result(c: HttpURLConnection): RemoteFile =
        metadata(JSONObject(c.getHeaderField("Dropbox-API-Result") ?: throw IOException("No metadata in the download")))

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
        const val API = "https://api.dropboxapi.com"
        const val CONTENT = "https://content.dropboxapi.com"
        const val SINGLE_UPLOAD = 150L * 1024 * 1024

        fun arg(json: JSONObject) = DropboxArg.escape(json.toString())

        fun commit(path: String, mode: WriteMode): JSONObject = JSONObject()
            .put("path", path)
            .put(
                "mode",
                when (mode) {
                    WriteMode.Add -> "add"
                    WriteMode.Overwrite -> "overwrite"
                    is WriteMode.Update -> JSONObject().put(".tag", "update").put("update", mode.rev)
                },
            )
            .put("autorename", false)
            .put("mute", true)
            // Without it, an update over a file someone deleted, or over the
            // same contents, quietly succeeds; sync wants to hear about both.
            .put("strict_conflict", mode is WriteMode.Update)

        fun metadata(e: JSONObject) = RemoteFile(
            path = e.optString("path_display").ifEmpty { e.getString("path_lower") },
            name = e.getString("name"),
            rev = e.getString("rev"),
            size = e.optLong("size"),
            contentHash = e.optString("content_hash").ifEmpty { null },
        )

        fun retryAfter(c: HttpURLConnection) = c.getHeaderField("Retry-After")?.trim()?.toLongOrNull() ?: 60

        private fun errorText(c: HttpURLConnection) =
            runCatching { c.errorStream?.use { it.readBytes() }?.toString(Charsets.UTF_8) }.getOrNull().orEmpty().take(300)

        private fun dropboxError(c: HttpURLConnection): IOException {
            val text = errorText(c)
            return try {
                val root = JSONObject(text)
                DropboxError(root.optJSONObject("error") ?: JSONObject(), root.optString("error_summary"))
            } catch (e: JSONException) {
                IOException("Dropbox answered 409: $text")
            }
        }
    }
}

// How a large file is cut for an upload session: 64 MiB pieces, a multiple
// of the 4 MiB Dropbox likes and well under its 150 MiB per request.
object UploadPieces {
    const val PIECE = 64L * 1024 * 1024

    fun of(size: Long): List<Long> {
        val pieces = ArrayList<Long>()
        var left = size
        while (left > 0) {
            pieces.add(minOf(PIECE, left))
            left -= PIECE
        }
        return pieces
    }
}

// OAuth 2 with PKCE, without a client secret (docs/sync.md, "Connecting").
object DropboxOAuth {
    const val AUTHORIZE = "https://www.dropbox.com/oauth2/authorize"
    val PORTS = listOf(53682, 53683, 53684)

    private val random = SecureRandom()
    private const val VERIFIER_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"

    fun verifier(): String = (1..64).map { VERIFIER_CHARS[random.nextInt(VERIFIER_CHARS.length)] }.joinToString("")

    fun state(): String = (1..32).map { "0123456789abcdef"[random.nextInt(16)] }.joinToString("")

    fun challenge(verifier: String): String =
        Base64.getUrlEncoder().withoutPadding()
            .encodeToString(MessageDigest.getInstance("SHA-256").digest(verifier.toByteArray(Charsets.US_ASCII)))

    fun redirectUri(port: Int) = "http://localhost:$port/dropbox"

    fun authorizeUrl(appKey: String, verifier: String, state: String, port: Int): String {
        val query = listOf(
            "client_id" to appKey,
            "response_type" to "code",
            "code_challenge" to challenge(verifier),
            "code_challenge_method" to "S256",
            "token_access_type" to "offline",
            "redirect_uri" to redirectUri(port),
            "state" to state,
        ).joinToString("&") { (k, v) -> k + "=" + URLEncoder.encode(v, "UTF-8") }
        return "$AUTHORIZE?$query"
    }

    class Tokens(val access: String, val expiresInSeconds: Long, val refresh: String?)

    // Trades the code from the redirect for a refresh and an access token.
    fun exchange(appKey: String, code: String, verifier: String, port: Int, api: String = DropboxRemote.API) = token(
        api,
        "grant_type" to "authorization_code",
        "code" to code,
        "code_verifier" to verifier,
        "client_id" to appKey,
        "redirect_uri" to redirectUri(port),
    )

    fun refresh(appKey: String, refreshToken: String, api: String = DropboxRemote.API) = token(
        api,
        "grant_type" to "refresh_token",
        "refresh_token" to refreshToken,
        "client_id" to appKey,
    )

    private fun token(api: String, vararg fields: Pair<String, String>): Tokens {
        val c = URL("$api/oauth2/token").openConnection() as HttpURLConnection
        try {
            c.connectTimeout = 20_000
            c.readTimeout = 30_000
            c.requestMethod = "POST"
            c.doOutput = true
            c.setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
            val body = fields.joinToString("&") { (k, v) -> k + "=" + URLEncoder.encode(v, "UTF-8") }
            c.outputStream.use { it.write(body.toByteArray()) }
            val code = c.responseCode
            if (code == 429 || code >= 500) throw RetryLaterException(DropboxRemote.retryAfter(c))
            // A refresh token the user revoked comes back as invalid_grant.
            if (code == 400 || code == 401) throw RevokedException()
            if (code >= 400) throw IOException("Dropbox answered $code to the token request")
            val json = JSONObject(c.inputStream.use { it.readBytes() }.toString(Charsets.UTF_8))
            return Tokens(
                json.getString("access_token"),
                json.optLong("expires_in", 14_400),
                json.optString("refresh_token").ifEmpty { null },
            )
        } finally {
            c.disconnect()
        }
    }
}

// Access tokens from a refresh token, kept in memory and renewed a minute
// before Dropbox says they expire.
class RefreshingTokens(
    private val appKey: String,
    private val refreshToken: String,
    private val api: String = DropboxRemote.API,
    private val clock: () -> Long = System::currentTimeMillis,
) : AccessTokens {
    private var access: String? = null
    private var expires = 0L

    @Synchronized
    override fun current(): String {
        val token = access
        return if (token != null && clock() < expires) token else refresh()
    }

    @Synchronized
    override fun refresh(): String {
        val tokens = DropboxOAuth.refresh(appKey, refreshToken, api)
        access = tokens.access
        expires = clock() + (tokens.expiresInSeconds - 60).coerceAtLeast(60) * 1000
        return tokens.access
    }
}

// Waits on 127.0.0.1 for the browser to come back from Dropbox with the code,
// on the first of the registered ports that is free.
class LoopbackReceiver private constructor(private val server: java.net.ServerSocket) {
    val port get() = server.localPort


    class Redirect(val code: String?, val state: String?, val error: String?)

    // Takes requests until one arrives at /dropbox (a browser may ask for a
    // favicon first), answers it with page(redirect), and stops listening.
    // Null when nothing came within the timeout.
    fun await(timeoutMs: Int, page: (Redirect) -> String): Redirect? {
        server.soTimeout = timeoutMs
        val deadline = System.currentTimeMillis() + timeoutMs
        try {
            while (System.currentTimeMillis() < deadline) {
                val socket = try {
                    server.accept()
                } catch (e: java.net.SocketTimeoutException) {
                    return null
                }
                socket.use { s ->
                    s.soTimeout = 10_000
                    val line = s.getInputStream().bufferedReader(Charsets.US_ASCII).readLine().orEmpty()
                    val target = line.split(' ').getOrNull(1).orEmpty()
                    val out = s.getOutputStream()
                    if (!target.startsWith("/dropbox")) {
                        out.write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".toByteArray())
                        return@use
                    }
                    val query = parseQuery(target.substringAfter('?', ""))
                    val redirect = Redirect(query["code"], query["state"], query["error"])
                    val html = page(redirect).toByteArray(Charsets.UTF_8)
                    out.write(
                        ("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: ${html.size}\r\n" +
                            "Cache-Control: no-store\r\nConnection: close\r\n\r\n").toByteArray(),
                    )
                    out.write(html)
                    out.flush()
                    return redirect
                }
            }
            return null
        } finally {
            close()
        }
    }

    fun close() = runCatching { server.close() }

    companion object {
        fun open(ports: List<Int> = DropboxOAuth.PORTS): LoopbackReceiver {
            for (port in ports) {
                try {
                    val server = java.net.ServerSocket(port, 1, java.net.InetAddress.getByName("127.0.0.1"))
                    return LoopbackReceiver(server)
                } catch (e: IOException) {
                    continue
                }
            }
            throw IOException("None of the ports Dropbox knows is free")
        }

        fun parseQuery(query: String): Map<String, String> =
            query.split('&').filter { it.contains('=') }.associate {
                val (k, v) = it.split('=', limit = 2)
                java.net.URLDecoder.decode(k, "UTF-8") to java.net.URLDecoder.decode(v, "UTF-8")
            }
    }
}
