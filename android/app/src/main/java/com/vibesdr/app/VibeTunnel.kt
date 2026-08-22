package com.vibesdr.app

import android.content.Context
import android.os.Build
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
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
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

    /** ★ How often to renew, in seconds. The directory returns its own `pingSec` on every register
     *  and ping, and that answer wins — the interval is the SERVER'S to choose, so it can be
     *  changed centrally without shipping a new app. This is only the value used until it has
     *  spoken, and matches what the directory currently advertises. */
    @Volatile private var PING_SEC: Long = 900

    /** ★ Identity, kept across app restarts. Lost on uninstall — which is what the "transfer to a
     *  new device" flow exists to cover, and what the usage-proportional address hold softens. */
    private const val K_ID = "id"
    private const val K_KEY = "key"
    private const val K_SLUG = "slug"

    /** ★★★ WHAT THIS SERVER IS LISTED AS, kept so the listing can COME BACK BY ITSELF. A tunnel
     *  belongs to the process that spawned it, so anything that restarts the app — an update, a
     *  low-memory kill, a reboot, or the owner reopening it — takes the tunnel with it while the
     *  directory goes on advertising the dead address until the entry expires (seen 2026-08-22,
     *  after an adb install: "it just lost the server"). Re-establishing has to be automatic;
     *  nobody should have to know to go and flick a switch again. */
    private const val K_WANT = "wantListed"
    private const val K_NAME = "pubName"
    private const val K_GRID = "pubGrid"
    private const val K_MODEL = "pubModel"
    private const val K_DRIVER = "pubDriver"
    private const val K_ANT = "pubAntenna"
    private const val K_COV = "pubCoverage"
    private const val K_LOCKED = "pubLocked"
    /** ★★★ WHEN THIS SHARE ENDS, as an ABSOLUTE time — not how long it was offered for.
     *  A restore that re-sent "15 minutes" would restart the clock on every reboot, so an offer
     *  meant to end at nine would quietly run for ever, fifteen minutes at a time. Storing the END
     *  makes a restart carry the share forward instead of renewing it. 0 = permanent. */
    private const val K_UNTIL = "pubUntilEpoch"

    private val http = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(30, TimeUnit.SECONDS)
        .build()

    private val JSON = "application/json; charset=utf-8".toMediaType()

    /**
     * ★★★ THE LISTING HAS TO BE RENEWED OR IT DISAPPEARS. The directory does not detect a dead
     *     server, it lets one EXPIRE: the list query is `WHERE expires_at > now()`, so a receiver
     *     that stops pinging simply stops being selected. That is what makes a flat battery or a
     *     killed app vanish on its own with no probe and no cron — and it is equally what removes a
     *     perfectly healthy server that never renews.
     *
     * ★★ It was registered once and never pinged again (2026-08-22), so the entry was ALWAYS going
     *    to fall off half an hour later, and the listener count could never change because nothing
     *    ever told the directory it had.
     *
     * ★ 15 minutes, the interval the directory advertises. The listener COUNT is not carried by
     *   this — see the directory page, which reads live status straight from a tunnelled server.
     *   This is liveness, not telemetry.
     */
    private val pinger = Executors.newSingleThreadScheduledExecutor { r ->
        Thread(r, "vibe-directory-ping").apply { isDaemon = true }
    }
    private var pingTask: ScheduledFuture<*>? = null
    private var expiryTask: ScheduledFuture<*>? = null
    /** ★ The share's end, mirrored for statusJson so the screen need not read prefs. */
    @Volatile private var pendingUntil: Long = 0L
    /** What the ping needs to describe this server, kept so the timer can run without the UI. */
    @Volatile private var lastPublish: (() -> Unit)? = null

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

    /** ★ Is the tunnel up? Asked by the boot path, which must re-apply loopback trust after a
     *  restart or the session limit silently stops applying — see VibeServerBoot. */
    fun isTunnelRunning(): Boolean = running.get()

    /** What the switch shows: whether we are up, the address to share, and why not if not. */
    fun statusJson(): String = JSONObject().apply {
        put("running", running.get())
        put("listed", listed)
        put("tunnelUrl", tunnelUrl)
        put("address", address)
        put("error", lastError)
        // ★ When this share ends, as an ABSOLUTE epoch — so the screen counts down from the same
        //   instant the directory does, rather than from a length it would have to track itself.
        put("until", pendingUntil)
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
    /**
     * ★★★ WHAT THE RECEIVER IS RUNNING ON. A directory entry that says only "RTL-SDR v4" leaves the
     *     interesting half out: a listener judging whether a stream will hold up wants to know it
     *     is a phone, and which phone. Stuart, 2026-08-22, wanting his own entry to read
     *     "Server: Motorola G35 Unisoc T760".
     * ★ SOC_MODEL is API 31+; below that the chip is simply not named rather than guessed at —
     *   [[feedback_no_inferred_hardware_readouts]]: if the platform cannot tell us, show nothing.
     */
    private fun hostModel(): String {
        val maker = Build.MANUFACTURER.replaceFirstChar { it.uppercase() }.trim()
        val model = Build.MODEL.trim()
        // ★ "Motorola moto g35 5G" reads badly; drop a maker the model already names.
        val name = if (model.lowercase().startsWith(maker.lowercase())) model else "$maker $model"
        val soc = if (Build.VERSION.SDK_INT >= 31) Build.SOC_MODEL.trim() else ""
        return if (soc.isNotEmpty() && !soc.equals("unknown", true)) "$name $soc" else name
    }

    private fun buildStatus(port: Int, radioModel: String, radioDriver: String,
                            antenna: String, coverage: String, locked: Boolean): JSONObject {
        val out = JSONObject()
        val radios = JSONArray()

        // ★★ READ ONCE, USED TWICE. /vibeserver.json carries both the counts and what this
        //    receiver can be tuned to, and the radio entries below need the second half — so it is
        //    fetched here rather than after the list is already built.
        val ident: JSONObject? = try {
            val req = Request.Builder().url("http://127.0.0.1:$port/vibeserver.json")
                .header("User-Agent", "VibeServer-directory").build()
            JSONObject(http.newCall(req).execute().use { it.body?.string() ?: "" })
        } catch (t: Throwable) {
            Log.w(TAG, "could not read /vibeserver.json: ${t.message}")
            null
        }

        // ★ The multi-radio front door. Populated on a machine running several receivers behind
        //   one door; EMPTY on a phone in Simple mode, which holds exactly one radio itself.
        try {
            // ★★ NAME OURSELVES. /vibeserver/radios counts a landing-page visitor, so an unlabelled
            //    poll shows up on the owner's admin screen as somebody choosing a radio. See
            //    vsIsSelfPoll in the shim.
            val req = Request.Builder().url("http://127.0.0.1:$port/vibeserver/radios")
                .header("User-Agent", "VibeServer-directory").build()
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
                    // ★★★ THE FRONT DOOR'S `coverage` IS NUMBERS, AND THE DIRECTORY'S IS WORDS.
                    //     Two different vocabularies behind one key: /vibeserver/radios sends
                    //     [[500,31000000]] as `coverage` and the band NAMES as `allowedNames`,
                    //     while the directory page joins `coverage` as text. Passing it straight
                    //     through would have printed "500,31000000" on the card of every
                    //     multi-radio server — the Pi and the Mac — the day one first listed. It
                    //     has never been seen only because the phone takes the branch below.
                    put("coverage", r.optJSONArray("allowedNames") ?: JSONArray())
                    // ★★ AND THE NUMBERS TRAVEL SEPARATELY, under a key that means numbers. What
                    //    the owner PERMITS, falling back to the hardware's reach where no lists
                    //    are set — a search must not offer a band the operator has blocked.
                    put("ranges", r.optJSONArray("allowed")
                        ?: r.optJSONArray("coverage") ?: JSONArray())
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
                // ★★★ THE RADIO'S MODE, NOT A GUESS. `locked` means each listener gets their own
                //     VFO inside a fixed window; unlocked with room for several means ONE dial that
                //     everyone shares — which is the difference between tuning freely and taking
                //     the station away from other people. Hardcoding false here said "shared dial"
                //     about every radio, including ones where it was untrue.
                put("locked", locked)
                put("shared", locked)
                // ★ What this radio is actually allowed to tune, when the owner has narrowed it —
                //   "FM broadcast" is far more use to a listener than a frequency pair.
                // ★ Prefer the owner's own words for the band; fall back to the names the
                //   server derived from its band plan, which is what an unrestricted radio has.
                val bands = ident?.optJSONArray("bands")
                // ★★★ ONE BAND PER ENTRY, NOT ONE STRING OF ALL OF THEM. The screen joins the
                //     owner's bands into "AM (medium wave) broadcast, FM Broadcast Band" for
                //     display, and that whole sentence was being published as a SINGLE coverage
                //     entry — so the directory drew one chip carrying both names, and filtering
                //     for FM alone could never match it. Seen live on the Xcover 4S the moment it
                //     listed itself (2026-08-22), which is precisely what a second real server was
                //     going to reveal: with one locked-to-FM phone in the estate, a list of one
                //     and a string of one are indistinguishable.
                //  ★ Split here rather than at the screen, because the screen's join is what the
                //    SETUP page shows a human and is right for that.
                if (coverage.isNotEmpty())
                    put("coverage", JSONArray().apply {
                        coverage.split(",").map { it.trim() }.filter { it.isNotEmpty() }
                                .forEach { put(it) }
                    })
                else if (bands != null && bands.length() > 0) put("coverage", bands)
                // ★★ WHAT IT CAN BE TUNED TO, in Hz — see vsTunableRanges in the shim. Absent when
                //    the server could not say, and absent must never be read as "anything".
                ident?.optJSONArray("tunable")?.let { put("ranges", it) }
            })
        }
        out.put("radios", radios)

        // ★★ COUNTS FROM /vibeserver.json — the server's OWN identity response, which is what the
        //    vibesdr.net demo card reads. Same numbers everywhere, rather than a second opinion
        //    assembled from the bridge.
        try {
            val j = ident ?: JSONObject()
            out.put("listeners", j.optInt("listeners", 0))
            out.put("maxListeners", j.optInt("maxUsers", 1))
            out.put("freeInSec", j.optInt("freeInSec", -1))
            // ★ The session limit is a fact a listener wants BEFORE clicking, not after being cut
            //   off — the directory is exactly where "how long do I get" belongs.
            out.put("limitMin", j.optInt("limitMin", 0))
            // ★★ WHETHER A PIN IS NEEDED, said BEFORE somebody clicks. A club receiver that only
            //   its members may use is a perfectly good listing — but a stranger arriving at a PIN
            //   box they cannot fill reads it as a broken server, and goes away rather than
            //   understanding it was never for them (Stuart, 2026-08-22).
            out.put("pin", j.optBoolean("pin", false))
        } catch (t: Throwable) {
            Log.w(TAG, "could not read /vibeserver.json: ${t.message}")
        }

        // ★★ SAID, NOT INFERRED. The directory drew a "temporary share" from how far the expiry
        //    sat from the last ping, so an ordinary listing was a yellow diamond on the map.
        out.put("temporary", false)
        if (antenna.isNotEmpty()) out.put("antenna", antenna)
        out.put("host", hostModel())
        // ★★ WHAT IT RUNS ON, WITH ITS VERSION — "Android 16", the way the daemon will say
        //   "Debian 13" or "Ubuntu 24.04" from /etc/os-release. A family name alone ("Linux")
        //   answers less than the question deserves: a Pi 500 on Debian 13 and an old box on
        //   Ubuntu 20.04 are different propositions to somebody choosing a server (Stuart,
        //   2026-08-22).
        //  ★ Composed HERE rather than assembled by the page: the platform knows how it likes to
        //    be named, and a page that built the string would need a rule per platform.
        //  ★★ And never inferred — if the version is missing, the family alone is the honest
        //     answer rather than a guessed number.
        out.put("platform",
                android.os.Build.VERSION.RELEASE?.trim()?.takeIf { it.isNotEmpty() }
                    ?.let { "Android $it" } ?: "Android")
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
                radioModel: String, radioDriver: String,
                antenna: String, coverage: String, locked: Boolean = false,
                shareForSec: Long = 0): String? {
        // ★ Remember how to say all this again, so the renewal below needs nothing from the UI —
        //   which may well be gone: the server keeps running with the screen off.
        // ★★★ THE OFFER IS SET ONCE, NOT RENEWED. A temporary share ends when it said it would —
        //     if every ping pushed the end further away, "up for the week of the contest" would
        //     quietly become "up for ever, one ping at a time". So the RENEWAL sends 0, meaning
        //     "unchanged", and only a deliberate act from the screen moves it.
        // ★ -1, not 0: a renewal must leave the share window exactly as it is, and 0 now means
        //   "permanent" — see publishOnce.
        lastPublish = { publishOnce(ctx, name, locator, port, radioModel, radioDriver, antenna, coverage, locked, -1L) }
        prefs(ctx).edit()
            .putBoolean(K_WANT, true).putString(K_NAME, name).putString(K_GRID, locator)
            .putString(K_MODEL, radioModel).putString(K_DRIVER, radioDriver)
            .putString(K_ANT, antenna).putString(K_COV, coverage).putBoolean(K_LOCKED, locked)
            .putLong(K_UNTIL, when {
                shareForSec > 0 -> (System.currentTimeMillis() / 1000) + shareForSec
                shareForSec == 0L -> 0L        // ★ deliberately permanent — clear any old end
                // ★★★ AND NEVER CARRY A DEAD ONE FORWARD. A share that had already expired left
                //     its end in prefs, so the next ORDINARY listing inherited it and the expiry
                //     timer fired the instant the tunnel came up — "temporary share ends in 0s"
                //     in the log, a brand-new permanent listing killed within a second of
                //     starting (seen 2026-08-22 21:11:22). A renewal keeps a LIVE end only.
                else -> prefs(ctx).getLong(K_UNTIL, 0L)
                            .takeIf { it > System.currentTimeMillis() / 1000 } ?: 0L
            })
            .apply()
        startPinging()
        // ★ Read BACK rather than recomputed: a renewal passes 0 and must keep the end it already
        //   has, and that rule already lives in the line above. Two places deciding when a share
        //   ends is how they come to disagree.
        scheduleExpiry(ctx, prefs(ctx).getLong(K_UNTIL, 0L))
        return publishOnce(ctx, name, locator, port, radioModel, radioDriver, antenna, coverage, locked, shareForSec)
    }

    /**
     * ★★★ END THE SHARE WHEN IT SAYS IT ENDS. The directory stops LISTING an expired share by
     *     itself — expiry is evaluated when the list is read — but nothing on the phone was acting
     *     on it, so the tunnel stayed open and the public address kept working long after the
     *     offer had run out (Stuart, 2026-08-22: "the temporary time limit was ignored and I still
     *     had access via the tunnel after expiry"). A listing that disappears while the door it
     *     advertised stays unlocked is the worst of both: the owner believes it is over.
     *
     * ★★ RESTORE ALREADY GOT THIS RIGHT, which is exactly why it was missed. A share that ended
     *    while the app was closed was refused on the way back up — so the case that was tested
     *    passed, and the one nobody restarts for ran for ever.
     *
     * ★ Unlisted, NOT stopped: "when the temporary server time expires then it simply reverts to
     *   an unlisted but still active vibeserver" (Stuart). The receiver keeps running for whoever
     *   is on it locally; only the public door closes.
     */
    @Synchronized
    private fun scheduleExpiry(ctx: Context, untilEpoch: Long) {
        expiryTask?.cancel(false); expiryTask = null
        pendingUntil = untilEpoch
        if (untilEpoch <= 0L) return
        val secs = untilEpoch - (System.currentTimeMillis() / 1000)
        // ★ Already gone: end it now rather than scheduling a negative delay, which would fire
        //   immediately anyway but through a path nobody reading this would expect.
        val delay = if (secs > 0) secs else 0L
        Log.i(TAG, "temporary share ends in ${delay}s — the tunnel will close then")
        expiryTask = pinger.schedule({
            try {
                Log.i(TAG, "temporary share has ended — delisting and closing the tunnel")
                delist(ctx)
                stopTunnel()
                applyLoopbackTrust(false, "")
            } catch (t: Throwable) { Log.w(TAG, "could not end the share cleanly: ${t.message}") }
        }, delay, TimeUnit.SECONDS)
    }

    /** ★ Renew on the advertised interval, and never let one failure end the schedule: a phone that
     *  loses signal for a minute must come back to a live listing, not a dead one. */
    @Synchronized
    private fun startPinging() {
        pingTask?.cancel(false)
        pingTask = pinger.scheduleWithFixedDelay({
            try { if (running.get()) lastPublish?.invoke() } catch (t: Throwable) {
                Log.w(TAG, "directory renewal failed, will try again: ${t.message}")
            }
        }, PING_SEC, PING_SEC, TimeUnit.SECONDS)
    }

    /**
     * ★ A quick attempt, without disturbing the steady interval behind it.
     * ★★ It re-enters publishOnce, whose ping branch checks the answer again — so this keeps
     *    trying while the address is unproven and stops of its own accord once it is. No counter
     *    to get wrong, and no way to leave a listing stuck unverified for a quarter of an hour.
     */
    @Synchronized
    private fun retrySoon() {
        pinger.schedule({
            try { if (running.get()) lastPublish?.invoke() } catch (t: Throwable) {
                Log.w(TAG, "verification retry failed: ${t.message}")
            }
        }, 30, TimeUnit.SECONDS)
    }

    @Synchronized
    private fun stopPinging() {
        pingTask?.cancel(false); pingTask = null; lastPublish = null
    }

    private fun publishOnce(ctx: Context, name: String, locator: String, port: Int,
                            radioModel: String, radioDriver: String,
                            antenna: String, coverage: String, locked: Boolean,
                            shareForSec: Long): String? {
        val status = buildStatus(port, radioModel, radioDriver, antenna, coverage, locked)
        val url = tunnelUrl
        if (url.isEmpty()) { lastError = "no tunnel yet"; return null }

        val p = prefs(ctx)
        val id = p.getString(K_ID, null)
        val key = p.getString(K_KEY, null)
        // ★ Re-supplied on every publish rather than once: the shim holds it only in memory, so a
        //   server that restarted under us must be given it again before the directory next asks.
        if (!key.isNullOrEmpty()) {
            try { VibeLocalSDR.setDirectoryKey(key) } catch (_: Throwable) {}
        }

        if (id != null && key != null) {
            val r = post("/api/directory/ping", JSONObject().apply {
                put("id", id); put("key", key); put("url", url)
                // ★★★ WHERE IT IS NOW, NOT WHERE IT WAS REGISTERED. The locator was sent ONCE, at
                //     registration, so a receiver that moved kept its original pin for ever — take
                //     the phone on holiday and the map would still show it at home (Stuart,
                //     2026-08-22, asking exactly that). The address is refreshed on every ping
                //     because a tunnel hostname rotates; the position deserves the same treatment
                //     for the same reason — it is a fact that can change while the listing lives.
                put("grid", locator)
                // ★★★ THREE STATES, NOT TWO. `0` used to mean both "make this permanent" and
                //     "leave it alone", so TURNING THE TEMPORARY TOGGLE OFF could not make a share
                //     permanent — the value was simply omitted and the old end kept. A renewal now
                //     says -1 (omit, unchanged) and only a deliberate change sends a number, which
                //     is the same distinction the directory already draws between an absent field
                //     and an explicit 0.
                if (shareForSec >= 0) put("shareForSec", shareForSec)
                put("name", name); put("status", status)
            })
            if (r != null && r.optInt("_status") == 200) {
                listed = true
                // ★★★ RETRY SOON WHILE THE ADDRESS IS UNPROVEN. The directory challenges the
                //     address on a PING, and the first ping happens the instant the tunnel comes
                //     up — the worst possible moment, when cloudflared has a hostname but the edge
                //     may not yet route to it. That one attempt failing meant nothing tried again
                //     for FIFTEEN MINUTES, with a working tunnel and a switch reading ON while the
                //     directory showed nothing (Stuart, 2026-08-22: "tunnel is working directory
                //     is not listing").
                //  ★★ So the interval follows the STATE, not the clock: a minute while there is
                //     something to prove, the full interval once proved. A listing that cannot
                //     verify is retried until it can, rather than punished for being new.
                if (!r.optBoolean("verified", true)) {
                    Log.i(TAG, "address not verified yet — retrying shortly")
                    retrySoon()
                }
                // ★★★ A SUCCESS MUST CLEAR THE LAST FAILURE. lastError was only ever SET, so the
                //     "no tunnel yet" raised while the tunnel was still dialling — which is normal
                //     during a restore, since publish() runs the moment the switch is restored and
                //     the hostname arrives seconds later — stayed on screen for ever. The switch
                //     read ON, with the right address beside it, and a red error underneath
                //     (Stuart, 2026-08-22). A stale error is worse than none: it says the thing in
                //     front of you is broken while it plainly works.
                lastError = ""
                r.optInt("pingSec", 0).takeIf { it > 0 }?.let { PING_SEC = it.toLong() }
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
            // ★★★ THE SHARE LENGTH TRAVELS ON THE REGISTRATION TOO. It was on the PING body only,
            //     so a temporary share set up from scratch was recorded as PERMANENT and stayed in
            //     the directory for ever — "I turned on temporary share 15 minutes and its a full
            //     share" (Stuart, 2026-08-22). It appeared to work exactly once, because flipping
            //     the toggle on an ALREADY-LISTED server goes through a ping, which carried it.
            //  ★ The path nobody tests is the first one every new user takes.
            if (shareForSec >= 0) put("shareForSec", shareForSec)
        })
        if (r == null) { listed = false; lastError = "could not reach the directory"; return null }
        when (r.optInt("_status")) {
            200 -> {
                p.edit()
                    .putString(K_ID, r.optString("id"))
                    .putString(K_KEY, r.optString("key"))
                    .putString(K_SLUG, r.optString("slug"))
                    .apply()
                // ★★★ THE SHIM ANSWERS THE DIRECTORY'S CHALLENGE, so it needs the key the
                //     directory just issued. Without it the listing can never be VERIFIED and will
                //     not be shown publicly — registering is a claim, and the challenge is what
                //     turns it into a fact.
                try { VibeLocalSDR.setDirectoryKey(r.optString("key")) } catch (_: Throwable) {}
                listed = true
                r.optInt("pingSec", 0).takeIf { it > 0 }?.let { PING_SEC = it.toLong() }
                address = r.optString("address", "")
                lastError = ""
                // ★★★ PING ONCE, IMMEDIATELY, OR A NEW LISTING IS INVISIBLE FOR FIFTEEN MINUTES.
                //     The directory cannot verify an address at REGISTRATION — the server has no
                //     key until it reads the response — so proving it happens on a ping, and the
                //     next one is a quarter of an hour away. A brand-new server would sit
                //     unverified and unlisted for all of it, which reads as "the switch does not
                //     work" (Stuart, 2026-08-22, asking how long it should take).
                //  ★★ Now that the key is stored AND handed to the shim, one extra request settles
                //     it in a second. It is the ordinary ping — no special path to keep in step.
                //  ★ Failure here costs nothing: the scheduled ping will try again.
                try {
                    val first = post("/api/directory/ping", JSONObject().apply {
                        put("id", r.optString("id")); put("key", r.optString("key"))
                        put("url", url); put("grid", locator)
                        if (shareForSec >= 0) put("shareForSec", shareForSec)
                        put("name", name); put("status", status)
                    })
                    // ★★★ AND CHECK WHAT IT SAID. This threw the answer away, so a REGISTRATION
                    //     whose first challenge failed scheduled nothing and sat unlisted until
                    //     the next interval — which is exactly what happens every time, because
                    //     Cloudflare answers 530 for a hostname that is seconds old and not yet
                    //     routed. Measured 2026-08-22: {"status":530,"reason":"http"} on a tunnel
                    //     that answered 200 a minute later.
                    //  ★★ The retry branch was wired into PING only, and a fresh listing never
                    //     goes through it — turning the switch off clears the stored id, so the
                    //     next enable is a registration. The path a user takes most often was the
                    //     one path that could not recover.
                    if (first == null || !first.optBoolean("verified", false)) {
                        Log.i(TAG, "address not verified yet — retrying shortly")
                        retrySoon()
                    }
                } catch (t: Throwable) {
                    Log.w(TAG, "first ping failed: ${t.message}")
                    retrySoon()
                }
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
    /**
     * Put the listing back after a restart, if it was on.
     *
     * ★★ The PORT is deliberately not remembered — it is whatever the server has just bound, and
     *    reusing yesterday's would tunnel to nothing. Everything else describes the SERVER and is
     *    the same as it was.
     * ★ Silent when listing was off, and silent when it fails: this runs on the boot path and must
     *   never be able to stop a receiver coming up.
     */
    fun restoreIfWanted(ctx: Context, port: Int) {
        try {
            val p = prefs(ctx)
            if (!p.getBoolean(K_WANT, false) || port <= 0) return
            if (running.get()) return
            val name = p.getString(K_NAME, "") ?: ""
            if (name.length < 2) return
            // ★★★ A TEMPORARY SHARE THAT HAS ALREADY ENDED MUST NOT COME BACK. The restore exists
            //     so a listing survives a reboot — but an OFFER that has run out is not something
            //     to survive, and re-listing it would be the server contradicting what its owner
            //     said. The receiver still starts; it simply starts unlisted.
            val until = p.getLong(K_UNTIL, 0L)
            val nowSec = System.currentTimeMillis() / 1000
            if (until > 0 && until <= nowSec) {
                Log.i(TAG, "temporary share ended while we were away — not re-listing")
                p.edit().putBoolean(K_WANT, false).apply()
                return
            }
            // ★ What is LEFT of it, never the original length — see K_UNTIL.
            val remaining = if (until > 0) (until - nowSec) else 0L
            Log.i(TAG, "restoring public listing for \"$name\"")
            applyLoopbackTrust(true, "")
            startTunnel(ctx, port) { url ->
                if (url == null) { Log.w(TAG, "listing not restored: $lastError"); return@startTunnel }
                publish(ctx, name, p.getString(K_GRID, "") ?: "", port,
                        p.getString(K_MODEL, "") ?: "", p.getString(K_DRIVER, "") ?: "",
                        p.getString(K_ANT, "") ?: "", p.getString(K_COV, "") ?: "",
                        p.getBoolean(K_LOCKED, false), remaining)
            }
        } catch (t: Throwable) { Log.w(TAG, "restoreIfWanted failed", t) }
    }

    fun delist(ctx: Context) {
        stopPinging()
        // ★ A share ended by hand has no end left to schedule — and leaving the timer armed would
        //   close a tunnel the owner had since started again.
        expiryTask?.cancel(false); expiryTask = null
        pendingUntil = 0L
        prefs(ctx).edit().putBoolean(K_WANT, false).apply()
        val p = prefs(ctx)
        val id = p.getString(K_ID, null) ?: return
        val key = p.getString(K_KEY, null) ?: return
        post("/api/directory/delist", JSONObject().apply { put("id", id); put("key", key) })
        p.edit().clear().apply()
        listed = false
        address = ""
    }
}
