package com.vibesdr.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat

/**
 * FULL MODE's front door, in its own process (`:frontdoor`).
 *
 * ★★★ A SERVICE, NOT A FORKED BINARY. The first plan was to ship the `vibeserver` executable as
 *     `libvibeserver.so` in nativeLibraryDir (since API 29 an app may not exec from its data dir)
 *     and fork/exec it. That works, and it buys three problems for nothing: packaging an
 *     executable Gradle does not package by default, W^X rules to stay the right side of, and —
 *     the one that matters — a child process **Android does not track**, so the low-memory killer
 *     treats it as free real estate and nothing restarts it. `VibeServerRestore.kt` exists
 *     precisely because process death is routine here.
 *     ▶ `android:process=":frontdoor"` gives a real Android component instead: foregroundable,
 *       tracked, restartable, START_STICKY. Same isolation, none of the packaging.
 *
 * ★★★ IT OWNS NO RADIO, AND THAT IS THE WHOLE POINT. It binds the public port, serves the landing
 *     page, the radio list, setup and admin, and hands anything addressed to `/r/<id>/…` over to
 *     the MAIN process, which is where the USB descriptor lives and always stays. So this process
 *     needs no USB permission, no libusb and no descriptor passed to it — which is what makes Full
 *     mode possible on Android at all (see VibeLocalSDR.kt).
 *
 * ★★ It runs in a SEPARATE PROCESS, so it gets its own copy of every static in libvibelocalsdr.so.
 *    That is deliberate: one process holding a radio and a front door in one address space would
 *    share the shim's globals, and the door would answer as the radio.
 */
class FrontDoorService : Service() {

    companion object {
        const val EXTRA_PORT   = "port"
        const val EXTRA_DIR    = "dir"        // where the hand-off sockets live
        const val EXTRA_SERIAL = "serial"
        const val EXTRA_ID     = "id"
        private const val CHANNEL_ID = "vibesdr_frontdoor"
        private const val NOTIF_ID = 4713     // distinct from the radio's own notification
        private const val TAG = "FrontDoorService"

        fun start(ctx: Context, port: Int, dir: String, serial: String, id: String) {
            val i = Intent(ctx, FrontDoorService::class.java)
                .putExtra(EXTRA_PORT, port).putExtra(EXTRA_DIR, dir)
                .putExtra(EXTRA_SERIAL, serial).putExtra(EXTRA_ID, id)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(i)
            else ctx.startService(i)
        }

        fun stop(ctx: Context) {
            ctx.stopService(Intent(ctx, FrontDoorService::class.java))
        }
    }

    private var wakeLock: PowerManager.WakeLock? = null
    private var port = 0

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val want   = intent?.getIntExtra(EXTRA_PORT, 0) ?: 0
        val dir    = intent?.getStringExtra(EXTRA_DIR) ?: ""
        val serial = intent?.getStringExtra(EXTRA_SERIAL) ?: ""
        val id     = intent?.getStringExtra(EXTRA_ID) ?: ""

        // ★ Foreground FIRST. A service that binds a port and only then posts its notification is
        //   one the system may kill in between, and the failure looks like "the server never
        //   started" rather than like a lifecycle problem.
        startForegroundCompat(if (port > 0) port else want)

        if (port == 0 && want > 0) {
            // ★★ THE ROUTE BEFORE THE DOOR. setHandoffRoute only stores a mapping, but a request
            //    can arrive on the very first accept, and a door that is up with no route answers
            //    404 for the radio — which reads as "this server has no receivers".
            if (dir.isNotEmpty() && serial.isNotEmpty()) {
                VibeLocalSDR.setHandoffRoute(dir, serial, id)
            }
            port = VibeLocalSDR.startFrontDoor(want)
            if (port < 0) {
                Log.e(TAG, "front door failed to bind $want")
                stopSelf()
                return START_NOT_STICKY
            }
            Log.i(TAG, "front door on $port -> /r/$id")
            startForegroundCompat(port)
            acquireWakeLock()
        }
        // ★★ START_STICKY: if Android kills this process the door must come back, because the
        //    radio in the main process is still there and still reachable — a dead door makes a
        //    working receiver look offline to everyone outside.
        return START_STICKY
    }

    override fun onDestroy() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        super.onDestroy()
    }

    private fun acquireWakeLock() {
        try {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "VibeSDR:frontdoor").apply {
                setReferenceCounted(false)
                acquire()
            }
        } catch (e: Exception) {
            Log.w(TAG, "no wake lock: ${e.message}")
        }
    }

    private fun startForegroundCompat(shownPort: Int) {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(CHANNEL_ID, "VibeServer front door",
                                         NotificationManager.IMPORTANCE_LOW)
            ch.setShowBadge(false)
            nm.createNotificationChannel(ch)
        }
        val tap = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val n: Notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("VibeServer — front door")
            .setContentText(if (shownPort > 0) "Listening on port $shownPort" else "Starting…")
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .setOngoing(true)
            .setContentIntent(tap)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
        // ★ `dataSync`, not `connectedDevice`: this process is deliberately the one with NO device.
        ServiceCompat.startForeground(this, NOTIF_ID, n,
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC else 0)
    }
}
