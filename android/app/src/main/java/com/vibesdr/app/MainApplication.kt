package com.vibesdr.app

import android.app.Application
import android.content.res.Configuration

import com.facebook.react.PackageList
import com.facebook.react.ReactApplication
import com.facebook.react.ReactNativeApplicationEntryPoint.loadReactNative
import com.facebook.react.ReactPackage
import com.facebook.react.ReactHost
import com.facebook.react.common.ReleaseLevel
import com.facebook.react.defaults.DefaultNewArchitectureEntryPoint

import expo.modules.ApplicationLifecycleDispatcher
import expo.modules.ExpoReactHostFactory

class MainApplication : Application(), ReactApplication {

  override val reactHost: ReactHost by lazy {
    ExpoReactHostFactory.getDefaultReactHost(
      context = applicationContext,
      packageList =
        PackageList(this).packages.apply {
          add(VibeStreamPackage())
        }
    )
  }

  override fun onCreate() {
    VibeDiag.start(this)   // ★ memory diagnostics — runs only in a -PvibeDiag=true build
    // ★ 2026-10-15: a Java crash leaves Android's exit record saying only "crash" (NickB's 1 GB crash).
    //   First, so a crash anywhere in start-up is caught too. See VibeJavaCrash.
    VibeJavaCrash.install(this)
    super.onCreate()
    // ★ 2026-10-05: names the build on this process's exit record, so Diagnostics can say which version
    //   crashed (Nick's overnight VibeServer crash had no version, no log, nothing). See VibeExitInfo.
    VibeExitInfo.stampProcess(this)
    DefaultNewArchitectureEntryPoint.releaseLevel = try {
      ReleaseLevel.valueOf(BuildConfig.REACT_NATIVE_RELEASE_LEVEL.uppercase())
    } catch (e: IllegalArgumentException) {
      ReleaseLevel.STABLE
    }
    // ★★ GitHub #52: per-server certificate pins, loaded and handed to React Native's client builders BEFORE it starts
    //   — a saved default server auto-connects at launch. See VibePinnedTls.
    VibeTlsTrustModule.install(this)
    loadReactNative(this)
    ApplicationLifecycleDispatcher.onApplicationCreate(this)
  }

  override fun onConfigurationChanged(newConfig: Configuration) {
    super.onConfigurationChanged(newConfig)
    ApplicationLifecycleDispatcher.onConfigurationChanged(this, newConfig)
  }
}
