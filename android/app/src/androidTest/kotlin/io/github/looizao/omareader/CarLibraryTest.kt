package io.github.looizao.omareader

import android.content.ComponentName
import android.media.MediaMetadata
import android.media.browse.MediaBrowser
import android.media.session.MediaController
import android.media.session.PlaybackState
import android.os.Bundle
import android.os.SystemClock
import android.service.media.MediaBrowserService
import androidx.media3.session.MediaConstants
import androidx.media3.session.SessionToken
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

// Drives PlaybackService through the platform's MediaBrowser, which is what
// Android Auto uses to show the library in the car and to play from it, with
// the real export the JVM tests read. Run with bin/android device-test.
@RunWith(AndroidJUnit4::class)
class CarLibraryTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val app = instrumentation.targetContext
    private val library = Library(app)
    private val browsers = mutableListOf<MediaBrowser>()
    private lateinit var book: StoredBook

    @Before
    fun importTheFixture() {
        emptyThePlayer()
        File(app.filesDir, "books").deleteRecursively()
        book = instrumentation.context.assets.open("fixture.omabook").use {
            BundleImporter(File(app.filesDir, "books")).import(it)
        }.book
        // Left in chapter 1 ("One"), four seconds in, at 1.25x.
        library.savePosition(book.id, Position(1, 4000))
        library.setSpeed(1.25f)
    }

    @After
    fun cleanUp() {
        instrumentation.runOnMainSync {
            for (browser in browsers) {
                if (browser.isConnected) MediaController(app, browser.sessionToken).transportControls.stop()
                browser.disconnect()
            }
        }
        emptyThePlayer()
        library.delete(book)
        library.setSpeed(1f)
    }

    // A book still in the player from the test before would have its place
    // saved over the one each test sets up.
    private fun emptyThePlayer() {
        val token = SessionToken(app, ComponentName(app, PlaybackService::class.java))
        val future = androidx.media3.session.MediaController.Builder(app, token).buildAsync()
        val controller = future.get(10, TimeUnit.SECONDS)
        instrumentation.runOnMainSync {
            controller.stop()
            controller.clearMediaItems()
            controller.release()
        }
    }

    private fun connect(hints: Bundle? = null): MediaBrowser {
        val latch = CountDownLatch(1)
        lateinit var browser: MediaBrowser
        instrumentation.runOnMainSync {
            val callback = object : MediaBrowser.ConnectionCallback() {
                override fun onConnected() = latch.countDown()
                override fun onConnectionFailed() = latch.countDown()
            }
            browser = MediaBrowser(app, ComponentName(app, PlaybackService::class.java), callback, hints)
            browser.connect()
        }
        assertTrue("connects", latch.await(10, TimeUnit.SECONDS))
        assertTrue("connected", browser.isConnected)
        browsers += browser
        return browser
    }

    private fun children(browser: MediaBrowser, parent: String): List<MediaBrowser.MediaItem> {
        val latch = CountDownLatch(1)
        var result: List<MediaBrowser.MediaItem>? = null
        val callback = object : MediaBrowser.SubscriptionCallback() {
            override fun onChildrenLoaded(parentId: String, children: MutableList<MediaBrowser.MediaItem>) {
                result = children.toList()
                latch.countDown()
            }

            override fun onError(parentId: String) = latch.countDown()
        }
        instrumentation.runOnMainSync { browser.subscribe(parent, callback) }
        assertTrue("lists $parent", latch.await(10, TimeUnit.SECONDS))
        instrumentation.runOnMainSync { browser.unsubscribe(parent) }
        return result ?: error("No children for $parent")
    }

    private fun controller(browser: MediaBrowser): MediaController {
        lateinit var controller: MediaController
        instrumentation.runOnMainSync { controller = MediaController(app, browser.sessionToken) }
        return controller
    }

    private fun waitFor(what: String, check: () -> Boolean) {
        val until = SystemClock.uptimeMillis() + 10_000
        while (SystemClock.uptimeMillis() < until) {
            var done = false
            instrumentation.runOnMainSync { done = check() }
            if (done) return
            SystemClock.sleep(100)
        }
        fail("Timed out waiting for $what")
    }

    @Test
    fun listsTheBooksAsTheCarShowsThem() {
        val browser = connect()
        assertEquals(CarCatalog.ROOT, browser.root)

        val root = children(browser, browser.root)
        assertEquals(listOf(CarCatalog.BOOKS), root.map { it.mediaId })
        val folder = root.single()
        assertTrue(folder.isBrowsable)
        assertFalse(folder.isPlayable)
        assertEquals(app.getString(R.string.car_books), folder.description.title.toString())
        assertEquals(
            MediaConstants.EXTRAS_VALUE_CONTENT_STYLE_GRID_ITEM,
            folder.description.extras?.getInt(MediaConstants.EXTRAS_KEY_CONTENT_STYLE_PLAYABLE),
        )

        val books = children(browser, CarCatalog.BOOKS)
        assertEquals(listOf(book.id), books.map { it.mediaId })
        val item = books.single()
        assertTrue(item.isPlayable)
        assertFalse(item.isBrowsable)
        assertEquals("fixture", item.description.title.toString())
        val extras = item.description.extras!!
        assertEquals(
            MediaConstants.EXTRAS_VALUE_COMPLETION_STATUS_PARTIALLY_PLAYED,
            extras.getInt(MediaConstants.EXTRAS_KEY_COMPLETION_STATUS),
        )
        val chapters = book.manifest.chapters
        val expected = (chapters[0].durationMs + 4000) / chapters.sumOf { it.durationMs }.toDouble()
        assertEquals(expected, extras.getDouble(MediaConstants.EXTRAS_KEY_COMPLETION_PERCENTAGE), 1e-6)
    }

    @Test
    fun offersTheLastBookToResume() {
        val browser = connect(Bundle().apply { putBoolean(MediaBrowserService.BrowserRoot.EXTRA_RECENT, true) })
        assertEquals(CarCatalog.RECENT, browser.root)
        assertEquals(listOf(book.id), children(browser, browser.root).map { it.mediaId })
    }

    @Test
    fun playsABookFromWhereItWasLeft() {
        val controller = controller(connect())
        instrumentation.runOnMainSync { controller.transportControls.playFromMediaId(book.id, null) }
        waitFor("the book to play") { controller.playbackState?.state == PlaybackState.STATE_PLAYING }
        instrumentation.runOnMainSync {
            assertEquals("One", controller.metadata?.getString(MediaMetadata.METADATA_KEY_TITLE))
            assertEquals("fixture", controller.metadata?.getString(MediaMetadata.METADATA_KEY_ALBUM))
            // The whole book is in the car's queue, for picking a chapter.
            assertEquals(3, controller.queue?.size)
            val state = controller.playbackState!!
            assertTrue("starts at 4 s, not ${state.position} ms", state.position in 3_900..9_000)
            assertEquals(1.25f, state.playbackSpeed, 0.01f)
        }
    }

    @Test
    fun playsTheBookThatWasAskedFor() {
        val controller = controller(connect())
        // Nothing on this phone by that name: nothing plays.
        instrumentation.runOnMainSync { controller.transportControls.playFromSearch("Neuromancer", null) }
        SystemClock.sleep(1_000)
        instrumentation.runOnMainSync {
            assertTrue(controller.playbackState?.state != PlaybackState.STATE_PLAYING)
        }

        instrumentation.runOnMainSync { controller.transportControls.playFromSearch("Fixture", null) }
        waitFor("the book asked for to play") { controller.playbackState?.state == PlaybackState.STATE_PLAYING }
        instrumentation.runOnMainSync {
            assertEquals("One", controller.metadata?.getString(MediaMetadata.METADATA_KEY_TITLE))
        }
    }
}
