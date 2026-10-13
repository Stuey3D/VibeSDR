package com.vibesdr.app

import kotlin.math.PI
import kotlin.math.ceil
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.min
import kotlin.math.sin

/**
 * ★ Streaming sample-rate converter for RECORDINGS: any source rate → one fixed output rate.
 *
 * Why it exists: Android's AAC encoder only takes the standard AAC rates (8k…48k). A KiwiSDR sends
 * 12001 Hz (its true fractional rate, rounded) or 20250 Hz (wideband Kiwis), neither of which is one —
 * the encoder ignored the rate it was given and kept its default 44.1 kHz, so a Kiwi recording played
 * back ~2–4× fast, in the app and everywhere else (2026-10-13, 20250 Hz → 44100 Hz header). iOS never
 * had this: it records the engine's fixed 48 kHz after its own converter. Android now does the same —
 * every recording is 48 kHz whatever the source, so a rate that changes mid-recording (a Kiwi
 * reconnect, an OWRX mode change) is converted too instead of being written at the wrong speed.
 *
 * Windowed-sinc (Blackman, 16 zero-crossings a side) from a precomputed table, so it is a handful of
 * multiply-adds per output sample — cheap enough for the 1 GB phones. The low-pass sits at the lower of
 * the two Nyquists, so upsampling adds no images and downsampling folds nothing down.
 */
class RecResampler(private val dstRate: Int) {
    private var srcRate = 0
    private var ch = 1
    private var step = 1.0          // input frames advanced per output frame
    private var fc = 1.0            // cut-off as a fraction of the INPUT Nyquist
    private var halfW = 0           // kernel half-width in input frames
    private var buf = FloatArray(0) // interleaved input frames still needed
    private var frames = 0          // frames held in buf
    private var pos = 0.0           // next output's position in buf, in input frames

    /** 16-bit LE interleaved PCM at [rate] → 16-bit LE interleaved PCM at dstRate (same channels). */
    fun process(pcm: ByteArray, rate: Int, channels: Int): ByteArray {
        if (rate == dstRate) { reset(); return pcm }
        if (rate != srcRate || channels != ch) configure(rate, channels)

        // Append the new input after what is held from last time.
        val inFrames = pcm.size / (2 * ch)
        val need = (frames + inFrames) * ch
        if (buf.size < need) buf = buf.copyOf(need + need / 2)
        var bi = frames * ch
        val inBytes = inFrames * ch * 2
        var i = 0
        while (i < inBytes) {
            buf[bi++] = ((pcm[i + 1].toInt() shl 8) or (pcm[i].toInt() and 0xFF)).toShort().toFloat()
            i += 2
        }
        frames += inFrames

        // Produce every output whose kernel lies wholly inside what is held.
        val outMax = ((frames - halfW - pos) / step).toInt().coerceAtLeast(0) + 1
        val out = ByteArray(outMax * ch * 2)
        var oi = 0
        while (pos + halfW < frames) {
            val centre = floor(pos).toInt()
            val frac = pos - centre
            for (c in 0 until ch) {
                var acc = 0.0
                var k = -halfW + 1
                while (k <= halfW) {
                    val idx = centre + k
                    if (idx >= 0) acc += buf[idx * ch + c] * kernel((k - frac) * fc)
                    k++
                }
                val s = (acc * fc).toInt().coerceIn(-32768, 32767)
                out[oi++] = (s and 0xFF).toByte(); out[oi++] = ((s shr 8) and 0xFF).toByte()
            }
            pos += step
        }

        // Keep the frames the next outputs still reach back to.
        val drop = (floor(pos).toInt() - halfW).coerceIn(0, frames)
        if (drop > 0) {
            System.arraycopy(buf, drop * ch, buf, 0, (frames - drop) * ch)
            frames -= drop
            pos -= drop
        }
        return if (oi == out.size) out else out.copyOf(oi)
    }

    fun reset() { srcRate = 0; frames = 0; pos = 0.0 }

    private fun configure(rate: Int, channels: Int) {
        srcRate = rate; ch = channels
        step = rate.toDouble() / dstRate
        fc = min(1.0, dstRate.toDouble() / rate)
        halfW = ceil(ZEROS / fc).toInt()
        frames = 0
        // Start with the first kernel's left half in silence, so the opening samples are not lost.
        pos = halfW.toDouble()
        val need = (halfW + 1024) * ch
        if (buf.size < need) buf = FloatArray(need)
        java.util.Arrays.fill(buf, 0, halfW * ch, 0f)
        frames = halfW
    }

    private companion object {
        const val ZEROS = 16                // zero-crossings each side
        const val RES = 512                 // table points per zero-crossing
        /** Blackman-windowed sinc over [0, ZEROS], one side (it is symmetric). */
        val TABLE = FloatArray(ZEROS * RES + 2).also { t ->
            for (n in t.indices) {
                val x = n.toDouble() / RES
                if (x > ZEROS) { t[n] = 0f; continue }
                val sinc = if (n == 0) 1.0 else sin(PI * x) / (PI * x)
                val w = 0.42 + 0.5 * cos(PI * x / ZEROS) + 0.08 * cos(2 * PI * x / ZEROS)
                t[n] = (sinc * w).toFloat()
            }
        }

        fun kernel(x: Double): Float {
            val a = (if (x < 0) -x else x) * RES
            val n = a.toInt()
            if (n >= ZEROS * RES) return 0f
            val f = (a - n).toFloat()
            return TABLE[n] + f * (TABLE[n + 1] - TABLE[n])
        }
    }
}
