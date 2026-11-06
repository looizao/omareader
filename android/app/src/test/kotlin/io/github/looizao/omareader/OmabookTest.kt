package io.github.looizao.omareader

import org.junit.Assert.assertArrayEquals
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
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class OmabookTest {
    @get:Rule
    val temp = TemporaryFolder()

    // fixture.omabook is a real export from the desktop app (Kokoro, voice
    // af_heart) of a three-chapter Markdown file, so these tests also check
    // that both sides agree on the format.
    private fun fixture(): ByteArray =
        javaClass.classLoader!!.getResourceAsStream("fixture.omabook")!!.use { it.readBytes() }

    private fun importFixture(books: File = temp.newFolder("books")) =
        BundleImporter(books).import(ByteArrayInputStream(fixture()))

    private fun zip(vararg entries: Pair<String, String>): ByteArray {
        val out = ByteArrayOutputStream()
        ZipOutputStream(out).use { zip ->
            for ((name, content) in entries) {
                zip.putNextEntry(ZipEntry(name))
                zip.write(content.toByteArray())
                zip.closeEntry()
            }
        }
        return out.toByteArray()
    }

    private fun manifest(version: Int = 1, id: String = "abc123", audio: String = "audio/001.m4a") =
        """{"format":"omabook","version":$version,"id":"$id","title":"T","author":"A",
           "chapters":[{"title":"One","audio":"$audio","text":"text/001.json","duration":1000}]}"""

    private val chapterJson =
        """{"title":"One","paragraphs":[{"text":"Hi there.","level":0}],"words":[0,0,2,0,300,0,3,5,300,900],"sentences":[0]}"""

    private fun expectFailure(reason: BundleException.Reason, block: () -> Unit) {
        try {
            block()
            fail("Expected $reason")
        } catch (e: BundleException) {
            assertEquals(reason, e.reason)
        }
    }

    @Test
    fun importsTheDesktopExport() {
        val books = temp.newFolder("books")
        val imported = importFixture(books)
        assertNull(imported.previous)
        val book = imported.book
        assertEquals("7218324797ea94e3", book.id)
        assertEquals(File(books, book.id), book.dir)
        assertEquals("fixture", book.manifest.title)
        assertEquals(listOf("A Short Test", "One", "Two"), book.manifest.chapters.map { it.title })
        assertEquals(9700L, book.manifest.chapters[1].durationMs)
        assertNull(book.cover())
        for (c in book.manifest.chapters.indices) {
            assertTrue(book.audio(c).isFile)
            // An MP4 file starts with its ftyp box.
            assertEquals("ftyp", String(book.audio(c).readBytes().copyOfRange(4, 8)))
        }
        // Nothing half-imported is left beside it, and the library lists it.
        assertEquals(listOf(book.id), books.list()!!.toList())
        assertEquals(listOf(book.id), BundleImporter(books).books().map { it.id })
    }

    @Test
    fun findsTheWordBeingHeard() {
        val text = importFixture().book.loadChapter(1)
        assertEquals("One", text.title)
        assertEquals(3, text.paragraphs.size)
        assertTrue(text.paragraphs[0].isHeading)
        assertEquals(14, text.wordCount)
        assertEquals("there", text.paragraphs[1].text.substring(text.start(2), text.end(2)))

        assertEquals(-1, text.wordAt(-1))
        assertEquals(0, text.wordAt(0))
        // The pause after the heading keeps the heading lit.
        assertEquals(0, text.wordAt(2149))
        assertEquals(1, text.wordAt(2150))
        assertEquals(2, text.wordAt(3000))
        assertEquals(13, text.wordAt(1_000_000))
        // Every word is found at its own start.
        for (w in 0 until text.wordCount) assertEquals(w, text.wordAt(text.startMs(w)))
    }

    @Test
    fun findsTheWordUnderAFinger() {
        val text = importFixture().book.loadChapter(1)
        assertEquals(0, text.wordAtOffset(0, 0))
        assertEquals(2, text.wordAtOffset(1, 6)) // "there"
        assertEquals(2, text.wordAtOffset(1, 10))
        assertEquals(3, text.wordAtOffset(1, 11)) // the full stop starts the next word
        assertEquals(10, text.firstWordFrom(2))
        assertEquals(-1, text.wordAtOffset(2, 1000))
        assertEquals(-1, text.wordAtOffset(7, 0))
    }

    @Test
    fun movesBySentence() {
        val text = importFixture().book.loadChapter(1)
        assertEquals(4, text.sentenceCount)
        assertEquals(0, text.sentenceOf(0))
        assertEquals(1, text.sentenceOf(2))
        assertEquals(2, text.sentenceOf(5))
        assertEquals(3, text.sentenceOf(13))
        assertEquals(10, text.sentenceStart(3))
    }

    @Test
    fun aNewExportReplacesTheOldOne() {
        val books = temp.newFolder("books")
        val first = importFixture(books)
        File(first.book.dir, "marker").writeText("old")
        val second = importFixture(books)
        assertNotNull(second.previous)
        assertTrue(File(second.previous!!.dir, "marker").isFile)
        assertFalse(File(second.book.dir, "marker").exists())
        second.previous!!.dir.deleteRecursively()
        assertEquals(listOf(second.book.id), books.list()!!.toList())
    }

    @Test
    fun turnsAwayFilesThatAreNotBooks() {
        val books = temp.newFolder("books")
        val importer = BundleImporter(books)
        expectFailure(BundleException.Reason.NotABundle) {
            importer.import(ByteArrayInputStream("just some text".toByteArray()))
        }
        // Another ZIP: refused at its first entry.
        expectFailure(BundleException.Reason.NotABundle) {
            importer.import(ByteArrayInputStream(zip("readme.txt" to "hello", "manifest.json" to manifest())))
        }
        expectFailure(BundleException.Reason.NotABundle) {
            importer.import(ByteArrayInputStream(zip("manifest.json" to """{"format":"other"}""")))
        }
        expectFailure(BundleException.Reason.TooNew) {
            importer.import(
                ByteArrayInputStream(
                    zip("manifest.json" to manifest(version = 2), "audio/001.m4a" to "x", "text/001.json" to chapterJson),
                ),
            )
        }
        assertTrue(books.list()!!.isEmpty())
    }

    @Test
    fun neverWritesOutsideTheBooksFolder() {
        val root = temp.newFolder("root")
        val books = File(root, "books")
        val importer = BundleImporter(books)
        expectFailure(BundleException.Reason.Damaged) {
            importer.import(ByteArrayInputStream(zip("manifest.json" to manifest(), "../evil.txt" to "boom")))
        }
        expectFailure(BundleException.Reason.Damaged) {
            importer.import(ByteArrayInputStream(zip("manifest.json" to manifest(), "audio/../../evil.m4a" to "boom")))
        }
        expectFailure(BundleException.Reason.Damaged) {
            Manifest.parse(manifest(id = "../escape"))
        }
        expectFailure(BundleException.Reason.Damaged) {
            Manifest.parse(manifest(audio = "../audio/001.m4a"))
        }
        assertEquals(listOf("books"), root.list()!!.toList())
        assertTrue(books.list()!!.isEmpty())
    }

    @Test
    fun aDamagedExportKeepsTheCopyAlreadyThere() {
        val books = temp.newFolder("books")
        val importer = BundleImporter(books)
        val good = zip("manifest.json" to manifest(), "audio/001.m4a" to "x", "text/001.json" to chapterJson)
        importer.import(ByteArrayInputStream(good))
        // The audio is missing from the new one.
        expectFailure(BundleException.Reason.Damaged) {
            importer.import(ByteArrayInputStream(zip("manifest.json" to manifest(), "text/001.json" to chapterJson)))
        }
        // A word that points past its paragraph.
        val bad = chapterJson.replace("0,3,5,300,900", "0,3,50,300,900")
        expectFailure(BundleException.Reason.Damaged) {
            importer.import(ByteArrayInputStream(zip("manifest.json" to manifest(), "audio/001.m4a" to "x", "text/001.json" to bad)))
        }
        assertEquals(listOf("abc123"), books.list()!!.toList())
        assertEquals("x", File(books, "abc123/audio/001.m4a").readText())
    }

    @Test
    fun checksTheWordsOfAChapter() {
        assertEquals(2, ChapterText.parse(chapterJson).wordCount)
        expectFailure(BundleException.Reason.Damaged) {
            ChapterText.parse(chapterJson.replace("0,3,5,300,900", "0,3,5,-1,900")) // time goes back
        }
        expectFailure(BundleException.Reason.Damaged) {
            ChapterText.parse(chapterJson.replace("0,3,5,300,900", "1,3,5,300,900")) // no such paragraph
        }
        expectFailure(BundleException.Reason.Damaged) {
            ChapterText.parse(chapterJson.replace("0,3,5,300,900", "0,3,5,300")) // not five per word
        }
        expectFailure(BundleException.Reason.Damaged) {
            ChapterText.parse(chapterJson.replace("\"sentences\":[0]", "\"sentences\":[0,7]"))
        }
        // An older export without sentences reads as one sentence.
        val noSentences = ChapterText.parse(chapterJson.replace(",\"sentences\":[0]", ""))
        assertEquals(1, noSentences.sentenceCount)
    }

    @Test
    fun positionsSurviveARoundTrip() {
        assertEquals(Position(3, 1234), Position.decode(Position(3, 1234).encode()))
        assertEquals(Position.START, Position.decode(null))
        assertEquals(Position.START, Position.decode("garbage"))
        assertEquals(Position(0, 0), Position.decode("-2:-5"))
    }

    @Test
    fun aNewExportKeepsThePlaceByWord() {
        val oldDir = temp.newFolder("old")
        val newDir = temp.newFolder("new")
        fun write(dir: File, chapters: List<String>, words: String) {
            val list = chapters.mapIndexed { i, t ->
                """{"title":"$t","audio":"audio/00${i + 1}.m4a","text":"text/00${i + 1}.json","duration":1}"""
            }
            File(dir, "manifest.json").writeText(
                """{"format":"omabook","version":1,"id":"b","title":"B","chapters":[${list.joinToString(",")}]}""",
            )
            File(dir, "text").mkdirs()
            chapters.indices.forEach { i ->
                File(dir, "text/00${i + 1}.json").writeText(
                    """{"title":"t","paragraphs":[{"text":"one two three","level":0}],"words":[$words]}""",
                )
            }
        }
        // The new export is read at half the speed and has an extra chapter
        // in front of the one we were in.
        write(oldDir, listOf("Intro", "Middle"), "0,0,3,0,500,0,4,3,500,1000,0,8,5,1000,1500")
        write(newDir, listOf("Preface", "Intro", "Middle"), "0,0,3,0,1000,0,4,3,1000,2000,0,8,5,2000,3000")
        val old = BundleImporter.readBook(oldDir)
        val new = BundleImporter.readBook(newDir)
        // In "Middle", during "two".
        assertEquals(Position(2, 1000), Position.remap(Position(1, 700), old, new))
        // A chapter the new export doesn't have by name stays at its number.
        val renamed = temp.newFolder("renamed")
        write(renamed, listOf("A", "B"), "0,0,3,0,1000,0,4,3,1000,2000,0,8,5,2000,3000")
        assertEquals(Position(1, 2000), Position.remap(Position(1, 1200), old, BundleImporter.readBook(renamed)))
        assertArrayEquals(arrayOf("manifest.json", "text"), oldDir.list()!!.sortedArray())
    }
}
