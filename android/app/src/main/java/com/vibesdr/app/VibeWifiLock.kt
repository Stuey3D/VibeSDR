package com.vibesdr.app

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build

/**
 * WifiLock holder for the network-SDR paths (rtl_tcp server and client).
 *
 * Without this the WiFi radio enters power-save between beacons, which stalls a
 * sustained multi-Mbit IQ stream long enough to overflow the server's bounded
 * client queue — the user hears audio breakup. WIFI_MODE_FULL_LOW_LATENCY tells
 * the framework to disable power-save outright while the lock is held.
 *
 * Each owner constructs its own instance; the underlying lock is not reference
 * counted, so acquire/release must be paired per instance. Requires WAKE_LOCK,
 * which the manifest already declares.
 */
class VibeWifiLock(private val context: Context, private val tag: String) {

    /* ★★ beta96 (Sloper 11-26: RTL-TCP "stutters a bit … blocks dropped"): hold BOTH locks. LOW_LATENCY only takes effect
     *  while the app is in the foreground with the screen on, and a phone server usually runs screen-off, where it does
     *  nothing; HIGH_PERF still keeps power-save off in the background up to Android 13 (it is non-functional from 14).
     *  Each is tagged and released with the other. */
    private var lowLatency: WifiManager.WifiLock? = null
    private var highPerf: WifiManager.WifiLock? = null

    fun acquire() {
        if (lowLatency != null || highPerf != null) return
        val wm = try {
            context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
        } catch (_: Throwable) { null } ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            lowLatency = take(wm, WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "$tag:lowlatency")
        }
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            @Suppress("DEPRECATION")
            highPerf = take(wm, WifiManager.WIFI_MODE_FULL_HIGH_PERF, "$tag:highperf")
        }
    }

    private fun take(wm: WifiManager, mode: Int, name: String): WifiManager.WifiLock? = try {
        wm.createWifiLock(mode, name).apply { setReferenceCounted(false); acquire() }
    } catch (_: Throwable) { null }

    fun release() {
        for (l in listOf(lowLatency, highPerf)) try { l?.let { if (it.isHeld) it.release() } } catch (_: Throwable) {}
        lowLatency = null
        highPerf = null
    }
}
