package com.vibesdr.app

/**
 * ★★ The activity the manifest declares (ICON & ART, 2026-10-05). It is MainActivity, renamed for one reason:
 * the component name `.MainActivity` is now the GREEN launcher <activity-alias>, so every existing home-screen
 * shortcut keeps pointing at a component that still exists and is still enabled after the update. The colour
 * aliases (.IconRed … .IconNeon) and the green one all target THIS class; the deep links and the USB attach
 * filter live on it too, because it is never disabled. Add nothing here — MainActivity.kt is the activity.
 */
class VibeActivity : MainActivity()

/** ★★ THE APP'S OWN ENTRY, NEVER A LAUNCHER ALIAS (2026-10-05, ICON & ART on Android). getLaunchIntentForPackage
 *  names whichever colour alias is enabled NOW; a notification built with it keeps that name, and when the icon
 *  colour switches (on leaving the app — exactly while the media notification is up) the alias is disabled and a
 *  tap does nothing. VibeActivity is the real activity and is never disabled. */
internal fun appEntryIntent(ctx: android.content.Context): android.content.Intent =
    android.content.Intent(ctx, VibeActivity::class.java)
        .setAction(android.content.Intent.ACTION_MAIN)
        .addCategory(android.content.Intent.CATEGORY_LAUNCHER)
        .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK or android.content.Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED)
