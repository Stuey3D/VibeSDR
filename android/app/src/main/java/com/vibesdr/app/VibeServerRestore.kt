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
            // ★ A fresh start has its radio: no earlier departure may be held against it.
            .remove(K_GONE_ELAPSED).remove(K_GONE_BOOTWALL).remove(K_GONE_BOOTNO)
            .apply()
    }

    /**
     * ★★★ A SETTING CHANGED WHILE THE SERVER IS RUNNING BELONGS IN THE RESTORE CONFIG TOO.
     *
     * `arm` captures the config as it was at the last START, so anything changed afterwards is
     * invisible to a restore — the server comes back as it was, not as it is. Stuart turned the
     * DAB whole-multiplex label scan on while the TV was serving, and the very next restore put
     * it straight back off; the log says `label scan: off` at 00:02:21 with his switch still on
     * (2026-09-23). He had every reason to think the setting had not stuck, because it had not.
     * ★★ This is the [[never_defer_a_write_nothing_will_retry]] shape: the toggle wrote to
     *    AsyncStorage, which only the UI reads, and the thing that rebuilds the server reads
     *    somewhere else entirely.
     * ★ Merged rather than replaced, so a key this build does not know about survives.
     */
    fun updateConfig(ctx: Context, key: String, value: Any) {
        val p = prefs(ctx)
        val cfg = try { org.json.JSONObject(p.getString(K_CONFIG, "{}") ?: "{}") }
                  catch (_: Throwable) { org.json.JSONObject() }
        // ★ Nothing to update if the server has never run: the next start writes the whole config.
        if (cfg.length() == 0) return
        try { cfg.put(key, value) } catch (_: Throwable) { return }
        p.edit().putString(K_CONFIG, cfg.toString()).apply()
        Log.i(TAG, "restore config updated: $key = $value")
    }

    /** The user stopped the server ON PURPOSE — do not resurrect it. */
    /** ★ Was this phone serving when it stopped? The update path asks before it acts — see
     *  VibeUpdateReceiver. Read-only; arming stays the business of the JS that started the server. */
    fun isArmed(ctx: Context): Boolean = prefs(ctx).getBoolean(K_ARMED, false)
    /**
     * ★★★ HOW LONG THE RADIO MAY BE GONE AND STILL COUNT AS A BLIP — five minutes, Stuart's figure.
     *
     * Stuart, 2026-09-29: *"if a quick connection blip then yes because as we know USB connections can
     * be flaky and a radio may temporarily blip and come back. If unplugged for a decent amount of time
     * dont auto resume the server as there is a good job the owner may have forgotten they were serving
     * previously and we dont want to assume that is what they want to do this time."* And the window:
     * five minutes.
     * ★ ONE figure for BOTH ways a radio comes back — the re-attach resume below (MainActivity) and the
     *   engine's own fresh-fd recovery (recoverUsbIfNeeded, polled by RtlTcpServerService). Past it the
     *   server is stopped and DISARMED, as though the owner had pressed Stop.
     * ★ A reboot is never a blip, however quick: see radioGoneForMs.
     */
    const val RADIO_BLIP_WINDOW_MS = 5 * 60 * 1000L

    // ★ The moment the served radio LEFT, on three clocks — see radioGoneForMs for why three.
    private const val K_GONE_ELAPSED  = "radioGoneElapsedMs"   // SystemClock.elapsedRealtime()
    private const val K_GONE_BOOTWALL = "radioGoneBootWallMs"  // wall clock minus elapsed = when that boot began
    private const val K_GONE_BOOTNO   = "radioGoneBootCount"   // Settings.Global.BOOT_COUNT, -1 where unknown

    private fun bootCount(ctx: Context): Int = try {
        android.provider.Settings.Global.getInt(ctx.contentResolver, android.provider.Settings.Global.BOOT_COUNT, -1)
    } catch (t: Throwable) { Log.w(TAG, "boot count unreadable: ${t.message}"); -1 }

    /** ★ The served radio has gone — a USB detach, or the engine finding its handle dead. Stamped ONCE:
     *  the first sign counts, so a departure seen twice (the broadcast, then the engine) does not restart
     *  the clock. Only while a server is ARMED: a radio leaving a phone that is not serving means nothing. */
    fun noteRadioGone(ctx: Context, why: String) {
        val p = prefs(ctx)
        if (!p.getBoolean(K_ARMED, false) || p.contains(K_GONE_ELAPSED)) return
        val el = android.os.SystemClock.elapsedRealtime()
        p.edit()
            .putLong(K_GONE_ELAPSED, el)
            .putLong(K_GONE_BOOTWALL, System.currentTimeMillis() - el)
            .putInt(K_GONE_BOOTNO, bootCount(ctx))
            // ★ commit, not apply: a dongle pulled mid-stream can take the process down with it, and an
            //   apply still queued at that moment is lost — the stamp is what a later attach decides on.
            .commit()
        Log.i(TAG, "the server's radio has gone ($why) — the server resumes if it is back within " +
                   "${RADIO_BLIP_WINDOW_MS / 1000} s, and stops after that")
    }

    /** ★ The radio is back and the server has it — the gone-stamp is spent. */
    fun noteRadioBack(ctx: Context) {
        val p = prefs(ctx)
        if (!p.contains(K_GONE_ELAPSED)) return
        p.edit().remove(K_GONE_ELAPSED).remove(K_GONE_BOOTWALL).remove(K_GONE_BOOTNO).apply()
        Log.i(TAG, "the server's radio is back")
    }

    /**
     * How long the served radio has been gone, in THIS boot; -1 when it was never seen to leave, or
     * left in an earlier boot.
     * ★★★ A REBOOT IS NEVER A BLIP. A phone can restart inside five minutes, so a wall-clock age alone
     *     would call "switched off and on again" a blip and resume a server the owner may have forgotten.
     *     elapsedRealtime restarts at zero on every boot, so the age is measured on it — and only when the
     *     stamp was written in this same boot: BOOT_COUNT where the OS keeps one (API 24+), otherwise the
     *     boot's own start time (wall minus elapsed) agreeing to within a minute.
     */
    fun radioGoneForMs(ctx: Context): Long {
        val p = prefs(ctx)
        if (!p.contains(K_GONE_ELAPSED)) return -1
        val el0 = p.getLong(K_GONE_ELAPSED, 0L)
        val bootWall0 = p.getLong(K_GONE_BOOTWALL, 0L)
        val bootNo0 = p.getInt(K_GONE_BOOTNO, -1)
        val el = android.os.SystemClock.elapsedRealtime()
        val bootNo = bootCount(ctx)
        val sameBoot = if (bootNo0 >= 0 && bootNo >= 0) bootNo0 == bootNo
                       else kotlin.math.abs((System.currentTimeMillis() - el) - bootWall0) < 60_000L
        if (!sameBoot || el < el0) return -1
        return el - el0
    }

    /** ★ Has the gone radio outstayed its blip window? False while it is merely away, or never went. */
    fun radioGoneTooLong(ctx: Context): Boolean {
        if (!prefs(ctx).contains(K_GONE_ELAPSED)) return false
        val gone = radioGoneForMs(ctx)
        return gone < 0 || gone > RADIO_BLIP_WINDOW_MS
    }

    /** ★★★ PUT THE SERVER BACK WHEN ITS RADIO IS ATTACHED? Only after a BLIP — see RADIO_BLIP_WINDOW_MS.
     *
     *  ★★ THIS USED TO BE `bootWanted()` — armed AND the owner's "start on boot" switch — and that switch is
     *     GONE (2026-09-28). On the Sony it was proven undeliverable: VibeBootReceiver ran at BOOT_COMPLETED
     *     and the restore gave up 60 s later with "no USB permission". Android grants a USB device to an app
     *     only on a LIVE attach (through the default association the owner ticked), never to one that was
     *     already present when the system came up. A switch that promises otherwise is a promise we cannot
     *     keep (Stuart: "we would be promising something that we couldnt achieve").
     *  ★★★ AND "ARMED" STOPPED BEING ENOUGH (2026-09-29). A server the owner left running whose radio comes
     *      back within five minutes of LEAVING resumes. One whose radio was away longer — or that we never
     *      saw leave, which is what a reboot with the dongle in looks like (the XCover enumerates it on the
     *      way up) — does NOT, and is disarmed here so no later attach resumes it either. The owner presses
     *      Start; the screen shows the server stopped, because it is.
     *  ★ A server still RUNNING (the engine waiting for its dongle) is left entirely alone: its own recovery
     *    hands the fresh fd over on the same window (recoverUsbIfNeeded), and a restore would refuse to
     *    double-open the radio anyway. Nothing here may disarm a live server.
     *  ★ A `startOnBoot` key in a config stored by an older build is simply never read again. */
    fun attachResumeWanted(ctx: Context): Boolean {
        if (!isArmed(ctx)) return false
        if (isShimServing()) return false
        val gone = radioGoneForMs(ctx)
        if (gone in 0..RADIO_BLIP_WINDOW_MS) {
            Log.i(TAG, "radio back after ${gone / 1000} s — a blip, resuming the server")
            return true
        }
        Log.i(TAG, when {
            !prefs(ctx).contains(K_GONE_ELAPSED) ->
                "radio attached, but it was never seen to leave a running server (a restart?) — not resuming; press Start"
            gone < 0 -> "radio attached, but it left before a restart — not resuming; press Start"
            else -> "radio back after ${gone / 1000} s, longer than ${RADIO_BLIP_WINDOW_MS / 1000} s — not resuming; press Start"
        })
        disarm(ctx)
        return false
    }

    /** ★ Disarming also spends any gone-stamp: a stopped server has no radio to wait for. */
    fun disarm(ctx: Context) {
        prefs(ctx).edit().putBoolean(K_ARMED, false)
            .remove(K_GONE_ELAPSED).remove(K_GONE_BOOTWALL).remove(K_GONE_BOOTNO).apply()
    }

    /**
     * ★★★ STOP A SERVER WHOSE RADIO HAS BEEN GONE TOO LONG — as the owner's Stop button would.
     *
     * The engine waits for a vanished dongle indefinitely (usbNeedsFreshFd), so without this a radio
     * replugged an hour later was handed straight back and the server carried on serving: exactly the
     * "assume that is what they want" Stuart ruled out. Called from RtlTcpServerService's 2 s tick, which
     * then stops itself.
     * ★ The same steps as VibeLocalSdrModule.stopVibeServer — disarm first, mDNS, the engine, the held
     *   connection, LAN off, the secret out of memory. The module's own session connection is the same
     *   object as the held one; closing it again on the next start or stop is harmless.
     * ★ The settings screen sees `running:false` on its next status poll and goes back to Start.
     */
    fun stopBecauseRadioGone(ctx: Context) {
        Log.w(TAG, "the server's radio has been gone for more than ${RADIO_BLIP_WINDOW_MS / 1000} s — " +
                   "stopping the server; the owner starts it again when they want it")
        disarm(ctx)
        try { VibeLocalSDR.stopMdns() } catch (t: Throwable) { Log.w(TAG, "stopMdns: ${t.message}") }
        try { VibeLocalSDR.stopSpectrumSync() } catch (t: Throwable) { Log.w(TAG, "stopping the engine: ${t.message}") }
        releaseServerConn()
        try { VibeLocalSDR.setServeOnLan(false) } catch (t: Throwable) { Log.w(TAG, "setServeOnLan: ${t.message}") }
        try { VibeLocalSDR.setVibeServerAuth("") } catch (t: Throwable) { Log.w(TAG, "clearing the secret: ${t.message}") }
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
        // ★★ THE BLIP RULE ON THIS DOOR TOO (RADIO_BLIP_WINDOW_MS). A process that died while its radio was
        //    away comes back here, and a radio gone longer than the window is the owner's to restart.
        if (radioGoneTooLong(ctx)) { disarm(ctx); return RADIO_GONE_TOO_LONG }

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

        VibeLocalSDR.setUsbModelName(VibeServerBoot.usbModelName(dev))   // see VibeServerBoot
        val port = VibeServerBoot.applyAndStart(cfg, fd, dev.vendorId, dev.productId, ctx.filesDir)
        VibeServerBoot.startBatteryMonitor(ctx)   // after start — see VibeLocalSdrModule
        // ★★★ AND PUT THE PUBLIC LISTING BACK. The tunnel dies with the process that spawned it, so
        //     an update, a low-memory kill or a reboot leaves the directory advertising an address
        //     that answers 530 until the entry expires — seen 2026-08-22 after an install: "it just
        //     lost the server". The switch is a STANDING INSTRUCTION, not a one-off action, so it
        //     is re-established here where the port it needs has just been bound.
        // ★ Never allowed to fail the start: restoreIfWanted swallows everything.
        if (port > 0) VibeTunnel.restoreIfWanted(ctx, port)
        if (port > 0) VibeGeoData.start(ctx)
        if (port > 0) VibeMapGL.start(ctx)   // the GPU map — see VibeMapGL
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
        noteRadioBack(ctx)   // ★ whatever gap there was, the server has its radio again

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

    /** ★ The USB connection the running server's dongle was opened with (by either start path, or by
     *  the re-enumeration recovery below). The engine works on its own dup; this is kept so the
     *  connection is not collected, and so a DEAD one can be closed when a fresh one replaces it. */
    @Volatile private var heldConn: android.hardware.usb.UsbDeviceConnection? = null

    /** The app's start path hands its connection here, so the recovery below can let go of it. */
    fun holdServerConn(conn: android.hardware.usb.UsbDeviceConnection?) { heldConn = conn }

    /** A deliberate stop: close whatever connection the server was last given. Safe to call twice —
     *  the start path may close the same object itself. */
    fun releaseServerConn() {
        val c = heldConn; heldConn = null
        try { c?.close() } catch (t: Throwable) { Log.w(TAG, "closing the server's USB connection: ${t.message}") }
    }

    /**
     * ★★★ HAND THE ENGINE ITS DONGLE BACK AFTER A RE-ENUMERATION (Sony, 2026-09-28).
     *
     * The dongle stalled (tune rc=-9, PIPE), dropped off the bus and came back on a new
     * /dev/bus/usb path — and the engine went on using the fd it had, which is bound to the OLD
     * device instance and answers -ENODEV for ever. Every tune failed rc=-1 until a restart. The
     * engine now notices (the descriptor read fails), releases that handle and says so through
     * usbNeedsFreshFd(); this — polled by RtlTcpServerService while the server runs — is the half
     * that only Java can do: open the dongle afresh through UsbManager and hand the new fd in.
     * ★ Permission for a LIVE re-attach comes through the default association the owner ticked
     *   (Android launched the app on the re-attach at 22:13 that night); until it lands this
     *   answers "waiting" and the next poll tries again. Nothing is prompted from here.
     * ★ The old connection is CLOSED once the new one is accepted — never kept open.
     * ★★★ ONLY WITHIN THE BLIP WINDOW (RADIO_BLIP_WINDOW_MS, 2026-09-29). This is the blip case at the
     *     engine level — the server never stopped — and it obeys the same five minutes as the re-attach
     *     resume in MainActivity: past them it answers RADIO_GONE_TOO_LONG and the service stops the
     *     server rather than hand a radio back to a server its owner may have forgotten.
     * Returns a short state for the log, or null when there is nothing to do.
     */
    @Synchronized
    fun recoverUsbIfNeeded(ctx: Context): String? {
        val mgr = ctx.getSystemService(Context.USB_SERVICE) as? UsbManager ?: return "no USB service"
        if (!VibeLocalSDR.usbNeedsFreshFd()) {
            // ★ No server running: a stopped or not-yet-restored server is restore()'s to judge, not this.
            if (!isShimServing()) return null
            // ★ Nothing for the engine to adopt. A radio that left (the detach broadcast stamped it) and is
            //   listed again with the engine's handle alive is back; one still missing is judged on the clock.
            if (radioGoneForMs(ctx) >= 0 && mgr.deviceList.values.any { isServable(it) }) { noteRadioBack(ctx); return null }
            return if (radioGoneTooLong(ctx)) RADIO_GONE_TOO_LONG else null
        }
        noteRadioGone(ctx, "the engine's USB handle died")
        if (radioGoneTooLong(ctx)) return RADIO_GONE_TOO_LONG
        val dev = mgr.deviceList.values.firstOrNull { isRtlSdr(it) } ?: return "waiting for the dongle to come back"
        if (!mgr.hasPermission(dev)) return "waiting for USB permission for the dongle"
        val conn = mgr.openDevice(dev) ?: return "openDevice returned null"
        if (conn.fileDescriptor < 0 || !VibeLocalSDR.adoptFreshUsbFd(conn.fileDescriptor)) {
            conn.close()
            return "the engine did not take the fresh handle"
        }
        val old = heldConn
        heldConn = conn
        try { old?.close() } catch (t: Throwable) { Log.w(TAG, "closing the dead USB connection: ${t.message}") }
        Log.i(TAG, "dongle handed back to the engine on a fresh USB handle (${dev.deviceName})")
        noteRadioBack(ctx)
        return "handed back"
    }

    /** ★ What recoverUsbIfNeeded and restore answer when the radio outstayed RADIO_BLIP_WINDOW_MS. */
    const val RADIO_GONE_TOO_LONG = "the radio was gone too long to resume"

    private fun isServable(dev: UsbDevice): Boolean =
        VibeLocalSdrModule.isServableRadio(dev.vendorId, dev.productId)

    internal fun isShimServing(): Boolean = try {
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
