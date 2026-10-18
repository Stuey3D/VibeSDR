package com.vibesdr.app

/**
 * RecordBufferRing — the V12 RECORD BUFFER's store on Android: the last 30 or 60 seconds of exactly what a
 * recording captures, so ⏺ REC can start the file with the audio from before the tap. The setting and the
 * retune rule live in JS (src/services/recordBuffer.ts); the iOS twin is ios/VibeSDR/RecordBufferRing.swift.
 *
 * ★★ CHUNKS AT THE SOURCE RATE. Each appended buffer becomes an immutable chunk tagged with its channels and
 *    rate — a Kiwi's 12 kHz costs a quarter of the memory 48 kHz does, and a rate flip mid-buffer needs no
 *    handling: the recorder's resampler takes each chunk as it comes, exactly as it takes live audio.
 * ★★ MEMORY (the 32-bit 1 GB phones): Int16, and MONO whenever L == R — every mode but WFM stereo or
 *    binaural — so 60 s at 48 kHz is ≈ 5.8 MB mono, ≈ 11.5 MB stereo. Nothing is held while the setting is Off:
 *    capacity 0 drops every chunk, and the call sites read `isOn` (one volatile load) before doing anything.
 * ★ Thread-safe: the audio threads append, the bridge sets/clears, the recorder snapshots. The lock is
 *   uncontended in practice and held only for a list operation — never across an encode.
 */
class RecordBufferRing {
    class Chunk(val samples: ShortArray, val channels: Int, val rate: Int) {
        val frames: Int get() = samples.size / channels
        val micros: Long get() = frames * 1_000_000L / rate.coerceAtLeast(1)
    }

    private val chunks = ArrayDeque<Chunk>()
    private var heldUs = 0L
    @Volatile var capacitySeconds = 0
        private set

    val isOn: Boolean get() = capacitySeconds > 0

    @Synchronized fun setCapacity(seconds: Int) {
        capacitySeconds = seconds.coerceIn(0, 120)
        if (capacitySeconds == 0) clear() else trim()
    }

    @Synchronized fun clear() { chunks.clear(); heldUs = 0 }

    /** Seconds currently held. */
    @Synchronized fun seconds(): Double = heldUs / 1e6

    @Synchronized fun bytesHeld(): Long = chunks.sumOf { it.samples.size * 2L }

    /**
     * Takes OWNERSHIP of [samples] (interleaved when [channels] == 2) — the caller must not write to it again.
     * A stereo buffer whose channels match is stored mono (a new, half-size array).
     */
    fun append(samples: ShortArray, channels: Int, rate: Int) {
        if (!isOn || samples.isEmpty() || rate <= 0) return
        var s = samples; var ch = channels.coerceIn(1, 2)
        if (ch == 2) {
            val frames = s.size / 2
            var same = true
            var i = 0
            while (i < frames) { if (s[i * 2] != s[i * 2 + 1]) { same = false; break }; i++ }
            if (same) { val m = ShortArray(frames); for (f in 0 until frames) m[f] = s[f * 2]; s = m; ch = 1 }
        }
        val c = Chunk(s, ch, rate)
        synchronized(this) {
            if (!isOn) return
            chunks.addLast(c)
            heldUs += c.micros
            trim()
        }
    }

    /** The held audio, oldest first. Chunks are immutable, so this copies references only. */
    @Synchronized fun snapshot(): List<Chunk> = chunks.toList()

    private fun trim() {
        val cap = capacitySeconds * 1_000_000L
        // Drop whole chunks from the front while what remains still covers the capacity.
        while (chunks.isNotEmpty() && heldUs - chunks.first().micros >= cap) {
            heldUs -= chunks.removeFirst().micros
        }
    }
}
