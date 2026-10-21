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
// ★★★ AND THE SAME RULE ON THE WEB AND THE WRIST (Stuart, 2026-10-21: "if I connect at 12b and rapidly
//     tune down to 7D hammering the multiplex button multiple times a second … dont tune the SDR just
//     hold it and tune on final move. However if I am tuning a multiplex and holding there for a second
//     to see the waterfall then tuning again we need to tune as normal"). The web client sent a `dab`
//     per press until then, and the phone tuned the watch's crown step by step. 450 ms trailing is the
//     line between the two: a run of presses is one tune; a press, a look, a press is two.

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
  private origin = -1;   // the block the burst started from — the one the server is on
  private timer: unknown = null;
  // ★ Plain fields, not constructor parameter properties: node runs this file type-stripped for the
  //   test, and strip-only mode refuses parameter properties.
  private readonly n: number;
  private readonly send: (i: number) => void;
  private readonly show: (i: number) => void;
  private readonly settleMs: number;
  private readonly timers: DabStepTimers;
  private readonly clamp: boolean;
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
    /** ★ The ends of Band III: the app's drum and the crown WRAP (a ring); the web client's arrows
     *  CLAMP (13F -> 5A in one press is not what a band scan meant). */
    edges: 'wrap' | 'clamp' = 'wrap',
  ) {
    this.n = n; this.send = send; this.show = show; this.settleMs = settleMs; this.timers = timers;
    this.clamp = edges === 'clamp';
  }

  /** The block a burst is heading for, or -1 when nothing is waiting to be sent. */
  pending(): number { return this.target; }

  /** Step `steps` blocks from `current` — or from the block a burst in flight is heading for, so ten
   *  quick presses are ten blocks even though the server has not been told any of them yet. */
  step(current: number, steps: number): number {
    if (!steps) return this.target >= 0 ? this.target : current;
    const base = this.target >= 0 ? this.target : (current < 0 ? 0 : current);
    const next = this.clamp ? Math.max(0, Math.min(this.n - 1, base + steps)) : dabWrap(base + steps, this.n);
    // ★ Pressed against an end of the band (clamp): nowhere to go, so nothing to show or send.
    if (next === base) return next;
    if (this.target < 0) this.origin = current;
    this.target = next;
    this.show(next);
    if (this.timer != null) this.timers.clear(this.timer);
    this.timer = this.timers.set(() => {
      this.timer = null;
      const i = this.target;
      this.target = -1;
      // ★ There and back again (> > < <): the burst ended where it began, on the block the server is
      //   already decoding. Sending it would tear that multiplex down and re-acquire it for nothing.
      if (i >= 0 && i !== this.origin) this.send(i);
    }, this.settleMs);
    return next;
  }

  /** Drop a burst that has not been sent — leaving DAB, or a block picked some other way. */
  cancel(): void {
    if (this.timer != null) this.timers.clear(this.timer);
    this.timer = null;
    this.target = -1;
    this.origin = -1;
  }
}

/** ★★★ THE LABEL DOES NOT SNAP BACK TO THE BLOCK WE LEFT (Stuart, 2026-10-21: "I tried to rapid tune
 *  the Pi2 earlier and it kept snapping back"). After we send a block, `dab` reports naming the block we
 *  LEFT keep arriving for a while — already on the wire, built by the server's DSP thread before our
 *  request was read, or queued behind a slow reader (a Pi 2 is all three) — and "follow the server's
 *  block" adopted each one, dragging the label back under the user's thumb.
 *  ★ So, from our send: a report naming a block we have just LEFT (the one we were on, or an earlier
 *    ask this one replaced) is DROPPED, whole — its station list belongs to that block too, and a stale
 *    list you can tap is worse than an empty one. It ends when the server names the block we asked for,
 *    or when DAB_ASK_HOLD_MS passes (the request failed, or someone moved the dial back — then the
 *    server is right and we follow it), or at once when a report names a THIRD block: somebody else
 *    genuinely moved the shared dial, and that is adopted immediately.
 *  ★★★ THE SHARED-DIAL CONTRACT: this only holds the DISPLAY. Nothing is ever re-sent to "correct" the
 *    server — recovery is not a user action; the server owns the state and we adopt it.
 *  ★ Jr has had the same filter since 2026-09-10 (UberClient dabWantIndex, 6 s): "it jumped from 5a -
 *    10a and for some reason started showing the 9A stations". Same bound, so ONE RULE, THREE READERS. */
export const DAB_ASK_HOLD_MS = 6000;

export class DabAskGuard {
  private asked = -1;
  private until = 0;
  private readonly left = new Set<number>();
  /** The block we are waiting for the server to name, or -1. */
  pending(): number { return this.asked; }
  /** We sent block `block`; `from` is the block the server last reported (-1 = none known). */
  sent(block: number, from: number, now: number): void {
    if (from >= 0 && from !== block) this.left.add(from);
    if (this.asked >= 0 && this.asked !== block) this.left.add(this.asked);
    this.left.delete(block);
    this.asked = block;
    this.until = now + DAB_ASK_HOLD_MS;
  }
  /** Leaving DAB, entering it with no block, a reconnect — nothing of ours is in flight any more. */
  clear(): void { this.asked = -1; this.left.clear(); }
  /** Should a report naming block `block` be believed now? (block < 0 = a report with no block.) */
  accept(block: number, now: number): boolean {
    if (this.asked < 0 || block < 0) return true;
    if (block === this.asked || now >= this.until || !this.left.has(block)) { this.clear(); return true; }
    return false;
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

/** ★★★ A `dab` REPORT THAT ARRIVES AFTER WE LEFT IS A GHOST (Stuart, 2026-10-05 23:06, iPhone → Pi 2:
 *  "I pressed the Exit DAB button which took me back to MW … but for some reason after a few seconds the
 *  DAB decoder box popped up again this time over the MW signal and needed to have exit DAB pressed
 *  again. It's been a bug for a little while now, it sometimes works normally though.")
 *  The server reports DAB twice a second from its DSP thread, so a report already on the wire when our
 *  `dab off` lands — or, on a server before the fix, one sent AFTER its `dab_off` — reaches us out of DAB.
 *  The report handler reads "a report while we are out" as "somebody put this receiver on a multiplex"
 *  (right for a shared dial) and opened the box again. Then the box's EXIT, a toggle, saw "not in DAB"
 *  and ENTERED it: the Pi's journal has "mode ON: channel 13B" ten seconds after the off with no tune,
 *  no landing and no resume — the last block, because a toggle sends none. That is the "needed to
 *  press exit again": the first press put him back in.
 *  ★ So reports are DROPPED from our own `dab off` until the server's `dab_off` (bounded, in case that
 *    never comes), and for a short tail after it for a server that still sends one late. Our own
 *    entry ends the quiet at once. Somebody ELSE entering DAB on a shared dial is still seen — the
 *    reports keep coming twice a second and the first one after the quiet opens the box (the same
 *    holds for a reconnect inside the quiet: its reports get through once it ends).
 *  Pure, with the clock passed in, so scripts/test_dab_stepper.ts drives it. */
export const DAB_EXIT_AWAIT_OFF_MS = 5000;   // our off sent, the server's dab_off not seen yet
export const DAB_EXIT_TAIL_MS = 1500;        // dab_off seen: stragglers from a server that races

export class DabExitGuard {
  private quietUntil = 0;
  /** We sent `dab off`. */
  left(now: number): void { this.quietUntil = now + DAB_EXIT_AWAIT_OFF_MS; }
  /** We sent `dab on` (any block) — our own entry is never a ghost. */
  entered(): void { this.quietUntil = 0; }
  /** The server said `dab_off`. Only shortens a quiet we started; a server-driven exit opens none. */
  offConfirmed(now: number): void {
    if (this.quietUntil > now) this.quietUntil = Math.min(this.quietUntil, now + DAB_EXIT_TAIL_MS);
  }
  /** Should a `dab` report arriving now be believed? */
  accept(now: number): boolean { return now >= this.quietUntil; }
}

/** ★★★ THE DAB BOX'S EXIT NEVER ENTERS DAB. It was wired to the DAB toggle, and a toggle pressed while
 *  the app thinks it is out of DAB goes IN — so any box left on screen out of DAB (a ghost report, a
 *  `dab_off` that did not close it) turned EXIT into ENTER. 'leave' sends `dab off`; 'close' only puts
 *  the box away (there is nothing to leave). */
export function dabExitAction(dabOn: boolean): 'leave' | 'close' {
  return dabOn ? 'leave' : 'close';
}
