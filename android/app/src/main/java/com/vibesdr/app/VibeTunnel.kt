package com.vibesdr.app

import android.content.Context
import android.util.Log
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * "Advertise on VibeSDR.net" — a Cloudflare Quick Tunnel plus the directory listing behind it.
 *
 * ★★★ THE TUNNEL EXISTS BECAUSE A PHONE CANNOT BE REACHED. On mobile data the handset is behind
 *     CGNAT: there is no address to publish and no port to forward, so "discovery here, data
 *     direct" — which is the right design for a port-forwarded Pi — cannot work at all. For this
 *     server the tunnel carries the audio or nothing does.
 *
 * ★★★ A QUICK TUNNEL, ON NOBODY'S ACCOUNT. Named tunnels would run on OUR Cloudflare account,
 *     making its 1000-tunnel cap the ceiling on how many VibeServers can ever be listed and making
 *     us the transit provider — and the moderator — for every listener's audio. Quick Tunnels cost
 *     us nothing and belong to no one.
 *
 * ★★★ THE HOSTNAME ROTATES ON EVERY RESTART, AND THAT IS FINE. Nothing keys on it: the listing is
 *     keyed on a server-issued id, and the shareable address is <slug>.vibeserver.vibesdr.net,
 *     which redirects to whatever the last ping reported. The friendly name is the stable identity
 *     the tunnel address never was.
 */
object VibeTunnel {

    private const val TAG = "VibeTunnel"
    private const val DIRECTORY = "https://vibeserver.vibesdr.net"
    private const val PREFS = "vibe_directory"

    /** ★ Identity, kept across app restarts. Lost on uninstall — which is what the "transfer to a
     *  new device" flow exists to cover, and what the usage-proportional address hold softens. */
    private const val K_ID = "id"
    private const val K_KEY = "key"
    private const val K_SLUG = "slug"

    private val http = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(30, TimeUnit.SECONDS)
        .build()

    private val JSON = "application/json; charset=utf-8".toMediaType()

    private var proc: Process? = null
    private var reader: Thread? = null
    private val running = AtomicBoolean(false)

    @Volatile private var tunnelUrl: String = ""
    @Volatile private var address: String = ""
    @Volatile private var lastError: String = ""
    @Volatile private var listed: Boolean = false

    /**
     * ★★★ IT MUST BE nativeLibraryDir, AND IT MUST BE CALLED lib*.so.
     *
     * Android has refused to execute anything from writable app storage since API 29 (W^X), so a
     * binary copied into filesDir at runtime cannot be started. Files in nativeLibraryDir can be.
     * The .so suffix is simply what gets a file packaged into that directory — this is a Go
     * executable, not a shared library.
     * ★★ And it is only actually PRESENT there when the libraries are extracted at install, which
     *    is why gradle.properties sets expo.useLegacyPackaging=true. With the default, that
     *    directory is empty — verified on the Moto.
     */
    private fun binary(ctx: Context): File =
        File(ctx.applicationInfo.nativeLibraryDir, "libcloudflared.so")

    fun isSupported(ctx: Context): Boolean = binary(ctx).exists()

    /** What the switch shows: whether we are up, the address to share, and why not if not. */
    fun statusJson(): String = JSONObject().apply {
        put("running", running.get())
        put("listed", listed)
        put("tunnelUrl", tunnelUrl)
        put("address", address)
        put("error", lastError)
    }.toString()

    // ── the tunnel ────────────────────────────────────────────────────────────────────────────

    /**
     * Start cloudflared against the local server port and wait for it to publish a hostname.
     *
     * ★★ Its output is the ONLY place the hostname appears — a Quick Tunnel is assigned by the
     *    edge, not chosen — so the log is parsed rather than merely logged.
     */
    @Synchronized
    fun startTunnel(ctx: Context, localPort: Int, onReady: (String?) -> Unit) {
        if (running.get()) { onReady(tunnelUrl.ifEmpty { null }); return }
        val bin = binary(ctx)
        if (!bin.exists()) {
            // ★ AGENTS.md: never leave a control visible and inert. The switch should be absent on
            //   a build without the binary — this is the belt to that braces.
            lastError = "the tunnel component is not present in this build"
            onReady(null); return
        }

        lastError = ""
        tunnelUrl = ""
        try {
            val pb = ProcessBuilder(
                bin.absolutePath,
                "tunnel",
                "--url", "http://127.0.0.1:$localPort",
                // ★ We ship a pinned build; letting it replace itself would both defeat that and
                //   try to write to a directory it cannot write to.
                "--no-autoupdate",
                // ★ Quieter than the default, and we only need the line carrying the hostname.
                "--loglevel", "info"
            ).redirectErrorStream(true)
            val p = pb.start()
            proc = p
            running.set(true)

            reader = Thread {
                try {
                    BufferedReader(InputStreamReader(p.inputStream)).use { r ->
                        var line = r.readLine()
                        var announced = false
                        while (line != null) {
                            val m = HOSTNAME.find(line)
                            if (m != null && !announced) {
                                announced = true
                                tunnelUrl = m.value
                                Log.i(TAG, "quick tunnel up: $tunnelUrl")
                                onReady(tunnelUrl)
                            }
                            line = r.readLine()
                        }
                    }
                } catch (t: Throwable) {
                    Log.w(TAG, "tunnel reader ended: ${t.message}")
                } finally {
                    // ★★ The process ending is not necessarily our doing — Android may kill it, or
                    //    Cloudflare may drop an account-less tunnel. Reflect that rather than
                    //    continuing to claim we are up.
                    running.set(false)
                    if (tunnelUrl.isEmpty()) {
                        lastError = "the tunnel stopped before it was given an address"
                        onReady(null)
                    }
                }
            }.also { it.isDaemon = true; it.start() }
        } catch (t: Throwable) {
            running.set(false)
            lastError = t.message ?: "could not start the tunnel"
            Log.e(TAG, "startTunnel failed", t)
            onReady(null)
        }
    }

    private val HOSTNAME = Regex("""https://[a-z0-9-]+\.trycloudflare\.com""")

    /**
     * ★★★ THE TUNNEL MAKES EVERY LISTENER 127.0.0.1, AND THIS HAS ALREADY BITTEN US.
     *
     * cloudflared connects to the server from loopback, so without loopback in the trusted-proxy
     * list every listener arrives as 127.0.0.1 and the session limit, the IP cooldown and the
     * one-address rule all switch themselves OFF — at the exact moment the server becomes public.
     *
     * ★★★ 2026-08-09, in local_sdr_shim.cpp's own words: "the demo went behind a Cloudflare tunnel
     *     and the countdown vanished. The countdown was the only visible symptom of a receiver
     *     that had stopped enforcing anything at all."
     *
     * ★★ So the switch sets this ITSELF, in the same action that starts the tunnel. Leaving it to
     *    the owner means a server that works perfectly while every protection it relies on is off,
     *    and nothing says so.
     * ★ LOOPBACK ONLY. Widening the list would let a forged X-Forwarded-For choose its own
     *   address, which is the attack vibe_proxy.h exists to prevent.
     */
    fun applyLoopbackTrust(on: Boolean, ownerCsv: String) {
        val owner = ownerCsv.split(',').map { it.trim() }.filter { it.isNotEmpty() }
        val merged = if (on) (owner + listOf("127.0.0.1", "::1")).distinct() else owner
        try {
            VibeLocalSDR.setTrustedProxies(merged.joinToString(","))
            Log.i(TAG, "trusted proxies -> ${merged.joinToString(",")}")
        } catch (t: Throwable) {
            Log.e(TAG, "could not set trusted proxies", t)
        }
    }

    @Synchronized
    fun stopTunnel() {
        try { proc?.destroy() } catch (_: Throwable) {}
        proc = null
        running.set(false)
        tunnelUrl = ""
    }

    // ── the directory ─────────────────────────────────────────────────────────────────────────

    private fun prefs(ctx: Context) = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /**
     * What the directory shows about this receiver: its radios and how busy it is.
     *
     * ★★★ ASKED OVER LOCALHOST, NOT REBUILT FROM THE BRIDGE. The shim already publishes exactly
     *     this at /vibeserver/radios — it is what the vibesdr.net demo card reads — so assembling
     *     a second version here from getVibeServerStatus() would be a copy that drifts. The first
     *     attempt did exactly that and every listing said "This server has not published its
     *     radios yet" (2026-08-22).
     *
     * ★★ NO SERIAL NUMBERS. They are hardware identity and have no business on a public page —
     *    the same line website/worker.js draws, and the server's own landing page before it.
     */
    private fun buildStatus(port: Int, radioModel: String, radioDriver: String): JSONObject {
        val out = JSONObject()
        val radios = JSONArray()

        // ★ The multi-radio front door. Populated on a machine running several receivers behind
        //   one door; EMPTY on a phone in Simple mode, which holds exactly one radio itself.
        try {
            val req = Request.Builder().url("http://127.0.0.1:$port/vibeserver/radios").build()
            val body = http.newCall(req).execute().use { it.body?.string() ?: "" }
            val arr = JSONObject(body).optJSONArray("radios") ?: JSONArray()
            for (i in 0 until arr.length()) {
                val r = arr.optJSONObject(i) ?: continue
                radios.put(JSONObject().apply {
                    put("name", r.optString("label"))
                    put("driver", r.optString("driver"))
                    put("mode", r.optString("mode"))
                    put("shared", r.optBoolean("locked", false))
                    put("restricted", r.optBoolean("restricted", false))
                    put("coverage", r.optJSONArray("coverage") ?: JSONArray())
                })
            }
        } catch (t: Throwable) {
            Log.w(TAG, "could not read the radio list: ${t.message}")
        }

        // ★★★ A PHONE IN SIMPLE MODE PUBLISHES NO RADIO LIST, AND THAT IS NOT AN ERROR.
        //     /vibeserver/radios describes the radios behind a FRONT DOOR; a phone serving its own
        //     dongle has no door and returns {"radios":[]}. Reading only that endpoint made every
        //     phone listing say "This server has not published its radios yet" while working
        //     perfectly (Stuart, 2026-08-22). So where the door lists nothing, describe the one
        //     radio this process is holding — which the app already knows the make of.
        if (radios.length() == 0 && (radioModel.isNotEmpty() || radioDriver.isNotEmpty())) {
            radios.put(JSONObject().apply {
                put("name", radioModel.ifEmpty { "Radio" })
                put("driver", radioDriver)
                put("shared", false)
            })
        }
        out.put("radios", radios)

        // ★★ COUNTS FROM /vibeserver.json — the server's OWN identity response, which is what the
        //    vibesdr.net demo card reads. Same numbers everywhere, rather than a second opinion
        //    assembled from the bridge.
        try {
            val req = Request.Builder().url("http://127.0.0.1:$port/vibeserver.json").build()
            val body = http.newCall(req).execute().use { it.body?.string() ?: "" }
            val j = JSONObject(body)
            out.put("listeners", j.optInt("listeners", 0))
            out.put("maxListeners", j.optInt("maxUsers", 1))
            out.put("freeInSec", j.optInt("freeInSec", -1))
        } catch (t: Throwable) {
            Log.w(TAG, "could not read /vibeserver.json: ${t.message}")
        }

        // ★★ SAID, NOT INFERRED. The directory drew a "temporary share" from how far the expiry
        //    sat from the last ping, so an ordinary listing was a yellow diamond on the map.
        out.put("temporary", false)
        return out
    }

    private fun post(path: String, body: JSONObject): JSONObject? = try {
        val req = Request.Builder()
            .url("$DIRECTORY$path")
            .post(body.toString().toRequestBody(JSON))
            .build()
        http.newCall(req).execute().use { res ->
            val text = res.body?.string() ?: "{}"
            JSONObject(text).put("_status", res.code)
        }
    } catch (t: Throwable) {
        Log.w(TAG, "$path failed: ${t.message}")
        null
    }

    /**
     * Register (first time) or ping (thereafter).
     *
     * ★★★ THE KEY IS THE IDENTITY and it is issued ONCE. It is never sent back to us by the
     *     directory and cannot be recovered — losing it means re-registering, which is why the
     *     address hold is proportional to how long the listing was actually used.
     */
    fun publish(ctx: Context, name: String, locator: String, port: Int,
                radioModel: String, radioDriver: String): String? {
        val status = buildStatus(port, radioModel, radioDriver)
        val url = tunnelUrl
        if (url.isEmpty()) { lastError = "no tunnel yet"; return null }

        val p = prefs(ctx)
        val id = p.getString(K_ID, null)
        val key = p.getString(K_KEY, null)

        if (id != null && key != null) {
            val r = post("/api/directory/ping", JSONObject().apply {
                put("id", id); put("key", key); put("url", url)
                put("name", name); put("status", status)
            })
            if (r != null && r.optInt("_status") == 200) {
                listed = true
                // ★★ A returning server may have LOST its address: away longer than the hold, and
                //    somebody else took the name. The switch must say so rather than keep showing
                //    an address that now belongs to a stranger.
                address = r.optString("address", "")
                if (address.isEmpty()) lastError = "this server's public address was released"
                return address.ifEmpty { null }
            }
            // ★ 404 = the directory has never heard of this id (its row was removed). Fall through
            //   and register afresh rather than sulking for ever.
            if (r != null && r.optInt("_status") == 404) {
                p.edit().clear().apply()
            } else {
                listed = false
                lastError = r?.optString("error") ?: "could not reach the directory"
                return null
            }
        }

        val r = post("/api/directory/register", JSONObject().apply {
            put("name", name); put("grid", locator); put("url", url)
            put("kind", "tunnel"); put("locator", locator); put("status", status)
        })
        if (r == null) { listed = false; lastError = "could not reach the directory"; return null }
        when (r.optInt("_status")) {
            200 -> {
                p.edit()
                    .putString(K_ID, r.optString("id"))
                    .putString(K_KEY, r.optString("key"))
                    .putString(K_SLUG, r.optString("slug"))
                    .apply()
                listed = true
                address = r.optString("address", "")
                lastError = ""
                return address
            }
            409 -> {
                // ★★ The name is taken. The choices come back with the refusal so the screen can
                //    offer them directly rather than making the owner guess what is free.
                listed = false
                val alts = r.optJSONArray("suggestions") ?: JSONArray()
                lastError = r.optString("error") + (if (alts.length() > 0) " — try: $alts" else "")
                return null
            }
            else -> {
                listed = false
                lastError = r.optString("error", "the directory refused the listing")
                return null
            }
        }
    }

    /** ★ Turning the switch OFF frees the public address IMMEDIATELY, rather than letting it lapse. */
    fun delist(ctx: Context) {
        val p = prefs(ctx)
        val id = p.getString(K_ID, null) ?: return
        val key = p.getString(K_KEY, null) ?: return
        post("/api/directory/delist", JSONObject().apply { put("id", id); put("key", key) })
        p.edit().clear().apply()
        listed = false
        address = ""
    }
}
