package io.github.looizao.omareader

import android.content.ContentProvider
import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import java.io.FileNotFoundException

// Hands book covers to Android Auto and the system's media controls, which
// draw them in their own processes and cannot read the app's files. It is not
// exported: PlaybackService grants read access to the apps it lets browse
// the library, and no one else can open a cover.
class CoverProvider : ContentProvider() {
    override fun onCreate() = true

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        if (mode != "r") throw SecurityException("Covers are read-only")
        val context = context ?: throw FileNotFoundException(uri.toString())
        // Library.book checks the id, so a path cannot reach outside the books.
        val cover = Library(context).book(uri.lastPathSegment)?.cover()?.takeIf { it.isFile }
            ?: throw FileNotFoundException(uri.toString())
        return ParcelFileDescriptor.open(cover, ParcelFileDescriptor.MODE_READ_ONLY)
    }

    override fun getType(uri: Uri) = "image/jpeg"

    override fun query(
        uri: Uri,
        projection: Array<out String>?,
        selection: String?,
        selectionArgs: Array<out String>?,
        sortOrder: String?,
    ) = null

    override fun insert(uri: Uri, values: ContentValues?): Uri? = null

    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?) = 0

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = 0

    companion object {
        fun root(context: Context): Uri = Uri.Builder()
            .scheme("content")
            .authority(context.packageName + ".covers")
            .build()

        fun uri(context: Context, book: StoredBook): Uri? =
            book.cover()?.takeIf { it.isFile }?.let { root(context).buildUpon().appendPath(book.id).build() }
    }
}
