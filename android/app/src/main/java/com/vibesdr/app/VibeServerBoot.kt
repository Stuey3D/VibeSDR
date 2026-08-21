package com.vibesdr.app

import android.util.Log
import org.json.JSONObject
import java.io.File

/**
 * ★★★ ONE PLACE THAT TURNS A CONFIG INTO A RUNNING SERVER — and the reason it exists.
 *
 * There were TWO paths that started a VibeServer: the normal one in VibeLocalSdrModule, which read
 * ~35 settings out of the JS options map, and the crash-restore one in VibeServerRestore, which
 * replayed a HAND-MAINTAINED SUBSET of twelve and hardcoded the rest — 100 MHz, "nfm", gain -1,
 * 1024 bins. Everything not on that list came back at its default, silently, on a server that
 * looked perfectly healthy.
 *
 * ★★ IT HAD ALREADY BITTEN TWICE and the file said so in its own comments: first the admin password
 *    and the session limit (added to start, forgotten here, so a restart brought the server back
 *    with admin:false and limitMin:0 while the PIN survived — 2026-07-27), then the resting gain
 *    (Stuart, 2026-08-21: *"the safe gain setting i set in the menu is being set to 0db every time
 *    i load in"* — `setGainLimits` was simply never called on this path).
 *
 * ★★★ AND THE FIX IS NOT "REMEMBER TO ADD IT TO BOTH LISTS". That instruction was written down, in
 *     capitals, next to the keys — and was still missed twice, because a list you must remember to
 *     update is a list that will be wrong. The config is now carried WHOLE, as the JSON the app
 *     sent, and applied by this one function. A setting added to the app is restored by
 *     construction; there is no second list to forget.
 *
 * ★★ ARMING BECAME PERMANENT (the "restart if it crashes" switch was removed) which made this path
 *    run far MORE often, so the cost of the old design went up sharply just as its last symptom
 *    appeared. That is why this is worth a refactor rather than one more field.
 *
 * ★ JSONObject, not ReadableMap: the restore path runs from a SERVICE after the app process died,
 *   where there is no React context and no bridge. Plain JSON crosses that boundary and stores
 *   itself in SharedPreferences unchanged.
 */
object VibeServerBoot {
    private const val TAG = "VibeServerBoot"

    // ── Tolerant readers ────────────────────────────────────────────────────────────────────────
    // ★ A ReadableMap turned into JSON gives every number as a Double, so `getInt` on a round
    //   number throws. These read whatever arrived and coerce, and an absent key takes the default
    //   — which must match the app's own defaults exactly, or a restore would quietly change the
    //   server's behaviour rather than reproduce it.
    private fun JSONObject.s(k: String, d: String = ""): String = optString(k, d) ?: d
    private fun JSONObject.n(k: String, d: Double): Double = if (has(k)) optDouble(k, d) else d
    private fun JSONObject.i(k: String, d: Int): Int = if (has(k)) optDouble(k, d.toDouble()).toInt() else d
    private fun JSONObject.b(k: String, d: Boolean): Boolean = if (has(k)) optBoolean(k, d) else d

    /** The name the server advertises. Read here so both callers agree on the default. */
    fun name(cfg: JSONObject): String = cfg.s("name", "VibeSDR").ifEmpty { "VibeSDR" }
    fun pin(cfg: JSONObject): String = cfg.s("pin", "")
    fun advertise(cfg: JSONObject): Boolean = cfg.b("advertise", true)
    fun autoRestore(cfg: JSONObject): Boolean = cfg.b("autoRestore", true)

    /**
     * Apply EVERY setting in `cfg` and open the radio. Returns the listening port, or <= 0.
     *
     * ★ The caller owns the USB connection and must close it if this returns <= 0 — it is the only
     *   one that can, and the two paths dispose of it differently.
     */
    fun applyAndStart(cfg: JSONObject, fd: Int, vendorId: Int, productId: Int, filesDir: File): Int {
        val centerFreq = cfg.n("centerFreq", 100_000_000.0)
        val sampleRate = cfg.n("sampleRate", 2_400_000.0)
        // ★★★ THE RESTING GAIN IS ALSO THE STARTING GAIN, and until now it was neither.
        //     The server screen promises "where the radio sits BEFORE ANYONE CONNECTS, and where it
        //     returns when they leave" — but the shim only applies it at the idle PARK
        //     (applyRestGain: "everybody has left"), so a freshly started server never used it. And
        //     with no gain given at all the shim does not fall back to auto: it starts at the
        //     tuner's MINIMUM and says so in the log ("no setting yet — starting at the tuner's
        //     minimum … (never auto)") — 0.0 dB on an R820T.
        //  ★★ So an owner who set 12.5 dB got 0.0 dB on every start, and the web client's slider was
        //     telling the exact truth about it: "still 0 gain set, although with stations coming in
        //     I suspect it is not 0 and this is a display issue" (Stuart, 2026-08-21). It was not a
        //     display issue — the radio really was at minimum, and a strong local FM station came
        //     through anyway, which is what made it look like one.
        //  ★ An explicit gainTenthDb still wins, for a caller that means a specific starting gain.
        val restGain   = cfg.i("restGain", -1)
        val gain       = if (cfg.has("gainTenthDb")) cfg.i("gainTenthDb", -1) else restGain
        val fftSize    = cfg.i("fftSize", 1024)
        val fftRate    = cfg.n("fftRate", 20.0)
        val mode       = cfg.s("mode", "nfm").ifEmpty { "nfm" }

        // Give the shim a file for its bookmarks BEFORE it starts, so it loads the saved set and
        // then saves every change itself. The JS side cannot be relied on: it is backgrounded while
        // serving, where its timers are suspended.
        VibeLocalSDR.setBookmarksPath(File(filesDir, "vibe_bookmarks.json").absolutePath)
        VibeLocalSDR.setVibeServerAuth(pin(cfg))
        VibeLocalSDR.setVibeServerLimits(cfg.n("maxBandwidthHz", 0.0), cfg.n("maxFftRate", 0.0))
        VibeLocalSDR.setVibeServerCompressAudio(cfg.b("compressAudio", true))
        // ★ Diagnostic: the password itself is NEVER logged — only whether one arrived, and how
        //   long it is. Enough to tell "the setting did not reach the shim" from "the shim ignored
        //   it", which is exactly the question that cost an evening.
        Log.i(TAG, "cfg: adminPw=${cfg.s("adminPassword").length} chars, " +
                   "limitMin=${cfg.i("sessionLimitMin", 0)}, uncomp=${cfg.i("uncompressedAudio", 0)}, " +
                   "restGain=${cfg.i("restGain", -1)}")
        VibeLocalSDR.setVibeServerAdminSecret(cfg.s("adminPassword"))
        VibeLocalSDR.setVibeServerUncompressedAudio(cfg.i("uncompressedAudio", 0))
        VibeLocalSDR.setVibeServerSessionLimit(cfg.i("sessionLimitMin", 0))
        VibeLocalSDR.setVibeServerSessionLimitSoft(cfg.b("sessionLimitSoft", false))
        VibeLocalSDR.setVibeServerIdleKick(cfg.i("idleKickMin", 0))
        VibeLocalSDR.setVibeServerWebEnabled(cfg.b("webServer", true))
        // ★★★ THE CAPTURED WINDOW. Shared listening only works because everybody gets a slice of
        //     ONE window, so the centre must be pinned.
        VibeLocalSDR.setVibeServerLockedCentre(cfg.n("lockedCentre", 0.0))
        VibeLocalSDR.setVibeServerZoomSpectrum(cfg.b("zoomSpectrum", false))
        VibeLocalSDR.setVibeServerSpectrogram(cfg.b("spectrogram", false))
        VibeLocalSDR.setVibeServerForceIdleSaver(cfg.b("forceIdleSaver", false))
        // ★ 0 = never park. 300 s matches the desktop's own default.
        VibeLocalSDR.setVibeServerIdleGrace(cfg.n("idleGraceSec", 300.0))
        VibeLocalSDR.setVibeServerLandingInfo(
            cfg.s("antenna"), cfg.s("antennaIcon"),
            cfg.s("landingMessage"), cfg.s("landingLinkUrl"), cfg.s("landingLinkLabel"))

        // ── ★★★ ADVANCED MODE ───────────────────────────────────────────────────────────────────
        // ★★ Always applied to a DEFINITE value, never "only when advanced". A limit left set from
        //    a previous run would outlive the mode that asked for it, with no way for the owner to
        //    see why.
        run {
            val adv = cfg.b("advanced", false)
            // ★ Bans and the log are recorded in BOTH modes and only DISPLAYED in Advanced — so
            //   switching a receiver public later arrives with its history already there.
            VibeLocalSDR.setAdminPaths(File(filesDir, "vibe_bans.json").absolutePath,
                                       File(filesDir, "vibe_connlog.json").absolutePath)
            VibeLocalSDR.setPublicSharing(adv)
            VibeLocalSDR.setTrustedProxies(if (adv) cfg.s("trustedProxies") else "")
            VibeLocalSDR.setMaxUsers(if (adv) cfg.i("maxUsers", 1).coerceAtLeast(1) else 1)
            VibeLocalSDR.setTuneLimits(
                if (adv) cfg.s("allowRanges") else "",
                if (adv) cfg.s("blockRanges") else "")
            // ★★★ THE RESTING GAIN IS NOT AN ADVANCED SETTING, AND GATING IT ON `adv` THREW IT AWAY.
            //     The server screen offers STARTING GAIN in BOTH modes — deliberately, and its own
            //     comment says so ("One control, one value — it was not duplicated into Advanced")
            //     — while this applied it only in Advanced. So a Simple-mode owner set a safe
            //     resting gain, watched it stick in the UI, and got AUTO on every single start.
            //     That is Stuart's "the safe gain setting i set in the menu is being set to 0db
            //     every time i load in" (2026-08-21), and it was never the restore path at all:
            //     it happened on EVERY start, restore merely being one of them.
            //  ★★ It is a property of the RADIO — where the dongle sits before anyone connects and
            //     what it returns to when they leave — not of sharing or management, which is what
            //     Advanced actually governs. Offering a control in a mode that ignores it is the
            //     no-op control AGENTS.md forbids.
            //  ★ The per-band CEILINGS and the AGC lock stay Advanced: those are limits imposed on
            //    other listeners, which is a sharing decision and meaningless with one user.
            //  ★ One JNI call carries all three; only the ARGUMENTS differ by mode. The native side
            //    keeps them independent (setGainLimits / setRestGain / setAgcLock are separate
            //    there), so an empty limit list with a real resting gain is a perfectly ordinary
            //    combination — which is exactly what Simple mode wants.
            // ★★★ GAIN AUTOMATION, IN BOTH MODES. Protection defaults ON — it can only ever
            //     prevent clipping, so there is nothing to opt into. AGC defaults OFF, because it
            //     may raise the gain ABOVE the owner's figure and that has to be asked for.
            //  ★ Not gated on `adv`: an overloading front end is not a sharing decision, and the
            //    starting gain it works against is offered in Simple mode too.
            // ★ Manual is manual: the first argument is the removed overload-protection flag and
            //   is now ignored by the shim. Passed false so a stored config carrying it cannot
            //   resurrect the behaviour.
            VibeLocalSDR.setGainAutomation(
                false, cfg.b("rtlAgc", false) || cfg.b("agcLock", false))
            VibeLocalSDR.setGainLimits(
                if (adv) cfg.s("gainLimits") else "",
                restGain,
                adv && cfg.b("agcLock", false))
        }
        VibeLocalSDR.setVibeServerLockedRate(cfg.n("lockedRate", 0.0))
        VibeLocalSDR.setServeOnLan(true)

        val port = VibeLocalSDR.startSpectrum(
            fd, vendorId, productId, centerFreq, sampleRate, gain, fftSize, fftRate, mode)

        // ★★★ THE AGC IS APPLIED AGAIN HERE, AFTER THE RADIO IS OPEN, AND THAT IS THE POINT.
        //     setGainAutomation above runs BEFORE startSpectrum, when there is no device — and
        //     turning the AGC on has to read the tuner's gain table to raise the CEILING to its
        //     maximum. With no device it sets the flag and returns, leaving the ceiling unset, and
        //     the loop does nothing at all without one.
        //  ★★ So an owner who switched the AGC on in the server screen got a server that came up
        //     with it inert, and the first listener to toggle it in the client was not enabling a
        //     feature — they were doing the job this line should have done. Stuart, 2026-08-21:
        //     "when the AGC switch is enabled in the GUI I still have to enable it manually in the
        //     menu of the client". The same fault existed on Linux and is fixed the same way.
        //  ★★ AND setRtlAgc HAD TO CHANGE FOR THIS TO WORK AT ALL: it used to return whenever the
        //     flag was unchanged, so calling it a second time did nothing — the flag already
        //     agreed. It now asks whether the CEILING has been applied, not whether the flag moved.
        // ★ The lock implies the AGC — see the same rule on the server (main.cpp) and in the
        //   setup page. Locking a loop that is not running locks nothing.
        if (port > 0) VibeLocalSDR.setGainAutomation(
            false, cfg.b("rtlAgc", false) || cfg.b("agcLock", false))
        return port
    }
}
