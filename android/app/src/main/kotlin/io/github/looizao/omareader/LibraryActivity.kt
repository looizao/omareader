package io.github.looizao.omareader

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.MediaStore
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.ImageView
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast

// The books on the phone, most recently opened first. Books arrive through
// the system file picker (which lists Google Drive, Dropbox and any other
// installed storage) or through "Open with" from those apps.
class LibraryActivity : Activity() {
    private lateinit var library: Library
    private lateinit var list: ListView
    private lateinit var empty: View
    private lateinit var busy: View
    private val adapter = BookAdapter()
    private var importing = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_library)
        library = Library(this)
        list = findViewById(R.id.books)
        empty = findViewById(R.id.empty)
        busy = findViewById(R.id.busy)
        list.adapter = adapter
        list.setOnItemClickListener { _, _, position, _ -> open(adapter.getItem(position)) }
        list.setOnItemLongClickListener { _, _, position, _ ->
            confirmRemove(adapter.getItem(position))
            true
        }
        findViewById<View>(R.id.add).setOnClickListener { pick() }
        findViewById<View>(R.id.empty_add).setOnClickListener { pick() }

        Background.run({ library.cleanUp() }) {}
        // A book shared into the app is imported once, not again when the
        // activity is recreated.
        if (savedInstanceState == null) handle(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handle(intent)
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun handle(intent: Intent?) {
        if (intent?.action == MediaStore.INTENT_ACTION_MEDIA_PLAY_FROM_SEARCH) {
            playFromSearch(intent.getStringExtra(android.app.SearchManager.QUERY))
            return
        }
        val uri: Uri? = when (intent?.action) {
            Intent.ACTION_VIEW -> intent.data
            Intent.ACTION_SEND -> if (Build.VERSION.SDK_INT >= 33) {
                intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(Intent.EXTRA_STREAM)
            }
            else -> null
        }
        if (uri != null) import(uri)
    }

    private fun refresh() {
        Background.run({ library.books().map { it to library.position(it.id) } }) { result ->
            val books = result.getOrDefault(emptyList())
            adapter.set(books)
            empty.visibility = if (books.isEmpty() && !importing) View.VISIBLE else View.GONE
        }
    }

    private fun pick() {
        if (importing) return
        // Drive and Dropbox seldom know an .omabook's type, so any file can
        // be picked and the import checks what it is.
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType("*/*")
        startActivityForResult(intent, PICK)
    }

    @Deprecated("Platform Activity results")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == PICK && resultCode == RESULT_OK) data?.data?.let { import(it) }
    }

    private fun import(uri: Uri) {
        if (importing) return
        importing = true
        busy.visibility = View.VISIBLE
        empty.visibility = View.GONE
        Toast.makeText(this, R.string.importing, Toast.LENGTH_SHORT).show()
        Background.run({ library.import(uri) }) { result ->
            importing = false
            busy.visibility = View.GONE
            result.onSuccess { book ->
                Toast.makeText(this, getString(R.string.imported, book.manifest.title), Toast.LENGTH_SHORT).show()
                open(book)
            }.onFailure { error ->
                Toast.makeText(this, message(error), Toast.LENGTH_LONG).show()
                refresh()
            }
        }
    }

    private fun message(error: Throwable): String = getString(
        when ((error as? BundleException)?.reason) {
            BundleException.Reason.NotABundle -> R.string.error_not_a_bundle
            BundleException.Reason.TooNew -> R.string.error_too_new
            BundleException.Reason.Damaged -> R.string.error_damaged
            else -> R.string.error_unreadable
        },
    )

    private fun open(book: StoredBook, play: Boolean = false) {
        startActivity(
            Intent(this, ReaderActivity::class.java)
                .putExtra(ReaderActivity.EXTRA_BOOK, book.id)
                .putExtra(ReaderActivity.EXTRA_PLAY, play),
        )
    }

    // The book the assistant heard, read aloud from where it was left; with
    // no title, the book opened last.
    private fun playFromSearch(query: String?) {
        Background.run({ CarCatalog.find(library.books(), query) }) { result ->
            val book = result.getOrNull()
            if (book != null) {
                open(book, play = true)
            } else {
                Toast.makeText(this, getString(R.string.no_book_named, query.orEmpty()), Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun confirmRemove(book: StoredBook) {
        AlertDialog.Builder(this)
            .setTitle(getString(R.string.remove_title, book.manifest.title))
            .setMessage(R.string.remove_message)
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(R.string.remove) { _, _ ->
                Background.run({ library.delete(book) }) { refresh() }
            }
            .show()
    }

    private inner class BookAdapter : BaseAdapter() {
        private var books: List<Pair<StoredBook, Position>> = emptyList()
        private val covers = HashMap<String, Bitmap?>()

        fun set(items: List<Pair<StoredBook, Position>>) {
            books = items
            notifyDataSetChanged()
        }

        override fun getCount() = books.size
        override fun getItem(position: Int) = books[position].first
        override fun getItemId(position: Int) = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val view = convertView ?: layoutInflater.inflate(R.layout.item_book, parent, false)
            val (book, place) = books[position]
            val manifest = book.manifest
            view.findViewById<TextView>(R.id.title).text = manifest.title
            view.findViewById<TextView>(R.id.author).apply {
                text = manifest.author
                visibility = if (manifest.author.isEmpty()) View.GONE else View.VISIBLE
            }
            view.findViewById<TextView>(R.id.progress).text =
                if (place == Position.START) getString(R.string.not_started)
                else getString(R.string.chapter_of, place.chapter + 1, manifest.chapters.size)
            val cover = view.findViewById<ImageView>(R.id.cover)
            cover.tag = book.id
            if (covers.containsKey(book.id)) {
                cover.setImageBitmap(covers[book.id])
            } else {
                cover.setImageDrawable(null)
                Background.run({ book.cover()?.let { decodeCover(it.path) } }) { result ->
                    covers[book.id] = result.getOrNull()
                    if (cover.tag == book.id) cover.setImageBitmap(covers[book.id])
                }
            }
            return view
        }

        // Covers are up to 1400 px; the list shows them at 72 dp, so decode
        // a fraction of the pixels.
        private fun decodeCover(path: String): Bitmap? {
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeFile(path, bounds)
            val target = (72 * resources.displayMetrics.density).toInt().coerceAtLeast(1)
            var sample = 1
            while (bounds.outHeight / (sample * 2) >= target) sample *= 2
            return BitmapFactory.decodeFile(path, BitmapFactory.Options().apply { inSampleSize = sample })
        }
    }

    private companion object {
        const val PICK = 1
    }
}
