/**
 * wfClock.ts — THE WATERFALL'S FIXED OUTPUT CLOCK: over-interpolate and discard (V12, 2026-10-29).
 * ONE rule for the app's JS path (WaterfallView), the web client (web/client/src/wfPacing.ts RowPacer) and — ported
 * line for line — the Android engine (android/app/src/main/cpp/spectrum/wf_row_clock.h). Pure, so
 * scripts/test_wf_fixed_clock.ts can drive it with simulated feeds and displays.
 *
 * ★★★ STUART (2026-10-29): "if we are only doing a straight 2x 3x interpolation an incoming rate that drifts about will
 *     make it feel juddery which is why I said to over interpolate and discard to avoid this issue" — and, in the Eco
 *     note (wfRowTarget.ts / wfSpeedCap.ts): "the interpolation always over interpolates and discards leaving headroom
 *     to account for link rate judder". His UberSDR's feed is "switching between 9-11fps", and "say an incoming drops
 *     to 9 then that is a 2 frame drop".
 *
 * WHAT IT REPLACES. Rows were made PER ARRIVAL (wfRowTarget rowsForArrival: each frame earned durMs / rowMs rows), so
 * the row SUPPLY followed the feed: a late frame emptied the display's playout buffer and the scroll stalled or
 * slipped — a hitch. Native at 10 fps has a whole 100 ms row of cover; the interpolated modes had about 40 ms.
 *
 * THE DESIGN.
 *  1. THE OUTPUT RATE IS FIXED at the chosen row rate (Minimum 2, Low 5, Eco 10, Default 30, Smooth 60 — the rule in
 *     wfRowTarget.ts decides it from the feed, latched; clockRowRate below keeps it from flapping). Rows come off a
 *     steady clock (tick k at t0 + k·1000/R), never off an arrival.
 *  2. EACH ROW IS THE MOST RECENT REAL FRAME presented by its tick — sample-and-hold, a REPEAT, never a blend (Stuart's
 *     rule 7bce170d). No new frame by the tick → the last one again. A late frame never stops the scroll; it costs one
 *     more repeat of the frame before it.
 *  3. SURPLUS IS DISCARDED: two frames presented within one tick → the older is dropped (a feed faster than the rate
 *     is thinned evenly, as the 2026-10-29 thinning did). Repeats absorb drift — never a pause, never a burst.
 *  4. A SMALL DELAY: each frame is PRESENTED on a de-jittered schedule — base + delay, where `base` follows the feed's
 *     mean arrival time (a slow loop on the measured interval) — so a frame that is a little late is still used on
 *     time, and a steady 10 fps feed at 30 rows/s gives every frame exactly 3 rows rather than 2, 4, 3 as the jitter
 *     lands. A frame later than the delay is presented when it arrives (one more repeat of the one before), and never
 *     later than delay + a quarter interval after it arrived. The delay is 0.2 × the feed interval, ≤ 40 ms (CLOCK_CFG
 *     — the knee of the sweep in scripts/test_wf_fixed_clock.ts: 0 → 0.2 took the frames on their modal repeat count
 *     from 69 % to 75 % on the web for +13 ms; 0.3 added 7 ms for 0.6 %).
 *  5. A DEAD LINK STILL STOPS: a frame is held for at most holdLimitMs (2.5 intervals + 100 ms) past its presentation
 *     — after that the clock's ticks pass with no row (the waterfall stops honestly rather than repeating one row down
 *     the screen), and resume on the next frame at the next tick, with no catch-up burst.
 *  NATIVE (Sharp) is NOT on this clock: one real row per frame, as it comes (Stuart: "native is pure unmodified lines
 *  direct from the server no modification at all").
 */
import { nominalRowRate, snappedTarget, stepRowTarget, ROW_HYST, ROW_RATES, type RowTargetState, type WfScroll } from './wfRowTarget.ts';   // ★ .ts: node runs this file directly (the tests)

/** The presentation delay as a share of the feed interval, and its ceiling (a 3.3 fps UberSDR is 300 ms a frame) —
 *  picked from the simulation (scripts/test_wf_fixed_clock.ts prints the sweep). Mutable only for that sweep. */
export const CLOCK_CFG = { delayFrac: 0.2, delayMaxMs: 40 };
/** The most frames waiting for their presentation time (more = a backlog: the oldest go). */
const CLOCK_QUEUE = 4;

export interface ClockFrame { id: number; p: number; h?: number }   // h: the A/V hold it entered with (wfAvSync.ts)
export interface RowClockState {
  // ── the feed's schedule (de-jitter) ──
  base: number;          // the schedule's last arrival time (follows the mean arrival), ms; 0 = none yet
  ivl: number;           // the feed interval, ms (smoothed)
  lastT: number;         // the last arrival, ms
  ps: number;            // the presentation schedule: when the last frame was due to be presented
  q: ClockFrame[];       // frames waiting for their presentation time, oldest first
  cur: number;           // the frame being held (−1 none)
  curP: number;          // its presentation time
  curH: number;          // ★ the A/V hold it entered with (wfAvSync.ts; 0 = none)
  hold: number;          // ★ the A/V hold the last arrival entered with
  // ── the output clock ──
  rate: number;          // rows/s (0 = off)
  t0: number;            // tick k is due at t0 + k·1000/rate
  k: number;             // the next tick
}
export const newRowClock = (): RowClockState =>
  ({ base: 0, ivl: 100, lastT: 0, ps: 0, q: [], cur: -1, curP: 0, curH: 0, hold: 0, rate: 0, t0: 0, k: 0 });

/** The presentation delay for a feed interval, ms. */
export const clockDelayMs = (ivl: number): number => Math.min(CLOCK_CFG.delayMaxMs, CLOCK_CFG.delayFrac * ivl);
/** How long one frame may be held past its presentation before the clock stops for a dead link. */
export const holdLimitMs = (ivl: number): number => Math.max(300, 2.5 * ivl + 100);

/** A real frame `id` arrived at `t` (ms). `instant` (a gesture): presented at once, the schedule restarts from it —
 *  tune/zoom must feel instant (the jitter buffer is bypassed then too). Returns its presentation time.
 *
 *  ★★★ V12 2026-10-30 `holdMs` — THE WATERFALL WAITS FOR THE SOUND (wfAvSync.ts). The frame enters the schedule `holdMs`
 *      later than it arrived; the schedule (base, ps) moves with the hold, the feed's interval is still measured on the
 *      REAL arrivals. Stuart (2026-10-30): "to prevent scrolling judder whilst the jitter buffer enables we can simply
 *      duplicate the previous lines if needed and then the waterfall still reacts to the audio in time but still stays
 *      smooth". So:
 *   • THE HOLD GROWS (re-engaging after a tune, the audio ratcheting deeper after a hiccup): the next frame is due later,
 *     and the ticks in between REPEAT the frame on screen — the path a late frame already takes. The scroll never stops:
 *     the dead-link limit is lengthened by exactly the growth since the held frame went in, so the repeats are not
 *     mistaken for a dead link.
 *   • THE HOLD SHRINKS: frames still waiting LATER than this newer one are DISCARDED — over-interpolate and discard, as
 *     the clock already does for a fast feed — never a burst. (A passthrough is a shrink to 0: everything still held goes.)
 *   • 0 throughout is the clock exactly as it was. The queue grows by the frames a hold keeps waiting (~10 at 400 ms on a
 *     23 fps feed). */
export function clockArrive(c: RowClockState, id: number, t0: number, instant = false, holdMs = 0): number {
  const dH = holdMs - c.hold;
  c.hold = holdMs;
  if (dH !== 0 && c.base > 0) { c.base += dH; c.ps += dH; }   // the schedule moves with the hold
  const t = t0 + holdMs;
  const gap = c.lastT > 0 ? t0 - c.lastT : 0;                 // the feed's interval: real arrivals
  c.lastT = t0;
  let p: number;
  if (instant || c.base <= 0 || !(gap > 0) || gap > 4 * c.ivl || gap > 2000) {
    // The first frame, a gesture or a resume after a pause: a fresh schedule from this arrival.
    if (gap > 0 && gap <= 2000 && c.base > 0 && !instant) c.ivl = c.ivl * 0.5 + Math.min(gap, 4 * c.ivl) * 0.5;
    c.base = t;
    p = instant ? t : t + clockDelayMs(c.ivl);
    c.ps = p;
  } else {
    // The interval: smoothed, one gap clipped to [½, 2] intervals (a late frame and the early one after it cancel).
    c.ivl = c.ivl * 0.9 + Math.max(0.5 * c.ivl, Math.min(2 * c.ivl, gap)) * 0.1;
    // The schedule: where this frame was expected, nudged toward where it came (a late frame pulls it only a little).
    const pred = c.base + c.ivl;
    const err = t - pred;
    c.base = pred + 0.15 * Math.max(-0.5 * c.ivl, Math.min(0.5 * c.ivl, err));
    if (c.base > t) c.base = Math.max(t, c.base - 0.5 * c.ivl);   // never schedule an early frame too far ahead
    const d = clockDelayMs(c.ivl);
    // ★ THE PRESENTATION SCHEDULE advances by exactly the smoothed interval, and follows the de-jittered arrival only
    //   when it strays outside a small deadband. Presented at base + d directly, the schedule's own few ms of noise
    //   flipped frames sitting near a row boundary between 2 and 4 rows (measured: 65–79 % of frames on the modal count
    //   at 10 fps → 30); on the schedule they fall in the feed's own pattern — 3,3,3… at 10 fps, 3,3,…,4 at 9.7.
    const want = c.base + d, dead = Math.min(0.15 * c.ivl, 10);
    c.ps += c.ivl;
    const e = want - c.ps;
    if (e > dead) c.ps += e - dead; else if (e < -dead) c.ps += e + dead;
    p = Math.min(Math.max(t, c.ps), t + d + 0.25 * c.ivl);
  }
  // ★ The hold shrank: frames still waiting later than this one are older than it — discarded, not shown after it.
  if (dH < 0) while (c.q.length && c.q[c.q.length - 1].p > p) c.q.pop();
  // Presentation times never go backwards (a frame cannot be shown before the one that came before it).
  const lastP = c.q.length ? c.q[c.q.length - 1].p : (c.cur >= 0 ? c.curP : -Infinity);
  if (p < lastP) p = lastP;
  c.q.push({ id, p, h: holdMs });
  const qMax = holdMs > 0 ? CLOCK_QUEUE + Math.ceil(holdMs / Math.max(10, c.ivl)) : CLOCK_QUEUE;
  while (c.q.length > qMax) c.q.shift();
  return p;
}

/** Set the output rate (rows/s; 0 = off). The phase runs on: the next tick is one NEW period after the last one, so a
 *  rate change never jumps or bursts. `now` starts the clock if it is not running. */
export function clockSetRate(c: RowClockState, rate: number, now: number): void {
  if (rate === c.rate) return;
  if (c.rate > 0 && rate > 0) {
    const last = c.t0 + (c.k - 1) * 1000 / c.rate;            // when the last tick was due
    c.t0 = Math.max(last + 1000 / rate, now); c.k = 0;        // tick 0 is the next one
  } else if (rate > 0) { c.t0 = now; c.k = 0; }
  c.rate = rate;
}

/** When the next tick is due (ms), or −1 when the clock is off. */
export const clockNextAt = (c: RowClockState): number => (c.rate > 0 ? c.t0 + c.k * 1000 / c.rate : -1);

/** Run the clock up to `now`: for every tick due, the frame it shows — pushed to `out` (−1 entries are never pushed: a
 *  tick with nothing to hold just passes). Returns the rows made. A caller that ran late (a busy JS thread) gets the
 *  ticks it missed, each with the frame that was presented by ITS time — but never more than `maxRows` (older ticks
 *  are skipped: a stall must not repay as a burst). */
export function clockRun(c: RowClockState, now: number, out: number[], maxRows = 6): number {
  if (!(c.rate > 0)) return 0;
  const per = 1000 / c.rate;
  let due = Math.floor((now - c.t0) / per + 1e-9) - c.k + 1;  // ticks with time ≤ now (a hair of slack for float)
  if (due <= 0) return 0;
  if (due > maxRows) { c.k += due - maxRows; due = maxRows; }
  let made = 0;
  for (let i = 0; i < due; i++) {
    const tt = c.t0 + c.k * per;
    c.k++;
    // Every frame presented by this tick: the newest is shown, the older ones are discarded.
    while (c.q.length && c.q[0].p <= tt) { const f = c.q.shift()!; c.cur = f.id; c.curP = f.p; c.curH = f.h ?? 0; }
    if (c.cur < 0) continue;
    // A dead link: stop, do not paint one row down the screen. ★ Lengthened by the A/V hold's growth since this frame went
    // in (clockArrive): those ticks are the repeats that carry the scroll while the hold grows, not a dead link.
    if (tt - c.curP > holdLimitMs(c.ivl) + Math.max(0, c.hold - c.curH)) continue;
    out.push(c.cur); made++;
  }
  // Rebase now and then so t0 + k·per keeps its precision.
  if (c.k > 1e6) { c.t0 += c.k * per; c.k = 0; }
  return made;
}

/** Forget the frames (background / covered / a new radio): the next arrival starts a fresh schedule. The output phase
 *  is kept. */
export function clockReset(c: RowClockState): void {
  c.q.length = 0; c.cur = -1; c.curP = 0; c.curH = 0; c.hold = 0; c.base = 0; c.lastT = 0; c.ps = 0;
}

/**
 * ★★★ THE CLOCK'S RATE FROM THE FEED — wfRowTarget's rule (nominalRowRate) on the latched feed, with ONE change: the
 * rate does not come DOWN while the feed still supports it within ROW_HYST (15 %). Stuart's UberSDR "switching between
 * 9-11fps": Smooth is 60 at 10 fps but 30 at 9 (6 × 9 = 54 snaps to 30), so a dip to 9 for a few seconds relatched the
 * feed and halved the scroll speed. The rate now steps down only once the feed is 15 % below what the rate needs
 * (Smooth 60 holds to ~8.4 fps, Default 30 to ~4.2), and steps UP as the rule says — Smooth taking a step the feed
 * is within 5 % of. Returns rows/s, 0 = actual rows
 * (a feed too slow for the setting to fill: Default under 3.3 fps).
 */
export function clockRowRate(st: RowTargetState & { clockRate?: number }, scroll: WfScroll, feedFps: number): number {
  if (scroll === 'sharp') return (st.clockRate = 0);
  stepRowTarget(st, scroll, feedFps);
  const fps = st.latched;
  let r = nominalRowRate(scroll, fps);
  // ★ SMOOTH: a feed a few % short of the rule's step still takes it. A cold start latched a 10 fps feed at 9.68 (the
  //   average still climbing from its 150 ms guess when the warm-up ended — measured in the simulation), and 6 × 9.68
  //   snapped DOWN to 30 for the rest of the session (the latch's own 4 % deadband kept it there). Only Smooth snaps
  //   down onto a step that way; Default already rounds UP (and 5 fps must stay 15, Stuart's 3 ×).
  if (scroll === 'smooth') { const up = snappedTarget(scroll, fps * 1.05); if (up > r) r = up; }
  const held = st.clockRate ?? 0;
  if (held > 0 && r < held && nominalRowRate(scroll, fps * (1 + ROW_HYST)) >= held) r = held;
  st.clockRate = r;
  return r;
}

/** Under a FRAME RATE cap below 60 (Stuart 2026-10-28: "in 30fps lock mode no point running a 60fps waterfall"): the
 *  largest ROW_RATE ≤ the rate that divides the cap (60 → 30; 30, 15, 10 kept). Unlike rowTargetUnderCap there is no
 *  "close to the feed → actual rows" test: on the clock, every non-Native mode runs at its rate. */
export function clockRateUnderCap(rate: number, capHz: number): number {
  if (!(rate > 0) || !(capHz > 0) || capHz >= 60) return rate;
  let best = 0;
  for (const r of [2, 5, ...ROW_RATES]) if (r <= rate && r <= capHz && capHz % r === 0) best = r;
  return best || rate;
}

/* ── THE DISPLAY SIDE: rows due on THIS display frame for an output of `rate` rows/s ──────────────────────────────────
 *  Used where rows are DRAWN on the display's frame (the web's RowPacer). Two ways, chosen by the panel:
 *   • LOCKED — the measured refresh is within 2 % of a whole multiple of the rate (60 Hz / 30, 120 / 60, a Mac sitting
 *     at 90 / 30): one row every m frames, counted in frames, so every step is the same (a5aab91c4's "display-quantised"
 *     rule). Kept until 5 % away (hysteresis — the median of
 *     1 ms-jittered timestamps wobbles ~1 %; a flip re-phases the count). The web draws at display time with nothing to
 *     drain, so a held lock only runs the rate up to 5 % off the mode's, evenly.
 *   • EXACT — anything else (the Mac panel that macOS walks between 90 and 100 Hz, 60 rows on 95 Hz): rows at exactly
 *     the rate by the frame's own time — steps of 3 and 4 frames on 95 Hz, as a whole-row display must — never more
 *     than the rate's share of one frame, and never more than one row banked (a hitch costs a row, not a burst).
 *  Either way the rate is the clock's, never the feed's. */
export interface DisplayClockState { m: number; cnt: number; acc: number; last: number }
export const newDisplayClock = (): DisplayClockState => ({ m: 0, cnt: 0, acc: 0.5, last: 0 });
export function displayClockStep(d: DisplayClockState, ts: number, rate: number, frameMs: number): number {
  const dt = d.last > 0 ? ts - d.last : frameMs;
  d.last = ts;
  if (!(rate > 0) || !(frameMs > 0)) return 0;
  const hz = 1000 / frameMs;
  const ratio = hz / rate, r = Math.round(ratio);
  let m = 0;
  if (r >= 1 && Math.abs(ratio - r) < (d.m === r ? 0.05 : 0.02) * r) m = r;
  if (m !== d.m) { d.m = m; d.cnt = m > 0 ? Math.min(m - 1, Math.floor(d.acc * m)) : 0; d.acc = 0.5; }
  if (m > 0) {
    d.cnt += Math.max(1, Math.round(dt / frameMs));           // a dropped display frame still counts
    if (d.cnt >= m) { d.cnt -= m; if (d.cnt >= m) d.cnt = m - 1; return 1; }
    return 0;
  }
  const maxPer = Math.max(1, Math.round(rate / hz));
  d.acc += Math.min(dt, 4 * frameMs) * rate / 1000;
  let n = Math.floor(d.acc);
  if (n > maxPer) n = maxPer;
  d.acc -= n;
  if (d.acc > 1) d.acc = 1;
  return n;
}

/** Nothing left to show: no frame waiting and the held one past its hold limit (a dead link, a paused feed) — the
 *  producer stops its timer and the next arrival restarts it (clockSetRate/clockArrive, then clockRun). */
export const clockIdle = (c: RowClockState, now: number): boolean =>
  c.q.length === 0 && (c.cur < 0 || now - c.curP > holdLimitMs(c.ivl) + Math.max(0, c.hold - c.curH));

/* ★★★ THE INTERACTION BOOST (Stuart, 2026-10-29): "I think the waterfall rate needs to increase during interaction to
 *  allow for smoothness, but we need to do it in a way that we dont try to pull down more data than a server can supply
 *  even if it means a 12x frame generation" — and, on a sweep at Smooth 60: "if the app is set to 120 we do 120".
 *  While a tune or zoom is in flight (and its settle), the CLOCK's row rate steps up to the display's maximum as the app
 *  is set (the FRAME RATE setting; Auto = the panel's), never above 120 and never above 12 × the measured feed. Every
 *  extra row is the newest frame already received, drawn at its own in-between tune/zoom position (sample-and-hold,
 *  never a blend) — the ease gets more rows, so a curve gets its detail. NOTHING is asked of the server: no speed, no
 *  fps, no re-subscribe. The display refresh is never held or changed (the 10-28 judder was a refresh hold; this is only
 *  the clock's tick rate, and clockSetRate changes it on a whole-row boundary with no jump). NATIVE is untouched: 0 in,
 *  0 out — "pure unmodified lines direct from the server" (Stuart: "I would rather have it look sharp and lacking detail
 *  than modify its behaviour").
 *  The rates are the ones a 60/120 Hz panel draws on whole frames: 120 (one row a refresh at 120 Hz), else 60, 30, 20,
 *  15, 12, 10, 5. On a panel between (the Mac's 90–100 Hz VRR) 60, drawn on the exact-time path. */
export const BOOST_MAX_RATE = 120;
export const BOOST_FEED_X = 12;
/** How long after the last move the boost holds (the settle), ms. */
export const BOOST_TAIL_MS = 750;
const BOOST_RATES = [120, 60, 30, 20, 15, 12, 10, 5];
/** The clock's rate while a tune/zoom is in flight: `base` (the setting's rate on the clock; 0 = Native/actual rows →
 *  0), `feedFps` the measured feed, `displayHz` the display's maximum as the app is set (0 = unknown → 60). Never lower
 *  than `base`. */
export function boostRowRate(base: number, feedFps: number, displayHz: number): number {
  if (!(base > 0)) return 0;                           // NATIVE / actual rows: untouched
  const hz = displayHz > 0 ? displayHz : 60;
  const lim = Math.min(BOOST_MAX_RATE, hz * 1.02, (feedFps > 0 ? feedFps : 0) * BOOST_FEED_X * 1.0001);
  for (const r of BOOST_RATES) {
    if (r > lim) continue;
    // whole display frames: at or above 119 Hz a 120 rate is one row a frame; otherwise the rate must divide 60
    if (r === 120 && hz < 119) continue;
    return Math.max(base, r);
  }
  return base;
}
/** The boost's state: when the last move was seen (ms; 0 = never). */
export interface BoostState { lastMove: number; on: boolean }
export const newBoost = (): BoostState => ({ lastMove: 0, on: false });
/** A tune or zoom moved the picture at `now` (an input, or a frame placed somewhere new). */
export function boostNote(b: BoostState, now: number): void { b.lastMove = now; b.on = true; }
/** Whether the boost holds at `now`: within the tail of the last move, or while an ease is still drawing (`easing`). */
export function boostActive(b: BoostState, now: number, easing: boolean): boolean {
  b.on = b.lastMove > 0 && (now - b.lastMove < BOOST_TAIL_MS || (b.on && easing));
  return b.on;
}
