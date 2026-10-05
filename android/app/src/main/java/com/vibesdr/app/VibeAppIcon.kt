package com.vibesdr.app

import android.content.ComponentName
import android.content.Context
import android.content.pm.PackageManager
import android.util.Log

/**
 * ICON & ART — the launcher icon in the user's illumination colour (Stuart, 2026-10-04; Android half 2026-10-05).
 *
 * One <activity-alias> per colour in the manifest, each with MAIN/LAUNCHER and its own icon (made by
 * assets/brand/colour_icons.py); exactly one is enabled. GREEN is `.MainActivity` — the shipped icon, the
 * component every existing install already has on its home screen.
 *
 * ★★ ONLY ON A PICK, AND ONLY ONCE WE ARE OFF SCREEN. setAppIcon from JS just records the wish; MainActivity
 *    .onStop applies it. Disabling the alias a task was launched through can finish that task (Android 10+), and
 *    some OEM launchers drop and re-add the home-screen shortcut (it can move, or land back in the app drawer
 *    only), so doing it while the user watches looked like a crash. Never at launch, as on iOS: nothing here
 *    runs unless a pick is pending. DONT_KILL_APP keeps the process — and so the audio service — alive.
 * ★ The pending pick lives in SharedPreferences, so a process death between the pick and the next onStop does
 *   not lose it; it is applied the next time the app leaves the screen.
 */
object VibeAppIcon {
    private const val TAG = "VibeAppIcon"
    private const val PREFS = "vibe_app_icon"
    private const val KEY_PENDING = "pending"

    /** Colour → alias. Green is the original launcher component name (see AndroidManifest.xml). */
    private val ALIASES = linkedMapOf(
        "green" to ".MainActivity", "red" to ".IconRed", "amber" to ".IconAmber", "blue" to ".IconBlue",
        "white" to ".IconWhite", "teal" to ".IconTeal", "neon" to ".IconNeon")

    fun isKnown(colour: String) = colour in ALIASES

    /** Remember the pick; it is applied when the app next leaves the screen. */
    fun request(ctx: Context, colour: String) {
        if (!isKnown(colour)) return
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(KEY_PENDING, colour).apply()
    }

    private fun component(ctx: Context, alias: String) = ComponentName(ctx.packageName, ctx.packageName + alias)

    /** The colour whose alias is on now (green when no colour alias is explicitly enabled — the manifest default). */
    fun current(ctx: Context): String {
        val pm = ctx.packageManager
        for ((c, a) in ALIASES) {
            if (c == "green") continue
            if (pm.getComponentEnabledSetting(component(ctx, a)) == PackageManager.COMPONENT_ENABLED_STATE_ENABLED) return c
        }
        return "green"
    }

    fun applyPending(ctx: Context) {
        val prefs = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val want = prefs.getString(KEY_PENDING, null) ?: return
        prefs.edit().remove(KEY_PENDING).apply()
        if (!isKnown(want)) return
        try {
            if (current(ctx) == want) return
            val pm = ctx.packageManager
            // ★ ENABLE the new one FIRST: for a moment there are two icons rather than none — a launcher that
            //   briefly sees no launcher activity at all may forget the app's place (or its shortcut) entirely.
            pm.setComponentEnabledSetting(component(ctx, ALIASES.getValue(want)),
                PackageManager.COMPONENT_ENABLED_STATE_ENABLED, PackageManager.DONT_KILL_APP)
            for ((c, a) in ALIASES) {
                if (c == want) continue
                pm.setComponentEnabledSetting(component(ctx, a),
                    PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP)
            }
        } catch (t: Throwable) {
            Log.w(TAG, "could not switch the launcher icon to $want: $t")
        }
    }
}
