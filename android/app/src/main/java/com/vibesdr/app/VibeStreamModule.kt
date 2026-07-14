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

    @ReactMethod
    fun startAudioEngine(baseUrl: String, frequency: Double, mode: String, uuid: String, password: String) {
        VibeStreamService.reactContext = reactContext
        val intent = Intent(reactContext, VibeStreamService::class.java).apply {
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
            Intent(reactContext, VibeStreamService::class.java).apply {
                action = VibeStreamService.ACTION_STOP
            }
        )
    }

    // External PCM audio (OWRX/Kiwi): start the foreground service in external
    // mode; pushExternalPcm/stopExternalAudio go straight to the live instance.
    @ReactMethod
    fun startExternalAudio(sampleRate: Double, pauseMode: String = "release") {
        VibeStreamService.reactContext = reactContext
        val intent = Intent(reactContext, VibeStreamService::class.java).apply {
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
    fun startLocalAudio(host: String, port: Double, initialTune: String, authSuffix: String) {
        VibeStreamService.reactContext = reactContext
        val intent = Intent(reactContext, VibeStreamService::class.java).apply {
            action = VibeStreamService.ACTION_START_LOCAL
            putExtra(VibeStreamService.EXTRA_HOST, host)
            putExtra(VibeStreamService.EXTRA_PORT, port.toInt())
            putExtra(VibeStreamService.EXTRA_TUNE, initialTune)
            putExtra(VibeStreamService.EXTRA_AUTH, authSuffix)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) reactContext.startForegroundService(intent)
        else reactContext.startService(intent)
    }

    @ReactMethod
    fun sendLocalTune(json: String) { VibeStreamService.instance?.sendLocalTune(json) }

    @ReactMethod
    fun stopLocalAudio() { VibeStreamService.instance?.stopLocalAudio() }

    // FM-DX Webserver (v7): the shared tuner's MP3-over-WS audio, consumed +
    // decoded natively (background-safe). JS owns only the /text + /chat sockets.
    @ReactMethod
    fun startFmdxAudio(baseUrl: String) {
        VibeStreamService.reactContext = reactContext
        val intent = Intent(reactContext, VibeStreamService::class.java).apply {
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

    // CarFM: frequency -> MediaMetadata ALBUM (spec §5b). Empty = "VibeSDR".
    @ReactMethod
    fun setNowPlayingAlbum(album: String) {
        VibeStreamService.instance?.setNowPlayingAlbumNative(album)
    }

    @ReactMethod
    fun setMediaSkipMode(mode: String) {
        VibeStreamService.instance?.skipMode = mode
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

    // NativeEventEmitter housekeeping (events arrive via RCTDeviceEventEmitter)
    @ReactMethod
    fun addListener(eventName: String) { /* no-op */ }

    @ReactMethod
    fun removeListeners(count: Double) { /* no-op */ }
}
