package com.vibesdr.app

import android.content.Intent
import android.os.Build
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod

/**
 * JS bridge — exposed as "VibePowerModule" to mirror the iOS module, so
 * AudioPlayer/SDRScreen drive ONE API on both platforms. The engine itself
 * lives in VibeStreamService (foreground service keeps audio alive in the
 * background); startAudioEngine goes via startForegroundService, everything
 * else through the running service instance.
 */
class VibeStreamModule(private val reactContext: ReactApplicationContext) :
    ReactContextBaseJavaModule(reactContext) {

    init {
        VibeStreamService.reactContext = reactContext
    }

    override fun getName() = "VibePowerModule"

    /** The owner's admin credential for the audio socket. Set before starting (or before a
     *  reconnect) — see VibeStreamService.adminSuffix. Empty clears it. */
    @ReactMethod
    fun setAdminAuth(q: String) {
        VibeStreamService.adminSuffix =
            if (q.isEmpty() || q.startsWith("&")) q else "&$q"
    }

    @ReactMethod
    fun startAudioEngine(baseUrl: String, frequency: Double, mode: String, uuid: String, password: String) {
        VibeStreamService.reactContext = reactContext
        val intent = Intent(reactContext, VibeStreamService::class.java).putExtra(VibeStreamService.EXTRA_CONTROL_TOKEN, VibeStreamService.CONTROL_TOKEN).apply {
            action = VibeStreamService.ACTION_START
            putExtra(VibeStreamService.EXTRA_BASE_URL, baseUrl)
            putExtra(VibeStreamService.EXTRA_FREQUENCY, frequency.toLong())
            putExtra(VibeStreamService.EXTRA_MODE, mode)
            putExtra(VibeStreamService.EXTRA_UUID, uuid)
            putExtra(VibeStreamService.EXTRA_PASSWORD, password)
        }
        // If the service is already running (the reconnect case: AudioPlayer does
        // stopAudioEngine then startAudioEngine on a fresh session uuid), use
        // startService so this START stays FIFO-ordered with stopAudioEngine's
        // startService(ACTION_STOP). startForegroundService uses a different,
        // prioritised queue, so the START jumped ahead of the STOP and the STOP
        // then killed the just-started engine → frozen "go back to instances" state.
        if (VibeStreamService.instance != null) {
            reactContext.startService(intent)
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            reactContext.startForegroundService(intent)
        } else {
            reactContext.startService(intent)
        }
    }

    @ReactMethod
    fun stopAudioEngine() {
        reactContext.startService(
            Intent(reactContext, VibeStreamService::class.java).putExtra(VibeStreamService.EXTRA_CONTROL_TOKEN, VibeStreamService.CONTROL_TOKEN).apply {
                action = VibeStreamService.ACTION_STOP
            }
        )
    }

    // External PCM audio (OWRX/Kiwi): start the foreground service in external
    // mode; pushExternalPcm/stopExternalAudio go straight to the live instance.
    @ReactMethod
    fun startExternalAudio(sampleRate: Double, pauseMode: String = "release") {
        VibeStreamService.reactContext = reactContext
        val intent = Intent(reactContext, VibeStreamService::class.java).putExtra(VibeStreamService.EXTRA_CONTROL_TOKEN, VibeStreamService.CONTROL_TOKEN).apply {
            action = VibeStreamService.ACTION_START_EXTERNAL
            putExtra(VibeStreamService.EXTRA_RATE, sampleRate.toInt())
            putExtra(VibeStreamService.EXTRA_PAUSE_MODE, pauseMode)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) reactContext.startForegroundService(intent)
        else reactContext.startService(intent)
    }

    @ReactMethod
    fun pushExternalPcm(base64: String, sampleRate: Double, channels: Double) {
        VibeStreamService.instance?.pushExternalPcm(base64, sampleRate.toInt(), channels.toInt())
    }

    /** ★ Widen the external player's cushion for a decoder that emits audio in BLOCKS (DRM, DAB,
     *  HD Radio, digital voice). 0 restores the analogue default. See VibeStreamService.extBurstMs
     *  — and note the @ReactMethod: without it this silently returns undefined, which this bridge
     *  has been caught by twice. */
    @ReactMethod
    fun setAudioBurstDepth(ms: Double) {
        VibeStreamService.instance?.setAudioBurstDepth(ms.toInt())
    }

    @ReactMethod
    fun stopExternalAudio() { VibeStreamService.instance?.stopExternalAudio() }

    // Exclude a full-width horizontal band (the VFO/zoom drums) from the system
    // back-edge swipe so a horizontal drag on a drum doesn't trigger the (useless,
    // in-app-handled) Android back gesture — which animated and blocked the drum
    // until it released. top/height are in dp; height<=0 clears the exclusion.
    @ReactMethod
    fun setSwipeExclusion(topDp: Double, heightDp: Double) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        val act = reactContext.currentActivity ?: return
        act.runOnUiThread {
            try {
                val dm = act.resources.displayMetrics
                val root = act.window.decorView
                if (heightDp <= 0) {
                    root.systemGestureExclusionRects = emptyList()
                } else {
                    val top = (topDp * dm.density).toInt()
                    val h = (heightDp * dm.density).toInt()
                    root.systemGestureExclusionRects =
                        listOf(android.graphics.Rect(0, top, dm.widthPixels, top + h))
                }
            } catch (_: Throwable) {}
        }
    }

    // Local hardware (V4): the foreground service reads the on-device shim's
    // /ws/audio natively (background-safe), so JS no longer pushes PCM. JS just
    // starts/stops it and forwards tune changes over the same WS.
    @ReactMethod
    fun startLocalAudio(host: String, port: Double, initialTune: String, authSuffix: String,
                        wsBase: String) {
        VibeStreamService.reactContext = reactContext
        val intent = Intent(reactContext, VibeStreamService::class.java).putExtra(VibeStreamService.EXTRA_CONTROL_TOKEN, VibeStreamService.CONTROL_TOKEN).apply {
            action = VibeStreamService.ACTION_START_LOCAL
            putExtra(VibeStreamService.EXTRA_HOST, host)
            putExtra(VibeStreamService.EXTRA_PORT, port.toInt())
            putExtra(VibeStreamService.EXTRA_TUNE, initialTune)
            putExtra(VibeStreamService.EXTRA_AUTH, authSuffix)
            // ★★★ A FULL ws BASE, because host+port CANNOT EXPRESS what this now has to reach: a
            //     multi-radio VibeServer puts every radio behind `/r/<id>/…`, and a public one is
            //     https (so wss). Rebuilding "ws://host:port" here dropped both — the audio went
            //     to the front door, which owns no radio, so the waterfall was perfect and there
            //     was no sound at all (Stuart, 2026-08-12).
            putExtra(VibeStreamService.EXTRA_WSBASE, wsBase)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) reactContext.startForegroundService(intent)
        else reactContext.startService(intent)
    }

    @ReactMethod
    fun sendLocalTune(json: String) { VibeStreamService.instance?.sendLocalTune(json) }

    @ReactMethod
    fun stopLocalAudio() { VibeStreamService.instance?.stopLocalAudio() }

    /** ★★ Is the local pump's socket still delivering? Reopens it if not — see
     *  VibeStreamService.reviveLocalAudio. @ReactMethod or JS cannot see it (a quiet no-op). */
    @ReactMethod
    fun reviveLocalAudio() { VibeStreamService.instance?.reviveLocalAudio() }

    // FM-DX Webserver (v7): the shared tuner's MP3-over-WS audio, consumed +
    // decoded natively (background-safe). JS owns only the /text + /chat sockets.
    @ReactMethod
    fun startFmdxAudio(baseUrl: String) {
        VibeStreamService.reactContext = reactContext
        val intent = Intent(reactContext, VibeStreamService::class.java).putExtra(VibeStreamService.EXTRA_CONTROL_TOKEN, VibeStreamService.CONTROL_TOKEN).apply {
            action = VibeStreamService.ACTION_START_FMDX
            putExtra(VibeStreamService.EXTRA_BASE_URL, baseUrl)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) reactContext.startForegroundService(intent)
        else reactContext.startService(intent)
    }

    @ReactMethod
    fun stopFmdxAudio() { VibeStreamService.instance?.stopFmdxAudio() }

    @ReactMethod
    fun revive() { VibeStreamService.instance?.revive() }

    /** ★★★ Hold the native self-heal across a DAB transition (entering/leaving DAB, a block change,
     *  a service pick) — seconds of frames arriving and nothing playing that are NOT a stall.
     *  ★ @ReactMethod or it does not exist to JS (see noteServerFreq). No service = no heal to hold. */
    @ReactMethod
    fun holdHealing(ms: Double) { VibeStreamService.instance?.holdHealing(ms.toLong()) }

    /** ★★★ ADOPT THE SERVER'S DIAL — A CACHE UPDATE, NOT A CONTROL ACTION. JS calls this when the
     *  server's `config` says where the dial is; it must never reach the wire. Its twin below,
     *  sendTuneCommand, is the ONLY thing that transmits, and only from a user action.
     *  ★ @ReactMethod or it does not exist — a method without it is invisible to JS and the call
     *    silently does nothing, which is how a fix ships and changes no behaviour at all. */
    @ReactMethod
    fun noteServerFreq(frequency: Double, mode: String) {
        VibeStreamService.instance?.noteServerFreq(frequency.toLong(), mode)
    }

    @ReactMethod
    fun sendTuneCommand(frequency: Double, mode: String) {
        VibeStreamService.instance?.sendTuneCommand(frequency.toLong(), mode)
    }

    @ReactMethod
    fun sendBandwidth(low: Double, high: Double) {
        VibeStreamService.instance?.sendBandwidth(low.toLong(), high.toLong())
    }

    @ReactMethod
    fun setStep(hz: Double) { VibeStreamService.instance?.setStep(hz.toLong()) }

    @ReactMethod
    fun setInstanceName(name: String) {
        VibeStreamService.instance?.setInstanceNameNative(name)
    }

    @ReactMethod
    fun setMuted(muted: Boolean) { VibeStreamService.instance?.setMutedNative(muted) }

    @ReactMethod
    fun setVolume(volume: Double) {
        VibeStreamService.instance?.setVolumeNative(volume.toFloat())
    }

    /** Server-NR / squelch / gate commands ride the audio WS (iOS parity). */
    @ReactMethod
    fun sendAudioCommand(json: String) {
        VibeStreamService.instance?.sendRawCommand(json)
    }

    @ReactMethod
    fun setNowPlaying(title: String, artist: String) {
        VibeStreamService.instance?.setNowPlayingNative(title, artist)
    }

    @ReactMethod
    fun setArtwork(serverType: String) {
        VibeStreamService.instance?.setArtworkNative(serverType)
    }

    @ReactMethod
    fun setMediaSkipMode(mode: String) {
        VibeStreamService.instance?.skipMode = mode
    }

    /** ⏮⏭ on/off — a shared dial with others listening switches them off (see skipAllowed). */
    @ReactMethod
    fun setMediaSkipEnabled(enabled: Boolean) {
        VibeStreamService.setSkipAllowed(enabled)
    }

    /** Car browse tree payload (bookmarks + band plan) for Android Auto. */
    @ReactMethod
    fun setBrowseItems(json: String) {
        VibeStreamService.instance?.setBrowseItemsNative(json)
    }

    /** Reconnect attempt failed (server full / rate-limited) — show "open app". */
    @ReactMethod
    fun setReconnectFailed(failed: Boolean) {
        VibeStreamService.instance?.setReconnectFailedNative(failed)
    }

    /** One-shot coarse location for nearest-first instance sorting. JS must
     *  request ACCESS_COARSE_LOCATION via PermissionsAndroid first. */
    @ReactMethod
    fun getLocation(promise: Promise) {
        try {
            val lm = reactContext.getSystemService(android.content.Context.LOCATION_SERVICE)
                as android.location.LocationManager
            var best: android.location.Location? = null
            for (p in lm.getProviders(true)) {
                try {
                    val l = lm.getLastKnownLocation(p) ?: continue
                    if (best == null || l.time > best!!.time) best = l
                } catch (_: SecurityException) { /* not granted */ }
            }
            val b = best
            if (b != null) {
                val map = com.facebook.react.bridge.Arguments.createMap()
                map.putDouble("lat", b.latitude)
                map.putDouble("lon", b.longitude)
                promise.resolve(map)
            } else {
                promise.resolve(null)
            }
        } catch (e: Exception) {
            promise.resolve(null)
        }
    }

    /** Whether this device actually has a vibrator/haptic motor. Some Android
     *  tablets have none, so JS hides the HAPTICS toggle when this is false. */
    @ReactMethod
    fun hasVibrator(promise: Promise) {
        try {
            val has = if (android.os.Build.VERSION.SDK_INT >= 31) {
                val vm = reactContext.getSystemService(android.content.Context.VIBRATOR_MANAGER_SERVICE)
                    as android.os.VibratorManager
                vm.defaultVibrator.hasVibrator()
            } else {
                @Suppress("DEPRECATION")
                val v = reactContext.getSystemService(android.content.Context.VIBRATOR_SERVICE)
                    as android.os.Vibrator
                v.hasVibrator()
            }
            promise.resolve(has)
        } catch (e: Exception) {
            promise.resolve(true) // assume present on error — safer than hiding a working toggle
        }
    }

    // Client NR/NR2/NB — VibeDSP.kt engines in the service decode path
    @ReactMethod
    fun setNrMode(mode: String) {
        VibeStreamService.instance?.let { it.nrMode = mode; it.requestDspReset() }
    }

    @ReactMethod
    fun setNoiseBlanker(on: Boolean) {
        VibeStreamService.instance?.let { it.nbOn = on; it.requestDspReset() }
    }

    // Auto notch (NLMS) — client-side, network backends (UberSDR/OWRX/Kiwi).
    @ReactMethod
    fun setNotch(on: Boolean) {
        VibeStreamService.instance?.setNotchOn(on)
    }

    // Client-side audio squelch gate (Kiwi etc.) — JS opens/closes from the meter.
    @ReactMethod
    fun setSquelchOpen(open: Boolean) {
        VibeStreamService.instance?.setSquelchOpen(open)
    }

    // Recorder — MediaCodec AAC + MediaMuxer on the service decode thread
    @ReactMethod
    fun startRecording(frequency: Double, mode: String, promise: Promise) {
        val svc = VibeStreamService.instance
        if (svc == null) promise.reject("not_running", "Audio engine is not running")
        else svc.startRecordingNative(frequency, mode, promise)
    }

    @ReactMethod
    fun stopRecording(promise: Promise) {
        val svc = VibeStreamService.instance
        if (svc == null) promise.resolve(null)
        else svc.stopRecordingNative(promise)
    }

    @ReactMethod
    fun shareRecording(path: String) {
        VibeStreamService.instance?.shareRecordingNative(path)
    }

    /* ★★ B19 PERF OVERLAY (src/constants/perfOverlay.ts — this build only, Stuart 2026-10-02): this process's
     *  CPU %, its memory, and the UI frame rate / frame times, so the tilt light can be judged by toggling MOTION
     *  EFFECTS and watching the figures. Called once a second by the overlay; @ReactMethod or JS cannot see it.
     *  ★ The frame clock (Choreographer) runs only while the overlay is asking — it stops itself 3 s after the
     *    last call, so a build with the overlay off pays nothing. */
    @ReactMethod
    fun perfStats(promise: Promise) {
        try {
            PerfStats.lastAsk = android.os.SystemClock.uptimeMillis()
            PerfStats.ensureFrameClock()
            val map = com.facebook.react.bridge.Arguments.createMap()
            map.putDouble("cpuPct", PerfStats.cpuPct())
            val mi = android.os.Debug.MemoryInfo()
            android.os.Debug.getMemoryInfo(mi)
            map.putDouble("footprintMB", mi.totalPss / 1024.0)          // PSS, kB → MB
            val f = PerfStats.frames()
            map.putDouble("uiFps", f[0]); map.putDouble("uiP50Ms", f[1]); map.putDouble("uiP90Ms", f[2])
            promise.resolve(map)
        } catch (e: Throwable) {
            promise.reject("perf", e)
        }
    }

    /* ★★ DIAGNOSTICS (2026-10-05) — the iOS methods of the same names, so diagnostics.ts reads ONE API on both
     *  platforms. Android had none of the three, which is why a VibeServer/libairspyhf crash on Nick's Pixel left
     *  no trace in a report that said "none recorded". The exit record comes from the system — see VibeExitInfo.
     *  ★ @ReactMethod on each, or JS sees undefined and prints "none recorded" — the very lie being fixed. */
    @ReactMethod
    fun getNativeCrash(promise: Promise) {
        try { promise.resolve(VibeExitInfo.read(reactContext)) }
        catch (e: Throwable) { promise.reject("exitinfo", e) }
    }

    @ReactMethod
    fun clearNativeCrash() {
        try { VibeExitInfo.clear(reactContext) } catch (_: Throwable) {}
    }

    /** Device identifiers for the report — the model and OS, no name, no account, no serial. */
    @ReactMethod
    fun getDeviceInfo(promise: Promise) {
        val map = com.facebook.react.bridge.Arguments.createMap()
        map.putString("model", "${Build.MANUFACTURER} ${Build.MODEL}")
        map.putString("os", "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
        map.putString("systemName", "Android")
        promise.resolve(map)
    }

    // NativeEventEmitter housekeeping (events arrive via RCTDeviceEventEmitter)
    @ReactMethod
    fun addListener(eventName: String) { /* no-op */ }

    @ReactMethod
    fun removeListeners(count: Double) { /* no-op */ }
}

/** ★ B19 perf overlay — see perfStats. */
private object PerfStats {
    @Volatile var lastAsk = 0L
    private var lastCpuTicks = -1L
    private var lastWallMs = 0L
    private val intervals = ArrayDeque<Double>()
    private var lastFrameNs = 0L
    private var running = false
    private val main = android.os.Handler(android.os.Looper.getMainLooper())

    /** CPU % of this process (100 % = one core), from /proc/self/stat utime+stime. */
    fun cpuPct(): Double {
        val parts = java.io.File("/proc/self/stat").readText().substringAfterLast(')').trim().split(' ')
        val ticks = parts[11].toLong() + parts[12].toLong()             // utime, stime (fields 14, 15)
        val now = android.os.SystemClock.elapsedRealtime()
        val prev = lastCpuTicks; val prevMs = lastWallMs
        lastCpuTicks = ticks; lastWallMs = now
        if (prev < 0 || now <= prevMs) return 0.0
        val hz = android.system.Os.sysconf(android.system.OsConstants._SC_CLK_TCK).toDouble()
        return (ticks - prev) / hz / ((now - prevMs) / 1000.0) * 100.0
    }

    private val cb = object : android.view.Choreographer.FrameCallback {
        override fun doFrame(ns: Long) {
            if (lastFrameNs != 0L) synchronized(intervals) {
                intervals.addLast((ns - lastFrameNs) / 1e6)
                while (intervals.size > 240) intervals.removeFirst()
            }
            lastFrameNs = ns
            if (android.os.SystemClock.uptimeMillis() - lastAsk > 3000) { running = false; lastFrameNs = 0L; return }
            android.view.Choreographer.getInstance().postFrameCallback(this)
        }
    }

    fun ensureFrameClock() {
        main.post {
            if (!running) { running = true; lastFrameNs = 0L; android.view.Choreographer.getInstance().postFrameCallback(cb) }
        }
    }

    /** [fps, p50 ms, p90 ms] over the last second of frames; drained on read. */
    fun frames(): DoubleArray {
        val xs = synchronized(intervals) { val c = intervals.toMutableList(); intervals.clear(); c }
        if (xs.isEmpty()) return doubleArrayOf(0.0, 0.0, 0.0)
        val sorted = xs.sorted()
        val total = xs.sum()
        val fps = if (total > 0) xs.size * 1000.0 / total else 0.0
        return doubleArrayOf(fps, sorted[sorted.size / 2], sorted[minOf(sorted.size - 1, (sorted.size * 9) / 10)])
    }
}
