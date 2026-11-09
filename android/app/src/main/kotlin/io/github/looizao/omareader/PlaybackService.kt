package io.github.looizao.omareader

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.datasource.FileDataSource
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.RenderersFactory
import androidx.media3.exoplayer.audio.MediaCodecAudioRenderer
import androidx.media3.exoplayer.mediacodec.MediaCodecSelector
import androidx.media3.exoplayer.source.ProgressiveMediaSource
import androidx.media3.extractor.ExtractorsFactory
import androidx.media3.extractor.mp4.Mp4Extractor
import androidx.media3.extractor.text.SubtitleParser
import androidx.media3.session.CommandButton
import androidx.media3.session.LibraryResult
import androidx.media3.session.MediaConstants
import androidx.media3.session.MediaLibraryService
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSession.MediaItemsWithStartPosition
import androidx.media3.session.SessionError
import com.google.common.collect.ImmutableList
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture
import com.google.common.util.concurrent.MoreExecutors
import java.util.concurrent.Executors

// Plays a book's chapters as one playlist and keeps playing with the screen
// off: the media session gives it the notification, the lock screen,
// headphone buttons and Bluetooth controls. The reader drives it through a
// MediaController and reads the position back to highlight the words.
//
// It is also a media library, which is what Android Auto shows in the car:
// a folder of books, each of which plays from where it was left, voice
// requests ("play Dune on omareader"), and picking up the last book when the
// car connects or a play button is pressed with the app closed.
//
// The trimmed player factories, the car's content styles and the seek
// buttons are Media3's "unstable" API: public, but free to change between
// releases, so an upgrade may need them adjusted.
@androidx.annotation.OptIn(markerClass = [UnstableApi::class])
class PlaybackService : MediaLibraryService() {
    private var session: MediaLibrarySession? = null
    private lateinit var library: Library
    private val handler = Handler(Looper.getMainLooper())

    // Listing the books reads every manifest, so library requests are
    // answered from one background thread rather than the main one.
    private val disk = MoreExecutors.listeningDecorator(Executors.newSingleThreadExecutor())

    override fun onCreate() {
        super.onCreate()
        library = Library(this)

        // Only what an .omabook needs: AAC in MP4, from a local file. The
        // default factories would pull in every container and video, text
        // and metadata renderer, which R8 can then not remove.
        val renderers = RenderersFactory { handler, _, audioListener, _, _ ->
            arrayOf(MediaCodecAudioRenderer(this, MediaCodecSelector.DEFAULT, handler, audioListener))
        }
        val extractors = ExtractorsFactory { arrayOf(Mp4Extractor(SubtitleParser.Factory.UNSUPPORTED)) }
        val sources = ProgressiveMediaSource.Factory(FileDataSource.Factory(), extractors)

        val player = ExoPlayer.Builder(this, renderers, sources)
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(C.USAGE_MEDIA)
                    .setContentType(C.AUDIO_CONTENT_TYPE_SPEECH)
                    .build(),
                true,
            )
            .setHandleAudioBecomingNoisy(true)
            .setWakeMode(C.WAKE_MODE_LOCAL)
            .setSeekBackIncrementMs(SEEK_BACK_MS)
            .setSeekForwardIncrementMs(SEEK_FORWARD_MS)
            .build()
        player.addListener(object : Player.Listener {
            override fun onIsPlayingChanged(isPlaying: Boolean) {
                save()
                PlayerState.playing = isPlaying
                handler.removeCallbacks(periodicSave)
                if (isPlaying) handler.postDelayed(periodicSave, SAVE_EVERY_MS)
            }

            override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
                PlayerState.bookId = mediaItem?.mediaId?.let { parseMediaId(it) }?.first
                save()
            }

            override fun onPositionDiscontinuity(
                oldPosition: Player.PositionInfo,
                newPosition: Player.PositionInfo,
                reason: Int,
            ) {
                if (reason == Player.DISCONTINUITY_REASON_SEEK) save()
            }
        })

        val openReader = PendingIntent.getActivity(
            this,
            0,
            Intent(this, ReaderActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        // In the car and the notification, the buttons beside play go back a
        // little and forward further, as in other audiobook players: a missed
        // sentence is one tap away. Chapters are in the car's queue, and the
        // reader keeps its own chapter and sentence buttons.
        val buttons = listOf(
            CommandButton.Builder(CommandButton.ICON_SKIP_BACK_10)
                .setPlayerCommand(Player.COMMAND_SEEK_BACK)
                .setDisplayName(getString(R.string.seek_back))
                .setSlots(CommandButton.SLOT_BACK)
                .build(),
            CommandButton.Builder(CommandButton.ICON_SKIP_FORWARD_30)
                .setPlayerCommand(Player.COMMAND_SEEK_FORWARD)
                .setDisplayName(getString(R.string.seek_forward))
                .setSlots(CommandButton.SLOT_FORWARD)
                .build(),
        )
        session = MediaLibrarySession.Builder(this, player, LibraryCallback())
            .setSessionActivity(openReader)
            .setMediaButtonPreferences(buttons)
            .build()
        SyncEvents.listen(syncListener)
    }

    // What another device changed, applied to the player while it is not
    // reading aloud: the place in the loaded book, the speed, and a book
    // replaced by a new export or removed.
    private val syncListener: (SyncEvents.Change, String?) -> Unit = { change, id ->
        val player = session?.player
        val loaded = player?.currentMediaItem?.mediaId?.let { parseMediaId(it) }?.first
        if (player != null && loaded != null && !player.isPlaying) {
            when (change) {
                SyncEvents.Change.Position -> if (id == loaded) {
                    val place = library.position(loaded)
                    if (place.chapter in 0 until player.mediaItemCount) player.seekTo(place.chapter, place.ms)
                }
                SyncEvents.Change.Settings -> library.book(loaded)?.let { player.setPlaybackSpeed(library.playbackSpeed(it)) }
                SyncEvents.Change.Library -> reload(player, loaded)
                SyncEvents.Change.Status -> Unit
            }
        }
    }

    // A book whose files changed under the player is loaded again where it
    // was left, or let go when it is no longer on the phone.
    private fun reload(player: Player, id: String) {
        val book = library.book(id)
        if (book == null) {
            player.clearMediaItems()
            return
        }
        val uri = player.currentMediaItem?.localConfiguration?.uri
        if (uri != null && java.io.File(uri.path.orEmpty()).exists() && player.mediaItemCount == book.manifest.chapters.size) return
        val place = library.position(id)
        player.setMediaItems(chapters(book), place.chapter.coerceIn(book.manifest.chapters.indices), place.ms)
        player.setPlaybackSpeed(library.playbackSpeed(book))
        player.prepare()
    }

    private val periodicSave = object : Runnable {
        override fun run() {
            save()
            handler.postDelayed(this, SAVE_EVERY_MS)
        }
    }

    // The place in the book playing now, kept by the service so it is saved
    // whether or not the reader is on screen.
    private fun save() {
        val player = session?.player ?: return
        val item = player.currentMediaItem ?: return
        val (id, chapter) = parseMediaId(item.mediaId) ?: return
        library.savePosition(id, Position(chapter, player.currentPosition.coerceAtLeast(0)))
    }

    override fun onGetSession(controllerInfo: MediaSession.ControllerInfo) = session

    override fun onTaskRemoved(rootIntent: Intent?) {
        save()
        val player = session?.player
        // Swiping the app away stops it unless it is reading aloud.
        if (player == null || !player.playWhenReady || player.mediaItemCount == 0) stopSelf()
    }

    override fun onDestroy() {
        save()
        SyncEvents.stop(syncListener)
        PlayerState.playing = false
        PlayerState.bookId = null
        handler.removeCallbacks(periodicSave)
        disk.shutdown()
        session?.run {
            player.release()
            release()
        }
        session = null
        super.onDestroy()
    }

    // A whole book, ready to play from where it was left, at the reader's
    // speed. Playing it from the car counts as opening it.
    private fun startBook(book: StoredBook): MediaItemsWithStartPosition {
        library.markOpened(book.id)
        val place = library.position(book.id)
        val chapters = chapters(book)
        val speed = library.playbackSpeed(book)
        handler.post { session?.player?.setPlaybackSpeed(speed) }
        return MediaItemsWithStartPosition(chapters, place.chapter.coerceIn(chapters.indices), place.ms)
    }

    private fun bookItem(book: StoredBook): MediaItem {
        val place = library.position(book.id)
        val progress = CarCatalog.progress(book.manifest, place)
        // The car shows how far each book has got.
        val extras = Bundle()
        when {
            place == Position.START -> extras.putInt(
                MediaConstants.EXTRAS_KEY_COMPLETION_STATUS,
                MediaConstants.EXTRAS_VALUE_COMPLETION_STATUS_NOT_PLAYED,
            )
            progress >= FINISHED -> extras.putInt(
                MediaConstants.EXTRAS_KEY_COMPLETION_STATUS,
                MediaConstants.EXTRAS_VALUE_COMPLETION_STATUS_FULLY_PLAYED,
            )
            else -> {
                extras.putInt(
                    MediaConstants.EXTRAS_KEY_COMPLETION_STATUS,
                    MediaConstants.EXTRAS_VALUE_COMPLETION_STATUS_PARTIALLY_PLAYED,
                )
                extras.putDouble(MediaConstants.EXTRAS_KEY_COMPLETION_PERCENTAGE, progress)
            }
        }
        val metadata = MediaMetadata.Builder()
            .setTitle(book.manifest.title)
            .setArtist(book.manifest.author)
            .setSubtitle(book.manifest.author)
            .setArtworkUri(CoverProvider.uri(this, book))
            .setMediaType(MediaMetadata.MEDIA_TYPE_AUDIO_BOOK)
            .setIsPlayable(true)
            .setIsBrowsable(false)
            .setExtras(extras)
            .build()
        return MediaItem.Builder().setMediaId(book.id).setMediaMetadata(metadata).build()
    }

    private fun folder(id: String, title: String?, type: Int, extras: Bundle? = null): MediaItem {
        val metadata = MediaMetadata.Builder()
            .setTitle(title)
            .setMediaType(type)
            .setIsPlayable(false)
            .setIsBrowsable(true)
            .setExtras(extras)
            .build()
        return MediaItem.Builder().setMediaId(id).setMediaMetadata(metadata).build()
    }

    // The books show as a grid of covers.
    private fun booksFolder() = folder(
        CarCatalog.BOOKS,
        getString(R.string.car_books),
        MediaMetadata.MEDIA_TYPE_FOLDER_AUDIO_BOOKS,
        Bundle().apply {
            putInt(MediaConstants.EXTRAS_KEY_CONTENT_STYLE_PLAYABLE, MediaConstants.EXTRAS_VALUE_CONTENT_STYLE_GRID_ITEM)
        },
    )

    private fun chapters(book: StoredBook) =
        book.manifest.chapters.indices.map { chapterItem(this, book, it, withUri = true) }

    // A chapter the reader asked for by id, with its file found here.
    private fun resolve(mediaId: String): MediaItem? {
        val (id, chapter) = parseMediaId(mediaId) ?: return null
        val book = library.book(id) ?: return null
        if (chapter !in book.manifest.chapters.indices) return null
        return chapterItem(this, book, chapter, withUri = true)
    }

    // Controllers send only ids: a book, or a chapter as "<book>/<chapter>".
    // The files are found here, so nothing outside the app can make it open
    // a path.
    private inner class LibraryCallback : MediaLibrarySession.Callback {
        override fun onConnect(
            session: MediaSession,
            controller: MediaSession.ControllerInfo,
        ): MediaSession.ConnectionResult {
            if (!CarCatalog.mayBrowse(controller.packageName, controller.isTrusted, packageName)) {
                return MediaSession.ConnectionResult.AcceptedResultBuilder(session, controller)
                    .setAvailableSessionCommands(MediaSession.ConnectionResult.DEFAULT_SESSION_COMMANDS)
                    .build()
            }
            // The covers the car and the system draw come from CoverProvider,
            // which only the apps let in here may read.
            if (controller.packageName != packageName) {
                runCatching {
                    grantUriPermission(
                        controller.packageName,
                        CoverProvider.root(this@PlaybackService),
                        Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PREFIX_URI_PERMISSION,
                    )
                }
            }
            return super.onConnect(session, controller)
        }

        override fun onGetLibraryRoot(
            session: MediaLibrarySession,
            browser: MediaSession.ControllerInfo,
            params: LibraryParams?,
        ): ListenableFuture<LibraryResult<MediaItem>> {
            // Asked for the recent root, the car offers to resume the last book.
            val id = if (params?.isRecent == true) CarCatalog.RECENT else CarCatalog.ROOT
            val root = folder(id, null, MediaMetadata.MEDIA_TYPE_FOLDER_MIXED)
            return Futures.immediateFuture(LibraryResult.ofItem(root, params))
        }

        override fun onGetChildren(
            session: MediaLibrarySession,
            browser: MediaSession.ControllerInfo,
            parentId: String,
            page: Int,
            pageSize: Int,
            params: LibraryParams?,
        ): ListenableFuture<LibraryResult<ImmutableList<MediaItem>>> = disk.submit<LibraryResult<ImmutableList<MediaItem>>> {
            val children = when (parentId) {
                CarCatalog.ROOT -> listOf(booksFolder())
                CarCatalog.RECENT -> library.books().take(1).map(::bookItem)
                CarCatalog.BOOKS -> library.books().map(::bookItem)
                else -> return@submit LibraryResult.ofError(SessionError.ERROR_BAD_VALUE)
            }
            val from = (page.toLong() * pageSize).coerceAtMost(children.size.toLong()).toInt()
            val to = (from.toLong() + pageSize).coerceAtMost(children.size.toLong()).toInt()
            LibraryResult.ofItemList(children.subList(from, to), params)
        }

        override fun onGetItem(
            session: MediaLibrarySession,
            browser: MediaSession.ControllerInfo,
            mediaId: String,
        ): ListenableFuture<LibraryResult<MediaItem>> = disk.submit<LibraryResult<MediaItem>> {
            val item = when (mediaId) {
                CarCatalog.ROOT, CarCatalog.RECENT -> folder(mediaId, null, MediaMetadata.MEDIA_TYPE_FOLDER_MIXED)
                CarCatalog.BOOKS -> booksFolder()
                else -> library.book(mediaId)?.let(::bookItem) ?: resolve(mediaId)
            }
            if (item != null) LibraryResult.ofItem(item, null) else LibraryResult.ofError(SessionError.ERROR_BAD_VALUE)
        }

        // Adding a book adds all its chapters.
        override fun onAddMediaItems(
            mediaSession: MediaSession,
            controller: MediaSession.ControllerInfo,
            mediaItems: MutableList<MediaItem>,
        ): ListenableFuture<MutableList<MediaItem>> = disk.submit<MutableList<MediaItem>> {
            mediaItems.flatMap { item ->
                library.book(item.mediaId)?.let(::chapters)
                    ?: listOf(resolve(item.mediaId) ?: throw IllegalArgumentException("Unknown book"))
            }.toMutableList()
        }

        // The reader sends a book's chapters and where to start. The car and
        // the assistant send one book, or what was said; those start where
        // the book was left.
        override fun onSetMediaItems(
            mediaSession: MediaSession,
            controller: MediaSession.ControllerInfo,
            mediaItems: MutableList<MediaItem>,
            startIndex: Int,
            startPositionMs: Long,
        ): ListenableFuture<MediaItemsWithStartPosition> {
            // Keep the place in the book being replaced, to the moment.
            save()
            return disk.submit<MediaItemsWithStartPosition> {
                val single = mediaItems.singleOrNull()
                val query = single?.requestMetadata?.searchQuery
                val book = when {
                    single == null -> null
                    query != null -> CarCatalog.find(library.books(), query)
                        ?: throw IllegalArgumentException("No book matches")
                    else -> library.book(single.mediaId)
                }
                if (book != null) {
                    startBook(book)
                } else {
                    val chapters = mediaItems.map { resolve(it.mediaId) ?: throw IllegalArgumentException("Unknown book") }
                    MediaItemsWithStartPosition(chapters, startIndex, startPositionMs)
                }
            }
        }

        // Play pressed in the car, on headphones or on the system's resume
        // card with nothing loaded: the book opened last, where it was left.
        override fun onPlaybackResumption(
            mediaSession: MediaSession,
            controller: MediaSession.ControllerInfo,
            isForPlayback: Boolean,
        ): ListenableFuture<MediaItemsWithStartPosition> = disk.submit<MediaItemsWithStartPosition> {
            startBook(library.books().firstOrNull() ?: throw IllegalStateException("No books"))
        }
    }

    companion object {
        private const val SAVE_EVERY_MS = 5_000L
        private const val SEEK_BACK_MS = 10_000L
        private const val SEEK_FORWARD_MS = 30_000L

        // A book this close to its end shows as finished in the car.
        private const val FINISHED = 0.99

        fun mediaId(book: StoredBook, chapter: Int) = "${book.id}/$chapter"

        fun parseMediaId(mediaId: String): Pair<String, Int>? {
            val slash = mediaId.lastIndexOf('/')
            if (slash <= 0) return null
            val chapter = mediaId.substring(slash + 1).toIntOrNull() ?: return null
            return mediaId.substring(0, slash) to chapter
        }

        // One chapter as the system shows it: its title, the book as the
        // album, and the cover. The reader sends these without a file.
        fun chapterItem(context: Context, book: StoredBook, chapter: Int, withUri: Boolean = false): MediaItem {
            val info = book.manifest.chapters[chapter]
            val metadata = MediaMetadata.Builder()
                .setTitle(info.title.ifEmpty { book.manifest.title })
                .setAlbumTitle(book.manifest.title)
                .setArtist(book.manifest.author)
                .setTrackNumber(chapter + 1)
                .setTotalTrackCount(book.manifest.chapters.size)
                .setMediaType(MediaMetadata.MEDIA_TYPE_AUDIO_BOOK_CHAPTER)
                .setIsPlayable(true)
                .setIsBrowsable(false)
                .setArtworkUri(CoverProvider.uri(context, book))
                .build()
            val item = MediaItem.Builder().setMediaId(mediaId(book, chapter)).setMediaMetadata(metadata)
            if (withUri) item.setUri(Uri.fromFile(book.audio(chapter)))
            return item.build()
        }
    }
}
