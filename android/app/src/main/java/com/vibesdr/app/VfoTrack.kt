package com.vibesdr.app

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import android.util.Log
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Executors
import kotlin.math.min

/**
 * VfoTrack — ★★★ V12 PER-VFO RECORDING (Stuart, 2026-11-07: "when the A/B VFO's are active there needs to be a 2nd record
 * line in the audio menu for the B VFO, both can be triggered independently and both have their own clocks and the signal
 * is recorded as it should be before we push it to one ear so Stereo for an FM signal and mono for others").
 *
 * ONE VFO's own audio, as the server sends it to a listener recording it — FORMAT 5: [0] channels · [1] 5 · [2..5] 48000 ·
 * [6] the VFO (0 A, 1 B) · [7] 3 Opus / 0 int16 PCM · payload (local_sdr_shim.cpp watchSendRec). Never played: the
 * listener hears the server's mix exactly as before. Its own Opus decoder (VibeLocalSDR.opusDecodeVfo — the main stream's
 * decoder carries that stream's state), its own record buffer and its own AAC file, all on its own thread: the socket's
 * reader only hands the bytes over (Network > Audio > Spectrum > Decoders). The iOS twin is VibeVfoTrack in
 * ios/VibeSDR/VibePowerModule.swift.
 * ★ The file keeps the VFO's channels as they are when it starts — mono stays mono, stereo stays stereo; a change midway
 *   is folded or doubled to fit (an m4a cannot change its channel count).
 */
class VfoTrack private constructor(val vfo: Int) {
    companion object {
        private const val TAG = "VfoTrack"
        private const val RATE = 48_000
        val tracks = arrayOf(VfoTrack(0), VfoTrack(1))
    }
    private val exec = Executors.newSingleThreadExecutor { r -> Thread(r, "vibe-vforec$vfo") }
    private val ring = RecordBufferRing()
    private var enc: MediaCodec? = null
    private var muxer: MediaMuxer? = null
    private var track = -1
    private var muxStarted = false
    private var ptsUs = 0L
    private var fileCh = 1
    private var path = ""
    @Volatile private var lastCh = 1
    @Volatile private var recording = false
    private val name get() = if (vfo == 0) "A" else "B"

    /** The socket's reader thread: hand over, nothing more. */
    fun feed(b: ByteArray) {
        if (!recording && !ring.isOn) return
        val copy = b.copyOf()
        exec.execute { decode(copy) }
    }

    private fun decode(b: ByteArray) {
        if (b.size <= 8) return
        val ch = if ((b[0].toInt() and 0xFF) == 2) 2 else 1
        val pcm: ShortArray = when (b[7].toInt() and 0xFF) {
            0 -> { val n = (b.size - 8) / 2; ShortArray(n).also { ByteBuffer.wrap(b, 8, n * 2).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().get(it) } }
            3 -> VibeLocalSDR.opusDecodeVfo(vfo, b.copyOfRange(8, b.size), RATE, ch) ?: return
            else -> return
        }
        lastCh = ch
        if (ring.isOn) ring.append(pcm.copyOf(), ch, RATE)
        if (recording) encode(pcm, ch)
    }

    /** exec only: into the file at ITS channel count. */
    private fun encode(src: ShortArray, srcCh: Int) {
        val e = enc ?: return
        val frames = src.size / srcCh
        val out = ShortArray(frames * fileCh)
        for (i in 0 until frames) {
            if (fileCh == 2) {
                out[i * 2] = src[i * srcCh]; out[i * 2 + 1] = src[i * srcCh + srcCh - 1]
            } else out[i] = if (srcCh == 2) ((src[i * 2].toInt() + src[i * 2 + 1].toInt()) / 2).toShort() else src[i]
        }
        val data = ByteArray(out.size * 2)
        ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().put(out)
        var off = 0
        try {
            while (off < data.size) {
                val idx = e.dequeueInputBuffer(10_000)
                if (idx < 0) { drain(false); continue }
                val ib = e.getInputBuffer(idx) ?: break
                ib.clear()
                val n = min(ib.capacity(), data.size - off)
                ib.put(data, off, n)
                e.queueInputBuffer(idx, 0, n, ptsUs, 0)
                ptsUs += (n / (2L * fileCh)) * 1_000_000L / RATE
                off += n
            }
            drain(false)
        } catch (x: Exception) {
            Log.e(TAG, "VFO $name encode failed: ${x.message}")
            recording = false
            release()
        }
    }

    private fun drain(eos: Boolean) {
        val e = enc ?: return
        val m = muxer ?: return
        val info = MediaCodec.BufferInfo()
        while (true) {
            val i = e.dequeueOutputBuffer(info, if (eos) 50_000 else 0)
            when {
                i == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> { track = m.addTrack(e.outputFormat); m.start(); muxStarted = true }
                i >= 0 -> {
                    val ob = e.getOutputBuffer(i)
                    if (ob != null && info.size > 0 && muxStarted && (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0)
                        m.writeSampleData(track, ob, info)
                    e.releaseOutputBuffer(i, false)
                    if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) return
                }
                else -> return
            }
        }
    }

    private fun release() {
        try { enc?.stop() } catch (_: Exception) {}
        try { enc?.release() } catch (_: Exception) {}
        try { if (muxStarted) muxer?.stop() } catch (_: Exception) {}
        try { muxer?.release() } catch (_: Exception) {}
        enc = null; muxer = null; track = -1; muxStarted = false
    }

    fun start(dir: java.io.File, freqHz: Double, mode: String, promise: Promise) {
        exec.execute {
            if (recording) { promise.resolve(Arguments.createMap().apply { putString("path", path); putDouble("bufferedSeconds", 0.0) }); return@execute }
            try {
                val prefix = if (ring.isOn) ring.snapshot() else emptyList()
                val prefixSec = prefix.sumOf { it.micros } / 1e6
                fileCh = if (lastCh == 2 || prefix.any { it.channels == 2 }) 2 else 1
                val ts = java.text.SimpleDateFormat("yyyy-MM-dd'T'HH-mm-ss", java.util.Locale.US).format(java.util.Date())
                val mhz = String.format(java.util.Locale.US, "%.4fMHz", freqHz / 1e6)
                val f = java.io.File(dir, "VibeSDR_${ts}_${mhz}_${mode.uppercase()}_VFO-$name.m4a")
                val fmt = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, RATE, fileCh)
                fmt.setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
                fmt.setInteger(MediaFormat.KEY_BIT_RATE, if (fileCh == 2) 128_000 else 64_000)
                fmt.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 65_536)
                val e = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
                e.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                e.start()
                enc = e
                muxer = MediaMuxer(f.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
                track = -1; muxStarted = false; ptsUs = 0; path = f.absolutePath
                recording = true
                Log.i(TAG, "VFO $name recording → ${f.name} ($fileCh ch, ${"%.1f".format(prefixSec)} s from the record buffer)")
                promise.resolve(Arguments.createMap().apply { putString("path", f.absolutePath); putDouble("bufferedSeconds", prefixSec) })
                for (c in prefix) encode(c.samples, c.channels)
            } catch (x: Exception) {
                Log.e(TAG, "VFO $name start failed: ${x.message}")
                recording = false
                release()
                promise.reject("rec_open", x.message)
            }
        }
    }

    fun stop(promise: Promise) {
        exec.execute {
            val p = path
            try {
                val e = enc
                if (e != null) {
                    val idx = e.dequeueInputBuffer(50_000)
                    if (idx >= 0) e.queueInputBuffer(idx, 0, 0, ptsUs, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                    drain(true)
                }
            } catch (x: Exception) { Log.e(TAG, "VFO $name stop: ${x.message}") }
            recording = false
            release()
            path = ""
            ring.clear()   // ★ the next recording never repeats this one (the main recorder's rule)
            Log.i(TAG, "VFO $name recording stopped: $p")
            promise.resolve(if (p.isEmpty()) null else p)
        }
    }

    fun setBuffer(seconds: Int) { val s = seconds.coerceIn(0, 120); if (ring.capacitySeconds != s) ring.setCapacity(s) }
    fun clearBuffer() { ring.clear() }
}
