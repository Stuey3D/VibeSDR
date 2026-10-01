package com.vibesdr.app

import android.content.Context
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.zip.GZIPInputStream

/**
 * ★★★ WHAT THIS BOX CAN CARRY (vibe_benchmark.h), on Android. VibeServer Lite runs this at first setup and
 * switches red features off by default; the owner may switch them back on (Stuart, 2026-09-19).
 *
 * ★★ THE RADIO GOES OFF THE AIR FOR THE RUN — the caller stops it. Measuring while capturing measures the two
 *    fighting for the cores, and would break the listener's audio anyway.
 * ★★ TWO THINGS C++ CANNOT DO HERE, so Kotlin does them and hands them over:
 *    - the DAB clip, fetched once and kept (the desktop path shells out to curl and gzip; Android has neither);
 *    - the uplink speed, for the same reason. Without either, that part is simply absent from the result —
 *      never a guessed figure.
 */
object VibeBenchmark {
    private const val TAG = "VibeBenchmark"
    private const val CLIP_URL =
        "https://github.com/Stuey3D/VibeSDR/releases/download/bench-clip-v1/dab-bench.vbu8.gz"
    private const val SPEEDTEST_URL = "https://vibeserver.vibesdr.net/api/speedtest"
    private const val RESULT = "benchmark.json"

    /** The last result, or null if it has never run here. */
    fun last(ctx: Context): JSONObject? = try {
        val f = File(ctx.filesDir, RESULT)
        if (f.exists()) JSONObject(f.readText()) else null
    } catch (t: Throwable) { null }

    /**
     * Measure this device. Blocks for a minute or two — never call it from the main thread.
     * @param wantDab fetch the DAB clip (~18 MB) if it is not already here. False skips the DAB rows.
     */
    fun run(ctx: Context, wantDab: Boolean = true): JSONObject? {
        val clip = if (wantDab) ensureClip(ctx) else ""
        val uplink = measureUplinkKBps()
        val json = try {
            VibeLocalSDR.runBenchmark(clip, uplink)
        } catch (t: Throwable) {
            Log.w(TAG, "benchmark failed: $t"); return null
        }
        if (json.isEmpty()) return null
        return try {
            val j = JSONObject(json)
            // ★ Written whole then moved: a half-written result would read as a broken machine.
            val tmp = File(ctx.filesDir, "$RESULT.tmp")
            tmp.writeText(json)
            tmp.renameTo(File(ctx.filesDir, RESULT))
            j
        } catch (t: Throwable) { Log.w(TAG, "result not stored: $t"); null }
    }

    /**
     * The DAB clip, fetched once and kept. Returns its path, or "" if it could not be had.
     * ★ It lands on a temporary name and is only moved into place once it looks like a clip: a part-download
     *   would grade the DAB rows "none" and read as a broken decoder rather than a bad download.
     */
    fun ensureClip(ctx: Context): String {
        val out = File(ctx.filesDir, "dab-bench.vbu8")
        if (out.exists() && out.length() > 1_000_000L) return out.absolutePath
        val tmp = File(ctx.filesDir, "dab-bench.vbu8.part")
        return try {
            val c = (URL(CLIP_URL).openConnection() as HttpURLConnection).apply {
                connectTimeout = 20_000; readTimeout = 60_000; instanceFollowRedirects = true
                // ★★ VibeTls: the clip is on GitHub, whose downloads chain to ISRG Root X1 — which Android
                //    5.1 does not have ("Trust anchor for certification path not found", B10). Set on the
                //    connection, so the github.com → release-assets redirect keeps it.
                (this as? javax.net.ssl.HttpsURLConnection)?.sslSocketFactory = VibeTls.socketFactory
            }
            if (c.responseCode != 200) { c.disconnect(); return "" }
            GZIPInputStream(c.inputStream).use { gz -> tmp.outputStream().use { gz.copyTo(it) } }
            c.disconnect()
            val ok = tmp.length() > 1_000_000L && tmp.inputStream().use { s ->
                val magic = ByteArray(8); s.read(magic); String(magic) == "VIBEBU81"
            }
            if (ok && tmp.renameTo(out)) out.absolutePath else { tmp.delete(); "" }
        } catch (t: Throwable) {
            Log.w(TAG, "clip not fetched: $t"); tmp.delete(); ""
        }
    }

    /**
     * Upload speed in kB/s, or -1 when it could not be measured — the other half of "how many listeners", for a
     * box with cores to spare and a thin link. ~2 MB posted to the directory, which reads and discards it.
     */
    fun measureUplinkKBps(): Double = try {
        val body = ByteArray(2 * 1024 * 1024) { (it and 0xFF).toByte() }
        val c = (URL(SPEEDTEST_URL).openConnection() as HttpURLConnection).apply {
            requestMethod = "POST"; doOutput = true
            connectTimeout = 15_000; readTimeout = 60_000
            (this as? javax.net.ssl.HttpsURLConnection)?.sslSocketFactory = VibeTls.socketFactory   // ★ see ensureClip
            setFixedLengthStreamingMode(body.size)
            setRequestProperty("Content-Type", "application/octet-stream")
        }
        val t0 = System.nanoTime()
        c.outputStream.use { it.write(body); it.flush() }
        val code = c.responseCode
        c.inputStream.use { it.readBytes() }
        val secs = (System.nanoTime() - t0) / 1e9
        c.disconnect()
        if (code == 200 && secs > 0.05) body.size / 1024.0 / secs else -1.0
    } catch (t: Throwable) {
        Log.w(TAG, "uplink not measured: $t"); -1.0
    }
}
