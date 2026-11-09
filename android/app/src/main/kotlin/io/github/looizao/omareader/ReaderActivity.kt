package io.github.looizao.omareader

import android.app.Activity
import android.app.AlertDialog
import android.content.ComponentName
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.graphics.Typeface
import android.text.Spannable
import android.text.SpannableString
import android.text.style.BackgroundColorSpan
import android.util.TypedValue
import android.view.Choreographer
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.AbsListView
import android.widget.BaseAdapter
import android.widget.ImageButton
import android.widget.ListView
import android.widget.SeekBar
import android.widget.TextView
import android.widget.Toast
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
import com.google.common.util.concurrent.ListenableFuture
import java.text.NumberFormat
import java.util.concurrent.Executor

// Shows a chapter's text and lights up each word as the audio reaches it.
// The audio plays in PlaybackService; this screen only asks the controller
// where playback is (a local estimate, no IPC) once a frame while it plays,
// finds the word by binary search, and moves one span. A tap on a word
// plays from it, and the view follows the voice until you scroll away.
class ReaderActivity : Activity() {
    private lateinit var library: Library
    private var book: StoredBook? = null
    private var controllerFuture: ListenableFuture<MediaController>? = null
    private var controller: MediaController? = null

    private var chapterIndex = -1
    private var loadingChapter = -1
    private var chapter: ChapterText? = null
    private var word = -1
    private var following = true
    private var ticking = false
    private var lastProgressUpdate = 0L
    private var seeking = false

    private lateinit var list: ListView
    private lateinit var chapterTitle: TextView
    private lateinit var speedButton: TextView
    private lateinit var follow: View
    private lateinit var play: ImageButton
    private lateinit var seek: SeekBar
    private lateinit var elapsed: TextView
    private lateinit var remaining: TextView
    private val adapter = ParagraphAdapter()
    private val main = Handler(Looper.getMainLooper())
    private val mainExecutor = Executor { main.post(it) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_reader)
        library = Library(this)

        list = findViewById(R.id.paragraphs)
        chapterTitle = findViewById(R.id.chapter_title)
        speedButton = findViewById(R.id.speed)
        follow = findViewById(R.id.follow)
        play = findViewById(R.id.play)
        seek = findViewById(R.id.seek)
        elapsed = findViewById(R.id.elapsed)
        remaining = findViewById(R.id.remaining)

        list.adapter = adapter
        list.setOnScrollListener(object : AbsListView.OnScrollListener {
            override fun onScrollStateChanged(view: AbsListView, state: Int) {
                // Only a finger stops the view following; our own scrolling
                // flings without touching.
                if (state == AbsListView.OnScrollListener.SCROLL_STATE_TOUCH_SCROLL && word >= 0) {
                    following = false
                    follow.visibility = View.VISIBLE
                }
            }

            override fun onScroll(view: AbsListView, first: Int, visible: Int, total: Int) = Unit
        })
        follow.setOnClickListener { followVoice() }
        findViewById<View>(R.id.back).setOnClickListener { finish() }
        findViewById<View>(R.id.contents).setOnClickListener { showContents() }
        findViewById<View>(R.id.text_size).setOnClickListener { chooseTextSize() }
        speedButton.setOnClickListener { chooseSpeed() }
        play.setOnClickListener { togglePlay() }
        findViewById<View>(R.id.previous_sentence).setOnClickListener { moveSentence(-1) }
        findViewById<View>(R.id.next_sentence).setOnClickListener { moveSentence(1) }
        findViewById<View>(R.id.previous_chapter).setOnClickListener { moveChapter(-1) }
        findViewById<View>(R.id.next_chapter).setOnClickListener { moveChapter(1) }
        seek.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: SeekBar, progress: Int, fromUser: Boolean) {
                if (fromUser) showTimes(progress.toLong(), bar.max.toLong())
            }

            override fun onStartTrackingTouch(bar: SeekBar) {
                seeking = true
            }

            override fun onStopTrackingTouch(bar: SeekBar) {
                seeking = false
                following = true
                follow.visibility = View.GONE
                controller?.seekTo(bar.progress.toLong())
            }
        })
        adapter.textSize = TEXT_SIZES[library.textSize()]
        showSpeed()
    }

    // Settings changed on another device show up here straight away.
    private val syncListener: (SyncEvents.Change, String?) -> Unit = { change, _ ->
        if (change == SyncEvents.Change.Settings) {
            showSpeed()
            book?.let { controller?.setPlaybackSpeed(library.playbackSpeed(it)) }
            val size = TEXT_SIZES[library.textSize()]
            if (size != adapter.textSize) {
                adapter.textSize = size
                adapter.notifyDataSetChanged()
                list.post { if (following) scrollToWord(smooth = false) }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        controller?.let { attach(it) }
    }

    override fun onStart() {
        super.onStart()
        SyncEvents.listen(syncListener)
        syncListener(SyncEvents.Change.Settings, null)
        val token = SessionToken(this, ComponentName(this, PlaybackService::class.java))
        val future = MediaController.Builder(this, token).buildAsync()
        controllerFuture = future
        future.addListener({
            if (controllerFuture !== future) return@addListener
            val connected = runCatching { future.get() }.getOrNull() ?: return@addListener
            controller = connected
            connected.addListener(playerListener)
            attach(connected)
        }, mainExecutor)
    }

    override fun onStop() {
        SyncEvents.stop(syncListener)
        stopTicking()
        controller?.removeListener(playerListener)
        controller = null
        controllerFuture?.let { MediaController.releaseFuture(it) }
        controllerFuture = null
        super.onStop()
    }

    // Puts this screen's book in the player, unless it is already there (the
    // screen reopened from the notification, or after leaving it playing).
    private fun attach(c: MediaController) {
        val requested = intent.getStringExtra(EXTRA_BOOK)
        val playing = c.currentMediaItem?.mediaId?.let { PlaybackService.parseMediaId(it) }
        val id = requested ?: playing?.first
        val stored = id?.let { book?.takeIf { b -> b.id == it } ?: library.book(it) }
        if (stored == null) {
            Toast.makeText(this, R.string.book_missing, Toast.LENGTH_LONG).show()
            finish()
            return
        }
        if (book?.id != stored.id) {
            chapterIndex = -1
            loadingChapter = -1
            chapter = null
            word = -1
        }
        book = stored
        library.markOpened(stored.id)
        val chapters = stored.manifest.chapters
        if (playing?.first != stored.id || c.mediaItemCount != chapters.size) {
            // Keep the other book's place before replacing it.
            if (playing != null) library.savePosition(playing.first, Position(c.currentMediaItemIndex, c.currentPosition))
            val place = library.position(stored.id)
            c.setMediaItems(
                chapters.indices.map { PlaybackService.chapterItem(this, stored, it) },
                place.chapter.coerceIn(chapters.indices),
                place.ms,
            )
            c.prepare()
        }
        c.setPlaybackSpeed(library.playbackSpeed(stored))
        // Opened by a voice request: read aloud straight away, once, so a
        // rotation or a return to the screen does not start it again.
        if (intent.getBooleanExtra(EXTRA_PLAY, false)) {
            intent.removeExtra(EXTRA_PLAY)
            if (c.playbackState == Player.STATE_IDLE) c.prepare()
            c.play()
        }
        following = true
        follow.visibility = View.GONE
        updatePlayButton()
        update()
        if (c.isPlaying) startTicking()
    }

    private val playerListener = object : Player.Listener {
        override fun onIsPlayingChanged(isPlaying: Boolean) {
            updatePlayButton()
            if (isPlaying) startTicking() else stopTicking()
            update()
        }

        override fun onPlaybackStateChanged(state: Int) {
            updatePlayButton()
            update()
        }

        override fun onMediaItemTransition(item: androidx.media3.common.MediaItem?, reason: Int) = update()

        override fun onPositionDiscontinuity(
            oldPosition: Player.PositionInfo,
            newPosition: Player.PositionInfo,
            reason: Int,
        ) = update()

        override fun onPlayerError(error: PlaybackException) {
            Toast.makeText(
                this@ReaderActivity,
                getString(R.string.playback_error, error.errorCodeName),
                Toast.LENGTH_LONG,
            ).show()
        }
    }

    private val frame = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!ticking) return
            update()
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    private fun startTicking() {
        if (ticking) return
        ticking = true
        // Reading along needs the screen; pressing power still locks it.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        Choreographer.getInstance().postFrameCallback(frame)
    }

    private fun stopTicking() {
        ticking = false
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        Choreographer.getInstance().removeFrameCallback(frame)
    }

    // Brings the screen to where playback is: the chapter, the word, the bar.
    private fun update() {
        val c = controller ?: return
        val b = book ?: return
        val index = c.currentMediaItemIndex
        if (index !in b.manifest.chapters.indices) return
        if (index != chapterIndex) {
            showChapter(index)
            return
        }
        val text = chapter ?: return
        val position = c.currentPosition
        val current = text.wordAt(position)
        if (current != word) setWord(current)
        val now = SystemClock.uptimeMillis()
        if (!seeking && (now - lastProgressUpdate > 250 || !ticking)) {
            lastProgressUpdate = now
            val duration = c.duration.takeIf { it > 0 } ?: b.manifest.chapters[index].durationMs
            seek.max = duration.toInt().coerceAtLeast(1)
            seek.progress = position.toInt()
            showTimes(position, duration)
        }
    }

    private fun showChapter(index: Int) {
        val b = book ?: return
        if (index == loadingChapter) return
        loadingChapter = index
        chapterTitle.text = b.manifest.chapters[index].title.ifEmpty { b.manifest.title }
        Background.run({ b.loadChapter(index) }) { result ->
            if (loadingChapter != index || book !== b) return@run
            val text = result.getOrElse {
                Toast.makeText(this, R.string.error_damaged, Toast.LENGTH_LONG).show()
                return@run
            }
            chapterIndex = index
            chapter = text
            word = -1
            adapter.setChapter(text)
            list.setSelection(0)
            following = true
            follow.visibility = View.GONE
            update()
            scrollToWord(smooth = false)
        }
    }

    private fun setWord(newWord: Int) {
        val text = chapter ?: return
        val oldParagraph = if (word >= 0) text.paragraph(word) else -1
        word = newWord
        if (newWord < 0) {
            adapter.highlight(-1, 0, 0)
        } else {
            adapter.highlight(text.paragraph(newWord), text.start(newWord), text.end(newWord))
        }
        if (oldParagraph >= 0 && oldParagraph != adapter.highlightParagraph) refreshRow(oldParagraph)
        if (adapter.highlightParagraph >= 0) refreshRow(adapter.highlightParagraph)
        if (following) scrollToWord(smooth = true)
    }

    // Rebinds one visible paragraph in place rather than the whole list.
    private fun refreshRow(paragraph: Int) {
        val child = list.getChildAt(paragraph - list.firstVisiblePosition) as? ParagraphView ?: return
        adapter.bind(child, paragraph)
    }

    // Keeps the word being heard in the upper part of the screen, scrolling
    // only when it would leave the comfortable band in the middle.
    private fun scrollToWord(smooth: Boolean, refined: Boolean = false) {
        val text = chapter ?: return
        if (word < 0 || list.height == 0) {
            if (list.height == 0) list.post { if (following) scrollToWord(smooth, refined) }
            return
        }
        val paragraph = text.paragraph(word)
        val height = list.height
        val child = list.getChildAt(paragraph - list.firstVisiblePosition) as? ParagraphView
        val layout = child?.layout
        if (child == null || layout == null) {
            if (refined) return
            list.setSelectionFromTop(paragraph, height / 4)
            list.post { if (following) scrollToWord(smooth = false, refined = true) }
            return
        }
        val line = layout.getLineForOffset(text.start(word))
        val top = child.top + child.totalPaddingTop + layout.getLineTop(line)
        val bottom = child.top + child.totalPaddingTop + layout.getLineBottom(line)
        if (top < height * 0.12 || bottom > height * 0.7) {
            val delta = top - height / 4
            if (smooth) list.smoothScrollBy(delta, 400) else list.scrollListBy(delta)
        }
    }

    private fun followVoice() {
        following = true
        follow.visibility = View.GONE
        scrollToWord(smooth = true)
    }

    private fun togglePlay() {
        val c = controller ?: return
        if (c.isPlaying) {
            c.pause()
        } else {
            if (c.playbackState == Player.STATE_ENDED) c.seekTo(0, 0)
            if (c.playbackState == Player.STATE_IDLE) c.prepare()
            c.play()
        }
    }

    private fun playFrom(chapterIndex: Int, ms: Long) {
        val c = controller ?: return
        following = true
        follow.visibility = View.GONE
        if (chapterIndex == c.currentMediaItemIndex) c.seekTo(ms) else c.seekTo(chapterIndex, ms)
        if (c.playbackState == Player.STATE_IDLE) c.prepare()
        c.play()
    }

    private fun moveSentence(delta: Int) {
        val text = chapter ?: return
        val b = book ?: return
        if (text.wordCount == 0) return
        val sentence = text.sentenceOf(word.coerceAtLeast(0)) + delta
        when {
            sentence < 0 -> playFrom(chapterIndex, 0)
            sentence >= text.sentenceCount ->
                if (chapterIndex + 1 < b.manifest.chapters.size) playFrom(chapterIndex + 1, 0)
            else -> playFrom(chapterIndex, text.startMs(text.sentenceStart(sentence)))
        }
    }

    // Back restarts the chapter, unless it has only just begun.
    private fun moveChapter(delta: Int) {
        val c = controller ?: return
        val b = book ?: return
        val target = if (delta < 0 && c.currentPosition > 3000) chapterIndex else chapterIndex + delta
        if (target in b.manifest.chapters.indices) playFrom(target, 0)
    }

    private fun showContents() {
        val b = book ?: return
        val titles = b.manifest.chapters.mapIndexed { i, c -> c.title.ifEmpty { "${i + 1}" } }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle(R.string.contents)
            .setSingleChoiceItems(titles, chapterIndex) { dialog, which ->
                dialog.dismiss()
                val c = controller ?: return@setSingleChoiceItems
                following = true
                c.seekTo(which, 0)
            }
            .show()
    }

    private fun speed() = library.speed()

    private fun formatSpeed(value: Float) =
        getString(R.string.speed_value, NumberFormat.getNumberInstance().apply { maximumFractionDigits = 2 }.format(value))

    private fun showSpeed() {
        speedButton.text = formatSpeed(speed())
    }

    private fun chooseSpeed() {
        val current = SPEEDS.indexOfFirst { it == speed() }
        AlertDialog.Builder(this)
            .setTitle(R.string.speed)
            .setSingleChoiceItems(SPEEDS.map { formatSpeed(it) }.toTypedArray(), current) { dialog, which ->
                dialog.dismiss()
                library.setSpeed(SPEEDS[which])
                book?.let { controller?.setPlaybackSpeed(library.playbackSpeed(it)) }
                showSpeed()
            }
            .show()
    }

    private fun chooseTextSize() {
        val names = listOf(
            R.string.text_size_small, R.string.text_size_medium, R.string.text_size_large,
            R.string.text_size_larger, R.string.text_size_largest,
        ).map { getString(it) }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle(R.string.text_size)
            .setSingleChoiceItems(names, library.textSize()) { dialog, which ->
                dialog.dismiss()
                library.setTextSize(which)
                adapter.textSize = TEXT_SIZES[which]
                adapter.notifyDataSetChanged()
                list.post { if (following) scrollToWord(smooth = false) }
            }
            .show()
    }

    private fun updatePlayButton() {
        val playing = controller?.isPlaying == true
        play.setImageResource(if (playing) R.drawable.ic_pause else R.drawable.ic_play)
        play.contentDescription = getString(if (playing) R.string.pause else R.string.play)
    }

    private fun showTimes(position: Long, duration: Long) {
        elapsed.text = clock(position)
        remaining.text = getString(R.string.time_left, clock((duration - position).coerceAtLeast(0)))
    }

    private fun clock(ms: Long): String {
        val seconds = ms / 1000
        val h = seconds / 3600
        val m = seconds / 60 % 60
        val s = seconds % 60
        return if (h > 0) "%d:%02d:%02d".format(h, m, s) else "%d:%02d".format(m, s)
    }

    // The chapter's paragraphs, with one word highlighted. The highlight is
    // a single span moved from word to word inside the paragraph's own text,
    // which only redraws that paragraph; nothing is laid out again.
    private inner class ParagraphAdapter : BaseAdapter() {
        private var text: ChapterText? = null
        private val span by lazy { BackgroundColorSpan(getColor(R.color.highlight)) }
        private val body = Typeface.SERIF
        private val bold = Typeface.create(Typeface.SERIF, Typeface.BOLD)
        var highlightParagraph = -1
            private set
        private var highlightStart = 0
        private var highlightEnd = 0
        var textSize = TEXT_SIZES[2]

        fun setChapter(chapter: ChapterText) {
            text = chapter
            highlightParagraph = -1
            notifyDataSetChanged()
        }

        fun highlight(paragraph: Int, start: Int, end: Int) {
            highlightParagraph = paragraph
            highlightStart = start
            highlightEnd = end
        }

        override fun getCount() = text?.paragraphs?.size ?: 0
        override fun getItem(position: Int) = text!!.paragraphs[position]
        override fun getItemId(position: Int) = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val view = (convertView ?: layoutInflater.inflate(R.layout.item_paragraph, parent, false)) as ParagraphView
            view.tag = null
            bind(view, position)
            return view
        }

        fun bind(view: ParagraphView, position: Int) {
            val chapter = text ?: return
            val paragraph = chapter.paragraphs[position]
            if (view.tag != position) {
                // Headings scale down with their level: h1 half again as big
                // as the text, h6 just a little bigger.
                val scale = if (paragraph.isHeading) (1.5f - 0.08f * (paragraph.level - 1)).coerceAtLeast(1.1f) else 1f
                view.setTextSize(TypedValue.COMPLEX_UNIT_SP, textSize * scale)
                view.typeface = if (paragraph.isHeading) bold else body
                view.setText(SpannableString(paragraph.text), TextView.BufferType.SPANNABLE)
                view.onTap = { offset -> onTap(position, offset) }
                view.tag = position
            }
            val spannable = view.text as? Spannable ?: return
            if (position == highlightParagraph && highlightEnd <= spannable.length) {
                spannable.setSpan(span, highlightStart, highlightEnd, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE)
            } else if (spannable.getSpanStart(span) >= 0) {
                spannable.removeSpan(span)
            }
        }

        override fun notifyDataSetChanged() {
            super.notifyDataSetChanged()
            // Rebinding every visible row picks up a new size or chapter.
            for (i in 0 until list.childCount) (list.getChildAt(i) as? ParagraphView)?.tag = null
        }
    }

    private fun onTap(paragraph: Int, offset: Int) {
        val text = chapter ?: return
        val target = text.wordAtOffset(paragraph, offset)
        if (target < 0) return
        playFrom(chapterIndex, text.startMs(target))
    }

    companion object {
        const val EXTRA_BOOK = "book"
        const val EXTRA_PLAY = "play"
        private val SPEEDS = Library.SPEEDS
        private val TEXT_SIZES = Library.TEXT_SIZES
    }
}
