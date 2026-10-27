package com.vibesdr.app

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.util.Base64
import android.util.Log
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * ★★ V12 TWO SERVERS AT ONCE — the second voice, and the pan both voices are placed by (GitHub discussion #57; Stuart:
 * "server 1 moves to left ear, server 2 right ear"). The pairing itself is JavaScript (src/services/dualServer.ts,
 * rules in dualMix.ts); this is only the sound, and the iOS twin is ios/VibeSDR/VibeDualAudio.swift.
 *
 *  · DualVoice — one server folded to mono and placed by position + level on VFO A/B's 400 ms raised cosine and its
 *    equal-power law (vibe_watch.h Ramp / PanMixer). ONE RULE, THREE READERS — dualMix.ts, the Swift twin and this.
 *  · VibeDualMix.primary is applied by VibeStreamService's ext writer to the main path (the VibeServer pump), AFTER the
 *    recorder's copy is taken. Not paired = engaged() is false and the writer is exactly as before.
 *  · The SECOND VOICE is its own AudioTrack on its own URGENT_AUDIO thread, fed Opus packets from JavaScript (the socket
 *    is read there) and decoded by a SECOND native Opus decoder (VibeLocalSDR.opusDecodeAux) — the main path's decoder
 *    carries state between packets and must never see the other server's.
 *  ★ Thread rule (Network > Audio > Spectrum > Decoders): the writer never waits on JS; a full queue drops the oldest
 *    rather than block the bridge.
 */
class DualRamp(v: Float) {
    var from = v; var to = v; var cur = v
    private var t = 1; private var dur = 1
    fun set(v: Float) { from = v; to = v; cur = v; t = 1; dur = 1 }
    fun aim(v: Float, steps: Int) { if (v == to) return; from = cur; to = v; t = 0; dur = max(1, steps) }
    val moving: Boolean get() = t < dur
    fun step(): Float {
        if (t < dur) {
            t++
            val u = t.toFloat() / dur.toFloat()
            cur = from + (to - from) * (0.5f - 0.5f * cos(PI.toFloat() * u))
        }
        return cur
    }
}

/** One server's place in the head. Targets from any thread; process() from ONE writer thread at a time. */
class DualVoice {
    @Volatile private var wantPos = 0.5f
    @Volatile private var wantGain = 1f
    @Volatile private var wantOn = false
    private val pos = DualRamp(0.5f)
    private val gain = DualRamp(1f)
    @Volatile private var on = false
    /** Smoothed level (0…1) for the card's MONO speaker. */
    @Volatile var level = 0f
        private set

    fun setTarget(p: Float, g: Float, o: Boolean) {
        wantPos = p.coerceIn(0f, 1f); wantGain = g.coerceIn(0f, 1f); wantOn = o
    }

    /** Must the writer route this buffer through process()? Read ONCE per buffer, before choosing the track's layout. */
    fun engaged(): Boolean = wantOn || on

    /**
     * int16 (ch 1|2, `samples` values) → int16 STEREO into `out` (≥ 2·frames). Returns the samples written. Always
     * stereo once engaged — the writer chose a stereo track on engaged(), so it must get stereo back.
     */
    fun process(src: ShortArray, samples: Int, ch: Int, rate: Int, out: ShortArray, extraGain: Float = 1f): Int {
        val p = wantPos; val g = wantGain; val o = wantOn
        if (!on) { on = true; pos.set(0.5f); gain.set(1f) }   // from the plain path: centre at unity — no step
        val frames = samples / max(1, ch)
        val steps = (0.4 * rate).roundToInt()                   // dualMix.ts DUAL_RAMP_SEC
        pos.aim(p, steps); gain.aim(g, steps)
        val k = sqrt(2f) * extraGain
        val hp = (PI / 2).toFloat()
        var sq = 0.0
        if (!pos.moving && !gain.moving) {
            val gl = gain.cur * k * cos(pos.cur * hp); val gr = gain.cur * k * sin(pos.cur * hp)
            for (i in 0 until frames) {
                val m = if (ch == 2) 0.5f * (src[2 * i] + src[2 * i + 1]) else src[i].toFloat()
                sq += (m * m).toDouble()
                out[2 * i] = clip(m * gl); out[2 * i + 1] = clip(m * gr)
            }
        } else {
            for (i in 0 until frames) {
                val m = if (ch == 2) 0.5f * (src[2 * i] + src[2 * i + 1]) else src[i].toFloat()
                sq += (m * m).toDouble()
                val th = pos.step() * hp; val v = m * gain.step() * k
                out[2 * i] = clip(v * cos(th)); out[2 * i + 1] = clip(v * sin(th))
            }
        }
        val rms = if (frames > 0) (sqrt(sq / frames) / 32768.0).toFloat() else 0f
        level = max(rms, level * 0.85f)
        // ★ Home and resting with the pairing over: stand down — the writer goes back to its own layout next buffer.
        if (!o && !pos.moving && !gain.moving && abs(pos.cur - 0.5f) < 1e-6f && gain.cur >= 1f) on = false
        return frames * 2
    }

    private fun clip(v: Float): Short = v.coerceIn(-32768f, 32767f).toInt().toShort()
}

/** The shared state the main writer and the second voice read. A singleton: JS states the mix whenever it likes. */
object VibeDualMix {
    val primary = DualVoice()
    val aux = DualVoice()
    /** ⏸ on the main path (VibeStreamService.setMutedNative) pauses the second voice too — one app, one pause. */
    @Volatile var primaryMuted = false
    /** The listener's volume × the page duck, as the main tracks get it (VibeStreamService.applyVolume). */
    @Volatile var masterVolume = 1f
}

class VibeDualAudioModule(ctx: ReactApplicationContext) : ReactContextBaseJavaModule(ctx) {
    override fun getName() = "VibeDualAudio"

    private companion object {
        const val TAG = "VibeDualAudio"
        const val RATE = 48_000
        const val PREROLL_MS = 220      // the main path's cushion
        const val CAP_MS = 500          // a burst never builds latency past this
    }

    /** Decoded 48 kHz frames: (samples, channels). */
    private val queue = LinkedBlockingQueue<Pair<ShortArray, Int>>(64)
    @Volatile private var wanted = false
    @Volatile private var gen = 0
    private var thread: Thread? = null

    @ReactMethod
    fun setNativeMix(pos: Double, gain: Double, active: Boolean) {
        VibeDualMix.primary.setTarget(pos.toFloat(), gain.toFloat(), active)
    }

    @ReactMethod
    fun setAuxMix(pos: Double, gain: Double) { VibeDualMix.aux.setTarget(pos.toFloat(), gain.toFloat(), true) }

    @ReactMethod
    fun startAux() {
        if (wanted && thread?.isAlive == true) return
        wanted = true
        startWriter()
    }

    @ReactMethod
    fun stopAux() {
        wanted = false
        gen++
        queue.clear()
        thread?.interrupt()
        thread = null
    }

    @ReactMethod
    fun pushAuxOpus(b64: String, rate: Double, channels: Double) {
        if (!wanted) return
        val pkt = try { Base64.decode(b64, Base64.NO_WRAP) } catch (_: Exception) { return }
        if (pkt.size < 3) return
        val ch = if (channels.toInt() == 2) 2 else 1
        // ★ Decoded at 48 kHz whatever the stream's rate: an Opus decoder runs at any of its rates, so this voice needs
        //   no resampler and never rebuilds its track on a mode change that flips the server's audio rate.
        val pcm = VibeLocalSDR.opusDecodeAux(pkt, RATE, ch) ?: return
        offer(pcm, ch)
    }

    /** Uncompressed int16 (an old server without Opus, or ADPCM decoded in JS) at its own rate → 48 kHz, linear. */
    @ReactMethod
    fun pushAuxPcm(b64: String, rate: Double, channels: Double) {
        if (!wanted) return
        val bytes = try { Base64.decode(b64, Base64.NO_WRAP) } catch (_: Exception) { return }
        val ch = if (channels.toInt() == 2) 2 else 1
        val r = rate.toInt()
        if (r < 8000 || r > 384_000) return
        val n = bytes.size / 2
        val s = ShortArray(n)
        ByteBuffer.wrap(bytes, 0, n * 2).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().get(s)
        if (r == RATE) { offer(s, ch); return }
        val inFrames = n / ch
        val outFrames = (inFrames.toLong() * RATE / r).toInt()
        if (outFrames <= 0) return
        val o = ShortArray(outFrames * ch)
        for (i in 0 until outFrames) {
            val x = i.toDouble() * r / RATE
            val i0 = min(inFrames - 1, x.toInt()); val i1 = min(inFrames - 1, i0 + 1); val f = (x - i0).toFloat()
            for (c in 0 until ch) o[i * ch + c] = (s[i0 * ch + c] * (1 - f) + s[i1 * ch + c] * f).toInt().toShort()
        }
        offer(o, ch)
    }

    @ReactMethod
    fun getLevels(promise: Promise) {
        val m = Arguments.createMap()
        m.putDouble("native", VibeDualMix.primary.level.toDouble())
        m.putDouble("aux", VibeDualMix.aux.level.toDouble())
        promise.resolve(m)
    }

    private fun offer(pcm: ShortArray, ch: Int) {
        if (!queue.offer(pcm to ch)) { queue.poll(); queue.offer(pcm to ch) }   // full: drop the oldest, never block
    }

    private fun startWriter() {
        val g = ++gen
        val t = Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
            var track: AudioTrack? = null
            var out = ShortArray(0)
            var primed = false
            var queuedFrames = 0L          // frames written since the track started, less what it has played
            try {
                track = buildTrack()
                while (wanted && g == gen) {
                    // ★ A cushion before the first sound, re-armed whenever the queue runs dry (the main path's rule).
                    if (!primed) {
                        val have = queue.sumOf { it.first.size / it.second }
                        if (have < RATE * PREROLL_MS / 1000) { Thread.sleep(10); continue }
                        primed = true
                    }
                    val item = queue.poll(250, TimeUnit.MILLISECONDS)
                    if (item == null) { primed = false; continue }
                    val (pcm, ch) = item
                    val frames = pcm.size / ch
                    if (out.size < frames * 2) out = ShortArray(frames * 2)
                    val n = VibeDualMix.aux.process(pcm, pcm.size, ch, RATE, out, VibeDualMix.masterVolume)
                    if (VibeDualMix.primaryMuted) continue
                    // ★ Bound the latency: a burst that would put more than CAP_MS in the track is dropped.
                    val played = track.playbackHeadPosition.toLong() and 0xFFFFFFFFL
                    if (queuedFrames - played > RATE.toLong() * CAP_MS / 1000) continue
                    try { track.write(out, 0, n) } catch (e: IllegalStateException) { Log.w(TAG, "track gone: ${e.message}"); break }
                    queuedFrames += frames
                }
            } catch (_: InterruptedException) {
                // normal stop
            } catch (e: Throwable) {
                Log.w(TAG, "second voice writer ended: ${e.message}")
            } finally {
                try { track?.release() } catch (_: Throwable) {}
            }
        }, "vibesdr-dual")
        t.priority = Thread.MAX_PRIORITY
        thread = t
        t.start()
    }

    private fun buildTrack(): AudioTrack {
        val mask = AudioFormat.CHANNEL_OUT_STEREO
        val minBuf = AudioTrack.getMinBufferSize(RATE, mask, AudioFormat.ENCODING_PCM_16BIT)
        val t = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build())
            .setAudioFormat(AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(RATE)
                .setChannelMask(mask)
                .build())
            .setBufferSizeInBytes(maxOf(minBuf * 4, RATE / 2 * 2 * 2))   // ≥ 500 ms, as the main tracks
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        t.play()
        Log.i(TAG, "second voice AudioTrack ${RATE}Hz stereo")
        return t
    }
}
