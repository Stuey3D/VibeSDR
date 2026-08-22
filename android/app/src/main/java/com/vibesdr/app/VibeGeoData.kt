package com.vibesdr.app

import android.content.Context
import android.util.Log
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.File
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.zip.GZIPInputStream

/**
 * Country and network data for the admin page — downloaded by the APP, parsed by the shim.
 *
 * ★★★ THE PARSERS ARE THE DAEMON'S OWN (vibeserver/geoip.cpp, asndb.cpp), compiled into this
 *     build unchanged. Only their DOWNLOAD half is unusable here: both shell out to `curl`, and
 *     asndb pipes through `gunzip`, neither of which exists on Android. So the split is the same
 *     one the EiBi schedule already uses — the app owns the fetch, the shim owns the parse — and
 *     there is exactly one implementation of the hard part.
 *
 * ★★ WHY THE ADMIN PAGE WAS BLANK. The shim asks a HANDLER for a country and a network; it holds
 *    no data itself. On Android nobody ever registered one, so "No country data yet" and a
 *    Network column reading "unknown" were not a missing dataset — they were a missing wire
 *    (Stuart, 2026-08-22: "it should be built the same way as the Linux version which works").
 *
 * ★★★ TENS OF MEGABYTES, ONCE A WEEK, AND NEVER ON A REQUEST. The five registry files plus the
 *     ASN table are large; they are fetched on a background thread, at most weekly, and only when
 *     the parsed cache is missing or stale. Failure is silent and harmless: the columns simply
 *     stay blank, which is what they did before and is honest — never a guessed country.
 */
object VibeGeoData {

    private const val TAG = "VibeGeoData"
    /** ★ The registries publish on their own schedule; weekly is well inside their churn and is
     *  what the daemon uses. */
    private const val MAX_AGE_DAYS = 7

    private val busy = AtomicBoolean(false)

    private val http = OkHttpClient.Builder()
        .connectTimeout(20, TimeUnit.SECONDS)
        .readTimeout(180, TimeUnit.SECONDS)     // ★ these are big files on a phone's uplink
        .build()

    /**
     * Wire the lookups up, and refresh in the background if the data is missing or old.
     *
     * ★ Safe to call on every server start: the wiring is idempotent and the download is skipped
     *   unless it is actually needed.
     */
    fun start(ctx: Context) {
        val dir = File(ctx.filesDir, "geo").apply { mkdirs() }
        try {
            VibeLocalSDR.geoInit(dir.absolutePath)
        } catch (t: Throwable) {
            Log.w(TAG, "could not initialise country lookup: ${t.message}")
            return
        }
        try {
            if (!VibeLocalSDR.geoStale(MAX_AGE_DAYS)) return
        } catch (_: Throwable) { return }

        if (!busy.compareAndSet(false, true)) return
        Thread({
            try { refresh(dir) } catch (t: Throwable) {
                Log.w(TAG, "country data refresh failed: ${t.message}")
            } finally { busy.set(false) }
        }, "vibe-geo-refresh").apply { isDaemon = true; priority = Thread.MIN_PRIORITY }.start()
    }

    /**
     * ★★ LOW PRIORITY ON PURPOSE. This is tens of megabytes of parsing on a phone that is also
     *    serving a radio, and nothing here is urgent — a blank flag for another hour costs
     *    nobody anything, whereas a stalled DSP block is heard by every listener. Today's lesson
     *    exactly: one listener joining cost 40 ms of DSP and everybody heard it.
     */
    private fun refresh(dir: File) {
        val urls = try { VibeLocalSDR.geoSources().lines().filter { it.isNotBlank() } }
                   catch (_: Throwable) { emptyList() }
        if (urls.isEmpty()) return
        Log.i(TAG, "refreshing country/network data (${urls.size} sources)")

        val paths = ArrayList<String>()
        urls.forEachIndexed { i, url ->
            // ★ The FIRST source is the ASN table and arrives gzipped — the shim's parser wants it
            //   decompressed, exactly as `curl | gunzip` would have handed it over.
            val gz = (i == 0)
            val out = File(dir, if (gz) "asn.tsv.dl" else "rir$i.dl")
            if (download(url, out, gz)) paths.add(out.absolutePath)
            else Log.w(TAG, "could not fetch $url")   // ★ one registry down must not lose the rest
        }
        if (paths.isEmpty()) { Log.w(TAG, "no sources fetched"); return }

        val problems = try { VibeLocalSDR.geoIngest(paths.joinToString("\n")) }
                       catch (t: Throwable) { "ingest threw: ${t.message}" }
        // ★ Delete the raw downloads either way: the parsed cache is what is kept, and leaving
        //   tens of megabytes of source text on a phone would be rude.
        paths.forEach { runCatching { File(it).delete() } }
        if (problems.isNullOrBlank()) Log.i(TAG, "country/network data updated")
        else Log.w(TAG, "country/network data: $problems")
    }

    /** ★ Streamed to disk, never held in memory: the ASN table alone is ~43 MB decompressed. */
    private fun download(url: String, out: File, gunzip: Boolean): Boolean = try {
        http.newCall(Request.Builder().url(url)
                .header("User-Agent", "VibeServer country data")
                .build()).execute().use { res ->
            if (!res.isSuccessful) false
            else {
                val body = res.body ?: return@use false
                out.outputStream().use { sink ->
                    val src = if (gunzip) GZIPInputStream(body.byteStream()) else body.byteStream()
                    src.use { it.copyTo(sink, 64 * 1024) }
                }
                out.length() > 0
            }
        }
    } catch (t: Throwable) {
        Log.w(TAG, "download failed for $url: ${t.message}")
        runCatching { out.delete() }
        false
    }
}
