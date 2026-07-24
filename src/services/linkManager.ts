// ADAPTIVE WATERFALL RATE — one controller, every backend.
//
// A DIRECT PORT of the wrist's `spike/WristSDR/WristSDR/LinkManager.swift`, which has been
// field-validated on Bluetooth against real servers. The constants below are MEASURED, not chosen,
// and every ★ note records something that was got wrong first. Port faithfully; do not "improve"
// the numbers without new measurements, and keep the two files in step.
//
// Ask the server for fewer waterfall frames when the link cannot carry them, and step back up when
// it recovers. The waterfall interpolates onto its own render clock, so a lower frame rate costs
// TIME RESOLUTION, not scroll smoothness — which is the whole reason this is a good trade. A
// stuttering link is visible and ugly; a slower one mostly is not.
//
// Every backend has the same shape (a rate lever and a 1s frame counter) and a different ladder, so
// the policy lives here once and each client supplies only its own rungs. Measured rates:
//
//   UberSDR    `set_rate` divisor 1/2/3  →  10 / 5 / 3.3 fps   (12.4 / 6.1 / 4.2 KB/s)
//   KiwiSDR    `wf_speed`  4/3/2         →  23 / 13 / 5  fps
//   VibeServer `fftRate`   20/10/5       →  20 / 10 / 5  fps
//   OpenWebRX  — no lever at all; fps/fft_fps/fft_size are ignored. No LinkManager.

/** What the user asked for. Not a boolean, because "slow on purpose" and "slow because the link is
 *  bad" are different states that must look different in the UI. */
export type LinkMode = 'full' | 'adaptive' | 'lowData';

export const LADDERS: Record<string, number[]> = {
  ubersdr:    [10, 5, 3.3],
  vibeserver: [20, 10, 5],
  kiwi:       [23, 13, 5],
};

/** Degrade fast, recover slow — asymmetric on purpose. A wrong step DOWN costs a little time
 *  resolution nobody notices; a wrong step UP costs a visible stutter. */
const DEGRADE_AFTER = 3;    // seconds below starveRatio
const RECOVER_AFTER = 20;   // seconds above healthyRatio, AFTER a real failure

/**
 * ★ THE FIRST CLIMB IS NOT A RECOVERY, so it does not deserve the same caution.
 *
 * RECOVER_AFTER is 20s because stepping up after a genuine failure risks re-triggering it —
 * oscillation is worse than staying low. But at connect we have not failed; we are simply being
 * careful, and 20s of deliberately-throttled waterfall on a perfectly good link is a poor first
 * impression.
 *
 * ★★ BUT NOT TOO SHORT — IT MUST OUTLAST THE CONNECT BURST. Servers dump a backlog on connect and
 *    MEASURED it is large: the same Kiwi read 22 KB/s over a window starting at connect and
 *    4.5 KB/s once ~5s of settle was discarded. That burst is dangerous precisely because it looks
 *    GOOD — a short probe would climb on the strength of data the link was never really carrying,
 *    arriving at full rate just as the burst ends. 8s clears the measured spike with margin.
 */
const FIRST_CLIMB_AFTER = 8;

const STARVE_RATIO  = 0.6;
const HEALTHY_RATIO = 0.85;

/**
 * ★ `framesPerSec` is an INTEGER COUNT in a 1s window, and that quantisation is brutal at low
 * rungs: at 5fps one late frame gives 4/5 = 0.80, which lands in the hold band and resets the
 * healthy counter. A single late frame per second therefore blocks recovery FOREVER — the ladder
 * holds a lower rung on a link that is actually fine (observed on-wrist, UberSDR, 2026-07-19).
 * Averaging over several seconds turns ±1 frame from ±20% into ±4%. Starvation still uses the
 * instantaneous value, so stepping DOWN stays fast.
 */
const RECOVERY_WINDOW = 5;

export class LinkManager {
  /** Expected fps at each rung, rung 1 (full rate) first. The LAST rung is the adaptive floor. */
  private ladder: number[];
  /** The deepest rung a USER may pin via Low Data. Rungs below it are ADAPTIVE-ONLY — UberSDR's
   *  3.3fps rung is jerky and reserved for a genuinely poor connection, never a user preference.
   *  Stuart: "low data rate minimum is 5fps as the interpolation can hide that." */
  private lowDataRung: number;
  private apply: (rung: number, fps: number) => void;
  private mode: LinkMode;

  /** The rate actually requested (1 = full). Includes a user-pinned Low Data floor. */
  rung = 1;
  /** How far the CONTROLLER has had to back off. Stays 1 in Low Data mode: a rate the user chose is
   *  a preference, not a symptom, and must never light the link indicator red. */
  adaptiveRung = 1;
  /** ★ STILL WORKING OUT WHAT THIS LINK CAN CARRY. True from connect until the rate is DECIDED —
   *  either by climbing (the link proved good) or by starving (it proved poor). The UI draws an
   *  indeterminate indicator meanwhile, because that is the truth: a settled bar count we have not
   *  earned would be a guess dressed as a measurement. Only meaningful in adaptive mode. */
  settling: boolean;

  private starvedSecs = 0;
  private healthySecs = 0;
  private recent: number[] = [];
  /** Has this session ever genuinely starved? Distinguishes "cautious start" from "recovering". */
  private everStarved = false;

  /**
   * ★★ START IN THE MIDDLE, EARN THE TOP. Connection setup is the most fragile moment there is —
   *    handshake, buffers filling, DSP spinning up, audio starting — and asking for the MAXIMUM
   *    frame rate exactly then is what tips a marginal link over.
   *
   *    So open on the middle rung and climb only once the link has proved it can carry it. The cost
   *    is nearly nothing: the waterfall interpolates regardless, so a lower rate still SCROLLS
   *    smoothly — what you briefly lose is time resolution, not fluidity. It also makes the
   *    indicator honest from the first second.
   *
   * ★★ THE USER'S CHOICE OUTRANKS THE CAUTIOUS START COMPLETELY. Auto Link Management off means
   *    off — opening throttled on a link the user told us not to manage is worse than the stutter
   *    it avoids. Low Data likewise opens on its pinned rung. Only 'adaptive' gets the mid start.
   */
  constructor(opts: {
    ladder: number[];
    lowDataRung: number;
    mode: LinkMode;
    startRung?: number;
    apply: (rung: number, fps: number) => void;
  }) {
    this.ladder = opts.ladder;
    this.lowDataRung = Math.min(Math.max(1, opts.lowDataRung), opts.ladder.length);
    this.apply = opts.apply;
    this.mode = opts.mode;

    if (opts.mode === 'full') this.rung = 1;
    else if (opts.mode === 'lowData') this.rung = this.lowDataRung;
    else {
      // ★ NEVER OPEN ON THE BOTTOM RUNG: "we don't want everything looking garbage as that gives a
      //   bad first impression" — so the cautious start is at most one rung above the floor.
      const start = opts.startRung ?? 2;
      this.rung = Math.min(Math.max(1, start), Math.max(1, opts.ladder.length - 1));
    }
    this.adaptiveRung = opts.mode === 'adaptive' ? this.rung : 1;
    this.settling = opts.mode === 'adaptive';
  }

  /** The fps this rung is expected to deliver — for seeding the waterfall's cadence. */
  get expectedFps(): number { return this.ladder[this.rung - 1] ?? 0; }

  /**
   * Call once a second with the observed frame rate. `settled` is false while a tune/zoom
   * re-subscription is in flight — frames legitimately pause there and it must not read as a bad
   * link. `live` is false when there is no working session to judge.
   */
  /** Change the user mode LIVE (Auto / Full / Low Data). Applies the pinned rung immediately so the
   *  rate change is felt at once rather than only on the next tick. Was never wired — toggling Low
   *  Data updated the client field but not the running controller, so it stayed at the adaptive rate
   *  (Stuart 2026-07-24: Low Data ignored, held 10fps). */
  setMode(mode: LinkMode): void {
    if (this.mode === mode) return;
    this.mode = mode;
    // FORCE the apply. The wire may already disagree with our rung: the adaptive start opens at
    // rung 2 WITHOUT applying (the server sits at its default divisor 1 = full rate), so toggling
    // Low Data while rung is still 2 would hit set()'s rung-unchanged guard and NEVER send the
    // divisor — the controller thinks it is at 5fps while the wire holds 10 (Stuart 2026-07-24,
    // "the FPS always climbs back to 10, like we aren't sending the divisor").
    if (mode === 'full')          this.set(1, false, true);
    else if (mode === 'lowData')  this.set(this.lowDataRung, false, true);
    else                          this.settling = true;   // adaptive: re-evaluate from here
  }

  tick(fps: number, live: boolean, settled: boolean): void {
    if (this.ladder.length <= 1) return;          // backend has no lever (OWRX)

    // Pinned modes RE-ASSERT every tick (force), not just on a rung change. The app has other code
    // paths that write the rate directly — idle-wake / markInteract call client.setRate(1) — and
    // those desync us: the wire goes to divisor 1 (10fps) while our rung still reads 2, so a plain
    // set() early-returns and never corrects it. Forcing the apply each second means nothing can
    // hold a pinned mode off its rate for more than ~1s (Stuart 2026-07-24: Low Data held 5fps for
    // ~30s then a wake knocked it back to 10 and it stuck).
    if (this.mode === 'full')    { this.set(1, false, true); return; }
    if (this.mode === 'lowData') { this.set(this.lowDataRung, false, true); return; }  // pinned by choice

    if (!live) return;
    if (!settled) { this.starvedSecs = 0; return; }

    const expected = this.ladder[this.rung - 1];
    const ratio = expected > 0 ? fps / expected : 1;

    this.recent.push(fps);
    if (this.recent.length > RECOVERY_WINDOW) this.recent.splice(0, this.recent.length - RECOVERY_WINDOW);
    const avg = this.recent.length ? this.recent.reduce((a, b) => a + b, 0) / this.recent.length : fps;
    const avgRatio = expected > 0 ? avg / expected : 1;

    if (ratio < STARVE_RATIO) {
      this.starvedSecs++; this.healthySecs = 0;
      if (this.starvedSecs >= DEGRADE_AFTER && this.rung < this.ladder.length) {
        this.everStarved = true;   // from here on, climbing back is a RECOVERY: be slow about it
        this.settling = false;     // decided: this link is poor
        this.set(this.rung + 1, true);
        this.starvedSecs = 0;
      }
    } else if (avgRatio >= HEALTHY_RATIO) {
      this.healthySecs++; this.starvedSecs = 0;
      const needed = this.everStarved ? RECOVER_AFTER : FIRST_CLIMB_AFTER;
      if (this.healthySecs >= needed) {
        this.settling = false;     // decided: we know what this link will carry
        if (this.rung > 1) { this.set(this.rung - 1, true); this.healthySecs = 0; }
      }
    } else {
      this.starvedSecs = 0; this.healthySecs = 0;   // in between — hold this rung
    }
  }

  /** Re-assert the current rung — call after a reconnect, where the server starts at its default. */
  reassert(): void { if (this.rung !== 1) this.apply(this.rung, this.ladder[this.rung - 1]); }

  /**
   * ★ THE OWNER'S CEILING IS NOT A FAILURE. A server may cap its frame rate (VibeServer's
   * `maxFftRate`). Without this the controller asks for 20, receives the permitted 10, reads
   * 10/20 = 0.5 as starvation and steps down forever chasing a limit it can never reach — landing
   * at the floor on a perfectly good link. Clamping the LADDER instead means the top rung simply
   * becomes what the owner allows, and the ratios are honest again.
   */
  applyServerCeiling(maxFps: number): void {
    if (!(maxFps > 0)) return;
    const clamped = this.ladder.map(f => Math.min(f, maxFps));
    // Rungs that collapse onto the same rate are no longer distinct steps; keep one of each.
    const deduped = clamped.filter((f, i) => i === 0 || f < clamped[i - 1]);
    if (deduped.length === this.ladder.length && deduped.every((f, i) => f === this.ladder[i])) return;
    this.ladder = deduped;
    this.rung = Math.min(this.rung, this.ladder.length);
    this.lowDataRung = Math.min(this.lowDataRung, this.ladder.length);
    this.recent = [];
    this.apply(this.rung, this.ladder[this.rung - 1]);
  }

  private set(r: number, adaptive: boolean, force = false): void {
    const clamped = Math.min(Math.max(1, r), this.ladder.length);
    this.adaptiveRung = adaptive ? clamped : 1;
    if (clamped === this.rung && !force) return;
    this.rung = clamped;
    this.starvedSecs = 0; this.healthySecs = 0;
    this.recent = [];       // the old rung's counts say nothing about the new one
    this.apply(clamped, this.ladder[clamped - 1]);
  }
}
