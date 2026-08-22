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
    private const val K_LOCJSON   = "locationJson"
    /** ★★★ THE WHOLE CONFIG, AS THE APP SENT IT. This used to be a dozen hand-picked keys, and
     *  anything left off the list was silently dropped on restore — the server came back looking
     *  healthy with that setting quietly back at its default. It bit twice: the admin password and
     *  session limit (2026-07-27), then the RESTING GAIN, which reverted on every load (Stuart,
     *  2026-08-21). The instruction "add it here too" was written in capitals beside the old keys
     *  and was still missed both times, because a list you must remember to update is a list that
     *  will be wrong.
     *  ★ Now there is nothing to remember: the config is stored whole and replayed through the same
     *    VibeServerBoot.applyAndStart the normal start uses. */
    private const val K_CONFIG    = "configJson"

    private fun prefs(ctx: Context) = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /** Remember the live config, whole. Called when the server starts. */
    fun arm(ctx: Context, cfg: org.json.JSONObject) {
        prefs(ctx).edit()
            .putBoolean(K_ARMED, true)
            .putString(K_CONFIG, cfg.toString())
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
            ?: return "no SDR attached"
        // No prompt is possible here (no activity), but after a crash the grant is
        // still live — the dongle never left.
        if (!mgr.hasPermission(dev)) return "no USB permission"

        val conn = mgr.openDevice(dev) ?: return "openDevice returned null"
        val fd = conn.fileDescriptor
        if (fd < 0) { conn.close(); return "bad fd" }

        // ★★★ THE SAME APPLY PATH THE APP USES — see VibeServerBoot. Nothing is reconstructed or
        //     assumed here any more: whatever the server was running with is what it comes back
        //     with, including everything added since this file was last thought about.
        val cfg = try { org.json.JSONObject(p.getString(K_CONFIG, "{}") ?: "{}") }
                  catch (t: Throwable) { org.json.JSONObject() }
        // ★★ A restore with no stored config would silently rebuild a DEFAULT server — 100 MHz,
        //    nfm, no admin password, no limits — which is precisely the failure this rewrite
        //    exists to end. Refuse instead: a server that does not come back is a great deal
        //    easier to notice than one that comes back wrong.
        if (cfg.length() == 0) { conn.close(); return "no stored config" }

        val port = VibeServerBoot.applyAndStart(cfg, fd, dev.vendorId, dev.productId, ctx.filesDir)
        // ★★★ AND PUT THE PUBLIC LISTING BACK. The tunnel dies with the process that spawned it, so
        //     an update, a low-memory kill or a reboot leaves the directory advertising an address
        //     that answers 530 until the entry expires — seen 2026-08-22 after an install: "it just
        //     lost the server". The switch is a STANDING INSTRUCTION, not a one-off action, so it
        //     is re-established here where the port it needs has just been bound.
        // ★ Never allowed to fail the start: restoreIfWanted swallows everything.
        if (port > 0) VibeTunnel.restoreIfWanted(ctx, port)
        if (port > 0) VibeGeoData.start(ctx)
        // ★★ STATION LOGOS FROM THE BROADCASTER, wired on BOTH start paths. The geo lookup above
        //    had to learn that lesson too: a headless restore is how this server usually comes
        //    back, so anything wired only where the UI starts it is missing exactly when nobody
        //    is watching.
        //  ★ The country is a hint from the device's own locale — tried first, with the rest of
        //    the ECC candidates behind it, so a wrong or absent one still resolves.
        if (port > 0) {
            try {
                VibeLocalSDR.initRadioDns(
                    java.io.File(ctx.filesDir, "radiodns").apply { mkdirs() }.absolutePath,
                    java.util.Locale.getDefault().country ?: "")
            } catch (t: Throwable) {
                android.util.Log.w("VibeSDR", "RadioDNS not started: ${t.message}")
            }
        }
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

        if (VibeServerBoot.advertise(cfg))
            advertise(ctx, VibeServerBoot.name(cfg), port, VibeServerBoot.pin(cfg).isNotEmpty())
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
