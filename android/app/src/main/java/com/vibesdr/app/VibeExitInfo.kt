package com.vibesdr.app

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import androidx.annotation.RequiresApi
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.WritableMap
import java.io.BufferedInputStream
import java.io.InputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.zip.GZIPInputStream

/**
 * How this app's processes last ENDED, for the diagnostics report — Android's answer to iOS's
 * VibeCrashLog. Read by VibeStreamModule.getNativeCrash ("VibePowerModule", the name diagnostics.ts
 * already calls, so the JS has no platform branch).
 *
 * ★★★ WHY (2026-10-05). Nick (Pixel, Android 16, Airspy HF+, the GitHub RC APK hosting VibeServer)
 *  "had another crash overnight … when I tried to restart, initially it couldn't find the airspy" —
 *  and nobody could read the crash. The GitHub APK never reaches Play vitals, and Diagnostics' native
 *  section was iOS-only, so a crash in the C++ engine (VibeServer, libairspyhf, the DSP) left NO
 *  trace. The system already keeps one: ActivityManager.getHistoricalProcessExitReasons (API 30+)
 *  records why each of our processes died, and on API 31+ hands back the native tombstone itself.
 *  We record nothing ourselves — we read what Android kept.
 *
 * ★★ ABNORMAL vs ORDINARY. Only a crash-shaped exit is REPORTED (crash, native crash, ANR, low-memory
 *  kill, resource kill, init failure, signal, dependency died). A swipe-away, a force-stop or an update
 *  is listed in the history only — "how the app last ended" is context, not a fault.
 *
 * ★ LOCAL ONLY, like everything in the report: read on demand, shown to the user, sent by them.
 *  Nothing personal is read — see TombstoneReader's privacy note (the last log lines are read REDACTED).
 */
object VibeExitInfo {
    private const val PREFS = "vibe_exit_info"
    private const val KEY_CLEARED = "clearedBeforeMs"
    private const val MAX_TRACE_BYTES = 8 * 1024 * 1024
    private const val ANR_MAX_LINES = 200

    // ★ Numeric, not the SDK constants: several arrived after API 30 (FREEZER 33, PACKAGE_* 34), and the
    //   numbers are what the system stores whatever the device's level.
    private val REASONS = mapOf(
        0 to "UNKNOWN", 1 to "EXIT_SELF", 2 to "SIGNALED", 3 to "LOW_MEMORY", 4 to "CRASH",
        5 to "CRASH_NATIVE", 6 to "ANR", 7 to "INITIALIZATION_FAILURE", 8 to "PERMISSION_CHANGE",
        9 to "EXCESSIVE_RESOURCE_USAGE", 10 to "USER_REQUESTED", 11 to "USER_STOPPED",
        12 to "DEPENDENCY_DIED", 13 to "OTHER", 14 to "FREEZER", 15 to "PACKAGE_STATE_CHANGE",
        16 to "PACKAGE_UPDATED",
    )
    private val ABNORMAL = setOf(2, 3, 4, 5, 6, 7, 9, 12)

    private fun importanceName(i: Int) = when (i) {
        100 -> "foreground"; 125 -> "foreground service"; 150 -> "top sleeping (old)"; 200 -> "visible"
        230 -> "perceptible"; 300 -> "service"; 325 -> "top sleeping"; 350 -> "can't save state"
        400 -> "cached"; 1000 -> "gone"; else -> "importance $i"
    }

    private fun iso(ms: Long): String =
        SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date(ms))

    private fun versionString(ctx: Context): String = try {
        val pi = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
        val code = if (Build.VERSION.SDK_INT >= 28) pi.longVersionCode else @Suppress("DEPRECATION") pi.versionCode.toLong()
        "${pi.versionName} ($code)"
    } catch (_: Throwable) { "?" }

    /**
     * ★ Stamps the running version onto this process (setProcessStateSummary, API 30+), so an exit
     *  record names the build that DIED rather than the one installed now — the difference matters the
     *  morning after an update. Called from MainApplication.onCreate, once per process. ≤ 128 bytes.
     */
    fun stampProcess(ctx: Context) {
        if (Build.VERSION.SDK_INT < 30) return
        try {
            val am = ctx.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val b = versionString(ctx).toByteArray(Charsets.UTF_8)
            am.setProcessStateSummary(if (b.size > 128) b.copyOf(128) else b)
        } catch (_: Throwable) {}
    }

    fun clear(ctx: Context) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putLong(KEY_CLEARED, System.currentTimeMillis()).apply()
    }

    /** null below API 30 or when the system holds no exits for us; else a map diagnostics.ts prints.
     *  `name` is present only when there is an abnormal exit newer than the last clear. */
    fun read(ctx: Context): WritableMap? {
        if (Build.VERSION.SDK_INT < 30) return null
        return read30(ctx)
    }

    @RequiresApi(30)
    private fun read30(ctx: Context): WritableMap? {
        val am = ctx.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val exits = am.getHistoricalProcessExitReasons(ctx.packageName, 0, 10)
        if (exits.isEmpty()) return null
        val clearedBefore = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getLong(KEY_CLEARED, 0L)
        val sorted = exits.sortedByDescending { it.timestamp }

        val map = Arguments.createMap()
        map.putString("model", "${Build.MANUFACTURER} ${Build.MODEL}")
        map.putString("os", "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
        map.putString("installed", versionString(ctx))
        // ★ The history — every exit the system kept, newest first, abnormal or not. "Swiped away at 02:10"
        //   next to "low-memory kill at 03:40" is exactly the context a crash report lacks.
        map.putString("history", sorted.joinToString("\n") { e ->
            val r = REASONS[e.reason] ?: "reason ${e.reason}"
            "${iso(e.timestamp)}  ${r.padEnd(24)} ${e.processName}  ${importanceName(e.importance)}" +
                (if (e.reason == 2 || e.reason == 5) "  sig ${e.status}" else if (e.reason == 1) "  status ${e.status}" else "")
        })

        val e = sorted.firstOrNull { it.reason in ABNORMAL && it.timestamp > clearedBefore } ?: return map
        val reason = REASONS[e.reason] ?: "reason ${e.reason}"
        map.putDouble("ts", e.timestamp.toDouble())
        map.putString("name", reason)
        map.putString("reason", e.description ?: "")
        map.putString("process", "${e.processName} (pid ${e.pid})")
        map.putString("importance", importanceName(e.importance))
        map.putString("status", if (e.reason == 2 || e.reason == 5) "signal ${e.status}" else e.status.toString())
        map.putString("memory", "pss ${e.pss / 1024} MB, rss ${e.rss / 1024} MB")
        val stamped = try { e.processStateSummary?.toString(Charsets.UTF_8) } catch (_: Throwable) { null }
        map.putString("version", stamped ?: guessVersion(ctx, e.timestamp))

        var log: String? = null
        val stack = try {
            when (e.reason) {
                // ★ The tombstone is read ONCE: its backtrace, and its last log lines REDACTED (TombstoneReader.logLines).
                5 -> if (Build.VERSION.SDK_INT >= 31) e.traceInputStream?.let { s ->
                         val b = readCapped(s); log = TombstoneReader.logLines(b); TombstoneReader.describe(b) }
                     else null
                6 -> e.traceInputStream?.let { anrExcerpt(String(readCapped(it), Charsets.UTF_8)) }
                else -> null
            }
        } catch (t: Throwable) { "(trace unreadable: ${t.javaClass.simpleName}: ${t.message})" }
        if (!stack.isNullOrEmpty()) map.putString("stack", stack)
        log?.takeIf { it.isNotEmpty() }?.let { map.putString("log", it) }
        return map
    }

    /** Without a stamp (a build older than the stamp, or a process that died before onCreate): the installed
     *  version if the exit is AFTER its install, otherwise say so rather than guess. */
    private fun guessVersion(ctx: Context, exitMs: Long): String = try {
        val pi = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
        if (exitMs >= pi.lastUpdateTime) "${versionString(ctx)} (installed version — no stamp)"
        else "older than ${versionString(ctx)} (updated ${iso(pi.lastUpdateTime)})"
    } catch (_: Throwable) { "?" }

    /** Reads up to MAX_TRACE_BYTES, un-gzipping if the system handed back a compressed trace. */
    private fun readCapped(raw: InputStream): ByteArray = raw.use { s0 ->
        val s = BufferedInputStream(s0)
        s.mark(2)
        val a = s.read(); val b = s.read()
        s.reset()
        val src: InputStream = if (a == 0x1f && b == 0x8b) GZIPInputStream(s) else s
        val out = java.io.ByteArrayOutputStream()
        val buf = ByteArray(16 * 1024)
        while (out.size() < MAX_TRACE_BYTES) {
            val n = src.read(buf, 0, minOf(buf.size, MAX_TRACE_BYTES - out.size()))
            if (n <= 0) break
            out.write(buf, 0, n)
        }
        out.toByteArray()
    }

    /**
     * ★ An ANR trace is the whole process's Java stacks — hundreds of threads. The main thread is the one
     *  that hung; vibe-* are ours. The preamble (pid, time, cmd line) up to the first thread is kept.
     */
    private fun anrExcerpt(text: String): String {
        val out = ArrayList<String>()
        var inBlock = false; var seenThread = false
        for (line in text.lineSequence()) {
            if (out.size >= ANR_MAX_LINES) break
            val header = line.startsWith("\"")
            if (header) {
                seenThread = true
                val name = line.substring(1).substringBefore('"')
                inBlock = name == "main" || name.startsWith("vibe", ignoreCase = true)
            }
            when {
                !seenThread -> if (line.isNotBlank() && out.size < 12) out += line
                inBlock -> { out += line; if (line.isBlank()) inBlock = false }
            }
        }
        return out.joinToString("\n").trimEnd()
    }
}
