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
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.json.JSONObject
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.LifecycleEventListener
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.ReadableMap
import com.facebook.react.bridge.ReadableType
import com.facebook.react.bridge.WritableArray
import com.facebook.react.bridge.WritableMap


/** Is any attached input device a touchscreen? (InputDevice.SOURCE_TOUCHSCREEN; a touchpad or an air mouse is not.) */
fun hasTouchscreenDevice(): Boolean = try {
    android.view.InputDevice.getDeviceIds().any { id ->
        val d = android.view.InputDevice.getDevice(id)
        d != null && (d.sources and android.view.InputDevice.SOURCE_TOUCHSCREEN) == android.view.InputDevice.SOURCE_TOUCHSCREEN
    }
} catch (_: Throwable) { true }   // ★ unknown = assume a touchscreen: today's behaviour

/**
 * ★★★ IS THIS DEVICE DRIVEN BY A REMOTE? ONE RULE, read by TvTextInput (JS, via the noTouchscreen/isTv constants)
 * AND by Lite's native TV navigation (TvTextInputManager.isTv → TvNav). They disagreed on Kiko's BTV B9 (2026-10-04):
 * the text fields were fixed but the row bar and highlight never appeared, because Lite asked only for the system's
 * television UI MODE. A TV in UI mode, a device that DECLARES itself a TV (television / leanback), or one with no
 * touchscreen — by feature or, on boxes that claim one they do not have, by the attached input devices.
 */
fun remoteDriven(ctx: android.content.Context): Boolean {
    val pm = ctx.packageManager
    val tvMode = (ctx.getSystemService(android.content.Context.UI_MODE_SERVICE) as? android.app.UiModeManager)
        ?.currentModeType == android.content.res.Configuration.UI_MODE_TYPE_TELEVISION
    return tvMode || pm.hasSystemFeature("android.hardware.type.television")
        || pm.hasSystemFeature("android.software.leanback_only")
        || !pm.hasSystemFeature("android.hardware.touchscreen") || !hasTouchscreenDevice()
}

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
    // ★ Prefixed with the INSTALLED package (2026-10-04): VibeSDR and VibeSDR (legacy) can be installed side by side,
    //   and an app-private action must be that app's own. The broadcast was already setPackage-scoped.
    private val ACTION_USB_PERMISSION get() = "${reactContext.packageName}.USB_PERMISSION"

    private val usbManager: UsbManager?
        get() = reactContext.getSystemService(Context.USB_SERVICE) as? UsbManager

    override fun getName() = "VibeLocalSDR"

    /** ★ isLite: the shared ServerModeScreen shows Lite-only options (the DAB label-scan switch) when the
     *  package is VibeServer Lite. Read synchronously as NativeModules.VibeLocalSDR.isLite. */
    override fun getConstants(): Map<String, Any> = mapOf(
        "isLite" to reactContext.packageName.endsWith(".serverlite"),
        // ★ A television (the OS's own device type — its battery service is not to be believed; see
        //   VibeServerBoot.startBatteryMonitor). The server screen uses it for TV-appropriate defaults.
        "isTv" to (reactContext.packageManager.hasSystemFeature("android.hardware.type.television")
                   || reactContext.packageManager.hasSystemFeature("android.software.leanback_only")),
        // ★ No touchscreen at all — a TV box driven by a remote or a keyboard that may not report the TV UI mode
        //   (Kiko, 2026-10-03: an Android TV box where Tab could not leave a text field). TvTextInput reads it.
        // ★★ …OR NO TOUCHSCREEN ATTACHED (Kiko, 2026-10-04, BTV B9 / Amlogic S905X, Android 6, telemetry dump): cheap
        //    boxes DECLARE the touchscreen feature and run a phone launcher, so neither test above fired and the setup
        //    screen still trapped the remote in a text field. Its real inputs were aml_keypad, gpio_keypad and
        //    cec_input — no touchscreen among them. Ask the hardware: a phone or tablet always has one attached.
        "noTouchscreen" to (!reactContext.packageManager.hasSystemFeature("android.hardware.touchscreen")
                            || !hasTouchscreenDevice()),
        // ★ The legacy com.vibesdr.app build (BRIEF-android-package-migration): it shows the "new home" notice.
        "isLegacyPackage" to BuildConfig.IS_LEGACY_PACKAGE,
        // ★ "Start automatically when power returns" is offered only where Android may allow it — VibeBootStart.
        "startOnPowerSupported" to VibeBootStart.supported())

    /** ★ The owner's "Start automatically when power returns" switch, stored natively because the boot
     *  receiver reads it with no JS running. See VibeBootStart. */
    @ReactMethod(isBlockingSynchronousMethod = true)
    fun getStartOnPower(): Boolean = VibeBootStart.isEnabled(reactContext)

    /**
     * ★★ FULL SCREEN IN LANDSCAPE (2026-10-03). Android's 3-button navigation bar sits down the RIGHT edge in
     *    landscape (~64 dp), and the radio screen drew its zoom drum and the right-hand cards underneath it —
     *    found on an emulator at the SE's 320 × 568 dp, which is a 480p Android phone exactly. iOS hides its
     *    status bar in landscape on its own; this does the same here: system bars hidden, a swipe from the edge
     *    shows them for a moment. SDRScreen calls it on entering landscape and turns it off on the way out.
     */
    @ReactMethod
    fun setImmersive(on: Boolean) {
        val act = reactContext.currentActivity ?: return
        act.runOnUiThread {
            try {
                val c = WindowCompat.getInsetsController(act.window, act.window.decorView)
                if (on) {
                    c.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                    c.hide(WindowInsetsCompat.Type.systemBars())
                } else {
                    c.show(WindowInsetsCompat.Type.systemBars())
                }
            } catch (t: Throwable) { Log.w("VibeLocalSDR", "setImmersive: ${t.message}") }
        }
    }

    @ReactMethod
    fun setStartOnPower(on: Boolean) { VibeBootStart.setEnabled(reactContext, on) }

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

    /** ★ A HackRF One — EXPERIMENTAL. Only the One: the Jawbreaker and rad1o prototypes have
     *  their own USB ids and are deliberately NOT claimed, because nothing here has been tested
     *  against them and claiming a device we then mishandle is worse than not offering.
     *  ★★ THIS IS THE THIRD OF FOUR COPIES of the same fact — the others are device_filter.xml,
     *  the dispatch in local_sdr_shim.cpp's start(), and describe()'s `kind` below. They must
     *  agree or the app is offered for a device it then refuses. */
    private fun isHackRf(dev: UsbDevice): Boolean =
        dev.vendorId == HACKRF_VID && dev.productId == HACKRF_PID

    /** ★ An Airspy R2 or Mini (2026-09-22) — see AIRSPY_VID. */
    private fun isAirspy(dev: UsbDevice): Boolean =
        dev.vendorId == AIRSPY_VID && dev.productId == AIRSPY_PID

    /** Any radio we can open directly over USB. */
    private fun isSupportedRadio(dev: UsbDevice): Boolean = isServableRadio(dev.vendorId, dev.productId)

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
        /* ★★★ A THREE-WAY FACT, NOT A TWO-WAY ONE. This was `if (isAirspyHf) "airspyhf" else
         * "rtl"`, which does not fail — it silently calls a HackRF a dongle, and everything
         * downstream that switches on `kind` then draws a dongle's gain list for a radio that
         * has three stages and no AGC. Adding a driver means revisiting every else-branch that
         * assumed there were only two. */
        m.putString("kind", when {
            isAirspyHf(dev) -> "airspyhf"
            isAirspy(dev)   -> "airspy"
            isHackRf(dev)   -> "hackrf"
            else            -> "rtl"
        })
        m.putString("label", radioModelName(dev))
        m.putBoolean("hasPermission", hasPermission)
        return m
    }

    /** ★★ FACEPLATE CHARACTER FOLDING (docs/BRIEF-faceplates.md §7): a non-Latin station name (Cyrillic,
     *  Greek, Arabic, CJK…) → plain ASCII with ICU's `Any-Latin; Latin-ASCII`, so the dot-matrix and
     *  14-segment displays can draw it. JS (src/constants/displayText.ts) folds the result further and
     *  falls back to the frequency when nothing usable comes back.
     *  ★ SYNCHRONOUS: called while a display prepares its text, and JS memoises every answer, so each
     *    distinct name crosses the bridge once. Below Android 10 there is no android.icu, so the input
     *    comes back unchanged — the JS fallback still works. Any failure → the input, never a throw.
     *  ★ Display only: the result is never stored or searched. */
    @ReactMethod(isBlockingSynchronousMethod = true)
    fun transliterate(text: String?): String {
        if (text.isNullOrEmpty()) return text ?: ""
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return text
        return try { IcuLatin.transliterate(text) } catch (e: Throwable) {
            Log.w(TAG, "transliterate failed: ${e.message}")
            text
        }
    }

    /** DEVICE CLASS — Transparency effects' low-end default (src/constants/transparency.ts):
     *  { totalMemoryBytes, model, isMac }. Mirrors VibeLocalSDR.mm's deviceClass on iOS; read once by
     *  src/services/deviceClass.ts. SYNCHRONOUS so the first frame already has the default.
     *  ★ totalMem is what the kernel reports — a "3 GB" phone shows a little under 3 GiB; the JS line
     *    sits at 3.5 GiB for that reason. 0 (unreadable) decides nothing in JS.
     *  ★ isMac is always false here: it exists for the iOS app on a Mac. */
    @ReactMethod(isBlockingSynchronousMethod = true)
    fun deviceClass(): WritableMap {
        val out = Arguments.createMap()
        val total = try {
            val am = reactContext.getSystemService(Context.ACTIVITY_SERVICE) as? android.app.ActivityManager
            val mi = android.app.ActivityManager.MemoryInfo()
            am?.getMemoryInfo(mi)
            mi.totalMem
        } catch (_: Throwable) { 0L }
        out.putDouble("totalMemoryBytes", total.toDouble())
        out.putString("model", "${Build.MANUFACTURER ?: ""} ${Build.MODEL ?: ""}".trim())
        out.putBoolean("isMac", false)
        return out
    }

    // ── FRAME RATE CAP — CONTROL CUSTOMISATION → FACEPLATE → FRAME RATE (src/services/frameRate.ts) ──
    // ★★ Mirrors VibeLocalSDR.mm's setFrameRateCap / maxRefreshRate. Power audit, 2026-10-01: a 90 /
    //   120 Hz phone runs the panel at its top rate while anything animates. A 60 Hz cap asks the
    //   WINDOW for the 60 Hz display mode at the current resolution (preferredDisplayModeId, API 23+,
    //   plus preferredRefreshRate) — the system honours a window's mode request while that window is
    //   in front, and drops it when it is not. LIVE, no restart.
    // ★ Re-applied on every resume: a recreated Activity has a fresh window with no preference. The
    //   cap is kept in SharedPreferences, so a new Activity is capped before JS has said anything.
    // ★ Below API 23 there are no display modes — those phones are 60 Hz anyway, maxRefreshRate says
    //   so, and the row is hidden (faceplate.ts frameRateChoices).
    private val fpsPrefs get() = reactContext.getSharedPreferences("vibe_frame_rate", Context.MODE_PRIVATE)
    @Volatile private var fpsCapHz: Int = try { fpsPrefs.getInt("capHz", 0) } catch (_: Throwable) { 0 }

    init {
        reactContext.addLifecycleEventListener(object : LifecycleEventListener {
            override fun onHostResume() { applyFrameRateCap() }
            override fun onHostPause() {}
            override fun onHostDestroy() {}
        })
        applyFrameRateCap()
    }

    private fun displayOf(act: android.app.Activity): android.view.Display? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) act.display
        else @Suppress("DEPRECATION") act.windowManager.defaultDisplay

    /** The display's modes at the resolution it is running now — a refresh rate only reachable by
     *  also changing resolution is not one this setting offers or picks. */
    @androidx.annotation.RequiresApi(Build.VERSION_CODES.M)
    private fun sameSizeModes(d: android.view.Display): List<android.view.Display.Mode> {
        val cur = d.mode
        return d.supportedModes.filter { it.physicalWidth == cur.physicalWidth && it.physicalHeight == cur.physicalHeight }
    }

    private fun applyFrameRateCap() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return
        val act = reactContext.currentActivity ?: return
        act.runOnUiThread {
            try {
                val w = act.window ?: return@runOnUiThread
                val d = displayOf(act) ?: return@runOnUiThread
                val cap = fpsCapHz
                var modeId = 0
                var rate = 0f
                if (cap > 0) {
                    val modes = sameSizeModes(d)
                    // ★ Only when the panel can go faster than the cap, and only to a mode AT the cap
                    //   (±1 Hz): never down to a 30 / 24 Hz mode an LTPO panel may also list.
                    if (modes.any { it.refreshRate > cap + 1f }) {
                        val pick = modes.filter { it.refreshRate in (cap - 1f)..(cap + 1f) }.maxByOrNull { it.refreshRate }
                        if (pick != null) { modeId = pick.modeId; rate = pick.refreshRate }
                    }
                }
                val lp = w.attributes
                if (lp.preferredDisplayModeId == modeId && lp.preferredRefreshRate == rate) return@runOnUiThread
                lp.preferredDisplayModeId = modeId
                lp.preferredRefreshRate = rate
                w.attributes = lp
                Log.i(TAG, "frame rate cap " + (if (cap > 0) "$cap Hz (mode $modeId @ $rate)" else "off"))
            } catch (t: Throwable) {
                Log.w(TAG, "frame rate cap failed: ${t.message}")
            }
        }
    }

    /** 0 = no cap; otherwise the cap in Hz (the app only ever sends 60). */
    @ReactMethod
    fun setFrameRateCap(hz: Double) {
        val cap = if (hz.isFinite() && hz > 0) Math.round(hz).toInt() else 0
        fpsCapHz = cap
        try { fpsPrefs.edit().putInt("capHz", cap).apply() } catch (_: Throwable) {}
        applyFrameRateCap()
    }

    /** The panel's top refresh rate at its current resolution (60 on a 60 Hz phone, and always the
     *  one rate below API 23, where there are no modes to choose between). */
    @ReactMethod
    fun maxRefreshRate(promise: Promise) {
        try {
            val act = reactContext.currentActivity
            val d = (if (act != null) displayOf(act) else null)
                ?: (reactContext.getSystemService(Context.DISPLAY_SERVICE) as? android.hardware.display.DisplayManager)
                    ?.getDisplay(android.view.Display.DEFAULT_DISPLAY)
            if (d == null) { promise.resolve(null); return }
            val hz = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
                sameSizeModes(d).maxOfOrNull { it.refreshRate } ?: d.refreshRate
            else d.refreshRate
            promise.resolve(hz.toDouble())
        } catch (t: Throwable) {
            promise.resolve(null)
        }
    }

    /** ★ Lite: "run the server in the background — minimise app". Home, in effect: the task goes to the back
     *  and NOTHING stops — the server lives in its foreground service either way. */
    @ReactMethod
    fun minimise() {
        // ★ reactContext.currentActivity, NOT the module's own: ReactContextBaseJavaModule no longer exposes it
        //   on React Native 0.86, and this file would not compile at all — see the note in AGENTS.md about a
        //   build that fails for a reason nothing in the diff caused.
        try { reactContext.currentActivity?.moveTaskToBack(true) } catch (_: Throwable) {}
    }

    /** ★★★ THE SERVER BENCHMARK — what this box can carry (VibeBenchmark, vibe_benchmark.h). Lite runs it at
     *  first setup; the main app offers it. Resolves the result as JSON.
     *  ★★ IT TAKES A MINUTE OR TWO and the radio must be off the air, so: never on the main thread, and the
     *     caller stops the server first — measuring while capturing measures the two fighting for the cores. */
    @ReactMethod
    fun runBenchmark(wantDab: Boolean, promise: Promise) {
        Thread {
            try {
                val j = VibeBenchmark.run(reactContext, wantDab)
                if (j == null) promise.reject("bench_failed", "the benchmark did not produce a result")
                else promise.resolve(j.toString())
            } catch (t: Throwable) { promise.reject("bench_failed", t.message ?: "$t") }
        }.start()
    }

    /** ★ The Band III block list, from the receiver's own table — see VibeLocalSDR.dabBlocksJson. */
    @ReactMethod
    fun dabBlocks(promise: Promise) {
        try { promise.resolve(VibeLocalSDR.dabBlocksJson()) } catch (t: Throwable) { promise.reject("dab_blocks", t) }
    }

    /**
     * ★★★ THE QUICK STATION SCAN for the DAB landing station (Stuart, 2026-09-28) — "what is on 12B?".
     *
     * The ENGINE does the work (GET /vibeserver/dab-scan: enter DAB on the block through the ordinary
     * entry, wait for the ensemble to list its services, hand the radio back). This only gets it a
     * radio to do it on:
     *   - SERVING already: ask the running server over loopback. It refuses, with the reason, while
     *     anybody is listening — a scan would move their dial.
     *   - NOT serving (the settings screen is only shown then): open the radio, bring the engine up
     *     on LOOPBACK ONLY for the length of the scan, ask it, and put everything away again. No
     *     listing, no mDNS, no foreground service — nothing a listener could ever see.
     * `known = true` answers from the stations the server has already heard on that block (only
     * when it is serving — a stopped engine has no memory loaded).
     * ★ Several seconds long (up to 15), so never on the bridge thread.
     */
    /** ★★ WHAT THE SCAN IS DOING NOW, for the screen's progress line — see dabScanPhase(). */
    @Volatile private var scanPhase = ""
    /** ★★ The scan's current phase, polled by the settings screen while it waits: the Sony took 43 s
     *  (2026-09-28) against a promised "up to 15 s", because the radio has to be opened and a private
     *  engine started and stopped around the engine's own 15 s. A counter with no words beside it
     *  reads as a hang; the phase says what the time is being spent on. */
    @ReactMethod
    fun dabScanPhase(promise: Promise) { promise.resolve(scanPhase) }

    @ReactMethod
    fun dabQuickScan(block: String, known: Boolean, ownerConfigJson: String, promise: Promise) {
        Thread {
            val t0 = android.os.SystemClock.elapsedRealtime()
            /* ★★★ EVERY PHASE STAMPED IN THE LOG, so the next slow scan names the phase that was slow
             *  instead of leaving it to be argued about (the engine stamps its own half: entry, lock,
             *  first FIB, hand-back). */
            fun phase(p: String) {
                scanPhase = p
                Log.i(TAG, "DAB quick scan: +${android.os.SystemClock.elapsedRealtime() - t0} ms — $p")
            }
            try {
                val st = org.json.JSONObject(VibeLocalSDR.getVibeServerStatus())
                val livePort = if (st.optBoolean("running", false)) st.optInt("port", 0) else 0
                if (livePort > 0) {
                    phase("scanning $block on the running server")
                    promise.resolve(dabScanHttp(livePort, block, known))
                    phase("done")
                    return@Thread
                }
                if (known) { promise.resolve("{\"ok\":true,\"known\":true,\"services\":[]}"); return@Thread }
                val mgr = usbManager ?: run { promise.reject("no_usb", "USB service unavailable"); return@Thread }
                val dev = mgr.deviceList.values.firstOrNull { isSupportedRadio(it) }
                    ?: run { promise.reject("no_device", "No SDR found — plug the radio in to scan"); return@Thread }
                if (!mgr.hasPermission(dev)) {
                    promise.reject("no_permission", "Allow this app to use the radio (start the server once), then scan again")
                    return@Thread
                }
                phase("opening the radio")
                stopSpectrumInternal()
                val conn = mgr.openDevice(dev)
                    ?: run { promise.reject("open_failed", "The radio could not be opened"); return@Thread }
                try {
                    /* ★★★ THE OWNER'S RADIO, NOT A BARE ONE (Stuart, 2026-09-29) — see
                     *  VibeServerBoot.privateScanConfig. The screen hands in the config its Start button
                     *  would send (one builder, two readers), and it goes through the SAME applyAndStart
                     *  as every server start: bias-T, ppm, direct sampling, the AGC and its rules, the
                     *  resting gain. Only what makes it private is taken out: loopback only (serveOnLan
                     *  false), no PIN, no landing, nothing pinned below 2.048 MS/s. The block's
                     *  remembered AGC gain is restored at DAB entry and learned if there is none —
                     *  the listener's own entry does both, and the scan uses it. */
                    val owner = try { org.json.JSONObject(ownerConfigJson) } catch (t: Throwable) {
                        promise.reject("bad_config", "The scan was not given the server's settings: ${t.message}"); return@Thread
                    }
                    val cfg = VibeServerBoot.privateScanConfig(owner)
                    VibeLocalSDR.setUsbModelName(VibeServerBoot.usbModelName(dev))
                    phase("starting the receiver with the server's own radio settings")
                    val port = VibeServerBoot.applyAndStart(cfg, conn.fileDescriptor, dev.vendorId, dev.productId,
                                                            reactContext.filesDir, serveOnLan = false)
                    Log.i(TAG, "DAB quick scan: private engine on the owner's settings — bias-T " +
                               (if (cfg.optBoolean("biasT", false)) "ON" else "off") +
                               ", ppm ${cfg.optDouble("ppm", 0.0).toInt()}, AGC " +
                               (if (cfg.optBoolean("rtlAgc", false) || cfg.optBoolean("agcLock", false)) "on" else "off"))
                    if (port <= 0) { promise.reject("start_failed", "The radio would not start for the scan"); return@Thread }
                    phase("locking onto $block and reading its station names")
                    val body = dabScanHttp(port, block, false)
                    phase("putting the radio away")
                    promise.resolve(body)
                } finally {
                    try { VibeLocalSDR.stopSpectrumSync() } catch (t: Throwable) { Log.w(TAG, "scan: stop failed: ${t.message}") }
                    try { conn.close() } catch (t: Throwable) { Log.w(TAG, "scan: close failed: ${t.message}") }
                    phase("done")
                }
            } catch (t: Throwable) {
                phase("failed: ${t.message ?: t}")
                promise.reject("dab_scan", t.message ?: "$t")
            }
        }.start()
    }

    private fun dabScanHttp(port: Int, block: String, known: Boolean): String {
        val url = java.net.URL("http://127.0.0.1:$port/vibeserver/dab-scan?block=" +
            java.net.URLEncoder.encode(block, "UTF-8") + (if (known) "&known=1" else ""))
        val c = url.openConnection() as java.net.HttpURLConnection
        c.connectTimeout = 3000
        c.readTimeout = 25000   // ★ the scan itself is capped at 15 s by the engine
        c.setRequestProperty("User-Agent", "VibeServer-setup")
        try {
            val code = c.responseCode
            val body = (if (code in 200..299) c.inputStream else c.errorStream)?.bufferedReader()?.use { it.readText() } ?: ""
            if (code !in 200..299) throw java.io.IOException("the server answered $code")
            return body
        } finally { c.disconnect() }
    }

    /**
     * ★★★ READ EVERY STATION'S NAME ON THE MULTIPLEX — LIVE, not at the next start.
     *
     * The switch was written to storage and sent in the start config, and the hint said "takes
     * effect when the server next starts" — which is true and is not good enough: on a TV the
     * server is left running for days, so the owner flips it, watches nothing happen, and
     * reasonably concludes the setting is broken (Stuart, 2026-09-22, having just had the
     * forwarding bug fixed in the same setting: "the multi station radio text still isnt enabling
     * even though i set it in the settings on the TV" — the server had not restarted since 22:28).
     * ★★ The engine has always been able to take it at runtime; there was simply no @ReactMethod,
     *    so JS could not reach it — the missing-bridge trap, where a native function exists,
     *    compiles, and is unreachable from the only place that would call it.
     */
    @ReactMethod
    fun setDabScanLabels(mode: Int, promise: Promise) {
        try {
            VibeLocalSDR.setDabScanLabels(mode)
            // ★ ...and into the config a restore replays, or the next rebuild undoes it. See
            //   VibeServerRestore.updateConfig for the evidence.
            VibeServerRestore.updateConfig(reactContext, "dabScanLabels", mode)
            promise.resolve(true)
        } catch (t: Throwable) { promise.reject("dab_scan_failed", t.message ?: "$t") }
    }

    /**
     * ★★★ THE AIRSPY PANEL'S CONTROLS WHEN THE RADIO IS PLUGGED INTO THIS PHONE.
     *
     * Remote receivers get these over the WebSocket (`airspy_control`); local hardware had no path
     * at all, so the whole panel was decorative — see nativeAirspyControl for the evidence.
     * ★ Takes the same field names as the WebSocket message, so the two paths cannot drift into
     *   describing the radio differently. Booleans arrive as 0/1.
     */
    @ReactMethod
    fun airspyControl(opts: ReadableMap, promise: Promise) {
        try {
            val it = opts.keySetIterator()
            while (it.hasNextKey()) {
                val k = it.nextKey()
                val v = when (opts.getType(k)) {
                    ReadableType.Boolean -> if (opts.getBoolean(k)) 1 else 0
                    ReadableType.Number  -> opts.getDouble(k).toInt()
                    else                 -> continue
                }
                VibeLocalSDR.airspyControl(k, v)
            }
            promise.resolve(true)
        } catch (t: Throwable) { promise.reject("airspy_control_failed", t.message ?: "$t") }
    }

    /** ★ How far through the benchmark is, for the bar. Cheap, and safe to call while it runs. */
    @ReactMethod
    fun benchProgress(promise: Promise) {
        try { promise.resolve(VibeLocalSDR.benchProgress()) }
        catch (t: Throwable) { promise.reject("bench_progress_failed", t.message ?: "$t") }
    }

    /** The last benchmark result, or null if it has never run on this device. */
    @ReactMethod
    fun lastBenchmark(promise: Promise) {
        try { promise.resolve(VibeBenchmark.last(reactContext)?.toString()) }
        catch (t: Throwable) { promise.reject("bench_read_failed", t.message ?: "$t") }
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
        // ★ A local dongle IS a loopback VibeServer: its web client's spots map fetches /mapgl/ too.
        VibeMapGL.start(reactApplicationContext)
        val res = Arguments.createMap()
        res.putInt("port", port)
        res.putString("wsBaseUrl", "http://127.0.0.1:$port")
        /* ★★ WHICH RADIO WAS OPENED, so the app can remember settings PER RADIO (2026-09-22). The
         *  settings were keyed by connection type ("usb"), so an RTL and an Airspy HF+ shared one
         *  memory — and an RTL's 2.4 MS/s was restored onto an HF+ that tops out at 912 kHz. The
         *  serial (readable now that permission is granted) separates two of the same kind. */
        res.putString("radioKind", when {
            isAirspyHf(dev) -> "airspyhf"; isAirspy(dev) -> "airspy"
            isHackRf(dev) -> "hackrf"; else -> "rtl" })
        res.putString("radioSerial", try { dev.serialNumber ?: "" } catch (_: SecurityException) { "" })
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
        // ★★★ Not while another VibeServer serves on this device (the other VibeSDR app, or Lite) — VibeServerBoot.
        VibeServerBoot.otherServerOnDevice()?.let { promise.reject("other_server", VibeServerBoot.otherServerMessage(it)); return }
        val conn = mgr.openDevice(dev)
            ?: run { promise.reject("open_failed", "openDevice returned null"); return }
        val fd = conn.fileDescriptor
        if (fd < 0) { conn.close(); promise.reject("bad_fd", "Invalid file descriptor"); return }
        sessionConn = conn
        // ★ Shared with the re-enumeration recovery, which closes it when it has to replace it.
        VibeServerRestore.holdServerConn(conn, dev)   // ★ and which radio — see heldVidPid

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

        // ★ Before start: the engine cannot read the descriptor on an fd-open. See usbModelName().
        VibeLocalSDR.setUsbModelName(VibeServerBoot.usbModelName(dev))
        val port = VibeServerBoot.applyAndStart(cfg, fd, dev.vendorId, dev.productId,
                                                reactContext.filesDir, serveOnLan = true)
        // ★ AFTER the server is up: the monitor pushes the sticky state the moment it registers, and
        //   a push before the native library is loaded is dropped (473 showed no level, 2026-09-17).
        VibeServerBoot.startBatteryMonitor(reactContext)
        // ★★★ PUT THE PUBLIC LISTING BACK IF IT WAS ON. The tunnel dies with the process that
        //     spawned it, so an update, a low-memory kill or a reboot leaves the directory
        //     advertising an address that answers 530 until the entry expires — seen 2026-08-22
        //     after an install: "it just lost the server". The switch is a STANDING INSTRUCTION,
        //     not a one-off action, so it is re-established here where the port has just been
        //     bound. ★ Never allowed to fail the start: restoreIfWanted swallows everything.
        if (port > 0) VibeTunnel.restoreIfWanted(reactApplicationContext, port)
        // ★★ COUNTRY AND NETWORK LOOKUP, wired up and refreshed if stale. Idempotent and cheap
        //    unless a download is actually needed — see VibeGeoData.
        if (port > 0) VibeGeoData.start(reactApplicationContext)
        // ★★★ THE GPU MAP (/mapgl/) — unpacked from the APK once, plus the High Detail downloader.
        //     On BOTH start paths, for the same reason as the line above. See VibeMapGL.
        if (port > 0) VibeMapGL.start(reactApplicationContext)
        // ★★ STATION LOGOS FROM THE BROADCASTER, wired on BOTH start paths. The geo lookup above
        //    had to learn that lesson too: a headless restore is how this server usually comes
        //    back, so anything wired only where the UI starts it is missing exactly when nobody
        //    is watching.
        //  ★ The country is a hint from the device's own locale — tried first, with the rest of
        //    the ECC candidates behind it, so a wrong or absent one still resolves.
        if (port > 0) {
            try {
                VibeLocalSDR.initRadioDns(
                    java.io.File(reactApplicationContext.filesDir, "radiodns").apply { mkdirs() }.absolutePath,
                    java.util.Locale.getDefault().country ?: "")
            } catch (t: Throwable) {
                android.util.Log.w("VibeSDR", "RadioDNS not started: ${t.message}")
            }
        }
        if (port <= 0) {
            VibeLocalSDR.setServeOnLan(false)
            conn.close(); sessionConn = null
            VibeServerRestore.holdServerConn(null)
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
            VibeServerRestore.arm(reactContext, cfg, dev)   // ★ and which radio — see K_VIDPID
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
        // ★ After the engine has stopped: the connection a re-enumeration recovery opened is ours to close.
        VibeServerRestore.releaseServerConn()
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
    /**
     * What to CALL this radio — maker and model, the way the landing page does.
     *
     * ★ A dongle's USB product string is often just "Blog V4"; the maker sits in a separate
     *   descriptor. Shown alone it identifies nothing, which matters most in a public directory
     *   where it is what somebody picks between.
     * ★★ Never doubled: a product string that already names its maker is returned as it is.
     */
    private fun radioModelName(dev: android.hardware.usb.UsbDevice): String {
        val product = (dev.productName ?: "").trim()
        val maker = (dev.manufacturerName ?: "").trim()
        val fallback = when {
            isAirspyHf(dev) -> "Airspy HF+"
            isAirspy(dev)   -> "Airspy"      // ★ R2 or Mini — the board id at open says which
            isHackRf(dev)   -> "HackRF One"
            else            -> "RTL-SDR"
        }
        if (product.isEmpty()) return if (maker.isNotEmpty()) "$maker $fallback" else fallback
        if (maker.isEmpty() || product.lowercase().startsWith(maker.lowercase())) return product
        return "$maker $product"
    }

    @ReactMethod
    fun getConnectedRadio(promise: Promise) {
        val mgr = usbManager ?: run { promise.resolve(null); return }
        val dev = mgr.deviceList.values.firstOrNull { isSupportedRadio(it) }
            ?: run { promise.resolve(null); return }
        val res = Arguments.createMap()
        res.putString("driver", when {
            isAirspyHf(dev) -> "airspyhf"
            isAirspy(dev)   -> "airspy"     // ★ was missing: an Airspy R2/Mini reported "rtl" here (2026-09-28)
            isHackRf(dev)   -> "hackrf"
            else            -> "rtl"
        })
        // ★★★ THE MAKER AND THE MODEL, as the receiver's own landing page names it: "RTLSDRBlog
        //     Blog V4", not the bare "Blog V4" the dongle reports as its product string. On its own
        //     that names nothing a buyer would recognise, and in a public directory it is the
        //     difference between identifying the hardware and hinting at it (Stuart, 2026-08-22).
        //  ★ Only where it ADDS something: a product string that already begins with the maker is
        //    left alone, or an Airspy would come out "Airspy Airspy HF+".
        res.putString("model", radioModelName(dev))
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
            // ★★ What the owner has to do about the radio — "needs unplugging and plugging back in" — or ""
            //    (2026-10-05). Field by field, as the note above warns: left out here, the screen never sees it.
            m.putString("radioProblem", o.optString("radioProblem", ""))
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
    /** ★ AUTO direct sampling for LOCAL hardware (2026-09-22) — the engine and VibeLocalSDR had it
     *  (the server config uses it); the bridge did not, so the local panel could only offer Off/On. */
    @ReactMethod fun setAutoDirectSampling(on: Boolean, belowHz: Double) {
        VibeLocalSDR.setAutoDirectSampling(on, if (belowHz > 0) belowHz else 24e6)
    }
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
                    radioModel: String, radioDriver: String,
                    antenna: String, coverage: String, locked: Boolean,
                    shareForSec: Double, promise: Promise) {
        try {
            VibeTunnel.applyLoopbackTrust(true, ownerProxies)
            VibeTunnel.startTunnel(reactApplicationContext, port) { url ->
                if (url == null) { promise.resolve(VibeTunnel.statusJson()); return@startTunnel }
                VibeTunnel.publish(reactApplicationContext, name, locator, port, radioModel, radioDriver,
                                   antenna, coverage, locked, shareForSec.toLong())
                promise.resolve(VibeTunnel.statusJson())
            }
        } catch (t: Throwable) { promise.reject("tunnel_start", t) }
    }

    /**
     * Re-publish the listing from the values the app holds RIGHT NOW, without touching the tunnel.
     *
     * ★★★ THE HEADLESS RESTORE CAN ONLY REPLAY WHAT IT STORED. When the server comes back on its
     *     own — after an update, a reboot or a low-memory kill — there is no UI to ask, so
     *     restoreIfWanted republishes the values captured the last time the switch was flicked BY
     *     HAND. That keeps a listing alive with nobody present, which is the point, but it also
     *     means the entry can describe the server as it was rather than as it is: Stuart's listing
     *     still read "fm" hours after the app had learned to say "FM Broadcast Band", and a
     *     changed aerial or band limit would be just as stale (2026-08-22).
     * ★★ So the screen corrects it as soon as there IS a UI. Cheap — it is the ordinary ping, with
     *    fresh arguments — and it needs no tunnel restart, so nobody listening is disturbed.
     */
    @ReactMethod
    fun tunnelRepublish(name: String, locator: String, port: Int,
                        radioModel: String, radioDriver: String,
                        antenna: String, coverage: String, locked: Boolean,
                        shareForSec: Double, promise: Promise) {
        try {
            if (!VibeTunnel.isTunnelRunning() || port <= 0 || name.length < 2) {
                promise.resolve(VibeTunnel.statusJson()); return
            }
            // ★★★ -1 means "leave the share window alone", 0 means "permanent", >0 sets a window.
            //     The periodic refresh sends -1, so it can never extend an offer by accident; only
            //     a deliberate change from the switch carries 0 or a length. Two states were not
            //     enough — with 0 doing double duty, turning the temporary toggle OFF could not
            //     make a share permanent (Stuart, 2026-08-22).
            VibeTunnel.publish(reactApplicationContext, name, locator, port,
                               radioModel, radioDriver, antenna, coverage, locked,
                               shareForSec.toLong())
            promise.resolve(VibeTunnel.statusJson())
        } catch (t: Throwable) { promise.reject("tunnel_republish", t) }
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

        /** HackRF One — see isHackRf(). Mirrored in res/xml/device_filter.xml (DECIMAL there). */
        internal const val HACKRF_VID = 0x1d50
        internal const val HACKRF_PID = 0x6089

        /** ★ Airspy R2 / Mini — ONE id for both models; the board id read at open tells them
         *  apart (see airspy_source.cpp). Mirrored in device_filter.xml and in the engine's
         *  start() dispatch: three copies of one fact, and they must agree. */
        internal const val AIRSPY_VID = 0x1d50
        internal const val AIRSPY_PID = 0x60a1

        /** ★ Any radio a server can be running on — the same four allowlists as isSupportedRadio(),
         *  reachable without a module instance. The service asks it of a USB device that has just
         *  DETACHED, to know whether the server's radio is what left (VibeServerRestore.noteRadioGone). */
        internal fun isServableRadio(vid: Int, pid: Int): Boolean =
            RTL_SDR_VIDPIDS.contains((vid shl 16) or pid) ||
            (vid == AIRSPYHF_VID && pid == AIRSPYHF_PID) ||
            (vid == HACKRF_VID && pid == HACKRF_PID) ||
            (vid == AIRSPY_VID && pid == AIRSPY_PID)
    }
}

/** ICU `Any-Latin; Latin-ASCII` for [VibeLocalSdrModule.transliterate]. Its own object so the
 *  android.icu class is only ever loaded on Android 10+ (the caller checks SDK_INT first). Built once:
 *  compiling a compound transform is the expensive part. ICU transliterators are not documented as
 *  thread-safe, so calls are serialised (they come from the JS thread anyway). */
@androidx.annotation.RequiresApi(Build.VERSION_CODES.Q)
private object IcuLatin {
    private val t: android.icu.text.Transliterator by lazy {
        android.icu.text.Transliterator.getInstance("Any-Latin; Latin-ASCII")
    }
    @Synchronized fun transliterate(text: String): String = t.transliterate(text)
}
