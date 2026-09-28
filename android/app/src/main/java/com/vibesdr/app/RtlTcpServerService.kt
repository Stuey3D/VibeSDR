package com.vibesdr.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
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
            pollUsbRecovery()
            handler.postDelayed(this, 2000)
        }
    }

    /** ★★★ A RE-ENUMERATED DONGLE NEEDS A FRESH FD, AND ONLY JAVA CAN GET ONE — see
     *  VibeServerRestore.recoverUsbIfNeeded. Polled on the 2 s tick while the service runs; off the main
     *  thread (openDevice is a binder call) and never two at once. */
    @Volatile private var usbPollBusy = false
    private var lastUsbState: String? = null
    private fun pollUsbRecovery() {
        if (usbPollBusy) return
        usbPollBusy = true
        Thread {
            try {
                val st = VibeServerRestore.recoverUsbIfNeeded(applicationContext)
                if (st != null && st != lastUsbState) Log.i(TAG, "USB recovery: $st")
                lastUsbState = st
                /* ★★★ GONE LONGER THAN A BLIP — STOP, as the owner's Stop button would (Stuart, 2026-09-29;
                 *  see VibeServerRestore.RADIO_BLIP_WINDOW_MS). The engine would otherwise wait for its
                 *  dongle for ever and serve again the moment it was replugged, however much later. */
                if (st == VibeServerRestore.RADIO_GONE_TOO_LONG) {
                    VibeServerRestore.stopBecauseRadioGone(applicationContext)
                    handler.post { stopSelf() }
                }
            } catch (t: Throwable) {
                Log.w(TAG, "USB recovery failed: $t")
            } finally { usbPollBusy = false }
        }.start()
    }

    /** ★★ THE MOMENT THE SERVED RADIO LEAVES, stamped for the blip rule — see
     *  VibeServerRestore.noteRadioGone. A runtime receiver, so it exists exactly while a server does;
     *  USB_DEVICE_DETACHED is a system broadcast, so no export flag is needed. The engine's own
     *  dead-handle report stamps it too (recoverUsbIfNeeded), for a departure this misses. */
    private var detachReceiver: BroadcastReceiver? = null
    private fun watchForDetach() {
        if (detachReceiver != null) return
        val r = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) {
                if (i.action != UsbManager.ACTION_USB_DEVICE_DETACHED) return
                @Suppress("DEPRECATION")
                val dev = i.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE) ?: return
                if (!VibeLocalSdrModule.isServableRadio(dev.vendorId, dev.productId)) return
                if (!VibeServerRestore.isShimServing()) return
                VibeServerRestore.noteRadioGone(applicationContext, "USB detach of ${dev.deviceName}")
            }
        }
        try {
            registerReceiver(r, IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED))
            detachReceiver = r
        } catch (t: Throwable) { Log.w(TAG, "cannot watch for the radio detaching: $t") }
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
                /* ★★ RETRY A MISSING DONGLE FOR A WHILE, don't give up on the first look. Straight after a
                 *  package update the USB service can list NO devices for the new package for a moment: on the
                 *  Sony TV the restore ran 0.2 s after MY_PACKAGE_REPLACED, said "no SDR attached" with the
                 *  V4 plugged in and its grant intact, and the server stayed down until somebody walked to the
                 *  TV (2026-09-19). The same path had worked at 14:51 — timing, not the dongle. Every other
                 *  reason is final and reported at once. */
                var err = VibeServerRestore.restore(applicationContext)
                var tries = 0
                /* ★ 30 tries x 2 s: after an update the USB service may not list the device yet, and on an
                 *   ATTACH (MainActivity.resumeServerIfWanted) the default-association grant lands a moment
                 *   after the activity is launched.
                 * ★★★ NOT A BOOT PATH ANY MORE. This loop was widened for start-on-boot, in the hope that the
                 *     grant would arrive if we waited. It does not: on the Sony (2026-09-28) the boot restore
                 *     waited the full minute and ended "no USB permission", because Android never grants a
                 *     device that was present at boot. The switch and VibeBootReceiver are gone. */
                while ((err == "no SDR attached" || err == "no USB permission") && tries < 30) {
                    Thread.sleep(2000); tries++
                    err = VibeServerRestore.restore(applicationContext)
                }
                if (err != null) {
                    Log.w(TAG, "could not rebuild VibeServer: $err")
                    /* ★★★ AND SAY IT WHERE SOMEBODY CAN SEE IT. On a TV box nobody reads logcat, and a server
                     *  that silently never came back after a power cut looks exactly like a broken app. The
                     *  notification is the only surface this thing has when it is running headless. */
                    restoreFailure = when (err) {
                        "no USB permission" ->
                            "Waiting for USB permission — open VibeServer Lite once and allow the radio"
                        "no SDR attached" -> "No radio found — check the dongle is plugged in"
                        "no stored config" -> "Nothing saved to restore — start the server once from the app"
                        VibeServerRestore.RADIO_GONE_TOO_LONG ->
                            "The radio was unplugged for more than 5 minutes, so the server was not restarted — open the app and press Start"
                        else -> "Could not restart the server: $err"
                    }
                    handler.post { updateNotification() }
                } else {
                    restoreFailure = null
                    if (tries > 0) Log.i(TAG, "VibeServer rebuilt after waiting ${tries * 2} s")
                }
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
        watchForDetach()
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

    /** ★ Why an unattended restart did not happen, shown in the notification until it does. */
    @Volatile private var restoreFailure: String? = null

    private fun statusText(): String {
        restoreFailure?.let { return it }
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
        detachReceiver?.let { r ->
            try { unregisterReceiver(r) } catch (t: Throwable) { Log.w(TAG, "unregistering the detach watch: $t") }
        }
        detachReceiver = null
        releaseWakeLock()
        wifiLock.release()
        super.onDestroy()
    }
}
