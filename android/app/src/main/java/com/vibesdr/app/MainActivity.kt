package com.vibesdr.app

import android.content.Intent
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle

import com.facebook.react.ReactActivity
import com.facebook.react.ReactActivityDelegate
import com.facebook.react.defaults.DefaultNewArchitectureEntryPoint.fabricEnabled
import com.facebook.react.defaults.DefaultReactActivityDelegate

import expo.modules.ReactActivityDelegateWrapper

/** ★ `open` for VibeActivity, the class the manifest actually declares (2026-10-05, ICON & ART): the component
 *  name ".MainActivity" now belongs to the GREEN launcher alias. Every line of the activity still lives here. */
open class MainActivity : ReactActivity() {
  companion object {
    // Set when the app is launched (or resumed) by plugging in a matching RTL-SDR
    // dongle — the USB_DEVICE_ATTACHED intent declared for this activity. JS reads
    // and clears it via VibeLocalSDR.consumeUsbLaunch() on the instance picker, to
    // route straight into Local Hardware instead of the default instance / picker.
    @Volatile @JvmField var usbLaunchPending = false
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    // Set the theme to AppTheme BEFORE onCreate to support
    // coloring the background, status bar, and navigation bar.
    // This is required for expo-splash-screen.
    setTheme(R.style.AppTheme);
    noteUsbLaunch(intent)   // cold start: JS reads the flag when the picker mounts
    super.onCreate(null)
  }

  override fun onNewIntent(intent: Intent?) {
    super.onNewIntent(intent)
    setIntent(intent)
    noteUsbLaunch(intent)   // warm start (singleTask): picker consumes on next focus
  }

  /** ★ ICON & ART (2026-10-05): a picked launcher colour is applied when the app LEAVES the screen — never while it
   *  is on it, never at launch. Disabling the alias the task was started from can close the task. See VibeAppIcon. */
  override fun onStop() {
    super.onStop()
    if (!isChangingConfigurations) VibeAppIcon.applyPending(this)
  }

  private fun noteUsbLaunch(intent: Intent?) {
    if (intent?.action != UsbManager.ACTION_USB_DEVICE_ATTACHED) return
    usbLaunchPending = true
    resumeServerIfWanted(intent ?: return)
  }

  /**
   * ★★★ PUT THE SERVER BACK WHEN THE RADIO COMES BACK — the other half of removing the attach
   * prompt, and the half that actually restarts anything.
   *
   * ★★ VibeServerRestore's own comment says the boot case is hopeless, because "Android's OTG
   *    stack never enumerates a dongle that was attached while the phone was off". The XCover
   *    (Android 11) disproved it on 2026-09-22: its battery died, it cold-booted with the dongle
   *    in, and the attach intent arrived and launched us. So the main app gets a boot path after
   *    all — not through BOOT_COMPLETED (which never works: no USB grant at boot), but through the attach
   *    itself, which is STRONGER EVIDENCE: it does not fire until the dongle is really enumerated,
   *    so there is no waiting and no race with the USB stack. A phone whose OTG stack genuinely
   *    does not enumerate on boot simply never gets here, and nothing is promised to it.
   * ★★ ARMED GOVERNS, NOT THE ATTACH. Only a server the owner left RUNNING comes back: one stopped
   *    on purpose is disarmed and stays stopped, and a phone that has never served gets nothing.
   *    Plugging a dongle into a phone must not silently start broadcasting from it.
   * ★★ THERE IS NO "START ON BOOT" SWITCH ANY MORE (2026-09-28) — see
   *    VibeServerRestore.attachResumeWanted. The attach is the one event Android grants the radio
   *    on, which is why this path survived the switch's removal and BOOT_COMPLETED did not.
   * ★★★ AND ONLY AFTER A BLIP (Stuart, 2026-09-29): a radio back within five minutes of leaving resumes
   *     the server; one away longer — or never seen to leave, which is what a restart looks like — does
   *     not, and the server is disarmed so it stays stopped. See VibeServerRestore.RADIO_BLIP_WINDOW_MS.
   * ★ Safe when the server is already up: the service's restore path refuses to double-open the
   *   radio (isShimServing), and this is the same EXTRA_RESTORE the sticky restart and the update
   *   receiver use — one restore path, not a fourth.
   * ★★★ NOW SHARED WITH LITE, AND IT HANDS A RUNNING SERVER ITS RADIO BACK TOO (2026-10-06) — see
   *     VibeServerRestore.onRadioAttached. This activity's attach is the one Android grants the radio with, so
   *     a running server waiting for its re-enumerated radio is told here as well as by the broadcast.
   */
  private fun resumeServerIfWanted(intent: Intent) {
    @Suppress("DEPRECATION")
    val dev = intent.getParcelableExtra<android.hardware.usb.UsbDevice>(UsbManager.EXTRA_DEVICE)
    try {
      VibeServerRestore.onRadioAttached(this, dev, "the app was opened for ${dev?.deviceName ?: "a radio"}")
    } catch (t: Throwable) {
      android.util.Log.w("MainActivity", "could not resume the server on attach: $t")
    }
  }

  /**
   * Returns the name of the main component registered from JavaScript. This is used to schedule
   * rendering of the component.
   */
  override fun getMainComponentName(): String = "main"

  /**
   * Returns the instance of the [ReactActivityDelegate]. We use [DefaultReactActivityDelegate]
   * which allows you to enable New Architecture with a single boolean flags [fabricEnabled]
   */
  override fun createReactActivityDelegate(): ReactActivityDelegate {
    return ReactActivityDelegateWrapper(
          this,
          BuildConfig.IS_NEW_ARCHITECTURE_ENABLED,
          object : DefaultReactActivityDelegate(
              this,
              mainComponentName,
              fabricEnabled
          ){})
  }

  /**
    * Align the back button behavior with Android S
    * where moving root activities to background instead of finishing activities.
    * @see <a href="https://developer.android.com/reference/android/app/Activity#onBackPressed()">onBackPressed</a>
    */
  override fun invokeDefaultOnBackPressed() {
      if (Build.VERSION.SDK_INT <= Build.VERSION_CODES.R) {
          if (!moveTaskToBack(false)) {
              // For non-root activities, use the default implementation to finish them.
              super.invokeDefaultOnBackPressed()
          }
          return
      }

      // Use the default back button implementation on Android S
      // because it's doing more than [Activity.moveTaskToBack] in fact.
      super.invokeDefaultOnBackPressed()
  }
}
