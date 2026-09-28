package com.vibesdr.app

import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow

/**
 * ★★★ AUDIO SELF-HEALING — IS WHAT ARRIVES ACTUALLY BEING PLAYED?
 *
 * The Kotlin port of src/services/audioSelfHeal.ts (the Swift port, shared by Jr and the iOS app,
 * is spike/WristSDR/WristSDR/AudioSelfHeal.swift). Change one, change all three:
 * scripts/test_audioSelfHeal.ts is the executable spec, and scripts/test_audioSelfHeal_kotlin.sh
 * runs THIS file against the same scenarios.
 *
 * Stuart, 2026-09-28: audio stopped (FM stereo, DAB) while the server kept sending and everything
 * else looked fine; only a reconnect cured it. The watchdog in VibeStreamService reopens the socket
 * when FRAMES STOP ARRIVING; nothing caught frames arriving and NOTHING PLAYING — a decode thread
 * that died, an AudioTrack write wedged on a track the system stopped, a codec that stopped
 * emitting. Two monotonic counters: `rx` (packets off the socket) and `played` (frames the
 * AudioTrack's playback head actually advanced). Receiving + nothing played for [stallMs] →
 * rebuild the local pipeline; still silent → reopen the socket; round again, slower (back-off,
 * 4-a-minute cap, never permanent). While `expected` is false (muted, paused, disconnected for the
 * data saver, focus lost) or during a [hold], every clock is reset, so a pause is never a stall.
 *
 * Pure: no timers, no Android calls, so it can be compiled and tested on the JVM alone.
 */
class AudioSelfHeal(
    val stallMs: Long = 3_000,
    val rxFreshMs: Long = 1_500,
    /** null = another watchdog owns "nothing arriving" (reviveIfDead) and this stays out of it. */
    val notRecvMs: Long? = null,
    val settleMs: Long = 3_000,
    val backoffMaxMs: Long = 60_000,
    val maxPerMin: Int = 4,
    val healthyResetMs: Long = 10_000,
) {
    enum class Action(val wire: String) { NONE("none"), REBUILD_PIPELINE("rebuild-pipeline"), REOPEN_SOCKET("reopen-socket") }
    enum class State(val wire: String) {
        IDLE("idle"), STARTING("starting"), HEALTHY("healthy"),
        RECEIVING_NOT_PLAYING("receiving-not-playing"), NOT_RECEIVING("not-receiving")
    }
    data class Decision(val action: Action, val state: State, val attempt: Long, val reason: String)

    private var lastRx = -1L
    private var lastPlayed = -1L
    private var lastRxAt = 0L
    private var lastPlayAt = 0L
    private var windowStart = 0L
    private var healthySince = 0L
    private var holdUntil = 0L
    private var nextRepairAt = 0L
    private var stage = 0L
    private val recent = ArrayDeque<Long>()
    var repairs = 0L; private set
    var lastReason = ""; private set
    var state = State.IDLE; private set

    /** Suppress judgement for [ms] — a legitimate transition (retune flush, DAB priming). */
    fun hold(now: Long, ms: Long) { holdUntil = max(holdUntil, now + ms) }

    fun reset() {
        lastRx = -1; lastPlayed = -1; lastRxAt = 0; lastPlayAt = 0; windowStart = 0
        healthySince = 0; holdUntil = 0; nextRepairAt = 0; stage = 0; recent.clear(); state = State.IDLE
    }

    /** [now] is monotonic milliseconds (SystemClock.elapsedRealtime), never 0 in practice. */
    fun tick(now: Long, rx: Long, played: Long, expected: Boolean): Decision {
        val rxAdv = lastRx >= 0 && rx > lastRx
        val plAdv = lastPlayed >= 0 && played > lastPlayed
        lastRx = rx
        lastPlayed = played

        if (!expected || now < holdUntil) {
            lastRxAt = 0; lastPlayAt = 0; windowStart = 0; healthySince = 0
            state = State.IDLE
            return Decision(Action.NONE, State.IDLE, stage, if (expected) "held (transition)" else "not expected")
        }
        if (windowStart == 0L) windowStart = now
        if (rxAdv) lastRxAt = now
        if (plAdv) lastPlayAt = now

        val receiving = lastRxAt > 0 && now - lastRxAt <= rxFreshMs
        val quiet = now - max(lastPlayAt, windowStart)
        val deaf = now - max(lastRxAt, windowStart)

        val fault: State? = when {
            receiving && quiet >= stallMs -> State.RECEIVING_NOT_PLAYING
            !receiving && notRecvMs != null && deaf >= notRecvMs -> State.NOT_RECEIVING
            else -> null
        }
        if (fault == null) {
            val playing = lastPlayAt > 0 && quiet < stallMs
            if (receiving && playing) {
                if (healthySince == 0L) healthySince = now
                if (stage > 0 && now - healthySince >= healthyResetMs) { stage = 0; nextRepairAt = 0 }
                state = State.HEALTHY
            } else {
                healthySince = 0
                state = if (receiving || lastRxAt == 0L) State.STARTING else State.NOT_RECEIVING
            }
            return Decision(Action.NONE, state, stage, "")
        }

        healthySince = 0
        state = fault
        if (now < nextRepairAt) return Decision(Action.NONE, fault, stage, "waiting (back-off)")
        while (recent.isNotEmpty() && now - recent.first() >= 60_000) recent.removeFirst()
        if (recent.size >= maxPerMin) {
            nextRepairAt = recent.first() + 60_000
            return Decision(Action.NONE, fault, stage, "rate-limited")
        }
        val action = if (fault == State.NOT_RECEIVING) Action.REOPEN_SOCKET
                     else if (stage % 2 == 0L) Action.REBUILD_PIPELINE else Action.REOPEN_SOCKET
        stage++
        repairs++
        recent.addLast(now)
        val backoff = min(backoffMaxMs.toDouble(), settleMs * 2.0.pow(max(0L, stage - 2).toDouble())).toLong()
        nextRepairAt = now + backoff
        windowStart = now; lastPlayAt = 0
        val reason = if (fault == State.RECEIVING_NOT_PLAYING)
            String.format(java.util.Locale.ROOT, "receiving but nothing played for %.1f s", quiet / 1000.0)
        else String.format(java.util.Locale.ROOT, "nothing received for %.1f s", deaf / 1000.0)
        lastReason = reason
        return Decision(action, fault, stage, reason)
    }
}
