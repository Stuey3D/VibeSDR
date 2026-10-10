package com.vibesdr.app

import android.app.Application
import com.facebook.react.ReactApplication
import com.facebook.react.ReactNativeHost
import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.defaults.DefaultReactNativeHost
import com.facebook.react.modules.network.OkHttpClientProvider
import com.facebook.react.shell.MainReactPackage
import com.facebook.react.uimanager.ViewManager
import com.facebook.soloader.SoLoader
import com.reactnativecommunity.asyncstorage.AsyncStoragePackage
import com.reactnativecommunity.slider.ReactSliderPackage
import com.th3rdwave.safeareacontext.SafeAreaContextPackage

/** The two bridge modules ServerModeScreen talks to — the main app's own classes, synced at build. */
class LitePackage : ReactPackage {
    override fun createNativeModules(ctx: ReactApplicationContext): List<NativeModule> =
        listOf(VibeLocalSdrModule(ctx), VibeMdnsModule(ctx),
               LiteDiagModule(ctx),    // ★ Diagnostics: crash record + device + Lite's own version (2026-10-07)
               LiteUpdateModule(ctx))  // ★ "Check for updates" — Lite only (Play forbids self-update in the main app)
    override fun createViewManagers(ctx: ReactApplicationContext): List<ViewManager<*, *>> =
        listOf(TvTextInputManager())   // ★ replaces RN's text field so a TV remote can reach it (TvTextInput.kt)
}

class MainApplication : Application(), ReactApplication {
    // ★ No autolinking (no React Native Gradle plugin — see settings.gradle): the list is by hand,
    //   and it is short because the screen needs exactly this much.
    override val reactNativeHost: ReactNativeHost = object : DefaultReactNativeHost(this) {
        override fun getPackages(): List<ReactPackage> = listOf(
            MainReactPackage(), AsyncStoragePackage(), ReactSliderPackage(), SafeAreaContextPackage(), LitePackage())
        override fun getJSMainModuleName(): String = "index"
        override fun getBundleAssetName(): String = "index.android.bundle"
        override fun getUseDeveloperSupport(): Boolean = false
        override val isNewArchEnabled: Boolean = false
        override val isHermesEnabled: Boolean = true
    }

    override fun onCreate() {
        super.onCreate()
        // ★ 2026-10-07: names the build on this process's exit record, as the main app does, so Diagnostics
        //   says which Lite crashed — the morning after an update that matters. See VibeExitInfo.
        VibeExitInfo.stampProcess(this)
        /* ★★★ LITE SPREADS THE RECEIVER OVER THE CORES (2026-09-18). A 2017 Fire 7 has four
         *  Cortex-A7s, and WFM stereo pinned ONE of them — vibe-dsp at ~99 % — while two sat idle
         *  and the audio surged. VIBE_DSP_THREADS=1 moves the spectrum FFT and the whole
         *  demodulator (with the Opus encode) onto their own threads; see RxPipeline::setDemodThread.
         *  Set before the engine starts: RxPipeline reads it in start(). The main app does not set it. */
        try { android.system.Os.setenv("VIBE_DSP_THREADS", "1", true) } catch (_: Throwable) {}
        /* ★ And DAB's receiver over two cores: the OFDM front end on vibe-dab, the MSC (the playing
         *  service AND the label scanner's sub-channels) on vibe-dab-msc. Measured on a Pi 2 at 900 MHz,
         *  2026-09-19: one thread could not keep up (22 % of the input dropped); split, 10.42 frames/s,
         *  0 dropped, 0 bad MP2 frames over 128 s. See DabReceiver::setMscThread. */
        try { android.system.Os.setenv("VIBE_DAB_SPLIT", "1", true) } catch (_: Throwable) {}
        /* ★★ THE SCREEN'S OWN HTTPS GETS THE SAME MODERN ROOTS AS THE SERVER'S DOWNLOADS (B10). Lite runs
         *  on Android 5, whose root store predates ISRG Root X1 — the directory, the address lookup and
         *  GitHub all sit on chains a 2015 phone may not know. Added to the system's roots, never instead
         *  of them: see VibeTls.kt. Every fetch() and WebSocket the JS makes comes from this factory. */
        OkHttpClientProvider.setOkHttpClientFactory {
            OkHttpClientProvider.createClientBuilder()
                .sslSocketFactory(VibeTls.socketFactory, VibeTls.trustManager)
                .build()
        }
        SoLoader.init(this, false)
    }
}
