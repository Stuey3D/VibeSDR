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
 *
 *  ★★ THIS CLASS NAMES NO android.* TYPE NEWER THAN minSdk (24). RN reflects over every declared method of a module
 *     class, and a newer type in a signature is a NoClassDefFoundError at launch on old phones (Lite 65, 2026-10-14).
 *     Everything from API 33 lives in TranscribeEngine33, which is only touched behind an SDK_INT check.
 */
class VibeTranscriberModule(private val ctx: ReactApplicationContext) : ReactContextBaseJavaModule(ctx) {
    override fun getName() = "VibeTranscriber"

    private var engine: Any? = null       // TranscribeEngine33 on 33+

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
        if (Build.VERSION.SDK_INT < 33) {
            val m = Arguments.createMap()
            m.putBoolean("available", false); m.putString("reason", "Needs Android 13 or later")
            p.resolve(m); return
        }
        TranscribeEngine33.capabilities(ctx, p)
    }

    @ReactMethod
    fun start(language: String, translateTo: String) {
        if (Build.VERSION.SDK_INT < 33) { emit("error", "Needs Android 13 or later"); return }
        stop()
        val e = TranscribeEngine33(ctx, this, language, translateTo)
        engine = e
        e.start()
    }

    @ReactMethod
    fun stop() {
        if (Build.VERSION.SDK_INT >= 33) (engine as? TranscribeEngine33)?.stop()
        engine = null
    }

    @ReactMethod fun addListener(eventName: String) { /* no-op */ }
    @ReactMethod fun removeListeners(count: Double) { /* no-op */ }
}

/**
 * The audio side. Called on the AudioTrack writer threads: a copy and a hand-off, nothing more — conversion and the
 * pipe write run on our own low-priority thread, and a backlog DROPS (Network > Audio > Spectrum > Decoders).
 */
object VibeTranscribeTap {
    /** (16 kHz mono PCM16 little-endian bytes) — set by the engine; null = off, and feed() costs one volatile read. */
    @Volatile var sink: ((ByteArray) -> Unit)? = null
    private val backlog = AtomicInteger(0)
    private val thread by lazy { HandlerThread("vibesdr-transcribe", android.os.Process.THREAD_PRIORITY_BACKGROUND).also { it.start() } }
    private val handler by lazy { Handler(thread.looper) }

    // Continuous-phase linear resampler state (tap thread only) — per-packet resampling loses samples at the joins.
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
                val out = resample16k(mono, rate)
                sink?.invoke(out)
            } catch (_: Throwable) {
            } finally { backlog.decrementAndGet() }
        }
    }

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
