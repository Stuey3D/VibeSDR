package com.vibesdr.app

/**
 * VibeSDR V4 — local-SDR shim (Stage 1).
 *
 * Thin Kotlin handle onto the bundled SDR++ Brown native core. Stage 1 only
 * proves the library loads (and transitively pulls in libsdrpp_core.so). The
 * real localhost UberSDR shim — USB enumeration, IQ → FFT/SPEC, Opus audio —
 * lands in later stages behind this same object.
 *
 * Not loaded at app startup yet; call [hello] from a debug action to verify the
 * native core links and loads on-device.
 */
object VibeLocalSDR {
    @Volatile private var loaded = false

    private fun ensureLoaded() {
        if (loaded) return
        synchronized(this) {
            if (!loaded) {
                System.loadLibrary("vibelocalsdr")
                loaded = true
            }
        }
    }

    fun hello(): String {
        ensureLoaded()
        return nativeHello()
    }

    /**
     * Open an RTL-SDR from a USB file descriptor (owned by a Kotlin
     * UsbDeviceConnection) and return a human-readable description. Returns a
     * string starting with "ERROR:" on failure. The fd stays owned by Kotlin.
     */
    fun probeRtl(fd: Int, vid: Int, pid: Int): String {
        ensureLoaded()
        return nativeProbeRtl(fd, vid, pid)
    }

    /**
     * Start the local-SDR spectrum pipeline (RTL-SDR → FFT → localhost UberSDR
     * spectrum WebSocket). Returns the bound TCP port, or -1 on failure. The fd
     * must stay open (caller keeps the UsbDeviceConnection alive) until [stopSpectrum].
     */
    fun startSpectrum(
        fd: Int, vid: Int, pid: Int,
        centerFreq: Double, sampleRate: Double, gainTenthDb: Int,
        fftSize: Int, fftRate: Double, mode: String
    ): Int {
        ensureLoaded()
        return nativeStartSpectrum(fd, vid, pid, centerFreq, sampleRate, gainTenthDb, fftSize, fftRate, mode)
    }

    // RTL-TCP: IQ from an rtl_tcp server over the network (host:port) — no USB.
    fun startTcp(
        host: String, port: Int,
        centerFreq: Double, sampleRate: Double, gainTenthDb: Int,
        fftSize: Int, fftRate: Double, mode: String
    ): Int {
        ensureLoaded()
        return nativeStartTcp(host, port, centerFreq, sampleRate, gainTenthDb, fftSize, fftRate, mode)
    }

    fun stopSpectrum() {
        if (!loaded) return
        nativeStopSpectrum()
    }

    /** Stop and WAIT for the radio to be closed. The caller owns the USB connection and must
     *  not close it until this returns — see the note on nativeStopSpectrumSync. */
    fun stopSpectrumSync() {
        if (!loaded) return
        nativeStopSpectrumSync()
    }
    private external fun nativeStopSpectrumSync()

    // Hardware controls (no-ops if no session running). gainTenthDb < 0 = auto.
    fun setGain(gainTenthDb: Int) { if (loaded) nativeSetGain(gainTenthDb) }
    fun setPpm(ppm: Int) { if (loaded) nativeSetPpm(ppm) }
    fun setBiasTee(on: Boolean) { if (loaded) nativeSetBiasTee(on) }
    fun setAgc(on: Boolean) { if (loaded) nativeSetAgc(on) }
    fun setDirectSampling(mode: Int) { if (loaded) nativeSetDirectSampling(mode) }
    fun setSampleRate(rate: Double) { if (loaded) nativeSetSampleRate(rate) }
    fun setDeemphasis(tau: Double) { if (loaded) nativeSetDeemphasis(tau) }
    fun setSquelch(on: Boolean, db: Float) { if (loaded) nativeSetSquelch(on, db) }
    fun setNR(on: Boolean) { if (loaded) nativeSetNR(on) }
    fun setNotch(on: Boolean) { if (loaded) nativeSetNotch(on) }
    fun setStereoEnabled(on: Boolean) { if (loaded) nativeSetStereoEnabled(on) }
    fun setNrStrength(s: Float) { if (loaded) nativeSetNrStrength(s) }
    fun getNrCpu(): Float { return if (loaded) nativeGetNrCpu() else 0f }
    // ensureLoaded() FIRST: on Kiwi (and any network backend) the native lib is
    // never loaded by a local-hardware session, so without this the decoder sidecar
    // returned -1 and feedDecoderPcm no-op'd → decoders/spots produced no output.
    fun startDecoderService(): Int { ensureLoaded(); return if (loaded) nativeStartDecoderService() else -1 }
    fun feedDecoderPcm(b64: String, rate: Int) { if (loaded) nativeFeedDecoderPcm(b64, rate) }
    fun setDecoderFreq(hz: Double) { if (loaded) nativeSetDecoderFreq(hz) }
    fun getTunerGains(): IntArray { return if (loaded) nativeGetTunerGains() ?: IntArray(0) else IntArray(0) }

    // ── RTL-TCP server (share this device's dongle over the network) ──────────
    // overrideRate 0.0 = client-controlled bandwidth; >0 forces that rate.
    fun startServer(
        fd: Int, vid: Int, pid: Int,
        sampleRate: Double, centerFreq: Double, gainTenthDb: Int,
        port: Int, overrideRate: Double
    ): Int {
        ensureLoaded()
        return nativeStartServer(fd, vid, pid, sampleRate, centerFreq, gainTenthDb, port, overrideRate)
    }
    fun stopServer() { if (loaded) nativeStopServer() }
    fun setServerSampleRate(rate: Double) { if (loaded) nativeSetServerSampleRate(rate) }
    fun getServerStatus(): String { return if (loaded) nativeGetServerStatus() else "{\"running\":false}" }
    fun getVibeServerStatus(): String { return if (loaded) nativeGetVibeServerStatus() else "{\"running\":false}" }
    fun getNetStatus(): String { return if (loaded) nativeGetNetStatus() else "{\"tcp\":false}" }

    fun startSpyServer(host: String, port: Int, centerFreq: Double, sampleRate: Double,
                       gainTenthDb: Int, fftSize: Int, fftRate: Double, mode: String): Int {
        ensureLoaded()
        return nativeStartSpyServer(host, port, centerFreq, sampleRate, gainTenthDb, fftSize, fftRate, mode)
    }
    private external fun nativeStartSpyServer(
        host: String, port: Int, centerFreq: Double, sampleRate: Double,
        gainTenthDb: Int, fftSize: Int, fftRate: Double, mode: String): Int

    /** Decode ONE Opus packet to interleaved int16. Null on any failure — the caller must
     *  DROP the frame rather than fall back to playing the bytes raw, which is the bug this
     *  exists to fix. The decoder is stateful and lives in native code across calls. */
    fun opusDecode(packet: ByteArray, rate: Int, channels: Int): ShortArray? {
        ensureLoaded(); return nativeOpusDecode(packet, rate, channels)
    }
    private external fun nativeOpusDecode(packet: ByteArray, rate: Int, channels: Int): ShortArray?

    // ─── FULL MODE ────────────────────────────────────────────────────────────────────────
    //
    // ★★★ THE APP IS THE RADIO; THE CHILD PROCESS IS THE FRONT DOOR. Inverted relative to Linux,
    //     because the radio is opened by Kotlin through UsbManager and a child process cannot
    //     call UsbManager — so on the obvious arrangement the USB descriptor would have to cross
    //     a process boundary, which Android does not allow (all but 0/1/2 are closed).
    //     ▶ The front door owns no radio, so it never needs one. This process keeps the device
    //       and serves it exactly as Simple mode does; the `:frontdoor` service binds the public
    //       port and hands connections back here over a unix socket.
    //
    // ★★ Which side calls which is not interchangeable:
    //      THIS process   listenForHandoff() + setPathPrefix()
    //      :frontdoor     startFrontDoor()   + setHandoffRoute()

    // ── ★★★ ADVANCED MODE ────────────────────────────────────────────────────────────────
    //
    // ★★★ THE FRONT DOOR WAS REMOVED FROM ANDROID, DELIBERATELY. The phone serves ONE radio, and
    //     with one radio Simple mode already listens on exactly one port — so a door bought
    //     nothing and cost a second process Android is free to kill (Stuart, 2026-08-12: "so its
    //     not FULL if compared to those"; hence ADVANCED, which promises no parity it cannot
    //     deliver). Everything below is a property of THIS process. It all existed in the shim
    //     already and served the admin API; none of it was reachable from Kotlin, which is why a
    //     wiring gap looked like a missing feature.

    /** Show the management surface — listeners, blocking, history, countries. ★ It changes what
     *  the admin page DISPLAYS, not what is recorded: a private server still keeps its log and
     *  honours its bans, so turning this on later arrives with history already there. */
    fun setPublicSharing(on: Boolean) { ensureLoaded(); nativeSetPublicSharing(on) }
    private external fun nativeSetPublicSharing(on: Boolean)

    /** ★★ Where the ban list and connection log LIVE. Without these both work and then evaporate
     *  on restart — a ban that holds all evening and is gone by morning. */
    fun setAdminPaths(bansPath: String, logPath: String) { ensureLoaded(); nativeSetAdminPaths(bansPath, logPath) }
    private external fun nativeSetAdminPaths(bansPath: String, logPath: String)

    /** Reverse proxies whose X-Forwarded-For we believe. ★★★ Behind a tunnel with this unset,
     *  EVERY visitor arrives as 127.0.0.1 — which counts as the owner, silently switching off the
     *  session limit and leaving the ban list unable to tell two people apart. */
    fun setTrustedProxies(csv: String) { ensureLoaded(); nativeSetTrustedProxies(csv) }
    private external fun nativeSetTrustedProxies(csv: String)

    /** How many listeners may share this radio at once. 1 = the single-occupant server. */
    fun setMaxUsers(n: Int) { ensureLoaded(); nativeSetMaxUsers(n) }
    private external fun nativeSetMaxUsers(n: Int)

    /** Where listeners may tune. Empty allow = anywhere it can hear; block always wins. */
    fun setTuneLimits(allowCsv: String, blockCsv: String) { ensureLoaded(); nativeSetTuneLimits(allowCsv, blockCsv) }
    private external fun nativeSetTuneLimits(allowCsv: String, blockCsv: String)

    /** Per-band gain ceilings, the gain to return to when everyone leaves (-1 = leave alone), and
     *  an AGC lock. ★ Ceilings are enforced on SET, on RETUNE INTO a capped band, at START and at
     *  the idle park — a cap applied at only one of those is one a listener can walk around. */
    fun setGainLimits(csv: String, restGain: Int, agcLock: Boolean) { ensureLoaded(); nativeSetGainLimits(csv, restGain, agcLock) }
    private external fun nativeSetGainLimits(csv: String, restGain: Int, agcLock: Boolean)

    /** The radio's REAL gain ladder + the RSP's RF position count, for the limiter's slider. */
    fun gainStepsJson(): String { ensureLoaded(); return nativeGainStepsJson() }
    private external fun nativeGainStepsJson(): String

    /** The server's own band list, so the phone's limiter uses the edges the server enforces. */
    fun bandsJson(): String { ensureLoaded(); return nativeBandsJson() }
    private external fun nativeBandsJson(): String

    /** VibeServer: serve the shim's spectrum/audio WS on the LAN, not just loopback.
     *  Call before startSpectrum(). */
    fun setServeOnLan(on: Boolean) { ensureLoaded(); nativeSetServeOnLan(on) }
    private external fun nativeSetServeOnLan(on: Boolean)
    // VibeServer PIN (empty = open access) + compatibility limits + audio codec.
    fun setVibeServerAuth(secret: String) { ensureLoaded(); nativeSetVibeServerAuth(secret) }
    fun setVibeServerLimits(maxBwHz: Double, maxFftRate: Double) { ensureLoaded(); nativeSetVibeServerLimits(maxBwHz, maxFftRate) }
    /** Admin password — gates CONTROL (bias-T, direct sampling, calibration), not access.
     *  Empty = nothing protected. Independent of the listening PIN. */
    fun setVibeServerAdminSecret(secret: String) { ensureLoaded(); nativeSetVibeServerAdminSecret(secret) }
    /** 0 = off, 1 = listener's choice, 2 = compatibility fallback only. Loopback is exempt. */
    fun setVibeServerUncompressedAudio(mode: Int) { ensureLoaded(); nativeSetVibeServerUncompressedAudio(mode) }
    /** Per-listener time limit, minutes. 0 = unlimited. Loopback + admin sessions exempt. */
    fun setVibeServerSessionLimit(minutes: Int) { ensureLoaded(); nativeSetVibeServerSessionLimit(minutes) }
    fun setVibeServerCompressAudio(on: Boolean) { ensureLoaded(); nativeSetVibeServerCompressAudio(on) }
    /** Serve the browser client at GET /. Off = app-only: a browser gets 403. */
    fun setVibeServerWebEnabled(on: Boolean) { ensureLoaded(); nativeSetVibeServerWebEnabled(on) }
    /** Pin the capture rate (Hz). 0 = client-controlled. */
    fun setVibeServerLockedRate(rate: Double) { ensureLoaded(); nativeSetVibeServerLockedRate(rate) }

    // ── The rest of the server's settings, one radio at a time ────────────────────────────────
    // ★★★ ALL OF THESE ALREADY WORKED IN THE ENGINE and were reachable only from the desktop
    //     binary. The phone runs the same server; it simply had no way to ask.

    /** The time limit is a GUARANTEE, not a deadline: kept past its time until somebody waits. */
    fun setVibeServerSessionLimitSoft(soft: Boolean) { ensureLoaded(); nativeSetVibeServerSessionLimitSoft(soft) }
    /** The aerial on this radio, its icon, and the machine's standing landing-screen message. */
    fun setVibeServerLandingInfo(antenna: String, icon: String, message: String,
                                 linkUrl: String, linkLabel: String) {
        ensureLoaded(); nativeSetVibeServerLandingInfo(antenna, icon, message, linkUrl, linkLabel)
    }
    /** Pin the CENTRE (Hz) — the captured window everyone shares. 0 = follows the listener. */
    fun setVibeServerLockedCentre(hz: Double) { ensureLoaded(); nativeSetVibeServerLockedCentre(hz) }
    /** Real bins at deep zoom instead of interpolation. Without it a shared radio goes blocky. */
    fun setVibeServerZoomSpectrum(on: Boolean) { ensureLoaded(); nativeSetVibeServerZoomSpectrum(on) }
    /** Does this radio draw the landing page's 24-hour spectrogram? */
    fun setVibeServerSpectrogram(on: Boolean) { ensureLoaded(); nativeSetVibeServerSpectrogram(on) }
    /** The spectrum slowdown when nobody is looking. CPU and uplink, not the radio. */
    fun setVibeServerForceIdleSaver(on: Boolean) { ensureLoaded(); nativeSetVibeServerForceIdleSaver(on) }
    /** Seconds after the last listener before the capture parks. The device stays CLAIMED. */
    fun setVibeServerIdleGrace(sec: Double) { ensureLoaded(); nativeSetVibeServerIdleGrace(sec) }
    private external fun nativeSetVibeServerWebEnabled(on: Boolean)
    private external fun nativeSetVibeServerLockedRate(rate: Double)
    /** Station list (JSON array) served at GET /stations for the web client's
     *  search. The app supplies it — it already downloads + caches EiBi, and a
     *  browser can't fetch eibispace.de itself (that host sends no CORS headers). */
    fun setStationsJson(json: String) { ensureLoaded(); nativeSetStationsJson(json) }
    /** Receiver coarse location, served at GET /location. */
    fun setLocationJson(json: String) { ensureLoaded(); nativeSetLocationJson(json) }
    /** Learned RDS station bookmarks (served at GET /bookmarks). The shim learns them
     *  from RDS; the app persists them across restarts. */
    fun setBookmarksJson(json: String) { ensureLoaded(); nativeSetBookmarksJson(json) }
    fun getBookmarksJson(): String { return if (loaded) nativeGetBookmarksJson() else "[]" }
    private external fun nativeSetBookmarksJson(json: String)
    private external fun nativeGetBookmarksJson(): String
    /** Empty the server's learned + saved bookmarks. */
    fun clearBookmarks() { if (loaded) nativeClearBookmarks() }
    private external fun nativeClearBookmarks()
    /** File the shim persists bookmarks to (it saves on every change, no JS involved). */
    fun setBookmarksPath(path: String) { ensureLoaded(); nativeSetBookmarksPath(path) }
    private external fun nativeSetBookmarksPath(path: String)
    /** mDNS hostname responder — serve "<host>.local". Renames itself on a clash. */
    fun startMdns(host: String, ipv4: String) { ensureLoaded(); nativeStartMdns(host, ipv4) }
    fun stopMdns() { if (loaded) nativeStopMdns() }
    /** The hostname actually taken (vibesdr-2 if vibesdr was already in use). */
    fun mdnsHostname(): String = if (loaded) nativeMdnsHostname() else ""
    private external fun nativeStartMdns(host: String, ipv4: String)
    private external fun nativeStopMdns()
    private external fun nativeMdnsHostname(): String
    private external fun nativeSetVibeServerAuth(secret: String)
    private external fun nativeSetVibeServerLimits(maxBwHz: Double, maxFftRate: Double)
    private external fun nativeSetVibeServerCompressAudio(on: Boolean)
    private external fun nativeSetVibeServerAdminSecret(secret: String)
    private external fun nativeSetVibeServerUncompressedAudio(mode: Int)
    private external fun nativeSetVibeServerSessionLimit(minutes: Int)
    private external fun nativeSetVibeServerSessionLimitSoft(soft: Boolean)
    private external fun nativeSetVibeServerLandingInfo(antenna: String, icon: String, message: String,
                                                        linkUrl: String, linkLabel: String)
    private external fun nativeSetVibeServerLockedCentre(hz: Double)
    private external fun nativeSetVibeServerZoomSpectrum(on: Boolean)
    private external fun nativeSetVibeServerSpectrogram(on: Boolean)
    private external fun nativeSetVibeServerForceIdleSaver(on: Boolean)
    private external fun nativeSetVibeServerIdleGrace(sec: Double)
    private external fun nativeSetStationsJson(json: String)
    private external fun nativeSetLocationJson(json: String)

    private external fun nativeGetNetStatus(): String
    private external fun nativeHello(): String
    private external fun nativeProbeRtl(fd: Int, vid: Int, pid: Int): String
    private external fun nativeStartSpectrum(
        fd: Int, vid: Int, pid: Int,
        centerFreq: Double, sampleRate: Double, gainTenthDb: Int,
        fftSize: Int, fftRate: Double, mode: String
    ): Int
    private external fun nativeStartTcp(
        host: String, port: Int,
        centerFreq: Double, sampleRate: Double, gainTenthDb: Int,
        fftSize: Int, fftRate: Double, mode: String
    ): Int
    private external fun nativeStopSpectrum()
    private external fun nativeSetGain(gainTenthDb: Int)
    private external fun nativeSetPpm(ppm: Int)
    private external fun nativeSetBiasTee(on: Boolean)
    private external fun nativeSetAgc(on: Boolean)
    private external fun nativeSetDirectSampling(mode: Int)
    private external fun nativeSetSampleRate(rate: Double)
    private external fun nativeSetDeemphasis(tau: Double)
    private external fun nativeSetSquelch(on: Boolean, db: Float)
    private external fun nativeSetNR(on: Boolean)
    private external fun nativeSetNotch(on: Boolean)
    private external fun nativeSetStereoEnabled(on: Boolean)
    private external fun nativeSetNrStrength(s: Float)
    private external fun nativeGetNrCpu(): Float
    private external fun nativeStartDecoderService(): Int
    private external fun nativeFeedDecoderPcm(b64: String, rate: Int)
    private external fun nativeSetDecoderFreq(hz: Double)
    private external fun nativeGetTunerGains(): IntArray?
    private external fun nativeStartServer(
        fd: Int, vid: Int, pid: Int,
        sampleRate: Double, centerFreq: Double, gainTenthDb: Int,
        port: Int, overrideRate: Double
    ): Int
    private external fun nativeStopServer()
    private external fun nativeSetServerSampleRate(rate: Double)
    private external fun nativeGetServerStatus(): String
    private external fun nativeGetVibeServerStatus(): String
}
