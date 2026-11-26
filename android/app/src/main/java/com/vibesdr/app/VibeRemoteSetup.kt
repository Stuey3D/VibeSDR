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
        // ★★ What decides which controls the page may offer (vibe_android_setup.h Platform): Lite has no raw IQ, a
        //    television no battery, and the public listing needs the tunnel binary (arm64 builds only).
        o.put("lite", ctx.packageName.endsWith(".serverlite"))
        o.put("tv", ctx.packageManager.hasSystemFeature("android.hardware.type.television")
                    || ctx.packageManager.hasSystemFeature("android.software.leanback_only"))
        val tunnel = try { VibeTunnel.isSupported(ctx) } catch (_: Throwable) { false }
        o.put("tunnelSupported", tunnel)
        if (tunnel) o.put("listing", try {
            JSONObject(VibeTunnel.statusJson()).put("wanted", VibeTunnel.wanted(ctx))
        } catch (_: Throwable) { JSONObject() })
        // ★ An Icom's audio needs the microphone permission — Android's own prompt, on the phone only.
        if (dev != null && VibeRadioIds.driverOf(dev.vendorId, dev.productId) == "icom")
            o.put("micPermission", ctx.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                == android.content.pm.PackageManager.PERMISSION_GRANTED)
        return o
    }

    // ── What a page save does beyond storing: where the receiver is, and the public listing ─────────────────────

    /** ★ The page set where the receiver is: tell the engine now, in this process, so listeners and the directory see it
     *  without the phone's screen being open (the screen re-publishes it with the place's town and country when it next
     *  opens — serverSettingsMirror). Off = the name only, as the screen's publishLocation does. */
    private fun applyLocation(ctx: Context, s: JSONObject) {
        val loc = s.optJSONObject("serverLocation") ?: return
        val name = s.optString("publicName", "").trim().ifEmpty { VibeServerBoot.name(s) }
        val o = JSONObject().put("name", name)
        if (loc.optString("mode") == "manual" && loc.has("lat") && loc.has("lon")) {
            val lat = loc.optDouble("lat"); val lon = loc.optDouble("lon")
            o.put("lat", lat).put("lon", lon).put("label", loc.optString("label", ""))
            val iso = loc.optString("country", "")
            if (iso.length == 2) o.put("iso", iso)
            o.put("grid", grid(lat, lon))
        }
        try { VibeLocalSDR.setLocationJson(o.toString()); VibeServerRestore.cacheLocation(ctx, o.toString()) }
        catch (t: Throwable) { Log.w(TAG, "location from the page: $t") }
    }

    private fun sameLocation(a: JSONObject?, b: JSONObject?): Boolean {
        if (a == null || b == null) return a == b
        return a.optString("mode") == b.optString("mode") && a.optBoolean("moving") == b.optBoolean("moving")
            && a.optString("label") == b.optString("label") && a.optString("country") == b.optString("country")
            && Math.abs(a.optDouble("lat", 0.0) - b.optDouble("lat", 0.0)) < 1e-6
            && Math.abs(a.optDouble("lon", 0.0) - b.optDouble("lon", 0.0)) < 1e-6
    }

    /** Maidenhead, 6 characters — the directory places a listing by it. */
    internal fun grid(lat: Double, lon: Double): String {
        if (lat.isNaN() || lon.isNaN() || lat !in -90.0..90.0 || lon !in -180.0..180.0) return ""
        val x = minOf(359.9999, lon + 180); val y = minOf(179.9999, lat + 90)
        return buildString {
            append('A' + (x / 20).toInt()); append('A' + (y / 10).toInt())
            append('0' + ((x % 20) / 2).toInt()); append('0' + (y % 10).toInt())
            append('a' + ((x % 2) * 12).toInt()); append('a' + ((y % 1) * 24).toInt())
        }
    }

    /** Why the page may not list this server now, or null — the screen's own three refusals, in its words. */
    private fun listingRefusal(s: JSONObject): String? {
        val nm = s.optString("publicName", "").trim().ifEmpty { VibeServerBoot.name(s) }
        if (nm.length < 2) return "Give this server a public name first."
        val loc = s.optJSONObject("serverLocation")
        val placed = loc != null && loc.optString("mode") == "manual" && loc.has("lat") && loc.has("lon")
        if (!placed && loc?.optBoolean("moving", false) != true)
            return "Set this receiver\u2019s location first \u2014 the directory places servers on a map."
        if (!VibeServerRestore.isShimServing()) return "Start the server first \u2014 the listing points at a receiver that has to be running."
        return null
    }

    /** ★★ List or unlist on vibesdr.net, as the phone's "List this server publicly" switch does (VibeServerOps tunnel.*),
     *  in the background — cloudflared can take a minute to hand out an address. */
    private fun applyListing(ctx: Context, want: Boolean, s: JSONObject) {
        val app = ctx.applicationContext
        Thread({
            try {
                val running = VibeTunnel.isTunnelRunning()
                if (!want) {
                    if (running || VibeTunnel.wanted(app)) {
                        VibeServerOps.call(app, "tunnel.stop", JSONObject().put("ownerProxies", s.optString("trustedProxies", "")))
                        VibeServerRestore.breadcrumb(app, "unlisted from the remote settings page")
                    }
                    return@Thread
                }
                val st = try { JSONObject(VibeLocalSDR.getVibeServerStatus()) } catch (_: Throwable) { JSONObject() }
                val port = st.optInt("port", 0)
                val loc = s.optJSONObject("serverLocation")
                val grid = if (loc != null && loc.optString("mode") == "manual") grid(loc.optDouble("lat"), loc.optDouble("lon")) else ""
                val a = JSONObject()
                    .put("name", s.optString("publicName", "").trim().ifEmpty { VibeServerBoot.name(s) })
                    .put("locator", grid).put("port", port)
                    .put("radioModel", s.optString("radioLabel", "").trim().ifEmpty { radioFacts(app).optString("model", "") })
                    .put("radioDriver", radioFacts(app).optString("driver", ""))
                    .put("antenna", s.optString("antenna", "").trim())
                    .put("coverage", s.optString("allowRanges", ""))
                    .put("locked", s.optBoolean("advanced", false) && s.optString("radioUse", "single") == "locked")
                    .put("shareForSec", s.optLong("publicShareSec", 0L))
                    .put("ownerProxies", s.optString("trustedProxies", ""))
                if (running) VibeServerOps.call(app, "tunnel.republish", a)
                else if (port > 0) VibeServerOps.call(app, "tunnel.start", a)
                VibeServerRestore.breadcrumb(app, "listed from the remote settings page")
            } catch (t: Throwable) { Log.w(TAG, "listing from the page: $t") }
        }, "vibe-remote-listing").start()
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
            // ★ An ACTION, not a setting: list / unlist now. Checked BEFORE anything is stored, so a refusal changes nothing.
            // ★★ Every page save carries the switch as the page last saw it, so only a FLIP acts (the standing instruction is
            //    VibeTunnel's own: a restart re-lists by itself) — or, while listed, a new public name or share length.
            val listing: Boolean? = if (p.has("publicListed")) p.optBoolean("publicListed") else null
            p.remove("publicListed")
            val listedNow = VibeTunnel.wanted(ctx) || VibeTunnel.isTunnelRunning()
            val before = settings(ctx)
            val offerChanged = (p.has("publicName") && p.optString("publicName") != before.optString("publicName", "")) ||
                               (p.has("publicShareSec") && p.optLong("publicShareSec") != before.optLong("publicShareSec", 0L))
            val listingAct: Boolean? = when {
                listing == null -> null
                listing != listedNow -> listing
                listing && offerChanged && VibeTunnel.isTunnelRunning() -> true   // republish with the new offer
                else -> null
            }
            val merged: JSONObject
            synchronized(this) {
                merged = settings(ctx)
                // ★★★ beta96 a VibeServer Simple is local network only — the page may not list it (Full in the app can).
                if (listingAct == true && VibeServerBoot.isSimple(merged))
                    return "This server runs as VibeServer Simple, which is local network only. Choose VibeServer Full in the app to list it on vibesdr.net."
                if (listingAct == true && !listedNow) {
                    val trial = JSONObject(merged.toString())
                    val ks = p.keys(); while (ks.hasNext()) { val k = ks.next(); if (!p.isNull(k)) trial.put(k, p.get(k)) }
                    listingRefusal(trial)?.let { return it }
                }
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
            // ★ Every page save carries the place; only a CHANGED one is published from here (the screen's own publish adds the
            //   town's country name, which this one cannot — so an unchanged place is left as the screen published it).
            if (p.has("serverLocation") && !sameLocation(p.optJSONObject("serverLocation"), before.optJSONObject("serverLocation")))
                applyLocation(ctx, merged)
            // ★ The listing after the restart (a restart closes and re-opens the tunnel itself — restoreIfWanted).
            if (restart) VibeServerOps.restartFromSettings(ctx) { if (listingAct != null) applyListing(ctx, listingAct, merged) }
            else if (listingAct != null) applyListing(ctx, listingAct, merged)
            ""
        } catch (t: Throwable) { Log.w(TAG, "configSetPatch: $t"); "the settings could not be saved (${t.javaClass.simpleName})" }
    }

    @JvmStatic private external fun nativeEnable(on: Boolean)

    // ── ★★★ beta96 "SET UP ON ANOTHER DEVICE" — FIRST-TIME SETUP ONLY (vibe_setup_code.h, docs/v12/SERVER-CHOICE.md) ──
    // The TV shows a one-time code; a laptop on the same network opens /setup, types it, and its first and only step is
    // to SET the first admin password (typed there, twice). Then remote access to /setup is switched ON — the point of
    // it — and the laptop carries on as an ordinary admin. With a password already set, no code is issued at all.
    @JvmStatic private external fun nativeSetupInit()

    /** setupCode.issue — "" + the code, or a fail. Serves /setup for the window (the switch's state comes back on cancel). */
    fun setupCodeIssue(ctx: Context): JSONObject {
        remember(ctx)
        if (settings(ctx).optString("adminPassword", "").isNotEmpty())
            return VibeServerOps.fail("password_exists",
                "This server already has an admin password. Open /setup on another device and sign in with it.")
        if (!VibeLocalSDR.isLoaded() || !VibeServerRestore.isShimServing())
            return VibeServerOps.fail("not_serving", "Start the server first — the setup page is served by it.")
        try { nativeSetupInit() } catch (t: Throwable) { return VibeServerOps.fail("engine", "the engine has no setup codes: $t") }
        try { nativeEnable(true) } catch (t: Throwable) { Log.w(TAG, "could not serve /setup for the code: $t") }
        val code = VibeLocalSDR.setupCodeIssue()
        if (code.isEmpty()) { applySwitch(ctx); return VibeServerOps.fail("password_exists", "This server already has an admin password.") }
        Log.i(TAG, "a one-time setup code was issued (five minutes)")   // ★ never the code itself
        return VibeServerOps.ok().put("code", code)
    }

    /** setupCode.cancel — the owner backed out: the code and any session die; /setup goes back to the owner's switch. */
    fun setupCodeCancel(ctx: Context): JSONObject {
        remember(ctx)
        try { VibeLocalSDR.setupCodeCancel() } catch (_: Throwable) {}
        applySwitch(ctx)
        return VibeServerOps.ok()
    }

    fun setupCodeState(ctx: Context): JSONObject {
        remember(ctx)
        val st = try { JSONObject(VibeLocalSDR.setupCodeState()) } catch (_: Throwable) { JSONObject() }
        return VibeServerOps.ok().put("state", st).put("hasAdminPassword", settings(ctx).optString("adminPassword", "").isNotEmpty())
            .put("remote", remoteOn(ctx))
    }

    /** ★ From the engine (JNI), on the shim's HTTP thread: the page set the FIRST admin password. "" = in force. */
    @JvmStatic
    fun setupPasswordSet(pw: String): String {
        val ctx = appCtx ?: return "the server is not ready"
        return try {
            synchronized(this) {
                val cur = settings(ctx)
                if (cur.optString("adminPassword", "").isNotEmpty()) return "this server already has an admin password"
                cur.put("adminPassword", pw)
                save(ctx, cur)   // ★ the store AND the restore's config (replaceConfig) — a restored server keeps it
            }
            VibeLocalSDR.setVibeServerAdminSecret(pw)
            // ★★★ Remote access to /setup ON — it is why the owner set the password from there (Stuart, 2026-11-26).
            setRemoteAccess(ctx, true)
            // ★ The phone's screen keeps its own copy of the password (vs_adminpw); hand it over so a Start from Full
            //   sends this one rather than an empty field. Same device, the server's own link to its UI.
            VibeServerOps.emit("adminPassword", JSONObject().put("password", pw))
            Log.i(TAG, "the first admin password was set from another device; remote access to /setup is ON")
            ""
        } catch (t: Throwable) { "the password could not be stored (${t.javaClass.simpleName})" }
    }
}
