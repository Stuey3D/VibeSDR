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
        // ★ 2026-10-15: a Java crash leaves Android's exit record saying only "crash" — note it ourselves,
        //   first, as the main app does. See VibeJavaCrash.
        VibeJavaCrash.install(this)
        super.onCreate()
        // ★ 2026-10-07: names the build on this process's exit record, as the main app does, so Diagnostics
        //   says which Lite crashed — the morning after an update that matters. See VibeExitInfo.
        VibeExitInfo.stampProcess(this)
        /* ★ Lite set VIBE_DSP_THREADS=1 and VIBE_DAB_SPLIT=1 here from 2026-09-18 (a Fire 7's four Cortex-A7s:
         *  one pinned at 99 % by WFM stereo, two idle). Since 2026-10-16 the split is the default on every
         *  multi-core server, set in the shared native code — g_vsSplitDefault in local_sdr_shim.cpp. */
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
