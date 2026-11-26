package com.vibesdr.app

import android.content.Context
import android.system.Os
import android.util.Base64
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
 * ★★★ beta96: THE THREE BIG PACKS ARE NOT UNPACKED ANY MORE (basic 18 MB + relief + runways ≈ 30.5 MB). They are
 *     stored uncompressed in the APK (mapgl-assets.gradle noCompress), so each is one run of bytes inside it:
 *     `apkSlices` finds them (AssetManager.openFd → the APK file, the entry's offset and length, the PMTiles magic
 *     checked there), the server is told through nativeSetBundleSlice (vibe_mapgl.h serves /mapgl/<pack> from that
 *     run — same URL, Range/206, ?r=), and the app's own map reads them through `readPackRange`. Fonts, scripts,
 *     icons and the style are still unpacked (small). A pack that turns out compressed (openFd refuses) is
 *     unpacked as before, logged. The marker records WHICH packs are in the APK, so an install unpacked by
 *     beta95/96 (marker = version alone) is cleared and redone once, without them — no orphaned 30 MB.
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
     * Wire the map into the server. Idempotent and cheap after the first call; the copy (~3 MB since beta96) runs
     * on its own low-priority thread, never on the caller's (a bridge thread or a service start).
     */
    fun start(ctx: Context) {
        val app = ctx.applicationContext
        try {
            System.loadLibrary("vibelocalsdr")   // no-op when VibeLocalSDR already loaded it
            val data = File(app.filesDir, "mapgl").apply { mkdirs() }
            // ★ The downloader and data dir first: an installed detail pack is served, and the
            //   admin page's Download button works, even if the bundled unpack fails.
            if (!nativeReady) {
                nativeInit(data.absolutePath)
                // ★★ V12 ADS-B aircraft data: the tables in filesDir/adsb (private, the server's own), the owner's
                //    logo/silhouette folders under the APP-SPECIFIC EXTERNAL dir when there is one
                //    (Android/data/<pkg>/files/adsb — reachable over USB), else beside the tables. The admin page's
                //    zip upload fills them either way. Shared by the main app and Lite (this file is SHARED).
                val adsb = File(app.filesDir, "adsb").apply { mkdirs() }
                val ext = try { app.getExternalFilesDir("adsb") } catch (t: Throwable) { null }
                nativeInitAdsb(adsb.absolutePath, (ext ?: adsb).absolutePath, "VibeServer-Android (+https://vibesdr.net)")
                // ★★ V12 SATELLITE DATA (docs/v12/SATELLITE-MODE.md item 9): vibesdr.net's sats.json for this phone's
                //    server — daily, conditional, held on errors, fetched AT ONCE when a listener opens SATELLITE on a
                //    server with none. The copy and its state live in filesDir/sat (a restart does not refetch). Never
                //    CelesTrak. The main app and Lite both come through here (this file is SHARED).
                try { nativeInitSats(app.filesDir.absolutePath, "VibeServer-Android (satellite data; +https://vibesdr.net)") }
                catch (t: Throwable) { Log.e(TAG, "satellite data not wired into the server: ${t.message}") }
                nativeReady = true
            }
        } catch (t: Throwable) {
            Log.e(TAG, "GPU map not wired into the server: ${t.message}", t)
            return
        }
        bundleReady?.let { try { nativeSetBundleDir(it) } catch (t: Throwable) { Log.e(TAG, "setBundleDir: ${t.message}") }; return }
        if (!unpackStarted.compareAndSet(false, true)) return
        Thread({
            try {
                val dir = bundleDir(app)
                registerSlices(app, dir)
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

    /** ★ One unpack at a time: the app's own map and a server start can both ask at once, and unpack()
     *  clears the directory before it copies — two of them interleaved would delete each other's files. */
    private val unpackLock = Any()

    /**
     * ★★★ THE APP'S OWN GPU MAP READS THIS SAME BUNDLE (2026-10-13). The WebView map page (MapOverlay) used to
     *  copy its files out of React Native's asset system (mapglPack.ts) — and on an Android RELEASE build an
     *  embedded asset has no file path at all (expo-asset hands back a bare resource name; the copy threw
     *  "URI is not absolute"), so every Android phone silently fell back to the Leaflet map: five full-screen
     *  canvases, ~250 MB of GPU memory and ~20 fps on the Moto G35's HFDL map. The APK already carries the
     *  identical files here for the server, so the app uses them: one copy on disk, not two.
     *  Blocking (first run copies ~3 MB: fonts, scripts, icons — the packs stay in the APK since beta96) — call off
     *  the UI thread. Returns the bundle directory.
     */
    fun bundleDir(ctx: Context): File = synchronized(unpackLock) { unpack(ctx.applicationContext) }

    /** The three packs served straight out of the APK, never unpacked (see the class comment). */
    private val APK_PACKS = listOf("vibemap-basic.pmtiles", "vibemap-relief.pmtiles", "vibemap-runways.pmtiles")

    @Volatile private var slices: Map<String, VibeApkPack.Slice>? = null

    /**
     * ★★ Which packs this process reads OUT OF THE APK, worked out once per process (the APK cannot change under a
     *  running process; an update restarts it). VibeApkPack.locate does the checks (stored not compressed, the
     *  manifest's length, inside the file, the PMTiles magic); the file behind the descriptor comes from
     *  /proc/self/fd (so a split APK is found too), sourceDir if that fails. Every process (the app's and the
     *  server's) computes the same answer from the same APK, so they agree on what the marker says. The server is
     *  told by `start` (registerSlices), never from here: the app's map must not load the server library.
     */
    fun apkSlices(ctx: Context): Map<String, VibeApkPack.Slice> {
        slices?.let { return it }
        synchronized(this) {
            slices?.let { return it }
            val app = ctx.applicationContext
            val bytes = try { manifest(app).second.associate { it.dest to it.bytes } } catch (t: Throwable) { emptyMap() }
            val out = VibeApkPack.locate(ASSET_DIR, APK_PACKS, bytes, { asset ->
                app.assets.openFd(asset).use { fd ->
                    val path = try { Os.readlink("/proc/self/fd/${fd.parcelFileDescriptor.fd}") }
                               catch (t: Throwable) { app.applicationInfo.sourceDir }
                    VibeApkPack.Slice(path, fd.startOffset, fd.length)
                }
            }) { Log.w(TAG, it) }
            Log.i(TAG, "GPU map packs read from the APK: ${out.keys.joinToString().ifEmpty { "none" }}")
            slices = out
            return out
        }
    }

    /**
     * Tell the server where each APK pack is (vibe_mapgl.h setBundleSlice — it checks bounds and magic again). Its
     * native state lives as long as the process, so once per process, before the bundle dir is set. ★ A refusal
     * should be impossible (Kotlin made the same checks); if it happens anyway that one pack is copied into the
     * bundle dir, as before beta96, so the listeners' map is never short of it.
     */
    private fun registerSlices(ctx: Context, root: File) {
        for ((name, sl) in apkSlices(ctx)) {
            val why = try { nativeSetBundleSlice(name, sl.path, sl.offset, sl.length) } catch (t: Throwable) { t.toString() }
            if (why.isEmpty()) continue
            Log.e(TAG, "$name: the server refused the APK slice ($why) — copying it out as before")
            try {
                val out = File(root, name); val tmp = File(root, "$name.tmp")
                ctx.assets.open("$ASSET_DIR/$name").use { i -> FileOutputStream(tmp).use { o -> i.copyTo(o, 64 * 1024); o.fd.sync() } }
                if (!tmp.renameTo(out)) throw IllegalStateException("rename failed")
            } catch (t: Throwable) { Log.e(TAG, "$name: fallback copy FAILED — /mapgl/$name will 404: ${t.message}") }
        }
    }

    private val reader = VibeApkPack.Reader()

    /**
     * ★★★ THE APP'S OWN MAP READS A PACK BY RANGE (mapglPack.ts readPackRange → VibePowerModule.mapglPackRange): exactly
     *  `length` bytes from `offset` of bundled pack `name` (fewer at its end), base64 — straight from the APK when the
     *  pack is a slice, else from the unpacked copy. Only the three packs; reads capped at 4 MB; null on any failure
     *  (the page leaves that tile out). Synchronous on the JS thread, like the iOS FileHandle read it replaces: a
     *  pread of a few KB from the page cache.
     */
    fun readPackRange(ctx: Context, name: String, offset: Long, length: Int): String? {
        if (name !in APK_PACKS || offset < 0 || length <= 0 || length > 4 * 1024 * 1024) return null
        val app = ctx.applicationContext
        val sl = apkSlices(app)[name]
        val buf = if (sl != null) reader.read(sl, offset, length)
                  else VibeApkPack.readFile(File(File(app.filesDir, "mapgl-bundled"), name), offset, length)
        return buf?.let { Base64.encodeToString(it, Base64.NO_WRAP) }
    }

    private fun manifest(ctx: Context): Pair<String, List<Entry>> {
        val lines = ctx.assets.open(MANIFEST).bufferedReader(Charsets.UTF_8).use { it.readLines() }
        val version = lines.firstOrNull()?.takeIf { it.startsWith("#version ") }?.removePrefix("#version ")?.trim()
            ?: throw IllegalStateException("manifest has no #version line")
        val entries = lines.drop(1).filter { it.isNotBlank() }.map {
            val p = it.split('\t')
            require(p.size == 3) { "bad manifest line: $it" }
            Entry(p[0], p[1], p[2].toLong())
        }
        return version to entries
    }

    private fun unpack(ctx: Context): File {
        val am = ctx.assets
        val (manifestVersion, allEntries) = manifest(ctx)
        // ★★ The packs read from the APK are not copied — and the marker says which they are, so a bundle unpacked
        //    WITH them (beta95/96: marker = the version alone) no longer matches and is cleared and redone once.
        val inApk = apkSlices(ctx).keys
        val entries = allEntries.filter { it.dest !in inApk }
        val version = if (inApk.isEmpty()) manifestVersion else "$manifestVersion apk:${inApk.sorted().joinToString(",")}"
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
            // ★★ VibeTls: modern roots ADDED to an old phone's store — see VibeTls.kt (B10, Android 5.1 had no RIPE/APNIC).
            .sslSocketFactory(VibeTls.socketFactory, VibeTls.trustManager)
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

    /**
     * ★★ V12 ADS-B AIRCRAFT DATA'S TRANSPORT (vibe_adsb_data_update.h owns the policy): a CONDITIONAL GET of
     *  `url` into `dest`. Returns "status\tetag\tlast-modified\terror": 200 = the body is in dest, 304 = unchanged
     *  (dest untouched), 0 = failed. Called from the server's own low-priority updater thread through JNI, once a
     *  week. Same client as `download` (VibeTls roots, redirects followed — codeload.github.com redirects).
     */
    @JvmStatic
    fun fetchIf(url: String, dest: String, ifNoneMatch: String, ifModifiedSince: String, userAgent: String): String {
        return try {
            val b = Request.Builder().url(url).header("User-Agent", userAgent)
            if (ifNoneMatch.isNotEmpty()) b.header("If-None-Match", ifNoneMatch)
            if (ifModifiedSince.isNotEmpty()) b.header("If-Modified-Since", ifModifiedSince)
            http.newCall(b.build()).execute().use { r ->
                val etag = r.header("ETag") ?: ""
                val lm = r.header("Last-Modified") ?: ""
                if (r.code == 304) return "304\t$etag\t$lm\t"
                if (!r.isSuccessful) return "0\t\t\tHTTP ${r.code}"
                val body = r.body ?: return "0\t\t\tno body"
                val f = File(dest); f.parentFile?.mkdirs()
                val buf = ByteArray(64 * 1024)
                body.byteStream().use { input ->
                    FileOutputStream(f, false).use { o ->
                        while (true) { val n = input.read(buf); if (n < 0) break; o.write(buf, 0, n) }
                        o.fd.sync()
                    }
                }
                "200\t$etag\t$lm\t"
            }
        } catch (t: Throwable) {
            Log.e(TAG, "aircraft data fetch $url failed: ${t.message}")
            "0\t\t\t${(t.message ?: "failed").replace('\t', ' ').replace('\n', ' ')}"
        }
    }

    /**
     * ★★ V12 SATELLITE DATA'S TRANSPORT (vibeserver/satcat.cpp owns the policy — the SAME file the Linux server runs:
     *  daily, conditional, the holds, the on-open fetch). One CONDITIONAL GET of vibesdr.net's sats.json into `dest`.
     *  Returns "status\tetag\tlast-modified\tretry-after\terror" with the REAL HTTP status (satcat tells a 503 from a 404:
     *  an overloaded mirror is held for its Retry-After, ≥ 15 min). 0 = no answer (network, timeout). No retry here,
     *  ever — the one request is satcat's to make. Called from satcat's own low-priority thread through JNI.
     */
    @JvmStatic
    fun fetchSats(url: String, dest: String, ifNoneMatch: String, ifModifiedSince: String, userAgent: String): String {
        val clean = { s: String -> s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ') }
        return try {
            val b = Request.Builder().url(url).header("User-Agent", userAgent)
            if (ifNoneMatch.isNotEmpty()) b.header("If-None-Match", ifNoneMatch)
            if (ifModifiedSince.isNotEmpty()) b.header("If-Modified-Since", ifModifiedSince)
            satHttp.newCall(b.build()).execute().use { r ->
                val etag = clean(r.header("ETag") ?: "")
                val lm = clean(r.header("Last-Modified") ?: "")
                val ra = clean(r.header("Retry-After") ?: "")
                if (r.code != 200) return "${r.code}\t$etag\t$lm\t$ra\t"
                val body = r.body ?: return "0\t\t\t\tno body"
                val f = File(dest); f.parentFile?.mkdirs()
                val buf = ByteArray(16 * 1024)
                var total = 0L
                body.byteStream().use { input ->
                    FileOutputStream(f, false).use { o ->
                        while (true) {
                            val n = input.read(buf); if (n < 0) break
                            total += n
                            if (total > 4L * 1024 * 1024) break   // ★ satcat's kMaxBytes: sats.json is ~20 KB
                            o.write(buf, 0, n)
                        }
                        o.fd.sync()
                    }
                }
                "200\t$etag\t$lm\t$ra\t"
            }
        } catch (t: Throwable) {
            Log.w(TAG, "satellite data fetch failed: ${t.message}")
            "0\t\t\t\t${clean(t.message ?: "failed")}"
        }
    }
    /** ★ Its own short-timeout client: no redirects (an address that moves is a fault to report, as curl -L is not used on
     *  Linux), 15 s to connect, 45 s in all — the Linux server's curl limits. */
    private val satHttp by lazy {
        OkHttpClient.Builder()
            .connectTimeout(15, TimeUnit.SECONDS)
            .callTimeout(45, TimeUnit.SECONDS)
            .followRedirects(false).followSslRedirects(false)
            .sslSocketFactory(VibeTls.socketFactory, VibeTls.trustManager)
            .build()
    }

    // ── Native (vibe_localsdr_jni.cpp) ─────────────────────────────────────────────────────────
    /** Caches this class for the downloader, sets the data dir, installs the downloader. */
    @JvmStatic private external fun nativeInit(dataDir: String)
    @JvmStatic private external fun nativeSetBundleDir(dir: String)
    /** beta96: serve bundle file `rel` from `length` bytes at `offset` of `path` (the APK). "" = accepted, else why not. */
    @JvmStatic private external fun nativeSetBundleSlice(rel: String, path: String, offset: Long, length: Long): String
    @JvmStatic private external fun nativeProgress(handle: Long, written: Long, total: Long)
    /** V12 ADS-B aircraft data: its data dir, the owner's image folders, the transport (fetchIf); starts the weekly job. */
    @JvmStatic private external fun nativeInitAdsb(dataDir: String, imagesDir: String, userAgent: String)
    /** V12 satellite data: the data dir (its sat/ folder), the transport (fetchSats); loads the copy, starts the daily
     *  runner and installs the on-open fetch (vibeserver/satcat.cpp). */
    @JvmStatic private external fun nativeInitSats(dataDir: String, userAgent: String)
}
