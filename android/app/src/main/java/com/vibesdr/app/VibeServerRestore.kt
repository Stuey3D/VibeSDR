package com.vibesdr.app

import android.content.Context
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.util.Log

/**
 * Rebuild a running VibeServer after the app PROCESS dies.
 *
 * RtlTcpServerService is START_STICKY, so Android already recreates the service on
 * its own after a crash or a low-memory kill. But it recreates only the SERVICE —
 * the native shim lived in the dead process and is gone. Without this, a crash left
 * a foreground notification claiming the server was up, with no radio behind it. A
 * zombie is worse than a clean stop.
 *
 * This is NOT the boot case, and deliberately makes no such promise. A reboot is
 * hopeless because Android's OTG stack never enumerates a dongle that was attached
 * while the phone was off, and no API can force it. A CRASH is entirely different:
 * the phone stayed up, so the dongle was never detached, it is still enumerated and
 * our USB permission still holds. We can simply re-open it and carry on — which is
 * why this one actually works.
 *
 * Config lives in ordinary SharedPreferences: unlike the boot path, the phone is
 * running and unlocked-at-least-once, so credential-encrypted storage is readable.
 */
object VibeServerRestore {
    private const val TAG = "VibeServerRestore"
    private const val PREFS = "vibe_server_restore"

    private const val K_ARMED     = "armed"       // a server was running when we died
    private const val K_NAME      = "name"
    private const val K_PIN       = "pin"
    private const val K_RATE      = "sampleRate"
    private const val K_LOCKED    = "lockedRate"
    private const val K_FFTRATE   = "maxFftRate"
    private const val K_COMPRESS  = "compressAudio"
    private const val K_WEB       = "webServer"
    private const val K_ADVERTISE = "advertise"
    private const val K_LOCJSON   = "locationJson"
    // ★★★ EVERY SETTING THE SERVER RUNS WITH BELONGS HERE. This snapshot is a HAND-MAINTAINED
    // SUBSET, and anything left out is silently DROPPED on the next restore — the server comes
    // back looking healthy with that protection quietly switched off.
    // ★ It bit exactly that way: the admin password and the session limit were added to the
    // start path and not to this one, so a process restart brought the server back with
    // admin:false and limitMin:0 while the PIN survived (because the PIN was on the list).
    // Stuart set both, saw neither take, and was right (2026-07-27).
    // ★ ADDING A SERVER SETTING? ADD IT HERE TOO. A security setting that fails OPEN across a
    // restart is worse than one that was never offered.
    private const val K_ADMINPW   = "adminPassword"
    private const val K_UNCOMP    = "uncompressedAudio"
    private const val K_LIMITMIN  = "sessionLimitMin"

    private fun prefs(ctx: Context) = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /** Remember the live config. Called when the server starts. */
    fun arm(
        ctx: Context, name: String, pin: String,
        sampleRate: Double, lockedRate: Double, maxFftRate: Double,
        compressAudio: Boolean, webServer: Boolean, advertise: Boolean,
        adminPassword: String, uncompressedAudio: Int, sessionLimitMin: Int,
    ) {
        prefs(ctx).edit()
            .putBoolean(K_ARMED, true)
            .putString(K_NAME, name)
            .putString(K_PIN, pin)
            .putFloat(K_RATE, sampleRate.toFloat())
            .putFloat(K_LOCKED, lockedRate.toFloat())
            .putFloat(K_FFTRATE, maxFftRate.toFloat())
            .putBoolean(K_COMPRESS, compressAudio)
            .putBoolean(K_WEB, webServer)
            .putBoolean(K_ADVERTISE, advertise)
            .putString(K_ADMINPW, adminPassword)
            .putInt(K_UNCOMP, uncompressedAudio)
            .putInt(K_LIMITMIN, sessionLimitMin)
            .apply()
    }

    /** The user stopped the server ON PURPOSE — do not resurrect it. */
    fun disarm(ctx: Context) {
        prefs(ctx).edit().putBoolean(K_ARMED, false).apply()
    }

    /** Cache what JS publishes, so a restored server still knows its own identity. */
    fun cacheLocation(ctx: Context, json: String) {
        prefs(ctx).edit().putString(K_LOCJSON, json).apply()
    }

    fun cacheStations(ctx: Context, json: String) {
        try { java.io.File(ctx.filesDir, "vs_stations.json").writeText(json) }
        catch (t: Throwable) { Log.w(TAG, "station cache failed: ${t.message}") }
    }

    /**
     * Called from the service's STICKY restart (null intent = we were recreated, not
     * started). Returns null on success, or a short reason.
     */
    @Synchronized
    fun restore(ctx: Context): String? {
        val p = prefs(ctx)
        if (!p.getBoolean(K_ARMED, false)) return "not armed"
        if (isShimServing()) return null                 // never double-open the dongle

        val mgr = ctx.getSystemService(Context.USB_SERVICE) as? UsbManager
            ?: return "no USB service"
        val dev: UsbDevice = mgr.deviceList.values.firstOrNull { isRtlSdr(it) }
            ?: return "no RTL-SDR attached"
        // No prompt is possible here (no activity), but after a crash the grant is
        // still live — the dongle never left.
        if (!mgr.hasPermission(dev)) return "no USB permission"

        val conn = mgr.openDevice(dev) ?: return "openDevice returned null"
        val fd = conn.fileDescriptor
        if (fd < 0) { conn.close(); return "bad fd" }

        val name    = p.getString(K_NAME, "VibeSDR") ?: "VibeSDR"
        val pin     = p.getString(K_PIN, "") ?: ""
        val rate    = p.getFloat(K_RATE, 2_400_000f).toDouble()
        val locked  = p.getFloat(K_LOCKED, 0f).toDouble()
        val fftRate = p.getFloat(K_FFTRATE, 20f).toDouble()

        VibeLocalSDR.setVibeServerAuth(pin)
        VibeLocalSDR.setVibeServerLimits(0.0, fftRate)
        VibeLocalSDR.setVibeServerCompressAudio(p.getBoolean(K_COMPRESS, true))
        VibeLocalSDR.setVibeServerWebEnabled(p.getBoolean(K_WEB, true))
        // ★ Restore the protections too — see the note by the keys. Defaults here are the SAFE
        // ones only because they match the product defaults; the point is that the stored value
        // is used, not that the fallback is harmless.
        VibeLocalSDR.setVibeServerAdminSecret(p.getString(K_ADMINPW, "") ?: "")
        VibeLocalSDR.setVibeServerUncompressedAudio(p.getInt(K_UNCOMP, 0))
        VibeLocalSDR.setVibeServerSessionLimit(p.getInt(K_LIMITMIN, 0))
        VibeLocalSDR.setVibeServerLockedRate(locked)
        VibeLocalSDR.setServeOnLan(true)

        val port = VibeLocalSDR.startSpectrum(
            fd, dev.vendorId, dev.productId, 100_000_000.0, rate, -1, 1024, fftRate, "nfm")
        if (port <= 0) {
            VibeLocalSDR.setServeOnLan(false)
            conn.close()
            return "native startSpectrum failed"
        }
        heldConn = conn      // the shim owns this fd; it must not be collected

        // Hand back the identity + station list JS would normally have published.
        val loc = p.getString(K_LOCJSON, "") ?: ""
        if (loc.isNotEmpty()) VibeLocalSDR.setLocationJson(loc)
        try {
            val f = java.io.File(ctx.filesDir, "vs_stations.json")
            if (f.exists()) VibeLocalSDR.setStationsJson(f.readText())
        } catch (_: Throwable) {}

        if (p.getBoolean(K_ADVERTISE, true)) advertise(ctx, name, port, pin.isNotEmpty())
        Log.i(TAG, "VibeServer rebuilt after a process death, port $port")
        return null
    }

    private var heldConn: android.hardware.usb.UsbDeviceConnection? = null

    private fun isShimServing(): Boolean = try {
        VibeLocalSDR.getVibeServerStatus().contains("\"running\":true")
    } catch (_: Throwable) { false }

    private fun isRtlSdr(dev: UsbDevice): Boolean {
        val key = (dev.vendorId shl 16) or dev.productId
        return VibeLocalSdrModule.RTL_SDR_VIDPIDS.contains(key)
    }

    private fun advertise(ctx: Context, name: String, port: Int, pinRequired: Boolean) {
        try {
            val m = ctx.getSystemService(Context.NSD_SERVICE) as? NsdManager ?: return
            val info = NsdServiceInfo().apply {
                serviceName = name
                serviceType = "_vibesdr._tcp."
                this.port = port
                setAttribute("name", name)
                setAttribute("proto", "vibeserver")
                setAttribute("pin", if (pinRequired) "1" else "0")
            }
            m.registerService(info, NsdManager.PROTOCOL_DNS_SD,
                object : NsdManager.RegistrationListener {
                    override fun onServiceRegistered(i: NsdServiceInfo) {}
                    override fun onRegistrationFailed(i: NsdServiceInfo, e: Int) {}
                    override fun onServiceUnregistered(i: NsdServiceInfo) {}
                    override fun onUnregistrationFailed(i: NsdServiceInfo, e: Int) {}
                })
        } catch (t: Throwable) {
            Log.w(TAG, "re-advertise failed: ${t.message}")
        }
    }
}
