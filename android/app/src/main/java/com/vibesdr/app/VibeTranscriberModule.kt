package com.vibesdr.app

import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.WritableMap
import com.facebook.react.modules.core.DeviceEventManagerModule
import java.util.concurrent.atomic.AtomicInteger

/*
 * ★★★ ON-DEVICE TRANSCRIPTION + TRANSLATION (V12, experimental) — the Android half of src/services/transcriber.ts.
 *
 *  Listens to WHAT WE PLAY (VibeStreamService's two writer threads hand their PCM to VibeTranscribeTap), so it works
 *  on every backend. Android's on-device SpeechRecognizer takes our audio through a pipe (EXTRA_AUDIO_SOURCE, API 33)
 *  and, on 14+, identifies the spoken language itself (EXTRA_ENABLE_LANGUAGE_DETECTION). Translation is ML Kit, on
 *  the device. Nothing leaves the phone, and there is no fallback to a cloud recogniser.
 *  ★ Android 12 (31-32, Kiko's Galaxy S10): the on-device recogniser exists but there is no API to list or download
 *    its languages, so the user chooses the language (no AUTO, no GET) — see TranscribeEngine's "ANDROID 12" note.
 *
 *  ★★ THIS CLASS NAMES NO android.* TYPE NEWER THAN minSdk (24). RN reflects over every declared method of a module
 *     class, and a newer type in a signature is a NoClassDefFoundError at launch on old phones (Lite 65, 2026-10-14).
 *     Everything from API 31 lives in TranscribeEngine, which is only touched behind an SDK_INT check.
 */
class VibeTranscriberModule(private val ctx: ReactApplicationContext) : ReactContextBaseJavaModule(ctx) {
    override fun getName() = "VibeTranscriber"

    private var engine: Any? = null       // TranscribeEngine on 31+

    fun emit(m: WritableMap) {
        try {
            ctx.getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter::class.java).emit("VibeTranscribe", m)
        } catch (_: Throwable) {}
    }

    fun emit(type: String, text: String, extra: Map<String, String> = emptyMap()) {
        val m = Arguments.createMap()
        m.putString("type", type); m.putString("text", text)
        for ((k, v) in extra) m.putString(k, v)
        emit(m)
    }

    @ReactMethod
    fun capabilities(p: Promise) {
        if (Build.VERSION.SDK_INT < 31) {
            val m = Arguments.createMap()
            m.putBoolean("available", false); m.putString("reason", "Needs Android 12 or later")
            p.resolve(m); return
        }
        TranscribeEngine.capabilities(ctx, p)
    }

    /** language: 'auto', 'auto:<prior order>' or a locale; translateTo: '' = off. (No options since 2026-10-17 night —
     *  LANGUAGES I UNDERSTAND went.) Must match JS startTranscribe's two arguments: the bridge checks the count. */
    @ReactMethod
    fun start(language: String, translateTo: String) {
        if (Build.VERSION.SDK_INT < 31) { emit("error", "Needs Android 12 or later"); return }
        stop()
        val e = TranscribeEngine(ctx, this, language, translateTo)
        engine = e
        e.start()
    }

    /** DOWNLOAD… in the language row — fetch a language's speech model (and translation pack) without listening. */
    @ReactMethod
    fun download(language: String, translateTo: String, p: Promise) {
        if (Build.VERSION.SDK_INT < 31) {
            val m = Arguments.createMap(); m.putBoolean("ok", false); m.putString("text", "Needs Android 12 or later")
            p.resolve(m); return
        }
        TranscribeEngine.download(ctx, language, translateTo, p)
    }

    /** The dial, for SSB auto-tune of the recogniser's feed: 'usb'/'lsb' arm it, any change resets it. */
    @ReactMethod
    fun tuning(mode: String, freqHz: Double) { VibeTranscribeTap.setTuning(mode, freqHz) }

    @ReactMethod
    fun stop() {
        if (Build.VERSION.SDK_INT >= 31) (engine as? TranscribeEngine)?.stop()
        engine = null
    }

    @ReactMethod fun addListener(eventName: String) { /* no-op */ }
    @ReactMethod fun removeListeners(count: Double) { /* no-op */ }
}

/**
 * ★ The recogniser's feed in native code — vibedsp/ssb_offset.h's RecogniserFeed, the SAME implementation iOS uses:
 *   any rate → 16 kHz through a proper anti-aliased polyphase resampler, then SSB auto-tune (USB/LSB only: the voice's
 *   own harmonics say how far the dial is off, and the RECOGNISER's copy is shifted back — never what the listener hears).
 *   ★ The library is the app's own (vibelocalsdr, loaded by the local-radio and affinity code too). If it cannot load,
 *     `ok` is false and the tap falls back to its old linear interpolator — captions still work, just without the two
 *     improvements.
 */
object VibeRecogniserFeed {
    val ok: Boolean = try { System.loadLibrary("vibelocalsdr"); true } catch (_: Throwable) { false }
    @JvmStatic external fun nCreate(): Long
    @JvmStatic external fun nSetTuning(h: Long, sideband: Int, dialHz: Double)
    @JvmStatic external fun nProcess(h: Long, input: FloatArray, n: Int, rate: Int, out: ByteArray): Int
    @JvmStatic external fun nDialCorrection(h: Long): Float
    @JvmStatic external fun nRms(h: Long): Float
    @JvmStatic external fun nStats(h: Long, out: FloatArray): Int
}

/**
 * The audio side. Called on the AudioTrack writer threads: a copy and a hand-off, nothing more — conversion and the
 * pipe write run on our own low-priority thread, and a backlog DROPS (Network > Audio > Spectrum > Decoders).
 */
object VibeTranscribeTap {
    /** (16 kHz mono PCM16 little-endian bytes) — set by the engine; null = off, and feed() costs one volatile read. */
    @Volatile var sink: ((ByteArray) -> Unit)? = null
    /** After every block, on the tap thread: its RMS (0..1), its 16 kHz sample count, and the SSB auto-tune's
     *  correction as a DIAL change in Hz (0 = none). The engine's stall watchdog and the box's note read these. */
    @Volatile var onBlock: ((rms: Float, samples: Int, dialCorrectionHz: Float) -> Unit)? = null
    private val backlog = AtomicInteger(0)
    private val thread by lazy { HandlerThread("vibesdr-transcribe", android.os.Process.THREAD_PRIORITY_BACKGROUND).also { it.start() } }
    private val handler by lazy { Handler(thread.looper) }
    /** One native feed for the process (tap thread only, except setTuning — which only stores). */
    private val feed: Long by lazy { if (VibeRecogniserFeed.ok) try { VibeRecogniserFeed.nCreate() } catch (_: Throwable) { 0L } else 0L }

    /** The dial: mode ('usb'/'lsb' arm the auto-tune; anything else is off) and frequency. Any change resets the
     *  estimate — the old offset belonged to another signal. Any thread. */
    fun setTuning(mode: String, freqHz: Double) {
        val sb = when (mode.lowercase()) { "usb" -> 1; "lsb" -> -1; else -> 0 }
        val h = feed
        if (h != 0L) VibeRecogniserFeed.nSetTuning(h, sb, freqHz)
    }

    /** Last estimate, for the debug log: [offsetHz, confidence, voicedSeconds, passes, estimates, corrections, shiftHz]. */
    fun stats(): FloatArray? {
        val h = feed
        if (h == 0L) return null
        val o = FloatArray(7)
        return if (VibeRecogniserFeed.nStats(h, o) == 7) o else null
    }

    // Fallback only (native library missing): the old continuous-phase linear resampler. Tap thread only.
    private var srcRate = 0
    private var phase = 0.0
    private var last = 0f

    fun reset() { srcRate = 0; phase = 0.0; last = 0f }

    /** Interleaved int16 samples. `silent`: squelch closed — the transcriber hears what the listener hears. */
    fun feed(pcm: ShortArray, len: Int, rate: Int, ch: Int, silent: Boolean) {
        sink ?: return
        if (backlog.get() > 40 || rate <= 0 || ch <= 0) return
        val frames = len / ch
        val mono = FloatArray(frames)
        if (!silent) {
            if (ch == 1) for (i in 0 until frames) mono[i] = pcm[i] / 32768f
            else for (i in 0 until frames) mono[i] = (pcm[i * ch] + pcm[i * ch + 1]) / 65536f
        }
        post(mono, rate)
    }

    fun feedBytes(pcm: ByteArray, len: Int, rate: Int, ch: Int, silent: Boolean) {
        sink ?: return
        if (backlog.get() > 40 || rate <= 0 || ch <= 0) return
        val frames = len / 2 / ch
        val mono = FloatArray(frames)
        if (!silent) {
            fun s(i: Int): Int = ((pcm[i * 2 + 1].toInt() shl 8) or (pcm[i * 2].toInt() and 0xff)).toShort().toInt()
            if (ch == 1) for (i in 0 until frames) mono[i] = s(i) / 32768f
            else for (i in 0 until frames) mono[i] = (s(i * ch) + s(i * ch + 1)) / 65536f
        }
        post(mono, rate)
    }

    private fun post(mono: FloatArray, rate: Int) {
        backlog.incrementAndGet()
        handler.post {
            try {
                val h = feed
                if (h != 0L) {
                    // ★ 16 kHz out = n·16000/rate + a little; the native side refuses a short buffer rather than overrun.
                    val out = ByteArray(((mono.size.toLong() * 16000 / rate).toInt() + 8) * 2)
                    val nb = VibeRecogniserFeed.nProcess(h, mono, mono.size, rate, out)
                    if (nb > 0) sink?.invoke(if (nb == out.size) out else out.copyOf(nb))
                    onBlock?.invoke(VibeRecogniserFeed.nRms(h), nb / 2, VibeRecogniserFeed.nDialCorrection(h))
                } else {
                    val out = resample16k(mono, rate)
                    sink?.invoke(out)
                    var e = 0.0
                    for (v in mono) e += (v * v).toDouble()
                    onBlock?.invoke(if (mono.isEmpty()) 0f else Math.sqrt(e / mono.size).toFloat(), out.size / 2, 0f)
                }
            } catch (_: Throwable) {
            } finally { backlog.decrementAndGet() }
        }
    }

    /** Fallback only — no anti-alias filter (a wideband feed folds its top octave into the speech band). */
    private fun resample16k(x: FloatArray, rate: Int): ByteArray {
        if (rate != srcRate) { srcRate = rate; phase = 0.0; last = 0f }
        val step = rate / 16000.0
        val out = ArrayList<Short>(((x.size / step) + 2).toInt())
        // Sample positions are measured from `last` (index -1) through x[0..n-1].
        var p = phase
        while (p < x.size - 1 + 1e-9) {
            val i = Math.floor(p).toInt()
            val f = (p - i).toFloat()
            val a = if (i < 0) last else x[i]
            val b = if (i + 1 < 0) last else x[minOf(i + 1, x.size - 1)]
            val v = (a + (b - a) * f).coerceIn(-1f, 1f)
            out.add((v * 32767f).toInt().toShort())
            p += step
        }
        phase = p - x.size
        if (x.isNotEmpty()) last = x[x.size - 1]
        val bytes = ByteArray(out.size * 2)
        for (k in out.indices) {
            val s = out[k].toInt()
            bytes[k * 2] = (s and 0xff).toByte(); bytes[k * 2 + 1] = ((s shr 8) and 0xff).toByte()
        }
        return bytes
    }
}
