package com.vibesdr.app

import android.app.ActivityManager
import android.content.ComponentName
import android.content.Context
import android.os.Build

/**
 * ★★★ WHICH PROCESS AM I, AND IS THE OTHER ONE ALIVE? (beta88, 2026-11-20)
 *
 * The hosted VibeServer runs in its OWN process, "<package>:server" (AndroidManifest: RtlTcpServerService,
 * VibeServerHost and the three receivers that bring a server back). The app's UI process loads React Native, Hermes and
 * — through React Native's cookie handler — System WebView, and Android kills every process that has loaded WebView when
 * Play updates WebView (XCover 2026-11-20 14:42:51 `am_kill … stop com.google.android.webview due to installPackageLI`,
 * and 10-28). The server process loads none of them, so a WebView update, a backup's kill of the app process, or the UI
 * simply crashing no longer takes the server down with it.
 *
 * ★ Lite declares no :server process (yet) — then the host service is in the app's own process and everything here
 *   answers "same process", which is how Lite worked before. Nothing assumes the split; it is read from the manifest.
 */
object VibeProcess {
    const val SERVER_SUFFIX = ":server"

    @Volatile private var cachedName: String? = null

    /** This process's name, e.g. "net.vibesdr.app" or "net.vibesdr.app:server". */
    fun name(ctx: Context): String {
        cachedName?.let { return it }
        val n = try {
            if (Build.VERSION.SDK_INT >= 28) android.app.Application.getProcessName()
            else java.io.File("/proc/self/cmdline").readText().trim('\u0000', ' ', '\n')
        } catch (_: Throwable) { "" }
        val out = n.ifEmpty { ctx.applicationInfo.processName ?: ctx.packageName }
        cachedName = out
        return out
    }

    /** Is THIS the server process? (In Lite, without the split, never.) */
    fun isServerProcess(ctx: Context): Boolean = name(ctx).endsWith(SERVER_SUFFIX)

    /** The process the manifest puts the server in — the app's own process name when there is no split. */
    fun serverProcessName(ctx: Context): String = try {
        val flags = 0
        @Suppress("DEPRECATION")
        val si = ctx.packageManager.getServiceInfo(ComponentName(ctx, VibeServerHost::class.java), flags)
        si.processName ?: ctx.applicationInfo.processName
    } catch (_: Throwable) { ctx.applicationInfo.processName }

    /** Does the server live in a process of its own (the main app), or in this one (Lite)? */
    fun serverIsSeparate(ctx: Context): Boolean = serverProcessName(ctx) != ctx.applicationInfo.processName

    /** Is a process of ours with this name running right now? Android lists only our own processes to us. */
    fun processAlive(ctx: Context, procName: String): Boolean = try {
        val am = ctx.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        am.runningAppProcesses?.any { it.processName == procName } == true
    } catch (_: Throwable) { false }

    /** Is the server's process running? Always true when it is this one. */
    fun serverProcessAlive(ctx: Context): Boolean {
        val p = serverProcessName(ctx)
        return p == name(ctx) || processAlive(ctx, p)
    }

    /**
     * ★★★ IS THE APP IN FRONT — ANY OF ITS PROCESSES, not just this one. The microphone type for an Icom's audio and the
     * "record now" decision used getMyMemoryState, which describes only the CALLING process — and the server process
     * never shows an activity, so from there the app would never be "visible" and an Icom's audio would never start.
     * Android grants the while-in-use permissions per app (uid), so the question is per app too.
     */
    fun appVisible(ctx: Context): Boolean = try {
        val am = ctx.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        am.runningAppProcesses?.any {
            it.uid == android.os.Process.myUid() &&
                (it.importance == ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND ||
                 it.importance == ActivityManager.RunningAppProcessInfo.IMPORTANCE_VISIBLE)
        } == true
    } catch (_: Throwable) { false }
}
