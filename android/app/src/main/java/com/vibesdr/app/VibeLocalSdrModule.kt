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

    // ── ★★★ THE SERVER LIVES IN ITS OWN PROCESS (beta88) — VibeServerHost / VibeServerLink / VibeServerOps ──────────
    // Everything below that starts, stops, asks or changes the HOSTED server goes through the link; this process keeps
    // only what needs an Activity (the USB and microphone prompts) and Local Hardware listening (its own engine copy).

    /** ★ Serial: a Start, a Stop and the rtl_tcp server happen in the order they were asked for. */
    private val serverWork = java.util.concurrent.Executors.newSingleThreadExecutor { r ->
        Thread(r, "vibe-server-ui").apply { isDaemon = true }
    }

    /** Ask the server process; resolve with `pick(answer)` or reject with its own words. Off the bridge thread. */
    private fun ask(method: String, args: JSONObject, promise: Promise, serial: Boolean = false,
                    pick: (JSONObject) -> Any?) {
        val run = Runnable {
            val r = VibeServerLink.call(reactContext, method, args)
            if (r.optBoolean("ok", false)) {
                try { promise.resolve(pick(r)) } catch (t: Throwable) { promise.reject("server_answer", t.message ?: "$t") }
            } else promise.reject(r.optString("code", "server"), r.optString("message", "the server did not answer"))
        }
        if (serial) serverWork.execute(run) else Thread(run, "vibe-server-ask").start()
    }

    /** A live setter on the running server — fire and forget, as the old direct calls were. */
    private fun tell(what: String, args: JSONObject = JSONObject()) =
        VibeServerLink.callAsync(reactContext, "set", args.put("what", what))

    /** ★ Is the server process even running? A status poll for one that is not answers here, WITHOUT binding — binding
     *  would create the process, and an idle :server process is RAM a 1 GB phone should not spend. */
    private fun serverProcessUp(): Boolean = !VibeProcess.serverIsSeparate(reactContext) || VibeProcess.serverProcessAlive(reactContext)

    /** The running server's port, or 0 — for the DAB scans, which talk to it over loopback HTTP. */
    private fun serverLivePort(): Int {
        if (!serverProcessUp()) return 0
        val r = VibeServerLink.call(reactContext, "status")
        val st = r.optJSONObject("status") ?: return 0
        return if (st.optBoolean("running", false)) st.optInt("port", 0) else 0
    }

    /** ★★ The server's events reach the screen as "VibeServerEvent" {type, json} — the /setup page changing a setting
     *  is one, so the phone's screen shows it at once (item B). */
    private fun emitServerEvent(type: String, json: JSONObject) {
        try {
            if (!reactContext.hasActiveReactInstance()) return
            val m = Arguments.createMap()
            m.putString("type", type)
            m.putString("json", json.toString())
            reactContext.getJSModule(com.facebook.react.modules.core.DeviceEventManagerModule.RCTDeviceEventEmitter::class.java)
                .emit("VibeServerEvent", m)
        } catch (t: Throwable) { Log.w(TAG, "server event not delivered: ${t.message}") }
    }

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
        "startOnPowerSupported" to VibeBootStart.supported(),
        // ★ "Keep radio alive" starts ON on a TV or TV box — VibeServerBoot.keepRadioAliveDefault, the one rule.
        "keepRadioAliveDefault" to VibeServerBoot.keepRadioAliveDefault(reactContext))

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

    /** ★ "Search for switch…" (2026-10-09) — a few seconds of LAN scan and broker listening, so it runs on its own thread
     *  and resolves with the shim's JSON: {"brokers":[…],"devices":[{"broker","id","name","kind","relays":[…]}]}. */
    @ReactMethod
    fun antSwitchSearch(host: String, port: Int, promise: com.facebook.react.bridge.Promise) {
        Thread {
            try { promise.resolve(VibeLocalSDR.antSwitchSearch(host, port)) }
            catch (t: Throwable) { promise.reject("ANTSWITCH", t.message ?: "search failed") }
        }.start()
    }

    /** ★ What the aerial covers / filters fitted, applied NOW on a running server. */
    @ReactMethod
    fun setAntennaBands(ranges: String, filters: String) {
        tell("antennaBands", JSONObject().put("ranges", ranges).put("filters", filters))
    }

    /** ★ V12 the served radio's hwinfo `radio` object as JSON ("" when nothing is served) — the ports the screen names. */
    @ReactMethod
    fun radioCaps(promise: Promise) {
        if (!serverProcessUp()) { promise.resolve(""); return }
        Thread { promise.resolve(VibeServerLink.call(reactContext, "radioCaps").optString("json", "")) }.start()
    }

    /** ★★ V12 what the background-decoding editor may offer (the engine's decoder table, ISM ids, Band list, slots). */
    @ReactMethod
    fun backgroundCaps(promise: Promise) {
        try { promise.resolve(VibeLocalSDR.backgroundCaps()) } catch (t: Throwable) { promise.resolve("") }
    }

    /** ★★★ V12 what this radio can do (vibe_radio_caps.h) — the Setup page's own answer, as JSON ("" = an engine without it). */
    @ReactMethod
    fun radioCapsFor(driver: String, model: String, autoDs: Boolean, directSampling: Double,
                     convOffsetHz: Double, convLoHz: Double, convHiHz: Double, promise: Promise) {
        try {
            promise.resolve(VibeLocalSDR.radioCapsFor(driver, model, autoDs, directSampling.toInt(), convOffsetHz, convLoHz, convHiHz))
        } catch (t: Throwable) { promise.resolve("") }
    }

    /** ★★ V12 the Setup page's save rule for background decoding — "" = good, else the engine's own sentence. */
    @ReactMethod
    fun validateBackgroundDecoding(json: String, rate: Double, slotCap: Double, lockCentre: Double,
                                   ismOk: Boolean, ndbOk: Boolean, aisOk: Boolean, promise: Promise) {
        try {
            promise.resolve(VibeLocalSDR.validateBackgroundDecoding(json, rate, slotCap.toInt(), lockCentre, ismOk, ndbOk, aisOk))
        } catch (t: Throwable) { promise.resolve("") }
    }

    /** ★★ V12 the gallery's disk: free space where it lives and what it holds now (bytes) — the size box says both. */
    @ReactMethod
    fun galleryDisk(promise: Promise) {
        try {
            val dir = VibeServerBoot.galleryDir(reactContext.filesDir)
            var used = 0L
            if (dir.exists()) dir.walkTopDown().forEach { if (it.isFile) used += it.length() }
            val m = Arguments.createMap()
            m.putDouble("freeBytes", reactContext.filesDir.usableSpace.toDouble())
            m.putDouble("usedBytes", used.toDouble())
            promise.resolve(m)
        } catch (t: Throwable) { promise.resolve(null) }
    }

    /** ★ V12 the owner's per-port antenna names, applied NOW on a running server (a save on the server screen). */
    @ReactMethod
    fun setAntennaPortNames(json: String) {
        tell("antennaPortNames", JSONObject().put("json", json))
    }

    /** ★ Apply the switch, the per-band aerial list and the lock NOW, on a running server (a save on the server screen). */
    @ReactMethod
    fun setAntennaSwitch(json: String, map: String, locked: Boolean) {
        tell("antennaSwitch", JSONObject().put("json", json).put("map", map).put("locked", locked))
    }

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

    /** ★★★ An SDRplay RSP (2026-10-22) — every model, through the OpenRSP driver inside the app. FALSE below
     *  Android 8.0 (API 26): the driver cannot run there, so the radio is not one this app offers. See VibeRsp. */
    private fun isRsp(dev: UsbDevice): Boolean = VibeRsp.isRsp(dev)

    /** ★ An Airspy R2 or Mini (2026-09-22) — see AIRSPY_VID. */
    private fun isAirspy(dev: UsbDevice): Boolean =
        dev.vendorId == AIRSPY_VID && dev.productId == AIRSPY_PID

    /** ★★★ An Icom transceiver, RECEIVE ONLY (docs/ICOM-SUPPORT-PLAN.md Phase 4) — its CP210x (or Icom-VID port) whose
     *  product or serial string names a model the engine supports. A bare CP2102 is never called an Icom. See VibeIcom. */
    private fun isIcom(dev: UsbDevice): Boolean = VibeIcom.isIcom(dev)

    /** Any radio we can open directly over USB — and that a SERVER can run (an Icom included). */
    private fun isSupportedRadio(dev: UsbDevice): Boolean = isServableRadio(dev.vendorId, dev.productId) || isIcom(dev)

    /** ★ What LOCAL listening opens: every radio but an Icom. An Icom's audio needs the server's microphone-type
     *  foreground service and the permission flow that only the server start has, so it is served (and listened to
     *  through the server), not opened as a local radio — plan Phase 4, Open for Stuart. */
    private fun isLocalRadio(dev: UsbDevice): Boolean = isServableRadio(dev.vendorId, dev.productId) && !VibeIcom.isCandidate(dev)

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
        /* ★★★ A FIVE-WAY FACT, NOT A TWO-WAY ONE (the RSP made it five, 2026-10-22). This was `if (isAirspyHf)
         * "airspyhf" else "rtl"`, which does not fail — it silently calls a HackRF a dongle, and everything
         * downstream that switches on `kind` then draws a dongle's gain list for a radio that
         * has three stages and no AGC. Adding a driver means revisiting every else-branch that
         * assumed there were only two. */
        m.putString("kind", when {
            isIcom(dev)     -> "icom"       // ★ before the else: a CP2102 is not a dongle
            isRsp(dev)      -> "sdrplay"
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
     *  { totalMemoryBytes, model, isMac, isLowRamDevice }. Mirrors VibeLocalSDR.mm's deviceClass on iOS; read once by
     *  src/services/deviceClass.ts. SYNCHRONOUS so the first frame already has the default.
     *  ★ totalMem is what the kernel reports — a "3 GB" phone shows a little under 3 GiB; the JS line
     *    sits at 3.5 GiB for that reason. 0 (unreadable) decides nothing in JS.
     *  ★ isMac is always false here: it exists for the iOS app on a Mac. */
    @ReactMethod(isBlockingSynchronousMethod = true)
    fun deviceClass(): WritableMap {
        val out = Arguments.createMap()
        var lowRam = false
        val total = try {
            val am = reactContext.getSystemService(Context.ACTIVITY_SERVICE) as? android.app.ActivityManager
            val mi = android.app.ActivityManager.MemoryInfo()
            am?.getMemoryInfo(mi)
            lowRam = am?.isLowRamDevice == true
            mi.totalMem
        } catch (_: Throwable) { 0L }
        out.putDouble("totalMemoryBytes", total.toDouble())
        // ★★ The waterfall ring's RAM tier (src/utils/wfRingSize.ts): an Android Go / low-RAM device keeps the 1024-row
        //   ring whatever totalMem says. Absent (an older binary) = not low-RAM, and an unread totalMem is the low tier anyway.
        out.putBoolean("isLowRamDevice", lowRam)
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
        VibeServerLink.addListener { type, json -> emitServerEvent(type, json) }
        reactContext.addLifecycleEventListener(object : LifecycleEventListener {
            override fun onHostResume() { applyFrameRateCap(); frameWatchAttach() }
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
                    // ★ The SLOWEST mode at or above the cap — but never below 60 Hz: a 30 / 24 Hz mode an LTPO panel
                    //   may list would slow touch and scrolling too. A 30 cap is paced by the app on top of the
                    //   60 Hz mode (src/services/uiCadence.ts: the frame callbacks run on every other vsync).
                    val floorHz = maxOf(cap, 60) - 1f
                    val pick = modes.filter { it.refreshRate >= floorHz }.minByOrNull { it.refreshRate }
                    if (pick != null && modes.any { it.refreshRate > pick.refreshRate + 1f }) {
                        modeId = pick.modeId; rate = pick.refreshRate
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

    /** 0 = no cap; otherwise the cap in Hz (60 or 30). */
    @ReactMethod
    fun setFrameRateCap(hz: Double) {
        val cap = if (hz.isFinite() && hz > 0) Math.round(hz).toInt() else 0
        fpsCapHz = cap
        try { fpsPrefs.edit().putInt("capHz", cap).apply() } catch (_: Throwable) {}
        applyFrameRateCap()
    }

    /** ★ Battery saver on? FRAME RATE → AUTO then runs at 30 (Stuart, 2026-10-13: "would be useful for low power mode").
     *  Read every few seconds by the AUTO loop; false when the system cannot say. */
    @ReactMethod(isBlockingSynchronousMethod = true)
    fun isPowerSaveMode(): Boolean = try {
        (reactContext.getSystemService(Context.POWER_SERVICE) as? android.os.PowerManager)?.isPowerSaveMode == true
    } catch (_: Throwable) { false }

    // ── FRAME WATCH — FRAME RATE → AUTO (constants/faceplate.ts autoWindowLate) ──
    //   What it counts and how: FrameWatchN, at the end of this file.
    // ★★★ THE API-24 TYPES LIVE IN FrameWatchN, NEVER IN THIS CLASS (Kiko, 2026-10-14: Lite 65 on an API 23 Amlogic box
    //     crashed at launch — "Exception in HostObject::get for prop 'VibeLocalSDR': NoClassDefFoundError:
    //     android.view.FrameMetrics"). The SDK_INT checks were right, but the listener lambda was desugared into a static
    //     method of THIS class with FrameMetrics in its signature, and React Native finds @ReactMethods with
    //     getDeclaredMethods — which resolves every declared method's types, so the whole module failed before any check
    //     could run. Lite's minSdk is 21; the app's is 24, which is why only Lite broke. Anything newer than Lite's minSdk
    //     goes in its own class here, like IcuLatin. scripts/check-lite-api.py fails the Lite build if a module
    //     method signature names one.
    @Volatile private var fwOn = false
    private var fw: Any? = null                                   // FrameWatchN on API 24+, else always null
    private val fwThread by lazy { android.os.HandlerThread("vibe-frame-watch").also { it.start() } }

    private fun frameWatchAttach() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N || !fwOn) return
        val act = reactContext.currentActivity ?: return
        act.runOnUiThread {
            try {
                val w = act.window ?: return@runOnUiThread
                val hz = (displayOf(act)?.refreshRate ?: 60f).coerceAtLeast(30f)
                val cur = (fw as? FrameWatchN) ?: FrameWatchN(fwThread).also { fw = it }
                cur.attach(w, (1.5e9 / hz).toLong())
            } catch (t: Throwable) {
                Log.w(TAG, "frame watch failed: ${t.message}")
            }
        }
    }

    private fun frameWatchDetachNow() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N) return
        (fw as? FrameWatchN)?.detach()
    }

    @ReactMethod
    fun startFrameWatch() {
        fwOn = true
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) (fw as? FrameWatchN)?.take()
        frameWatchAttach()
    }

    @ReactMethod
    fun stopFrameWatch() {
        fwOn = false
        reactContext.currentActivity?.runOnUiThread { frameWatchDetachNow() }
    }

    /** {frames, late} since the last call (and resets them); frames = 0 below API 24. */
    @ReactMethod(isBlockingSynchronousMethod = true)
    fun takeFrameWatch(): com.facebook.react.bridge.WritableMap {
        val out = com.facebook.react.bridge.Arguments.createMap()
        val fl = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) (fw as? FrameWatchN)?.take() else null
        out.putInt("frames", fl?.get(0) ?: 0); out.putInt("late", fl?.get(1) ?: 0)
        return out
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

    /** ★★ V12 THE SAME BENCHMARK, ASKED FROM LOCAL HARDWARE (the phone is its own radio — LocalHardwarePanel's
     *  "What this phone can run"). The app pauses its waterfall first and resumes it after; the engine releases the
     *  radio for the run and takes it back (nativeRunBenchmark `ownRadio`). Same result file as runBenchmark, so a
     *  measurement from either screen shows in both. A separate method so the server screen's call is untouched. */
    @ReactMethod
    fun runBenchmarkOwnRadio(wantDab: Boolean, promise: Promise) {
        Thread {
            try {
                val j = VibeBenchmark.run(reactContext, wantDab, ownRadio = true)
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

    /** ★★★ THE SCAN'S ANSWER, KEPT WHERE THE SCREEN CAN ASK FOR IT AGAIN (2026-10-06).
     *  Stuart's Sony: the scan finished in 4.7 s ("11A — 30 station(s) (complete)", "+4744 ms — done") and
     *  the screen went on counting — "SCANNING 11A… 38 s" under a line promising 15 s, and on to 86 s. The
     *  promise was the ONLY road back, and whatever lost it, the screen had no other way to find out. Now the
     *  outcome is also kept here, and the screen's progress poll reads it, so a finished scan is shown as
     *  finished even if the promise never lands. JSON {"ok":true,"body":…} / {"ok":false,"why":…}; null while
     *  a scan runs or before the first. Cleared when a scan starts. */
    @Volatile private var scanOutcome: String? = null
    /** ★ Which scan an outcome belongs to, so the screen can never take the LAST scan's answer for this one. */
    @Volatile private var scanSeq = 0
    private fun keepScanOutcome(ok: Boolean, bodyOrWhy: String) {
        scanOutcome = org.json.JSONObject().apply {
            put("seq", scanSeq); put("ok", ok); put(if (ok) "body" else "why", bodyOrWhy)
        }.toString()
    }
    @ReactMethod
    fun dabScanOutcome(promise: Promise) { promise.resolve(scanOutcome) }

    @ReactMethod
    fun dabQuickScan(block: String, known: Boolean, ownerConfigJson: String, raw: Promise) {
        scanSeq++
        scanOutcome = null
        // ★ Every settle also leaves the outcome for dabScanOutcome — see the note there.
        val promise = object : Promise by raw {
            override fun resolve(value: Any?) { keepScanOutcome(true, value?.toString() ?: ""); raw.resolve(value) }
            override fun reject(code: String?, message: String?) { keepScanOutcome(false, message ?: code ?: "the scan failed"); raw.reject(code, message) }
        }
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
                val livePort = serverLivePort()   // ★ beta88: the server is in its own process
                if (livePort > 0) {
                    phase("scanning $block on the running server")
                    promise.resolve(dabScanHttp(livePort, block, known))
                    phase("done")
                    return@Thread
                }
                if (known) { promise.resolve("{\"ok\":true,\"known\":true,\"services\":[]}"); return@Thread }
                val mgr = usbManager ?: run { promise.reject("no_usb", "USB service unavailable"); return@Thread }
                val dev = mgr.deviceList.values.firstOrNull { isLocalRadio(it)   /* ★ IQ radios only — never an Icom's CP2102 */ }
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
                                                            reactContext.filesDir, serveOnLan = false,
                                                            keepRadioAlive = VibeServerBoot.keepRadioAlive(reactContext, owner))
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

    /** ★★★ A radio that fell over at a rate a mode boosted to (vibe_rate_guard.h): what is recorded, for the red line
     *  beside the rate switch. The files dir backs it whether or not the server is running. */
    @ReactMethod
    fun rateGuardState(promise: Promise) {
        try {
            VibeLocalSDR.setRateGuardBase(VibeServerBoot.rateGuardBase(reactContext.filesDir))
            promise.resolve(VibeLocalSDR.rateGuardJson())
        } catch (t: Throwable) { promise.reject("rate_guard", t.message ?: "$t") }
    }
    /** The owner has read it: forget it, so the rate switch can be ticked again. */
    @ReactMethod
    fun rateGuardClear(promise: Promise) {
        try {
            VibeLocalSDR.setRateGuardBase(VibeServerBoot.rateGuardBase(reactContext.filesDir))
            VibeLocalSDR.rateGuardClear()
            promise.resolve(VibeLocalSDR.rateGuardJson())
        } catch (t: Throwable) { promise.reject("rate_guard", t.message ?: "$t") }
    }

    /* ★★★ V12 THE OWNER'S DAB BAND SCAN — "Scan all DAB blocks" (the Setup page's, Stuart 2026-10-26: "DAB scan I will
     *  add for initial setup"). The ENGINE does it (GET /vibeserver/dab-bandscan — local_sdr_shim.cpp dabBandScanRun:
     *  every Band III block on the idle radio, each station found added to the server's bookmarks). This only gets it a
     *  radio, exactly as dabQuickScan does: the running server over loopback if serving; otherwise the owner's radio on
     *  a PRIVATE loopback-only engine (privateScanConfig) for the length of the scan, stopped and closed at the end. The
     *  bookmarks it adds go to the same vibe_bookmarks.json the real server loads, so listeners find them.
     *  ★ The state is the engine's own JSON (dabBandJsonLocked), kept here for the screen's once-a-second poll. */
    @Volatile private var bandScanJson: String = ""
    @Volatile private var bandScanPort = 0
    @Volatile private var bandScanBusy = false

    private fun dabBandHttp(port: Int, q: String): String {
        val c = java.net.URL("http://127.0.0.1:$port/vibeserver/dab-bandscan" + (if (q.isEmpty()) "" else "?$q"))
            .openConnection() as java.net.HttpURLConnection
        c.connectTimeout = 3000; c.readTimeout = 8000
        c.setRequestProperty("User-Agent", "VibeServer-setup")
        try {
            val code = c.responseCode
            val body = (if (code in 200..299) c.inputStream else c.errorStream)?.bufferedReader()?.use { it.readText() } ?: ""
            if (code !in 200..299) throw java.io.IOException("the server answered $code")
            return body
        } finally { c.disconnect() }
    }
    private fun bandScanFail(why: String) {
        bandScanJson = org.json.JSONObject().apply { put("running", false); put("finished", false); put("error", why) }.toString()
    }

    @ReactMethod
    fun dabBandScanStart(ownerConfigJson: String, promise: Promise) {
        if (bandScanBusy) { promise.resolve(bandScanJson); return }
        bandScanBusy = true
        bandScanJson = "{\"running\":true,\"done\":0,\"total\":0,\"found\":[]}"
        promise.resolve(bandScanJson)   // ★ at once: the screen polls dabBandScanState
        Thread {
            try {
                val livePort = serverLivePort()   // ★ beta88: the server is in its own process
                val poll = { port: Int ->
                    bandScanPort = port
                    var j = org.json.JSONObject(dabBandHttp(port, "start=1"))
                    bandScanJson = j.toString()
                    while (j.optBoolean("running", false)) {
                        Thread.sleep(1000)
                        j = org.json.JSONObject(dabBandHttp(port, ""))
                        bandScanJson = j.toString()
                    }
                    bandScanPort = 0
                }
                if (livePort > 0) { poll(livePort); return@Thread }
                val mgr = usbManager ?: run { bandScanFail("USB service unavailable"); return@Thread }
                val dev = mgr.deviceList.values.firstOrNull { isLocalRadio(it) }
                    ?: run { bandScanFail("No SDR found — plug the radio in to scan"); return@Thread }
                if (!mgr.hasPermission(dev)) { bandScanFail("Allow this app to use the radio (start the server once), then scan again"); return@Thread }
                stopSpectrumInternal()
                val conn = mgr.openDevice(dev) ?: run { bandScanFail("The radio could not be opened"); return@Thread }
                try {
                    val owner = try { org.json.JSONObject(ownerConfigJson) } catch (t: Throwable) {
                        bandScanFail("The scan was not given the server's settings: ${t.message}"); return@Thread
                    }
                    val cfg = VibeServerBoot.privateScanConfig(owner)
                    VibeLocalSDR.setUsbModelName(VibeServerBoot.usbModelName(dev))
                    val port = VibeServerBoot.applyAndStart(cfg, conn.fileDescriptor, dev.vendorId, dev.productId,
                                                            reactContext.filesDir, serveOnLan = false,
                                                            keepRadioAlive = VibeServerBoot.keepRadioAlive(reactContext, owner))
                    if (port <= 0) { bandScanFail("The radio would not start for the scan"); return@Thread }
                    Log.i(TAG, "DAB band scan: private engine on the owner's settings, port $port")
                    poll(port)
                } finally {
                    bandScanPort = 0
                    try { VibeLocalSDR.stopSpectrumSync() } catch (t: Throwable) { Log.w(TAG, "band scan: stop failed: ${t.message}") }
                    try { conn.close() } catch (t: Throwable) { Log.w(TAG, "band scan: close failed: ${t.message}") }
                }
            } catch (t: Throwable) {
                Log.w(TAG, "DAB band scan: ${t.message}")
                bandScanFail(t.message ?: "$t")
            } finally { bandScanBusy = false }
        }.start()
    }
    /** The scan's state — the engine's JSON ("" before any scan). `busy` is ours: the radio is still being put away. */
    @ReactMethod
    fun dabBandScanState(promise: Promise) {
        val m = Arguments.createMap()
        m.putString("json", bandScanJson); m.putBoolean("busy", bandScanBusy)
        promise.resolve(m)
    }
    @ReactMethod
    fun dabBandScanCancel(promise: Promise) {
        val port = bandScanPort
        Thread {
            try { if (port > 0) dabBandHttp(port, "cancel=1") } catch (t: Throwable) { Log.w(TAG, "band scan cancel: ${t.message}") }
            promise.resolve(true)
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
        // ★ Both halves (the engine's and the restore config's) are the server process's — VibeServerOps.set.
        if (!serverProcessUp()) { promise.resolve(true); return }
        ask("set", JSONObject().put("what", "dabScanLabels").put("mode", mode), promise) { true }
    }

    /** ★★★ KEEP RADIO ALIVE, APPLIED NOW (2026-10-06) — the same lesson as setDabScanLabels above: a TV server runs
     *  for days, so a switch that waits for the next start looks broken. Both halves: the engine's (never release
     *  while serving) and the restore config's, which is where VibeServerRestore reads the give-up half from. */
    @ReactMethod
    fun setKeepRadioAlive(on: Boolean, promise: Promise) {
        if (!serverProcessUp()) { promise.resolve(true); return }
        ask("set", JSONObject().put("what", "keepRadioAlive").put("on", on), promise) { true }
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
        val dev = mgr.deviceList.values.firstOrNull { isLocalRadio(it)   /* ★ IQ radios only — never an Icom's CP2102 */ }
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

    // ★ The multicast lock, the host slug and the LAN address moved to VibeServerOps with the server (beta88).

    /**
     * Open the first attached RTL-SDR and start the local-SDR spectrum server.
     * Resolves with { port, wsBaseUrl } so JS can point UberSDRClient at
     * ws://127.0.0.1:<port>. Requests USB permission first if needed.
     */
    @ReactMethod
    fun startSpectrum(opts: com.facebook.react.bridge.ReadableMap, promise: Promise) {
        val mgr = usbManager ?: run { promise.reject("no_usb", "USB service unavailable"); return }
        val dev = mgr.deviceList.values.firstOrNull { isLocalRadio(it) }
            ?: run {
                promise.reject("no_device",
                    if (mgr.deviceList.values.any { isIcom(it) }) "An Icom is shared through VibeServer — start the server and listen to it there"
                    else "No SDR found")
                return
            }
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
        /* ★★ NOT WHILE THIS PHONE'S SERVER HAS THE RADIO (beta88). Both used to be one engine in one process, and starting
         *  local listening simply stopped it — the server, its notification still up, gone. Now the server is a process of
         *  its own and would keep the radio; say so instead of fighting it for the USB interface. */
        if (serverProcessUp() && VibeServerLink.call(reactContext, "isServing").optBoolean("serving", false)) {
            promise.reject("server_running", "This phone is sharing the radio as a VibeServer — listen to it as a server, " +
                "or stop the server first")
            return
        }
        // Learned bookmarks persist for LOCAL listening too — the shim learns whenever
        // it runs, not only when serving.
        VibeLocalSDR.setBookmarksPath(java.io.File(reactContext.filesDir, "vibe_bookmarks.json").absolutePath)
        stopSpectrumInternal()
        val conn = mgr.openDevice(dev)
            ?: run { promise.reject("open_failed", "openDevice returned null"); return }
        val fd = conn.fileDescriptor
        if (fd < 0) { conn.close(); promise.reject("bad_fd", "Invalid file descriptor"); return }
        // ★★ An RSP goes to OpenRSP's service, not to the engine — see VibeRsp and LocalSdrShim::start.
        if (isRsp(dev)) VibeRsp.attach(reactContext, conn, dev)?.let { conn.close(); promise.reject("rsp", it); return }
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
            isRsp(dev) -> "sdrplay"
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
            ?: mgr.deviceList.values.firstOrNull { VibeIcom.unconfirmed(mgr, it) }   // ★ an Icom Android will not name yet
            ?: run { promise.reject("no_device", "No SDR found"); return }
        if (!mgr.hasPermission(dev)) {
            openAndProbeThen(mgr, dev, promise) {
                /* ★★ A CP2102 IS ONLY AN ICOM ONCE ITS SERIAL SAYS SO — readable now the owner has granted it (Android
                 *    10+ hides it until then). Not one: say so plainly and touch nothing; it is somebody's GPS. */
                if (VibeIcom.isCandidate(dev) && !isIcom(dev))
                    promise.reject("not_icom", "That USB serial device is not an Icom radio VibeServer supports " +
                        "(it says \"${VibeIcom.serialOf(dev) ?: dev.productName ?: "nothing"}\") — it was not used")
                else startVibeServerNow(mgr, dev, opts, promise)
            }
            return
        }
        startVibeServerNow(mgr, dev, opts, promise)
    }

    /**
     * ★★★ THE START ITSELF RUNS IN THE SERVER'S PROCESS (beta88) — VibeServerOps.startVibeServer, the same body that lived
     * here, moved so the server outlives this process. This side has done the part that needs an Activity (the USB
     * permission prompt, and the Icom check that needs it) and stops its own Local Hardware engine, which would otherwise
     * be holding the very radio the server is about to open.
     */
    private fun startVibeServerNow(
        mgr: UsbManager, dev: UsbDevice,
        opts: com.facebook.react.bridge.ReadableMap, promise: Promise
    ) {
        serverWork.execute {
            stopSpectrumInternal()   // ★ this process's Local Hardware engine lets go of the radio first
            // ★★★ THE CONFIG TRAVELS WHOLE (VibeServerBoot) — `toHashMap` gives plain JSON-shaped values.
            val cfg = JSONObject(opts.toHashMap() as Map<*, *>)
            val args = JSONObject().put("cfg", cfg).put("vid", dev.vendorId).put("pid", dev.productId)
                .put("deviceName", dev.deviceName)
            // ★ A start can take a while on a slow box (the engine, the map unpack): wait for it as the old call did.
            val r = VibeServerLink.call(reactContext, "start", args, waitMs = 15_000)
            if (!r.optBoolean("ok", false)) {
                promise.reject(r.optString("code", "start_failed"), r.optString("message", "the server did not start"))
                return@execute
            }
            /* ★★★ AN ICOM'S AUDIO — the permission is asked for HERE, with the app in front (Android's while-in-use rule);
             *     the answer goes to the server process (VibeIcom.askForMicIfNeeded → VibeServerLink). */
            if (r.optBoolean("icom", false)) {
                if (VibeIcom.hasMicPermission(reactContext)) VibeServerLink.callAsync(reactContext, "icomMicGranted")
                else VibeIcom.askForMicIfNeeded(reactContext.currentActivity, reactContext)
            }
            val res = Arguments.createMap()
            res.putString("ip", r.optString("ip", "0.0.0.0"))
            res.putInt("port", r.optInt("port", 0))
            res.putString("name", r.optString("name", ""))
            promise.resolve(res)
        }
    }

    @ReactMethod
    fun stopVibeServer(promise: Promise) {
        // ★ Disarm, mDNS, NSD, the service, the engine, the secret, the public door — all in the server's process now
        //   (VibeServerOps.stopVibeServer), in the same order as before. Nothing to do when its process is not running.
        if (!serverProcessUp()) { promise.resolve(null); return }
        ask("stop", JSONObject(), promise, serial = true) { null }
    }

    /** Live toggle: switch compressed audio on/off without restarting the server. */
    @ReactMethod
    fun setVibeServerCompressAudio(on: Boolean) { tell("compressAudio", JSONObject().put("on", on)) }

    /** Live, no restart — the operator can lock the controls on a running server. */
    @ReactMethod
    fun setVibeServerAdminSecret(secret: String) { tell("adminSecret", JSONObject().put("secret", secret)) }

    @ReactMethod
    fun setVibeServerUncompressedAudio(mode: Double) {
        tell("uncompressed", JSONObject().put("mode", mode.toInt()))
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
        // ★ An Icom is named by its model, never by the CP2102 it talks through ("Silicon Labs CP2102 USB to UART…").
        VibeIcom.identify(dev)?.let { return "Icom $it" }
        val fallback = when {
            isRsp(dev)      -> VibeRsp.modelName(dev.productId)
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
            ?: mgr.deviceList.values.firstOrNull { VibeIcom.unconfirmed(mgr, it) }
            ?: run { promise.resolve(null); return }
        val res = Arguments.createMap()
        /* ★★★ AN ICOM, OR MAYBE ONE. `icomConfirmed` false = a CP210x Android will not identify until the owner grants it
         *     (Android 10+ hides its serial): the screen should say "an Icom? press Start and allow it", and Start checks
         *     before it uses it. ★ "icom" before the else — a CP2102 is not a dongle ("else means dongle"). */
        if (VibeIcom.isCandidate(dev)) {
            res.putString("driver", "icom")
            res.putBoolean("icomConfirmed", isIcom(dev))
            res.putString("model", if (isIcom(dev)) radioModelName(dev) else "Icom radio? (USB serial — press Start and allow it to check)")
            res.putBoolean("micPermission", VibeIcom.hasMicPermission(reactContext))
            promise.resolve(res)
            return
        }
        res.putString("driver", when {
            isRsp(dev)      -> "sdrplay"    // ★ the same name every VibeServer reports for an RSP
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

    /** ★★★ THE ICOM'S SETUP FACTS for the server screen — JSON (VibeIcom.facts): the engine's own hwinfo `radio`
     *  (model, CI-V address, baud, transport, linesHeldLow, the preflight with each menu item and where it is, audio and
     *  audioProblem) plus what only Android knows (micPermission, audioRunning, audioDevice). "{}"-shaped with only the
     *  Android half when no Icom server is running. Read-only; nothing is opened. */
    @ReactMethod
    fun icomSetup(promise: Promise) {
        // ★ The engine's half is the server process's; with no server running there is only the Android half.
        if (!serverProcessUp()) { promise.resolve(VibeIcom.facts(reactContext).toString()); return }
        ask("icomFacts", JSONObject(), promise) { it.optString("json", "{}") }
    }

    /** ★ The owner tapped "allow the radio's audio" on the setup card — the same explained ask as at Start. Does
     *  nothing unless an Icom is being served. */
    @ReactMethod
    fun icomAskForAudio() {
        if (!serverProcessUp()) return
        Thread {
            if (!VibeServerLink.call(reactContext, "icomServing").optBoolean("serving", false)) return@Thread
            if (VibeIcom.hasMicPermission(reactContext)) VibeServerLink.callAsync(reactContext, "icomMicGranted")
            else VibeIcom.askForMicIfNeeded(reactContext.currentActivity, reactContext)
        }.start()
    }

    @ReactMethod
    fun setVibeServerSessionLimit(minutes: Double) {
        tell("sessionLimit", JSONObject().put("minutes", minutes.toInt()))
    }

    /** Hand the web client's search its station list (JSON array), served at
     *  GET /stations. The app owns the EiBi download + cache; the browser can't
     *  fetch eibispace.de itself (no CORS headers there), and this also means the
     *  search still works with no internet — the allotment case. */
    @ReactMethod
    fun setStationsJson(json: String) {
        // ★ The server's copy (and the restore's cache) — large, so it may travel as a file (VibeServerLink.wrap).
        if (serverProcessUp()) tell("stations", JSONObject().put("json", json))
        else VibeServerRestore.cacheStations(reactContext, json)
    }

    /** Publish the RECEIVER's coarse location (GET /location). Clients compute
     *  spot distances, map centring and the ITU region from the ANTENNA's
     *  position — not from wherever the listener happens to be. */
    @ReactMethod
    fun setLocationJson(json: String) {
        // ★ Local Hardware's own engine wants it too (its spots map), and the server's (its listeners').
        try { VibeLocalSDR.setLocationJson(json) } catch (_: Throwable) {}
        if (serverProcessUp()) tell("location", JSONObject().put("json", json))
    }

    /** Learned station bookmarks (RDS). The shim learns them; JS persists them. */
    @ReactMethod
    fun setBookmarksJson(json: String) {
        // ★ Whichever engine is running: the server's when this phone serves, else Local Hardware's own.
        if (serverProcessUp()) tell("bookmarks", JSONObject().put("json", json)) else VibeLocalSDR.setBookmarksJson(json)
    }

    @ReactMethod
    fun getBookmarksJson(promise: Promise) {
        if (!serverProcessUp()) { promise.resolve(VibeLocalSDR.getBookmarksJson()); return }
        ask("bookmarks.get", JSONObject(), promise) { it.optString("json", "[]") }
    }

    @ReactMethod
    fun clearBookmarks(promise: Promise) {
        try { VibeLocalSDR.clearBookmarks() } catch (_: Throwable) {}
        if (!serverProcessUp()) { promise.resolve(null); return }
        ask("bookmarks.clear", JSONObject(), promise) { null }
    }

    /** The .local hostname the responder actually took — it renames itself on a clash,
     *  so this is not necessarily the one we asked for. */
    @ReactMethod
    fun getMdnsHostname(promise: Promise) {
        if (!serverProcessUp()) { promise.resolve(""); return }
        ask("mdnsHostname", JSONObject(), promise) { it.optString("host", "") }
    }

    // ★ The process CPU figure moved to VibeServerOps (beta88): measured in the server's own process.

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
        // ★★ beta88: the server's own process answers (VibeServerOps.status) — its CPU figure is now the SERVER's cost.
        //    No server process at all: "not running", without creating one to ask.
        if (!serverProcessUp()) { promise.resolve(statusMap(JSONObject().put("ok", true).put("status", JSONObject()))); return }
        Thread {
            val r = VibeServerLink.call(reactContext, "status")
            if (!r.optBoolean("ok", false)) {
                Log.w(TAG, "getVibeServerStatus failed: ${r.optString("message")}")
                promise.reject("status_failed", r.optString("message"))
            } else promise.resolve(statusMap(r))
        }.start()
    }

    /** ★ Field by field, as before — a field missing here is dropped on its way to the screen (see the old notes). */
    private fun statusMap(r: JSONObject): WritableMap {
        val o = r.optJSONObject("status") ?: JSONObject()
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
        m.putDouble("sampleRate", o.optLong("sampleRate", 0).toDouble())
        m.putInt("port", o.optInt("port", 0))
        m.putInt("listeners", o.optInt("listeners", 0))
        m.putInt("maxUsers", o.optInt("maxUsers", 1))
        m.putString("radioProblem", o.optString("radioProblem", ""))
        m.putString("ip", r.optString("ip", ""))
        m.putDouble("cpu", r.optDouble("cpu", 0.0))
        m.putInt("cores", r.optInt("cores", 0))
        return m
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
    // ★ beta88: run by the server's process, like VibeServer (VibeServerOps.rtlStart) — the same foreground service.

    @ReactMethod
    fun startRtlTcpServer(opts: com.facebook.react.bridge.ReadableMap, promise: Promise) {
        val mgr = usbManager ?: run { promise.reject("no_usb", "USB service unavailable"); return }
        val dev = mgr.deviceList.values.firstOrNull { isLocalRadio(it)   /* ★ IQ radios only — never an Icom's CP2102 */ }
            ?: run { promise.reject("no_device", "No SDR found"); return }
        if (!mgr.hasPermission(dev)) {
            openAndProbeThen(mgr, dev, promise) { startRtlTcpServerNow(dev, opts, promise) }
            return
        }
        startRtlTcpServerNow(dev, opts, promise)
    }

    private fun startRtlTcpServerNow(dev: UsbDevice, opts: com.facebook.react.bridge.ReadableMap, promise: Promise) {
        serverWork.execute {
            stopSpectrumInternal()
            val args = JSONObject().put("opts", JSONObject(opts.toHashMap() as Map<*, *>))
                .put("vid", dev.vendorId).put("pid", dev.productId).put("deviceName", dev.deviceName)
            val r = VibeServerLink.call(reactContext, "rtl.start", args, waitMs = 15_000)
            if (!r.optBoolean("ok", false)) { promise.reject(r.optString("code", "start_failed"), r.optString("message")); return@execute }
            val res = Arguments.createMap()
            res.putString("ip", r.optString("ip")); res.putInt("port", r.optInt("port")); res.putString("name", r.optString("name"))
            promise.resolve(res)
        }
    }

    @ReactMethod
    fun stopRtlTcpServer(promise: Promise) {
        if (!serverProcessUp()) { promise.resolve(null); return }
        ask("rtl.stop", JSONObject(), promise, serial = true) { null }
    }

    @ReactMethod
    fun setServerSampleRate(rate: Double) { tell("rtlRate", JSONObject().put("rate", rate)) }

    @ReactMethod
    fun getServerStatus(promise: Promise) {
        val empty = { r: JSONObject ->
            val o = r.optJSONObject("status") ?: JSONObject()
            val m = Arguments.createMap()
            m.putBoolean("running", o.optBoolean("running", false))
            m.putBoolean("client", o.optBoolean("client", false))
            m.putString("clientAddr", o.optString("clientAddr", ""))
            m.putDouble("sampleRate", o.optLong("sampleRate", 0).toDouble())
            m.putDouble("overrideRate", o.optLong("overrideRate", 0).toDouble())
            m.putDouble("droppedBytes", o.optLong("droppedBytes", 0).toDouble())
            m.putInt("port", o.optInt("port", 0))
            m.putString("ip", r.optString("ip", ""))
            m.putDouble("cpu", r.optDouble("cpu", 0.0))
            m.putInt("cores", r.optInt("cores", 0))
            m
        }
        if (!serverProcessUp()) { promise.resolve(empty(JSONObject())); return }
        ask("rtl.status", JSONObject(), promise) { empty(it) }
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
    /** ★ V12 the region defaults of the decoder switches (SAME off outside the Americas, DAB off in them) — JSON. */
    @ReactMethod fun getRegionDefaults(promise: Promise) {
        try { promise.resolve(VibeLocalSDR.regionDefaultsJson()) }
        catch (t: Throwable) { promise.reject("region", t) }
    }

    @ReactMethod fun startDecoderService(promise: Promise) { promise.resolve(VibeLocalSDR.startDecoderService()) }
    @ReactMethod fun feedDecoderPcm(b64: String, rate: Double) { VibeLocalSDR.feedDecoderPcm(b64, rate) }
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
        // ★ An Icom's AudioRecord thread stops pushing BEFORE the source it pushes into is torn down.
        VibeIcom.onServerStopping()
        try { VibeLocalSDR.stopSpectrumSync() } catch (_: Throwable) {}
        // ★ After the engine has stopped: the RSP driver's devices are let go (nothing is streaming from them now).
        VibeRsp.releaseAll(reactContext)
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
        // ★ beta88: NOT the server — it lives in its own process precisely so that this one going away leaves it running.
        super.invalidate()
    }

    // ── ADVERTISE ON VIBESDR.NET ──────────────────────────────────────────────────────────────
    // ★★★ EVERY ONE OF THESE NEEDS ITS @ReactMethod. See getVibeServerStatus above: without the
    //     annotation the method simply is not exported, and `Local?.foo?.()` returns UNDEFINED
    //     rather than throwing — two days were lost to exactly that, twice.

    /* ★★★ THE LISTING'S NETWORK WORK NEVER RUNS ON THE BRIDGE THREAD (2026-10-06).
     *  tunnelStart / tunnelRepublish / tunnelStop did their HTTP inline — two loopback reads of the
     *  server's own status plus a POST to vibesdr.net, on OkHttp timeouts of 15 s to connect and 30 s
     *  to read (seen on the Sony's logcat, 20:22:30: VibeTunnel.buildStatus inside tunnelRepublish on
     *  MessageQueueThreadImpl). On the old architecture EVERY module's methods share that one thread
     *  (mqt_native_modules) — AsyncStorage's reads, the DAB scan, the scan's progress poll, the server
     *  status — so a slow directory or a server not answering on 48000 froze all of them behind it.
     *  ★ One SERIAL worker, so a start, a republish and a stop still happen in the order they were
     *    asked for. The promise resolves when the work is done, exactly as before. */
    private val tunnelWork = java.util.concurrent.Executors.newSingleThreadExecutor { r ->
        Thread(r, "vibe-tunnel-ui").apply { isDaemon = true }
    }

    /** ★ Is the tunnel binary in this build at all? arm64 only — the switch must be ABSENT, not
     *  inert, where it cannot work (AGENTS.md). */
    @ReactMethod
    fun tunnelSupported(promise: Promise) {
        // ★ A file check — answered here, without waking the server process.
        try { promise.resolve(VibeTunnel.isSupported(reactApplicationContext)) }
        catch (t: Throwable) { promise.resolve(false) }
    }

    @ReactMethod
    fun tunnelStatus(promise: Promise) {
        if (!serverProcessUp()) { promise.resolve(VibeTunnel.statusJson()); return }
        ask("tunnel.status", JSONObject(), promise) { it.optString("json", "{}") }
    }

    /**
     * Start the tunnel, then list the server — in the SERVER's process (beta88): cloudflared lives and dies with the
     * process that spawned it, and that must be the one that survives.
     * ★★★ THE TRUSTED-PROXY WIRING IS PART OF THIS ACTION — see VibeServerOps.tunnelStart.
     */
    @ReactMethod
    fun tunnelStart(name: String, locator: String, port: Int, ownerProxies: String,
                    radioModel: String, radioDriver: String,
                    antenna: String, coverage: String, locked: Boolean,
                    shareForSec: Double, promise: Promise) {
        tunnelWork.execute {
            val r = VibeServerLink.call(reactContext, "tunnel.start", JSONObject()
                .put("name", name).put("locator", locator).put("port", port).put("ownerProxies", ownerProxies)
                .put("radioModel", radioModel).put("radioDriver", radioDriver).put("antenna", antenna)
                .put("coverage", coverage).put("locked", locked).put("shareForSec", shareForSec.toLong()), waitMs = 15_000)
            if (r.optBoolean("ok", false)) promise.resolve(r.optString("json", "{}"))
            else promise.reject("tunnel_start", r.optString("message"))
        }
    }

    /** Re-publish the listing from the values the app holds RIGHT NOW (see VibeServerOps.tunnelRepublish). */
    @ReactMethod
    fun tunnelRepublish(name: String, locator: String, port: Int,
                        radioModel: String, radioDriver: String,
                        antenna: String, coverage: String, locked: Boolean,
                        shareForSec: Double, promise: Promise) {
        if (!serverProcessUp()) { promise.resolve(VibeTunnel.statusJson()); return }
        tunnelWork.execute {
            val r = VibeServerLink.call(reactContext, "tunnel.republish", JSONObject()
                .put("name", name).put("locator", locator).put("port", port)
                .put("radioModel", radioModel).put("radioDriver", radioDriver).put("antenna", antenna)
                .put("coverage", coverage).put("locked", locked).put("shareForSec", shareForSec.toLong()))
            if (r.optBoolean("ok", false)) promise.resolve(r.optString("json", "{}"))
            else promise.reject("tunnel_republish", r.optString("message"))
        }
    }

    /** ★ Off means OFF: delist immediately so the public address is freed now, not at expiry. */
    @ReactMethod
    fun tunnelStop(ownerProxies: String, promise: Promise) {
        tunnelWork.execute {
            val r = VibeServerLink.call(reactContext, "tunnel.stop", JSONObject().put("ownerProxies", ownerProxies))
            if (r.optBoolean("ok", false)) promise.resolve(r.optString("json", "{}"))
            else promise.reject("tunnel_stop", r.optString("message"))
        }
    }

    companion object {
        // ★ The allowlists live in VibeRadioIds (beta88) — the server process reads them without React Native.
        internal val RTL_SDR_VIDPIDS: Set<Int> get() = VibeRadioIds.RTL_SDR_VIDPIDS
        internal const val AIRSPYHF_VID = VibeRadioIds.AIRSPYHF_VID
        internal const val AIRSPYHF_PID = VibeRadioIds.AIRSPYHF_PID
        internal const val HACKRF_VID = VibeRadioIds.HACKRF_VID
        internal const val HACKRF_PID = VibeRadioIds.HACKRF_PID
        internal const val AIRSPY_VID = VibeRadioIds.AIRSPY_VID
        internal const val AIRSPY_PID = VibeRadioIds.AIRSPY_PID
        internal fun isServableRadio(vid: Int, pid: Int): Boolean = VibeRadioIds.isServableRadio(vid, pid)
    }
}

/** FRAME WATCH for FRAME RATE → AUTO (constants/faceplate.ts autoWindowLate) — counts frames and LATE frames from the
 *  system's own per-frame timings (Window.OnFrameMetricsAvailableListener, API 24): the numbers `dumpsys gfxinfo`
 *  counts as janky, not a guess from the model or a JS timer. LATE = total time from the intended vsync over 1.5
 *  refresh intervals (it missed its slot). JS takes the counts every few seconds and decides; this only counts, on its
 *  own small thread so counting never touches the UI thread. Re-attached on resume (a recreated Activity has a new
 *  window).
 *  ★★★ Its own class so FrameMetrics is only ever loaded on Android 7+ — see the note at VibeLocalSdrModule.fw. */
@androidx.annotation.RequiresApi(Build.VERSION_CODES.N)
private class FrameWatchN(private val thread: android.os.HandlerThread) {
    @Volatile private var frames = 0
    @Volatile private var late = 0
    private var window: android.view.Window? = null
    private var listener: android.view.Window.OnFrameMetricsAvailableListener? = null

    /** On the UI thread. Same window → nothing to do. */
    fun attach(w: android.view.Window, lateNs: Long) {
        if (window === w && listener != null) return
        detach()
        val l = android.view.Window.OnFrameMetricsAvailableListener { _, m, _ ->
            frames++
            if (m.getMetric(android.view.FrameMetrics.TOTAL_DURATION) > lateNs) late++
        }
        w.addOnFrameMetricsAvailableListener(l, android.os.Handler(thread.looper))
        window = w; listener = l
    }

    /** On the UI thread. */
    fun detach() {
        val w = window; val l = listener
        window = null; listener = null
        if (w != null && l != null) try { w.removeOnFrameMetricsAvailableListener(l) } catch (_: Throwable) {}
    }

    /** [frames, late] since the last call, and resets them. */
    fun take(): IntArray { val r = intArrayOf(frames, late); frames = 0; late = 0; return r }
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
