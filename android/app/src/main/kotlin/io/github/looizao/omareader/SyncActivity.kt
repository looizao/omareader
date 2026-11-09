package io.github.looizao.omareader

import android.app.Activity
import android.app.AlertDialog
import android.os.Bundle
import android.text.format.DateUtils
import android.view.View
import android.widget.Button
import android.widget.Switch
import android.widget.TextView

// Connecting Dropbox and seeing how sync is going. Also where the page the
// browser lands on after Dropbox's consent sends the user back to.
class SyncActivity : Activity() {
    private lateinit var status: TextView
    private lateinit var detail: TextView
    private lateinit var problem: TextView
    private lateinit var connect: Button
    private lateinit var syncNow: Button
    private lateinit var disconnect: Button
    private lateinit var mobileData: Switch
    private lateinit var busy: View

    private val listener: (SyncEvents.Change, String?) -> Unit = { change, _ ->
        if (change == SyncEvents.Change.Status) show()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_sync)
        status = findViewById(R.id.status)
        detail = findViewById(R.id.detail)
        problem = findViewById(R.id.problem)
        connect = findViewById(R.id.connect)
        syncNow = findViewById(R.id.sync_now)
        disconnect = findViewById(R.id.disconnect)
        mobileData = findViewById(R.id.mobile_data)
        busy = findViewById(R.id.busy)

        findViewById<View>(R.id.back).setOnClickListener { finish() }
        connect.setOnClickListener { SyncAccount.connect(this) }
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

    private fun show() {
        val connected = SyncAccount.connected(this)
        val connecting = SyncAccount.isConnecting()
        val running = SyncRunner.isRunning()
        val account = SyncAccount.account(this)
        status.text = when {
            !SyncAccount.available -> getString(R.string.sync_unavailable)
            connected && account.isNotEmpty() -> getString(R.string.sync_connected_as, account)
            connected -> getString(R.string.sync_connected)
            connecting -> getString(R.string.sync_connecting)
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
        problem.visibility = if (trouble != null && !connecting) View.VISIBLE else View.GONE
        problem.text = when (trouble) {
            SyncAccount.Problem.Network -> getString(R.string.sync_problem_network)
            SyncAccount.Problem.Revoked -> getString(R.string.sync_problem_revoked)
            SyncAccount.Problem.Server -> getString(R.string.sync_problem_server)
            SyncAccount.Problem.Storage -> getString(R.string.sync_problem_storage)
            null -> ""
        }
        busy.visibility = if (running || connecting) View.VISIBLE else View.GONE
        connect.visibility = if (!connected && SyncAccount.available) View.VISIBLE else View.GONE
        connect.isEnabled = !connecting
        syncNow.visibility = if (connected) View.VISIBLE else View.GONE
        syncNow.isEnabled = !running
        mobileData.visibility = if (connected) View.VISIBLE else View.GONE
        mobileData.isChecked = SyncAccount.mobileData(this)
        disconnect.visibility = if (connected) View.VISIBLE else View.GONE
    }

    private fun confirmDisconnect() {
        AlertDialog.Builder(this)
            .setTitle(R.string.sync_disconnect_title)
            .setMessage(R.string.sync_disconnect_message)
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(R.string.sync_disconnect) { _, _ -> SyncAccount.disconnect(this) }
            .show()
    }
}
