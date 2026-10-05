// dabStepper.ts — stepping the DAB multiplex from the tuning keys, coalesced, and what leaving DAB
// does to the live station. Pure (timers injected) so scripts/test_dab_stepper.ts can drive it.
//
// ★★★ ONE RETUNE PER BURST (Stuart, 2026-10-05): "In DAB mode with the rotary drum control the drum
//     is too sensitive and causes super fast tunes which then cause the server to have a massive
//     nightmare trying to keep up and it goes erratic switching to NFM etc." Every block change is a
//     full re-acquire on the server (retune, AGC settle, FIC lock, decoder restart), so a flick or a
//     run of taps that sent one `dab` per block queued a dozen tear-downs behind each other. The
//     label moves on EVERY press, so the user sees where they are going; the server is told only
//     the block they STOPPED on, once the presses have settled.
//  ★ Trailing-only, deliberately. A leading send (first press goes at once) still costs TWO
//    re-acquires for "> >" — the first one torn down half-way — and a single press waiting
//    SETTLE_MS is invisible next to the second or more a multiplex takes to lock anyway.

export const DAB_STEP_SETTLE_MS = 450;

export interface DabStepTimers {
  set: (fn: () => void, ms: number) => unknown;
  clear: (h: unknown) => void;
}

/** Wrap an index into 0..n-1. */
export function dabWrap(i: number, n: number): number {
  return ((i % n) + n) % n;
}

export class DabBlockStepper {
  private target = -1;
  private timer: unknown = null;
  // ★ Plain fields, not constructor parameter properties: node runs this file type-stripped for the
  //   test, and strip-only mode refuses parameter properties.
  private readonly n: number;
  private readonly send: (i: number) => void;
  private readonly show: (i: number) => void;
  private readonly settleMs: number;
  private readonly timers: DabStepTimers;
  constructor(
    n: number,
    /** Sends the block to the server — called once per settled burst. */
    send: (i: number) => void,
    /** Moves the on-screen label at once (and the app's own idea of the block). */
    show: (i: number) => void,
    settleMs = DAB_STEP_SETTLE_MS,
    timers: DabStepTimers = {
      set: (fn, ms) => setTimeout(fn, ms),
      clear: (h) => clearTimeout(h as ReturnType<typeof setTimeout>),
    },
  ) {
    this.n = n; this.send = send; this.show = show; this.settleMs = settleMs; this.timers = timers;
  }

  /** The block a burst is heading for, or -1 when nothing is waiting to be sent. */
  pending(): number { return this.target; }

  /** Step `steps` blocks from `current` — or from the block a burst in flight is heading for, so ten
   *  quick presses are ten blocks even though the server has not been told any of them yet. */
  step(current: number, steps: number): number {
    if (!steps) return this.target >= 0 ? this.target : current;
    const base = this.target >= 0 ? this.target : (current < 0 ? 0 : current);
    const next = dabWrap(base + steps, this.n);
    this.target = next;
    this.show(next);
    if (this.timer != null) this.timers.clear(this.timer);
    this.timer = this.timers.set(() => {
      this.timer = null;
      const i = this.target;
      this.target = -1;
      if (i >= 0) this.send(i);
    }, this.settleMs);
    return next;
  }

  /** Drop a burst that has not been sent — leaving DAB, or a block picked some other way. */
  cancel(): void {
    if (this.timer != null) this.timers.clear(this.timer);
    this.timer = null;
    this.target = -1;
  }
}

/** ★★★ WHAT LEAVING DAB DOES TO THE LIVE STATION (Stuart, 2026-10-05: "After exiting DAB mode I went
 *  to the airband and the DAB VTS was stuck in place … rather than dropping away it stayed and showed
 *  the last tuned station name"). The DAB service is put into `liveStation` by the DAB state report,
 *  and nothing ever took it out again: RDS clears itself with an empty metadata frame, but on AM no
 *  metadata arrives at all, so the service name held the bar for as long as the app was open.
 *  On the transition OUT of DAB a DAB-badged station is dropped; anything else (an RDS station that
 *  has already replaced it) is kept. Returns the SAME object when nothing changes. */
export function liveStationAfterDab<T extends { badge?: string }>(
  wasOn: boolean, nowOn: boolean, cur: T,
): T | Record<string, never> {
  if (!wasOn || nowOn) return cur;
  return cur.badge === 'DAB' ? {} : cur;
}
