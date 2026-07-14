package com.vibesdr.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Base64
import android.support.v4.media.MediaBrowserCompat
import android.support.v4.media.MediaDescriptionCompat
import android.support.v4.media.MediaMetadataCompat
import android.support.v4.media.session.MediaSessionCompat
import android.support.v4.media.session.PlaybackStateCompat
import android.util.Log
import androidx.car.app.connection.CarConnection
import androidx.core.app.NotificationCompat
import androidx.lifecycle.Observer
import androidx.media.MediaBrowserServiceCompat
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.modules.core.DeviceEventManagerModule
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.LinkedBlockingDeque
import java.util.concurrent.TimeUnit
import kotlin.math.abs
import kotlin.math.min

/**
 * Native audio engine + foreground service — Android mirror of the iOS
 * VibePowerModule pipeline (the old ExoPlayer /audio/stream HTTP path cannot
 * connect to v2 servers; audio rides the session WebSocket now).
 *
 * Packet layout (version=2, always 21-byte header):
 *   [0:8]   uint64 LE  timestamp
 *   [8:12]  uint32 LE  sample rate (encoder input rate — informational)
 *   [12]    uint8      channels
 *   [13:17] float32 LE baseband power
 *   [17:21] float32 LE noise density
 *   [21:]   Opus payload
 *
 * Design notes:
 *  - MediaCodec "audio/opus" decoder configured ONCE at 48 kHz. Opus payloads
 *    are rate-agnostic, so the server's per-mode sample-rate flips (linear
 *    12k / FM 24k) need no decoder or AudioTrack rebuilds — the half-speed
 *    race the iOS engine had cannot exist here by construction.
 *  - Single decode thread owns codec + AudioTrack; the WS callback only
 *    enqueues. Bounded deque drops OLDEST on overflow so playback hugs the
 *    live edge (iOS queuedSeconds parity).
 *  - Watchdog: packets flow ~50/s; >8s stale or dead socket → reopen with
 *    the SAME session uuid (a fresh one orphans decoders/spectrum WS
 *    server-side — "no active audio session").
 */
class VibeStreamService : MediaBrowserServiceCompat() {

    companion object {
        const val CHANNEL_ID = "vibesdr_audio"
        const val NOTIF_ID = 1
        // Media-browser node ids (Android Auto / CarPlay-style browse tree)
        const val MEDIA_ROOT_ID = "vibesdr_root"
        const val BOOKMARKS_ID = "bookmarks"
        const val BANDS_ID = "bands"
        const val ACTION_PLAY = "com.vibesdr.app.PLAY"
        const val ACTION_PAUSE = "com.vibesdr.app.PAUSE"
        const val ACTION_STOP = "com.vibesdr.app.STOP"
        const val ACTION_NEXT = "com.vibesdr.app.NEXT"
        const val ACTION_PREV = "com.vibesdr.app.PREV"
        const val ACTION_START = "com.vibesdr.app.START"
        const val ACTION_START_EXTERNAL = "com.vibesdr.app.START_EXTERNAL"
        // Local hardware (V4): the on-device shim's /ws/audio consumed NATIVELY, so
        // audio + the media card survive backgrounding (JS owns only the spectrum
        // WS, which is paused in the background to save power).
        const val ACTION_START_LOCAL = "com.vibesdr.app.START_LOCAL"
        // FM-DX Webserver: the shared TEF6686 tuner's MP3-over-WS audio stream,
        // consumed + decoded (MediaCodec "audio/mpeg") NATIVELY so audio + the
        // media card survive backgrounding. Pause = stop the stream (power saving,
        // like UberSDR), play = reopen it. The /text + /chat control WS stay in JS.
        const val ACTION_START_FMDX = "com.vibesdr.app.START_FMDX"
        const val EXTRA_PORT = "port"
        const val EXTRA_TUNE = "tune"
        const val EXTRA_HOST = "host"
        const val EXTRA_AUTH = "auth"
        const val EXTRA_RATE = "rate"
        // External-audio pause behaviour: "release" (OWRX — pause disconnects AND
        // drops the media card; reconnect resets the server profile so play isn't
        // offered), "reconnect" (Kiwi — pause disconnects but keeps the card, play
        // reconnects, same as UberSDR), "resume" (local USB SDR — pause mutes in
        // place keeping USB/shim alive, play resumes).
        const val EXTRA_PAUSE_MODE = "pauseMode"
        const val EXTRA_BASE_URL = "baseUrl"
        const val EXTRA_FREQUENCY = "frequency"
        const val EXTRA_MODE = "mode"
        const val EXTRA_UUID = "uuid"
        const val EXTRA_PASSWORD = "password"

        private const val TAG = "VibeStream"
        private const val HEADER_LEN = 21

        var reactContext: ReactApplicationContext? = null
        @Volatile var instance: VibeStreamService? = null

        // IMA-ADPCM tables (VibeServer compressed-audio decode).
        private val ADPCM_STEP = intArrayOf(
            7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,
            88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,
            544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,
            2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
            10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767)
        private val ADPCM_INDEX = intArrayOf(-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8)
    }

    private var mediaSession: MediaSessionCompat? = null
    private val mainHandler = Handler(Looper.getMainLooper())

    // ── Engine state ─────────────────────────────────────────────────────────
    @Volatile private var running = false
    @Volatile private var externalAudio = false   // OWRX/Kiwi/local: raw PCM pushed from JS
    @Volatile private var externalPauseMode = "release"  // see EXTRA_PAUSE_MODE
    @Volatile private var muted = false
    @Volatile private var volume = 1f
    @Volatile private var currentFreq = 14_074_000L
    @Volatile private var currentMode = "usb"
    @Volatile private var currentStep = 1_000L
    private var currentBase = ""
    private var currentUuid = ""
    // Bypass password (rate-limit/ban bypass) — appended to the audio WS URL
    private var bypassPassword = ""
    private var instanceName = ""
    @Volatile private var lastPacketAt = 0L
    @Volatile private var packetCount = 0

    private var httpClient: OkHttpClient? = null
    @Volatile private var ws: WebSocket? = null
    private val packetQueue = LinkedBlockingDeque<ByteArray>(32)
    private var decodeThread: Thread? = null

    // SERVER BUG WORKAROUND (FM half-speed): ubersdr creates its opus encoder
    // ONCE per WS at the then-current sample rate; a mode change flips radiod
    // to a new rate but keeps the old encoder → audio time-stretched INSIDE
    // the opus stream. A header-rate flip cycles the WS so the server builds
    // a fresh encoder (3-packet confirm + 4s cooldown for flip stragglers).
    @Volatile private var wsBaseSr = 0
    @Volatile private var srFlipCount = 0
    @Volatile private var lastSrCycleAt = 0L

    // ── Client noise DSP (VibeDSP.kt — iOS/skin parity) ──────────────────────
    // Engines run at the STREAM rate (packet-header sr: linear 12k / FM 24k)
    // for exact tuning parity; the opus decoder always outputs 48k, so the
    // decode thread decimates by the integer factor, processes, interpolates
    // back. All DSP state is decode-thread-only.
    @Volatile var nrMode = "off"        // "off" | "nr" | "nr2"
    @Volatile var nbOn = false
    @Volatile var nrBandwidthHz = 2700.0

    // Client-side auto notch (NLMS), network backends only — local/RTL-TCP audio
    // arrives already notched from the shim. One filter per channel.
    @Volatile private var notchOn = false
    private val notchCh = arrayOfNulls<AutoNotch>(2)
    private var notchF0 = FloatArray(0)
    private var notchF1 = FloatArray(0)
    fun setNotchOn(on: Boolean) { notchOn = on }

    // Client-side audio squelch gate (network backends, e.g. Kiwi). JS drives it
    // from the S-meter level vs the threshold; closed → output silence (the track
    // keeps flowing — distinct from pause/muted). Defaults open.
    @Volatile private var squelchOpen = true
    fun setSquelchOpen(open: Boolean) { squelchOpen = open }
    @Volatile private var dspResetRequested = false
    private var dspRate = 0
    private var nbEngine: NoiseBlankerEngine? = null
    private var nr2Chunker: BlockChunker? = null
    private var websdrEngine: WebSDRNREngine? = null
    private var websdrChunker: BlockChunker? = null
    @Volatile private var streamSr = 12_000   // latest packet-header rate

    // ── Recorder (decode-thread state machine) ───────────────────────────────
    // Taps the post-DSP 48k PCM into MediaCodec AAC + MediaMuxer .m4a;
    // records through mutes. Start/stop arrive as requests and are handled
    // on the decode thread (MediaCodec is not thread-safe).
    @Volatile var recArmed = false
    @Volatile private var recStartReq: com.facebook.react.bridge.Promise? = null
    @Volatile private var recStopReq: com.facebook.react.bridge.Promise? = null
    private var recEncoder: android.media.MediaCodec? = null
    private var recMuxer: android.media.MediaMuxer? = null
    private var recTrackIndex = -1
    private var recMuxerStarted = false
    private var recChannels = 1
    private var recRate = 48_000     // encoder rate: 48k (UberSDR) or the external stream rate
    private var recReqFreq = 0.0     // freq/mode from the UI for the recording filename
    private var recReqMode = ""
    private var recPtsUs = 0L
    private var recFile: java.io.File? = null
    private var recDisplayName = ""

    // Now-playing overrides — JS computes VTS-aware title/artist (station or
    // band name, user's frequency unit, tune step); native skips clear them
    // until the JS VibeTuned round-trip refreshes (sub-second).
    @Volatile private var npTitle: String? = null
    @Volatile private var npArtist: String? = null
    // Media skip routing: "step" = native tune±step; "bookmark" = emit
    // VibeSkip and let JS jump bookmarks (it owns the VTS station list)
    @Volatile var skipMode = "step"
    // Pause disconnects the SDR (server drops it on suspend anyway) and Play
    // reconnects; these track the two non-playing notification states: cleanly
    // disconnected vs a reconnect that failed (server full / rate-limited).
    @Volatile private var dataSaverDisconnected = false
    @Volatile private var reconnectFailed = false
    @Volatile private var lastSignalEmit = 0L   // throttle the SNR (VibeSignal) event
    // Audio focus — when another app takes it (parity with iOS interruption) we
    // register a mute so the UI + data saver reflect it; user presses Play to
    // return (no auto-resume on regain).
    private val audioManager by lazy { getSystemService(AUDIO_SERVICE) as AudioManager }
    private var audioFocusRequest: AudioFocusRequest? = null
    private val focusListener = AudioManager.OnAudioFocusChangeListener { change ->
        when (change) {
            // Permanent loss = another media app took over for good. Relinquish
            // fully like iOS does — stop the engine and tear down the foreground
            // service + Now Playing notification so VibeSDR doesn't linger in the
            // notification shade looking "still running" (GitHub #6).
            AudioManager.AUDIOFOCUS_LOSS ->
                mainHandler.post { if (running) { stopEngine(); stopSelf() } }
            // Transient loss (a call, a notification ping) = just mute; we resume.
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK ->
                mainHandler.post { if (running && !muted && !dataSaverDisconnected) setMutedNative(true) }
        }
    }
    // Composited album art (app icon + server-type logo inset), cached
    private var npArtwork: android.graphics.Bitmap? = null
    private var npArtworkType = ""

    // ── Car browse tree (Android Auto) ───────────────────────────────────────
    // Bookmarks + Band Plan lists pushed from JS (setBrowseItems) and served via
    // MediaBrowserServiceCompat. Cached to prefs so a cold service start (Android
    // Auto connecting before the app has run) still shows the last known list.
    private data class BrowseItem(
        val name: String, val freq: Long, val mode: String, val step: Long, val isBand: Boolean,
    )
    @Volatile private var browseBookmarks: List<BrowseItem> = emptyList()
    @Volatile private var browseBands: List<BrowseItem> = emptyList()
    private var carConnection: CarConnection? = null
    private var carObserver: Observer<Int>? = null

    private var watchdog: Runnable? = null
    // Tune coalescing: the velocity drum can emit 20+ steps/s; one WS tune
    // per step thrashes radiod. Leading send + 80ms trailing timer.
    private var pendingTuneFreq = 0L
    private var pendingTuneMode = ""
    private var hasPendingTune = false
    private var lastTuneSentAt = 0L
    private var tuneFlush: Runnable? = null

    // ── Service lifecycle ────────────────────────────────────────────────────

    override fun onCreate() {
        super.onCreate()
        instance = this
        createNotificationChannel()
        setupMediaSession()
        // Required so MediaBrowserServiceCompat can serve Android Auto / Wear.
        sessionToken = mediaSession?.sessionToken
        loadCachedBrowse()
        startCarConnectionWatch()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                val base = intent.getStringExtra(EXTRA_BASE_URL) ?: return START_STICKY
                val freq = intent.getLongExtra(EXTRA_FREQUENCY, 14_074_000L)
                val mode = intent.getStringExtra(EXTRA_MODE) ?: "usb"
                val uuid = intent.getStringExtra(EXTRA_UUID) ?: return START_STICKY
                val pw = intent.getStringExtra(EXTRA_PASSWORD) ?: ""
                startAudioEngine(base, freq, mode, uuid, pw)
            }
            ACTION_START_EXTERNAL -> startExternalAudio(intent.getIntExtra(EXTRA_RATE, 48000), intent.getStringExtra(EXTRA_PAUSE_MODE) ?: "release")
            ACTION_START_LOCAL -> startLocalAudio(
                intent.getStringExtra(EXTRA_HOST) ?: "127.0.0.1",
                intent.getIntExtra(EXTRA_PORT, 0),
                intent.getStringExtra(EXTRA_TUNE) ?: "",
                intent.getStringExtra(EXTRA_AUTH) ?: "")
            ACTION_START_FMDX -> startFmdxAudio(intent.getStringExtra(EXTRA_BASE_URL) ?: return START_STICKY)
            ACTION_PLAY -> setMutedNative(false)
            ACTION_PAUSE -> setMutedNative(true)
            // stopSelf(startId) — NOT bare stopSelf() — so a reconnect (AudioPlayer
            // fires stopAudioEngine→ACTION_STOP then startAudioEngine→ACTION_START
            // back-to-back) doesn't destroy the service in the gap: if a newer START
            // is already queued, stopSelf(startId) is ignored and the service (and
            // its MediaSession/notification card) survives — startAudioEngine just
            // reconfigures it. Bare stopSelf() killed the card on every reconnect.
            ACTION_STOP -> { stopEngine(); stopSelf(startId); return START_NOT_STICKY }
            ACTION_NEXT -> tuneByStep(+1)
            ACTION_PREV -> tuneByStep(-1)
        }
        return START_STICKY
    }

    override fun onDestroy() {
        stopEngine()
        stopCarConnectionWatch()
        mediaSession?.release()
        instance = null
        super.onDestroy()
    }

    /** User swiped VibeSDR out of the recent-apps list → fully shut down: close
     *  the audio/spectrum WS + USB, release audio focus, and remove the foreground
     *  notification (stopEngine does all of that incl. stopForeground(REMOVE)),
     *  then stop the service. Without this the foreground service (and its media
     *  notification) can survive the task swipe on many devices — the app looks
     *  "still running" in the shade (GitHub #6). stopForeground alone isn't enough
     *  on a mediaPlayback FGS; we stop the whole service. */
    override fun onTaskRemoved(rootIntent: Intent?) {
        stopEngine()
        stopSelf()
        super.onTaskRemoved(rootIntent)
    }

    /** Start the foreground service with an EXPLICIT mediaPlayback type. The
     *  2-arg startForeground(id, notification) lets the framework INFER the type
     *  from the manifest — which the Unisoc/Moto ActivityManager fumbles to
     *  types=0x0 (service ends up isForeground=false → demoted to a cached
     *  process → /background cpuset → little cores → the DSP thread starves the
     *  audio writer → background breakup). Samsung infers 0x2 fine, so this is a
     *  device-specific framework quirk. Passing the type explicitly (the form
     *  Android recommends for targetSdk 34+) makes the Moto keep the foreground
     *  state. Guarded for API<30 where the type param doesn't exist. */
    private fun startForegroundMedia() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            androidx.core.app.ServiceCompat.startForeground(
                this, NOTIF_ID, buildNotification(),
                android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } else {
            startForeground(NOTIF_ID, buildNotification())
        }
    }

    // ── Engine control (called from VibeStreamModule / media controls) ───────

    fun startAudioEngine(baseUrl: String, frequency: Long, mode: String, uuid: String, password: String) {
        Log.i(TAG, "startAudioEngine $baseUrl $frequency $mode")
        stopEngine()
        currentBase = baseUrl
        currentFreq = frequency
        currentMode = mode
        currentUuid = uuid
        bypassPassword = password
        running = true
        muted = false
        // Fresh session — clear the disconnected / reconnect-failed card state.
        dataSaverDisconnected = false
        reconnectFailed = false
        lastArtworkKey = ""
        packetCount = 0
        lastPacketAt = SystemClock.elapsedRealtime()
        packetQueue.clear()
        requestAudioFocus()
        startDecodeThread()
        openWs()
        startWatchdog()
        mediaSession?.isActive = true
        updateMetadataSession()
        updatePlaybackState(PlaybackStateCompat.STATE_PLAYING)
        startForegroundMedia()
    }

    fun stopEngine() {
        running = false
        // Reset external state too, so switching OWRX→UberSDR doesn't leave the
        // engine in external mode (which made UberSDR pause take the external path
        // → media-control "pause springs back to play").
        externalAudio = false
        externalPauseMode = "release"
        // FM-DX teardown (switching FM-DX → another backend)
        fmdxAudio = false
        closeFmdxWs()
        mp3Thread?.interrupt(); mp3Thread = null
        mp3Queue.clear()
        localAudioWs?.cancel(); localAudioWs = null; lastLocalTune = null
        extThread?.interrupt(); extThread = null
        extQueue.clear()
        extTrack?.release(); extTrack = null; extRate = 0
        abandonAudioFocus()
        watchdog?.let { mainHandler.removeCallbacks(it) }
        watchdog = null
        tuneFlush?.let { mainHandler.removeCallbacks(it) }
        tuneFlush = null
        ws?.close(1001, "going away")
        ws = null
        packetQueue.clear()
        decodeThread?.interrupt()
        decodeThread = null
        mediaSession?.isActive = false
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            @Suppress("DEPRECATION")
            stopForeground(true)
        }
    }

    fun sendTuneCommand(frequency: Long, mode: String) {
        currentFreq = frequency
        currentMode = mode
        mainHandler.post {
            pendingTuneFreq = frequency
            pendingTuneMode = mode
            hasPendingTune = true
            val since = SystemClock.elapsedRealtime() - lastTuneSentAt
            if (since >= 80) {
                flushPendingTune()
            } else if (tuneFlush == null) {
                val r = Runnable { tuneFlush = null; flushPendingTune() }
                tuneFlush = r
                mainHandler.postDelayed(r, 80 - since)
            }
            updateMetadataSession()
            updateNotification()
        }
    }

    private fun flushPendingTune() {
        if (!hasPendingTune) return
        hasPendingTune = false
        lastTuneSentAt = SystemClock.elapsedRealtime()
        sendWsJson(JSONObject().put("type", "tune")
            .put("frequency", pendingTuneFreq).put("mode", pendingTuneMode))
        // Drop queued (pre-tune) audio so what you HEAR snaps to the new
        // frequency — fine-tuning SSB through a stale backlog is impossible.
        packetQueue.clear()
    }

    fun sendBandwidth(low: Long, high: Long) {
        sendWsJson(JSONObject().put("type", "tune")
            .put("bandwidthLow", low).put("bandwidthHigh", high))
        // Audio occupies 0..max-edge Hz regardless of sideband (NR syncBins)
        nrBandwidthHz = maxOf(abs(low), abs(high)).toDouble()
    }

    fun setStep(hz: Long) { currentStep = hz }

    fun setInstanceNameNative(name: String) {
        instanceName = name
        mainHandler.post { updateMetadataSession(); updateNotification() }
    }

    fun setMutedNative(m: Boolean) {
        muted = m
        if (m) packetQueue.clear()
        emitEvent("VibeMuted") { it.putBoolean("muted", m) }
        // FM-DX power-saving pause: STOP the MP3 audio stream (close the /audio WS)
        // so it isn't draining battery/network in the background (AirPods out /
        // Bluetooth off) — the same intent as UberSDR's disconnect-on-pause. ▶
        // reopens FM-DX's OWN audio WS. The media card stays (the /text control WS
        // in JS is untouched — it self-throttles in the background).
        if (fmdxAudio) {
            mainHandler.post {
                if (m) closeFmdxWs() else openFmdxWs()
                updatePlaybackState(if (m) PlaybackStateCompat.STATE_PAUSED else PlaybackStateCompat.STATE_PLAYING)
                updateNotification()
            }
            return
        }
        // OWRX/Kiwi (external): an OWRX reconnect resets the server to its default
        // profile, so we don't offer play-to-reconnect. PAUSE fully releases the
        // media controls (stopExternalAudio clears the notification); JS closes its
        // WS on the VibeMuted event and shows an in-app reconnect prompt, so a
        // (profile-resetting) reconnect is always a deliberate user action.
        if (externalAudio) {
            when (externalPauseMode) {
                "resume" -> {
                    // Local USB SDR: no server to disconnect. The RTL/shim keep
                    // running and pushExternalPcm drops samples while muted, so just
                    // mute in place and keep the media card (▶ resumes). Preserves
                    // USB state — no teardown.
                    mainHandler.post {
                        updatePlaybackState(if (m) PlaybackStateCompat.STATE_PAUSED else PlaybackStateCompat.STATE_PLAYING)
                        updateNotification()
                    }
                }
                "reconnect" -> {
                    // Kiwi: behave like UberSDR — pause disconnects but keeps the
                    // disconnect card (▶), play does a full reconnect. JS closes/
                    // reopens its WS off the VibeDataSaver* events these emit.
                    mainHandler.post { if (m) disconnectForPause() else resumeFromDataSaver() }
                }
                else -> {
                    // OWRX ("release"): pause disconnects AND drops the card (an
                    // OWRX reconnect resets the server profile, so no play-to-
                    // reconnect). JS closes its WS off VibeMuted + shows a prompt.
                    if (m) mainHandler.post { stopExternalAudio() }
                }
            }
            return
        }
        // UberSDR — Pause = disconnect, Play = reconnect (disconnect card + ▶).
        mainHandler.post {
            if (m) {
                disconnectForPause()
            } else if (dataSaverDisconnected) {
                resumeFromDataSaver()
            } else {
                updatePlaybackState(PlaybackStateCompat.STATE_PLAYING)
                updateNotification()
            }
        }
    }

    // ── Pause = disconnect / Play = reconnect ────────────────────────────────

    private fun disconnectForPause() {
        if (dataSaverDisconnected) return
        dataSaverDisconnected = true
        abandonAudioFocus()                       // release the audio route
        ws?.close(1001, "paused"); ws = null
        packetQueue.clear()
        // Keep the foreground notification + session so ▶ reconnects; show it as
        // a clearly "Disconnected" card (see nowPlaying* / refreshArtwork).
        mainHandler.post {
            updatePlaybackState(PlaybackStateCompat.STATE_PAUSED)
            updateMetadataSession(); updateNotification()
        }
        emitEvent("VibeDataSaverDisconnect") { }
    }

    private fun resumeFromDataSaver() {
        if (!dataSaverDisconnected) return
        // Don't reopen the old session — a partial reopen lands in a broken
        // half-state. Hand off to JS for a full from-scratch reconnect (new uuid
        // → fresh startAudioEngine, which clears the disconnected state). The flag
        // stays set until then so the watchdog won't revive the stale socket.
        emitEvent("VibeDataSaverResume") { }
    }

    /** JS calls this when a reconnect attempt fails (server full / rate-limited)
     *  so the notification tells the user to open the app. */
    fun setReconnectFailedNative(failed: Boolean) {
        reconnectFailed = failed
        if (failed) dataSaverDisconnected = false
        mainHandler.post {
            // COMPUTE the state (matching iOS updateNowPlaying) — don't hardcode
            // PAUSED. JS calls setReconnectFailed(false) on every successful
            // connect, which used to flip a freshly-PLAYING session to PAUSED
            // (the media card stuck on a play button / pause-play spring). Only
            // pause when actually failed / disconnected / muted.
            val playing = !reconnectFailed && !dataSaverDisconnected && !muted
            updatePlaybackState(if (playing) PlaybackStateCompat.STATE_PLAYING else PlaybackStateCompat.STATE_PAUSED)
            updateMetadataSession(); updateNotification()
        }
    }

    private fun requestAudioFocus() {
        val attrs = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val req = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(attrs)
                .setOnAudioFocusChangeListener(focusListener, mainHandler)
                .build()
            audioFocusRequest = req
            audioManager.requestAudioFocus(req)
        } else {
            @Suppress("DEPRECATION")
            audioManager.requestAudioFocus(
                focusListener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN)
        }
    }

    private fun abandonAudioFocus() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            audioFocusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
            audioFocusRequest = null
        } else {
            @Suppress("DEPRECATION")
            audioManager.abandonAudioFocus(focusListener)
        }
    }

    // ── External PCM audio (OWRX/Kiwi) ───────────────────────────────────────
    // The WS + decode live in JS; JS pushes raw 16-bit mono PCM and we just play
    // it. A dedicated AudioTrack (recreated when the rate changes) plays at the
    // pushed rate — Android resamples to the device rate, so the DAB speed
    // correction (JS under-states the rate) works the same as on iOS. Background
    // audio survives because the foreground service keeps the app + JS alive.
    @Volatile private var extTrack: AudioTrack? = null
    private var extRate = 0
    private var extChannels = 1
    private var extThread: Thread? = null
    // (rate, channels, interleaved int16 samples)
    private val extQueue = LinkedBlockingDeque<Triple<Int, Int, ShortArray>>(64)

    fun startExternalAudio(rate: Int, pauseMode: String = "release") {
        Log.i(TAG, "startExternalAudio $rate pauseMode=$pauseMode")
        stopEngine()                       // tear down any opus path
        externalAudio = true
        externalPauseMode = pauseMode
        running = true
        muted = false
        dataSaverDisconnected = false
        reconnectFailed = false
        lastArtworkKey = ""
        packetCount = 0
        lastPacketAt = SystemClock.elapsedRealtime()
        extQueue.clear()
        requestAudioFocus()
        startExtWriter()
        mediaSession?.isActive = true
        updateMetadataSession()
        updatePlaybackState(PlaybackStateCompat.STATE_PLAYING)
        startForegroundMedia()
    }

    // channels: 1=mono (OWRX/Kiwi, local narrow modes), 2=interleaved stereo
    // (local WFM). Samples are interleaved L,R for stereo.
    fun pushExternalPcm(base64: String, rate: Int, channels: Int = 1) {
        if (!externalAudio || muted) return
        val bytes = try { Base64.decode(base64, Base64.DEFAULT) } catch (e: Exception) { return }
        val n = bytes.size / 2
        if (n == 0) return
        val shorts = ShortArray(n)
        ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().get(shorts)
        lastPacketAt = SystemClock.elapsedRealtime()
        packetCount++
        extQueue.offer(Triple(rate, if (channels == 2) 2 else 1, shorts))   // drop when full (backpressure)
    }

    fun stopExternalAudio() {
        Log.i(TAG, "stopExternalAudio")
        externalAudio = false
        externalPauseMode = "release"
        localAudioWs?.cancel(); localAudioWs = null; lastLocalTune = null
        running = false
        extThread?.interrupt(); extThread = null
        extQueue.clear()
        abandonAudioFocus()
        mediaSession?.isActive = false
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) stopForeground(STOP_FOREGROUND_REMOVE)
        else { @Suppress("DEPRECATION") stopForeground(true) }
    }

    // ── Local hardware audio (V4) — native /ws/audio consumer ────────────────
    // The on-device shim serves demodulated PCM on ws://127.0.0.1:<port>/ws/audio.
    // We read it HERE (OkHttp, native) rather than in JS so audio + the media card
    // survive backgrounding — the foreground service keeps the process (and the
    // in-process shim) alive. JS owns only the spectrum WS, which it pauses in the
    // background to save power. Tune/mode/bandwidth changes ride this same WS as
    // JSON (sendLocalTune), since that's the shim's control channel.
    @Volatile private var localAudioWs: WebSocket? = null
    @Volatile private var lastLocalTune: String? = null

    fun startLocalAudio(host: String, port: Int, initialTune: String, authSuffix: String) {
        Log.i(TAG, "startLocalAudio $host:$port")
        if (port <= 0) return
        val h = if (host.isNotEmpty()) host else "127.0.0.1"
        startExternalAudio(48_000, "resume")   // external PCM engine; local pause = mute
        lastLocalTune = if (initialTune.isNotEmpty()) initialTune else null
        val client = httpClient ?: OkHttpClient.Builder()
            .pingInterval(20, TimeUnit.SECONDS)
            .build().also { httpClient = it }
        localAudioWs?.cancel()
        // authSuffix is "&vs_nonce=…&vs_auth=…"; /ws/audio has no query so it needs
        // a leading "?" rather than "&".
        val authQ = if (authSuffix.isNotEmpty()) "?" + authSuffix.removePrefix("&") else ""
        localAudioWs = client.newWebSocket(
            Request.Builder().url("ws://$h:$port/ws/audio$authQ").build(),
            object : WebSocketListener() {
                override fun onOpen(webSocket: WebSocket, response: Response) {
                    lastLocalTune?.let { webSocket.send(it) }
                }
                override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                    if (!externalAudio || muted) return
                    val b = bytes.toByteArray()
                    if (b.size <= 6) return
                    // Frame: [0]=channels, [1]=format (0 raw / 1 ADPCM mono / 2 M/S),
                    // [2..5]=rate LE, then payload.
                    val rate = ByteBuffer.wrap(b, 2, 4).order(ByteOrder.LITTLE_ENDIAN).int
                    if (rate <= 0) return
                    val format = b[1].toInt() and 0xFF
                    val out: Triple<Int, Int, ShortArray>? = when (format) {
                        1, 2 -> {
                            val count = (b[6].toInt() and 0xFF) or ((b[7].toInt() and 0xFF) shl 8)
                            if (count <= 0) null else {
                                val nb = (count + 1) / 2
                                val mid = adpcmDecodeBlock(b, 8, count)
                                if (format == 2 && b.size >= 8 + 4 + nb + 4 + nb) {
                                    val side = adpcmDecodeBlock(b, 8 + 4 + nb, count)
                                    val s = ShortArray(count * 2)
                                    for (i in 0 until count) {
                                        val m = mid[i].toInt(); val sd = side[i].toInt()
                                        s[i * 2]     = (m + sd).coerceIn(-32768, 32767).toShort()
                                        s[i * 2 + 1] = (m - sd).coerceIn(-32768, 32767).toShort()
                                    }
                                    Triple(rate, 2, s)
                                } else Triple(rate, 1, mid)
                            }
                        }
                        else -> {
                            val channels = b[0].toInt() and 0xFF
                            val n = (b.size - 6) / 2
                            if (n <= 0) null else {
                                val shorts = ShortArray(n)
                                ByteBuffer.wrap(b, 6, n * 2).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().get(shorts)
                                Triple(rate, if (channels == 2) 2 else 1, shorts)
                            }
                        }
                    } ?: return
                    lastPacketAt = SystemClock.elapsedRealtime()
                    packetCount++
                    extQueue.offer(out)  // drop when full
                }
                override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                    Log.w(TAG, "local audio WS failure: ${t.message}")
                }
            })
    }

    // IMA-ADPCM ('kiwi' flavour) — decode one self-seeded VibeServer block:
    // [predictor int16 LE][index u8][pad] + ceil(count/2) nibble bytes (low nibble
    // first). Matches the shim's adpcmEncodeBlock() and imaAdpcm.ts decoder.
    private fun adpcmDecodeBlock(b: ByteArray, off: Int, count: Int): ShortArray {
        var predictor = ((b[off].toInt() and 0xFF) or ((b[off + 1].toInt() and 0xFF) shl 8)).toShort().toInt()
        var index = (b[off + 2].toInt() and 0xFF).coerceIn(0, 88)
        val out = ShortArray(count)
        var bi = off + 4
        var s = 0
        while (s < count) {
            val byte = b[bi].toInt() and 0xFF; bi++
            var half = 0
            while (half < 2 && s < count) {
                val nib = if (half == 0) byte and 0x0F else (byte ushr 4) and 0x0F
                val step = ADPCM_STEP[index]
                var diff = step shr 3
                if (nib and 1 != 0) diff += step shr 2
                if (nib and 2 != 0) diff += step shr 1
                if (nib and 4 != 0) diff += step
                if (nib and 8 != 0) diff = -diff
                predictor = (predictor + diff).coerceIn(-32768, 32767)
                index = (index + ADPCM_INDEX[nib]).coerceIn(0, 88)
                out[s++] = predictor.toShort()
                half++
            }
        }
        return out
    }

    /** Forward a tune/mode/bandwidth change to the shim over the audio WS. Also
     *  refresh the media metadata from the tune itself (frequency/mode) so the
     *  now-playing title updates instantly when skipping in the BACKGROUND — JS's
     *  setNowPlaying is debounced and Android throttles its timers when backgrounded,
     *  so the title would otherwise lag. JS still refines it (station name) on the
     *  next foreground tick. Mirrors the UberSDR tuneByStep behaviour. */
    fun sendLocalTune(json: String) {
        lastLocalTune = json
        localAudioWs?.send(json)
        try {
            val o = JSONObject(json)
            val f = o.optLong("frequency", -1)
            val m = o.optString("mode", "")
            if (f > 0) currentFreq = f
            if (m.isNotEmpty()) currentMode = m
            npTitle = null; npArtist = null   // fall back to "freq · mode" until JS catches up
            mainHandler.post { updateMetadataSession(); updateNotification() }
        } catch (_: Exception) { }
    }

    fun stopLocalAudio() {
        Log.i(TAG, "stopLocalAudio")
        localAudioWs?.cancel(); localAudioWs = null
        lastLocalTune = null
        stopExternalAudio()
    }

    // ── FM-DX Webserver audio (v7) — native MP3-over-WS consumer ──────────────
    // The FM-DX server streams headerless MP3 frames on <base>/audio (3LAS, after
    // a {"type":"fallback","data":"mp3"} handshake). We read + MediaCodec-decode
    // it HERE (native) so audio + the media card survive backgrounding, then push
    // the PCM through the shared external-audio writer/track. The /text (tune,
    // RDS, SNR) and /chat sockets stay in JS. Reuses externalAudio=true for the
    // ext writer + watchdog-skip; fmdxAudio adds the WS + power-saving pause.
    @Volatile private var fmdxAudio = false
    private var fmdxBase = ""
    @Volatile private var fmdxWs: WebSocket? = null
    @Volatile private var fmdxGen = 0                 // supersede in-flight receives/reconnects
    private var mp3Thread: Thread? = null
    private val mp3Queue = LinkedBlockingDeque<ByteArray>(64)
    private var mp3Codec: MediaCodec? = null
    private var mp3PtsUs = 0L
    private var mp3Rate = 44_100
    private var mp3Ch = 2

    fun startFmdxAudio(baseUrl: String) {
        Log.i(TAG, "startFmdxAudio $baseUrl")
        if (baseUrl.isEmpty()) return
        startExternalAudio(48_000, "resume")   // sets up the ext writer + track + media card
        fmdxAudio = true
        fmdxBase = baseUrl
        mp3Queue.clear()
        startMp3DecodeThread()
        openFmdxWs()
    }

    fun stopFmdxAudio() {
        Log.i(TAG, "stopFmdxAudio")
        fmdxAudio = false
        closeFmdxWs()
        mp3Thread?.interrupt(); mp3Thread = null
        mp3Queue.clear()
        stopExternalAudio()
    }

    private fun fmdxWsUrl(base: String): String {
        var s = base.trim().trimEnd('/')
        s = when {
            s.startsWith("https://") -> "wss://" + s.removePrefix("https://")
            s.startsWith("http://") -> "ws://" + s.removePrefix("http://")
            s.startsWith("ws://") || s.startsWith("wss://") -> s
            else -> "wss://$s"
        }
        return "$s/audio"
    }

    private fun closeFmdxWs() {
        fmdxGen++                       // any pending receive/reconnect for the old gen no-ops
        fmdxWs?.cancel(); fmdxWs = null
    }

    private fun openFmdxWs() {
        val base = fmdxBase
        if (base.isEmpty()) return
        fmdxGen++
        val gen = fmdxGen
        val client = httpClient ?: OkHttpClient.Builder()
            .readTimeout(0, TimeUnit.MILLISECONDS)
            .pingInterval(20, TimeUnit.SECONDS)
            .build().also { httpClient = it }
        val url = fmdxWsUrl(base)
        Log.i(TAG, "opening fmdx audio WS: $url")
        fmdxWs?.cancel()
        fmdxWs = client.newWebSocket(Request.Builder().url(url).build(),
            object : WebSocketListener() {
                override fun onOpen(webSocket: WebSocket, response: Response) {
                    // 3LAS fallback handshake — request MP3 (matches the iOS path).
                    webSocket.send("{\"type\":\"fallback\",\"data\":\"mp3\"}")
                }
                override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                    if (gen != fmdxGen || !fmdxAudio || muted) return
                    val b = bytes.toByteArray()
                    if (b.isEmpty()) return
                    lastPacketAt = SystemClock.elapsedRealtime()
                    packetCount++
                    if (!mp3Queue.offerLast(b)) { mp3Queue.pollFirst(); mp3Queue.offerLast(b) }
                }
                override fun onMessage(webSocket: WebSocket, text: String) {
                    // Handshake acks / keepalive JSON — audio is binary only.
                }
                override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                    if (gen != fmdxGen) return
                    Log.w(TAG, "fmdx audio WS failure: ${t.message} — reconnect in 2s")
                    mainHandler.postDelayed({
                        if (fmdxAudio && !muted && gen == fmdxGen) openFmdxWs()
                    }, 2_000)
                }
            })
    }

    private fun startMp3DecodeThread() {
        mp3Thread?.interrupt()
        val t = Thread({
            try { mp3DecodeLoop() }
            catch (e: InterruptedException) { /* normal shutdown */ }
            catch (e: Exception) { Log.e(TAG, "mp3 decode died: ${e.message}", e) }
            finally { releaseMp3Codec() }
        }, "vibesdr-mp3")
        t.priority = Thread.MAX_PRIORITY
        mp3Thread = t
        t.start()
    }

    private fun ensureMp3Codec() {
        if (mp3Codec != null) return
        // Raw (containerless) MP3 stream — no csd needed (unlike opus). The rate/
        // channels passed are only a hint; the decoder corrects them via
        // INFO_OUTPUT_FORMAT_CHANGED (FM-DX serves 44.1k stereo).
        val fmt = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_MPEG, 44_100, 2)
        val c = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_MPEG)
        c.configure(fmt, null, null, 0)
        c.start()
        mp3Codec = c
        mp3PtsUs = 0
        Log.i(TAG, "mp3 decoder created")
    }

    private fun releaseMp3Codec() {
        try { mp3Codec?.stop() } catch (_: Exception) {}
        try { mp3Codec?.release() } catch (_: Exception) {}
        mp3Codec = null
    }

    private fun mp3DecodeLoop() {
        val info = MediaCodec.BufferInfo()
        ensureMp3Codec()
        while (running && fmdxAudio) {
            val pkt = mp3Queue.poll(250, TimeUnit.MILLISECONDS) ?: continue
            if (pkt.isEmpty()) continue
            val c = mp3Codec ?: run { ensureMp3Codec(); mp3Codec } ?: continue
            var fed = false
            var attempts = 0
            while (!fed && attempts < 50 && running) {
                val inIdx = c.dequeueInputBuffer(10_000)
                if (inIdx >= 0) {
                    val ib = c.getInputBuffer(inIdx) ?: break
                    ib.clear()
                    ib.put(pkt, 0, pkt.size)
                    c.queueInputBuffer(inIdx, 0, pkt.size, mp3PtsUs, 0)
                    mp3PtsUs += 26_000   // ~26ms/frame @44.1k — PTS only needs to be monotonic
                    fed = true
                }
                drainMp3(c, info)
                attempts++
            }
            drainMp3(c, info)
        }
    }

    private fun drainMp3(c: MediaCodec, info: MediaCodec.BufferInfo) {
        while (true) {
            val outIdx = c.dequeueOutputBuffer(info, 0)
            when {
                outIdx >= 0 -> {
                    val ob = c.getOutputBuffer(outIdx)
                    if (ob != null && info.size > 0) {
                        val pcm = ByteArray(info.size)
                        ob.position(info.offset)
                        ob.get(pcm, 0, info.size)
                        val n = info.size / 2
                        val shorts = ShortArray(n)
                        ByteBuffer.wrap(pcm).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().get(shorts)
                        extQueue.offer(Triple(mp3Rate, mp3Ch, shorts))   // drop when full (backpressure)
                    }
                    c.releaseOutputBuffer(outIdx, false)
                }
                outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    val f = c.outputFormat
                    mp3Rate = f.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                    mp3Ch = f.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                    Log.i(TAG, "mp3 out ${mp3Rate}Hz ${mp3Ch}ch")
                }
                else -> return
            }
        }
    }

    private fun startExtWriter() {
        val t = Thread({
            // Real audio-thread priority (not just JVM MAX_PRIORITY ≈ nice -8):
            // this thread feeds the AudioTrack, and under the New Architecture the
            // Fabric/worklets/JS threads can preempt it on weak cores (Moto/Unisoc)
            // → bursty writes → AudioTrack underruns → thin/sibilant/breaking audio.
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
            try {
                while (running && externalAudio) {
                    val item = extQueue.poll(250, TimeUnit.MILLISECONDS)
                    handleRecRequests()   // arm/stop the recorder on this thread too
                    if (item == null) continue
                    ensureExtTrack(item.first, item.second)
                    if (!muted) {
                        notchShorts(item.third, item.second)   // auto notch (network)
                        if (!squelchOpen) java.util.Arrays.fill(item.third, 0)  // squelch gate
                        extTrack?.write(item.third, 0, item.third.size)  // blocking = backpressure
                    }
                    // Feed the recorder encoder (the UberSDR decode loop isn't
                    // running on this path, so recording is driven from here).
                    if (recArmed) {
                        val sh = item.third
                        val bytes = ByteArray(sh.size * 2)
                        ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().put(sh)
                        encodePcm(bytes, item.second)
                    }
                }
            } catch (e: InterruptedException) {
                // normal shutdown
            } finally {
                extTrack?.release(); extTrack = null; extRate = 0
            }
        }, "vibesdr-ext")
        t.priority = Thread.MAX_PRIORITY
        extThread = t
        t.start()
    }

    private fun ensureExtTrack(rate: Int, channels: Int = 1) {
        if (extTrack != null && extRate == rate && extChannels == channels) return
        extTrack?.release()
        val mask = if (channels == 2) AudioFormat.CHANNEL_OUT_STEREO else AudioFormat.CHANNEL_OUT_MONO
        val minBuf = AudioTrack.getMinBufferSize(rate, mask, AudioFormat.ENCODING_PCM_16BIT)
        val t = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(rate)
                    .setChannelMask(mask)
                    .build()
            )
            .setBufferSizeInBytes(maxOf(minBuf * 4, rate / 2 * 2 * channels))  // ≥500ms — absorb New-Arch scheduling jitter
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        t.setVolume(volume)
        t.play()
        extTrack = t
        extRate = rate
        extChannels = channels
        Log.i(TAG, "ext AudioTrack ${rate}Hz ch=$channels")
    }

    fun setVolumeNative(v: Float) {
        volume = v.coerceIn(0f, 1f)
        track?.setVolume(volume)
        extTrack?.setVolume(volume)
    }

    fun sendRawCommand(json: String) {
        val sock = ws ?: return
        sock.send(json)
    }

    fun revive() {
        mainHandler.post { reviveIfDead(3_000) }
    }

    private fun tuneByStep(direction: Int) {
        // External (OWRX/Kiwi): tuning lives in JS — delegate so we don't tune the
        // native UberSDR WS (resurrecting a session). JS handles step vs bookmark
        // vs DAB-programme cycling from its own state.
        if (externalAudio || skipMode == "bookmark") {
            emitEvent("VibeSkip") { it.putString("direction", if (direction > 0) "next" else "prev") }
            return
        }
        val newFreq = snapStep(direction)
        currentFreq = newFreq
        // Stale VTS strings (old station name) — fall back until JS catches up
        npTitle = null
        npArtist = null
        sendWsJson(JSONObject().put("type", "tune").put("frequency", newFreq))
        emitEvent("VibeTuned") {
            it.putDouble("frequency", newFreq.toDouble())
            it.putString("mode", currentMode)
        }
        mainHandler.post { updateMetadataSession(); updateNotification() }
    }

    /** Snap a media-control skip to the step grid, matching the VFO drum: an
     *  off-grid frequency lands on the next/previous multiple of the step; an
     *  on-grid one moves exactly one step. direction +1 = up, -1 = down. */
    private fun snapStep(direction: Int): Long {
        val s = currentStep
        if (s <= 0) return currentFreq.coerceAtLeast(100_000L)
        val snapped = if (direction > 0) (currentFreq / s + 1) * s
                      else ((currentFreq + s - 1) / s - 1) * s
        return snapped.coerceAtLeast(100_000L)
    }

    // ── WebSocket ────────────────────────────────────────────────────────────

    private fun wsUrl(): String {
        var s = currentBase.trim().trimEnd('/')
        s = when {
            s.startsWith("https://") -> "wss://" + s.removePrefix("https://")
            s.startsWith("http://") -> "ws://" + s.removePrefix("http://")
            else -> s
        }
        var url = "$s/ws?user_session_id=$currentUuid&frequency=$currentFreq" +
            "&mode=$currentMode&format=opus&version=2"
        if (bypassPassword.isNotEmpty()) {
            url += "&password=" + java.net.URLEncoder.encode(bypassPassword, "UTF-8")
        }
        return url
    }

    private fun openWs() {
        val client = httpClient ?: OkHttpClient.Builder()
            .readTimeout(0, TimeUnit.MILLISECONDS)
            .pingInterval(20, TimeUnit.SECONDS)
            .build().also { httpClient = it }
        val url = wsUrl()
        Log.i(TAG, "opening audio WS: $url")
        wsBaseSr = 0
        srFlipCount = 0
        val socket = client.newWebSocket(Request.Builder().url(url).build(),
            object : WebSocketListener() {
                override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                    if (!running || ws !== webSocket) return
                    packetCount++
                    lastPacketAt = SystemClock.elapsedRealtime()
                    if (packetCount <= 3) Log.i(TAG, "ws pkt#$packetCount len=${bytes.size}")
                    // Header rate flip → server encoder mismatched, cycle WS
                    if (bytes.size > HEADER_LEN) {
                        val sr = (bytes[8].toInt() and 0xFF) or ((bytes[9].toInt() and 0xFF) shl 8) or
                            ((bytes[10].toInt() and 0xFF) shl 16) or ((bytes[11].toInt() and 0xFF) shl 24)
                        if (sr in 8000..96000) {
                            if (wsBaseSr == 0) {
                                wsBaseSr = sr
                            } else if (sr != wsBaseSr) {
                                srFlipCount++
                                if (srFlipCount >= 3 &&
                                    SystemClock.elapsedRealtime() - lastSrCycleAt > 4_000) {
                                    Log.i(TAG, "sample rate $wsBaseSr→$sr — cycling WS for fresh server encoder")
                                    lastSrCycleAt = SystemClock.elapsedRealtime()
                                    webSocket.cancel()
                                    ws = null
                                    mainHandler.post { if (running) openWs() }
                                    return
                                }
                            } else {
                                srFlipCount = 0
                            }
                        }
                    }
                    // Recording keeps decoding through mutes (file taps the
                    // decoded feed); playback is gated at the AudioTrack write
                    if (!muted || recArmed) {
                        // Live-edge bound: drop OLDEST on overflow
                        val arr = bytes.toByteArray()
                        if (!packetQueue.offerLast(arr)) {
                            packetQueue.pollFirst()
                            packetQueue.offerLast(arr)
                        }
                    }
                }

                override fun onMessage(webSocket: WebSocket, text: String) {
                    if (!running || ws !== webSocket) return
                    // dsp_filters / dsp_status / dsp_error etc. — JS owns the
                    // server-NR UI (same event name as iOS)
                    emitEvent("VibeWsText") { it.putString("text", text) }
                }

                override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                    if (!running || ws !== webSocket) return
                    Log.w(TAG, "ws failure: ${t.message} — reconnecting in 2s")
                    mainHandler.postDelayed({
                        // SAME uuid — decoders + spectrum WS are keyed to it
                        if (running && ws === webSocket) { ws = null; openWs() }
                    }, 2_000)
                }
            })
        ws = socket
    }

    private fun sendWsJson(obj: JSONObject) {
        ws?.send(obj.toString())
    }

    // ── Watchdog (zombie-socket revive, iOS parity) ──────────────────────────

    private fun startWatchdog() {
        watchdog?.let { mainHandler.removeCallbacks(it) }
        val r = object : Runnable {
            override fun run() {
                if (!running) return
                reviveIfDead(8_000)
                mainHandler.postDelayed(this, 4_000)
            }
        }
        watchdog = r
        mainHandler.postDelayed(r, 4_000)
    }

    private fun reviveIfDead(staleAfterMs: Long) {
        // External (OWRX/Kiwi) has no native WS to revive — JS owns it.
        if (!running || externalAudio || dataSaverDisconnected) return  // data saver owns the closed WS
        val stale = SystemClock.elapsedRealtime() - lastPacketAt
        if (stale <= staleAfterMs && ws != null) return
        Log.i(TAG, "watchdog: stale=${stale}ms — reviving audio WS")
        lastPacketAt = SystemClock.elapsedRealtime() // debounce one revive/window
        ws?.cancel()
        ws = null
        openWs()
    }

    // ── Decode thread: MediaCodec opus → AudioTrack ──────────────────────────

    private var codec: MediaCodec? = null
    private var codecChannels = 0
    @Volatile private var track: AudioTrack? = null
    private var trackRate = 0
    private var trackChannels = 0
    private var ptsUs = 0L

    private fun startDecodeThread() {
        val t = Thread({
            try {
                decodeLoop()
            } catch (e: InterruptedException) {
                // normal shutdown
            } catch (e: Exception) {
                Log.e(TAG, "decode loop died: ${e.message}", e)
            } finally {
                releaseCodec()
                recArmed = false
                releaseRecorder()  // finalises the .m4a if a recording was live
                recStartReq?.reject("stopped", "audio engine stopped"); recStartReq = null
                recStopReq?.resolve(null); recStopReq = null
                track?.release()
                track = null
                trackRate = 0
                trackChannels = 0
            }
        }, "vibesdr-decode")
        t.priority = Thread.MAX_PRIORITY
        decodeThread = t
        t.start()
    }

    private fun decodeLoop() {
        val info = MediaCodec.BufferInfo()
        while (running) {
            handleRecRequests()   // runs even when no packets flow
            val pkt = packetQueue.poll(250, TimeUnit.MILLISECONDS) ?: continue
            if (pkt.size <= HEADER_LEN) continue
            val ch = pkt[12].toInt() and 0xFF
            if (ch != 1 && ch != 2) continue
            val opusLen = pkt.size - HEADER_LEN
            if (opusLen < 3) continue
            // Stream sample rate from the header (encoder input rate) — the
            // DSP engines run at this rate for skin parity
            val sr = (pkt[8].toInt() and 0xFF) or ((pkt[9].toInt() and 0xFF) shl 8) or
                ((pkt[10].toInt() and 0xFF) shl 16) or ((pkt[11].toInt() and 0xFF) shl 24)
            if (sr in 8000..96000) streamSr = sr

            // SNR meter = radiod channel SNR = basebandPower − noiseDensity (both
            // dBFS, per-packet header) — the demodulator's own channel measure, so
            // independent of the spectrum/zoom (matches UberSDR).
            val bb = Float.fromBits((pkt[13].toInt() and 0xFF) or ((pkt[14].toInt() and 0xFF) shl 8) or
                ((pkt[15].toInt() and 0xFF) shl 16) or ((pkt[16].toInt() and 0xFF) shl 24))
            val nd = Float.fromBits((pkt[17].toInt() and 0xFF) or ((pkt[18].toInt() and 0xFF) shl 8) or
                ((pkt[19].toInt() and 0xFF) shl 16) or ((pkt[20].toInt() and 0xFF) shl 24))
            if (bb > -900f && nd > -900f) {
                val now = SystemClock.elapsedRealtime()
                if (now - lastSignalEmit > 200) {
                    lastSignalEmit = now
                    emitEvent("VibeSignal") { it.putDouble("snr", (bb - nd).toDouble()); it.putDouble("dbfs", bb.toDouble()) }
                }
            }

            // Channel-count flip → rebuild codec synchronously (no race:
            // this thread owns codec + track exclusively)
            if (codec == null || codecChannels != ch) ensureCodec(ch)
            val c = codec ?: continue

            // Feed input (drain between attempts so the decoder never stalls)
            var fed = false
            var attempts = 0
            while (!fed && attempts < 50 && running) {
                val inIdx = c.dequeueInputBuffer(10_000)
                if (inIdx >= 0) {
                    val ib = c.getInputBuffer(inIdx) ?: break
                    ib.clear()
                    ib.put(pkt, HEADER_LEN, opusLen)
                    c.queueInputBuffer(inIdx, 0, opusLen, ptsUs, 0)
                    ptsUs += 20_000
                    fed = true
                }
                drainOutput(c, info)
                attempts++
            }
            drainOutput(c, info)
        }
    }

    private fun drainOutput(c: MediaCodec, info: MediaCodec.BufferInfo) {
        while (true) {
            val outIdx = c.dequeueOutputBuffer(info, 0)
            when {
                outIdx >= 0 -> {
                    val ob = c.getOutputBuffer(outIdx)
                    if (ob != null && info.size > 0) {
                        var pcm = ByteArray(info.size)
                        ob.position(info.offset)
                        ob.get(pcm, 0, info.size)
                        ensureTrackFor(c.outputFormat)
                        if (trackChannels == 1 && (nbOn || nrMode != "off")) {
                            pcm = dspProcess(pcm)
                        }
                        notchBytes(pcm, trackChannels)   // auto notch (network)
                        if (!squelchOpen) java.util.Arrays.fill(pcm, 0)  // squelch gate
                        if (recArmed) encodePcm(pcm)
                        if (!muted) track?.write(pcm, 0, pcm.size)  // blocking = backpressure
                    }
                    c.releaseOutputBuffer(outIdx, false)
                }
                outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    ensureTrackFor(c.outputFormat)
                }
                else -> return
            }
        }
    }

    // ── Client noise DSP (decode-thread only) ────────────────────────────────

    // ── Auto notch (NLMS adaptive line enhancer) ─────────────────────────────
    // Same spec as the shared C++ vibe::AutoNotch / the iOS Swift port, so all
    // backends sound identical. Applied per channel on the final PCM, after any
    // decoder taps and the existing NB/NR DSP, just before playback/record.
    private fun notchShorts(sh: ShortArray, channels: Int) {
        if (!notchOn) return
        val frames = sh.size / channels
        if (frames <= 0) return
        if (notchF0.size < frames) notchF0 = FloatArray(frames)
        if (channels >= 2 && notchF1.size < frames) notchF1 = FloatArray(frames)
        if (channels == 1) {
            val f = notchF0
            for (i in 0 until frames) f[i] = sh[i] / 32768f
            (notchCh[0] ?: AutoNotch().also { notchCh[0] = it }).process(f, frames)
            for (i in 0 until frames) sh[i] = (f[i] * 32768f).toInt().coerceIn(-32768, 32767).toShort()
        } else {
            val fl = notchF0; val fr = notchF1
            for (i in 0 until frames) { fl[i] = sh[i * 2] / 32768f; fr[i] = sh[i * 2 + 1] / 32768f }
            (notchCh[0] ?: AutoNotch().also { notchCh[0] = it }).process(fl, frames)
            (notchCh[1] ?: AutoNotch().also { notchCh[1] = it }).process(fr, frames)
            for (i in 0 until frames) {
                sh[i * 2]     = (fl[i] * 32768f).toInt().coerceIn(-32768, 32767).toShort()
                sh[i * 2 + 1] = (fr[i] * 32768f).toInt().coerceIn(-32768, 32767).toShort()
            }
        }
    }
    private fun notchBytes(pcm: ByteArray, channels: Int) {
        if (!notchOn) return
        val sh = ShortArray(pcm.size / 2)
        ByteBuffer.wrap(pcm).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().get(sh)
        notchShorts(sh, channels)
        ByteBuffer.wrap(pcm).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().put(sh)
    }
    private class AutoNotch {
        private val D = 8; private val L = 160; private val M = D + L
        private val buf = FloatArray(2 * M); private val w = FloatArray(L); private var p = 0
        private val mu = 0.003f; private val leak = 0.9999f; private val eps = 1e-6f
        fun process(x: FloatArray, count: Int) {
            for (n in 0 until count) {
                p = if (p == 0) M - 1 else p - 1
                val inp = x[n]
                buf[p] = inp; buf[p + M] = inp
                val base = p + D
                var fir = 0f; var pwr = 0f
                for (i in 0 until L) { val s = buf[base + i]; fir += w[i] * s; pwr += s * s }
                val err = inp - fir
                x[n] = err
                val g = mu * err / (eps + pwr)
                for (i in 0 until L) w[i] = leak * w[i] + g * buf[base + i]
            }
        }
    }

    /** 16-bit mono 48k PCM → decimate to streamSr → NB → NR/NR2 → back to 48k. */
    private fun dspProcess(pcm: ByteArray): ByteArray {
        val sr = streamSr
        val factor = if (sr > 0) 48_000 / sr else 1
        if (factor < 1 || 48_000 % sr != 0) return pcm

        if (dspResetRequested || dspRate != sr) {
            dspRate = sr
            dspResetRequested = false
            nbEngine = null
            nr2Chunker = null
            websdrEngine = null
            websdrChunker = null
        }

        val n48 = pcm.size / 2
        val n = n48 / factor
        if (n == 0) return pcm

        // Decimate: boxcar average per factor group (content is band-limited
        // to the stream rate already — opus encoded at sr)
        var mono = FloatArray(n)
        var bi = 0
        for (i in 0 until n) {
            var acc = 0.0f
            for (k in 0 until factor) {
                val lo = pcm[bi].toInt() and 0xFF
                val hi = pcm[bi + 1].toInt()
                acc += ((hi shl 8) or lo) / 32768.0f
                bi += 2
            }
            mono[i] = acc / factor
        }

        if (nbOn) {
            if (nbEngine == null) nbEngine = NoiseBlankerEngine(sr.toDouble())
            nbEngine!!.process(mono)
        }
        when (nrMode) {
            "nr2" -> {
                if (nr2Chunker == null) {
                    val eng = NR2Engine()
                    nr2Chunker = BlockChunker(512) { blk -> eng.processHop(blk) }
                }
                mono = nr2Chunker!!.run(mono)
            }
            "nr" -> {
                if (websdrChunker == null) {
                    val eng = WebSDRNREngine()
                    eng.syncBins(nrBandwidthHz, sr.toDouble())
                    websdrEngine = eng
                    websdrChunker = BlockChunker(WebSDRNREngine.BLOCK) { blk -> eng.processWithDelay(blk) }
                }
                websdrEngine?.syncBins(nrBandwidthHz, sr.toDouble())
                mono = websdrChunker!!.run(mono)
            }
        }

        // Interpolate back to 48k (linear)
        val out = ByteArray(n * factor * 2)
        var oi = 0
        for (i in 0 until n) {
            val cur = mono[i]
            val next = if (i + 1 < n) mono[i + 1] else cur
            for (k in 0 until factor) {
                val v = cur + (next - cur) * k / factor
                val s = (v * 32767.0f).toInt().coerceIn(-32768, 32767)
                out[oi] = (s and 0xFF).toByte()
                out[oi + 1] = ((s shr 8) and 0xFF).toByte()
                oi += 2
            }
        }
        return out
    }

    fun requestDspReset() { dspResetRequested = true }

    // ── Recorder (decode-thread state machine) ───────────────────────────────

    private fun handleRecRequests() {
        recStartReq?.let { promise ->
            recStartReq = null
            try {
                val ts = java.text.SimpleDateFormat("yyyy-MM-dd'T'HH-mm-ss", java.util.Locale.US)
                    .format(java.util.Date())
                val rf = if (recReqFreq > 0) recReqFreq else currentFreq.toDouble()
                val rm = if (recReqMode.isNotEmpty()) recReqMode else currentMode
                val mhz = String.format(java.util.Locale.US, "%.4fMHz", rf / 1e6)
                recDisplayName = "VibeSDR_${ts}_${mhz}_${rm.uppercase()}.m4a"
                // filesDir == Expo's FileSystem.documentDirectory, so the JS
                // RecordingsOverlay can list/play/delete it. (Was cacheDir +
                // MediaStore publish; now the in-app browser is the home for
                // recordings on both platforms — share exports elsewhere.)
                val f = java.io.File(filesDir, recDisplayName)
                // External audio (local/Kiwi/OWRX) has its own rate/channels;
                // the UberSDR path is always 48k. Match the encoder to the source
                // so recordings play at the right speed.
                recRate = if (externalAudio && extRate > 0) extRate else 48_000
                recChannels = if (externalAudio) extChannels.coerceIn(1, 2)
                              else if (trackChannels == 2) 2 else 1
                val fmt = MediaFormat.createAudioFormat(
                    MediaFormat.MIMETYPE_AUDIO_AAC, recRate, recChannels)
                fmt.setInteger(MediaFormat.KEY_AAC_PROFILE,
                    android.media.MediaCodecInfo.CodecProfileLevel.AACObjectLC)
                fmt.setInteger(MediaFormat.KEY_BIT_RATE, 128_000)
                fmt.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 65_536)
                val enc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
                enc.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                enc.start()
                recEncoder = enc
                recMuxer = android.media.MediaMuxer(
                    f.absolutePath, android.media.MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
                recTrackIndex = -1
                recMuxerStarted = false
                recPtsUs = 0
                recFile = f
                recArmed = true
                Log.i(TAG, "recording → $recDisplayName")
                promise.resolve(f.absolutePath)
            } catch (e: Exception) {
                Log.e(TAG, "rec start failed: ${e.message}")
                promise.reject("rec_open", e.message)
                releaseRecorder()
            }
        }
        recStopReq?.let { promise ->
            recStopReq = null
            recArmed = false
            try {
                val enc = recEncoder
                if (enc != null) {
                    val idx = enc.dequeueInputBuffer(50_000)
                    if (idx >= 0) {
                        enc.queueInputBuffer(idx, 0, 0, recPtsUs,
                            MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                    }
                    drainEncoder(true)
                }
                val src = recFile
                releaseRecorder()
                // Keep the file in filesDir (in-app browser is its home); return
                // the bare path. Sharing wraps it in Expo's content provider JS-side.
                promise.resolve(src?.absolutePath)
            } catch (e: Exception) {
                Log.e(TAG, "rec stop failed: ${e.message}")
                releaseRecorder()
                promise.reject("rec_stop", e.message)
            }
        }
    }

    private fun encodePcm(pcm: ByteArray, srcChannels: Int = trackChannels) {
        val enc = recEncoder ?: return
        // Source channels can flip mid-recording — adapt to the encoder's count
        val data = matchChannels(pcm, srcChannels, recChannels)
        var off = 0
        try {
            while (off < data.size) {
                val idx = enc.dequeueInputBuffer(10_000)
                if (idx < 0) { drainEncoder(false); continue }
                val ib = enc.getInputBuffer(idx) ?: break
                ib.clear()
                val chunk = min(ib.capacity(), data.size - off)
                ib.put(data, off, chunk)
                enc.queueInputBuffer(idx, 0, chunk, recPtsUs, 0)
                recPtsUs += (chunk / (2L * recChannels)) * 1_000_000L / recRate
                off += chunk
            }
            drainEncoder(false)
        } catch (e: Exception) {
            Log.e(TAG, "rec encode failed: ${e.message}")
            recArmed = false
            releaseRecorder()
        }
    }

    private fun drainEncoder(eos: Boolean) {
        val enc = recEncoder ?: return
        val muxer = recMuxer ?: return
        val info = MediaCodec.BufferInfo()
        while (true) {
            val outIdx = enc.dequeueOutputBuffer(info, if (eos) 50_000 else 0)
            when {
                outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    recTrackIndex = muxer.addTrack(enc.outputFormat)
                    muxer.start()
                    recMuxerStarted = true
                }
                outIdx >= 0 -> {
                    val ob = enc.getOutputBuffer(outIdx)
                    if (ob != null && info.size > 0 && recMuxerStarted &&
                        (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                        muxer.writeSampleData(recTrackIndex, ob, info)
                    }
                    enc.releaseOutputBuffer(outIdx, false)
                    if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) return
                }
                else -> return
            }
        }
    }

    private fun matchChannels(pcm: ByteArray, from: Int, to: Int): ByteArray {
        if (from == to) return pcm
        val frames = pcm.size / (2 * from)
        val out = ByteArray(frames * 2 * to)
        var ii = 0; var oi = 0
        for (f in 0 until frames) {
            if (from == 1) { // mono → stereo: duplicate
                out[oi] = pcm[ii]; out[oi + 1] = pcm[ii + 1]
                out[oi + 2] = pcm[ii]; out[oi + 3] = pcm[ii + 1]
                ii += 2; oi += 4
            } else {         // stereo → mono: average
                val l = ((pcm[ii + 1].toInt() shl 8) or (pcm[ii].toInt() and 0xFF))
                val r = ((pcm[ii + 3].toInt() shl 8) or (pcm[ii + 2].toInt() and 0xFF))
                val m = (l + r) / 2
                out[oi] = (m and 0xFF).toByte(); out[oi + 1] = ((m shr 8) and 0xFF).toByte()
                ii += 4; oi += 2
            }
        }
        return out
    }

    private fun releaseRecorder() {
        try { recEncoder?.stop() } catch (_: Exception) {}
        try { recEncoder?.release() } catch (_: Exception) {}
        recEncoder = null
        try { if (recMuxerStarted) recMuxer?.stop() } catch (_: Exception) {}
        try { recMuxer?.release() } catch (_: Exception) {}
        recMuxer = null
        recMuxerStarted = false
        recTrackIndex = -1
    }

    /** Copy the finished .m4a into MediaStore (Music/VibeSDR) so it's visible
     *  in music/file apps; returns a shareable content:// URI (or the raw
     *  file path below API 29). */
    private fun publishRecording(src: java.io.File): String {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return src.absolutePath
        return try {
            val values = android.content.ContentValues().apply {
                put(android.provider.MediaStore.Audio.Media.DISPLAY_NAME, recDisplayName)
                put(android.provider.MediaStore.Audio.Media.MIME_TYPE, "audio/mp4")
                put(android.provider.MediaStore.Audio.Media.RELATIVE_PATH, "Music/VibeSDR")
                put(android.provider.MediaStore.Audio.Media.IS_PENDING, 1)
            }
            val uri = contentResolver.insert(
                android.provider.MediaStore.Audio.Media.EXTERNAL_CONTENT_URI, values)
                ?: return src.absolutePath
            contentResolver.openOutputStream(uri)?.use { out ->
                java.io.FileInputStream(src).use { it.copyTo(out) }
            }
            values.clear()
            values.put(android.provider.MediaStore.Audio.Media.IS_PENDING, 0)
            contentResolver.update(uri, values, null, null)
            src.delete()
            uri.toString()
        } catch (e: Exception) {
            Log.w(TAG, "MediaStore publish failed: ${e.message}")
            src.absolutePath
        }
    }

    fun startRecordingNative(frequency: Double, mode: String, promise: com.facebook.react.bridge.Promise) {
        if (!running) { promise.reject("not_running", "Audio engine is not running"); return }
        // Use the frequency/mode the UI passes (accurate for every backend,
        // including local/Kiwi where the service's currentFreq isn't synced).
        if (frequency > 0) recReqFreq = frequency
        if (mode.isNotEmpty()) recReqMode = mode
        recStartReq = promise
    }

    fun stopRecordingNative(promise: com.facebook.react.bridge.Promise) {
        recStopReq = promise
    }

    /** System share sheet for the finished recording (content:// URI). */
    fun shareRecordingNative(uriOrPath: String) {
        try {
            val uri = if (uriOrPath.startsWith("content:")) android.net.Uri.parse(uriOrPath)
                      else return  // bare paths (pre-Q fallback) aren't shareable without a provider
            val send = Intent(Intent.ACTION_SEND).apply {
                type = "audio/mp4"
                putExtra(Intent.EXTRA_STREAM, uri)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            val chooser = Intent.createChooser(send, "Share recording").apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            }
            startActivity(chooser)
        } catch (e: Exception) {
            Log.w(TAG, "share failed: ${e.message}")
        }
    }

    private fun ensureCodec(ch: Int) {
        releaseCodec()
        val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, 48_000, ch)
        format.setByteBuffer("csd-0", ByteBuffer.wrap(opusHead(ch)))
        format.setByteBuffer("csd-1", ByteBuffer.wrap(le64(0)))           // pre-skip ns
        format.setByteBuffer("csd-2", ByteBuffer.wrap(le64(80_000_000)))  // seek pre-roll ns
        val c = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS)
        c.configure(format, null, null, 0)
        c.start()
        codec = c
        codecChannels = ch
        ptsUs = 0
        Log.i(TAG, "opus decoder created ch=$ch")
    }

    private fun releaseCodec() {
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        codec = null
        codecChannels = 0
    }

    /** OpusHead identification header (RFC 7845 §5.1) for MediaCodec csd-0. */
    private fun opusHead(ch: Int): ByteArray {
        val b = ByteBuffer.allocate(19).order(ByteOrder.LITTLE_ENDIAN)
        b.put("OpusHead".toByteArray(Charsets.US_ASCII))
        b.put(1)                 // version
        b.put(ch.toByte())       // channel count
        b.putShort(0)            // pre-skip
        b.putInt(48_000)         // input sample rate (informational)
        b.putShort(0)            // output gain
        b.put(0)                 // mapping family
        return b.array()
    }

    private fun le64(v: Long): ByteArray =
        ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(v).array()

    private fun ensureTrackFor(format: MediaFormat) {
        val rate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
        val ch = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
        if (track != null && trackRate == rate && trackChannels == ch) return
        track?.release()
        val mask = if (ch == 2) AudioFormat.CHANNEL_OUT_STEREO else AudioFormat.CHANNEL_OUT_MONO
        val minBuf = AudioTrack.getMinBufferSize(rate, mask, AudioFormat.ENCODING_PCM_16BIT)
        val t = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(rate)
                    .setChannelMask(mask)
                    .build()
            )
            .setBufferSizeInBytes(maxOf(minBuf * 2, 9_600 * ch)) // ≥100ms
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        t.setVolume(volume)
        t.play()
        track = t
        trackRate = rate
        trackChannels = ch
        Log.i(TAG, "AudioTrack ${rate}Hz ${ch}ch")
    }

    // ── Events to JS ─────────────────────────────────────────────────────────

    private fun emitEvent(name: String, fill: (com.facebook.react.bridge.WritableMap) -> Unit) {
        val ctx = reactContext ?: return
        try {
            val map = Arguments.createMap()
            fill(map)
            ctx.getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter::class.java)
                .emit(name, map)
        } catch (e: Exception) {
            Log.w(TAG, "emit $name failed: ${e.message}")
        }
    }

    // ── Car browse tree (MediaBrowserServiceCompat) ──────────────────────────

    override fun onGetRoot(
        clientPackageName: String, clientUid: Int, rootHints: Bundle?,
    ): BrowserRoot? {
        // Opt OUT of Android 11+ media resumption (the system "resume" query sets
        // EXTRA_RECENT). On some skins (HyperOS/MIUI) the resume card hijacks the
        // LIVE MediaSession when the app is backgrounded — showing a paused card
        // while audio plays, failing to resume ("Cannot resume"), so the lock-screen
        // play/pause springs and never reaches our onPause/onPlay. Returning null
        // here keeps the live session controls in charge. Live SDR isn't a
        // resumable on-demand stream anyway.
        if (rootHints?.getBoolean(BrowserRoot.EXTRA_RECENT) == true) return null
        // Any other caller may browse — the tree is just public bookmarks + the
        // band plan (no user data). Android Auto / Wear connect here.
        return BrowserRoot(MEDIA_ROOT_ID, null)
    }

    override fun onLoadChildren(
        parentId: String, result: Result<MutableList<MediaBrowserCompat.MediaItem>>,
    ) {
        val items = ArrayList<MediaBrowserCompat.MediaItem>()
        when (parentId) {
            MEDIA_ROOT_ID -> {
                items.add(browsableFolder(BOOKMARKS_ID, "Bookmarks"))
                items.add(browsableFolder(BANDS_ID, "Band Plan"))
            }
            BOOKMARKS_ID -> browseBookmarks.forEach { items.add(playableItem(it)) }
            BANDS_ID -> browseBands.forEach { items.add(playableItem(it)) }
        }
        result.sendResult(items)
    }

    private fun browsableFolder(id: String, title: String): MediaBrowserCompat.MediaItem {
        val desc = MediaDescriptionCompat.Builder().setMediaId(id).setTitle(title).build()
        return MediaBrowserCompat.MediaItem(desc, MediaBrowserCompat.MediaItem.FLAG_BROWSABLE)
    }

    private fun playableItem(it: BrowseItem): MediaBrowserCompat.MediaItem {
        val mhz = String.format(java.util.Locale.US, "%.3f MHz", it.freq / 1_000_000.0)
        val sub = if (it.mode.isNotEmpty()) "$mhz · ${it.mode.uppercase()}" else mhz
        val desc = MediaDescriptionCompat.Builder()
            // id = freq|mode|step|isBand — parsed back in playFromBrowseId
            .setMediaId("${it.freq}|${it.mode}|${it.step}|${if (it.isBand) 1 else 0}")
            .setTitle(it.name)
            .setSubtitle(sub)
            .build()
        return MediaBrowserCompat.MediaItem(desc, MediaBrowserCompat.MediaItem.FLAG_PLAYABLE)
    }

    private fun playFromBrowseId(mediaId: String?) {
        val parts = mediaId?.split("|") ?: return
        if (parts.size < 4) return
        val freq = parts[0].toLongOrNull() ?: return
        val mode = parts[1]
        val isBand = parts[3] == "1"
        emitEvent("VibeCarTune") {
            it.putDouble("frequency", freq.toDouble())
            it.putString("mode", mode.ifEmpty { null })
            it.putBoolean("isBand", isBand)
        }
    }

    /** JS pushes the browse payload ({bookmarks:[{name,frequency,mode}],
     *  bands:[{name,frequency,mode,step}]}). Cached to prefs + Auto refreshed. */
    fun setBrowseItemsNative(json: String) {
        try {
            val obj = JSONObject(json)
            browseBookmarks = parseBrowseArr(obj.optJSONArray("bookmarks"), isBand = false)
            browseBands = parseBrowseArr(obj.optJSONArray("bands"), isBand = true)
            getSharedPreferences("vibesdr_browse", MODE_PRIVATE)
                .edit().putString("payload", json).apply()
            mainHandler.post {
                notifyChildrenChanged(BOOKMARKS_ID)
                notifyChildrenChanged(BANDS_ID)
            }
        } catch (e: Exception) {
            Log.w(TAG, "setBrowseItems parse failed: ${e.message}")
        }
    }

    private fun parseBrowseArr(arr: org.json.JSONArray?, isBand: Boolean): List<BrowseItem> {
        if (arr == null) return emptyList()
        val out = ArrayList<BrowseItem>(arr.length())
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val name = o.optString("name")
            val freq = o.optDouble("frequency", 0.0).toLong()
            if (name.isEmpty() || freq <= 0) continue
            out.add(BrowseItem(name, freq, o.optString("mode", ""), o.optLong("step", 0), isBand))
        }
        return out
    }

    private fun loadCachedBrowse() {
        val json = getSharedPreferences("vibesdr_browse", MODE_PRIVATE)
            .getString("payload", null) ?: return
        try {
            val obj = JSONObject(json)
            browseBookmarks = parseBrowseArr(obj.optJSONArray("bookmarks"), isBand = false)
            browseBands = parseBrowseArr(obj.optJSONArray("bands"), isBand = true)
        } catch (_: Exception) {}
    }

    // ── Car-connected signal (Android Auto projection/native) ────────────────

    private fun startCarConnectionWatch() {
        try {
            val cc = CarConnection(this)
            val obs = Observer<Int> { type ->
                val connected = type == CarConnection.CONNECTION_TYPE_PROJECTION ||
                    type == CarConnection.CONNECTION_TYPE_NATIVE
                emitEvent("VibeCarConnected") { it.putBoolean("connected", connected) }
            }
            cc.type.observeForever(obs)
            carConnection = cc
            carObserver = obs
        } catch (e: Exception) {
            Log.w(TAG, "CarConnection watch failed: ${e.message}")
        }
    }

    private fun stopCarConnectionWatch() {
        carObserver?.let { carConnection?.type?.removeObserver(it) }
        carObserver = null
        carConnection = null
    }

    // ── Notification / MediaSession ──────────────────────────────────────────

    private fun nowPlayingTitle(): String {
        if (reconnectFailed) return "Failed to reconnect"
        if (dataSaverDisconnected) return "Disconnected"
        return npTitle ?: run {
            val mhz = String.format("%.3f MHz", currentFreq / 1_000_000.0)
            "$mhz ${currentMode.uppercase()}"
        }
    }

    private fun nowPlayingArtist(): String {
        if (reconnectFailed) return "Open VibeSDR to reconnect"
        if (dataSaverDisconnected) return "VibeSDR — press ▶ to reconnect"
        return npArtist ?: instanceName.ifEmpty { currentBase }
    }

    /** VTS-aware now-playing strings from JS (empty string clears). */
    fun setNowPlayingNative(title: String, artist: String) {
        npTitle = title.ifEmpty { null }
        npArtist = artist.ifEmpty { null }
        mainHandler.post { updateMetadataSession(); updateNotification() }
    }

    // CarFM: the album slot carries the tuned frequency (spec §5b, freq -> ALBUM)
    // so the ESP32 display gets it alongside PS/RT. Empty restores the default.
    private var npAlbum: String? = null

    fun setNowPlayingAlbumNative(album: String) {
        npAlbum = album.ifEmpty { null }
        mainHandler.post { updateMetadataSession(); updateNotification() }
    }

    /** Album art: VibeSDR icon with the server-type logo inset bottom-right
     *  (multi-server prep — type picks the overlay, "ubersdr" for now). */
    fun setArtworkNative(serverType: String) {
        if (serverType == npArtworkType) return
        npArtworkType = serverType
        mainHandler.post { refreshArtwork(); updateMetadataSession(); updateNotification() }
    }

    // State-aware album art: server logo inset while playing, a muted-speaker
    // glyph + minutes-to-disconnect while muted, a disconnected glyph once the
    // data saver drops the stream. Keyed so it only recomposites on change.
    private var lastArtworkKey = ""
    private fun refreshArtwork() {
        val key = when {
            reconnectFailed -> "fail"
            dataSaverDisconnected -> "disc"
            else -> "play-$npArtworkType"
        }
        if (key == lastArtworkKey) return
        lastArtworkKey = key
        try {
            val base = android.graphics.BitmapFactory.decodeResource(
                resources, R.drawable.artwork_base) ?: return
            val composed = base.copy(android.graphics.Bitmap.Config.ARGB_8888, true)
            val canvas = android.graphics.Canvas(composed)
            val inset = composed.width * 0.30f
            val pad = composed.width * 0.045f
            val dst = android.graphics.RectF(
                composed.width - inset - pad, composed.height - inset - pad,
                composed.width - pad, composed.height - pad)
            when {
                reconnectFailed -> { drawInsetGlyph(canvas, dst, "⛔", null); drawBadge(canvas, dst, "❗") }
                dataSaverDisconnected -> drawInsetGlyph(canvas, dst, "⛔", null)
                // Local hardware (RTL-SDR): the custom USB-SDR icon (same as the
                // LOCAL HARDWARE menu card) on a dark inset, so its card is distinct
                // from a network UberSDR session.
                npArtworkType == "local" -> drawInsetVector(canvas, dst, R.drawable.ic_local_sdr)
                // RTL-TCP: the supplied rtltcp icon (black line art) amber-tinted on
                // the same dark inset, matching its menu card.
                npArtworkType == "rtltcp" -> drawInsetTintedBitmap(canvas, dst, R.drawable.logo_rtltcp, 0xFFFFB833.toInt())
                else -> {
                    val overlayId = resources.getIdentifier("logo_$npArtworkType", "drawable", packageName)
                    if (overlayId != 0) {
                        android.graphics.BitmapFactory.decodeResource(resources, overlayId)?.let {
                            canvas.drawBitmap(it, null, dst, null)
                        }
                    }
                }
            }
            npArtwork = composed
        } catch (e: Exception) {
            Log.w(TAG, "artwork composite failed: ${e.message}")
        }
    }

    /** Inset a (black line-art) bitmap on the dark rounded box, tinted so it reads
     *  over the app artwork — used for the supplied RTL-TCP icon. */
    private fun drawInsetTintedBitmap(canvas: android.graphics.Canvas, dst: android.graphics.RectF, drawableId: Int, tint: Int) {
        val bg = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply { color = 0xCC101418.toInt() }
        val r = dst.width() * 0.18f
        canvas.drawRoundRect(dst, r, r, bg)
        val d = androidx.core.content.ContextCompat.getDrawable(this, drawableId) ?: return
        d.mutate(); d.setTint(tint)
        val padIn = dst.width() * 0.16f
        d.setBounds((dst.left + padIn).toInt(), (dst.top + padIn).toInt(),
                    (dst.right - padIn).toInt(), (dst.bottom - padIn).toInt())
        d.draw(canvas)
    }

    /** Inset a vector drawable (e.g. the local USB-SDR icon) on the same dark
     *  rounded box drawInsetGlyph uses, so it reads over the app artwork. */
    private fun drawInsetVector(canvas: android.graphics.Canvas, dst: android.graphics.RectF, drawableId: Int) {
        val bg = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply { color = 0xCC101418.toInt() }
        val r = dst.width() * 0.18f
        canvas.drawRoundRect(dst, r, r, bg)
        val d = androidx.core.content.ContextCompat.getDrawable(this, drawableId) ?: return
        val padIn = dst.width() * 0.16f
        d.setBounds((dst.left + padIn).toInt(), (dst.top + padIn).toInt(),
                    (dst.right - padIn).toInt(), (dst.bottom - padIn).toInt())
        d.draw(canvas)
    }

    private fun drawInsetGlyph(canvas: android.graphics.Canvas, dst: android.graphics.RectF, glyph: String, sub: String?) {
        val bg = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply { color = 0xCC101418.toInt() }
        val r = dst.width() * 0.18f
        canvas.drawRoundRect(dst, r, r, bg)
        val tp = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply {
            color = android.graphics.Color.WHITE
            textAlign = android.graphics.Paint.Align.CENTER
        }
        if (sub != null) {
            tp.textSize = dst.height() * 0.40f
            canvas.drawText(glyph, dst.centerX(), dst.top + dst.height() * 0.46f, tp)
            tp.textSize = dst.height() * 0.30f
            tp.isFakeBoldText = true
            canvas.drawText(sub, dst.centerX(), dst.bottom - dst.height() * 0.12f, tp)
        } else {
            tp.textSize = dst.height() * 0.52f
            canvas.drawText(glyph, dst.centerX(), dst.centerY() + dst.height() * 0.20f, tp)
        }
    }

    /** Small exclamation badge in the bottom-right of the inset (reconnect failed). */
    private fun drawBadge(canvas: android.graphics.Canvas, dst: android.graphics.RectF, glyph: String) {
        val tp = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply {
            textAlign = android.graphics.Paint.Align.CENTER
            textSize = dst.height() * 0.34f
        }
        canvas.drawText(glyph, dst.right - dst.width() * 0.16f, dst.bottom - dst.height() * 0.04f, tp)
    }

    private fun updateMetadataSession() {
        refreshArtwork()  // keep the inset glyph/countdown in step with the state
        val b = MediaMetadataCompat.Builder()
            .putString(MediaMetadataCompat.METADATA_KEY_TITLE, nowPlayingTitle())
            .putString(MediaMetadataCompat.METADATA_KEY_ARTIST, nowPlayingArtist())
            .putString(MediaMetadataCompat.METADATA_KEY_ALBUM, npAlbum ?: "VibeSDR")
        npArtwork?.let { b.putBitmap(MediaMetadataCompat.METADATA_KEY_ALBUM_ART, it) }
        mediaSession?.setMetadata(b.build())
    }

    private fun updatePlaybackState(state: Int) {
        // Use a concrete position (0) + current update time, not -1/UNKNOWN: some
        // system media panels treat "PLAYING with unknown position" as not really
        // playing and render a play button (→ taps send ACTION_PLAY, the spring).
        // FM-DX is a SHARED tuner — one skip retunes it for everyone, so the skip
        // transport controls are omitted entirely (iOS parity).
        var actions = PlaybackStateCompat.ACTION_PLAY or
            PlaybackStateCompat.ACTION_PAUSE or
            PlaybackStateCompat.ACTION_PLAY_PAUSE or
            PlaybackStateCompat.ACTION_STOP
        if (!fmdxAudio) {
            actions = actions or
                PlaybackStateCompat.ACTION_SKIP_TO_NEXT or
                PlaybackStateCompat.ACTION_SKIP_TO_PREVIOUS
        }
        mediaSession?.setPlaybackState(
            PlaybackStateCompat.Builder()
                .setState(state, 0L, 1f, SystemClock.elapsedRealtime())
                .setActions(actions)
                .build()
        )
    }

    private fun updateNotification() {
        if (!running) return
        val manager = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        manager.notify(NOTIF_ID, buildNotification())
    }

    private fun setupMediaSession() {
        mediaSession = MediaSessionCompat(this, "VibeSDR").apply {
            setCallback(object : MediaSessionCompat.Callback() {
                // Play/pause = unmute/mute, skips = tune ± step (iOS parity)
                override fun onPlay() { setMutedNative(false) }
                override fun onPause() { setMutedNative(true) }
                override fun onStop() { stopEngine(); stopSelf() }
                override fun onSkipToNext() { tuneByStep(+1) }
                override fun onSkipToPrevious() { tuneByStep(-1) }
                // Car browse-list pick → hand the tune to JS (owns region logic)
                override fun onPlayFromMediaId(mediaId: String?, extras: Bundle?) {
                    playFromBrowseId(mediaId)
                }
            })
        }
        updatePlaybackState(PlaybackStateCompat.STATE_NONE)
    }

    private fun pi(requestCode: Int, action: String) = PendingIntent.getService(
        this, requestCode,
        Intent(this, VibeStreamService::class.java).apply { this.action = action },
        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
    )

    private fun buildNotification(): Notification {
        val launchIntent = packageManager.getLaunchIntentForPackage(packageName)
        val contentPi = PendingIntent.getActivity(
            this, 0, launchIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val playPauseIcon = if (!muted) android.R.drawable.ic_media_pause else android.R.drawable.ic_media_play
        val playPauseLabel = if (!muted) "Mute" else "Unmute"
        val playPauseAction = if (!muted) ACTION_PAUSE else ACTION_PLAY

        val b = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setLargeIcon(npArtwork)
            .setContentTitle(nowPlayingTitle())
            .setContentText(nowPlayingArtist())
            .setContentIntent(contentPi)
        // FM-DX (shared tuner): no prev/next — a skip would retune it for everyone.
        val compact: IntArray
        if (fmdxAudio) {
            b.addAction(playPauseIcon, playPauseLabel, pi(2, playPauseAction))
            b.addAction(android.R.drawable.ic_delete, "Stop", pi(4, ACTION_STOP))
            compact = intArrayOf(0)
        } else {
            b.addAction(android.R.drawable.ic_media_previous, "Prev", pi(1, ACTION_PREV))
            b.addAction(playPauseIcon, playPauseLabel, pi(2, playPauseAction))
            b.addAction(android.R.drawable.ic_media_next, "Next", pi(3, ACTION_NEXT))
            b.addAction(android.R.drawable.ic_delete, "Stop", pi(4, ACTION_STOP))
            compact = intArrayOf(0, 1, 2)
        }
        return b
            .setStyle(
                androidx.media.app.NotificationCompat.MediaStyle()
                    .setMediaSession(mediaSession?.sessionToken)
                    .setShowActionsInCompactView(*compact)
            )
            .setOngoing(true)
            .setSilent(true)
            .build()
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID, "VibeSDR Audio",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "SDR audio stream"
                setSound(null, null)
            }
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(channel)
        }
    }
}
