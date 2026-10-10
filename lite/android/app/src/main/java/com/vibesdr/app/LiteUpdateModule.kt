package com.vibesdr.app

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.provider.Settings
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.modules.core.DeviceEventManagerModule
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.concurrent.TimeUnit

/**
 * ★★★ LITE UPDATES ITSELF — WITH THE OWNER'S TAP (Stuart, 2026-10-10: "that seems like a great option").
 *
 * Lite is never in a store: every update was a sideloaded APK, found by hand on GitHub. On a TV box or a Fire
 * tablet with no account that is a real barrier, and the Sony only ever updated because adb pushed it.
 *
 * ★★ WHAT IT DOES, AND ONLY WHEN ASKED. Nothing here runs on a timer — a "Check for updates" press reads the
 *    release list once (GitHub's API), and an "Update" press downloads the APK and hands it to Android's own
 *    installer. ★ ANDROID ASKS THE OWNER, every time: Lite targets API 28, so the silent self-update Android 12+
 *    allows (setRequireUserAction) does not apply — one confirmation tap per update, which is what we want.
 * ★★ LITE ONLY. Google Play forbids an app updating itself outside Play, so this module is never in the main
 *    app (it lives under lite/, not in the SHARED list) and REQUEST_INSTALL_PACKAGES is in Lite's manifest only.
 * ★★ PackageInstaller sessions, not an ACTION_VIEW intent: one path from Android 5.0 to today, no FileProvider,
 *    and Android itself refuses an APK that is not this package signed with our keys (the RC4 rotation).
 *    We also check the downloaded file BEFORE offering it: right package, a higher versionCode, the size GitHub
 *    listed — a partial or wrong file never reaches the installer.
 * ★ HTTPS through VibeTls (github.com and its download host are Sectigo / Let's Encrypt chains an old box may
 *   not have), the same client setup as VibeHttp.
 */
class LiteUpdateModule(private val ctx: ReactApplicationContext) : ReactContextBaseJavaModule(ctx) {
    override fun getName() = "LiteUpdate"

    private val http = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(30, TimeUnit.SECONDS)
        .followRedirects(true).followSslRedirects(true)
        .sslSocketFactory(VibeTls.socketFactory, VibeTls.trustManager)
        .build()

    @Volatile private var downloading = false

    private fun myCode(): Long {
        val pi = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
        return if (Build.VERSION.SDK_INT >= 28) pi.longVersionCode else @Suppress("DEPRECATION") pi.versionCode.toLong()
    }

    /** This build: versionName (e.g. 11.0.0~rc37-lite) and versionCode. */
    @ReactMethod
    fun current(promise: Promise) {
        try {
            val m = Arguments.createMap()
            m.putString("versionName", ctx.packageManager.getPackageInfo(ctx.packageName, 0).versionName ?: "")
            m.putDouble("versionCode", myCode().toDouble())
            promise.resolve(m)
        } catch (e: Throwable) { promise.reject("current", e) }
    }

    /**
     * The Lite releases on GitHub, as a JSON array of {tag, apkName, apkUrl, size, published} — every non-draft
     * `lite-v…` release with an .apk attached. Which one is newer is decided in JS (services/liteUpdate.ts),
     * where it is tested. Runs off the JS thread.
     */
    @ReactMethod
    fun check(promise: Promise) {
        Thread {
            try {
                val req = Request.Builder()
                    .url("https://api.github.com/repos/Stuey3D/VibeSDR/releases?per_page=50")
                    .header("Accept", "application/vnd.github+json")
                    .header("User-Agent", "VibeServer-Lite")
                    .build()
                val body = http.newCall(req).execute().use { r ->
                    if (!r.isSuccessful) throw RuntimeException("GitHub answered ${r.code}")
                    r.body?.string() ?: ""
                }
                val out = JSONArray()
                val arr = JSONArray(body)
                for (i in 0 until arr.length()) {
                    val rel = arr.getJSONObject(i)
                    val tag = rel.optString("tag_name")
                    if (!tag.startsWith("lite-v") || rel.optBoolean("draft")) continue
                    val assets = rel.optJSONArray("assets") ?: continue
                    for (k in 0 until assets.length()) {
                        val a = assets.getJSONObject(k)
                        val name = a.optString("name")
                        if (!name.endsWith(".apk")) continue
                        out.put(JSONObject()
                            .put("tag", tag).put("apkName", name)
                            .put("apkUrl", a.optString("browser_download_url"))
                            .put("size", a.optLong("size"))
                            .put("published", rel.optString("published_at")))
                        break
                    }
                }
                promise.resolve(out.toString())
            } catch (e: Throwable) {
                promise.reject("check", e.message ?: e.javaClass.simpleName)
            }
        }.start()
    }

    /** May this app hand an APK to the installer? API 26+: the per-app "Install unknown apps" switch. Below that
     *  the device-wide "Unknown sources" setting, which must already be on — Lite itself was sideloaded. */
    @ReactMethod
    fun canInstall(promise: Promise) {
        try {
            promise.resolve(if (Build.VERSION.SDK_INT >= 26) ctx.packageManager.canRequestPackageInstalls() else true)
        } catch (e: Throwable) { promise.resolve(true) }
    }

    /** Opens the switch canInstall() reports on. The owner comes back to the app; JS checks again then. */
    @ReactMethod
    fun openInstallPermission(promise: Promise) {
        try {
            val i = if (Build.VERSION.SDK_INT >= 26)
                Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + ctx.packageName))
            else Intent(Settings.ACTION_SECURITY_SETTINGS)
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            ctx.startActivity(i)
            promise.resolve(true)
        } catch (e: Throwable) {
            // ★ Some TV builds have no per-app screen: the general settings are the next best door.
            try { ctx.startActivity(Intent(Settings.ACTION_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)); promise.resolve(false) }
            catch (_: Throwable) { promise.reject("perm", e.message ?: "no settings screen") }
        }
    }

    private fun emit(event: String, map: com.facebook.react.bridge.WritableMap) {
        try { ctx.getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter::class.java).emit(event, map) } catch (_: Throwable) {}
    }

    private fun updateFile(): File = File(File(ctx.cacheDir, "update").apply { mkdirs() }, "lite-update.apk")

    /**
     * Download the APK (progress as "LiteUpdateProgress" {got, total}), then CHECK it before anything else sees
     * it: the size GitHub listed, our package name, a higher versionCode. Resolves with the file's path.
     */
    @ReactMethod
    fun download(url: String, size: Double, promise: Promise) {
        if (downloading) { promise.reject("busy", "a download is already running"); return }
        if (!url.startsWith("https://github.com/Stuey3D/VibeSDR/releases/download/")) {
            promise.reject("url", "not a VibeSDR release"); return       // ★ only ever our own releases
        }
        downloading = true
        Thread {
            val f = updateFile()
            try {
                f.delete()
                val total = size.toLong()
                http.newCall(Request.Builder().url(url).header("User-Agent", "VibeServer-Lite").build()).execute().use { r ->
                    if (!r.isSuccessful) throw RuntimeException("the download answered ${r.code}")
                    val src = r.body?.byteStream() ?: throw RuntimeException("empty download")
                    f.outputStream().use { out ->
                        val buf = ByteArray(64 * 1024)
                        var got = 0L; var lastEmit = 0L
                        while (true) {
                            val n = src.read(buf); if (n < 0) break
                            out.write(buf, 0, n); got += n
                            if (got - lastEmit >= 512 * 1024 || got == total) {
                                lastEmit = got
                                emit("LiteUpdateProgress", Arguments.createMap().apply {
                                    putDouble("got", got.toDouble()); putDouble("total", total.toDouble()) })
                            }
                        }
                        out.fd.sync()
                    }
                }
                if (total > 0 && f.length() != total)
                    throw RuntimeException("the download was incomplete (${f.length()} of $total bytes)")
                val pi = ctx.packageManager.getPackageArchiveInfo(f.absolutePath, 0)
                    ?: throw RuntimeException("the file is not an app")
                if (pi.packageName != ctx.packageName) throw RuntimeException("the file is a different app (${pi.packageName})")
                val code = if (Build.VERSION.SDK_INT >= 28) pi.longVersionCode else @Suppress("DEPRECATION") pi.versionCode.toLong()
                if (code <= myCode()) throw RuntimeException("the file is not newer than this build")
                promise.resolve(f.absolutePath)
            } catch (e: Throwable) {
                f.delete()
                promise.reject("download", e.message ?: e.javaClass.simpleName)
            } finally { downloading = false }
        }.start()
    }

    private var receiverRegistered = false
    private val statusAction get() = ctx.packageName + ".LITE_UPDATE_STATUS"

    /** The installer's answers: ask the owner (STATUS_PENDING_USER_ACTION), or report how it ended. */
    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            val status = i.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
            if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                @Suppress("DEPRECATION")
                val confirm = i.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)
                if (confirm != null) { confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK); try { c.startActivity(confirm) } catch (_: Throwable) {} }
                return
            }
            // ★ On success the process is replaced and VibeUpdateReceiver restarts a running server; we rarely
            //   get to say anything. A failure or a "Cancel" comes back here.
            emit("LiteUpdateStatus", Arguments.createMap().apply {
                putBoolean("ok", status == PackageInstaller.STATUS_SUCCESS)
                putString("message", i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "")
                putInt("status", status)
            })
            updateFile().delete()
        }
    }

    /** Hand the checked file to Android's installer. Android shows its own confirmation. */
    @ReactMethod
    fun install(path: String, promise: Promise) {
        try {
            val f = File(path)
            if (f.absolutePath != updateFile().absolutePath || !f.exists()) { promise.reject("install", "no checked download"); return }
            if (!receiverRegistered) {
                @Suppress("UnspecifiedRegisterReceiverFlag")
                if (Build.VERSION.SDK_INT >= 33) ctx.registerReceiver(receiver, IntentFilter(statusAction), Context.RECEIVER_NOT_EXPORTED)
                else ctx.registerReceiver(receiver, IntentFilter(statusAction))
                receiverRegistered = true
            }
            val pi = ctx.packageManager.packageInstaller
            val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
            params.setAppPackageName(ctx.packageName)
            params.setSize(f.length())
            val id = pi.createSession(params)
            pi.openSession(id).use { s ->
                s.openWrite("lite.apk", 0, f.length()).use { out ->
                    f.inputStream().use { it.copyTo(out, 64 * 1024) }
                    s.fsync(out)
                }
                // ★ FLAG_MUTABLE (31+): the installer writes its status into this intent.
                val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
                val pending = PendingIntent.getBroadcast(ctx, id, Intent(statusAction).setPackage(ctx.packageName), flags)
                s.commit(pending.intentSender)
            }
            promise.resolve(true)
        } catch (e: Throwable) { promise.reject("install", e.message ?: e.javaClass.simpleName) }
    }
}
