package io.github.looizao.omareader

import android.app.PendingIntent
import android.content.Intent
import android.net.Uri
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
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSessionService
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture

// Plays a book's chapters as one playlist and keeps playing with the screen
// off: the media session gives it the notification, the lock screen,
// headphone buttons and Bluetooth controls. The reader drives it through a
// MediaController and reads the position back to highlight the words.
class PlaybackService : MediaSessionService() {
    private var session: MediaSession? = null
    private lateinit var library: Library
    private val handler = Handler(Looper.getMainLooper())

    // The trimmed factories below are Media3's "unstable" API: public, but
    // free to change between releases, so an upgrade may need them adjusted.
    @androidx.annotation.OptIn(markerClass = [UnstableApi::class])
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
            .build()
        player.addListener(object : Player.Listener {
            override fun onIsPlayingChanged(isPlaying: Boolean) {
                save()
                handler.removeCallbacks(periodicSave)
                if (isPlaying) handler.postDelayed(periodicSave, SAVE_EVERY_MS)
            }

            override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) = save()

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
        session = MediaSession.Builder(this, player)
            .setCallback(ResolvingCallback())
            .setSessionActivity(openReader)
            .build()
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
        handler.removeCallbacks(periodicSave)
        session?.run {
            player.release()
            release()
        }
        session = null
        super.onDestroy()
    }

    // Controllers send only media ids ("<book>/<chapter>"); the files are
    // found here, so nothing outside the app can make it open a path.
    private inner class ResolvingCallback : MediaSession.Callback {
        override fun onAddMediaItems(
            mediaSession: MediaSession,
            controller: MediaSession.ControllerInfo,
            mediaItems: MutableList<MediaItem>,
        ): ListenableFuture<MutableList<MediaItem>> {
            val resolved = mediaItems.mapNotNull { resolve(it.mediaId) }
            return if (resolved.size == mediaItems.size) {
                Futures.immediateFuture(resolved.toMutableList())
            } else {
                Futures.immediateFailedFuture(IllegalArgumentException("Unknown book"))
            }
        }

        private fun resolve(mediaId: String): MediaItem? {
            val (id, chapter) = parseMediaId(mediaId) ?: return null
            val book = library.book(id) ?: return null
            if (chapter !in book.manifest.chapters.indices) return null
            return chapterItem(book, chapter, withUri = true)
        }
    }

    companion object {
        private const val SAVE_EVERY_MS = 5_000L

        fun mediaId(book: StoredBook, chapter: Int) = "${book.id}/$chapter"

        fun parseMediaId(mediaId: String): Pair<String, Int>? {
            val slash = mediaId.lastIndexOf('/')
            if (slash <= 0) return null
            val chapter = mediaId.substring(slash + 1).toIntOrNull() ?: return null
            return mediaId.substring(0, slash) to chapter
        }

        // One chapter as the system shows it: its title, the book as the
        // album, and the cover. The reader sends these without a file.
        fun chapterItem(book: StoredBook, chapter: Int, withUri: Boolean = false): MediaItem {
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
            book.cover()?.takeIf { it.isFile }?.let { metadata.setArtworkUri(Uri.fromFile(it)) }
            val item = MediaItem.Builder().setMediaId(mediaId(book, chapter)).setMediaMetadata(metadata.build())
            if (withUri) item.setUri(Uri.fromFile(book.audio(chapter)))
            return item.build()
        }
    }
}
