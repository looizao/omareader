package io.github.looizao.omareader

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class CarCatalogTest {
    @get:Rule
    val temp = TemporaryFolder()

    private fun book(id: String, title: String, author: String, vararg durations: Long): StoredBook {
        val dir = temp.newFolder(id)
        val chapters = durations.mapIndexed { i, ms ->
            """{"title":"$i","audio":"audio/00${i + 1}.m4a","text":"text/00${i + 1}.json","duration":$ms}"""
        }
        File(dir, "manifest.json").writeText(
            """{"format":"omabook","version":1,"id":"$id","title":"$title","author":"$author",
               "chapters":[${chapters.joinToString(",")}]}""",
        )
        return BundleImporter.readBook(dir)
    }

    @Test
    fun onlyKnownAppsListTheBooks() {
        val own = "io.github.looizao.omareader"
        assertTrue(CarCatalog.mayBrowse(own, trusted = false, ownPackage = own))
        assertTrue(CarCatalog.mayBrowse("com.google.android.projection.gearhead", trusted = false, ownPackage = own))
        assertTrue(CarCatalog.mayBrowse("com.google.android.googlequicksearchbox", trusted = false, ownPackage = own))
        // The system's own controllers: notification, lock screen, Bluetooth.
        assertTrue(CarCatalog.mayBrowse("com.android.systemui", trusted = true, ownPackage = own))
        assertFalse(CarCatalog.mayBrowse("com.example.snoop", trusted = false, ownPackage = own))
        assertFalse(CarCatalog.mayBrowse("android.media.session.MediaController", trusted = false, ownPackage = own))
    }

    @Test
    fun measuresHowFarIntoTheBook() {
        val manifest = book("aa01", "T", "A", 1000, 3000).manifest
        assertEquals(0.0, CarCatalog.progress(manifest, Position.START), 1e-9)
        assertEquals(0.125, CarCatalog.progress(manifest, Position(0, 500)), 1e-9)
        assertEquals(0.5, CarCatalog.progress(manifest, Position(1, 1000)), 1e-9)
        // A place past the end of its chapter, or a chapter that is gone,
        // stays inside the book.
        assertEquals(0.25, CarCatalog.progress(manifest, Position(0, 99_000)), 1e-9)
        assertEquals(1.0, CarCatalog.progress(manifest, Position(7, 99_000)), 1e-9)
        assertEquals(0.0, CarCatalog.progress(book("aa02", "T", "A", 0).manifest, Position(0, 10)), 1e-9)
    }

    @Test
    fun findsTheBookThatWasAskedFor() {
        // Most recently opened first, as Library.books() lists them.
        val dune = book("b001", "Dune", "Frank Herbert", 1)
        val memorias = book("b002", "Memórias Póstumas de Brás Cubas", "Machado de Assis", 1)
        val messiah = book("b003", "Dune Messiah", "Frank Herbert", 1)
        val herbertBio = book("b004", "Dreamer of Dune", "Brian Herbert", 1)
        val books = listOf(dune, memorias, messiah, herbertBio)

        assertSame(dune, CarCatalog.find(books, "dune"))
        assertSame(messiah, CarCatalog.find(books, "Dune Messiah"))
        // Accents and case don't matter, nor does the order of the words.
        assertSame(memorias, CarCatalog.find(books, "memorias postumas de bras cubas"))
        assertSame(memorias, CarCatalog.find(books, "BRÁS CUBAS"))
        // A title beats an author: "Herbert" is no title, so the author counts,
        // and the book opened last wins.
        assertSame(dune, CarCatalog.find(books, "herbert"))
        assertSame(memorias, CarCatalog.find(books, "machado"))
        assertSame(herbertBio, CarCatalog.find(books, "dreamer brian herbert"))
        // Nothing said: the book opened last.
        assertSame(dune, CarCatalog.find(books, ""))
        assertSame(dune, CarCatalog.find(books, null))
        assertNull(CarCatalog.find(books, "neuromancer"))
        assertNull(CarCatalog.find(emptyList(), null))
    }

    @Test
    fun normalizesWhatWasSaid() {
        assertEquals("sao paulo cia", CarCatalog.normalize("  São Paulo & Cia. "))
        assertEquals("o cortico", CarCatalog.normalize("O Cortiço"))
    }
}
