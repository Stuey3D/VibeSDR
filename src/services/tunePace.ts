// tunePace.ts — send fewer tunes to a server that says it is struggling. Pure (timers injected) so
// scripts/test_tune_pace.ts can drive it; the app (VibeServerWsClient) and the web page (spectrum.ts)
// share it.
//
// ★★★ WHY (Stuart, 2026-10-05): "same as we do for if the ping increases. Basically if the server's
//     CPU is reporting that it is struggling we need to slow down the amount of tune commands so that
//     we don't overload it, I had a bit of a nightmare setting up the pi2 in the garage last night
//     probably due to network connection and the server CPU getting bogged down."
//     A drum spin is dozens of tunes a second; each one the server applies can be a re-centre, an IF
//     filter, an AGC reset. On the Pi 2 the WFM chain is one thread behind a 4-slot queue and a full
//     queue BLOCKS the DSP thread, so a clump of retunes there is heard by everybody on the radio.
// ★★ THE DIAL STAYS INSTANT. Only what goes on the WIRE is paced: the on-screen frequency, the needle
//    and the readouts move on every step as before. Latest wins, and the trailing send is guaranteed —
//    the radio always ends on the frequency the user stopped on.
// ★★ THE SIGNALS ARE THE ONES THE CLIENT ALREADY HAS: the server's `health` message (the CPU rung and
//    the snail, pushed on change, already hysteresis'd server side — "rises at once, falls late" — so no
//    second smoothing here) and the ping RTT. No new message, so it works against every server that
//    sends a health pill, and an older server that sends none simply stays at full rate.
// ★ The server paces too (vibe_tune_pace.h) and covers clients that cannot be patched; this half
//   keeps the tunes off a slow LINK as well, which the server cannot do for us.

/** What the client knows about the server and the path to it. Every field optional: absent = fine. */
export interface TuneLoad {
  /** Health pill CPU rung: 0 OK, 1 WARM, 2 HIGH (>= 75 %), 3 CRITICAL (>= 90 %). */
  cpuLevel?: number;
  /** The snail: the server is fully loaded and running below its all-core clock (any cause). */
  throttled?: boolean;
  /** Median ping round trip, ms. */
  rttMs?: number;
}

/** ★ The same two rungs as the server (vibe_tune_pace.h), so a client and a server that both pace
 *  agree rather than stacking: 150 ms is one tune per drum detent on a fast spin and still reads as a
 *  moving dial; 350 ms is "already not keeping up". Below a second, because a dial that lags a second
 *  feels broken. */
export const TUNE_PACE_LOADED_MS = 150;
export const TUNE_PACE_CHOKED_MS = 350;

/** The minimum gap between two tunes on the wire. 0 = send each at once, which is today's behaviour.
 *  - CHOKED: the CPU rung is CRITICAL, or the ping is 600 ms or worse (a tune queued behind a slow link
 *    arrives in a clump, which is the garage).
 *  - LOADED: the CPU rung is HIGH, the snail is out, or the ping is 250 ms or worse.
 *  ★ WARM is not load — the pill's amber is "busy and fine". */
export function tunePaceMs(l: TuneLoad): number {
  const cpu = Number(l.cpuLevel) || 0;
  const rtt = Number(l.rttMs) || 0;
  if (cpu >= 3 || rtt >= 600) return TUNE_PACE_CHOKED_MS;
  if (cpu >= 2 || l.throttled === true || rtt >= 250) return TUNE_PACE_LOADED_MS;
  return 0;
}

/** The health message's temp kinds that mean "throttled" (thermal, power, or cause unknown). */
export function healthThrottled(kind: string | undefined): boolean {
  return kind === 'thermal' || kind === 'power' || kind === 'throttle';
}

export interface TunePaceTimers {
  set: (fn: () => void, ms: number) => unknown;
  clear: (h: unknown) => void;
  now: () => number;
}

/** ★ LEADING AND TRAILING, latest wins. The first tune after a quiet spell goes at once — a single
 *  click must never wait — and a burst inside the gap is held as ONE pending value, sent when the gap
 *  is up. With a gap of 0 every push is sent synchronously, exactly as before this existed. */
export class TunePacer<T> {
  private gapMs = 0;
  private lastSentAt = -Infinity;
  private pending: { v: T } | null = null;
  private timer: unknown = null;
  // ★ Plain fields, not parameter properties: node runs this file type-stripped for the test.
  private readonly send: (v: T) => void;
  private readonly timers: TunePaceTimers;
  constructor(
    send: (v: T) => void,
    timers: TunePaceTimers = {
      set: (fn, ms) => setTimeout(fn, ms),
      clear: (h) => clearTimeout(h as ReturnType<typeof setTimeout>),
      now: () => Date.now(),
    },
  ) {
    this.send = send; this.timers = timers;
  }

  /** The current gap, ms. */
  gap(): number { return this.gapMs; }

  /** Change the gap. A tune already held is re-timed against the new gap, so dropping back to 0 when
   *  the server recovers releases it at once rather than leaving it to the old, longer wait. */
  setGap(ms: number): void {
    const g = Math.max(0, Math.round(Number(ms) || 0));
    if (g === this.gapMs) return;
    this.gapMs = g;
    if (this.pending) this.arm();
  }

  push(v: T): void {
    this.pending = { v };
    this.arm();
  }

  /** Send a held tune now (leaving a screen, a socket about to close). */
  flush(): void {
    if (this.timer != null) { this.timers.clear(this.timer); this.timer = null; }
    const p = this.pending;
    if (!p) return;
    this.pending = null;
    this.lastSentAt = this.timers.now();
    this.send(p.v);
  }

  /** Drop a held tune unsent — something else has decided where the radio goes (DAB took the dial, a
   *  mode switch carried its own frequency). */
  cancel(): void {
    if (this.timer != null) { this.timers.clear(this.timer); this.timer = null; }
    this.pending = null;
  }

  /** True while a tune is being held. */
  holding(): boolean { return this.pending != null; }

  private arm(): void {
    const wait = this.lastSentAt + this.gapMs - this.timers.now();
    if (wait <= 0) { this.flush(); return; }
    if (this.timer != null) this.timers.clear(this.timer);
    this.timer = this.timers.set(() => { this.timer = null; this.flush(); }, wait);
  }
}
