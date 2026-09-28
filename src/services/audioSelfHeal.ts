/**
 * ★★★ AUDIO SELF-HEALING — IS WHAT ARRIVES ACTUALLY BEING PLAYED?
 *
 * Stuart, 2026-09-28: audio stopped in FM stereo and in DAB while everything else looked fine. The
 * server went on sending (its DAB countdown completed, its own audit showed real audio), and a
 * browser refresh — or, in the app, nothing, because there is no refresh button — cured it. A
 * server-side harness found no fault in ~90 randomised transitions. So the fault is in delivery or
 * in the client, and the cure is: "we need to know if the client is actually receiving the audio
 * even if the server is sending it. If we can detect the audio status on the client end we could
 * simply repair the connection."
 *
 * ★★ WHAT THE WATCHDOGS BEFORE THIS COULD NOT SEE. The native watchdogs (VibePowerModule /
 *    VibeStreamService `reviveIfDead`) reopen the socket when FRAMES STOP ARRIVING. The web
 *    client's output watchdog judged "feeding" by AUDIBLE output from the worklet — i.e. by the
 *    very thing that had stopped, so on the Worker path (every modern browser) it could never
 *    fire. None of them caught: frames arriving and NOTHING PLAYING — a decoder that stopped
 *    emitting, a player node stopped, an engine suspended or re-routed, frames dropped by the
 *    client, a jitter buffer wedged.
 *
 * ★★★ THE MEASURE: two monotonic counters, compared.
 *      rx     — frames that arrived from the socket (any content — the server sends continuous
 *               frames in every mode, squelched and silent included);
 *      played — samples (or frames) the OUTPUT actually consumed: the worklet's drain count, the
 *               AVAudioPlayerNode's completed buffers of REAL audio, the AudioTrack's playback head.
 *    Frames arriving + nothing consumed for STALL_MS = RECEIVING-NOT-PLAYING → rebuild the local
 *    pipeline; still bad after a settle window → reopen the socket; and so on, with back-off.
 *    Nothing arriving for NOT_RECV_MS (only where no other watchdog owns that) → reopen the socket.
 *
 * ★★ NEVER FIGHT THE LISTENER. `expected` is false while muted / paused / backgrounded-without-
 *    audio / behind an autoplay gate / in a decoder the counters cannot see (DAB+ AAC through a
 *    media element). While not expected, every clock is reset, so returning to "expected" starts a
 *    fresh window rather than judging the pause as a stall. `hold(ms)` does the same for a
 *    legitimate transition (a retune flush, DAB priming, a mode change).
 * ★★ NEVER LOOP. At most MAX_PER_MIN repairs in any rolling minute, exponential back-off between
 *    attempts (capped, never permanent — see memory never_limit_permanently), and a clean run of
 *    HEALTHY_RESET_MS forgets the escalation.
 *
 * ★ PURE AND PORTED. No timers, no platform calls: the caller ticks it once a second with its own
 *   counters and carries out the action. The SAME rules are ported, line for line, to
 *     - spike/WristSDR/WristSDR/AudioSelfHeal.swift  (Jr, and compiled into the iOS app too)
 *     - android/app/src/main/java/com/vibesdr/app/AudioSelfHeal.kt
 *   Change one, change all three — scripts/test_audioSelfHeal.ts is the executable spec.
 */

export type HealAction = 'none' | 'rebuild-pipeline' | 'reopen-socket';

export type HealState =
  | 'idle'                    // not expected to play (muted, paused, hidden, held, gated)
  | 'starting'                // expected, but nothing has arrived yet
  | 'healthy'                 // receiving and playing
  | 'receiving-not-playing'   // THE fault this exists for
  | 'not-receiving';          // nothing arriving (half-open socket, dead link)

export interface HealInput {
  /** Monotonic milliseconds. */
  now: number;
  /** Cumulative frames received from the socket. Only ever increases (a reset is tolerated). */
  rx: number;
  /** Cumulative samples/frames consumed by the output. Only ever increases (a reset is tolerated). */
  played: number;
  /** Does the listener expect sound right now? False = muted, paused, gated, backgrounded… */
  expected: boolean;
}

export interface HealDecision {
  action: HealAction;
  state: HealState;
  /** 1-based count of repairs in the current escalation (the one being ordered, when action ≠ none). */
  attempt: number;
  /** Human-readable, for the log line. */
  reason: string;
}

export interface HealConfig {
  /** Receiving-not-playing must persist this long before anything is done. */
  stallMs: number;
  /** rx must have advanced within this window to count as "receiving". */
  rxFreshMs: number;
  /** Nothing received, while expected, for this long → reopen the socket. null = another watchdog
   *  owns that case (the native apps' reviveIfDead) and this one stays out of it. */
  notRecvMs: number | null;
  /** After a repair, how long the pipeline gets before it is judged again (the base back-off). */
  settleMs: number;
  /** Back-off ceiling. The retry never stops; it only slows to this. */
  backoffMaxMs: number;
  /** Rolling-minute cap on repairs. */
  maxPerMin: number;
  /** Continuous healthy time that forgets the escalation. */
  healthyResetMs: number;
}

export const DEFAULT_HEAL_CONFIG: HealConfig = {
  stallMs: 3000,
  rxFreshMs: 1500,
  notRecvMs: null,
  settleMs: 3000,
  backoffMaxMs: 60000,
  maxPerMin: 4,
  healthyResetMs: 10000,
};

export class AudioSelfHeal {
  readonly cfg: HealConfig;
  private lastRx = -1;
  private lastPlayed = -1;
  private lastRxAt = 0;          // 0 = never
  private lastPlayAt = 0;
  private windowStart = 0;       // when "expected" last began (or a repair/hold last ended)
  private healthySince = 0;
  private holdUntil = 0;
  private nextRepairAt = 0;
  private stage = 0;             // repairs in the current escalation
  private recent: number[] = []; // timestamps of repairs in the last minute
  /** Lifetime totals, for logs and debug readouts. */
  repairs = 0;
  lastReason = '';
  state: HealState = 'idle';

  constructor(cfg: Partial<HealConfig> = {}) {
    this.cfg = { ...DEFAULT_HEAL_CONFIG, ...cfg };
  }

  /** Suppress judgement for `ms` — a legitimate transition (retune flush, DAB priming, mode
   *  change, a repair the caller performed itself). Clocks restart when it ends. */
  hold(now: number, ms: number): void {
    this.holdUntil = Math.max(this.holdUntil, now + ms);
  }

  /** Forget everything (a new session, a new server). */
  reset(): void {
    this.lastRx = -1; this.lastPlayed = -1; this.lastRxAt = 0; this.lastPlayAt = 0;
    this.windowStart = 0; this.healthySince = 0; this.holdUntil = 0; this.nextRepairAt = 0;
    this.stage = 0; this.recent = []; this.state = 'idle';
  }

  tick(inp: HealInput): HealDecision {
    const c = this.cfg;
    const now = inp.now;
    // Advance detection. A counter that goes BACKWARDS (a rebuilt node restarting at zero) counts
    // as neither advancing nor stalled — it re-bases.
    const rxAdv = this.lastRx >= 0 && inp.rx > this.lastRx;
    const plAdv = this.lastPlayed >= 0 && inp.played > this.lastPlayed;
    this.lastRx = inp.rx;
    this.lastPlayed = inp.played;

    // ★★ NOT EXPECTED, OR HELD: judge nothing, and restart every clock so the moment it IS expected
    //    again is the start of a fresh window, never the tail of a "stall" that was a pause.
    if (!inp.expected || now < this.holdUntil) {
      this.lastRxAt = 0; this.lastPlayAt = 0; this.windowStart = 0; this.healthySince = 0;
      this.state = 'idle';
      return { action: 'none', state: 'idle', attempt: this.stage,
               reason: inp.expected ? 'held (transition)' : 'not expected' };
    }
    if (this.windowStart === 0) this.windowStart = now;
    if (rxAdv) this.lastRxAt = now;
    if (plAdv) this.lastPlayAt = now;

    const receiving = this.lastRxAt > 0 && now - this.lastRxAt <= c.rxFreshMs;
    // Silence is measured from the last consumption OR the start of this window, whichever is later
    // — a window that has just opened has not been silent for longer than it has existed.
    const quietMs = now - Math.max(this.lastPlayAt, this.windowStart);
    const deafMs = now - Math.max(this.lastRxAt, this.windowStart);

    let fault: HealState | null = null;
    if (receiving && quietMs >= c.stallMs) fault = 'receiving-not-playing';
    else if (!receiving && c.notRecvMs !== null && deafMs >= c.notRecvMs) fault = 'not-receiving';

    if (!fault) {
      const playing = this.lastPlayAt > 0 && quietMs < c.stallMs;
      if (receiving && playing) {
        if (this.healthySince === 0) this.healthySince = now;
        // ★ A CLEAN RUN FORGETS THE ESCALATION — the same rule as the Opus decoder's opusFails and
        //   the web watchdog's stallRebuilds: a limit on CONSECUTIVE failures, not on failures for
        //   the life of the session (that version left the page permanently disarmed, 2026-08-26).
        if (this.stage > 0 && now - this.healthySince >= c.healthyResetMs) {
          this.stage = 0; this.nextRepairAt = 0;
        }
        this.state = 'healthy';
      } else {
        this.healthySince = 0;
        // Receiving but nothing consumed YET in this window (a fresh start, a just-repaired
        // pipeline refilling its cushion) is 'starting', not 'healthy'.
        this.state = receiving || this.lastRxAt === 0 ? 'starting' : 'not-receiving';
      }
      return { action: 'none', state: this.state, attempt: this.stage, reason: '' };
    }

    this.healthySince = 0;
    this.state = fault;
    if (now < this.nextRepairAt) {
      return { action: 'none', state: fault, attempt: this.stage, reason: 'waiting (back-off)' };
    }
    // Rolling-minute cap.
    this.recent = this.recent.filter((t) => now - t < 60000);
    if (this.recent.length >= c.maxPerMin) {
      this.nextRepairAt = this.recent[0] + 60000;
      return { action: 'none', state: fault, attempt: this.stage, reason: 'rate-limited' };
    }

    // ★★ THE LADDER. Receiving-not-playing: the cheap LOCAL cure first (decoder + player/engine),
    //    and only if that does not bring sound back, the socket; then round again, slower. A
    //    socket that delivers nothing only ever gets the socket.
    const action: HealAction = fault === 'not-receiving'
      ? 'reopen-socket'
      : (this.stage % 2 === 0 ? 'rebuild-pipeline' : 'reopen-socket');
    this.stage++;
    this.repairs++;
    this.recent.push(now);
    const backoff = Math.min(c.backoffMaxMs, c.settleMs * Math.pow(2, Math.max(0, this.stage - 2)));
    this.nextRepairAt = now + backoff;
    // The repaired pipeline gets its own window: judged from now, not from before the repair.
    this.windowStart = now; this.lastPlayAt = 0;
    const reason = fault === 'receiving-not-playing'
      ? `receiving but nothing played for ${(quietMs / 1000).toFixed(1)} s`
      : `nothing received for ${(deafMs / 1000).toFixed(1)} s`;
    this.lastReason = reason;
    return { action, state: fault, attempt: this.stage, reason };
  }
}
