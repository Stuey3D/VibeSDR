package com.vibesdr.app

/**
 * ★★ The activity the manifest declares (ICON & ART, 2026-10-05). It is MainActivity, renamed for one reason:
 * the component name `.MainActivity` is now the GREEN launcher <activity-alias>, so every existing home-screen
 * shortcut keeps pointing at a component that still exists and is still enabled after the update. The colour
 * aliases (.IconRed … .IconNeon) and the green one all target THIS class; the deep links and the USB attach
 * filter live on it too, because it is never disabled. Add nothing here — MainActivity.kt is the activity.
 */
class VibeActivity : MainActivity()
