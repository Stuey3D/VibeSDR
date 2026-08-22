package com.vibesdr.app

import android.util.Log
import okhttp3.OkHttpClient
import okhttp3.Request
import java.util.concurrent.TimeUnit

/**
 * The one HTTP GET the native side can reach — because Android has no curl.
 *
 * ★★★ WHY THIS EXISTS. Everything shared with the daemon fetches through popen()+curl: right on a
 *     Pi or a Mac, which have no TLS stack of their own, and impossible here. RadioDNS was the
 *     casualty — it was compiled into the daemon ONLY, so nothing on a phone ever registered a
 *     station-logo handler and /vibeserver/stationlogo answered {} for ever. Logos still appeared,
 *     from the NAME ladder, which is precisely the guessing game RadioDNS exists to replace
 *     (Stuart, 2026-08-22: "RadioDNS hasnt been working on android again").
 *
 * ★★ TRANSPORT ONLY. The lookup's real work — deriving the ECC for the many stations that never
 *    transmit one — stays in radiodns.cpp, unmoved and uncopied. A second implementation of that
 *    is how the browser and the app came to disagree about the same station in the first place.
 *
 * ★ Called from a native thread through JNI and BLOCKS, which is correct here: the lookup already
 *   runs off the DSP thread and its answers are cached for a day.
 */
object VibeHttp {

    private const val TAG = "VibeHttp"

    // ★ Short timeouts: this sits in front of a station logo, and a slow broadcaster host must
    //   never hold a listener's RDS panel. Failure is silent and falls back to the name search.
    private val http = OkHttpClient.Builder()
        .connectTimeout(6, TimeUnit.SECONDS)
        .readTimeout(10, TimeUnit.SECONDS)
        .build()

    /**
     * @param accept a value for the Accept header, or empty. DoH needs application/dns-json.
     * @return the body, or "" on any failure — the native side treats empty as "not found",
     *         which is the same thing curl's `-f` gives the daemon.
     */
    @JvmStatic
    fun get(url: String, accept: String): String = try {
        val b = Request.Builder().url(url).header("User-Agent", "VibeServer")
        if (accept.isNotEmpty()) b.header("Accept", accept)
        http.newCall(b.build()).execute().use { r ->
            if (r.isSuccessful) (r.body?.string() ?: "") else ""
        }
    } catch (t: Throwable) {
        Log.w(TAG, "GET failed: ${t.message}")
        ""
    }
}
