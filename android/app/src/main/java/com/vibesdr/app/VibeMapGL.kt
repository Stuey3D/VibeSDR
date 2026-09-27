package com.vibesdr.app

import android.content.Context
import android.util.Log
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.File
import java.io.FileOutputStream
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * ★★★ THE GPU MAP ON THE ANDROID VIBESERVER — the files it serves at /mapgl/, and the download
 *     of the optional High Detail pack (briefs/BRIEF-server-gpu-maps.md §2/§3, 2026-09-28).
 *
 * Shared by the main app and VibeServer Lite (Lite compiles this very file — see SHARED in
 * lite/android/app/build.gradle), because both run the same C++ server.
 *
 * TWO JOBS:
 *  1. UNPACK. The APK carries the map as assets under `mapgl/` (android/app/mapgl-assets.gradle).
 *     A C++ server cannot read inside an APK, so the files are copied ONCE to
 *     `filesDir/mapgl-bundled/` and the server is pointed there. `mapgl/manifest.tsv` names every
 *     file and carries a content version; the marker `.unpacked` holding that version is written
 *     LAST, so an upgrade with a new pack re-unpacks and a half-copy (killed mid-way, disk full) is
 *     never trusted. The server is only told about the bundle directory once the copy is COMPLETE.
 *  2. DOWNLOAD. The server's High Detail install needs an HTTPS GET, and Android has no curl.
 *     `download` is called by the native side from its worker thread through JNI (see
 *     jniMapglDownload in vibe_localsdr_jni.cpp) and streams the file to the path it is given,
 *     reporting progress back through nativeProgress. The C++ side owns the .part name, the size
 *     and magic checks and the rename — this is transport only, like VibeHttp.
 *
 * ★ `filesDir/mapgl/` is the writable data dir — the same place the main app's React Native
 *   High Detail download already writes `vibemap-detail.pmtiles`, so one download serves both the
 *   app's own map and the server's listeners.
 */
object VibeMapGL {
    private const val TAG = "VibeMapGL"
    private const val ASSET_DIR = "mapgl"
    private const val MANIFEST = "mapgl/manifest.tsv"
    private const val MARKER = ".unpacked"

    /** One unpack per process; a server restart only re-points the native side. */
    private val unpackStarted = AtomicBoolean(false)
    @Volatile private var bundleReady: String? = null
    @Volatile private var nativeReady = false

    /**
     * Wire the map into the server. Idempotent and cheap after the first call; the ~22 MB copy runs
     * on its own low-priority thread, never on the caller's (a bridge thread or a service start).
     */
    fun start(ctx: Context) {
        val app = ctx.applicationContext
        try {
            System.loadLibrary("vibelocalsdr")   // no-op when VibeLocalSDR already loaded it
            val data = File(app.filesDir, "mapgl").apply { mkdirs() }
            // ★ The downloader and data dir first: an installed detail pack is served, and the
            //   admin page's Download button works, even if the bundled unpack fails.
            if (!nativeReady) { nativeInit(data.absolutePath); nativeReady = true }
        } catch (t: Throwable) {
            Log.e(TAG, "GPU map not wired into the server: ${t.message}", t)
            return
        }
        bundleReady?.let { try { nativeSetBundleDir(it) } catch (t: Throwable) { Log.e(TAG, "setBundleDir: ${t.message}") }; return }
        if (!unpackStarted.compareAndSet(false, true)) return
        Thread({
            try {
                val dir = unpack(app)
                bundleReady = dir.absolutePath
                nativeSetBundleDir(dir.absolutePath)
            } catch (t: Throwable) {
                // ★ Logged, never swallowed: without the bundle the server 404s /mapgl/ and the web
                //   client falls back to its old Leaflet map — degraded, and this line says why.
                Log.e(TAG, "GPU map unpack FAILED — /mapgl/ will 404 until the next start: ${t.message}", t)
                unpackStarted.set(false)   // let the next server start try again
            }
        }, "vibe-mapgl-unpack").apply { isDaemon = true; priority = Thread.MIN_PRIORITY }.start()
    }

    private data class Entry(val asset: String, val dest: String, val bytes: Long)

    private fun unpack(ctx: Context): File {
        val am = ctx.assets
        val lines = am.open(MANIFEST).bufferedReader(Charsets.UTF_8).use { it.readLines() }
        val version = lines.firstOrNull()?.takeIf { it.startsWith("#version ") }?.removePrefix("#version ")?.trim()
            ?: throw IllegalStateException("manifest has no #version line")
        val entries = lines.drop(1).filter { it.isNotBlank() }.map {
            val p = it.split('\t')
            require(p.size == 3) { "bad manifest line: $it" }
            Entry(p[0], p[1], p[2].toLong())
        }
        val root = File(ctx.filesDir, "mapgl-bundled")
        val marker = File(root, MARKER)
        if (marker.isFile && marker.readText().trim() == version) {
            Log.i(TAG, "GPU map bundle current ($version)")
            return root
        }
        val t0 = System.currentTimeMillis()
        // ★ A different (or no) version: start clean. Stale files from an older pack must not be
        //   served beside the new style, and the marker must be gone BEFORE any file changes.
        if (root.exists() && !root.deleteRecursively()) Log.w(TAG, "could not fully clear $root; overwriting")
        root.mkdirs()
        val canon = root.canonicalPath + File.separator
        val buf = ByteArray(64 * 1024)
        for (e in entries) {
            val out = File(root, e.dest)
            // ★ The manifest is ours, but a path that climbs out of the bundle is refused anyway.
            if (!out.canonicalPath.startsWith(canon)) throw SecurityException("manifest path escapes: ${e.dest}")
            out.parentFile?.mkdirs()
            val tmp = File(out.path + ".tmp")
            var n = 0L
            am.open("$ASSET_DIR/${e.asset}").use { input ->
                FileOutputStream(tmp).use { o ->
                    while (true) {
                        val r = input.read(buf); if (r < 0) break
                        o.write(buf, 0, r); n += r
                    }
                    o.fd.sync()
                }
            }
            if (n != e.bytes) { tmp.delete(); throw IllegalStateException("${e.dest}: copied $n of ${e.bytes} bytes") }
            if (!tmp.renameTo(out)) throw IllegalStateException("rename failed: ${out.path}")
        }
        // ★★ LAST. Only a complete copy earns the marker.
        val mtmp = File(root, "$MARKER.tmp")
        mtmp.writeText(version)
        if (!mtmp.renameTo(marker)) throw IllegalStateException("could not write the marker")
        Log.i(TAG, "GPU map unpacked: ${entries.size} files, version $version, ${System.currentTimeMillis() - t0} ms")
        return root
    }

    // ── The downloader (called FROM NATIVE) ────────────────────────────────────────────────────
    /* ★ OkHttp, not HttpURLConnection: it is already in both APKs (VibeHttp), and it follows the
     *   GitHub release redirect to objects.githubusercontent.com — a cross-HOST https redirect —
     *   by default, which HttpURLConnection does not reliably do.
     * ★ No read timeout beyond 60 s of SILENCE: a 169 MB file over a slow link is fine as long as
     *   bytes keep arriving. */
    private val http by lazy {
        OkHttpClient.Builder()
            .connectTimeout(20, TimeUnit.SECONDS)
            .readTimeout(60, TimeUnit.SECONDS)
            .followRedirects(true).followSslRedirects(true)
            .build()
    }

    /**
     * Stream `url` to `dest` (truncating it). Returns true only when the whole body arrived — the
     * native side then checks the size and the PMTiles magic before it renames anything.
     * `handle` is the native progress function for this call; pass it back to nativeProgress.
     */
    @JvmStatic
    fun download(url: String, dest: String, handle: Long): Boolean {
        return try {
            val req = Request.Builder().url(url).header("User-Agent", "VibeServer").build()
            http.newCall(req).execute().use { r ->
                if (!r.isSuccessful) { Log.e(TAG, "download $url: HTTP ${r.code}"); return false }
                val body = r.body ?: run { Log.e(TAG, "download $url: no body"); return false }
                val total = body.contentLength()   // -1 when the server does not say
                val f = File(dest); f.parentFile?.mkdirs()
                var written = 0L
                var lastReport = 0L
                val buf = ByteArray(256 * 1024)
                body.byteStream().use { input ->
                    FileOutputStream(f, false).use { o ->
                        nativeProgress(handle, 0, total)
                        while (true) {
                            val n = input.read(buf); if (n < 0) break
                            o.write(buf, 0, n); written += n
                            val now = System.currentTimeMillis()
                            if (now - lastReport >= 250) { lastReport = now; nativeProgress(handle, written, total) }
                        }
                        o.fd.sync()
                    }
                }
                nativeProgress(handle, written, total)
                if (total >= 0 && written != total) {
                    Log.e(TAG, "download $url: short body, $written of $total"); false
                } else {
                    Log.i(TAG, "downloaded $url → $dest ($written bytes)"); true
                }
            }
        } catch (t: Throwable) {
            Log.e(TAG, "download $url failed: ${t.message}", t)
            false
        }
    }

    // ── Native (vibe_localsdr_jni.cpp) ─────────────────────────────────────────────────────────
    /** Caches this class for the downloader, sets the data dir, installs the downloader. */
    @JvmStatic private external fun nativeInit(dataDir: String)
    @JvmStatic private external fun nativeSetBundleDir(dir: String)
    @JvmStatic private external fun nativeProgress(handle: Long, written: Long, total: Long)
}
