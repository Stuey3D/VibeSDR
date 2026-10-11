package com.vibesdr.app

import android.content.Context
import android.os.Debug
import android.util.Log
import java.util.concurrent.atomic.AtomicInteger

/**
 * ★★★ THE MEMORY DIAGNOSTICS BUILD (Stuart, 2026-10-11, the Moto G35): RC39 on a 3.4 GB phone grew 534 → 635 MB
 * resident in ~35 min (graphics 265 → 347 MB, native heap 128 → 153 MB), kswapd ran nonstop and the WHOLE phone went
 * sluggish — and "we have users on 1 GB phones". This finds out WHAT grows, feature by feature.
 *
 * ★ OFF in every normal build: it runs only when gradle was given -PvibeDiag=true (BuildConfig.VIBE_DIAG), so the code
 *   can stay for the next time without ever running in a store build.
 * ★ Every 10 s, one logcat line, tag VibeDiag (read it with `adb logcat -s VibeDiag ReactNativeJS`):
 *     memory from Debug.MemoryInfo (java / native / graphics / code / total PSS / swap, KB) and how many times the
 *     media session's metadata, its playback state and the notification were pushed to Android in those 10 s —
 *     SystemUI sat at 89 % and system_server at 71 % on the G35, so the UPDATE RATE is a suspect too.
 */
object VibeDiag {
    val metaUpdates = AtomicInteger()
    val stateUpdates = AtomicInteger()
    val notifyUpdates = AtomicInteger()
    @Volatile private var started = false

    fun start(@Suppress("UNUSED_PARAMETER") ctx: Context) {
        if (!BuildConfig.VIBE_DIAG || started) return
        started = true
        Thread({
            while (true) {
                try {
                    val mi = Debug.MemoryInfo()
                    Debug.getMemoryInfo(mi)
                    fun s(k: String) = mi.getMemoryStat(k) ?: "?"
                    Log.i("VibeDiag",
                        "mem KB java=${s("summary.java-heap")} native=${s("summary.native-heap")} " +
                        "graphics=${s("summary.graphics")} code=${s("summary.code")} " +
                        "pss=${s("summary.total-pss")} swap=${s("summary.total-swap")} | " +
                        "10s: meta=${metaUpdates.getAndSet(0)} state=${stateUpdates.getAndSet(0)} " +
                        "notify=${notifyUpdates.getAndSet(0)}")
                } catch (t: Throwable) {
                    Log.w("VibeDiag", "sample failed: $t")
                }
                try { Thread.sleep(10_000) } catch (_: InterruptedException) { return@Thread }
            }
        }, "vibe-diag").apply { isDaemon = true; priority = Thread.MIN_PRIORITY }.start()
    }
}
