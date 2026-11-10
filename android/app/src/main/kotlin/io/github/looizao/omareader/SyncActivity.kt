package io.github.looizao.omareader

import android.app.AlertDialog
import android.os.Bundle
import android.text.format.DateUtils
import android.view.View
import android.widget.Button
import android.widget.Switch
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.result.IntentSenderRequest
import androidx.activity.result.contract.ActivityResultContracts

// Connecting Dropbox or Google Drive and seeing how sync is going. Also
// where the page the browser lands on after Dropbox's consent sends the user
// back to. A ComponentActivity, which comes with Play services anyway, so
// Google's consent sheet can answer through the Activity Result API.
class SyncActivity : ComponentActivity() {
    private lateinit var heading: TextView
    private lateinit var status: TextView
    private lateinit var detail: TextView
    private lateinit var problem: TextView
    private lateinit var connect: Button
    private lateinit var connectDrive: Button
    private lateinit var syncNow: Button
    private lateinit var disconnect: Button
    private lateinit var mobileData: Switch
    private lateinit var explain: TextView
    private lateinit var busy: View

    private val listener: (SyncEvents.Change, String?) -> Unit = { change, _ ->
        if (change == SyncEvents.Change.Status) show()
    }

    private val driveSheet = registerForActivityResult(ActivityResultContracts.StartIntentSenderForResult()) { result ->
        SyncAccount.driveAnswered(this, result.resultCode == RESULT_OK, result.data)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_sync)
        heading = findViewById(R.id.title)
        status = findViewById(R.id.status)
        detail = findViewById(R.id.detail)
        problem = findViewById(R.id.problem)
        connect = findViewById(R.id.connect)
        connectDrive = findViewById(R.id.connect_drive)
        syncNow = findViewById(R.id.sync_now)
        disconnect = findViewById(R.id.disconnect)
        mobileData = findViewById(R.id.mobile_data)
        explain = findViewById(R.id.explain)
        busy = findViewById(R.id.busy)

        findViewById<View>(R.id.back).setOnClickListener { finish() }
        connect.setOnClickListener { SyncAccount.connectDropbox(this) }
        connectDrive.setOnClickListener {
            SyncAccount.connectDrive(this) { sheet ->
                driveSheet.launch(IntentSenderRequest.Builder(sheet.intentSender).build())
            }
        }
        syncNow.setOnClickListener { SyncRunner.runInBackground(this) }
        disconnect.setOnClickListener { confirmDisconnect() }
        mobileData.setOnCheckedChangeListener { _, checked ->
            if (checked != SyncAccount.mobileData(this)) SyncAccount.setMobileData(this, checked)
        }
    }

    override fun onStart() {
        super.onStart()
        SyncEvents.listen(listener)
        show()
    }

    override fun onStop() {
        SyncEvents.stop(listener)
        super.onStop()
    }

    private fun name(provider: SyncAccount.Provider) = getString(
        when (provider) {
            SyncAccount.Provider.Dropbox -> R.string.provider_dropbox
            SyncAccount.Provider.Drive -> R.string.provider_drive
        },
    )

    private fun show() {
        val provider = SyncAccount.provider(this)
        val connected = provider != null
        val connecting = SyncAccount.connectingTo()
        val running = SyncRunner.isRunning()
        val account = SyncAccount.account(this)
        heading.text = if (provider != null) getString(R.string.sync_title_provider, name(provider)) else getString(R.string.sync_title)
        status.text = when {
            !SyncAccount.available -> getString(R.string.sync_unavailable)
            connected && account.isNotEmpty() -> getString(R.string.sync_connected_as, account)
            provider != null -> getString(R.string.sync_connected, name(provider))
            connecting == SyncAccount.Provider.Dropbox -> getString(R.string.sync_connecting)
            connecting == SyncAccount.Provider.Drive -> getString(R.string.sync_connecting_drive)
            else -> getString(R.string.sync_not_connected)
        }
        val last = SyncAccount.lastSync(this)
        detail.text = when {
            !connected -> getString(R.string.sync_what)
            running -> getString(R.string.sync_running)
            last > 0 -> getString(
                R.string.sync_last,
                DateUtils.getRelativeTimeSpanString(last, System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS),
            )
            else -> getString(R.string.sync_never)
        }
        val trouble = SyncAccount.problem(this)
        val troubled = name(SyncAccount.problemProvider(this))
        problem.visibility = if (trouble != null && connecting == null) View.VISIBLE else View.GONE
        problem.text = when (trouble) {
            SyncAccount.Problem.Network -> getString(R.string.sync_problem_network, troubled)
            SyncAccount.Problem.Revoked -> getString(R.string.sync_problem_revoked, troubled)
            SyncAccount.Problem.Server -> getString(R.string.sync_problem_server, troubled)
            SyncAccount.Problem.Storage -> getString(R.string.sync_problem_storage, troubled)
            SyncAccount.Problem.NoPlayServices -> getString(R.string.sync_problem_play_services)
            null -> ""
        }
        // Connected, the text describes that provider; otherwise the ones
        // this build offers.
        explain.setText(
            when {
                provider == SyncAccount.Provider.Drive -> R.string.sync_explain_drive
                provider == SyncAccount.Provider.Dropbox -> R.string.sync_explain
                SyncAccount.dropboxAvailable && SyncAccount.driveAvailable -> R.string.sync_explain_choose
                SyncAccount.driveAvailable -> R.string.sync_explain_drive
                else -> R.string.sync_explain
            },
        )
        busy.visibility = if (running || connecting != null) View.VISIBLE else View.GONE
        connect.visibility = if (!connected && SyncAccount.dropboxAvailable) View.VISIBLE else View.GONE
        connect.isEnabled = connecting == null
        connectDrive.visibility = if (!connected && SyncAccount.driveAvailable) View.VISIBLE else View.GONE
        connectDrive.isEnabled = connecting == null
        syncNow.visibility = if (connected) View.VISIBLE else View.GONE
        syncNow.isEnabled = !running
        mobileData.visibility = if (connected) View.VISIBLE else View.GONE
        mobileData.isChecked = SyncAccount.mobileData(this)
        disconnect.visibility = if (connected) View.VISIBLE else View.GONE
    }

    private fun confirmDisconnect() {
        val provider = SyncAccount.provider(this) ?: return
        AlertDialog.Builder(this)
            .setTitle(getString(R.string.sync_disconnect_title, name(provider)))
            .setMessage(getString(R.string.sync_disconnect_message, name(provider)))
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(R.string.sync_disconnect) { _, _ -> SyncAccount.disconnect(this) }
            .show()
    }
}
