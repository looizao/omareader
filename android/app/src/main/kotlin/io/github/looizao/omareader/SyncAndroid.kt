package io.github.looizao.omareader

import android.app.job.JobInfo
import android.app.job.JobParameters
import android.app.job.JobScheduler
import android.app.job.JobService
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.text.Html
import com.google.android.gms.common.api.ApiException
import java.io.File
import java.io.IOException
import java.security.KeyStore
import java.util.concurrent.CopyOnWriteArraySet
import java.util.concurrent.atomic.AtomicReference
import java.util.concurrent.locks.ReentrantLock
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

// Sync on the phone, through Dropbox or Google Drive: the account, the job
// that syncs, and the library as the sync engine sees it. The rules
// themselves are in Sync.kt and SyncEngine.kt, which run without Android.

// Dropbox's refresh token, the only credential kept (Drive's grant stays
// with Play services), encrypted with an AES key that never leaves the
// Android Keystore. Backups exclude the app's data anyway.
object TokenStore {
    private const val ALIAS = "omareader-dropbox"
    private const val KEY = "token"

    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return generator.generateKey()
    }

    fun save(context: Context, token: String) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, key()) }
        val sealed = cipher.iv + cipher.doFinal(token.toByteArray(Charsets.UTF_8))
        prefs(context).edit().putString(KEY, android.util.Base64.encodeToString(sealed, android.util.Base64.NO_WRAP)).apply()
    }

    fun load(context: Context): String? {
        val saved = prefs(context).getString(KEY, null) ?: return null
        return runCatching {
            val sealed = android.util.Base64.decode(saved, android.util.Base64.NO_WRAP)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, sealed, 0, 12))
            String(cipher.doFinal(sealed, 12, sealed.size - 12), Charsets.UTF_8)
        }.getOrNull()
    }

    fun clear(context: Context) {
        prefs(context).edit().remove(KEY).apply()
    }

    fun prefs(context: Context) = context.applicationContext.getSharedPreferences("sync", Context.MODE_PRIVATE)
}

// What the sync screen and the library listen for. Calls arrive on the main
// thread.
object SyncEvents {
    enum class Change { Status, Library, Position, Settings }

    private val listeners = CopyOnWriteArraySet<(Change, String?) -> Unit>()
    private val main = Handler(Looper.getMainLooper())

    fun listen(listener: (Change, String?) -> Unit) = listeners.add(listener)
    fun stop(listener: (Change, String?) -> Unit) = listeners.remove(listener)

    fun post(change: Change, id: String? = null) {
        main.post { listeners.forEach { it(change, id) } }
    }
}

// The book in the player and whether it is playing, which the sync needs to
// leave a book being listened to alone.
object PlayerState {
    @Volatile var bookId: String? = null
    @Volatile var playing = false

    fun isPlaying(id: String) = playing && bookId == id
}

// The connection to Dropbox or Google Drive, one at a time, and how the last
// sync went.
object SyncAccount {
    enum class Provider { Dropbox, Drive }
    enum class Problem { Network, Revoked, Server, Storage, NoPlayServices }

    private const val PROVIDER = "provider"
    private const val ACCOUNT = "account"
    private const val DRIVE_EMAIL = "driveEmail"
    private const val LAST_SYNC = "lastSync"
    private const val PROBLEM = "problem"
    private const val PROBLEM_PROVIDER = "problemProvider"
    private const val MOBILE_DATA = "mobileData"
    private const val TOKEN = "token"
    private const val CONNECT_TIMEOUT_MS = 5 * 60 * 1000

    private var tokens: RefreshingTokens? = null
    private val connecting = AtomicReference<Provider?>(null)

    val dropboxAvailable get() = BuildConfig.DROPBOX_APP_KEY.isNotEmpty()
    val driveAvailable get() = BuildConfig.GOOGLE_DRIVE_ENABLED
    val available get() = dropboxAvailable || driveAvailable

    // The provider this phone syncs through. A phone connected before Drive
    // existed has a Dropbox token and no provider saved.
    fun provider(context: Context): Provider? {
        val prefs = TokenStore.prefs(context)
        return when (prefs.getString(PROVIDER, Provider.Dropbox.name)) {
            Provider.Drive.name -> Provider.Drive.takeIf { driveAvailable }
            Provider.Dropbox.name -> Provider.Dropbox.takeIf { dropboxAvailable && prefs.contains(TOKEN) }
            else -> null
        }
    }

    fun connected(context: Context) = provider(context) != null
    fun isConnecting() = connecting.get() != null
    fun connectingTo(): Provider? = connecting.get()
    fun account(context: Context) = TokenStore.prefs(context).getString(ACCOUNT, "").orEmpty()
    fun lastSync(context: Context) = TokenStore.prefs(context).getLong(LAST_SYNC, 0)
    fun problem(context: Context) =
        TokenStore.prefs(context).getString(PROBLEM, null)?.let { runCatching { Problem.valueOf(it) }.getOrNull() }

    // Which provider the problem was with, which may no longer be connected.
    fun problemProvider(context: Context) =
        TokenStore.prefs(context).getString(PROBLEM_PROVIDER, null)?.let { runCatching { Provider.valueOf(it) }.getOrNull() }
            ?: provider(context) ?: Provider.Dropbox

    fun mobileData(context: Context) = TokenStore.prefs(context).getBoolean(MOBILE_DATA, false)

    fun setMobileData(context: Context, allowed: Boolean) {
        TokenStore.prefs(context).edit().putBoolean(MOBILE_DATA, allowed).apply()
        SyncScheduler.soon(context, 0)
    }

    // What a sync talks to. Drive asks Play services for a fresh token at
    // each sync, so its remote is made anew every time.
    fun remote(context: Context): Remote? = when (provider(context)) {
        Provider.Dropbox -> dropboxRemote(context)
        Provider.Drive -> DriveRemote(PlayTokens(context, TokenStore.prefs(context).getString(DRIVE_EMAIL, null)))
        null -> null
    }

    @Synchronized
    private fun dropboxRemote(context: Context): DropboxRemote? {
        if (!dropboxAvailable) return null
        val existing = tokens
        if (existing != null) return DropboxRemote(existing)
        val refresh = TokenStore.load(context) ?: return null
        return DropboxRemote(RefreshingTokens(BuildConfig.DROPBOX_APP_KEY, refresh).also { tokens = it })
    }

    // Opens Dropbox's consent page in the browser and waits on the loopback
    // for it to come back. The page the browser lands on says how it went
    // and offers a way back to the app.
    fun connectDropbox(context: Context): Boolean {
        if (!dropboxAvailable || !connecting.compareAndSet(null, Provider.Dropbox)) return false
        val app = context.applicationContext
        val receiver = try {
            LoopbackReceiver.open()
        } catch (e: IOException) {
            connecting.set(null)
            setProblem(app, Problem.Network, Provider.Dropbox)
            return false
        }
        val verifier = DropboxOAuth.verifier()
        val state = DropboxOAuth.state()
        val key = BuildConfig.DROPBOX_APP_KEY
        Thread({
            try {
                receiver.await(CONNECT_TIMEOUT_MS) { redirect ->
                    val connected = redirect.state == state && redirect.code != null && finishConnect(
                        app, key, redirect.code, verifier, receiver.port,
                    )
                    page(app, connected)
                }
            } finally {
                connecting.set(null)
                SyncEvents.post(SyncEvents.Change.Status)
            }
        }, "omareader-connect").start()
        context.startActivity(
            Intent(Intent.ACTION_VIEW, Uri.parse(DropboxOAuth.authorizeUrl(key, verifier, state, receiver.port)))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
        )
        SyncEvents.post(SyncEvents.Change.Status)
        return true
    }

    private fun finishConnect(app: Context, key: String, code: String, verifier: String, port: Int): Boolean = try {
        val result = DropboxOAuth.exchange(key, code, verifier, port)
        val refresh = result.refresh ?: throw IOException("Dropbox sent no refresh token")
        TokenStore.save(app, refresh)
        synchronized(this) { tokens = RefreshingTokens(key, refresh) }
        val name = runCatching { dropboxRemote(app)?.account() }.getOrNull().orEmpty()
        TokenStore.prefs(app).edit()
            .putString(PROVIDER, Provider.Dropbox.name).putString(ACCOUNT, name)
            .remove(PROBLEM).remove(PROBLEM_PROVIDER)
            .apply()
        SyncScheduler.start(app)
        true
    } catch (e: Exception) {
        setProblem(app, Problem.Network, Provider.Dropbox)
        false
    }

    private fun page(context: Context, connected: Boolean): String {
        fun text(id: Int) = Html.escapeHtml(context.getString(id))
        val back = "intent://sync#Intent;scheme=omareader;package=${context.packageName};end"
        val title = text(if (connected) R.string.sync_page_connected else R.string.sync_page_failed)
        return """<!doctype html><html><head><meta charset="utf-8">
            <meta name="viewport" content="width=device-width, initial-scale=1"><title>omareader</title>
            <style>body{font-family:serif;background:#FAF8F3;color:#1C1B19;margin:0;padding:48px 24px;text-align:center}
            a{display:inline-block;margin-top:24px;padding:12px 28px;border-radius:999px;background:#FFD60A;
            color:#16161A;text-decoration:none;font-family:sans-serif}</style></head>
            <body><h1>$title</h1><a href="$back">${text(R.string.sync_page_back)}</a></body></html>"""
    }

    // Asks Play services for Drive. A user who agreed before is connected at
    // once; otherwise Play services hands back its consent sheet, which the
    // sync screen shows and passes the answer of to driveAnswered.
    fun connectDrive(context: Context, showSheet: (android.app.PendingIntent) -> Unit) {
        if (!driveAvailable || !connecting.compareAndSet(null, Provider.Drive)) return
        val app = context.applicationContext
        if (!DriveAuth.playServices(app)) {
            connecting.set(null)
            setProblem(app, Problem.NoPlayServices, Provider.Drive)
            SyncEvents.post(SyncEvents.Change.Status)
            return
        }
        DriveAuth.client(context).authorize(DriveAuth.request(null))
            .addOnSuccessListener { result ->
                val sheet = result.pendingIntent
                if (result.hasResolution() && sheet != null) showSheet(sheet) else finishDrive(app, result.accessToken)
            }
            .addOnFailureListener { failDrive(app) }
        SyncEvents.post(SyncEvents.Change.Status)
    }

    // The consent sheet's answer. Closing it without agreeing is no problem
    // to report; the user just isn't connected.
    fun driveAnswered(context: Context, agreed: Boolean, data: Intent?) {
        val app = context.applicationContext
        if (!agreed) {
            connecting.set(null)
            SyncEvents.post(SyncEvents.Change.Status)
            return
        }
        val token = try {
            DriveAuth.client(context).getAuthorizationResultFromIntent(data).accessToken
        } catch (e: ApiException) {
            null
        }
        finishDrive(app, token)
    }

    private fun finishDrive(app: Context, token: String?) {
        if (token == null) return failDrive(app)
        Thread({
            try {
                val user = DriveRemote(PlayTokens(app, null, token)).account()
                TokenStore.prefs(app).edit()
                    .putString(PROVIDER, Provider.Drive.name).putString(ACCOUNT, user.label).putString(DRIVE_EMAIL, user.email)
                    .remove(PROBLEM).remove(PROBLEM_PROVIDER)
                    .apply()
                SyncScheduler.start(app)
            } catch (e: Exception) {
                setProblem(app, Problem.Network, Provider.Drive)
            } finally {
                connecting.set(null)
                SyncEvents.post(SyncEvents.Change.Status)
            }
        }, "omareader-connect").start()
    }

    private fun failDrive(app: Context) {
        setProblem(app, Problem.Network, Provider.Drive)
        connecting.set(null)
        SyncEvents.post(SyncEvents.Change.Status)
    }

    // Forgets the connection here and ends it on the provider's side. The
    // files there stay.
    fun disconnect(context: Context) {
        val app = context.applicationContext
        val prefs = TokenStore.prefs(app)
        val provider = provider(app)
        val dropbox = dropboxRemote(app)
        val email = prefs.getString(DRIVE_EMAIL, null)
        synchronized(this) { tokens = null }
        TokenStore.clear(app)
        prefs.edit()
            .remove(PROVIDER).remove(ACCOUNT).remove(DRIVE_EMAIL).remove(LAST_SYNC).remove(PROBLEM).remove(PROBLEM_PROVIDER)
            .apply()
        PhoneSyncStore.forgetRemote(app)
        SyncScheduler.stop(app)
        SyncEvents.post(SyncEvents.Change.Status)
        if (dropbox != null) Thread({ runCatching { dropbox.revoke() } }, "omareader-revoke").start()
        if (provider == Provider.Drive && !email.isNullOrEmpty()) DriveAuth.revoke(app, email)
    }

    fun synced(context: Context) {
        TokenStore.prefs(context).edit().putLong(LAST_SYNC, System.currentTimeMillis())
            .remove(PROBLEM).remove(PROBLEM_PROVIDER).apply()
    }

    fun setProblem(context: Context, problem: Problem, provider: Provider? = provider(context)) {
        val editor = TokenStore.prefs(context).edit().putString(PROBLEM, problem.name)
        if (provider != null) editor.putString(PROBLEM_PROVIDER, provider.name) else editor.remove(PROBLEM_PROVIDER)
        editor.apply()
    }
}

// The library as the sync engine sees it.
class PhoneSyncStore(private val context: Context, private val library: Library = Library(context)) : SyncStore {
    private val prefs = TokenStore.prefs(context)
    override val device = library.device

    override fun localIds() = library.knownIds()

    override fun localState(id: String): BookState? {
        val present = library.presentRegister(id)
        val book = library.book(id)
        val position = book?.let { b ->
            library.positionRegister(id)?.let { saved ->
                runCatching { Anchors.fromPosition(b, saved.value) }.getOrNull()?.let { Register(it, saved.at, saved.by) }
            }
        }
        if (present == null && position == null) return null
        val (title, author) = library.name(id)
        return BookState(id, title, author, present, position)
    }

    override fun applyState(merged: BookState) {
        val id = merged.id
        val present = merged.present
        if (present != null && !present.sameChange(library.presentRegister(id))) {
            library.setPresent(id, present)
        }
        val position = merged.position ?: return
        val book = library.book(id) ?: return
        val local = library.positionRegister(id)
        if (position.sameChange(local) || PlayerState.isPlaying(id)) return
        val place = runCatching { Anchors.toPosition(book, position.value) }.getOrNull() ?: return
        library.setPosition(id, Register(place, position.at, position.by))
        SyncEvents.post(SyncEvents.Change.Position, id)
    }

    // Removals that couldn't happen during the sync (the book was playing)
    // happen on a later one.
    fun settle(): Boolean {
        var removed = false
        for (book in library.books()) {
            if (library.presentRegister(book.id)?.value == false && !PlayerState.isPlaying(book.id)) {
                library.removeFiles(book.id)
                removed = true
            }
        }
        return removed
    }

    override fun localSettings(): SyncSettings {
        var settings = SyncSettings()
        library.speedRegister()?.let { settings = settings.with(SyncSettings.SPEED, Register(it.value, it.at, it.by)) }
        library.textSizeRegister()?.let { settings = settings.with(SyncSettings.TEXT_SIZE_PHONE, Register(it.value, it.at, it.by)) }
        return settings
    }

    override fun applySettings(merged: SyncSettings) {
        var changed = false
        merged[SyncSettings.SPEED]?.let { r ->
            val value = (r.value as? Number)?.toDouble() ?: return@let
            if (!r.sameChange(library.speedRegister())) {
                library.setSpeed(Register(value, r.at, r.by))
                changed = true
            }
        }
        merged[SyncSettings.TEXT_SIZE_PHONE]?.let { r ->
            val value = (r.value as? Number)?.toDouble() ?: return@let
            if (!r.sameChange(library.textSizeRegister())) {
                library.setTextSize(Register(value, r.at, r.by))
                changed = true
            }
        }
        if (changed) SyncEvents.post(SyncEvents.Change.Settings)
    }

    override fun cached(path: String): Pair<String, String>? {
        val saved = prefs.getString(CACHE + path, null) ?: return null
        return saved.substringBefore('\n') to saved.substringAfter('\n')
    }

    override fun cache(path: String, rev: String, text: String) {
        prefs.edit().putString(CACHE + path, rev + "\n" + text).apply()
    }

    override fun forget(path: String) {
        prefs.edit().remove(CACHE + path).apply()
    }

    override fun bundleRev(id: String): String? =
        prefs.getString(BUNDLE + id, null)?.takeIf { library.book(id) != null }

    override fun partialFile(id: String, rev: String) =
        File(File(context.noBackupFilesDir, "downloads"), "$id-${rev.filter { it.isLetterOrDigit() }}.part")

    override fun importBundle(id: String, file: File, rev: String) {
        val book = library.importFile(file)
        if (book.id != id) {
            // A bundle whose name says another book than its manifest.
            library.removeFiles(book.id)
            throw BundleException(BundleException.Reason.Damaged)
        }
        prefs.edit().putString(BUNDLE + id, rev).apply()
        // The position Dropbox has for the book could not be placed before
        // the book was here.
        cached("/state/$id.json")?.let { (_, text) ->
            runCatching { BookState.parse(text) }.getOrNull()?.let { state ->
                val local = library.positionRegister(id)
                val remote = state.position
                if (remote != null && (local == null || remote.beats(local))) applyState(state.copy(present = null))
            }
        }
    }

    companion object {
        private const val CACHE = "cache:"
        private const val BUNDLE = "bundleRev:"

        // Another account, or another provider, may be connected next;
        // nothing read from this one should be taken for its files.
        fun forgetRemote(context: Context) {
            val prefs = TokenStore.prefs(context)
            val editor = prefs.edit()
            prefs.all.keys.filter { it.startsWith(CACHE) || it.startsWith(BUNDLE) }.forEach { editor.remove(it) }
            editor.apply()
        }
    }
}

// Runs one sync at a time, from the job or from the sync screen's button.
object SyncRunner {
    private val lock = ReentrantLock()

    // Returns whether the job should be tried again later.
    fun run(context: Context, cancelled: () -> Boolean = { false }): Boolean {
        val app = context.applicationContext
        lock.lock()
        try {
            val remote = SyncAccount.remote(app) ?: return false
            val store = PhoneSyncStore(app)
            val connectivity = app.getSystemService(ConnectivityManager::class.java)
            val downloads = SyncAccount.mobileData(app) || connectivity?.isActiveNetworkMetered == false
            val report = SyncEngine(remote, store).sync(downloads, cancelled)
            val removed = store.settle()
            SyncAccount.synced(app)
            if (report.waiting > 0) SyncScheduler.whenUnmetered(app)
            if (report.changedLibrary || removed) SyncEvents.post(SyncEvents.Change.Library)
            return false
        } catch (e: RevokedException) {
            // The provider no longer knows this phone; it has to connect
            // again.
            val provider = SyncAccount.provider(app)
            SyncAccount.disconnect(app)
            SyncAccount.setProblem(app, SyncAccount.Problem.Revoked, provider)
            return false
        } catch (e: InterruptedException) {
            return true
        } catch (e: RetryLaterException) {
            SyncAccount.setProblem(app, SyncAccount.Problem.Server)
            return true
        } catch (e: BundleException) {
            SyncAccount.setProblem(app, SyncAccount.Problem.Storage)
            return false
        } catch (e: IOException) {
            SyncAccount.setProblem(app, SyncAccount.Problem.Network)
            return true
        } catch (e: Exception) {
            // A record this version can't make sense of, say; the next sync
            // tries again, and the rest of the app carries on.
            SyncAccount.setProblem(app, SyncAccount.Problem.Server)
            return false
        } finally {
            lock.unlock()
            SyncEvents.post(SyncEvents.Change.Status)
        }
    }

    fun runInBackground(context: Context) {
        val app = context.applicationContext
        SyncEvents.post(SyncEvents.Change.Status)
        Thread({ run(app) }, "omareader-sync").start()
    }

    fun isRunning() = lock.isLocked
}

class SyncJob : JobService() {
    @Volatile private var cancelled = false

    override fun onStartJob(params: JobParameters): Boolean {
        cancelled = false
        Thread({
            val again = SyncRunner.run(this) { cancelled }
            jobFinished(params, again)
        }, "omareader-sync-job").start()
        return true
    }

    // The system wants the job stopped (the network went, or it ran too
    // long); a download picks up where it stopped next time.
    override fun onStopJob(params: JobParameters): Boolean {
        cancelled = true
        return true
    }
}

// When syncs happen: when the app opens, a few seconds after something that
// syncs changes, every 15 minutes, and, for bundles waiting for Wi-Fi, once
// the phone is on an unmetered network.
object SyncScheduler {
    const val AFTER_CHANGE_MS = 5_000L
    private const val PERIODIC = 1
    private const val SOON = 2
    private const val UNMETERED = 3
    private const val PERIOD_MS = 15 * 60 * 1000L

    private fun scheduler(context: Context) = context.getSystemService(JobScheduler::class.java)
    private fun job(context: Context, id: Int) = JobInfo.Builder(id, ComponentName(context, SyncJob::class.java))

    // A change made within the delay replaces the job, so several changes in
    // a row make one sync.
    fun soon(context: Context, delayMs: Long) {
        if (!SyncAccount.connected(context)) return
        scheduler(context)?.schedule(
            job(context, SOON)
                .setMinimumLatency(delayMs)
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                .setBackoffCriteria(60_000, JobInfo.BACKOFF_POLICY_EXPONENTIAL)
                .build(),
        )
    }

    // Started whenever the app opens, since the job is not kept over a
    // restart of the phone (that would need another permission).
    fun start(context: Context) {
        if (!SyncAccount.connected(context)) return
        val scheduler = scheduler(context) ?: return
        if (scheduler.getPendingJob(PERIODIC) == null) {
            scheduler.schedule(
                job(context, PERIODIC)
                    .setPeriodic(PERIOD_MS)
                    .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                    .build(),
            )
        }
        soon(context, 0)
    }

    fun whenUnmetered(context: Context) {
        scheduler(context)?.schedule(
            job(context, UNMETERED)
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_UNMETERED)
                .setBackoffCriteria(60_000, JobInfo.BACKOFF_POLICY_EXPONENTIAL)
                .build(),
        )
    }

    fun stop(context: Context) {
        scheduler(context)?.run {
            cancel(PERIODIC)
            cancel(SOON)
            cancel(UNMETERED)
        }
    }
}
