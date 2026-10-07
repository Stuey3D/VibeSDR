package com.vibesdr.app

import android.os.Build
import android.os.SystemClock
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod

/**
 * ★★★ LITE'S HALF OF DIAGNOSTICS (2026-10-07). Lite had NO crash capture at all: VibeExitInfo and
 *  TombstoneReader were never in its SHARED list and nothing answered `VibePowerModule`, so
 *  services/diagnostics.ts printed "none recorded" for a native crash on the one build whose whole job
 *  is hosting the C++ engine. NickB's Pixel 6 hosts a VibeServer and keeps crashing; the report he
 *  sent came from a DIFFERENT phone, because the only export lived in the listening app's About.
 *
 * ★ Registered under the name diagnostics.ts already calls ("VibePowerModule" — the main app's
 *  VibeStreamModule answers to it on Android), so the JS has no Lite branch. It carries ONLY the
 *  diagnostics methods; every other caller of VibePowerModule uses optional calls (`mod?.x?.()`), and
 *  none of them is reachable from Lite's one screen anyway.
 *
 * ★ The server runs IN this process (no android:process in the manifest), so the system's exit record
 *  for the app IS the server's — a native crash in VibeServer, libairspyhf or the DSP lands here.
 */
class LiteDiagModule(private val ctx: ReactApplicationContext) : ReactContextBaseJavaModule(ctx) {
    override fun getName() = "VibePowerModule"

    @ReactMethod
    fun getNativeCrash(promise: Promise) {
        try { promise.resolve(VibeExitInfo.read(ctx)) }
        catch (e: Throwable) { promise.reject("exitinfo", e) }
    }

    @ReactMethod
    fun clearNativeCrash() {
        try { VibeExitInfo.clear(ctx) } catch (_: Throwable) {}
    }

    /** The model and OS (no name, no account, no serial), plus WHICH BUILD this is — Lite bundles the main
     *  app's version.ts, so without these the report would name the main app's version, not Lite's.
     *  Same three fields as the main app's getDeviceInfo (VibeStreamModule / VibePowerModule.swift). */
    @ReactMethod
    fun getDeviceInfo(promise: Promise) {
        val m = Arguments.createMap()
        m.putString("model", "${Build.MANUFACTURER} ${Build.MODEL}")
        m.putString("os", "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
        m.putString("systemName", "Android")
        m.putString("packageId", ctx.packageName)
        try {
            val pi = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
            val code = if (Build.VERSION.SDK_INT >= 28) pi.longVersionCode else @Suppress("DEPRECATION") pi.versionCode.toLong()
            m.putString("versionName", pi.versionName ?: "")
            m.putString("versionCode", code.toString())
        } catch (_: Throwable) {}
        processStartMs()?.let { m.putDouble("processStartMs", it.toDouble()) }
        promise.resolve(m)
    }

    /** When this process (and so the server inside it) last started, as wall-clock ms. API 24+. */
    private fun processStartMs(): Long? = if (Build.VERSION.SDK_INT >= 24) try {
        System.currentTimeMillis() - (SystemClock.elapsedRealtime() - android.os.Process.getStartElapsedRealtime())
    } catch (_: Throwable) { null } else null
}
