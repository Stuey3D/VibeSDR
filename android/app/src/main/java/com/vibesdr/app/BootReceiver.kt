package com.vibesdr.app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * CarFM (spec §5c): bring the app up with the ignition on a permanent car
 * head-unit install. On boot we launch MainActivity; the app's own launch flow
 * (InstancePicker) then auto-connects a plugged-in RTL-SDR and resumes the last
 * station, so the head unit boots straight into FM with no interaction.
 *
 * Note: background activity-launch from BOOT_COMPLETED is restricted on stock
 * Android 10+. Car head units (e.g. DUDU OS) generally allow it, but the app may
 * still need to be added to the unit's own "auto-start" allowlist. If a future
 * OS blocks the activity launch, switch this to start VibeStreamService as a
 * foreground service instead.
 */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_BOOT_COMPLETED,
            Intent.ACTION_LOCKED_BOOT_COMPLETED,
            "android.intent.action.QUICKBOOT_POWERON" -> {}
            else -> return
        }
        try {
            val launch = context.packageManager
                .getLaunchIntentForPackage(context.packageName) ?: return
            launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            launch.putExtra("carBootAutostart", true)
            context.startActivity(launch)
        } catch (e: Exception) {
            Log.w("BootReceiver", "boot autostart failed: ${e.message}")
        }
    }
}
