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
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import org.json.JSONObject

/**
 * Foreground service for the RTL-TCP SERVER (v6.1). Keeps the app alive + the
 * CPU awake while the phone serves its USB dongle over the network, and shows a
 * live notification (bandwidth + connected client). Uses the `connectedDevice`
 * FGS type (there's no audio — the mediaPlayback service is for on-device
 * listening), a PARTIAL_WAKE_LOCK so screen-off doze can't stall the USB/TCP
 * stream, and a WifiLock so the radio doesn't power-save mid-stream.
 *
 * The notification text is refreshed on a 2s timer from the native server
 * status, so it stays current even when the JS/UI is backgrounded.
 */
class RtlTcpServerService : Service() {

    companion object {
        const val EXTRA_NAME = "name"
        const val EXTRA_IP   = "ip"
        const val EXTRA_PORT = "port"
        const val EXTRA_MODE = "mode"          // "rtltcp" (default) | "vibeserver"
        /** ★ Started to REBUILD a server the app update took down — see VibeUpdateReceiver. The
         *  crash path signals the same thing with a null intent; this one cannot, because it is
         *  started deliberately and therefore always has one. */
        const val EXTRA_RESTORE = "restore"
        private const val CHANNEL_ID = "vibesdr_rtltcp_server"
        private const val NOTIF_ID = 4711
        private const val TAG = "RtlTcpServerService"

        fun start(ctx: Context, name: String, ip: String, port: Int, mode: String = "rtltcp") {
            val i = Intent(ctx, RtlTcpServerService::class.java)
                .putExtra(EXTRA_NAME, name).putExtra(EXTRA_IP, ip)
                .putExtra(EXTRA_PORT, port).putExtra(EXTRA_MODE, mode)
            ContextCompat_startForegroundService(ctx, i)
        }

        fun stop(ctx: Context) {
            ctx.stopService(Intent(ctx, RtlTcpServerService::class.java))
        }

        private fun ContextCompat_startForegroundService(ctx: Context, i: Intent) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(i)
            else ctx.startService(i)
        }
    }

    private var wakeLock: PowerManager.WakeLock? = null
    private val wifiLock by lazy { VibeWifiLock(this, "VibeSDR:RtlTcpServer") }
    private val handler = Handler(Looper.getMainLooper())
    private var name = "VibeSDR RTL-SDR"
    private var ip = ""
    private var port = 1234
    private var mode = "rtltcp"

    private val ticker = object : Runnable {
        override fun run() {
            updateNotification()
            handler.postDelayed(this, 2000)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // A NULL intent means Android RECREATED us after the process died (START_STICKY),
        // rather than someone starting us. The service is back but the native shim went
        // down with the old process — so rebuild it, or we'd sit here showing a
        // "server running" notification with no radio behind it.
        //
        // Don't test `mode` here: this is a FRESH process, so the field is back to its
        // default and would never match. VibeServerRestore's armed flag is the only
        // thing that survived, and it is the actual source of truth.
        if (intent == null || intent.getBooleanExtra(EXTRA_RESTORE, false)) {
            Thread {
                val err = VibeServerRestore.restore(applicationContext)
                if (err != null) Log.w(TAG, "could not rebuild VibeServer: $err")
            }.start()
        }
        intent?.let {
            name = it.getStringExtra(EXTRA_NAME) ?: name
            ip   = it.getStringExtra(EXTRA_IP) ?: ip
            port = it.getIntExtra(EXTRA_PORT, port)
            mode = it.getStringExtra(EXTRA_MODE) ?: mode
        }
        ensureChannel()
        startForegroundInternal()
        acquireWakeLock()
        wifiLock.acquire()
        handler.removeCallbacks(ticker)
        handler.post(ticker)
        return START_STICKY
    }

    private fun startForegroundInternal() {
        val notif = buildNotification("Starting…")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            ServiceCompat.startForeground(
                this, NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(NOTIF_ID, notif)
        }
    }

    private fun acquireWakeLock() {
        if (wakeLock != null) return
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "VibeSDR:RtlTcpServer").apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    private fun releaseWakeLock() {
        try { wakeLock?.let { if (it.isHeld) it.release() } } catch (_: Throwable) {}
        wakeLock = null
    }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (nm.getNotificationChannel(CHANNEL_ID) == null) {
            val ch = NotificationChannel(
                CHANNEL_ID, "RTL-TCP server", NotificationManager.IMPORTANCE_LOW)
            ch.setShowBadge(false)
            nm.createNotificationChannel(ch)
        }
    }

    private fun bandwidthLabel(sampleRate: Long, overrideRate: Long): String {
        if (sampleRate <= 0) return "—"
        val mhz = sampleRate / 1_000_000.0
        val clean = String.format("%.3f", mhz).trimEnd('0').trimEnd('.') + " MHz"
        return if (overrideRate > 0) "$clean (capped)" else clean
    }

    private fun rate(bytesPerSec: Long): String {
        if (bytesPerSec <= 0) return "0"
        val kb = bytesPerSec / 1024.0
        return if (kb >= 1000) String.format("%.1f MB/s", kb / 1024.0)
               else String.format("%.0f KB/s", kb)
    }

    private fun statusText(): String {
        if (mode == "vibeserver") {
            return try {
                val j = JSONObject(VibeLocalSDR.getVibeServerStatus())
                val client = j.optBoolean("client", false)
                val addr = j.optString("clientAddr", "")
                val spec = j.optLong("specBytesPerSec", 0)
                val aud = j.optLong("audioBytesPerSec", 0)
                if (client) {
                    val who = if (addr.isNotEmpty()) addr else "client"
                    "$ip:$port · $who · spec ${rate(spec)} · audio ${rate(aud)}"
                } else "$ip:$port · waiting for client"
            } catch (_: Throwable) { "$ip:$port" }
        }
        return try {
            val j = JSONObject(VibeLocalSDR.getServerStatus())
            val sr = j.optLong("sampleRate", 0)
            val ov = j.optLong("overrideRate", 0)
            val client = j.optBoolean("client", false)
            val addr = j.optString("clientAddr", "")
            val bw = bandwidthLabel(sr, ov)
            val who = if (client) (if (addr.isNotEmpty()) "client $addr" else "client connected") else "waiting for client"
            "$ip:$port · $bw · $who"
        } catch (_: Throwable) {
            "$ip:$port"
        }
    }

    private fun buildNotification(text: String): Notification {
        val launch = packageManager.getLaunchIntentForPackage(packageName)
        val pi = PendingIntent.getActivity(
            this, 0, launch,
            (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) PendingIntent.FLAG_IMMUTABLE else 0)
                or PendingIntent.FLAG_UPDATE_CURRENT)
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("VibeSDR — Sharing $name")
            .setContentText(text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(text))
            .setSmallIcon(R.mipmap.ic_launcher)
            .setOngoing(true)
            .setContentIntent(pi)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }

    private fun updateNotification() {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.notify(NOTIF_ID, buildNotification(statusText()))
    }

    override fun onDestroy() {
        handler.removeCallbacks(ticker)
        releaseWakeLock()
        wifiLock.release()
        super.onDestroy()
    }
}
