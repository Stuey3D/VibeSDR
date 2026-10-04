package com.vibesdr.app

import com.facebook.react.ReactActivity
import com.facebook.react.ReactActivityDelegate
import com.facebook.react.defaults.DefaultReactActivityDelegate

/**
 * VibeServer Lite, stage 2 — the screen is the MAIN app's ServerModeScreen.tsx, verbatim, on React
 * Native 0.73 (see lite/app/index.js). Stage 1's plain-Kotlin proof screen is in the history
 * (b5429f7a); it answered "will it even host" on a 2017 Fire 7 and was then replaced by this.
 */
class MainActivity : ReactActivity() {
    companion object {
        // ★ The shared VibeLocalSdrModule reads and clears this (consumeUsbLaunch) exactly as it does
        //   in the main app: true once when a dongle being plugged in is what opened the app.
        @Volatile @JvmField var usbLaunchPending = false
    }
    override fun onCreate(savedInstanceState: android.os.Bundle?) {
        if (intent?.action == android.hardware.usb.UsbManager.ACTION_USB_DEVICE_ATTACHED) usbLaunchPending = true
        super.onCreate(null)      // null: nothing of ours is saved, and RN screens must not be restored from a bundle
    }
    override fun onNewIntent(intent: android.content.Intent?) {
        super.onNewIntent(intent)
        if (intent?.action == android.hardware.usb.UsbManager.ACTION_USB_DEVICE_ATTACHED) usbLaunchPending = true
    }
    // ★★ The remote drives rows + a highlight (TvNav.kt) on EVERY device the moment a remote, keyboard or controller is
    //    used, and a real touch puts it away (Stuart, 2026-10-04). A device nobody presses a key on never sees it.
    private var tvNav: TvNav? = null
    override fun onPostCreate(savedInstanceState: android.os.Bundle?) {
        super.onPostCreate(savedInstanceState)
        tvNav = TvNav(this)
    }
    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean =
        tvNav?.handle(event) == true || super.dispatchKeyEvent(event)
    /** ★ A FINGER (not a mouse or air-mouse click — those are pointer events too) ends remote mode. */
    override fun dispatchTouchEvent(ev: android.view.MotionEvent): Boolean {
        if (ev.actionMasked == android.view.MotionEvent.ACTION_DOWN &&
            ev.isFromSource(android.view.InputDevice.SOURCE_TOUCHSCREEN)) tvNav?.hide()
        return super.dispatchTouchEvent(ev)
    }
    override fun getMainComponentName(): String = "VibeServerLite"
    override fun createReactActivityDelegate(): ReactActivityDelegate =
        DefaultReactActivityDelegate(this, mainComponentName, false)
}
