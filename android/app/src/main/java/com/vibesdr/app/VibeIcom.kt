package com.vibesdr.app

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.util.Log
import org.json.JSONObject

/**
 * ★★★ AN ICOM TRANSCEIVER AS A RECEIVER, ON ANDROID — RECEIVE ONLY (docs/ICOM-SUPPORT-PLAN.md Phase 4, 2026-10-22).
 * Shared by the main app's server mode and VibeServer Lite (Lite's SHARED list).
 *
 * The engine is the same as on a Pi (icom_source.cpp): CI-V through the whole-frame allow-list, the scope as the
 * waterfall, the radio's audio in after the demodulator. What only Java can do lives here:
 *  1. WHICH DEVICE IS AN ICOM. The IC-7300's CI-V port is a Silicon Labs CP2102 (10c4:ea60) — the same chip as
 *     half the GPS receivers, ESP32 boards and programming leads in the world. The radio programs its MODEL into the
 *     chip's serial ("IC-7300 03001234"), and that is the only thing that tells it apart. On Android 10+ the serial
 *     is not readable until the owner has granted the device, so a CP2102 is a CANDIDATE until then — and is only
 *     ever opened on the owner's Start (which asks for the device), never at app start, never on a plug-in.
 *     The engine looks again before it claims or writes a byte (icom_transport.cpp, "fd:").
 *  2. ★★ NOT IN THE DEVICE FILTER — decided 2026-10-22. A filter entry is the only way Android offers "open VibeSDR
 *     when this is plugged in", and Android's filter can match a serial only EXACTLY (one radio's own number), not
 *     "starts with IC-7300". 10c4:ea60 alone would offer this app for every CP2102 gadget an owner plugs in, which
 *     is the wrong side of the trade. Icom's own VID (0x0c26, native-USB 705/9700/MK2) would be specific, but no
 *     native-USB model is supported yet (icom_models.h `known`), and offering the app for a radio it then refuses
 *     is a control that does not work (AGENTS.md). So: neither filter has any Icom id, and
 *     scripts/check-device-filter.mjs holds that. The cost: no "use by default" tick, so after a re-plug or reboot
 *     the owner taps Allow once more — recorded in the plan's Open for Stuart.
 *  3. ITS AUDIO. snd-usb-audio owns the radio's USB codec, so the engine cannot open it. [IcomAudio] records it with
 *     AudioRecord on the radio's TYPE_USB_DEVICE input (setPreferredDevice), 48 kHz mono PCM16, and pushes every
 *     10 ms block into the engine's "push" source. ★★★ IT NEVER STREAMS THE PHONE'S OWN MICROPHONE: if Android
 *     routes the recording anywhere but the USB input it picked, capture stops and the setup screen says why.
 *     CAPTURE ONLY — nothing here opens the radio's codec for playback (that is its MOD input).
 *  4. THE MICROPHONE PERMISSION — asked ONLY when an Icom is actually being served, with a plain explanation first;
 *     never at app start. Refused = the dial and waterfall still work, and the audio's absence is said in words.
 *  5. ANDROID 14+ MICROPHONE FOREGROUND SERVICE. Background capture needs the server's foreground service to carry
 *     the `microphone` type, which Android only grants while the app is in front (RtlTcpServerService asks for it
 *     on its tick whenever an Icom wants audio and the app is visible). A server restored in the background (crash,
 *     update) therefore serves dial + waterfall and says "open VibeSDR once to give the server the radio's audio".
 *  ★ Idle cost: nothing until an Icom is started — no library load, no thread, no receiver.
 */
object VibeIcom {
    private const val TAG = "VibeIcom"
    const val SILABS_VID = 0x10c4
    /** ★ CP2102/CP2109 (the 7300's) and the dual CP2105. Mirrors icom_transport.cpp and LocalSdrShim::start. */
    val CP210X_PIDS = intArrayOf(0xea60, 0xea70)
    /** ★ Icom's own USB vendor id — native-USB radios (705/9700/7300MK2) [unverified per model]. */
    const val ICOM_VID = 0x0c26
    /** ★★ Mirrors the `known = true` rows of android/app/src/main/cpp/icom_models.h — scripts/check-device-filter.mjs
     *  compares them. A model NOT here is named and refused by the engine; it is never offered as servable. */
    val KNOWN_MODELS = arrayOf("IC-7300")

    /** A CP210x or an Icom-VID device: MAYBE an Icom. Nothing is opened to say so. */
    fun isCandidate(vid: Int, pid: Int): Boolean =
        (vid == SILABS_VID && CP210X_PIDS.contains(pid)) || vid == ICOM_VID
    fun isCandidate(dev: UsbDevice): Boolean = isCandidate(dev.vendorId, dev.productId)

    /** The longest known model the text STARTS with, ending at a word boundary — "IC-7300 0300…" is an IC-7300,
     *  "IC-7300MK2 …" is not (the same rule as icom_models.h modelBySerial). */
    fun modelFromText(text: String?): String? {
        val t = text?.trim() ?: return null
        var best: String? = null
        for (m in KNOWN_MODELS) {
            if (!t.startsWith(m)) continue
            val next = if (t.length > m.length) t[m.length] else ' '
            if ((next == ' ' || next == '_') && (best == null || m.length > best.length)) best = m
        }
        return best
    }

    /** The device's serial, or null where Android will not say (10+ without the owner's grant). */
    fun serialOf(dev: UsbDevice): String? = try { dev.serialNumber } catch (_: SecurityException) { null }

    /** ★ The known Icom model this device IS, or null. The product string first (readable without a grant — and
     *  some radios name themselves there [unverified on a 7300]), then the serial. */
    fun identify(dev: UsbDevice): String? {
        if (!isCandidate(dev)) return null
        modelFromText(dev.productName)?.let { return it }
        return modelFromText(serialOf(dev))
    }
    fun isIcom(dev: UsbDevice): Boolean = identify(dev) != null

    /** A candidate Android will not identify until the owner grants it (10+): the server screen can say "an Icom?
     *  press Start and allow it" instead of nothing. */
    fun unconfirmed(mgr: UsbManager, dev: UsbDevice): Boolean =
        isCandidate(dev) && identify(dev) == null && !mgr.hasPermission(dev) && serialOf(dev) == null

    /** Is the running server an Icom? From the engine itself (its hwinfo radio), so it cannot drift. */
    fun serving(): Boolean = try { VibeLocalSDR.icomFacts().isNotEmpty() } catch (_: Throwable) { false }

    // ── Starting one ────────────────────────────────────────────────────────────────────────────────────────────

    /** The owner's Icom settings → the engine, before the ordinary start(fd, vid, pid). Same names as the Linux
     *  server's config (vibeserver_config.h): icomBaud, icomCiv, icomScopeSpeed, icomOwnerLock, icomRfOpen. */
    fun prepareStart(cfg: JSONObject, dev: UsbDevice) {
        VibeLocalSDR.setIcomNext(
            cfg.optInt("icomBaud", 115200),
            cfg.optInt("icomCiv", 0),
            cfg.optBoolean("icomOwnerLock", true),     // ★ DEFAULT ON — "Listeners follow the radio" (plan §3.4)
            cfg.optBoolean("icomRfOpen", false),
            cfg.optInt("icomScopeSpeed", 0),
            serialOf(dev) ?: dev.productName ?: "")
        Log.i(TAG, "starting ${identify(dev) ?: "an Icom"} at ${dev.deviceName} (receive only)")
    }

    /** The server is up with an Icom: start its audio if Android lets us now, else say what is missing. */
    fun onServerStarted(ctx: Context) {
        if (!serving()) return
        IcomAudio.startIfPossible(ctx)
    }

    /** Before the engine stops: the AudioRecord thread must not push into a source that is going away. */
    fun onServerStopping() { IcomAudio.stop() }

    // ── The microphone permission — only when an Icom is served ────────────────────────────────────────────────

    fun hasMicPermission(ctx: Context): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.M ||
            ctx.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    @Volatile private var asking = false
    private const val REQ_MIC = 7300

    /**
     * ★★ Ask for RECORD_AUDIO with a plain explanation FIRST — called by the start path once an Icom server is up and
     * the permission is missing. Never at app start, never for any other radio. The answer starts the audio (and the
     * service's microphone type); a refusal is reported, and asked again only on the owner's next Start.
     */
    fun askForMicIfNeeded(activity: Activity?, ctx: Context) {
        if (hasMicPermission(ctx) || asking) return
        val act = activity ?: run { IcomAudio.report(ctx, problem = NEEDS_PERMISSION); return }
        val pa = act as? com.facebook.react.modules.core.PermissionAwareActivity
            ?: run { IcomAudio.report(ctx, problem = NEEDS_PERMISSION); return }
        asking = true
        act.runOnUiThread {
            try {
                AlertDialog.Builder(act)
                    .setTitle("The radio's audio")
                    .setMessage("Your Icom sends its received audio to this phone through USB, and Android treats any " +
                        "audio input as a microphone. VibeSDR needs the microphone permission to pass the radio's " +
                        "audio on to your listeners.\n\nIt records ONLY the radio's USB audio — never this phone's own " +
                        "microphone — and only while you are serving the radio. Nothing is ever sent TO the radio.")
                    .setPositiveButton("Continue") { _, _ ->
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                            pa.requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQ_MIC) { code, _, results ->
                                if (code != REQ_MIC) return@requestPermissions false
                                asking = false
                                if (results.isNotEmpty() && results[0] == PackageManager.PERMISSION_GRANTED) {
                                    Log.i(TAG, "microphone permission granted — starting the radio's audio")
                                    RtlTcpServerService.wantMicrophone(ctx)
                                    IcomAudio.startIfPossible(ctx)
                                } else {
                                    IcomAudio.report(ctx, problem = PERMISSION_REFUSED)
                                }
                                true
                            }
                        } else asking = false
                    }
                    .setNegativeButton("Not now") { _, _ -> asking = false; IcomAudio.report(ctx, problem = PERMISSION_REFUSED) }
                    .setOnCancelListener { asking = false; IcomAudio.report(ctx, problem = NEEDS_PERMISSION) }
                    .show()
            } catch (t: Throwable) {
                asking = false
                Log.w(TAG, "could not ask for the microphone permission: $t")
                IcomAudio.report(ctx, problem = NEEDS_PERMISSION)
            }
        }
    }

    const val NEEDS_PERMISSION = "VibeSDR needs the microphone permission to pass the radio's audio on — press Start again to be asked"
    const val PERMISSION_REFUSED = "the microphone permission was refused, so listeners get the dial and waterfall but no audio — allow it in Android's Settings > Apps > VibeSDR > Permissions"
    const val OPEN_APP_FOR_AUDIO = "Android lets a server record only once its app has been opened — open VibeSDR once to give the server the radio's audio"

    /**
     * The setup facts the server screen shows for an Icom: the engine's own hwinfo `radio` object (model, CI-V
     * address, baud, transport, preflight with each menu item and where it lives, audio and its problem) plus what only
     * Android knows (the microphone permission, the USB input picked, whether capture is running). `{}` when no Icom
     * server is running. ★ Read by VibeLocalSdrModule.icomSetup for the shared TS setup card.
     */
    fun facts(ctx: Context): JSONObject {
        val out = JSONObject()
        val radio = try { VibeLocalSDR.icomFacts() } catch (_: Throwable) { "" }
        if (radio.isNotEmpty()) try { out.put("radio", JSONObject(radio)) } catch (_: Throwable) {}
        out.put("micPermission", hasMicPermission(ctx))
        out.put("audioRunning", IcomAudio.running)
        out.put("audioDevice", IcomAudio.deviceLabel)
        out.put("audioProblem", IcomAudio.lastProblem)
        out.put("audioSupported", Build.VERSION.SDK_INT >= Build.VERSION_CODES.N)
        return out
    }
}

/**
 * ★★★ THE RADIO'S AUDIO, RECORDED BY ANDROID — see VibeIcom point 3. Its own object so the API 23/24 classes it names
 * (AudioDeviceInfo, setPreferredDevice, getRoutedDevice) are only ever loaded on a phone that has them, and never
 * appear in a React module's signatures (scripts/check-lite-api.py — Lite's minSdk is 21).
 * ★★ API 24+ (Android 7.0) ONLY: before it there is no getRoutedDevice, so we could not PROVE the recording comes from
 *    the radio rather than the phone's microphone — and broadcasting the wrong one is the failure this must never have.
 *    Below 24 the Icom serves dial + waterfall and says so.
 * ★ One thread, URGENT_AUDIO, a direct buffer allocated once: the AudioRecord thread never allocates.
 */
object IcomAudio {
    private const val TAG = "VibeIcomAudio"
    private const val RATE = 48000
    private const val BLOCK = 480                     // 10 ms — the block every icom audio source delivers

    @Volatile var running = false; private set
    @Volatile var deviceLabel = ""; private set
    @Volatile var lastProblem = ""; private set
    @Volatile private var thread: Thread? = null
    @Volatile private var stopFlag = false

    /** Tell the engine (hwinfo `audio` / `audioProblem`) and remember for the setup screen. */
    fun report(ctx: Context?, device: String = deviceLabel, problem: String) {
        deviceLabel = device; lastProblem = problem
        try { VibeLocalSDR.icomAudioState(if (device.isEmpty()) "Android USB audio" else device, problem) } catch (_: Throwable) {}
        if (problem.isNotEmpty()) Log.w(TAG, "radio audio: $problem")
    }

    @Synchronized
    fun startIfPossible(ctx: Context) {
        if (running) return
        if (!VibeLocalSDR.icomAudioWanted()) return                        // ★ no Icom served: nothing to record for
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N) {
            report(ctx, problem = "this Android version cannot prove which input it records from, so the radio's audio is not served (Android 7.0 or later needed)")
            return
        }
        if (!VibeIcom.hasMicPermission(ctx)) { report(ctx, problem = VibeIcom.NEEDS_PERMISSION); return }
        if (!RtlTcpServerService.mayRecordNow(ctx)) { report(ctx, problem = VibeIcom.OPEN_APP_FOR_AUDIO); return }
        Api24.start(ctx)
    }

    @Synchronized
    fun stop() {
        stopFlag = true
        val t = thread
        thread = null
        if (t != null) try { t.join(2000) } catch (_: InterruptedException) {}
        running = false
    }

    @androidx.annotation.RequiresApi(Build.VERSION_CODES.N)
    private object Api24 {
        /** ★ The radio's codec: a USB input. A 7300's TI PCM2900 calls itself "USB Audio CODEC" [unverified on Android];
         *  a native-USB Icom names its model. With exactly one USB input, that is it. With several and none named,
         *  we do not guess — the setup screen says to unplug the others. */
        fun pickInput(ctx: Context): android.media.AudioDeviceInfo? {
            val am = ctx.getSystemService(Context.AUDIO_SERVICE) as android.media.AudioManager
            val usb = am.getDevices(android.media.AudioManager.GET_DEVICES_INPUTS).filter {
                it.type == android.media.AudioDeviceInfo.TYPE_USB_DEVICE || it.type == android.media.AudioDeviceInfo.TYPE_USB_HEADSET
            }
            val named = usb.firstOrNull { d ->
                val n = d.productName?.toString() ?: ""
                n.contains("USB Audio CODEC", ignoreCase = true) || VibeIcom.KNOWN_MODELS.any { n.contains(it) }
            }
            return named ?: usb.singleOrNull()
        }

        fun start(ctx: Context) {
            val dev = pickInput(ctx) ?: run {
                report(ctx, device = "", problem = "Android shows no USB audio input from the radio — check its USB cable, and that the phone supports USB audio")
                return
            }
            val label = "${dev.productName} (USB)"
            val am = ctx.getSystemService(Context.AUDIO_SERVICE) as android.media.AudioManager
            // ★ UNPROCESSED where the phone has it (no AGC, no noise suppression — the radio has done its own), else
            //   VOICE_RECOGNITION, which the CDD requires to be free of AGC/NS by default. Never MIC: it may be processed.
            val source = if (am.getProperty(android.media.AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED) == "true")
                android.media.MediaRecorder.AudioSource.UNPROCESSED else android.media.MediaRecorder.AudioSource.VOICE_RECOGNITION
            val min = android.media.AudioRecord.getMinBufferSize(RATE, android.media.AudioFormat.CHANNEL_IN_MONO,
                                                                 android.media.AudioFormat.ENCODING_PCM_16BIT)
            if (min <= 0) { report(ctx, device = label, problem = "this phone cannot record 48 kHz audio from USB"); return }
            val rec = try {
                @Suppress("MissingPermission")
                android.media.AudioRecord(source, RATE, android.media.AudioFormat.CHANNEL_IN_MONO,
                                          android.media.AudioFormat.ENCODING_PCM_16BIT, maxOf(min * 2, BLOCK * 2 * 8))
            } catch (t: Throwable) { report(ctx, device = label, problem = "Android refused to open the radio's audio ($t)"); return }
            if (rec.state != android.media.AudioRecord.STATE_INITIALIZED) {
                rec.release(); report(ctx, device = label, problem = "Android could not open the radio's audio input"); return
            }
            rec.preferredDevice = dev
            try { rec.startRecording() } catch (t: Throwable) {
                rec.release(); report(ctx, device = label, problem = "Android would not start recording the radio ($t)"); return
            }
            stopFlag = false
            running = true
            report(ctx, device = label, problem = "")
            val t = Thread({ loop(ctx, rec, dev.id, label) }, "vibe-icom-audio")
            thread = t
            t.start()
            Log.i(TAG, "recording the radio's audio from $label (source $source, buffer ${maxOf(min * 2, BLOCK * 16)} B)")
        }

        private fun loop(ctx: Context, rec: android.media.AudioRecord, wantId: Int, label: String) {
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
            val buf = java.nio.ByteBuffer.allocateDirect(BLOCK * 2).order(java.nio.ByteOrder.nativeOrder())
            var checkAt = 0L
            var why = ""
            try {
                while (!stopFlag) {
                    // ★★★ ONCE A SECOND: is Android still recording from the RADIO? The first check runs before a single
                    //     block is pushed. Anything else — the phone's own microphone after the codec dropped off the
                    //     bus, a headset someone plugged in — stops capture: listeners must never hear this room.
                    val now = android.os.SystemClock.elapsedRealtime()
                    if (now >= checkAt) {
                        checkAt = now + 1000
                        val routed = rec.routedDevice
                        if (routed == null || routed.id != wantId) {
                            why = "Android moved the recording off the radio's USB audio (to ${routed?.productName ?: "nothing"}), so it was stopped — the radio's audio cable may have been unplugged"
                            break
                        }
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && rec.activeRecordingConfiguration?.isClientSilenced == true) {
                            if (lastProblem.isEmpty()) report(ctx, device = label, problem = VibeIcom.OPEN_APP_FOR_AUDIO)
                        } else if (lastProblem == VibeIcom.OPEN_APP_FOR_AUDIO) report(ctx, device = label, problem = "")
                    }
                    buf.clear()
                    val n = rec.read(buf, BLOCK * 2, android.media.AudioRecord.READ_BLOCKING)
                    if (n < 0) { why = "the radio's audio stopped (AudioRecord error $n)"; break }
                    if (n >= 2) VibeLocalSDR.icomPushPcm(buf, n / 2, 1, RATE)
                }
            } catch (t: Throwable) {
                why = "the radio's audio stopped ($t)"
            } finally {
                try { rec.stop() } catch (_: Throwable) {}
                rec.release()
                running = false
                if (why.isNotEmpty()) report(ctx, device = label, problem = why)
                Log.i(TAG, "radio audio capture ended${if (why.isNotEmpty()) ": $why" else ""}")
            }
        }
    }
}
