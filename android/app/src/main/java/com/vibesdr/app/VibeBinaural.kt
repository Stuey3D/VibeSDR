package com.vibesdr.app

import java.util.concurrent.atomic.AtomicBoolean

/**
 * ★★ V12 BINAURAL TUNING — the audio leans toward the ear on the side the signal lies, and comes back to the middle
 * of the head as you tune onto it. The DSP is vibedsp/binaural (C++), the SAME object the iOS audio engine calls:
 * the far ear quieter (≤ 10 dB, never silent), later (≤ 0.6 ms) and duller (head shadow); the SSB voice estimator
 * on its own worker thread; the WFM stereo hand-off. See binaural.h for the why.
 *
 * ★ A singleton, NOT a field of VibeStreamService: JS states the setting (and the offset source) whenever it likes,
 *   including before the service exists or between two of its lives, and the next AudioTrack writer must find it.
 * ★ Setters only store atomics in the C++ object — any thread. process*() is called by the AudioTrack writers.
 */
object VibeBinaural {
    private val h: Long = try { System.loadLibrary("vibelocalsdr"); nCreate() } catch (_: Throwable) { 0L }

    /** The listener's setting. Off = the writers' original path exactly (mono tracks stay mono). */
    @Volatile var on = false
        private set

    /* ★★ ONE CALLER AT A TIME, AND NEVER A WAIT. The C++ object has one spatialiser and one scratch buffer. The
     *  writers are normally exclusive, but a superseded ext writer can still be finishing a packet while its
     *  replacement starts (see startExtWriter), so two could meet here. The loser of the race plays this packet
     *  DRY (upmixed, unpanned) rather than wait: Network > Audio — the audio thread never blocks. */
    private val busy = AtomicBoolean(false)

    fun setEnabled(v: Boolean) { on = v && h != 0L; if (h != 0L) nSetEnabled(h, v) }
    fun setSource(src: Int, deadHz: Float, fullHz: Float) { if (h != 0L) nSetSource(h, src, deadHz, fullHz) }
    fun setOffset(hz: Float, valid: Boolean) { if (h != 0L) nSetOffset(h, hz, valid) }
    fun setStereo(hint: Int) { if (h != 0L) nSetStereo(h, hint) }
    fun setDial(hz: Double) { if (h != 0L) nSetDial(h, hz) }
    /** The waterfall's coarse SSB cue: the voice's carrier, ABSOLUTE Hz (binauralOffset.ts ssbCoarseCarrier). */
    fun setSsbHint(carrierHz: Double, valid: Boolean) { if (h != 0L) nSetSsbHint(h, carrierHz, valid) }
    /** The mode's spread map (0 none — AM/FM/DAB; 1 USB, 2 LSB, 3/4 CW) and the default SSB passband's edges. Not a
     *  style: native spreads only as far as the listener is off tune, by the same weight as the lean. */
    fun setSpread(kind: Int, edgeLoHz: Float, edgeHiHz: Float) { if (h != 0L) nSetSpread(h, kind, edgeLoHz, edgeHiHz) }

    /** int16 (ch 1|2) → int16 stereo into `out` (≥ 2·frames). Returns the SAMPLES written (2·frames). */
    fun processShorts(src: ShortArray, frames: Int, ch: Int, rate: Int, out: ShortArray): Int {
        if (busy.compareAndSet(false, true)) {
            try { if (nProcessShorts(h, src, ch, frames, rate, out) == frames) return frames * 2 } finally { busy.set(false) }
        }
        return upmix(src, frames, ch, out)
    }

    /** PCM16 LE bytes (ch 1|2) → PCM16 LE stereo bytes into `out` (≥ 4·frames). Returns the BYTES written. */
    fun processBytes(src: ByteArray, frames: Int, ch: Int, rate: Int, out: ByteArray): Int {
        if (busy.compareAndSet(false, true)) {
            try { val n = nProcessBytes(h, src, ch, frames, rate, out); if (n == frames * 4) return n } finally { busy.set(false) }
        }
        // Dry: mono → both ears, stereo as it is.
        if (ch == 2) { System.arraycopy(src, 0, out, 0, frames * 4); return frames * 4 }
        for (i in 0 until frames) {
            val lo = src[2 * i]; val hi = src[2 * i + 1]
            out[4 * i] = lo; out[4 * i + 1] = hi; out[4 * i + 2] = lo; out[4 * i + 3] = hi
        }
        return frames * 4
    }

    private fun upmix(src: ShortArray, frames: Int, ch: Int, out: ShortArray): Int {
        if (ch == 2) { System.arraycopy(src, 0, out, 0, frames * 2); return frames * 2 }
        for (i in 0 until frames) { val v = src[i]; out[2 * i] = v; out[2 * i + 1] = v }
        return frames * 2
    }

    @JvmStatic private external fun nCreate(): Long
    @JvmStatic private external fun nSetEnabled(h: Long, on: Boolean)
    @JvmStatic private external fun nSetSource(h: Long, src: Int, dead: Float, full: Float)
    @JvmStatic private external fun nSetOffset(h: Long, hz: Float, valid: Boolean)
    @JvmStatic private external fun nSetStereo(h: Long, hint: Int)
    @JvmStatic private external fun nSetDial(h: Long, hz: Double)
    @JvmStatic private external fun nSetSsbHint(h: Long, carrierHz: Double, valid: Boolean)
    @JvmStatic private external fun nSetSpread(h: Long, kind: Int, lo: Float, hi: Float)
    @JvmStatic private external fun nProcessShorts(h: Long, input: ShortArray, inCh: Int, frames: Int, rate: Int, out: ShortArray): Int
    @JvmStatic private external fun nProcessBytes(h: Long, input: ByteArray, inCh: Int, frames: Int, rate: Int, out: ByteArray): Int
}
