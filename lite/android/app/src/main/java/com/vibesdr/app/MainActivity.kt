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
        noteUsbLaunch(intent)
        super.onCreate(null)      // null: nothing of ours is saved, and RN screens must not be restored from a bundle
    }
    override fun onNewIntent(intent: android.content.Intent?) {
        super.onNewIntent(intent)
        noteUsbLaunch(intent)
    }
    /** ★★★ A RADIO ATTACH THAT OPENED US IS ALSO A RADIO COMING BACK (2026-10-06). Lite only set the flag here, so a
     *  server waiting for its re-enumerated dongle learnt of it from its own poll alone, and a server that had stopped
     *  had no attach resume at all. The Sony launched this activity for the dongle every twelve seconds all night
     *  and nothing listened. Now it is the same entry the main app and the service use — see
     *  VibeServerRestore.onRadioAttached. Never allowed to stop the screen opening. */
    private fun noteUsbLaunch(i: android.content.Intent?) {
        if (i == null || i.action != android.hardware.usb.UsbManager.ACTION_USB_DEVICE_ATTACHED) return
        usbLaunchPending = true
        try {
            @Suppress("DEPRECATION")
            val dev = i.getParcelableExtra<android.hardware.usb.UsbDevice>(android.hardware.usb.UsbManager.EXTRA_DEVICE)
            VibeServerRestore.onRadioAttached(this, dev, "the app was opened for ${dev?.deviceName ?: "a radio"}")
        } catch (t: Throwable) { android.util.Log.w("MainActivity", "attach: $t") }
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
