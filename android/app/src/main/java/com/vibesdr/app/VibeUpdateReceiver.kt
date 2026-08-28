package com.vibesdr.app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import android.util.Log

/**
 * Put a running server back after the APP ITSELF is updated.
 *
 * ★★★ AN UPDATE IS NOT A CRASH, and only the crash was covered. RtlTcpServerService is
 *     START_STICKY, so Android rebuilds it after a process death and VibeServerRestore re-opens the
 *     dongle — that path is proven. A PACKAGE REPLACE is different in the one way that matters:
 *     Android stops the app and does NOT bring the service back. Nothing was left to notice.
 *
 * ★★★ SO THE RECEIVER GOES DARK AND STAYS DARK. Stuart, 2026-08-28: "the play store shouldnt auto
 *     update a running app and if it does there is no way to restart the app automatically." He is
 *     right on both counts, and it is not hypothetical — installing build 340 on his XCover this
 *     afternoon took his public receiver off the air, and it stayed off until somebody physically
 *     walked over and pressed Start. A phone in a loft or on a mast has no somebody.
 *
 * ★★ IT REUSES THE CRASH PATH RATHER THAN INVENTING A SECOND ONE. The armed flag and the cached
 *    config are already the source of truth for "this machine was serving, and here is how"; this
 *    just supplies the trigger the update case lacks. If the owner stopped the server deliberately
 *    it is disarmed, and an update leaves it stopped — which is what they asked for.
 * ★ MY_PACKAGE_REPLACED is one of the documented exemptions that may start a foreground service
 *   from the background on Android 12+, which is exactly why this is the right hook and a plain
 *   background start would be refused.
 * ★ The USB permission survives, and that is not luck: an update keeps the package's UID, and the
 *   dongle is never detached, so the grant still stands. See VibeServerRestore's own note.
 */
class VibeUpdateReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_MY_PACKAGE_REPLACED) return
        if (!VibeServerRestore.isArmed(ctx)) {
            Log.i(TAG, "updated, but this phone was not serving — nothing to bring back")
            return
        }
        Log.i(TAG, "updated while serving — restarting the server")
        val svc = Intent(ctx, RtlTcpServerService::class.java)
            .putExtra(RtlTcpServerService.EXTRA_RESTORE, true)
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(svc)
            else ctx.startService(svc)
        } catch (t: Throwable) {
            // ★ Never take the update down with us: a refused start is a receiver that stays off,
            //   which is where we already were — a crash here would be strictly worse.
            Log.w(TAG, "could not restart the server after the update: $t")
        }
    }

    private companion object { const val TAG = "VibeUpdateReceiver" }
}
