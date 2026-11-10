package io.github.looizao.omareader

import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import org.json.JSONArray
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.IOException
import java.net.InetSocketAddress
import java.net.URLDecoder
import java.util.Collections

// DriveRemote against a local server that keeps files the way Drive's v3
// API describes them: properties, revisions, the trash, paged listings and
// resumable uploads.
class DriveTest {
    @get:Rule
    val temp = TemporaryFolder()

    private val drive = FakeDrive()
    private val tokens = Tokens()
    private val sleeps = Collections.synchronizedList(ArrayList<Long>())

    private fun remote(piece: Int = DriveRemote.PIECE) =
        DriveRemote(tokens, drive.base, { sleeps.add(it) }, { JITTER }, piece)

    @After
    fun stop() = drive.stop()

    private class Tokens : AccessTokens {
        var token = "good"
        var refreshes = 0
        var refreshTo = "good"
        override fun current() = token
        override fun refresh(): String {
            refreshes++
            token = refreshTo
            return token
        }
    }

    @Test
    fun theFolderIsMadeOnceAtTheTopAndReused() {
        remote().write("/state/a.json", "{}".toByteArray(), WriteMode.Add)
        remote().write("/state/b.json", "{}".toByteArray(), WriteMode.Add)
        val folders = drive.files.filter { it.folder }
        assertEquals(1, folders.size)
        val folder = folders.single()
        assertEquals("omareader", folder.name)
        assertEquals(listOf("root"), folder.parents)
        assertEquals("folder", folder.properties["omareader"])
        val files = drive.files.filter { !it.folder }
        assertEquals(setOf("a.json", "b.json"), files.map { it.name }.toSet())
        assertTrue(files.all { it.parents == listOf(folder.id) && it.properties["omareader"] == "file" })
        assertEquals("/state/a.json", files.first { it.name == "a.json" }.properties["omareaderPath"])
    }

    @Test
    fun ofTwoFoldersTheFirstMadeHoldsNewFiles() {
        drive.add(DFile(id = "z", name = "omareader", properties = mapOf("omareader" to "folder"), folder = true, created = 1))
        drive.add(DFile(id = "y", name = "omareader", properties = mapOf("omareader" to "folder"), folder = true, created = 2))
        drive.add(DFile(id = "x", name = "omareader", properties = mapOf("omareader" to "folder"), folder = true, created = 1))
        remote().write("/settings.json", "{}".toByteArray(), WriteMode.Add)
        assertEquals(listOf("x"), drive.files.single { !it.folder }.parents)
    }

    @Test
    fun theListingFollowsEveryPageAndNamesFilesByTheirPath() {
        drive.addFile("/state/a.json", "a")
        drive.addFile("/state/b.json", "b")
        drive.addFile("/settings.json", "s")
        drive.addFile("/books/dune.omabook", "0123456789", name = "Dune.omabook")
        drive.addFile("/state/c.json", "c").trashed = true
        // Someone else's file without the properties is never listed.
        drive.add(DFile(id = "other", name = "notes.txt"))
        val files = remote().list().associateBy { it.path }
        assertEquals(setOf("/state/a.json", "/state/b.json", "/settings.json", "/books/dune.omabook"), files.keys)
        val bundle = files.getValue("/books/dune.omabook")
        assertEquals("dune.omabook", bundle.name)
        assertEquals(10L, bundle.size)
        assertEquals(drive.byPath("/books/dune.omabook").rev, bundle.rev)
        assertEquals(DriveRemote.sha256("0123456789".byteInputStream()), bundle.contentHash)
        assertTrue(drive.requests.any { it.contains("pageToken=") })
        val q = drive.queries.first()
        assertTrue(q, q.contains("properties has {key='omareader' and value='file'}") && q.contains("trashed=false"))
    }

    @Test
    fun duplicatesCollapseAndTheExtrasGoToTheTrash() {
        // Records keep the copy made first, then the smaller id.
        drive.addFile("/state/a.json", "late", id = "a1", created = 5)
        drive.addFile("/state/a.json", "early", id = "a3", created = 2)
        drive.addFile("/state/a.json", "early too", id = "a2", created = 2)
        // Bundles keep the copy changed last, then the larger id.
        drive.addFile("/books/b.omabook", "old", id = "b1", modified = 1)
        drive.addFile("/books/b.omabook", "new", id = "b2", modified = 9)
        drive.addFile("/books/b.omabook", "new too", id = "b3", modified = 9)
        val remote = remote()
        val files = remote.list().associateBy { it.path }
        assertEquals(2, files.size)
        assertEquals(drive.byId("a2").rev, files.getValue("/state/a.json").rev)
        assertEquals(drive.byId("b3").rev, files.getValue("/books/b.omabook").rev)
        assertEquals(setOf("a1", "a3", "b1", "b2"), drive.files.filter { it.trashed }.map { it.id }.toSet())
        // Nothing is deleted for good.
        assertEquals(6, drive.files.size)
        assertEquals("early too", String(remote.read("/state/a.json").first))
    }

    @Test
    fun addingOverAnExistingFileIsAConflict() {
        drive.addFile("/state/a.json", "theirs")
        try {
            remote().write("/state/a.json", "mine".toByteArray(), WriteMode.Add)
            fail()
        } catch (e: ConflictException) {
            assertEquals(1, drive.files.count { !it.folder })
            assertEquals("theirs", String(drive.byPath("/state/a.json").bytes))
        }
    }

    @Test
    fun anUpdateOverAMovedRevisionIsAConflict() {
        val file = drive.addFile("/state/a.json", "one")
        val remote = remote()
        val listed = remote.list().single()
        // Another device writes after this one listed.
        drive.write(file, "two".toByteArray())
        try {
            remote.write("/state/a.json", "mine".toByteArray(), WriteMode.Update(listed.rev))
            fail()
        } catch (e: ConflictException) {
            assertEquals("two", String(file.bytes))
        }
        val written = remote.write("/state/a.json", "three".toByteArray(), WriteMode.Update(file.rev))
        assertEquals("three", String(file.bytes))
        assertEquals(file.rev, written.rev)
        assertEquals("/state/a.json", written.path)
        assertTrue(drive.requests.any { it.startsWith("PATCH /upload/drive/v3/files/${file.id}") && it.contains("uploadType=media") })
        // A file someone trashed is gone as far as an update goes.
        file.trashed = true
        try {
            remote.write("/state/a.json", "four".toByteArray(), WriteMode.Update(file.rev))
            fail()
        } catch (e: ConflictException) {
            assertEquals("three", String(file.bytes))
        }
    }

    @Test
    fun bundlesGoUpInPiecesAndResumeAfterADroppedConnection() {
        val source = temp.newFile().apply { writeText("0123456789") }
        // The second piece gets two bytes in and the connection drops.
        var pieces = 0
        drive.onPut = { exchange, range ->
            if (range == "bytes 4-7/10" && pieces++ == 0) {
                drive.receive(exchange, 2)
                true
            } else {
                false
            }
        }
        val written = remote(piece = 4).upload("/books/dune.omabook", source, "Dune.omabook")
        val file = drive.byPath("/books/dune.omabook")
        assertEquals("0123456789", String(file.bytes))
        assertEquals("Dune.omabook", file.name)
        assertEquals(file.rev, written.rev)
        assertEquals(listOf("bytes 0-3/10", "bytes 4-7/10", "bytes */10", "bytes 6-9/10"), drive.ranges)
        assertEquals(listOf(1000 + JITTER), sleeps)

        // A new export of the same book replaces the file's contents.
        source.writeText("abcdefgh")
        drive.ranges.clear()
        remote(piece = 4).upload("/books/dune.omabook", source, "Dune.omabook")
        assertEquals(1, drive.files.count { !it.folder })
        assertEquals("abcdefgh", String(file.bytes))
        assertTrue(drive.requests.any { it.startsWith("PATCH /upload/drive/v3/files/${file.id}") && it.contains("uploadType=resumable") })
    }

    @Test
    fun anExpiredUploadStartsOver() {
        var expired = false
        drive.onPut = { exchange, _ ->
            if (!expired) {
                expired = true
                exchange.sendResponseHeaders(404, -1)
                exchange.close()
                true
            } else {
                false
            }
        }
        remote(piece = 4).write("/books/a.omabook", "0123456789".toByteArray(), WriteMode.Overwrite)
        assertEquals("0123456789", String(drive.byPath("/books/a.omabook").bytes))
        assertEquals(2, drive.sessions)
    }

    @Test
    fun downloadsResumeWithARange() {
        drive.addFile("/books/a.omabook", "0123456789")
        val remote = remote()
        remote.list()
        val out = temp.newFile().apply { writeText("0123") }
        remote.download("/books/a.omabook", out, 4) { false }
        assertEquals("0123456789", out.readText())
        assertTrue(drive.headers.any { it == "bytes=4-" })
        // A server that ignores the range starts the file over.
        drive.ignoreRange = true
        out.writeText("xx")
        remote.download("/books/a.omabook", out, 2) { false }
        assertEquals("0123456789", out.readText())
    }

    @Test
    fun deletingMovesToTheTrashAndWhatIsGoneIsFine() {
        val file = drive.addFile("/state/a.json", "a")
        val remote = remote()
        remote.list()
        remote.delete("/state/a.json")
        assertTrue(file.trashed)
        assertTrue(file in drive.files)
        // Gone from Drive altogether since the listing.
        val other = drive.addFile("/state/b.json", "b")
        remote.list()
        drive.files.remove(other)
        remote.delete("/state/b.json")
        remote.delete("/state/never.json")
    }

    @Test
    fun aRefusedTokenIsRenewedOnce() {
        drive.addFile("/state/a.json", "a")
        tokens.token = "stale"
        assertEquals(1, remote().list().size)
        assertEquals(1, tokens.refreshes)
        assertEquals("good", tokens.token)
    }

    @Test
    fun aTokenThatStaysRefusedNeedsTheUser() {
        tokens.token = "stale"
        tokens.refreshTo = "still stale"
        try {
            remote().list()
            fail()
        } catch (e: RevokedException) {
            assertEquals(1, tokens.refreshes)
        }
    }

    @Test
    fun busyMomentsBackOffAndThenSucceed() {
        drive.addFile("/state/a.json", "a")
        var answers = listOf(
            429 to "{}",
            403 to """{"error":{"errors":[{"domain":"usageLimits","reason":"userRateLimitExceeded"}],"code":403}}""",
            503 to "{}",
        )
        drive.intercept = { exchange ->
            if (exchange.requestURI.path == "/drive/v3/files" && answers.isNotEmpty()) {
                answers.first().also { answers = answers.drop(1) }
            } else {
                null
            }
        }
        assertEquals(1, remote().list().size)
        assertEquals(listOf(1000 + JITTER, 2000 + JITTER, 4000 + JITTER), sleeps)
    }

    @Test
    fun aDriveThatStaysBusyEndsTheSync() {
        drive.intercept = { exchange ->
            exchange.responseHeaders.add("Retry-After", "30")
            429 to "{}"
        }
        try {
            remote().list()
            fail()
        } catch (e: RetryLaterException) {
            assertEquals(30, e.seconds)
            assertEquals(listOf(1000 + JITTER, 2000 + JITTER, 4000 + JITTER), sleeps)
            assertEquals(4, drive.requests.size)
        }
    }

    @Test
    fun aRefusalThatIsNotARateLimitIsNotRetried() {
        drive.intercept = { 403 to """{"error":{"errors":[{"reason":"insufficientFilePermissions"}],"code":403}}""" }
        try {
            remote().list()
            fail()
        } catch (e: IOException) {
            assertFalse(e is RetryLaterException)
            assertTrue(sleeps.isEmpty())
        }
    }

    @Test
    fun theAccountComesFromAbout() {
        val user = remote().account()
        assertEquals("Ana Souza", user.name)
        assertEquals("ana@example.com", user.email)
        assertEquals("Ana Souza · ana@example.com", user.label)
        assertTrue(drive.requests.single().contains("fields=user%28displayName%2CemailAddress%29"))
    }

    @Test
    fun downloadsAreCheckedAgainstThePlainSha256() {
        // Worked out separately, with sha256sum.
        assertEquals(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            remote().contentHash(temp.newFile().apply { writeText("abc") }),
        )
        val state = """{"format":"omareader-state","version":1,"id":"b1","present":{"value":true,"at":1,"by":"bbbb"}}"""
        drive.addFile("/state/b1.json", state)
        val bundle = drive.addFile("/books/b1.omabook", "the whole book")

        bundle.sha256 = DriveRemote.sha256("another book".byteInputStream())
        val store = Store(temp.newFolder())
        try {
            SyncEngine(remote(), store).sync(downloadBundles = true)
            fail()
        } catch (e: IOException) {
            assertTrue(store.imported.isEmpty())
        }

        bundle.sha256 = null
        SyncEngine(remote(), store).sync(downloadBundles = true)
        assertEquals(listOf("the whole book"), store.imported)

        drive.write(bundle, "a new export".toByteArray())
        SyncEngine(remote(), store).sync(downloadBundles = true)
        assertEquals(listOf("the whole book", "a new export"), store.imported)
    }

    @Test
    fun theEngineSyncsStateThroughDrive() {
        val store = Store(temp.newFolder())
        store.states["b1"] = BookState("b1", "Dune", "Herbert", Register(true, 5, "aaaa"))
        SyncEngine(remote(), store).sync(downloadBundles = false)
        val written = BookState.parse(String(drive.byPath("/state/b1.json").bytes))
        assertEquals(Register(true, 5L, "aaaa"), written.present)

        // A change from another device comes back, and this side's newer
        // one goes over it as an update of the revision it read.
        store.states["b1"] = store.states.getValue("b1").copy(present = Register(false, 9, "aaaa"))
        SyncEngine(remote(), store).sync(downloadBundles = false)
        assertEquals(false, BookState.parse(String(drive.byPath("/state/b1.json").bytes)).present?.value)
        assertEquals(1, drive.files.count { !it.folder && !it.trashed })
    }

    private class Store(private val dir: File) : SyncStore {
        override val device = "aaaa"
        val states = HashMap<String, BookState>()
        val cache = HashMap<String, Pair<String, String>>()
        val bundles = HashMap<String, String>()
        val imported = ArrayList<String>()

        override fun localIds() = states.keys.toSet()
        override fun localState(id: String) = states[id]
        override fun applyState(merged: BookState) {
            states[merged.id] = merged
        }
        override fun localSettings() = SyncSettings()
        override fun applySettings(merged: SyncSettings) = Unit
        override fun cached(path: String) = cache[path]
        override fun cache(path: String, rev: String, text: String) {
            cache[path] = rev to text
        }
        override fun forget(path: String) {
            cache.remove(path)
        }
        override fun bundleRev(id: String) = bundles[id]
        override fun partialFile(id: String, rev: String) = File(dir, "$id-$rev.part")
        override fun importBundle(id: String, file: File, rev: String) {
            bundles[id] = rev
            imported.add(file.readText())
        }
    }

    class DFile(
        val id: String,
        var name: String,
        val properties: Map<String, String> = emptyMap(),
        var parents: List<String> = emptyList(),
        var bytes: ByteArray = ByteArray(0),
        val folder: Boolean = false,
        val created: Int = 0,
        var modified: Int = 0,
        var trashed: Boolean = false,
        var rev: String = "",
        var sha256: String? = null,
    )

    // Drive, as far as omareader uses it. Times are seconds into a day so
    // tests can order copies; revisions and ids count up.
    class FakeDrive {
        private val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        val base get() = "http://127.0.0.1:${server.address.port}"
        val files = Collections.synchronizedList(ArrayList<DFile>())
        val requests = Collections.synchronizedList(ArrayList<String>())
        val queries = Collections.synchronizedList(ArrayList<String>())
        val ranges = Collections.synchronizedList(ArrayList<String>())
        val headers = Collections.synchronizedList(ArrayList<String>())
        var intercept: ((HttpExchange) -> Pair<Int, String>?)? = null
        // Takes over a PUT to an upload session when it returns true.
        var onPut: ((HttpExchange, String) -> Boolean)? = null
        var ignoreRange = false
        var sessions = 0
        private var counter = 100
        private val uploads = HashMap<Int, Upload>()

        private class Upload(val target: DFile?, val metadata: JSONObject, val total: Long) {
            val received = java.io.ByteArrayOutputStream()
        }

        init {
            server.createContext("/") { exchange ->
                try {
                    handle(exchange)
                } catch (e: Exception) {
                    e.printStackTrace()
                    runCatching { exchange.sendResponseHeaders(500, -1) }
                } finally {
                    exchange.close()
                }
            }
            server.start()
        }

        fun stop() = server.stop(0)

        fun add(file: DFile): DFile {
            if (file.rev.isEmpty()) file.rev = "h${++counter}"
            files.add(file)
            return file
        }

        fun addFile(path: String, text: String, name: String = path.substringAfterLast('/'), id: String = "f${++counter}", created: Int = ++counter % 60, modified: Int = created): DFile {
            val bytes = text.toByteArray()
            return add(
                DFile(
                    id = id, name = name, properties = mapOf("omareader" to "file", "omareaderPath" to path),
                    bytes = bytes, created = created, modified = modified,
                    sha256 = DriveRemote.sha256(bytes.inputStream()),
                ),
            )
        }

        fun write(file: DFile, bytes: ByteArray) {
            file.bytes = bytes
            file.rev = "h${++counter}"
            file.modified = counter % 60
            file.sha256 = DriveRemote.sha256(bytes.inputStream())
        }

        fun byPath(path: String) = files.single { !it.trashed && it.properties["omareaderPath"] == path }
        fun byId(id: String) = files.single { it.id == id }

        // Reads part of a piece, then drops the connection.
        fun receive(exchange: HttpExchange, bytes: Int) {
            val session = exchange.requestURI.path.substringAfterLast('/').toInt()
            val body = exchange.requestBody.readBytes()
            uploads.getValue(session).received.write(body, 0, bytes)
            exchange.close()
        }

        private fun time(seconds: Int) = "2026-11-09T10:00:%02dZ".format(seconds)

        private fun json(f: DFile) = JSONObject()
            .put("id", f.id).put("name", f.name).put("properties", JSONObject(f.properties))
            .put("headRevisionId", f.rev).put("size", f.bytes.size.toString())
            .put("createdTime", time(f.created)).put("modifiedTime", time(f.modified))
            .apply { f.sha256?.let { put("sha256Checksum", it) } }

        private fun reply(exchange: HttpExchange, code: Int, body: String) {
            val bytes = body.toByteArray()
            exchange.sendResponseHeaders(code, if (bytes.isEmpty()) -1 else bytes.size.toLong())
            if (bytes.isNotEmpty()) exchange.responseBody.use { it.write(bytes) }
        }

        private fun handle(exchange: HttpExchange) {
            val method = exchange.requestHeaders.getFirst("X-HTTP-Method-Override") ?: exchange.requestMethod
            val path = exchange.requestURI.path
            val query = (exchange.requestURI.rawQuery ?: "").split('&').filter { it.contains('=') }.associate {
                val (k, v) = it.split('=', limit = 2)
                k to URLDecoder.decode(v, "UTF-8")
            }
            requests.add("$method $path?${exchange.requestURI.rawQuery ?: ""}")
            intercept?.invoke(exchange)?.let { (code, body) -> return reply(exchange, code, body) }
            if (!path.startsWith("/upload/session/") &&
                exchange.requestHeaders.getFirst("Authorization") != "Bearer good"
            ) {
                return reply(exchange, 401, """{"error":{"code":401}}""")
            }
            val id = path.substringAfterLast('/')
            when {
                method == "GET" && path == "/drive/v3/about" ->
                    reply(exchange, 200, """{"user":{"displayName":"Ana Souza","emailAddress":"ana@example.com"}}""")
                method == "GET" && path == "/drive/v3/files" -> list(exchange, query)
                method == "POST" && path == "/drive/v3/files" -> {
                    val m = JSONObject(exchange.requestBody.readBytes().toString(Charsets.UTF_8))
                    assertEquals(DriveRemote.FOLDER_TYPE, m.getString("mimeType"))
                    val f = add(
                        DFile(
                            "d${++counter}", m.getString("name"), props(m), parents(m), folder = true, created = counter % 60,
                        ),
                    )
                    reply(exchange, 200, JSONObject().put("id", f.id).toString())
                }
                method == "GET" && path.startsWith("/drive/v3/files/") -> {
                    val f = files.firstOrNull { it.id == id } ?: return reply(exchange, 404, "{}")
                    if (query["alt"] == "media") {
                        val range = exchange.requestHeaders.getFirst("Range")
                        range?.let { headers.add(it) }
                        if (range != null && !ignoreRange) {
                            val from = range.removePrefix("bytes=").removeSuffix("-").toInt()
                            exchange.sendResponseHeaders(206, (f.bytes.size - from).toLong())
                            exchange.responseBody.use { it.write(f.bytes, from, f.bytes.size - from) }
                        } else {
                            exchange.sendResponseHeaders(200, f.bytes.size.toLong())
                            exchange.responseBody.use { it.write(f.bytes) }
                        }
                    } else {
                        reply(exchange, 200, json(f).put("trashed", f.trashed).toString())
                    }
                }
                method == "PATCH" && path.startsWith("/drive/v3/files/") -> {
                    val f = files.firstOrNull { it.id == id } ?: return reply(exchange, 404, "{}")
                    val m = JSONObject(exchange.requestBody.readBytes().toString(Charsets.UTF_8))
                    if (m.optBoolean("trashed")) f.trashed = true
                    reply(exchange, 200, JSONObject().put("id", f.id).toString())
                }
                path.startsWith("/upload/drive/v3/files") && query["uploadType"] == "multipart" -> {
                    assertEquals("POST", method)
                    val (m, bytes) = multipart(exchange)
                    val f = add(DFile("f${++counter}", m.getString("name"), props(m), parents(m), created = counter % 60))
                    write(f, bytes)
                    reply(exchange, 200, json(f).toString())
                }
                path.startsWith("/upload/drive/v3/files/") && query["uploadType"] == "media" -> {
                    assertEquals("PATCH", method)
                    val f = files.firstOrNull { it.id == id } ?: return reply(exchange, 404, "{}")
                    write(f, exchange.requestBody.readBytes())
                    reply(exchange, 200, json(f).toString())
                }
                path.startsWith("/upload/drive/v3/files") && query["uploadType"] == "resumable" -> {
                    val target = if (method == "PATCH") files.first { it.id == id } else null
                    val m = JSONObject(exchange.requestBody.readBytes().toString(Charsets.UTF_8))
                    val total = exchange.requestHeaders.getFirst("X-Upload-Content-Length").toLong()
                    val session = ++sessions
                    uploads[session] = Upload(target, m, total)
                    exchange.responseHeaders.add("Location", "$base/upload/session/$session")
                    reply(exchange, 200, "")
                }
                method == "PUT" && path.startsWith("/upload/session/") -> {
                    val range = exchange.requestHeaders.getFirst("Content-Range")
                    ranges.add(range)
                    if (onPut?.invoke(exchange, range) == true) return
                    val upload = uploads[id.toInt()] ?: return reply(exchange, 404, "")
                    val body = exchange.requestBody.readBytes()
                    if (!range.startsWith("bytes */")) {
                        val from = range.removePrefix("bytes ").substringBefore('-').toLong()
                        assertEquals(upload.received.size().toLong(), from)
                        upload.received.write(body)
                    }
                    val got = upload.received.size().toLong()
                    if (got < upload.total) {
                        if (got > 0) exchange.responseHeaders.add("Range", "bytes=0-${got - 1}")
                        return reply(exchange, 308, "")
                    }
                    val f = upload.target ?: add(
                        DFile("f${++counter}", upload.metadata.getString("name"), props(upload.metadata), parents(upload.metadata), created = counter % 60),
                    )
                    upload.metadata.optString("name").ifEmpty { null }?.let { f.name = it }
                    write(f, upload.received.toByteArray())
                    reply(exchange, if (upload.target == null) 201 else 200, json(f).toString())
                }
                else -> reply(exchange, 400, """{"error":{"message":"unexpected $method $path"}}""")
            }
        }

        private fun list(exchange: HttpExchange, query: Map<String, String>) {
            val q = query.getValue("q")
            queries.add(q)
            assertEquals("drive", query["spaces"])
            val kind = Regex("key='omareader' and value='(\\w+)'").find(q)!!.groupValues[1]
            val path = Regex("key='omareaderPath' and value='([^']*)'").find(q)?.groupValues?.get(1)
            val matching = files.filter {
                it.properties["omareader"] == kind && !it.trashed && (path == null || it.properties["omareaderPath"] == path)
            }
            // Two to a page, so every listing of three or more has pages.
            val start = query["pageToken"]?.toInt() ?: 0
            val page = matching.drop(start).take(2)
            val out = JSONObject().put("files", JSONArray(page.map { json(it) }))
            if (start + 2 < matching.size) out.put("nextPageToken", (start + 2).toString())
            reply(exchange, 200, out.toString())
        }

        private fun props(m: JSONObject): Map<String, String> {
            val p = m.optJSONObject("properties") ?: return emptyMap()
            return p.keys().asSequence().associateWith { p.getString(it) }
        }

        private fun parents(m: JSONObject): List<String> {
            val p = m.optJSONArray("parents") ?: return emptyList()
            return (0 until p.length()).map { p.getString(it) }
        }

        private fun multipart(exchange: HttpExchange): Pair<JSONObject, ByteArray> {
            val type = exchange.requestHeaders.getFirst("Content-Type")
            assertTrue(type.startsWith("multipart/related"))
            val boundary = type.substringAfter("boundary=")
            val body = exchange.requestBody.readBytes().toString(Charsets.ISO_8859_1)
            val parts = body.split("--$boundary").drop(1).dropLast(1).map {
                it.substringAfter("\r\n\r\n").removeSuffix("\r\n")
            }
            val metadata = JSONObject(String(parts[0].toByteArray(Charsets.ISO_8859_1), Charsets.UTF_8))
            return metadata to parts[1].toByteArray(Charsets.ISO_8859_1)
        }
    }

    @Test
    fun theUploadRangeDriveReportsIsReadRight() {
        assertEquals(43L, DriveRemote.received("bytes=0-42"))
        assertEquals(0L, DriveRemote.received(null))
        assertEquals("it\\'s a \\\\ path", DriveRemote.quote("it's a \\ path"))
    }

    companion object {
        private const val JITTER = 7L
    }
}
