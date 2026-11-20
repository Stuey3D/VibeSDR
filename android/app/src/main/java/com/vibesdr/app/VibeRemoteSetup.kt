package com.vibesdr.app

import android.content.Context
import android.hardware.usb.UsbManager
import android.util.Log
import org.json.JSONObject

/**
 * ★★★ REMOTE ACCESS TO SERVER SETTINGS — and the phone's server settings, kept in ONE place (beta88, 2026-11-20).
 *
 * Stuart: "can we expose the android server settings via the /setup page like we do on linux servers … Make it an option
 * called remote access to server settings … When a setting is changed in the remote setup page the setting changes on the
 * device menu too so its mirrored and both views show the current status."
 *
 * ★★ ONE TRUTH, IN THE SERVER'S PROCESS. The settings are the config the server is started and restored from (the keys
 *    VibeServerBoot reads), stored here (filesDir/vibe_server_settings.json). Three readers, one copy:
 *      • the phone's screen — reads it when it opens, writes it as the owner changes things (settings.put), and is TOLD
 *        when the page changed something (the "settings" event through VibeServerLink), so it redraws at once;
 *      • the /setup page — GET/POST /vibeserver/config, translated by vibe_android_setup.h, through the JNI upcalls below;
 *      • the restore — VibeServerRestore's config follows every change (replaceConfig), so a server Android brings back
 *        is the server as it is NOW, not as it was at the last Start.
 *    It lives in the server's process because that is the one that is there when the UI is not (a WebView update, the UI
 *    killed for memory): the page must work on a phone nobody has opened all week.
 *
 * ★★ THE SWITCH IS THE OWNER'S, OFF BY DEFAULT, AND NEEDS THE ADMIN PASSWORD. Off: the handlers are not registered, and
 *    /vibeserver/config answers 501 exactly as it always has on a phone; /setup is not served. On: the shim's own gate —
 *    the admin password's nonce + HMAC challenge, the brute-force lockout, "no password set = refused" — guards it, the
 *    same code the Linux /setup uses (local_sdr_shim.cpp, audit 2026-10-03). The password is the one the owner TYPED on
 *    the phone; nothing here makes one up (feedback: admin passwords typed by the owner, never generated).
 * ★ Device-only settings (battery optimisation, start when power returns, the USB grant, keep radio alive) are shown on
 *   the page read-only, with "change it on the phone": Android's own dialogs are the only way to set the first three.
 */
object VibeRemoteSetup {
    private const val TAG = "VibeRemoteSetup"
    private const val STORE = "vibe_server_settings.json"
    private const val FLAG = "vibe_remote_setup"

    @Volatile private var appCtx: Context? = null
    private fun remember(ctx: Context) { if (appCtx == null) appCtx = ctx.applicationContext }

    // ── The store ────────────────────────────────────────────────────────────────────────────────────────────────

    @Synchronized
    fun settings(ctx: Context): JSONObject {
        remember(ctx)
        val f = java.io.File(ctx.filesDir, STORE)
        if (f.exists()) try { return JSONObject(f.readText()) } catch (t: Throwable) { Log.w(TAG, "settings unreadable: $t") }
        // ★ First run of this build: what the server last ran with is the best truth there is.
        return VibeServerRestore.storedConfig(ctx)
    }

    @Synchronized
    private fun save(ctx: Context, cfg: JSONObject) {
        val dir = ctx.filesDir
        val tmp = java.io.File(dir, "$STORE.tmp")
        tmp.writeText(cfg.toString())
        if (!tmp.renameTo(java.io.File(dir, STORE))) { java.io.File(dir, STORE).writeText(cfg.toString()); tmp.delete() }
        VibeServerRestore.replaceConfig(ctx, cfg)
    }

    /** ★ A Start (the screen's, or a restore) writes the config it started with WHOLE — Start is the owner's full answer. */
    fun onStarted(ctx: Context, cfg: JSONObject) {
        remember(ctx)
        try { save(ctx, cfg) } catch (t: Throwable) { Log.w(TAG, "could not keep the settings: $t") }
        applySwitch(ctx)
    }

    /** settings.get — for the phone's screen. Never the admin password: the screen has its own copy, and this crosses a
     *  process boundary into JavaScript. */
    fun settingsGet(ctx: Context): JSONObject {
        val s = JSONObject(settings(ctx).toString())
        val hasPw = s.optString("adminPassword", "").isNotEmpty()
        s.remove("adminPassword")
        return VibeServerOps.ok().put("settings", s).put("remote", remoteOn(ctx)).put("hasAdminPassword", hasPw)
            .put("hasStore", java.io.File(ctx.filesDir, STORE).exists())
    }

    /**
     * settings.put — the phone's screen sends the config its Start would send (`settings`, whole). It REPLACES the store
     * (the screen is the full editor), except that keys the screen does not send are kept (a newer page's, say).
     * No event back to the screen: it is the source.
     */
    fun settingsPut(ctx: Context, a: JSONObject): JSONObject {
        val incoming = a.optJSONObject("settings") ?: return VibeServerOps.fail("bad_settings", "no settings were sent")
        synchronized(this) {
            val cur = settings(ctx)
            val keys = incoming.keys()
            while (keys.hasNext()) { val k = keys.next(); cur.put(k, incoming.get(k)) }
            if (a.optBoolean("dropLanding", false)) { cur.remove("centerFreq"); cur.remove("mode") }
            save(ctx, cur)
        }
        // ★ No admin password left = the remote door closes (the shim would refuse everyone anyway; say so honestly).
        if (remoteOn(ctx) && settings(ctx).optString("adminPassword", "").isEmpty()) setRemoteAccess(ctx, false)
        return VibeServerOps.ok()
    }

    // ── The switch ───────────────────────────────────────────────────────────────────────────────────────────────

    fun remoteOn(ctx: Context): Boolean = try { java.io.File(ctx.filesDir, FLAG).readText().trim() == "1" } catch (_: Throwable) { false }

    fun setRemoteAccess(ctx: Context, on: Boolean): JSONObject {
        remember(ctx)
        if (on && settings(ctx).optString("adminPassword", "").isEmpty())
            return VibeServerOps.fail("no_admin_password",
                "Set an admin password first — it is what the remote settings page asks for. Type one you will remember.")
        try { java.io.File(ctx.filesDir, FLAG).writeText(if (on) "1" else "0") }
        catch (t: Throwable) { return VibeServerOps.fail("io", "could not save the switch: $t") }
        applySwitch(ctx)
        Log.i(TAG, "remote access to server settings ${if (on) "ON" else "off"}")
        VibeServerOps.emit("remote", JSONObject().put("on", on))
        return VibeServerOps.ok().put("remote", on)
    }

    /** Register (or remove) the /vibeserver/config handlers in the engine — only where the engine is loaded. */
    private fun applySwitch(ctx: Context) {
        if (!VibeLocalSDR.isLoaded()) return
        try { nativeEnable(remoteOn(ctx)) } catch (t: Throwable) { Log.w(TAG, "the engine would not take the switch: $t") }
    }

    // ── What only the phone knows, for the page's read-only card ────────────────────────────────────────────────

    private fun deviceFacts(ctx: Context, s: JSONObject): JSONObject {
        val o = JSONObject()
        o.put("batteryOptimisation", if (VibeServerWatchdog.exempt(ctx)) "off" else "on")
        o.put("startOnPowerSupported", VibeBootStart.supported())
        o.put("startOnPower", VibeBootStart.isEnabled(ctx))
        o.put("keepRadioAlive", VibeServerBoot.keepRadioAlive(ctx, s))
        val mgr = ctx.getSystemService(Context.USB_SERVICE) as? UsbManager
        val want = VibeServerRestore.storedVidPid(ctx)
        val dev = mgr?.deviceList?.values?.firstOrNull { want >= 0 && ((it.vendorId shl 16) or it.productId) == want }
            ?: mgr?.deviceList?.values?.firstOrNull { VibeRadioIds.isServableRadio(it.vendorId, it.productId) }
        o.put("radioPresent", dev != null)
        o.put("usbPermission", dev != null && mgr?.hasPermission(dev) == true)
        o.put("serving", VibeServerRestore.isShimServing())
        return o
    }

    /** Which radio — the page branches its controls on the driver, as the Linux page does. From the USB ids, never by
     *  opening anything. */
    private fun radioFacts(ctx: Context): JSONObject {
        val o = JSONObject()
        val mgr = ctx.getSystemService(Context.USB_SERVICE) as? UsbManager ?: return o
        val want = VibeServerRestore.storedVidPid(ctx)
        val dev = mgr.deviceList.values.firstOrNull { want >= 0 && ((it.vendorId shl 16) or it.productId) == want }
            ?: mgr.deviceList.values.firstOrNull { VibeRadioIds.isServableRadio(it.vendorId, it.productId) }
            ?: return o
        o.put("driver", VibeRadioIds.driverOf(dev.vendorId, dev.productId))
        o.put("model", (dev.productName ?: "").trim())
        return o
    }

    // ── The engine's upcalls (JNI, on the HTTP thread that answered /vibeserver/config) ──────────────────────────

    /** {"settings":…, "device":…, "radio":…} — vibe_android_setup.h pageConfig() turns it into the page's shape. The
     *  admin password stays out of it: the page never needs to see it, and the request that reads this already proved it. */
    @JvmStatic
    fun configGetRaw(): String {
        val ctx = appCtx ?: return "{}"
        return try {
            val s = JSONObject(settings(ctx).toString())
            val facts = deviceFacts(ctx, s)
            s.remove("adminPassword")
            JSONObject().put("settings", s).put("device", facts).put("radio", radioFacts(ctx)).toString()
        } catch (t: Throwable) { Log.w(TAG, "configGetRaw: $t"); "{}" }
    }

    /** The page saved: `patch` (Android keys; a JSON null removes the key) from vibe_android_setup.h settingsPatch().
     *  Returns "" on success or the reason. Restarting is asked for, never assumed, as on Linux. */
    @JvmStatic
    fun configSetPatch(patch: String, restart: Boolean): String {
        val ctx = appCtx ?: return "the server is not ready"
        return try {
            val p = JSONObject(patch)
            val merged: JSONObject
            synchronized(this) {
                merged = settings(ctx)
                val keys = p.keys()
                while (keys.hasNext()) {
                    val k = keys.next()
                    if (k == "adminPassword") continue   // ★ never from the page
                    if (p.isNull(k)) merged.remove(k) else merged.put(k, p.get(k))
                }
                save(ctx, merged)
            }
            Log.i(TAG, "settings changed from the remote page (${p.length()} fields)${if (restart) " — restarting the server" else ""}")
            VibeServerRestore.breadcrumb(ctx, "remote settings saved (${p.length()} fields, restart=$restart)")
            // ★★ THE MIRROR: the phone's screen hears it now and redraws (ServerModeScreen listens for "settings").
            val shown = JSONObject(merged.toString()).apply { remove("adminPassword") }
            VibeServerOps.emit("settings", JSONObject().put("source", "remote").put("settings", shown))
            if (restart) VibeServerOps.restartFromSettings(ctx)
            ""
        } catch (t: Throwable) { Log.w(TAG, "configSetPatch: $t"); "the settings could not be saved (${t.javaClass.simpleName})" }
    }

    @JvmStatic private external fun nativeEnable(on: Boolean)
}
