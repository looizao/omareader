package io.github.looizao.omareader

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.File

class SyncTest {
    @get:Rule
    val temp = TemporaryFolder()

    // sync/merge.json holds the cases the desktop's tests check too, so both
    // apps merge the same way.
    private val vectors by lazy { JSONObject(resource("sync/merge.json").toString(Charsets.UTF_8)) }

    private fun resource(name: String): ByteArray =
        javaClass.classLoader!!.getResourceAsStream(name)!!.use { it.readBytes() }

    private fun importFixture(): StoredBook =
        BundleImporter(temp.newFolder("books")).import(ByteArrayInputStream(resource("fixture.omabook"))).book

    @Test
    fun booksMergeAsTheSharedCasesSayInEitherOrder() {
        val cases: JSONArray = vectors.getJSONArray("cases")
        assertEquals(3, cases.length())
        for (i in 0 until cases.length()) {
            val case = cases.getJSONObject(i)
            val name = case.getString("name")
            val local = BookState.parse(case.getJSONObject("local").toString())
            val remote = BookState.parse(case.getJSONObject("remote").toString())
            val expected = BookState.parse(case.getJSONObject("merged").toString())
            assertEquals(name, expected, local.mergedWith(remote))
            assertEquals("$name, reversed", expected, remote.mergedWith(local))
            // What this side writes reads back the same.
            assertEquals(name, expected, BookState.parse(local.mergedWith(remote).toJson().toString()))
        }
    }

    @Test
    fun settingsMergeAndKeepKeysThisVersionDoesNotKnow() {
        val s = vectors.getJSONObject("settings")
        val local = SyncSettings.parse(s.getJSONObject("local").toString())
        val remote = SyncSettings.parse(s.getJSONObject("remote").toString())
        val expected = SyncSettings.parse(s.getJSONObject("merged").toString())
        assertFalse(local.mergedWith(remote).differsFrom(expected))
        assertFalse(remote.mergedWith(local).differsFrom(expected))
        assertEquals("kept", local.mergedWith(remote)["future.setting"]!!.value)
        assertTrue(local.differsFrom(remote))
        assertFalse(SyncSettings.parse(expected.toJson().toString()).differsFrom(expected))
    }

    @Test
    fun recordsFromANewerVersionOrOfAnotherKindAreRefused() {
        try {
            BookState.parse("""{"format":"omareader-state","version":2,"id":"a"}""")
            fail()
        } catch (e: SyncFormatException) {
            assertTrue(e.tooNew)
        }
        try {
            SyncSettings.parse("""{"format":"omabook","version":1}""")
            fail()
        } catch (e: SyncFormatException) {
            assertFalse(e.tooNew)
        }
    }

    @Test
    fun contentHashMatchesDropbox() {
        val h = vectors.getJSONObject("contentHash")
        assertEquals(h.getString("hash"), ContentHash.of(ByteArrayInputStream(resource("fixture.omabook"))))
        val pattern = h.getJSONObject("pattern")
        val bytes = ByteArray(pattern.getInt("size")) { (it % 251).toByte() }
        assertEquals(pattern.getString("hash"), ContentHash.of(ByteArrayInputStream(bytes)))
    }

    @Test
    fun headerArgumentsEscapeEverythingButAscii() {
        val escaped = "{\"path\":\"/caf\\u00e9 \\ud83d\\ude00\\u007f\"}"
        assertEquals(escaped, DropboxArg.escape("{\"path\":\"/café 😀\u007f\"}"))
        assertEquals("""{"path":"/plain"}""", DropboxArg.escape("""{"path":"/plain"}"""))
    }

    @Test
    fun positionsTravelThroughTheBookChapter() {
        val book = importFixture()
        assertTrue(book.manifest.knowsBookChapters)
        assertEquals(1.0, book.manifest.voiceSpeed, 0.0)
        val text = book.loadChapter(1)
        // "omareader," in the first paragraph of chapter "One".
        val word = text.wordAtOffset(1, 21)
        val anchor = Anchors.fromPosition(book, Position(1, text.startMs(word) + 10))!!
        assertEquals(Anchor(1, 1, 21, anchor.progress), anchor)
        assertTrue(anchor.progress > 0.0 && anchor.progress < 1.0)
        assertEquals(Position(1, text.startMs(word)), Anchors.toPosition(book, anchor))
        // The desktop saves the start of a sentence, between words.
        assertEquals(Position(1, text.startMs(text.wordAtOffset(2, 0))), Anchors.toPosition(book, Anchor(1, 1, 50, 0.5)))
        // A chapter past the end of the bundle lands at its end.
        assertEquals(2, Anchors.toPosition(book, Anchor(9, 0, 0, 1.0))!!.chapter)
    }

    @Test
    fun bundlesWithoutBookChaptersKeepTheirPositionsToThemselves() {
        val books = temp.newFolder("old")
        val dir = File(books, "abc").apply { mkdirs() }
        File(dir, "manifest.json").writeText(
            """{"format":"omabook","version":1,"id":"abc","title":"T","author":"A",
               "voice":{"speed":1.25},
               "chapters":[{"title":"One","audio":"audio/001.m4a","text":"text/001.json","duration":1000}]}""",
        )
        val book = BundleImporter.readBook(dir)
        assertFalse(book.manifest.knowsBookChapters)
        assertEquals(1.25, book.manifest.voiceSpeed, 0.0)
        assertNull(Anchors.fromPosition(book, Position(0, 10)))
        assertNull(Anchors.toPosition(book, Anchor(0, 0, 0, 0.0)))
    }

    // The engine, against an app folder in memory.

    private class FakeRemote : Remote {
        val files = HashMap<String, ByteArray>()
        val revs = HashMap<String, String>()
        var reads = 0
        var counter = 0
        // Runs once just before the next write, as another device would.
        var beforeWrite: (() -> Unit)? = null

        fun put(path: String, bytes: ByteArray) {
            files[path] = bytes
            revs[path] = "rev${++counter}"
        }

        private fun meta(path: String) = RemoteFile(
            path, path.substringAfterLast('/'), revs.getValue(path), files.getValue(path).size.toLong(),
            ContentHash.of(ByteArrayInputStream(files.getValue(path))),
        )

        override fun list() = files.keys.map(::meta)

        override fun read(path: String): Pair<ByteArray, RemoteFile> {
            reads++
            return files.getValue(path) to meta(path)
        }

        override fun download(path: String, out: File, from: Long, cancelled: () -> Boolean): RemoteFile {
            val bytes = files.getValue(path)
            java.io.FileOutputStream(out, true).use { it.write(bytes, from.toInt(), bytes.size - from.toInt()) }
            return meta(path)
        }

        override fun write(path: String, bytes: ByteArray, mode: WriteMode): RemoteFile {
            beforeWrite?.let { beforeWrite = null; it() }
            when (mode) {
                is WriteMode.Add -> if (path in files) throw ConflictException()
                is WriteMode.Update -> if (revs[path] != mode.rev) throw ConflictException()
                is WriteMode.Overwrite -> Unit
            }
            put(path, bytes)
            return meta(path)
        }

        override fun delete(path: String) {
            files.remove(path)
            revs.remove(path)
        }
    }

    private class FakeStore(override val device: String, private val dir: File) : SyncStore {
        val states = HashMap<String, BookState>()
        var settings = SyncSettings()
        val cache = HashMap<String, Pair<String, String>>()
        val bundles = HashMap<String, String>()
        val imported = ArrayList<String>()

        override fun localIds() = states.keys.toSet()
        override fun localState(id: String) = states[id]
        override fun applyState(merged: BookState) {
            states[merged.id] = merged
        }
        override fun localSettings() = settings
        override fun applySettings(merged: SyncSettings) {
            settings = merged
        }
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

    private fun state(present: Boolean?, presentAt: Long, chapter: Int? = null, at: Long = 0, by: String = "aaaa") =
        BookState(
            "book1", "Title", "Author",
            present?.let { Register(it, presentAt, by) },
            chapter?.let { Register(Anchor(it, 0, 0, 0.5), at, by) },
        )

    @Test
    fun anEngineSendsWhatIsNewerHereAndTakesWhatIsNewerThere() {
        val remote = FakeRemote()
        val phone = FakeStore("bbbb", temp.newFolder())
        phone.states["book1"] = state(true, 100, chapter = 2, at = 500, by = "bbbb")
        phone.settings = SyncSettings().with(SyncSettings.SPEED, Register(1.25, 50, "bbbb"))
        SyncEngine(remote, phone).sync(downloadBundles = true)
        assertEquals(state(true, 100, chapter = 2, at = 500, by = "bbbb"), BookState.parse(String(remote.files.getValue("/state/book1.json"))))
        assertEquals(1.25, SyncSettings.parse(String(remote.files.getValue("/settings.json")))[SyncSettings.SPEED]!!.value)

        // The desktop reads on further; the phone takes it.
        val further = state(true, 100, chapter = 4, at = 900, by = "aaaa").copy(present = Register(true, 100, "bbbb"))
        remote.put("/state/book1.json", further.toJson().toString().toByteArray())
        val report = SyncEngine(remote, phone).sync(downloadBundles = true)
        assertEquals(4, phone.states.getValue("book1").position!!.value.chapter)
        assertEquals(0, report.uploaded)
        assertTrue(report.changedLibrary)

        // Nothing changed anywhere: nothing is read or written again.
        val reads = remote.reads
        val again = SyncEngine(remote, phone).sync(downloadBundles = true)
        assertEquals(reads, remote.reads)
        assertEquals(0, again.uploaded)
        assertFalse(again.changedLibrary)
    }

    @Test
    fun aWriteThatCollidesIsMergedAgain() {
        val remote = FakeRemote()
        remote.put("/state/book1.json", state(true, 100).toJson().toString().toByteArray())
        val phone = FakeStore("bbbb", temp.newFolder())
        phone.states["book1"] = state(true, 100, chapter = 2, at = 500, by = "bbbb")
        // The desktop removes the book just as the phone writes its place.
        remote.beforeWrite = {
            remote.put("/state/book1.json", state(false, 700, by = "aaaa").toJson().toString().toByteArray())
        }
        SyncEngine(remote, phone).sync(downloadBundles = true)
        val written = BookState.parse(String(remote.files.getValue("/state/book1.json")))
        assertEquals(false, written.present!!.value)
        assertEquals(2, written.position!!.value.chapter)
        assertEquals(false, phone.states.getValue("book1").present!!.value)
    }

    @Test
    fun bundlesDownloadOnceAndGoWhenTheBookIsRemoved() {
        val remote = FakeRemote()
        remote.put("/books/book1.omabook", "bundle one".toByteArray())
        remote.put("/state/book1.json", state(true, 100, by = "aaaa").toJson().toString().toByteArray())
        val phone = FakeStore("bbbb", temp.newFolder())

        val waiting = SyncEngine(remote, phone).sync(downloadBundles = false)
        assertEquals(1, waiting.waiting)
        assertTrue(phone.imported.isEmpty())

        val report = SyncEngine(remote, phone).sync(downloadBundles = true)
        assertEquals(1, report.downloaded)
        assertEquals(listOf("bundle one"), phone.imported)
        assertEquals(0, SyncEngine(remote, phone).sync(downloadBundles = true).downloaded)

        // A new export replaces it.
        remote.put("/books/book1.omabook", "bundle two".toByteArray())
        SyncEngine(remote, phone).sync(downloadBundles = true)
        assertEquals(listOf("bundle one", "bundle two"), phone.imported)

        // Removed on the phone: the bundle leaves Dropbox too.
        phone.states["book1"] = phone.states.getValue("book1").copy(present = Register(false, 2000, "bbbb"))
        val removed = SyncEngine(remote, phone).sync(downloadBundles = true)
        assertEquals(1, removed.removed)
        assertFalse("/books/book1.omabook" in remote.files)
    }

    @Test
    fun aBundleWithoutABookRecordWaitsForOne() {
        val remote = FakeRemote()
        remote.put("/books/book1.omabook", "bundle".toByteArray())
        val phone = FakeStore("bbbb", temp.newFolder())
        assertEquals(0, SyncEngine(remote, phone).sync(downloadBundles = true).downloaded)
    }

    @Test
    fun aDownloadPicksUpWhereItStopped() {
        val remote = FakeRemote()
        remote.put("/books/book1.omabook", "0123456789".toByteArray())
        remote.put("/state/book1.json", state(true, 100).toJson().toString().toByteArray())
        val dir = temp.newFolder()
        val phone = FakeStore("bbbb", dir)
        val rev = remote.revs.getValue("/books/book1.omabook")
        File(dir, "book1-$rev.part").writeText("01234")
        SyncEngine(remote, phone).sync(downloadBundles = true)
        assertEquals(listOf("0123456789"), phone.imported)
        assertFalse(File(dir, "book1-$rev.part").exists())
    }

    @Test
    fun aDamagedDownloadIsThrownAway() {
        val remote = FakeRemote()
        remote.put("/books/book1.omabook", "0123456789".toByteArray())
        remote.put("/state/book1.json", state(true, 100).toJson().toString().toByteArray())
        val dir = temp.newFolder()
        val phone = FakeStore("bbbb", dir)
        val rev = remote.revs.getValue("/books/book1.omabook")
        File(dir, "book1-$rev.part").writeText("xxxxx")
        try {
            SyncEngine(remote, phone).sync(downloadBundles = true)
            fail()
        } catch (e: java.io.IOException) {
            assertFalse(File(dir, "book1-$rev.part").exists())
        }
        assertTrue(phone.imported.isEmpty())
    }

    @Test
    fun aStateFileFromANewerVersionIsLeftAlone() {
        val remote = FakeRemote()
        val newer = """{"format":"omareader-state","version":2,"id":"book1"}"""
        remote.put("/state/book1.json", newer.toByteArray())
        val phone = FakeStore("bbbb", temp.newFolder())
        phone.states["book1"] = state(true, 100)
        SyncEngine(remote, phone).sync(downloadBundles = true)
        assertEquals(newer, String(remote.files.getValue("/state/book1.json")))
        assertNotNull(phone.states["book1"])
    }

    @Test
    fun filesThatAreNotOursAreIgnored() {
        assertNull(SyncEngine.stateId("notes.txt"))
        assertNull(SyncEngine.stateId("../x.json"))
        assertEquals("abc", SyncEngine.bundleId("abc.omabook"))
    }
}
