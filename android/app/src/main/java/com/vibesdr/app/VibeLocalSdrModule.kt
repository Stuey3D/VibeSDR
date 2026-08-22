package com.vibesdr.app

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.util.Log
import androidx.core.content.ContextCompat
import org.json.JSONObject
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.WritableArray
import com.facebook.react.bridge.WritableMap

/**
 * VibeSDR V4 — local-SDR USB bridge (Android only).
 *
 * Owns the Android side of the V4 local-hardware path: enumerate attached
 * RTL-SDR dongles, run the USB permission dance, and on grant hand the
 * UsbDeviceConnection's file descriptor to the native shim
 * ([VibeLocalSDR.probeRtl]) which opens the device via librtlsdr.
 *
 * Stage 2 only enumerates + probes (logs device identity). Later stages start
 * the localhost UberSDR shim against the opened fd.
 */
class VibeLocalSdrModule(private val reactContext: ReactApplicationContext) :
    ReactContextBaseJavaModule(reactContext) {

    private val TAG = "VibeLocalSDR"
    private val ACTION_USB_PERMISSION = "com.vibesdr.app.USB_PERMISSION"

    private val usbManager: UsbManager?
        get() = reactContext.getSystemService(Context.USB_SERVICE) as? UsbManager

    override fun getName() = "VibeLocalSDR"

    private fun isRtlSdr(dev: UsbDevice): Boolean {
        val key = (dev.vendorId shl 16) or dev.productId
        return RTL_SDR_VIDPIDS.contains(key)
    }

    /** ★ An Airspy HF+ (Discovery / Dual Port). One VID/PID for the whole family — the models
     *  are told apart by serial, not by USB id. Kept beside isRtlSdr so the two allowlists that
     *  decide "is this a radio we can drive" stay visibly together; device_filter.xml is the
     *  third copy and must agree, or the app is offered for a device it then refuses. */
    private fun isAirspyHf(dev: UsbDevice): Boolean =
        dev.vendorId == AIRSPYHF_VID && dev.productId == AIRSPYHF_PID

    /** Any radio we can open directly over USB. */
    private fun isSupportedRadio(dev: UsbDevice): Boolean = isRtlSdr(dev) || isAirspyHf(dev)

    private fun describe(dev: UsbDevice, hasPermission: Boolean): WritableMap {
        val m = Arguments.createMap()
        m.putString("deviceName", dev.deviceName)
        m.putInt("vendorId", dev.vendorId)
        m.putInt("productId", dev.productId)
        m.putString("vendorIdHex", String.format("0x%04x", dev.vendorId))
        m.putString("productIdHex", String.format("0x%04x", dev.productId))
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP_MR1) {
            m.putString("productName", dev.productName ?: "")
            m.putString("manufacturerName", dev.manufacturerName ?: "")
        }
        // ★★★ SAY WHICH RADIO IT IS. describe() reported ids and a USB product string but never
        //   which driver we would actually open it with, so every caller had to assume — and the
        //   only caller assumed RTL, which is how an Airspy HF+ came up labelled "RTL-SDR".
        //   openAndProbe has reported `driver`/`model` for a while (see the radioCaps builder);
        //   this puts the same two facts on the LISTING, where the UI decides what to call it
        //   before anything is opened. Same family as [[else_means_dongle_trap]]: with two radios
        //   supported, "it is a device we know" no longer implies "it is a dongle".
        m.putString("kind", if (isAirspyHf(dev)) "airspyhf" else "rtl")
        m.putString("label", dev.productName ?: (if (isAirspyHf(dev)) "Airspy HF+" else "RTL-SDR"))
        m.putBoolean("hasPermission", hasPermission)
        return m
    }

    /** List attached RTL-SDR dongles (filtered by the known VID/PID allowlist). */
    @ReactMethod
    fun listDevices(promise: Promise) {
        val mgr = usbManager ?: run { promise.reject("no_usb", "USB service unavailable"); return }
        val out: WritableArray = Arguments.createArray()
        for ((_, dev) in mgr.deviceList) {
            if (!isSupportedRadio(dev)) continue
            out.pushMap(describe(dev, mgr.hasPermission(dev)))
        }
        promise.resolve(out)
    }

    private var pendingPromise: Promise? = null

    /**
     * Open the first attached RTL-SDR (requesting USB permission if needed) and
     * probe it via the native shim. Resolves with a description string, or
     * rejects on no-device / denied-permission / open failure.
     */
    @ReactMethod
    fun openAndProbe(promise: Promise) {
        val mgr = usbManager ?: run { promise.reject("no_usb", "USB service unavailable"); return }
        val dev = mgr.deviceList.values.firstOrNull { isSupportedRadio(it) }
            ?: run { promise.reject("no_device", "No SDR found"); return }

        if (mgr.hasPermission(dev)) {
            openAndProbe(mgr, dev, promise)
            return
        }

        if (pendingPromise != null) {
            promise.reject("busy", "A USB permission request is already in progress")
            return
        }
        pendingPromise = promise
        registerUsbReceiver()
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            PendingIntent.FLAG_MUTABLE else 0
        val intent = Intent(ACTION_USB_PERMISSION).setPackage(reactContext.packageName)
        val pi = PendingIntent.getBroadcast(reactContext, 0, intent, flags)
        Log.i(TAG, "requesting USB permission for $dev")
        mgr.requestPermission(dev, pi)
    }

    private fun openAndProbe(mgr: UsbManager, dev: UsbDevice, promise: Promise) {
        val conn = mgr.openDevice(dev)
            ?: run { promise.reject("open_failed", "openDevice returned null"); return }
        try {
            val fd = conn.fileDescriptor
            if (fd < 0) { promise.reject("bad_fd", "Invalid file descriptor"); return }
            val desc = VibeLocalSDR.probeRtl(fd, dev.vendorId, dev.productId)
            Log.i(TAG, "probe result: $desc")
            if (desc.startsWith("ERROR:")) promise.reject("probe_failed", desc)
            else promise.resolve(desc)
        } catch (e: Throwable) {
            promise.reject("probe_exception", e.message, e)
        } finally {
            conn.close()
        }
    }

    // ── Spectrum session (Stage 3) ────────────────────────────────────────
    // The UsbDeviceConnection must stay open for the whole session (the native
    // shim holds the fd), so we keep it here rather than close it after open.
    private var sessionConn: android.hardware.usb.UsbDeviceConnection? = null

    // Held only for the duration of an rtl_tcp CLIENT session (network IQ in).
    // The USB path needs no WiFi lock; the server path holds its own in the FGS.
    private val tcpWifiLock by lazy { VibeWifiLock(reactContext, "VibeSDR:RtlTcpClient") }

    // MULTICAST LOCK — mandatory for the mDNS responder. Android drops multicast packets
    // for apps that don't hold one, so without this the responder binds, joins the group
    // and then never sees a single query: "vibesdr.local" would simply never resolve.
    private var multicastLock: android.net.wifi.WifiManager.MulticastLock? = null

    private fun acquireMulticastLock() {
        if (multicastLock != null) return
        try {
            val wm = reactContext.applicationContext
                .getSystemService(Context.WIFI_SERVICE) as android.net.wifi.WifiManager
            multicastLock = wm.createMulticastLock("VibeSDR:mDNS").apply {
                setReferenceCounted(false)
                acquire()
            }
        } catch (t: Throwable) { Log.w(TAG, "multicast lock failed: ${t.message}") }
    }

    private fun releaseMulticastLock() {
        try { multicastLock?.let { if (it.isHeld) it.release() } } catch (_: Throwable) {}
        multicastLock = null
    }

    /** "VibeSDR: Moto G35" -> "vibesdr-moto-g35". A hostname can't carry spaces or
     *  punctuation, and a name the user typed is full of both. */
    private fun hostSlug(name: String): String {
        val s = name.lowercase()
            .replace(Regex("[^a-z0-9]+"), "-")
            .trim('-')
        return if (s.isEmpty()) "vibesdr" else s.take(32)
    }

    /**
     * Open the first attached RTL-SDR and start the local-SDR spectrum server.
     * Resolves with { port, wsBaseUrl } so JS can point UberSDRClient at
     * ws://127.0.0.1:<port>. Requests USB permission first if needed.
     */
    @ReactMethod
    fun startSpectrum(opts: com.facebook.react.bridge.ReadableMap, promise: Promise) {
        val mgr = usbManager ?: run { promise.reject("no_usb", "USB service unavailable"); return }
        val dev = mgr.deviceList.values.firstOrNull { isSupportedRadio(it) }
            ?: run { promise.reject("no_device", "No SDR found"); return }
        if (!mgr.hasPermission(dev)) {
            // Reuse the permission flow, then retry once granted.
            openAndProbeThen(mgr, dev, promise) { startSpectrumNow(mgr, dev, opts, promise) }
            return
        }
        startSpectrumNow(mgr, dev, opts, promise)
    }

    private fun startSpectrumNow(
        mgr: UsbManager, dev: UsbDevice,
        opts: com.facebook.react.bridge.ReadableMap, promise: Promise
    ) {
        // Learned bookmarks persist for LOCAL listening too — the shim learns whenever
        // it runs, not only when serving.
        VibeLocalSDR.setBookmarksPath(java.io.File(reactContext.filesDir, "vibe_bookmarks.json").absolutePath)
        stopSpectrumInternal()
        val conn = mgr.openDevice(dev)
            ?: run { promise.reject("open_failed", "openDevice returned null"); return }
        val fd = conn.fileDescriptor
        if (fd < 0) { conn.close(); promise.reject("bad_fd", "Invalid file descriptor"); return }
        sessionConn = conn

        val centerFreq = if (opts.hasKey("centerFreq")) opts.getDouble("centerFreq") else 100_000_000.0
        val sampleRate = if (opts.hasKey("sampleRate")) opts.getDouble("sampleRate") else 2_400_000.0
        val gain       = if (opts.hasKey("gainTenthDb")) opts.getInt("gainTenthDb") else -1 // auto
        val fftSize    = if (opts.hasKey("fftSize")) opts.getInt("fftSize") else 1024
        val fftRate    = if (opts.hasKey("fftRate")) opts.getDouble("fftRate") else 20.0
        val mode       = if (opts.hasKey("mode")) opts.getString("mode") ?: "nfm" else "nfm"

        val port = VibeLocalSDR.startSpectrum(
            fd, dev.vendorId, dev.productId, centerFreq, sampleRate, gain, fftSize, fftRate, mode)
        if (port <= 0) {
            conn.close(); sessionConn = null
            promise.reject("start_failed", "native startSpectrum failed (see logcat)")
            return
        }
        Log.i(TAG, "spectrum started on port $port")
        val res = Arguments.createMap()
        res.putInt("port", port)
        res.putString("wsBaseUrl", "http://127.0.0.1:$port")
        promise.resolve(res)
    }

    // RTL-TCP: connect to an rtl_tcp server (host:port) and run the same local
    // spectrum/audio shim against it — no USB, so this also works on iOS.
    @ReactMethod
    fun startTcp(opts: com.facebook.react.bridge.ReadableMap, promise: Promise) {
        val host = opts.getString("host") ?: run { promise.reject("no_host", "host required"); return }
        val port = if (opts.hasKey("port")) opts.getInt("port") else 1234
        stopSpectrumInternal()
        val centerFreq = if (opts.hasKey("centerFreq")) opts.getDouble("centerFreq") else 100_000_000.0
        val sampleRate = if (opts.hasKey("sampleRate")) opts.getDouble("sampleRate") else 2_400_000.0
        val gain       = if (opts.hasKey("gainTenthDb")) opts.getInt("gainTenthDb") else -1
        val fftSize    = if (opts.hasKey("fftSize")) opts.getInt("fftSize") else 1024
        val fftRate    = if (opts.hasKey("fftRate")) opts.getDouble("fftRate") else 20.0
        val mode       = if (opts.hasKey("mode")) opts.getString("mode") ?: "nfm" else "nfm"

        val bound = VibeLocalSDR.startTcp(host, port, centerFreq, sampleRate, gain, fftSize, fftRate, mode)
        if (bound <= 0) { promise.reject("start_failed", "could not connect to rtl_tcp $host:$port (see logcat)"); return }
        // Receiving a multi-Mbit IQ stream is as power-save-sensitive as serving one.
        tcpWifiLock.acquire()
        Log.i(TAG, "rtl_tcp $host:$port started on port $bound")
        val res = Arguments.createMap()
        res.putInt("port", bound)
        res.putString("wsBaseUrl", "http://127.0.0.1:$bound")
        promise.resolve(res)
    }

    // SpyServer: connect to a SpyServer-compatible server and run the same local
    // spectrum/audio shim against it — no USB, so this also works on iOS.
    @ReactMethod
    fun startSpyServer(opts: com.facebook.react.bridge.ReadableMap, promise: Promise) {
        val host = opts.getString("host") ?: run { promise.reject("no_host", "host required"); return }
        val port = if (opts.hasKey("port")) opts.getInt("port") else 5555
        stopSpectrumInternal()
        val centerFreq = if (opts.hasKey("centerFreq")) opts.getDouble("centerFreq") else 100_000_000.0
        val sampleRate = if (opts.hasKey("sampleRate")) opts.getDouble("sampleRate") else 2_400_000.0
        val gain       = if (opts.hasKey("gainTenthDb")) opts.getInt("gainTenthDb") else -1
        val fftSize    = if (opts.hasKey("fftSize")) opts.getInt("fftSize") else 1024
        val fftRate    = if (opts.hasKey("fftRate")) opts.getDouble("fftRate") else 20.0
        val mode       = if (opts.hasKey("mode")) opts.getString("mode") ?: "nfm" else "nfm"

        val bound = VibeLocalSDR.startSpyServer(host, port, centerFreq, sampleRate, gain, fftSize, fftRate, mode)
        if (bound <= 0) { promise.reject("start_failed", "could not connect to SpyServer $host:$port (see logcat)"); return }
        tcpWifiLock.acquire()
        Log.i(TAG, "SpyServer $host:$port started on port $bound")
        val res = Arguments.createMap()
        res.putInt("port", bound)
        res.putString("wsBaseUrl", "http://127.0.0.1:$bound")
        promise.resolve(res)
    }

    /** VibeServer: bind the shim's WS server to the LAN before starting a session. */
    @ReactMethod
    fun setServeOnLan(on: Boolean) { VibeLocalSDR.setServeOnLan(on) }

    // ── VibeServer (share this dongle with server-side DSP; compressed) ───────
    // Runs the SAME shim as a local session but LAN-bound and silent on this
    // phone: no local audio/spectrum client, so the single client slot goes to
    // the one remote VibeSDR. Config (pin/limits/compress) is applied before the
    // shim starts. Resolves { ip, port, name } for the sharing screen + mDNS.
    @ReactMethod
    fun startVibeServer(opts: com.facebook.react.bridge.ReadableMap, promise: Promise) {
        val mgr = usbManager ?: run { promise.reject("no_usb", "USB service unavailable"); return }
        val dev = mgr.deviceList.values.firstOrNull { isSupportedRadio(it) }
            ?: run { promise.reject("no_device", "No SDR found"); return }
        if (!mgr.hasPermission(dev)) {
            openAndProbeThen(mgr, dev, promise) { startVibeServerNow(mgr, dev, opts, promise) }
            return
        }
        startVibeServerNow(mgr, dev, opts, promise)
    }

    private fun startVibeServerNow(
        mgr: UsbManager, dev: UsbDevice,
        opts: com.facebook.react.bridge.ReadableMap, promise: Promise
    ) {
        stopSpectrumInternal()
        stopServerInternal()
        val conn = mgr.openDevice(dev)
            ?: run { promise.reject("open_failed", "openDevice returned null"); return }
        val fd = conn.fileDescriptor
        if (fd < 0) { conn.close(); promise.reject("bad_fd", "Invalid file descriptor"); return }
        sessionConn = conn

        // ★★★ THE CONFIG TRAVELS WHOLE. Every setting is read and applied by VibeServerBoot, which
        //     the CRASH-RESTORE path also uses — see that file for why there is no longer a second,
        //     hand-maintained list of "settings worth restoring". `toHashMap` gives plain
        //     JSON-shaped values, so the same object stores itself in SharedPreferences unchanged.
        val cfg = JSONObject(opts.toHashMap() as Map<*, *>)
        val name       = VibeServerBoot.name(cfg)
        val pin        = VibeServerBoot.pin(cfg)
        val advertiseOnStart = VibeServerBoot.advertise(cfg)
        // Rebuild the server if the process dies under it? Owner's choice: a shim that crashes
        // REPEATEDLY would otherwise crash-loop, re-opening the dongle each time.
        val autoRestore = VibeServerBoot.autoRestore(cfg)

        val port = VibeServerBoot.applyAndStart(cfg, fd, dev.vendorId, dev.productId,
                                                reactContext.filesDir)
        if (port <= 0) {
            VibeLocalSDR.setServeOnLan(false)
            conn.close(); sessionConn = null
            promise.reject("start_failed", "native startVibeServer failed (see logcat)")
            return
        }
        val ip = getLocalIp() ?: "0.0.0.0"
        // "<name>.local" in any browser on the network. The responder probes first and
        // renames itself (vibesdr-2, ...) if the name is already taken, so two phones
        // serving at once don't fight over one name.
        if (ip != "0.0.0.0") {
            acquireMulticastLock()
            VibeLocalSDR.startMdns(hostSlug(name), ip)
        }
        RtlTcpServerService.start(reactContext, name, ip, port, "vibeserver")
        // Remember the live config so the service can rebuild the shim if the process
        // dies under it (START_STICKY brings the service back, but not the radio).
        if (autoRestore) {
            VibeServerRestore.arm(reactContext, cfg)
        } else {
            VibeServerRestore.disarm(reactContext)
        }
        Log.i(TAG, "VibeServer started $ip:$port as \"$name\" (pin=${pin.isNotEmpty()})")

        val res = Arguments.createMap()
        res.putString("ip", ip)
        res.putInt("port", port)
        res.putString("name", name)
        promise.resolve(res)
    }

    @ReactMethod
    fun stopVibeServer(promise: Promise) {
        // DISARM FIRST. A deliberate stop must not be undone: without this the crash-
        // recovery path would happily resurrect a server the user had just switched off.
        VibeServerRestore.disarm(reactContext)
        VibeLocalSDR.stopMdns()
        releaseMulticastLock()
        RtlTcpServerService.stop(reactContext)
        stopSpectrumInternal()
        VibeLocalSDR.setServeOnLan(false)
        VibeLocalSDR.setVibeServerAuth("")   // clear the secret from process memory
        promise.resolve(null)
    }

    /** Live toggle: switch compressed audio on/off without restarting the server. */
    @ReactMethod
    fun setVibeServerCompressAudio(on: Boolean) { VibeLocalSDR.setVibeServerCompressAudio(on) }

    /** Live, no restart — the operator can lock the controls on a running server. */
    @ReactMethod
    fun setVibeServerAdminSecret(secret: String) { VibeLocalSDR.setVibeServerAdminSecret(secret) }

    @ReactMethod
    fun setVibeServerUncompressedAudio(mode: Double) {
        VibeLocalSDR.setVibeServerUncompressedAudio(mode.toInt())
    }

    /** ★★ WHICH RADIO IS PLUGGED IN, before anything is opened. The server settings screen
     *  runs BEFORE the shim exists, so it cannot ask radioCapsJson() — and without this it
     *  offered a dongle's 2.4 MHz to an Airspy HF+, which tops out near 912 kHz, so every rate
     *  on the menu was impossible (Stuart, 2026-07-27).
     *  ★ VID/PID only: no USB open, no permission prompt, and nothing for the user to approve
     *  just to draw a menu. The radio's exact rate list still comes from the radio once it is
     *  running — this decides which menu to show, not what the hardware will accept. */
    @ReactMethod
    fun getConnectedRadio(promise: Promise) {
        val mgr = usbManager ?: run { promise.resolve(null); return }
        val dev = mgr.deviceList.values.firstOrNull { isSupportedRadio(it) }
            ?: run { promise.resolve(null); return }
        val res = Arguments.createMap()
        res.putString("driver", if (isAirspyHf(dev)) "airspyhf" else "rtl")
        res.putString("model", dev.productName ?: (if (isAirspyHf(dev)) "Airspy HF+" else "RTL-SDR"))
        promise.resolve(res)
    }

    @ReactMethod
    fun setVibeServerSessionLimit(minutes: Double) {
        VibeLocalSDR.setVibeServerSessionLimit(minutes.toInt())
    }

    /** Hand the web client's search its station list (JSON array), served at
     *  GET /stations. The app owns the EiBi download + cache; the browser can't
     *  fetch eibispace.de itself (no CORS headers there), and this also means the
     *  search still works with no internet — the allotment case. */
    @ReactMethod
    fun setStationsJson(json: String) {
        VibeLocalSDR.setStationsJson(json)
        VibeServerRestore.cacheStations(reactContext, json)
    }

    /** Publish the RECEIVER's coarse location (GET /location). Clients compute
     *  spot distances, map centring and the ITU region from the ANTENNA's
     *  position — not from wherever the listener happens to be. */
    @ReactMethod
    fun setLocationJson(json: String) {
        VibeLocalSDR.setLocationJson(json)
        VibeServerRestore.cacheLocation(reactContext, json)
    }

    /** Learned station bookmarks (RDS). The shim learns them; JS persists them. */
    @ReactMethod
    fun setBookmarksJson(json: String) { VibeLocalSDR.setBookmarksJson(json) }

    @ReactMethod
    fun getBookmarksJson(promise: Promise) {
        promise.resolve(VibeLocalSDR.getBookmarksJson())
    }

    @ReactMethod
    fun clearBookmarks(promise: Promise) {
        VibeLocalSDR.clearBookmarks()
        promise.resolve(null)
    }

    /** The .local hostname the responder actually took — it renames itself on a clash,
     *  so this is not necessarily the one we asked for. */
    @ReactMethod
    fun getMdnsHostname(promise: Promise) {
        promise.resolve(VibeLocalSDR.mdnsHostname())
    }

    @ReactMethod
    // ── Process CPU, for the server screen ────────────────────────────────────
    //
    // A phone acting as a receiver is a device you leave running on a shelf for hours, so
    // "what is this costing me" is a fair question to be able to answer on the screen rather
    // than by guessing from how warm it feels. Read from /proc/self/stat, which needs no
    // permission and no NDK call.
    //
    // Reported as a percentage of ONE core (so >100% is possible and meaningful on a
    // multi-core phone) — the same convention the DSP benchmarks use, which keeps it
    // comparable with the Pi figures.
    // SELF-TIMED AND CACHED, so any number of callers is safe. A naive per-call delta breaks
    // the moment a second poller appears: the two interleave and each measures the other's
    // gap. This recomputes at most every 500ms and hands everyone the same current reading.
    private var lastCpuTicks = 0L
    private var lastCpuAt = 0L
    private var cpuValue = 0.0
    private val clkTck: Long by lazy {
        try { android.system.Os.sysconf(android.system.OsConstants._SC_CLK_TCK) } catch (_: Throwable) { 100L }
    }
    @Synchronized
    private fun processCpuPercent(): Double = try {
        val now = android.os.SystemClock.elapsedRealtime()
        if (lastCpuAt > 0L && now - lastCpuAt < 500L) cpuValue      // too soon — reuse
        else {
            val stat = java.io.File("/proc/self/stat").readText()
            // Fields 1-2 are pid and (comm); comm can itself contain spaces and brackets, so
            // split AFTER the last ')' or a process named "foo bar" shifts every index.
            val f = stat.substring(stat.lastIndexOf(')') + 2).split(" ")
            val ticks = f[11].toLong() + f[12].toLong()             // utime + stime (fields 14, 15)
            if (lastCpuAt > 0L) {
                cpuValue = (((ticks - lastCpuTicks).toDouble() / clkTck) / ((now - lastCpuAt) / 1000.0) * 100.0)
                    .coerceIn(0.0, 100.0 * Runtime.getRuntime().availableProcessors())
            }
            lastCpuTicks = ticks; lastCpuAt = now
            cpuValue                                                // first call has no delta yet: 0
        }
    } catch (_: Throwable) { 0.0 }                                  // never break a status for a stat read

    /** ★★★ THE ANNOTATION IS THE WHOLE FEATURE. Without @ReactMethod this is not exported to JS at
     *  all — and the call site reads `Local?.getVibeServerStatus?.()`, whose optional call returns
     *  UNDEFINED for a missing method rather than throwing. So the screen asked for the status
     *  forever, got nothing, threw no error and logged nothing, and sat on "Waiting for a client…"
     *  while the server it was describing had a listener and its own admin page showed every
     *  detail correctly. Two days of hunting (2026-08-19/20), and the fault was one missing line.
     *  ★★ It also explains the shape that made it so hard: the SERVER was provably fine from every
     *     other direction, because every other direction reads the shim over HTTP. Only the app
     *     reads it through this bridge. */
    @ReactMethod
    fun getVibeServerStatus(promise: Promise) {
        try {
            val o = org.json.JSONObject(VibeLocalSDR.getVibeServerStatus())
            val m = Arguments.createMap()
            m.putBoolean("running", o.optBoolean("running", false))
            m.putBoolean("client", o.optBoolean("client", false))
            m.putString("clientAddr", o.optString("clientAddr", ""))
            m.putDouble("specBytesPerSec", o.optLong("specBytesPerSec", 0).toDouble())
            m.putDouble("audioBytesPerSec", o.optLong("audioBytesPerSec", 0).toDouble())
            m.putBoolean("compressed", o.optBoolean("compressed", true))
            m.putBoolean("pinEnabled", o.optBoolean("pinEnabled", false))
            m.putDouble("fftRate", o.optLong("fftRate", 0).toDouble())
            m.putDouble("bandwidthHz", o.optLong("bandwidthHz", 0).toDouble())
            // NB: this map is built field-by-field, so a new field in the C++ JSON
            // is silently DROPPED here until it's added. That's why SAMPLE RATE
            // showed "—" despite the native side emitting it.
            m.putDouble("sampleRate", o.optLong("sampleRate", 0).toDouble())
            m.putInt("port", o.optInt("port", 0))
            // ★ The listener COUNT, from the shim's one authoritative counter. See the note on
            //   clientConnected in getVibeServerStatus() — the screen used to have its own idea.
            m.putInt("listeners", o.optInt("listeners", 0))
            m.putInt("maxUsers", o.optInt("maxUsers", 1))
            // ★★★ THE DECORATIONS MUST NOT BE ABLE TO KILL THE READING. Everything above comes
            //     from the shim's own JSON and is the ANSWER; everything in this block is a nicety
            //     read from the phone — the local address, a CPU percentage out of /proc, the core
            //     count. They were inside the same try as the rest, so ONE of them throwing
            //     rejected the WHOLE promise: getVibeServerStatus() returned null, `if (s)
            //     setStatus(s)` never fired, and the screen sat on "Waiting for a client…" while
            //     the server had a listener and the admin page showed it correctly (Stuart,
            //     2026-08-19, on a Unisoc Moto — exactly the sort of device where reading /proc is
            //     restricted).
            // ★★ Same shape as the comment above about a field being silently dropped: this
            //    screen's job is to say what the server is doing, and it must degrade to "I could
            //    not measure the CPU" rather than "there is nothing here".
            try { m.putString("ip", if (o.optBoolean("running", false)) (getLocalIp() ?: "") else "") }
            catch (e: Throwable) { m.putString("ip", "") }
            try { m.putDouble("cpu", processCpuPercent()) }
            catch (e: Throwable) { m.putDouble("cpu", 0.0) }
            try { m.putInt("cores", Runtime.getRuntime().availableProcessors()) }
            catch (e: Throwable) { m.putInt("cores", 0) }
            promise.resolve(m)
        } catch (e: Throwable) {
            // ★ And say WHY in the log. A rejected status is invisible on the screen — it looks
            //   exactly like a server with nobody on it, which is the fault this cost us.
            Log.w(TAG, "getVibeServerStatus failed: ${e}")
            promise.reject("status_failed", e.message)
        }
    }

    @ReactMethod
    fun stopSpectrum(promise: Promise) {
        // ★ Off the bridge thread: the teardown is synchronous now (it must be — see
        // stopSpectrumInternal) and can take a moment on a USB cancel plus thread joins.
        // Leaving a local session must never freeze the UI.
        Thread {
            stopSpectrumInternal()
            promise.resolve(null)
        }.start()
    }

    // ── RTL-TCP SERVER (share this device's USB dongle over the network) ──────
    // Kept separate from the spectrum session's UsbDeviceConnection: the two
    // modes are mutually exclusive, but a distinct handle keeps teardown clean.
    private var serverConn: android.hardware.usb.UsbDeviceConnection? = null

    /**
     * Open the attached RTL-SDR and start the RTL-TCP server. Resolves with
     * { ip, port, name } so JS can advertise it via mDNS and show the address.
     * opts: name, port?(1234), sampleRate?(2.4M), gainTenthDb?(-1 auto),
     *       centerFreq?(100M), overrideRate?(0 = client-controlled).
     */
    @ReactMethod
    fun startRtlTcpServer(opts: com.facebook.react.bridge.ReadableMap, promise: Promise) {
        val mgr = usbManager ?: run { promise.reject("no_usb", "USB service unavailable"); return }
        val dev = mgr.deviceList.values.firstOrNull { isSupportedRadio(it) }
            ?: run { promise.reject("no_device", "No SDR found"); return }
        if (!mgr.hasPermission(dev)) {
            openAndProbeThen(mgr, dev, promise) { startRtlTcpServerNow(mgr, dev, opts, promise) }
            return
        }
        startRtlTcpServerNow(mgr, dev, opts, promise)
    }

    private fun startRtlTcpServerNow(
        mgr: UsbManager, dev: UsbDevice,
        opts: com.facebook.react.bridge.ReadableMap, promise: Promise
    ) {
        // Free any on-device session + prior server first.
        stopSpectrumInternal()
        stopServerInternal()
        val conn = mgr.openDevice(dev)
            ?: run { promise.reject("open_failed", "openDevice returned null"); return }
        val fd = conn.fileDescriptor
        if (fd < 0) { conn.close(); promise.reject("bad_fd", "Invalid file descriptor"); return }
        serverConn = conn

        val name       = if (opts.hasKey("name")) opts.getString("name") ?: "VibeSDR RTL-SDR" else "VibeSDR RTL-SDR"
        val port       = if (opts.hasKey("port")) opts.getInt("port") else 1234
        val sampleRate = if (opts.hasKey("sampleRate")) opts.getDouble("sampleRate") else 2_400_000.0
        val gain       = if (opts.hasKey("gainTenthDb")) opts.getInt("gainTenthDb") else -1
        val centerFreq = if (opts.hasKey("centerFreq")) opts.getDouble("centerFreq") else 100_000_000.0
        val override   = if (opts.hasKey("overrideRate")) opts.getDouble("overrideRate") else 0.0

        val bound = VibeLocalSDR.startServer(
            fd, dev.vendorId, dev.productId, sampleRate, centerFreq, gain, port, override)
        if (bound <= 0) {
            conn.close(); serverConn = null
            promise.reject("start_failed", "native startServer failed (see logcat)")
            return
        }
        val ip = getLocalIp() ?: "0.0.0.0"
        RtlTcpServerService.start(reactContext, name, ip, bound)
        Log.i(TAG, "RTL-TCP server started $ip:$bound as \"$name\"")
        val res = Arguments.createMap()
        res.putString("ip", ip)
        res.putInt("port", bound)
        res.putString("name", name)
        promise.resolve(res)
    }

    @ReactMethod
    fun stopRtlTcpServer(promise: Promise) {
        stopServerInternal()
        promise.resolve(null)
    }

    @ReactMethod
    fun setServerSampleRate(rate: Double) { VibeLocalSDR.setServerSampleRate(rate) }

    @ReactMethod
    fun getServerStatus(promise: Promise) {
        try {
            val json = VibeLocalSDR.getServerStatus()
            val o = org.json.JSONObject(json)
            val m = Arguments.createMap()
            m.putBoolean("running", o.optBoolean("running", false))
            m.putBoolean("client", o.optBoolean("client", false))
            m.putString("clientAddr", o.optString("clientAddr", ""))
            m.putDouble("sampleRate", o.optLong("sampleRate", 0).toDouble())
            m.putDouble("overrideRate", o.optLong("overrideRate", 0).toDouble())
            m.putDouble("droppedBytes", o.optLong("droppedBytes", 0).toDouble())
            // ip/port let the UI re-adopt a server that is ALREADY running (persist
            // mode: the server outlives the screen, so the screen must attach to it
            // rather than start a second one on the same dongle).
            m.putInt("port", o.optInt("port", 0))
            m.putString("ip", if (o.optBoolean("running", false)) (getLocalIp() ?: "") else "")
            m.putDouble("cpu", processCpuPercent())
            m.putInt("cores", Runtime.getRuntime().availableProcessors())
            promise.resolve(m)
        } catch (e: Throwable) {
            promise.reject("status_failed", e.message)
        }
    }

    /** rtl_tcp CLIENT link health — drives the connection meter on the network path. */
    @ReactMethod
    fun getNetStatus(promise: Promise) {
        try {
            val o = org.json.JSONObject(VibeLocalSDR.getNetStatus())
            val m = Arguments.createMap()
            m.putBoolean("tcp", o.optBoolean("tcp", false))
            m.putDouble("stalls", o.optLong("stalls", 0).toDouble())
            m.putDouble("droppedSamples", o.optLong("droppedSamples", 0).toDouble())
            m.putDouble("bufferedMs", o.optLong("bufferedMs", 0).toDouble())
            m.putBoolean("spy", o.optBoolean("spy", false))
            m.putBoolean("canControl", o.optBoolean("canControl", true))
            m.putBoolean("closed", o.optBoolean("closed", false))
            promise.resolve(m)
        } catch (e: Throwable) {
            promise.reject("net_status_failed", e.message)
        }
    }

    private fun stopServerInternal() {
        try { VibeLocalSDR.stopServer() } catch (_: Throwable) {}
        try { RtlTcpServerService.stop(reactContext) } catch (_: Throwable) {}
        serverConn?.let { try { it.close() } catch (_: Exception) {} }
        serverConn = null
    }

    /** Best-effort non-loopback IPv4 for the LAN (prefer wlan/AP interfaces). */
    private fun getLocalIp(): String? {
        return try {
            val ifaces = java.net.NetworkInterface.getNetworkInterfaces()
            var fallback: String? = null
            for (nif in ifaces) {
                if (!nif.isUp || nif.isLoopback) continue
                for (addr in nif.inetAddresses) {
                    if (addr.isLoopbackAddress || addr !is java.net.Inet4Address) continue
                    val ip = addr.hostAddress ?: continue
                    val n = nif.name.lowercase()
                    if (n.startsWith("wlan") || n.startsWith("ap") || n.startsWith("swlan")) return ip
                    if (fallback == null) fallback = ip
                }
            }
            fallback
        } catch (_: Throwable) { null }
    }

    // ── Hardware controls ──────────────────────────────────────────────────
    @ReactMethod fun setGain(gainTenthDb: Double) { VibeLocalSDR.setGain(gainTenthDb.toInt()) }
    @ReactMethod fun setPpm(ppm: Double) { VibeLocalSDR.setPpm(ppm.toInt()) }
    @ReactMethod fun setBiasTee(on: Boolean) { VibeLocalSDR.setBiasTee(on) }
    @ReactMethod fun setAgc(on: Boolean) { VibeLocalSDR.setAgc(on) }
    @ReactMethod fun setDirectSampling(mode: Double) { VibeLocalSDR.setDirectSampling(mode.toInt()) }
    @ReactMethod fun setSampleRate(rate: Double) { VibeLocalSDR.setSampleRate(rate) }
    @ReactMethod fun setDeemphasis(tau: Double) { VibeLocalSDR.setDeemphasis(tau) }
    @ReactMethod fun setSquelch(on: Boolean, db: Double) { VibeLocalSDR.setSquelch(on, db.toFloat()) }
    @ReactMethod fun setNR(on: Boolean) { VibeLocalSDR.setNR(on) }
    @ReactMethod fun setNotch(on: Boolean) { VibeLocalSDR.setNotch(on) }
    @ReactMethod fun setStereoEnabled(on: Boolean) { VibeLocalSDR.setStereoEnabled(on) }
    @ReactMethod fun setNrStrength(s: Double) { VibeLocalSDR.setNrStrength(s.toFloat()) }
    /**
     * ★★★ THE BAND LIST, FROM THE SERVER THAT ENFORCES IT. The server screen needs the same bands
     *     the browser's setup page offers, and vibe_bands.h is the only list that matters — it is
     *     what actually resolves "fm" or "airband" when a limit is applied. A copy in TypeScript
     *     would drift, and the copy that drifts is the one that quietly stops matching.
     *  ★★★ AND IT EXISTED ALREADY, UNEXPORTED. VibeLocalSDR.bandsJson() has been there all along
     *      with no @ReactMethod, so JS could not reach it — the same shape as getVibeServerStatus,
     *      where `Local?.x?.()` returns UNDEFINED rather than throwing and cost two days of looking
     *      at a server that was fine (2026-08-20). Exported now, and by name.
     */
    @ReactMethod fun getBands(promise: Promise) {
        try { promise.resolve(VibeLocalSDR.bandsJson()) }
        catch (t: Throwable) { promise.reject("bands", t) }
    }

    @ReactMethod fun startDecoderService(promise: Promise) { promise.resolve(VibeLocalSDR.startDecoderService()) }
    @ReactMethod fun feedDecoderPcm(b64: String, rate: Double) { VibeLocalSDR.feedDecoderPcm(b64, rate.toInt()) }
    @ReactMethod fun setDecoderFreq(hz: Double) { VibeLocalSDR.setDecoderFreq(hz) }
    @ReactMethod fun stopDecoderService() { VibeLocalSDR.stopSpectrum() }
    @ReactMethod fun getNrCpu(promise: Promise) { promise.resolve(VibeLocalSDR.getNrCpu().toDouble()) }

    /** Supported tuner gains in tenths of dB (e.g. 207 = 20.7 dB). */
    @ReactMethod
    fun getTunerGains(promise: Promise) {
        val gains = VibeLocalSDR.getTunerGains()
        val arr = Arguments.createArray()
        for (g in gains) arr.pushInt(g)
        promise.resolve(arr)
    }

    /** True (once) if this launch/resume was triggered by plugging in a matching
     *  RTL-SDR dongle (USB_DEVICE_ATTACHED). Reading it CLEARS it, so it fires the
     *  Local-Hardware auto-connect exactly once per attach. JS calls this on the
     *  instance picker to route straight to Local Hardware instead of the default
     *  instance / picker (bug: USB attach used to land on the default). */
    @ReactMethod
    fun consumeUsbLaunch(promise: Promise) {
        val pending = MainActivity.usbLaunchPending
        MainActivity.usbLaunchPending = false
        promise.resolve(pending)
    }

    /** True if the OS has this app "background restricted" (the user — or an
     *  aggressive OEM like Motorola/Lenovo by default — set it to "Restricted").
     *  When restricted, Android strips our mediaPlayback foreground service in the
     *  background (isForeground=false → cached process → /background cpuset →
     *  little cores only), which starves the local-SDR DSP/audio threads and
     *  breaks up background audio. Samsung etc. default to unrestricted and are
     *  fine. JS uses this to prompt the user toward the Unrestricted setting. */
    @ReactMethod
    fun isBackgroundRestricted(promise: Promise) {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                val am = reactContext.getSystemService(Context.ACTIVITY_SERVICE) as android.app.ActivityManager
                promise.resolve(am.isBackgroundRestricted)
            } else {
                promise.resolve(false)
            }
        } catch (_: Throwable) { promise.resolve(false) }
    }

    /** Open this app's system settings page (App info), where the user can set
     *  battery usage to Unrestricted / allow background. We do NOT force-quit the
     *  app afterwards: an abnormal self-termination can feed an OEM's "this app
     *  misbehaves, restrict it" heuristic (the very thing we're trying to undo),
     *  and the mediaPlayback FGS re-grants itself once the app is unrestricted and
     *  returns to the foreground anyway. The prompt tells the user to swipe the app
     *  away from recents and reopen it, which is the clean way to pick up the new
     *  setting. */
    @ReactMethod
    fun openAppSettings() {
        try {
            val intent = Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
                .setData(android.net.Uri.fromParts("package", reactContext.packageName, null))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            reactContext.startActivity(intent)
        } catch (_: Throwable) {}
    }

    private fun stopSpectrumInternal() {
        // ★★★ SYNCHRONOUS, AND THAT IS THE POINT. This used to fire the native teardown at a
        // DETACHED THREAD and return immediately — then closed the UsbDeviceConnection on the
        // next line, pulling the file descriptor out from under libusb while it was still
        // closing the radio.
        // ★ Fatal on the Airspy, whose handle is a WRAPPED fd: libusb_close aborted with
        // "pthread_mutex_lock called on a destroyed mutex" every time the user backed out of
        // VibeServer (Stuart, 2026-07-27). The dongle had the same race and was getting away
        // with it, which is why this survived so long.
        // ★ It also fixes a second race nobody had noticed: startVibeServerNow() calls this and
        // then immediately REOPENS the device, so an async stop put the old close and the new
        // open in a straight race.
        // ★ The cost is that teardown now blocks its caller. The user-facing @ReactMethod hands
        // this to a background thread so the bridge never stalls; the internal callers WANT to
        // wait, because every one of them is about to touch the device again.
        try { VibeLocalSDR.stopSpectrumSync() } catch (_: Throwable) {}
        tcpWifiLock.release()
        sessionConn?.let { try { it.close() } catch (_: Exception) {} }
        sessionConn = null
    }

    // Like openAndProbe's permission path, but runs [onGranted] instead of probing.
    private var grantedAction: (() -> Unit)? = null
    private fun openAndProbeThen(mgr: UsbManager, dev: UsbDevice, promise: Promise, onGranted: () -> Unit) {
        if (pendingPromise != null) { promise.reject("busy", "USB permission request in progress"); return }
        pendingPromise = promise
        grantedAction = onGranted
        registerUsbReceiver()
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
        val intent = Intent(ACTION_USB_PERMISSION).setPackage(reactContext.packageName)
        mgr.requestPermission(dev, PendingIntent.getBroadcast(reactContext, 0, intent, flags))
    }

    private var receiver: BroadcastReceiver? = null

    private fun registerUsbReceiver() {
        if (receiver != null) return
        val r = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                if (intent.action != ACTION_USB_PERMISSION) return
                val promise = pendingPromise
                pendingPromise = null
                unregisterUsbReceiver()
                val mgr = usbManager
                @Suppress("DEPRECATION")
                val dev = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE)
                val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                val action = grantedAction
                grantedAction = null
                if (promise == null) return
                if (!granted || dev == null || mgr == null) {
                    promise.reject("permission_denied", "USB permission denied")
                    return
                }
                if (action != null) action() else openAndProbe(mgr, dev, promise)
            }
        }
        receiver = r
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        ContextCompat.registerReceiver(
            reactContext, r, filter, ContextCompat.RECEIVER_NOT_EXPORTED
        )
    }

    private fun unregisterUsbReceiver() {
        receiver?.let {
            try { reactContext.unregisterReceiver(it) } catch (_: Exception) {}
        }
        receiver = null
    }

    override fun invalidate() {
        unregisterUsbReceiver()
        pendingPromise = null
        stopSpectrumInternal()
        stopServerInternal()
        super.invalidate()
    }

    // ── ADVERTISE ON VIBESDR.NET ──────────────────────────────────────────────────────────────
    // ★★★ EVERY ONE OF THESE NEEDS ITS @ReactMethod. See getVibeServerStatus above: without the
    //     annotation the method simply is not exported, and `Local?.foo?.()` returns UNDEFINED
    //     rather than throwing — two days were lost to exactly that, twice.

    /** ★ Is the tunnel binary in this build at all? arm64 only — the switch must be ABSENT, not
     *  inert, where it cannot work (AGENTS.md). */
    @ReactMethod
    fun tunnelSupported(promise: Promise) {
        try { promise.resolve(VibeTunnel.isSupported(reactApplicationContext)) }
        catch (t: Throwable) { promise.resolve(false) }
    }

    @ReactMethod
    fun tunnelStatus(promise: Promise) {
        try { promise.resolve(VibeTunnel.statusJson()) }
        catch (t: Throwable) { promise.reject("tunnel_status", t) }
    }

    /**
     * Start the tunnel, then list the server.
     *
     * ★★★ THE TRUSTED-PROXY WIRING IS PART OF THIS ACTION, NOT A SETTING TO REMEMBER. cloudflared
     *     dials in from loopback, so without it every listener reads as 127.0.0.1 and the session
     *     limit, IP cooldown and one-address rule silently stop applying — which is precisely what
     *     happened to the demo on 2026-08-09.
     */
    @ReactMethod
    fun tunnelStart(name: String, locator: String, port: Int, ownerProxies: String,
                    radioModel: String, radioDriver: String, promise: Promise) {
        try {
            VibeTunnel.applyLoopbackTrust(true, ownerProxies)
            VibeTunnel.startTunnel(reactApplicationContext, port) { url ->
                if (url == null) { promise.resolve(VibeTunnel.statusJson()); return@startTunnel }
                VibeTunnel.publish(reactApplicationContext, name, locator, port, radioModel, radioDriver)
                promise.resolve(VibeTunnel.statusJson())
            }
        } catch (t: Throwable) { promise.reject("tunnel_start", t) }
    }

    /** ★ Off means OFF: delist immediately so the public address is freed now, not at expiry. */
    @ReactMethod
    fun tunnelStop(ownerProxies: String, promise: Promise) {
        try {
            VibeTunnel.delist(reactApplicationContext)
            VibeTunnel.stopTunnel()
            VibeTunnel.applyLoopbackTrust(false, ownerProxies)
            promise.resolve(VibeTunnel.statusJson())
        } catch (t: Throwable) { promise.reject("tunnel_stop", t) }
    }

    companion object {
        // RTL-SDR VID/PID allowlist (from SDR++ Brown), packed as (vid<<16)|pid.
        // internal, not private: VibeServerRestore matches on the SAME list — a copy
        // would drift and quietly stop recognising a dongle on the restore path only.
        internal val RTL_SDR_VIDPIDS: Set<Int> = listOf(
            0x0bda to 0x2832, 0x0bda to 0x2838, 0x0413 to 0x6680, 0x0413 to 0x6f0f,
            0x0458 to 0x707f, 0x0ccd to 0x00a9, 0x0ccd to 0x00b3, 0x0ccd to 0x00b4,
            0x0ccd to 0x00b5, 0x0ccd to 0x00b7, 0x0ccd to 0x00b8, 0x0ccd to 0x00b9,
            0x0ccd to 0x00c0, 0x0ccd to 0x00c6, 0x0ccd to 0x00d3, 0x0ccd to 0x00d7,
            0x0ccd to 0x00e0, 0x1554 to 0x5020, 0x15f4 to 0x0131, 0x15f4 to 0x0133,
            0x185b to 0x0620, 0x185b to 0x0650, 0x185b to 0x0680, 0x1b80 to 0xd393,
            0x1b80 to 0xd394, 0x1b80 to 0xd395, 0x1b80 to 0xd397, 0x1b80 to 0xd398,
            0x1b80 to 0xd39d, 0x1b80 to 0xd3a4, 0x1b80 to 0xd3a8, 0x1b80 to 0xd3af,
            0x1b80 to 0xd3b0, 0x1d19 to 0x1101, 0x1d19 to 0x1102, 0x1d19 to 0x1103,
            0x1d19 to 0x1104, 0x1f4d to 0xa803, 0x1f4d to 0xb803, 0x1f4d to 0xc803,
            0x1f4d to 0xd286, 0x1f4d to 0xd803
        ).map { (vid, pid) -> (vid shl 16) or pid }.toSet()

        /** Airspy HF+ — see isAirspyHf(). Mirrored in res/xml/device_filter.xml (DECIMAL there). */
        internal const val AIRSPYHF_VID = 0x03eb
        internal const val AIRSPYHF_PID = 0x800c
    }
}
