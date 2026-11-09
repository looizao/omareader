package io.github.looizao.omareader

import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.net.HttpURLConnection
import java.net.InetSocketAddress
import java.net.URL
import java.util.Collections

// DropboxRemote and the OAuth pieces against a local server that answers
// the way Dropbox's documentation says it does.
class DropboxTest {
    @get:Rule
    val temp = TemporaryFolder()

    private val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0).apply { start() }
    private val base get() = "http://127.0.0.1:${server.address.port}"
    private val requests = Collections.synchronizedList(ArrayList<String>())

    @After
    fun stop() = server.stop(0)

    private fun route(path: String, handler: (HttpExchange) -> Pair<Int, String>) {
        server.createContext(path) { exchange ->
            val body = exchange.requestBody.readBytes().toString(Charsets.UTF_8)
            requests.add("${exchange.requestMethod} ${exchange.requestURI.path} $body")
            val (code, text) = handler(exchange)
            val bytes = text.toByteArray()
            exchange.sendResponseHeaders(code, if (bytes.isEmpty()) -1 else bytes.size.toLong())
            if (bytes.isNotEmpty()) exchange.responseBody.use { it.write(bytes) }
            exchange.close()
        }
    }

    private class Tokens : AccessTokens {
        var token = "old"
        var refreshes = 0
        override fun current() = token
        override fun refresh(): String {
            refreshes++
            token = "new"
            return token
        }
    }

    private val tokens = Tokens()
    private val remote by lazy { DropboxRemote(tokens, base, base) }

    private fun file(path: String, rev: String) =
        """{".tag":"file","name":"${path.substringAfterLast('/')}","path_lower":"$path","path_display":"$path",
            "rev":"$rev","size":3,"content_hash":"abc"}"""

    @Test
    fun listingFollowsEveryPageAndSkipsFolders() {
        route("/2/files/list_folder") {
            200 to """{"entries":[{".tag":"folder","name":"state","path_lower":"/state"},${file("/state/a.json", "r1")}],
                "cursor":"c1","has_more":true}"""
        }
        route("/2/files/list_folder/continue") {
            200 to """{"entries":[${file("/books/a.omabook", "r2")}],"cursor":"c2","has_more":false}"""
        }
        val files = remote.list()
        assertEquals(listOf("/state/a.json", "/books/a.omabook"), files.map { it.path })
        assertEquals("r2", files[1].rev)
        assertTrue(requests.any { it.contains("\"cursor\":\"c1\"") })
    }

    @Test
    fun anExpiredTokenIsRefreshedOnce() {
        route("/2/files/list_folder") { exchange ->
            if (exchange.requestHeaders.getFirst("Authorization") == "Bearer new") {
                200 to """{"entries":[],"cursor":"c","has_more":false}"""
            } else {
                401 to """{"error_summary":"expired_access_token/","error":{".tag":"expired_access_token"}}"""
            }
        }
        assertTrue(remote.list().isEmpty())
        assertEquals(1, tokens.refreshes)
    }

    @Test
    fun aTokenThatStaysRefusedMeansTheAppWasDisconnected() {
        route("/2/files/list_folder") { 401 to """{"error":{".tag":"invalid_access_token"}}""" }
        try {
            remote.list()
            fail()
        } catch (e: RevokedException) {
            assertEquals(1, tokens.refreshes)
        }
    }

    @Test
    fun busyServersSayWhenToComeBack() {
        route("/2/files/list_folder") { exchange ->
            exchange.responseHeaders.add("Retry-After", "7")
            429 to """{"error":{"reason":{".tag":"too_many_requests"}}}"""
        }
        try {
            remote.list()
            fail()
        } catch (e: RetryLaterException) {
            assertEquals(7, e.seconds)
        }
    }

    @Test
    fun aStaleRevIsAConflict() {
        route("/2/files/upload") { exchange ->
            val arg = JSONObject(exchange.requestHeaders.getFirst("Dropbox-API-Arg"))
            assertEquals("r1", arg.getJSONObject("mode").getString("update"))
            assertTrue(arg.getBoolean("strict_conflict"))
            409 to """{"error_summary":"path/conflict/file/","error":{".tag":"path","reason":{".tag":"conflict",
                "conflict":{".tag":"file"}},"upload_session_id":"s"}}"""
        }
        try {
            remote.write("/state/a.json", "{}".toByteArray(), WriteMode.Update("r1"))
            fail()
        } catch (e: ConflictException) {
            // What the engine retries on.
        }
    }

    @Test
    fun writesSendTheirArgumentEscapedAndReturnTheNewRev() {
        route("/2/files/upload") { exchange ->
            val raw = exchange.requestHeaders.getFirst("Dropbox-API-Arg")
            assertFalse(raw.any { it.code > 0x7e })
            assertEquals("add", JSONObject(raw).getString("mode"))
            assertEquals("/state/é.json", JSONObject(raw).getString("path"))
            200 to file("/state/é.json", "r9").replace("\n", "")
        }
        assertEquals("r9", remote.write("/state/é.json", "{}".toByteArray(), WriteMode.Add).rev)
    }

    @Test
    fun deletingWhatIsAlreadyGoneIsFine() {
        route("/2/files/delete_v2") {
            409 to """{"error_summary":"path_lookup/not_found/","error":{".tag":"path_lookup","path_lookup":{".tag":"not_found"}}}"""
        }
        remote.delete("/books/a.omabook")
    }

    @Test
    fun downloadsResumeWithARange() {
        route("/2/files/download") { exchange ->
            val meta = file("/books/a.omabook", "r3").replace("\n", "")
            exchange.responseHeaders.add("Dropbox-API-Result", meta)
            if (exchange.requestHeaders.getFirst("Range") == "bytes=4-") 206 to "456789" else 200 to "0123456789"
        }
        val out = temp.newFile()
        out.writeText("0123")
        val meta = remote.download("/books/a.omabook", out, 4) { false }
        assertEquals("0123456789", out.readText())
        assertEquals("r3", meta.rev)
        // A server that ignores the range starts the file over.
        out.writeText("xx")
        server.removeContext("/2/files/download")
        route("/2/files/download") { exchange ->
            exchange.responseHeaders.add("Dropbox-API-Result", file("/books/a.omabook", "r3").replace("\n", ""))
            200 to "0123456789"
        }
        remote.download("/books/a.omabook", out, 2) { false }
        assertEquals("0123456789", out.readText())
    }

    @Test
    fun largeUploadsAreCutIntoPieces() {
        assertEquals(listOf(UploadPieces.PIECE, UploadPieces.PIECE, 5L), UploadPieces.of(2 * UploadPieces.PIECE + 5))
        assertTrue(UploadPieces.of(0).isEmpty())
        assertEquals(0L, UploadPieces.PIECE % ContentHash.BLOCK)
    }

    @Test
    fun pkceAndTheAuthorizeAddress() {
        val verifier = DropboxOAuth.verifier()
        assertEquals(64, verifier.length)
        assertTrue(verifier.all { it.isLetterOrDigit() || it in "-._~" })
        // Worked out separately, with Python's hashlib and base64.
        assertEquals("bwWFMyPfdG9qreDhH2lmftFx_dFeLDalzcT1gb_j68g", DropboxOAuth.challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r7wW1gFWFOEjXk"))
        val url = DropboxOAuth.authorizeUrl("key", verifier, "st", 53683)
        val query = LoopbackReceiver.parseQuery(url.substringAfter('?'))
        assertEquals("offline", query["token_access_type"])
        assertEquals("S256", query["code_challenge_method"])
        assertEquals("http://localhost:53683/dropbox", query["redirect_uri"])
        assertEquals("st", query["state"])
    }

    @Test
    fun tokensAreExchangedAndRefreshedWithoutASecret() {
        route("/oauth2/token") { exchange ->
            assertEquals("application/x-www-form-urlencoded", exchange.requestHeaders.getFirst("Content-Type"))
            200 to """{"access_token":"a1","expires_in":14400,"refresh_token":"r1","token_type":"bearer"}"""
        }
        val tokens = DropboxOAuth.exchange("key", "code", "verifier", 53682, base)
        assertEquals("r1", tokens.refresh)
        val body = requests.single()
        assertTrue(body.contains("code_verifier=verifier") && body.contains("client_id=key"))
        assertFalse(body.contains("secret"))

        var now = 0L
        val refreshing = RefreshingTokens("key", "r1", base) { now }
        assertEquals("a1", refreshing.current())
        assertEquals("a1", refreshing.current())
        assertEquals(2, requests.size)
        now = 14_400_000
        refreshing.current()
        assertEquals(3, requests.size)
    }

    @Test
    fun aRevokedRefreshTokenDisconnects() {
        route("/oauth2/token") { 400 to """{"error":"invalid_grant"}""" }
        try {
            RefreshingTokens("key", "r1", base).current()
            fail()
        } catch (e: RevokedException) {
            // Expected.
        }
    }

    @Test
    fun theBrowserComesBackToTheLoopback() {
        // Port 0 lets the system choose, so the test never collides with a
        // real connection in progress.
        val receiver = LoopbackReceiver.open(listOf(0))
        val port = receiver.port
        val result = arrayOfNulls<LoopbackReceiver.Redirect>(1)
        val thread = Thread { result[0] = receiver.await(5_000) { r -> "<p>${r.code}</p>" } }
        thread.start()
        // A favicon first, as browsers do, then the redirect.
        assertEquals(404, get("http://127.0.0.1:$port/favicon.ico").first)
        val (code, page) = get("http://127.0.0.1:$port/dropbox?code=abc%2B1&state=xyz")
        thread.join(5_000)
        assertEquals(200, code)
        assertEquals("<p>abc+1</p>", page)
        assertEquals("abc+1", result[0]!!.code)
        assertEquals("xyz", result[0]!!.state)
        assertNull(result[0]!!.error)
    }

    private fun get(url: String): Pair<Int, String> {
        val c = URL(url).openConnection() as HttpURLConnection
        return try {
            val code = c.responseCode
            code to (if (code < 400) c.inputStream.use { it.readBytes() }.toString(Charsets.UTF_8) else "")
        } finally {
            c.disconnect()
        }
    }
}
