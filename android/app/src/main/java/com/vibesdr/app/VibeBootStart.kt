package com.vibesdr.app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.Build
import android.util.Log

/**
 * ★★★ START THE SERVER WHEN POWER RETURNS — ONLY WHERE ANDROID MAY ALLOW IT (Stuart, 2026-10-03).
 *
 * This was removed in B2 (2026-09-28) because it could not work on the Sony TV or the XCover: modern Android
 * keeps USB locked until the first unlock and never grants an app a device that was already plugged in at
 * boot — the restore waited a minute and ended "no USB permission". Then Kiko (PU5WHB) bench-tested his Moto G
 * on Android 5.1 with Screen Lock = None: power cut, dongle left in, power back, press Start — the radio worked
 * with no replug. Android 5.0–7.0 with no lock screen comes up fully unlocked, before the first-unlock USB
 * lockout existed. So the option is back, but only on those versions, and it never pretends:
 *   1. it waits for a working network and a believable clock (the tunnel and NTP need them — Kiko's home fibre
 *      took 2–3 minutes), capped, instead of a blind timer;
 *   2. it restores the server exactly as a restart after a crash does (VibeServerRestore.restore);
 *   3. if Android still withholds the radio, it says so in a notification the owner can tap, and does not sit
 *      there looking like a running server.
 * ★ Only a server that was RUNNING when the power went (armed) comes back; one stopped on purpose stays stopped.
 */
object VibeBootStart {
    private const val TAG = "VibeBootStart"
    private const val PREFS = "vibe_boot_start"
    private const val K_ON = "startOnPower"

    /**
     * ★★ THE VERSION GATE — the newest Android the option is offered on. 24 = Android 7.0.
     *   ✓ Kiko's Moto G, Android 5.1 (API 22), Screen Lock = None: the radio worked after a cold restart.
     *   ✗ The Sony Bravia (Android TV 9+) and the XCover 4S (Android 9+): USB withheld after boot (2026-09-28).
     * Android 7.0 is the newest version from before the first-unlock / Direct Boot USB lockout; widen this ONLY
     * when a newer device is actually confirmed to give the radio back after a restart.
     */
    const val MAX_SDK = 24

    fun supported(): Boolean = Build.VERSION.SDK_INT <= MAX_SDK

    private fun prefs(ctx: Context) = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun isEnabled(ctx: Context): Boolean = prefs(ctx).getBoolean(K_ON, false)
    fun setEnabled(ctx: Context, on: Boolean) { prefs(ctx).edit().putBoolean(K_ON, on).apply() }

    /** Start at boot? The owner's switch, a version that may allow it, and a server that was running. */
    fun wanted(ctx: Context): Boolean = supported() && isEnabled(ctx) && VibeServerRestore.isArmed(ctx)

    /** A clock before this is the unset RTC a phone boots with, not the date (2026-01-01 UTC). */
    private const val SANE_EPOCH_MS = 1_767_225_600_000L
    /** How long to wait for network + clock before trying anyway. Kiko's router took 2–3 minutes. */
    const val NETWORK_WAIT_CAP_MS = 5 * 60 * 1000L

    private fun networkUp(ctx: Context): Boolean = try {
        val cm = ctx.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            val caps = cm.getNetworkCapabilities(cm.activeNetwork)
            caps != null && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)
        } else {
            @Suppress("DEPRECATION") cm.activeNetworkInfo?.isConnected == true
        }
    } catch (t: Throwable) { Log.w(TAG, "network state unreadable: ${t.message}"); false }

    private fun clockSane(): Boolean = System.currentTimeMillis() > SANE_EPOCH_MS

    /**
     * Wait (on a worker thread) until the network is up and the clock is believable, polling with backoff
     * (2 s, doubling to 15 s), up to NETWORK_WAIT_CAP_MS. Returns true if both arrived; false means the cap was
     * reached and the caller tries anyway — a server with no tunnel still serves the LAN.
     */
    fun waitForNetworkAndClock(ctx: Context): Boolean {
        val t0 = android.os.SystemClock.elapsedRealtime()
        var pause = 2_000L
        while (true) {
            val net = networkUp(ctx); val clock = clockSane()
            if (net && clock) {
                // ★ A short settle once both are up: NTP often steps the clock just after the link comes up.
                Thread.sleep(10_000L)
                Log.i(TAG, "network and clock ready after ${(android.os.SystemClock.elapsedRealtime() - t0) / 1000} s")
                return true
            }
            val waited = android.os.SystemClock.elapsedRealtime() - t0
            if (waited >= NETWORK_WAIT_CAP_MS) {
                Log.w(TAG, "still no ${if (!net) "network" else "clock"} after ${waited / 1000} s — starting anyway")
                return false
            }
            Thread.sleep(pause)
            pause = minOf(pause * 2, 15_000L)
        }
    }
}

/** BOOT_COMPLETED → start the server's restore, flagged as a boot start (see RtlTcpServerService.EXTRA_BOOT). */
class VibeBootReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        if (!VibeBootStart.wanted(ctx)) {
            Log.i(TAG, "booted — start when power returns is off, not offered on this Android, or the server was stopped")
            return
        }
        Log.i(TAG, "booted — starting the server once the network and clock are up")
        val svc = Intent(ctx, RtlTcpServerService::class.java)
            .putExtra(RtlTcpServerService.EXTRA_RESTORE, true)
            .putExtra(RtlTcpServerService.EXTRA_BOOT, true)
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(svc) else ctx.startService(svc)
        } catch (t: Throwable) { Log.w(TAG, "could not start the server at boot: $t") }
    }
    private companion object { const val TAG = "VibeBootReceiver" }
}
