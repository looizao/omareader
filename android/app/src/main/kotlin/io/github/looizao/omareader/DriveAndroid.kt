package io.github.looizao.omareader

import android.accounts.Account
import android.content.Context
import com.google.android.gms.auth.api.identity.AuthorizationClient
import com.google.android.gms.auth.api.identity.AuthorizationRequest
import com.google.android.gms.auth.api.identity.ClearTokenRequest
import com.google.android.gms.auth.api.identity.Identity
import com.google.android.gms.auth.api.identity.RevokeAccessRequest
import com.google.android.gms.common.ConnectionResult
import com.google.android.gms.common.GoogleApiAvailability
import com.google.android.gms.common.api.ApiException
import com.google.android.gms.common.api.CommonStatusCodes
import com.google.android.gms.common.api.Scope
import com.google.android.gms.tasks.Tasks
import java.io.IOException
import java.util.concurrent.ExecutionException
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException

// Google Drive's sign-in on the phone, through Play services (docs/sync.md,
// "Connecting to Drive"). Play services keeps the grant; the phone keeps no
// token of its own, only the account's address to ask for the same account
// again.
object DriveAuth {
    const val SCOPE = "https://www.googleapis.com/auth/drive.file"
    private const val GOOGLE = "com.google"

    fun client(context: Context): AuthorizationClient = Identity.getAuthorizationClient(context)

    // Without an account, Play services lets the user pick one; with the
    // connected one, a phone with several accounts never asks which.
    fun request(email: String?): AuthorizationRequest = AuthorizationRequest.builder()
        .setRequestedScopes(listOf(Scope(SCOPE)))
        .apply { if (!email.isNullOrEmpty()) setAccount(Account(email, GOOGLE)) }
        .build()

    fun playServices(context: Context) =
        GoogleApiAvailability.getInstance().isGooglePlayServicesAvailable(context) == ConnectionResult.SUCCESS

    // Ends the grant on Google's side, so a phone that is lost or sold no
    // longer reaches the files. Best effort, like Dropbox's revoke.
    fun revoke(context: Context, email: String) {
        runCatching {
            client(context).revokeAccess(
                RevokeAccessRequest.builder()
                    .setAccount(Account(email, GOOGLE))
                    .setScopes(listOf(Scope(SCOPE)))
                    .build(),
            )
        }
    }
}

// Access tokens for Drive, asked of Play services when a sync starts and
// kept in memory only. When Play services needs the user again (the grant
// was revoked), a background sync can't show its sheet, so it stops as
// Dropbox's does when its token is revoked and the sync screen asks the
// user to connect again.
class PlayTokens(context: Context, private val email: String?, private var token: String? = null) : AccessTokens {
    private val app = context.applicationContext

    @Synchronized
    override fun current(): String = token ?: fetch()

    // Drive refused the token: Play services hands out its cached token
    // until told to forget it.
    @Synchronized
    override fun refresh(): String {
        token?.let { old -> runCatching { await(DriveAuth.client(app).clearToken(ClearTokenRequest.builder().setToken(old).build())) } }
        token = null
        return fetch()
    }

    private fun fetch(): String {
        val result = await(DriveAuth.client(app).authorize(DriveAuth.request(email)))
        if (result.hasResolution()) throw RevokedException()
        return (result.accessToken ?: throw IOException("Play services gave no access token")).also { token = it }
    }

    private fun <T> await(task: com.google.android.gms.tasks.Task<T>): T = try {
        Tasks.await(task, 30, TimeUnit.SECONDS)
    } catch (e: TimeoutException) {
        throw IOException("Play services did not answer")
    } catch (e: ExecutionException) {
        val status = (e.cause as? ApiException)?.statusCode
        // Without a network Play services can't reach Google; anything else
        // (the account was removed, say) needs the user.
        if (status == null || status in TRANSIENT) throw IOException("Play services could not authorize Drive")
        throw RevokedException()
    }

    companion object {
        private val TRANSIENT = setOf(
            CommonStatusCodes.NETWORK_ERROR,
            CommonStatusCodes.TIMEOUT,
            CommonStatusCodes.INTERNAL_ERROR,
            CommonStatusCodes.API_NOT_CONNECTED,
            CommonStatusCodes.INTERRUPTED,
            CommonStatusCodes.CONNECTION_SUSPENDED_DURING_CALL,
            CommonStatusCodes.RECONNECTION_TIMED_OUT,
            CommonStatusCodes.RECONNECTION_TIMED_OUT_DURING_UPDATE,
        )
    }
}
