package com.vibesdr.app

import android.app.AlarmManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log

/**
 * ★★★ THE SERVER COMES BACK, WHATEVER TOOK IT DOWN (beta88, 2026-11-20) — Stuart: "do both for maximum resilience".
 *
 * START_STICKY is the first line: Android re-creates RtlTcpServerService a second or so after its process dies, and the
 * restore puts the server back. The XCover showed that line is not enough on its own: at 15:18:12 Android started a
 * process FOR THE SERVICE (am_proc_start, pid 3141) after the backup's kill — and no server ever came up, nothing logged,
 * nothing listening, until somebody opened the app. Whatever stopped that restore (see RtlTcpServerService's breadcrumbs
 * — they now say how far each start got), nothing came round to try again.
 *
 * This is that second line: an alarm, re-armed every few minutes for as long as the owner has a server ARMED
 * (VibeServerRestore's armed flag — Stop disarms it, and so does a radio gone longer than the blip window). When it
 * fires and the server is not serving, the ordinary restore runs (EXTRA_RESTORE) — no second restart path.
 *
 * ★★★ ANDROID 12+ BACKGROUND FOREGROUND-SERVICE RULES (NickB G4IRX, Pixel 6, 2026-10-16 — see onStartCommand). An
 *     ordinary alarm is NOT an exemption to start a foreground service from the background; an EXACT alarm would be, but
 *     SCHEDULE_EXACT_ALARM is denied by default from Android 14 and USE_EXACT_ALARM is reserved by Play for alarm-clock and
 *     calendar apps — a receiver must not claim to be one. What IS an exemption is the owner having switched battery
 *     optimisation off for the app, which the server screen already asks for before every Start (checkBackgroundAllowed).
 *     So: with the exemption (every server started from the screen since that check), the watchdog restarts the server;
 *     without it, startForegroundService throws ForegroundServiceStartNotAllowedException, which is CAUGHT — never a
 *     crash — and the owner gets the tappable "VibeServer stopped" notification instead of silence.
 * ★ Inexact and allow-while-idle: at most one wake every ~5 minutes (Android stretches it to ~9-15 in Doze). It costs
 *   nothing while serving beyond the wake itself, and it is cancelled the moment the owner presses Stop.
 * ★ The receiver runs in the SERVER's process (manifest), so it never loads React Native just to check.
 */
object VibeServerWatchdog {
    private const val TAG = "VibeServerWatchdog"
    const val ACTION = "com.vibesdr.app.SERVER_WATCHDOG"
    private const val PERIOD_MS = 5 * 60 * 1000L

    private fun pending(ctx: Context, create: Boolean): PendingIntent? {
        val i = Intent(ctx, VibeWatchdogReceiver::class.java).setAction(ACTION).setPackage(ctx.packageName)
        var flags = if (create) PendingIntent.FLAG_UPDATE_CURRENT else PendingIntent.FLAG_NO_CREATE
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) flags = flags or PendingIntent.FLAG_IMMUTABLE
        return PendingIntent.getBroadcast(ctx, 4713, i, flags)
    }

    /** Arm (or re-arm) the next check. Safe to call often: it replaces the one already set. */
    fun schedule(ctx: Context, delayMs: Long = PERIOD_MS) {
        try {
            val am = ctx.getSystemService(Context.ALARM_SERVICE) as AlarmManager
            val pi = pending(ctx, true) ?: return
            val at = SystemClock.elapsedRealtime() + delayMs
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) am.setAndAllowWhileIdle(AlarmManager.ELAPSED_REALTIME_WAKEUP, at, pi)
            else am.set(AlarmManager.ELAPSED_REALTIME_WAKEUP, at, pi)
        } catch (t: Throwable) { Log.w(TAG, "could not set the watchdog: $t") }
    }

    fun cancel(ctx: Context) {
        try {
            val pi = pending(ctx, false) ?: return
            (ctx.getSystemService(Context.ALARM_SERVICE) as AlarmManager).cancel(pi)
            pi.cancel()
        } catch (t: Throwable) { Log.w(TAG, "could not cancel the watchdog: $t") }
    }

    /** Is the app exempt from battery optimisation — the exemption that lets a background start go foreground? */
    fun exempt(ctx: Context): Boolean = Build.VERSION.SDK_INT < Build.VERSION_CODES.M || try {
        (ctx.getSystemService(Context.POWER_SERVICE) as PowerManager).isIgnoringBatteryOptimizations(ctx.packageName)
    } catch (_: Throwable) { false }

    /**
     * The check. Armed and serving: nothing to do but set the next check. Armed and NOT serving, with no restore already
     * under way: run the restore. Not armed: stop checking.
     */
    fun check(ctx: Context, why: String) {
        if (!VibeServerRestore.isArmed(ctx)) { Log.i(TAG, "$why: no server armed — the watchdog stands down"); cancel(ctx); return }
        schedule(ctx)   // ★ first, so a throw below can never leave the server unwatched
        if (VibeServerRestore.isShimServing()) return
        if (RtlTcpServerService.restoreRunning) { Log.i(TAG, "$why: a restore is already under way"); return }
        Log.w(TAG, "$why: the server is armed but not serving — restarting it" +
                   (if (exempt(ctx)) "" else " (battery optimisation is ON: Android may refuse a background start)"))
        VibeServerRestore.breadcrumb(ctx, "watchdog: armed, not serving ($why) — asking for a restore")
        // ★ The service is alive here but its server is not (the XCover's 15:18:12 shape): restore in place — no Intent,
        //   so no background-start rule to meet.
        if (RtlTcpServerService.kickRestore("watchdog: $why")) return
        val svc = Intent(ctx, RtlTcpServerService::class.java).putExtra(RtlTcpServerService.EXTRA_RESTORE, true)
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(svc) else ctx.startService(svc)
        } catch (t: Throwable) {
            // ★★ ForegroundServiceStartNotAllowedException (Android 12+) or IllegalStateException (8-11, background start):
            //    never a crash. Say so where the owner will see it; the next check tries again.
            Log.w(TAG, "Android would not let the watchdog restart the server: $t")
            VibeServerRestore.breadcrumb(ctx, "watchdog: start refused — $t")
            RtlTcpServerService.postStopped(ctx)
        }
    }
}

/** ★ The alarm's receiver — in the server's process (manifest). The work is a few prefs reads; the restore runs in the
 *  service, so goAsync is not needed. */
class VibeWatchdogReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        if (intent.action != VibeServerWatchdog.ACTION) return
        try { VibeServerWatchdog.check(ctx.applicationContext, "watchdog alarm") }
        catch (t: Throwable) { Log.w("VibeServerWatchdog", "check failed: $t") }
    }
}
