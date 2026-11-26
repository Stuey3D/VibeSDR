package com.vibesdr.app

import android.content.Context
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.IBinder
import android.os.Parcel
import android.util.Log
import org.json.JSONObject

/**
 * ★★★ WHAT THE SERVER PROCESS DOES WHEN THE UI ASKS (beta88, 2026-11-20) — reached only through VibeServerHost.
 *
 * Every body here USED TO LIVE IN VibeLocalSdrModule and ran in the UI's process, beside React Native and WebView, so
 * anything that killed the UI killed the server: a Play update of System WebView (XCover 2026-11-20 14:42:51, and
 * 10-28), a backup's kill of the app process (15:18:10), a crash in the UI. They were moved here as they were — same
 * order, same notes — so the start the owner presses and the restore Android runs are still ONE path
 * (VibeServerBoot.applyAndStart), now both in the process that survives.
 *
 * ★★ What stays in the UI: anything that needs an Activity — the USB permission prompt, the Icom microphone dialog —
 *    and Local Hardware listening, which has its own copy of the native engine in the UI's process. The UI asks for the
 *    permission, then hands this side the radio's identity; this side opens it (USB permission is per app, so the grant
 *    the UI obtained is this process's too).
 * ★ Answers are JSON: {ok:true, …} or {ok:false, code, message}.
 */
object VibeServerOps {
    private const val TAG = "VibeServerOps"

    fun ok(): JSONObject = JSONObject().put("ok", true)
    fun fail(code: String, message: String): JSONObject =
        JSONObject().put("ok", false).put("code", code).put("message", message)

    // ── Events to the UI (VibeServerLink listeners) ──────────────────────────────────────────────────────────────

    private val listeners = java.util.concurrent.CopyOnWriteArrayList<IBinder>()

    fun addListener(b: IBinder) {
        if (listeners.any { it == b }) return
        listeners.add(b)
        try { b.linkToDeath({ listeners.remove(b) }, 0) } catch (_: Throwable) { /* in-process: never dies alone */ }
    }

    /** ★ One-way: a UI that is slow or gone never holds the server up. */
    fun emit(type: String, json: JSONObject) {
        for (b in listeners) {
            val d = Parcel.obtain()
            try {
                d.writeString(type)
                d.writeString(json.toString())
                b.transact(VibeServerHost.EV_EVENT, d, null, IBinder.FLAG_ONEWAY)
            } catch (t: Throwable) {
                listeners.remove(b)
            } finally { d.recycle() }
        }
    }

    // ── The dispatcher ───────────────────────────────────────────────────────────────────────────────────────────

    fun call(ctx: Context, method: String, a: JSONObject): JSONObject = when (method) {
        "start"            -> startVibeServer(ctx, a)
        "stop"             -> { stopVibeServer(ctx); ok() }
        "status"           -> status(ctx)
        "isServing"        -> ok().put("serving", VibeServerRestore.isShimServing())
        "set"              -> set(ctx, a)
        "bookmarks.get"    -> ok().put("json", VibeLocalSDR.getBookmarksJson())
        "bookmarks.clear"  -> { VibeLocalSDR.clearBookmarks(); ok() }
        "mdnsHostname"     -> ok().put("host", VibeLocalSDR.mdnsHostname())
        "radioCaps"        -> ok().put("json", try { VibeLocalSDR.radioCaps() } catch (_: Throwable) { "" })
        "icomFacts"        -> ok().put("json", VibeIcom.facts(ctx).toString())
        "icomServing"      -> ok().put("serving", VibeIcom.serving())
        "icomMicGranted"   -> { RtlTcpServerService.wantMicrophone(ctx); IcomAudio.startIfPossible(ctx); ok() }
        "icomReport"       -> { IcomAudio.report(ctx, problem = a.optString("problem")); ok() }
        "radioAttached"    -> radioAttached(ctx, a)
        "rtl.start"        -> rtlStart(ctx, a)
        "rtl.stop"         -> { rtlStop(ctx); ok() }
        "rtl.status"       -> rtlStatus()
        "tunnel.supported" -> ok().put("supported", try { VibeTunnel.isSupported(ctx) } catch (_: Throwable) { false })
        "tunnel.status"    -> ok().put("json", VibeTunnel.statusJson())
        "tunnel.start"     -> tunnelStart(ctx, a)
        "tunnel.republish" -> tunnelRepublish(ctx, a)
        "tunnel.stop"      -> tunnelStop(ctx, a)
        "watchdog"         -> { VibeServerWatchdog.check(ctx, "asked by the app"); ok() }
        // ★★ Item B — the server's settings, one truth for the phone's screen and the /setup page. See VibeRemoteSetup.
        "settings.get"     -> VibeRemoteSetup.settingsGet(ctx)
        "settings.put"     -> VibeRemoteSetup.settingsPut(ctx, a)
        "remote.set"       -> VibeRemoteSetup.setRemoteAccess(ctx, a.optBoolean("on", false))
        else               -> fail("unknown_method", "the server does not know \"$method\"")
    }

    // ── Start / stop ─────────────────────────────────────────────────────────────────────────────────────────────

    /** The radio the UI chose and got permission for — matched by its bus name first, then by its ids. */
    private fun findDevice(mgr: UsbManager, a: JSONObject): UsbDevice? {
        val name = a.optString("deviceName", "")
        val vid = a.optInt("vid", -1); val pid = a.optInt("pid", -1)
        val all = mgr.deviceList.values
        return all.firstOrNull { it.deviceName == name && it.vendorId == vid && it.productId == pid }
            ?: all.firstOrNull { it.vendorId == vid && it.productId == pid }
    }

    /** ★★ A START THAT NEVER RETURNS SAYS WHERE IT IS (the Sony, Lite 77, 2026-10-19) — moved here with the start. */
    private fun <T> stallWatched(what: String, body: () -> T): T {
        val target = Thread.currentThread()
        val done = java.util.concurrent.atomic.AtomicBoolean(false)
        val w = Thread({
            var secs = 0
            while (!done.get()) {
                try { Thread.sleep(3000) } catch (_: InterruptedException) { return@Thread }
                if (done.get()) break
                secs += 3
                val st = target.stackTrace.take(30).joinToString("\n  at ")
                Log.w("VibeStall", "$what still running after ${secs}s on ${target.name}:\n  at $st")
            }
        }, "vibe-stall-watch")
        w.isDaemon = true
        w.start()
        try { return body() } finally { done.set(true); w.interrupt() }
    }

    /** ★ The server's engine and its USB connection, put away (was the module's stopSpectrumInternal for the server). */
    private fun stopEngine(ctx: Context) {
        VibeIcom.onServerStopping()   // ★ the AudioRecord thread first, then the engine it pushes into
        try { VibeLocalSDR.stopSpectrumSync() } catch (_: Throwable) {}
        VibeRsp.releaseAll(ctx)
        VibeServerRestore.releaseServerConn()
    }

    @Synchronized
    fun startVibeServer(ctx: Context, a: JSONObject): JSONObject = stallWatched("VibeServer start") {
        val mgr = ctx.getSystemService(Context.USB_SERVICE) as? UsbManager
            ?: return@stallWatched fail("no_usb", "USB service unavailable")
        val dev = findDevice(mgr, a) ?: return@stallWatched fail("no_device", "No SDR found")
        if (!mgr.hasPermission(dev)) return@stallWatched fail("no_permission", "Allow this app to use the radio, then press Start again")
        val cfgIn = a.optJSONObject("cfg") ?: return@stallWatched fail("bad_config", "the server was not given its settings")
        /* ★★★ beta96 VIBESERVER SIMPLE sends only its four (src/utils/serverChoice.ts simpleStartConfig). They go INTO the
         *  one settings store, which keeps every Full setting for the day the owner goes back to Full; the engine then runs
         *  the Simple view of it (VibeServerBoot.effective — local network only). */
        val cfg = if (VibeServerBoot.isSimple(cfgIn))
            VibeServerBoot.simpleMerge(try { VibeRemoteSetup.settings(ctx) } catch (_: Throwable) { JSONObject() }, cfgIn)
        else cfgIn

        stopEngine(ctx)
        rtlStop(ctx)
        unadvertise(ctx)
        // ★★★ Not while another VibeServer serves on this device (the other VibeSDR app, or Lite) — VibeServerBoot.
        VibeServerBoot.otherServerOnDevice()?.let { return@stallWatched fail("other_server", VibeServerBoot.otherServerMessage(it)) }
        val conn = mgr.openDevice(dev) ?: return@stallWatched fail("open_failed", "openDevice returned null")
        val fd = conn.fileDescriptor
        if (fd < 0) { conn.close(); return@stallWatched fail("bad_fd", "Invalid file descriptor") }
        // ★★ An RSP goes to OpenRSP's service, not to the engine — see VibeRsp and LocalSdrShim::start.
        if (VibeRsp.isRsp(dev)) VibeRsp.attach(ctx, conn, dev)?.let { conn.close(); return@stallWatched fail("rsp", it) }
        // ★ Shared with the re-enumeration recovery, which closes it when it has to replace it.
        VibeServerRestore.holdServerConn(conn, dev)

        val name = VibeServerBoot.name(cfg)
        val pin = VibeServerBoot.pin(cfg)
        val autoRestore = VibeServerBoot.autoRestore(cfg)
        val icom = VibeIcom.isIcom(dev)

        VibeLocalSDR.setUsbModelName(VibeServerBoot.usbModelName(dev))
        if (icom) VibeIcom.prepareStart(cfg, dev)
        val port = VibeServerBoot.applyAndStart(cfg, fd, dev.vendorId, dev.productId, ctx.filesDir, serveOnLan = true,
                                                keepRadioAlive = VibeServerBoot.keepRadioAlive(ctx, cfg))
        VibeServerBoot.startBatteryMonitor(ctx)
        // ★★★ beta96 a Simple server is LOCAL NETWORK ONLY: its public listing is not put back (the owner's standing
        //     instruction is kept for Full — not a delist), and a tunnel still up from before is closed.
        if (port > 0 && !VibeServerBoot.isSimple(cfg)) VibeTunnel.restoreIfWanted(ctx, port)
        if (port > 0 && VibeServerBoot.isSimple(cfg)) try { VibeTunnel.pauseForServerStop() } catch (_: Throwable) {}
        if (port > 0) VibeGeoData.start(ctx)
        if (port > 0) VibeMapGL.start(ctx)
        if (port > 0) {
            try {
                VibeLocalSDR.initRadioDns(java.io.File(ctx.filesDir, "radiodns").apply { mkdirs() }.absolutePath,
                                          java.util.Locale.getDefault().country ?: "")
            } catch (t: Throwable) { Log.w(TAG, "RadioDNS not started: ${t.message}") }
        }
        if (port <= 0) {
            VibeLocalSDR.setServeOnLan(false)
            conn.close()
            VibeServerRestore.holdServerConn(null)
            return@stallWatched fail("start_failed", "native startVibeServer failed (see logcat)")
        }
        val ip = localIp() ?: "0.0.0.0"
        if (ip != "0.0.0.0") {
            acquireMulticastLock(ctx)
            VibeLocalSDR.startMdns(hostSlug(VibeServerBoot.mdnsName(cfg)), ip)
        }
        RtlTcpServerService.start(ctx, name, ip, port, "vibeserver")
        if (icom) VibeIcom.onServerStarted(ctx)
        if (autoRestore) VibeServerRestore.arm(ctx, cfg, dev) else VibeServerRestore.disarm(ctx)
        // ★★ THE PHONE'S OWN SETTINGS STORE IS THE TRUTH (item B) — the config just started with is what /setup shows.
        VibeRemoteSetup.onStarted(ctx, cfg)
        // ★★ NSD here, beside the server, not in the UI: a registration dies with the process that made it, and the UI is
        //    exactly the process that can now die without taking the server along.
        if (VibeServerBoot.advertise(cfg)) advertise(ctx, name, port, pin.isNotEmpty())
        // ★★★ THE WATCHDOG — brings the server back if this process dies and Android does not (VibeServerWatchdog).
        if (autoRestore) VibeServerWatchdog.schedule(ctx)
        Log.i(TAG, "VibeServer started $ip:$port as \"$name\" (pin=${pin.isNotEmpty()}) in process ${VibeProcess.name(ctx)}")
        ok().put("ip", ip).put("port", port).put("name", name).put("icom", icom)
    }

    @Synchronized
    fun stopVibeServer(ctx: Context) {
        // DISARM FIRST. A deliberate stop must not be undone: the restore and the watchdog read it.
        VibeServerRestore.disarm(ctx)
        VibeServerWatchdog.cancel(ctx)
        unadvertise(ctx)
        VibeLocalSDR.stopMdns()
        releaseMulticastLock()
        RtlTcpServerService.stop(ctx)
        stopEngine(ctx)
        VibeLocalSDR.setServeOnLan(false)
        VibeLocalSDR.setVibeServerAuth("")   // clear the secret from process memory
        try { VibeTunnel.pauseForServerStop() } catch (t: Throwable) { Log.w(TAG, "tunnel pause: ${t.message}") }
    }

    /**
     * ★★ THE /setup PAGE SAVED AND ASKED FOR A RESTART (VibeRemoteSetup.configSetPatch) — "Saving restarts it, so listeners
     * will reconnect", as on Linux. The engine is stopped and started again from the settings just saved, through the
     * restore (VibeServerRestore.restore → VibeServerBoot.applyAndStart): one start path. The foreground service keeps
     * running throughout, and the server stays armed.
     * ★ Later, on its own thread: the request that asked for it is still being answered by the engine being restarted.
     */
    fun restartFromSettings(ctx: Context, after: (() -> Unit)? = null) {
        val app = ctx.applicationContext
        Thread({
            try { Thread.sleep(600) } catch (_: InterruptedException) {}
            synchronized(this) {
                if (!VibeServerRestore.isShimServing()) { Log.i(TAG, "remote save: no server running — it starts with the new settings"); return@synchronized }
                VibeServerRestore.breadcrumb(app, "restarting for the remote settings page")
                unadvertise(app)
                try { VibeLocalSDR.stopMdns() } catch (_: Throwable) {}
                stopEngine(app)
                val err = VibeServerRestore.restore(app)
                if (err != null) {
                    Log.w(TAG, "the restart for the remote settings did not come back: $err")
                    VibeServerRestore.breadcrumb(app, "remote restart failed: $err")
                    RtlTcpServerService.kickRestore("remote restart failed: $err")
                    return@synchronized
                }
                val ip = localIp() ?: ""
                val name = VibeServerBoot.mdnsName(VibeRemoteSetup.settings(app))
                if (ip.isNotEmpty()) { acquireMulticastLock(app); VibeLocalSDR.startMdns(hostSlug(name), ip) }
                Log.i(TAG, "restarted with the settings saved on the remote page")
            }
            try { after?.invoke() } catch (t: Throwable) { Log.w(TAG, "after the remote restart: $t") }
        }, "vibe-remote-restart").start()
    }

    // ── Status ───────────────────────────────────────────────────────────────────────────────────────────────────

    /** ★ The shim's own status plus what only this process can read: the LAN address and THIS process's CPU — which,
     *  with the split, is the server's cost alone, the figure the owner actually wants. */
    fun status(ctx: Context): JSONObject {
        val o = try { JSONObject(VibeLocalSDR.getVibeServerStatus()) } catch (_: Throwable) { JSONObject().put("running", false) }
        val out = ok().put("status", o)
        try { out.put("ip", if (o.optBoolean("running", false)) (localIp() ?: "") else "") } catch (_: Throwable) { out.put("ip", "") }
        try { out.put("cpu", processCpuPercent()) } catch (_: Throwable) { out.put("cpu", 0.0) }
        try { out.put("cores", Runtime.getRuntime().availableProcessors()) } catch (_: Throwable) { out.put("cores", 0) }
        return out
    }

    private var lastCpuTicks = 0L
    private var lastCpuAt = 0L
    private var cpuValue = 0.0
    private val clkTck: Long by lazy {
        try { android.system.Os.sysconf(android.system.OsConstants._SC_CLK_TCK) } catch (_: Throwable) { 100L }
    }
    /** Percent of ONE core used by this process (see the module's old note: self-timed, cached for 500 ms). */
    @Synchronized
    fun processCpuPercent(): Double = try {
        val now = android.os.SystemClock.elapsedRealtime()
        if (lastCpuAt > 0L && now - lastCpuAt < 500L) cpuValue
        else {
            val stat = java.io.File("/proc/self/stat").readText()
            val f = stat.substring(stat.lastIndexOf(')') + 2).split(" ")
            val ticks = f[11].toLong() + f[12].toLong()
            if (lastCpuAt > 0L) {
                cpuValue = (((ticks - lastCpuTicks).toDouble() / clkTck) / ((now - lastCpuAt) / 1000.0) * 100.0)
                    .coerceIn(0.0, 100.0 * Runtime.getRuntime().availableProcessors())
            }
            lastCpuTicks = ticks; lastCpuAt = now
            cpuValue
        }
    } catch (_: Throwable) { 0.0 }

    // ── Live settings on a running server ───────────────────────────────────────────────────────────────────────

    private fun set(ctx: Context, a: JSONObject): JSONObject {
        when (a.optString("what")) {
            "compressAudio"    -> VibeLocalSDR.setVibeServerCompressAudio(a.optBoolean("on", true))
            "adminSecret"      -> VibeLocalSDR.setVibeServerAdminSecret(a.optString("secret", ""))
            "uncompressed"     -> VibeLocalSDR.setVibeServerUncompressedAudio(a.optInt("mode", 0))
            "sessionLimit"     -> VibeLocalSDR.setVibeServerSessionLimit(a.optInt("minutes", 0))
            "stations"         -> { val j = a.optString("json", "[]"); VibeLocalSDR.setStationsJson(j); VibeServerRestore.cacheStations(ctx, j) }
            "location"         -> { val j = a.optString("json", ""); VibeLocalSDR.setLocationJson(j); VibeServerRestore.cacheLocation(ctx, j) }
            "bookmarks"        -> VibeLocalSDR.setBookmarksJson(a.optString("json", "[]"))
            "keepRadioAlive"   -> { val on = a.optBoolean("on", false); VibeLocalSDR.setKeepRadioAlive(on); VibeServerRestore.updateConfig(ctx, "keepRadioAlive", on) }
            "dabScanLabels"    -> { val m = a.optInt("mode", -1); VibeLocalSDR.setDabScanLabels(m); VibeServerRestore.updateConfig(ctx, "dabScanLabels", m) }
            "antennaBands"     -> VibeLocalSDR.setAntennaBands(a.optString("ranges", ""), a.optString("filters", ""))
            "antennaPortNames" -> VibeLocalSDR.setAntennaPortNames(a.optString("json", ""))
            "antennaSwitch"    -> VibeLocalSDR.setAntennaSwitch(a.optString("json", ""), a.optString("map", ""), a.optBoolean("locked", false))
            else               -> return fail("unknown_setting", "the server does not know \"${a.optString("what")}\"")
        }
        return ok()
    }

    // ── A radio was attached (the activity Android launched for it) ────────────────────────────────────────────

    private fun radioAttached(ctx: Context, a: JSONObject): JSONObject {
        val mgr = ctx.getSystemService(Context.USB_SERVICE) as? UsbManager
        val dev = if (mgr != null && a.has("vid")) findDevice(mgr, a) else null
        VibeServerRestore.onRadioAttached(ctx, dev, a.optString("why", "a radio was attached"))
        return ok()
    }

    // ── The rtl_tcp server — the same service, mode "rtltcp" ──────────────────────────────────────────────────────


    /** ★★★ RTL-TCP = THE ENGINE + ITS FRONT DOOR (Sloper, 2026-11-25). The librtlsdr-only server opened every radio
     *  as a dongle and sent an Airspy's or an RSP's owner a header and then nothing. Now the radio is opened exactly as
     *  VibeServer opens it — its own driver, the RSP through OpenRSP — and the engine serves it on the rtl_tcp port
     *  (LocalSdrShim::startRtlTcpDoor): RAW IQ OUT's stream with VibeAGC and every capture fix, LOCAL NETWORK ONLY.
     *  The engine runs with the owner's saved radio settings (ppm, auto-PPM, notches, the RSP's VibeAGC choice…) and
     *  NONE of the serving: no web page on the LAN (loopback only), no listing, no tunnel, no background decoding, no
     *  restore — one LAN client drives the radio. A radio that will not open, or a port that is taken, is a refusal
     *  with the reason, never a server that looks up. */
    @Synchronized
    private fun rtlStart(ctx: Context, a: JSONObject): JSONObject = stallWatched("RTL-TCP start") {
        val mgr = ctx.getSystemService(Context.USB_SERVICE) as? UsbManager ?: return@stallWatched fail("no_usb", "USB service unavailable")
        val dev = findDevice(mgr, a) ?: return@stallWatched fail("no_device", "No SDR found")
        if (!mgr.hasPermission(dev)) return@stallWatched fail("no_permission", "Allow this app to use the radio, then press Start again")
        if (VibeIcom.isIcom(dev)) return@stallWatched fail("icom", "An Icom sends no IQ — share it with VibeServer")
        val o = a.optJSONObject("opts") ?: JSONObject()
        // ★ An rtl_tcp server is not restorable: a VibeServer armed earlier must not be brought back over it.
        VibeServerRestore.disarm(ctx)
        VibeServerWatchdog.cancel(ctx)
        stopEngine(ctx)
        rtlStop(ctx)
        unadvertise(ctx)
        VibeServerBoot.otherServerOnDevice()?.let { return@stallWatched fail("other_server", VibeServerBoot.otherServerMessage(it)) }
        val conn = mgr.openDevice(dev) ?: return@stallWatched fail("open_failed", "The radio could not be opened")
        val fd = conn.fileDescriptor
        if (fd < 0) { conn.close(); return@stallWatched fail("bad_fd", "Invalid file descriptor") }
        if (VibeRsp.isRsp(dev)) VibeRsp.attach(ctx, conn, dev)?.let { conn.close(); return@stallWatched fail("rsp", it) }
        VibeServerRestore.holdServerConn(conn, dev)   // ★ released by stopEngine, as for VibeServer

        val name = o.optString("name", "VibeSDR").ifEmpty { "VibeSDR" }
        val port = o.optInt("port", 1234)
        val cfg = rtlTcpConfig(ctx, dev)
        VibeLocalSDR.setUsbModelName(VibeServerBoot.usbModelName(dev))
        val enginePort = VibeServerBoot.applyAndStart(cfg, fd, dev.vendorId, dev.productId, ctx.filesDir, serveOnLan = false,
                                                      keepRadioAlive = VibeServerBoot.keepRadioAlive(ctx, cfg))
        if (enginePort <= 0) {
            stopEngine(ctx)
            return@stallWatched fail("start_failed", "The radio did not start — unplug it, plug it back in and try again")
        }
        val why = VibeLocalSDR.startRtlTcpDoor(port)
        if (why.isNotEmpty()) {
            stopEngine(ctx)
            return@stallWatched fail("door_failed", "The RTL-TCP server did not start: $why")
        }
        rtlRunning = true
        val ip = localIp() ?: "0.0.0.0"
        RtlTcpServerService.start(ctx, name, ip, port)
        Log.i(TAG, "RTL-TCP server started $ip:$port as \"$name\" on ${VibeServerBoot.usbModelName(dev)} (local network only)")
        ok().put("ip", ip).put("port", port).put("name", name)
    }

    /** The engine's settings for RTL-TCP: the owner's saved radio settings, with everything that SERVES turned off. */
    private fun rtlTcpConfig(ctx: Context, dev: UsbDevice): JSONObject {
        val cfg = try { JSONObject(VibeRemoteSetup.settings(ctx).toString()) } catch (_: Throwable) { JSONObject() }
        // ★ The starting rate is this radio's lightest sensible one; the client sets its own at once.
        val rate = when {
            VibeRsp.isRsp(dev) -> 2_000_000.0
            dev.vendorId == VibeRadioIds.AIRSPYHF_VID && dev.productId == VibeRadioIds.AIRSPYHF_PID -> 912_000.0
            dev.vendorId == VibeRadioIds.AIRSPY_VID && dev.productId == VibeRadioIds.AIRSPY_PID -> 3_000_000.0
            dev.vendorId == VibeRadioIds.HACKRF_VID && dev.productId == VibeRadioIds.HACKRF_PID -> 2_000_000.0
            else -> 2_400_000.0
        }
        cfg.put("sampleRate", rate)
        cfg.put("gainTenthDb", -1)                  // ★ VibeAGC (or the radio's AGC) until the client says otherwise
        cfg.put("advanced", false)                  // ★ one listener: the rtl_tcp client
        cfg.put("webServer", false)
        cfg.put("bgDecode", ""); cfg.put("dxScan", ""); cfg.put("adsbFeed", "")   // ★★ beta96 no Beast port / pushes here
        cfg.put("rawIq", 0); cfg.put("rawIqMax", 0)
        cfg.put("pskrOn", false); cfg.put("wsprnetOn", false)
        cfg.put("landingAdsb", false); cfg.put("adsbKeepRunning", false)
        cfg.put("landingMap", ""); cfg.put("landingModes", ""); cfg.put("mapKeep", false); cfg.put("singleMode", "")   // ★★★ beta96 nor a kept map mode
        cfg.put("spectrogram", false)
        cfg.put("lockedCentre", 0.0); cfg.put("lockedRate", 0.0); cfg.put("rateLock", false)
        cfg.put("fftRate", 10.0)                    // ★ the spectrum only feeds VibeAGC here — nobody watches it
        return cfg
    }

    @Volatile private var rtlRunning = false

    @Synchronized
    internal fun rtlStop(ctx: Context) {
        if (!rtlRunning) return
        rtlRunning = false
        try { VibeLocalSDR.stopRtlTcpDoor() } catch (_: Throwable) {}
        stopEngine(ctx)
        try { RtlTcpServerService.stop(ctx) } catch (_: Throwable) {}
    }

    private fun rtlStatus(): JSONObject {
        val o = try { JSONObject(VibeLocalSDR.getServerStatus()) } catch (_: Throwable) { JSONObject().put("running", false) }
        return ok().put("status", o)
            .put("ip", if (o.optBoolean("running", false)) (localIp() ?: "") else "")
            .put("cpu", processCpuPercent())
            .put("cores", Runtime.getRuntime().availableProcessors())
    }

    // ── The public listing (cloudflared lives and dies with the process that spawned it — so, here) ───────────────

    private fun tunnelStart(ctx: Context, a: JSONObject): JSONObject {
        VibeTunnel.applyLoopbackTrust(true, a.optString("ownerProxies", ""))
        val done = java.util.concurrent.CountDownLatch(1)
        VibeTunnel.startTunnel(ctx, a.optInt("port", 0)) { url ->
            try {
                if (url != null) VibeTunnel.publish(ctx, a.optString("name"), a.optString("locator"), a.optInt("port"),
                    a.optString("radioModel"), a.optString("radioDriver"), a.optString("antenna"),
                    a.optString("coverage"), a.optBoolean("locked"), a.optLong("shareForSec", 0L))
            } finally { done.countDown() }
        }
        // ★ cloudflared can take a while to hand out an address on a slow line; the UI's promise waits with it.
        done.await(120, java.util.concurrent.TimeUnit.SECONDS)
        return ok().put("json", VibeTunnel.statusJson())
    }

    private fun tunnelRepublish(ctx: Context, a: JSONObject): JSONObject {
        val port = a.optInt("port", 0); val name = a.optString("name", "")
        if (!VibeTunnel.isTunnelRunning() || port <= 0 || name.length < 2) return ok().put("json", VibeTunnel.statusJson())
        VibeTunnel.publish(ctx, name, a.optString("locator"), port, a.optString("radioModel"), a.optString("radioDriver"),
                           a.optString("antenna"), a.optString("coverage"), a.optBoolean("locked"), a.optLong("shareForSec", -1L))
        return ok().put("json", VibeTunnel.statusJson())
    }

    private fun tunnelStop(ctx: Context, a: JSONObject): JSONObject {
        VibeTunnel.delist(ctx)
        VibeTunnel.stopTunnel()
        VibeTunnel.applyLoopbackTrust(false, a.optString("ownerProxies", ""))
        return ok().put("json", VibeTunnel.statusJson())
    }

    // ── The network bits a server needs (moved from the module) ────────────────────────────────────────────────────

    private var multicastLock: android.net.wifi.WifiManager.MulticastLock? = null

    /** MULTICAST LOCK — mandatory for the mDNS responder (Android drops multicast for apps that hold none). */
    private fun acquireMulticastLock(ctx: Context) {
        if (multicastLock != null) return
        try {
            val wm = ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as android.net.wifi.WifiManager
            multicastLock = wm.createMulticastLock("VibeSDR:mDNS").apply { setReferenceCounted(false); acquire() }
        } catch (t: Throwable) { Log.w(TAG, "multicast lock failed: ${t.message}") }
    }
    private fun releaseMulticastLock() {
        try { multicastLock?.let { if (it.isHeld) it.release() } } catch (_: Throwable) {}
        multicastLock = null
    }

    /** "VibeSDR: Moto G35" -> "vibesdr-moto-g35". */
    fun hostSlug(name: String): String {
        val s = name.lowercase().replace(Regex("[^a-z0-9]+"), "-").trim('-')
        return if (s.isEmpty()) "vibesdr" else s.take(32)
    }

    /** Best-effort non-loopback IPv4 for the LAN (prefer wlan/AP interfaces). */
    fun localIp(): String? = try {
        var fallback: String? = null
        var found: String? = null
        for (nif in java.net.NetworkInterface.getNetworkInterfaces()) {
            if (!nif.isUp || nif.isLoopback) continue
            for (addr in nif.inetAddresses) {
                if (addr.isLoopbackAddress || addr !is java.net.Inet4Address) continue
                val ip = addr.hostAddress ?: continue
                val n = nif.name.lowercase()
                if (n.startsWith("wlan") || n.startsWith("ap") || n.startsWith("swlan")) { found = ip; break }
                if (fallback == null) fallback = ip
            }
            if (found != null) break
        }
        found ?: fallback
    } catch (_: Throwable) { null }

    // ── NSD (Android's own service discovery) — registered by the server, unregistered by its stop ────────────────

    @Volatile private var nsdListener: NsdManager.RegistrationListener? = null

    fun advertise(ctx: Context, name: String, port: Int, pinRequired: Boolean) {
        unadvertise(ctx)
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
            val l = object : NsdManager.RegistrationListener {
                override fun onServiceRegistered(i: NsdServiceInfo) {}
                override fun onRegistrationFailed(i: NsdServiceInfo, e: Int) { Log.w(TAG, "NSD registration failed: $e") }
                override fun onServiceUnregistered(i: NsdServiceInfo) {}
                override fun onUnregistrationFailed(i: NsdServiceInfo, e: Int) {}
            }
            m.registerService(info, NsdManager.PROTOCOL_DNS_SD, l)
            nsdListener = l
        } catch (t: Throwable) { Log.w(TAG, "advertise failed: ${t.message}") }
    }

    fun unadvertise(ctx: Context) {
        val l = nsdListener ?: return
        nsdListener = null
        try { (ctx.getSystemService(Context.NSD_SERVICE) as? NsdManager)?.unregisterService(l) } catch (_: Throwable) {}
    }
}
