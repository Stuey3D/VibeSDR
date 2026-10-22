package com.vibesdr.app

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Handler
import android.util.Log
import java.io.File
import java.security.MessageDigest

/**
 * ★★★ THE SDRplay RSP ON ANDROID — OpenRSP's driver with its service EMBEDDED in this process
 * (docs/OPENRSP-BUNDLE-PLAN.md §5.3, 2026-10-22). Shared by the main app and VibeServer Lite.
 *
 * What only Java can do, and so lives here:
 *  1. FIRMWARE. The RSP's USB bridge boots a ROM loader at every power-up and the driver uploads OpenRSP's firmware
 *     each time. The images ship as assets; they are copied to filesDir/openrsp/firmware/own and EVERY image is
 *     checked against manifest.sha256 before the driver is told where they are. A mismatch is no RSP, never a
 *     guess — this is the one component that talks to the receiver's bridge directly.
 *  2. THE DEVICES. An app may not enumerate USB, so each RSP's UsbManager fd is handed to the service
 *     (VibeLocalSDR.rspAttach). ★★ THE RSP RE-ENUMERATES AFTER ITS FIRMWARE UPLOAD: it leaves the bus and comes
 *     back as a NEW UsbDevice with a new fd, while the driver waits up to 15 s for it inside the first GetDevices.
 *     So while an RSP session is live this watches USB_DEVICE_ATTACHED / DETACHED itself, attaches the returning
 *     radio (asking for permission if Android has not already granted it through the "use by default" association)
 *     and detaches one that leaves. ✗ UNMEASURED: whether Android asks a SECOND time after the re-enumeration on a
 *     phone where the owner did not tick "use by default" — nobody here has an RSP on Android yet.
 *  ★★ API 26 ONLY. The service needs ASharedMemory_create (Android 8.0). Below that isRsp() is false everywhere, the
 *     device filter (res/xml-v26) does not offer the app, and nothing here runs: an RSP is simply not a radio on
 *     that phone — "a control that only works on one radio should not be there" applies to an OS version too.
 *  ★ Idle cost: nothing at all until an RSP is attached — no library load, no thread, no receiver.
 */
object VibeRsp {
    private const val TAG = "VibeRsp"
    const val RSP_VID = 0x1df7
    /** ★ Mirrors res/xml-v26/device_filter.xml (DECIMAL there) — scripts/check-device-filter.mjs compares them. */
    val RSP_PIDS = intArrayOf(0x2500, 0x3000, 0x3010, 0x3020, 0x3030, 0x3050, 0x3060)
    private const val ACTION_PERMISSION = "com.vibesdr.app.RSP_USB_PERMISSION"

    val supported: Boolean get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O   // API 26

    fun isRsp(vid: Int, pid: Int): Boolean = supported && vid == RSP_VID && RSP_PIDS.contains(pid)
    fun isRsp(dev: UsbDevice): Boolean = isRsp(dev.vendorId, dev.productId)

    /** The model as SDRplay print it on the case, from the USB product id — before anything is opened. */
    fun modelName(pid: Int): String = when (pid) {
        0x2500 -> "SDRplay RSP1"
        0x3000 -> "SDRplay RSP1A"
        0x3010 -> "SDRplay RSP2"
        0x3020 -> "SDRplay RSPduo"
        0x3030 -> "SDRplay RSPdx"
        0x3050 -> "SDRplay RSP1B"
        0x3060 -> "SDRplay RSPdx-R2"
        else   -> "SDRplay RSP"
    }

    private val conns = HashMap<String, UsbDeviceConnection>()   // deviceName → the connection whose fd we gave
    private var receiver: BroadcastReceiver? = null
    private var watchThread: android.os.HandlerThread? = null
    @Volatile private var fwReady: String? = null

    /**
     * Copy the firmware set out of the APK (once per install) and check it. Returns the directory the driver wants
     * (the one HOLDING own/), or null if any image is missing or does not match the manifest.
     */
    @Synchronized
    fun prepareFirmware(ctx: Context): String? {
        fwReady?.let { return it }
        val base = File(ctx.filesDir, "openrsp/firmware")
        val own = File(base, "own").apply { mkdirs() }
        val names = listOf("manifest.sha256", "fw_common.bin", "fw_rsp1.bin", "fw_rspduo.bin")
        try {
            for (n in names) {
                ctx.assets.open("openrsp/firmware/own/$n").use { inp ->
                    val bytes = inp.readBytes()
                    val f = File(own, n)
                    if (!f.exists() || !f.readBytes().contentEquals(bytes)) f.writeBytes(bytes)
                }
            }
            val manifest = File(own, "manifest.sha256").readLines()
                .mapNotNull { l -> l.trim().split(Regex("\\s+")).takeIf { it.size == 2 }?.let { it[1] to it[0].lowercase() } }
                .toMap()
            for (n in names.drop(1)) {
                val want = manifest[n] ?: run { Log.e(TAG, "firmware: $n is not in the manifest"); return null }
                val got = MessageDigest.getInstance("SHA-256").digest(File(own, n).readBytes())
                    .joinToString("") { "%02x".format(it) }
                if (got != want) { Log.e(TAG, "firmware: $n is $got, the manifest says $want — refusing"); return null }
            }
        } catch (t: Throwable) {
            Log.e(TAG, "firmware could not be prepared: $t")
            return null
        }
        VibeLocalSDR.rspSetFirmwareDir(base.absolutePath)
        fwReady = base.absolutePath
        Log.i(TAG, "firmware ready (${names.size - 1} images match the manifest)")
        return fwReady
    }

    /**
     * Hand an opened RSP to the driver. The caller keeps its own connection (the server's start paths hold it); a
     * second attach of the same device is a no-op. Returns null on success, or the reason.
     */
    @Synchronized
    fun attach(ctx: Context, conn: UsbDeviceConnection, dev: UsbDevice): String? {
        if (!isRsp(dev)) return "not an SDRplay RSP on this Android version"
        prepareFirmware(ctx) ?: return "the RSP firmware inside the app is damaged — reinstall the app"
        if (!VibeLocalSDR.rspSupported()) return "the RSP driver could not be loaded"
        watch(ctx)
        val r = VibeLocalSDR.rspAttach(conn.fileDescriptor, dev.deviceName)
        if (r != 0) return "the RSP driver refused the device ($r)"
        conns[dev.deviceName] = conn
        Log.i(TAG, "attached ${modelName(dev.productId)} at ${dev.deviceName}")
        return null
    }

    /** A USB_DEVICE_ATTACHED for an RSP while a session is live — the post-firmware re-enumeration, or a re-plug.
     *  Also called from VibeServerRestore.onRadioAttached, which carries the activity's grant. */
    @Synchronized
    fun onAttached(ctx: Context, dev: UsbDevice) {
        if (!isRsp(dev) || receiver == null) return          // ★ no RSP session: an ordinary plug-in, nothing to do
        if (conns.containsKey(dev.deviceName)) return
        val mgr = ctx.getSystemService(Context.USB_SERVICE) as? UsbManager ?: return
        if (!mgr.hasPermission(dev)) {
            // ★ The driver is waiting (up to 15 s) for this device; ask now rather than let it time out.
            val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
            val intent = Intent(ACTION_PERMISSION).setPackage(ctx.packageName)
            Log.i(TAG, "${dev.deviceName}: asking for USB permission for the re-enumerated RSP")
            try { mgr.requestPermission(dev, PendingIntent.getBroadcast(ctx, 1, intent, flags)) }
            catch (t: Throwable) { Log.w(TAG, "permission request failed: $t") }
            return
        }
        val conn = mgr.openDevice(dev) ?: run { Log.w(TAG, "${dev.deviceName}: openDevice returned null"); return }
        val r = VibeLocalSDR.rspAttach(conn.fileDescriptor, dev.deviceName)
        if (r != 0) { conn.close(); Log.w(TAG, "${dev.deviceName}: driver refused the device ($r)"); return }
        conns[dev.deviceName] = conn
        Log.i(TAG, "re-attached ${modelName(dev.productId)} at ${dev.deviceName}")
    }

    @Synchronized
    private fun onDetached(dev: UsbDevice) {
        if (dev.vendorId != RSP_VID) return
        VibeLocalSDR.rspDetach(dev.deviceName)
        conns.remove(dev.deviceName)?.let { try { it.close() } catch (_: Throwable) {} }
    }

    /** The session is over (server stopped, local listening ended): detach everything and stop watching. Called
     *  AFTER the engine has stopped, so no handle of the driver's is still streaming from these devices. */
    @Synchronized
    fun releaseAll(ctx: Context) {
        for ((name, c) in conns) {
            VibeLocalSDR.rspDetach(name)
            try { c.close() } catch (_: Throwable) {}
        }
        conns.clear()
        receiver?.let { try { ctx.applicationContext.unregisterReceiver(it) } catch (_: Throwable) {} }
        receiver = null
        watchThread?.quitSafely(); watchThread = null
    }

    /** ★ Its own thread, like the service's watch: React's start-up can hold the main thread for seconds, and the
     *  driver gives the re-enumerated radio 15 s to come back. */
    private fun watch(ctx: Context) {
        if (receiver != null) return
        val app = ctx.applicationContext
        val t = android.os.HandlerThread("vibe-rsp-usb").apply { start() }
        val r = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) {
                @Suppress("DEPRECATION")
                val dev = i.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE) ?: return
                when (i.action) {
                    UsbManager.ACTION_USB_DEVICE_ATTACHED -> onAttached(app, dev)
                    UsbManager.ACTION_USB_DEVICE_DETACHED -> onDetached(dev)
                    ACTION_PERMISSION ->
                        if (i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) onAttached(app, dev)
                        else Log.w(TAG, "${dev.deviceName}: USB permission refused — the RSP cannot be used")
                }
            }
        }
        val f = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            addAction(ACTION_PERMISSION)
        }
        try {
            // ★ Our own permission broadcast is package-scoped (setPackage); the USB ones are system broadcasts.
            if (Build.VERSION.SDK_INT >= 33) app.registerReceiver(r, f, null, Handler(t.looper), Context.RECEIVER_NOT_EXPORTED)
            else app.registerReceiver(r, f, null, Handler(t.looper))
            receiver = r; watchThread = t
        } catch (e: Throwable) {
            Log.w(TAG, "cannot watch the RSP's USB re-enumeration: $e")
            t.quitSafely()
        }
    }
}
