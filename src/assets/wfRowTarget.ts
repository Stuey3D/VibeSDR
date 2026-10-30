/**
 * wfRowTarget.ts — HOW MANY WATERFALL ROWS A SECOND, FROM THE FEED THAT IS ACTUALLY ARRIVING.
 * ONE rule for the app (WaterfallView JS path), the web client (web/client/src/wfPacing.ts) and — ported line for
 * line — the native Android engine (android/app/src/main/cpp/spectrum/wf_row_target.h). Pure, so
 * scripts/test_wf_row_target.ts proves every row of the table below and the hysteresis.
 *
 * ★★★ Stuart 10-20: "60 rows/s from a 5 fps feed (x12) is too much" — he proposed 5 / 15 / 30 for 5 fps servers.
 *     As one rule that follows the MEASURED feed rate:
 *       Sharp   = actual rows (x1).
 *       Default = min(20, 3 x feed fps).
 *       Smooth  = min(60, 6 x feed fps).
 *     Each target snaps DOWN to the nearest rate that divides 60 evenly — {10, 12, 15, 20, 30, 60}. (★ 2026-10-29: a
 *     feed faster than the rate is no longer shown as it comes — see nominalRowRate: Default/Smooth go UP to the next
 *     rate, Eco/Minimum THIN it.) Every row on screen is a REAL frame, repeated, never a
 *     blend (7bce170d), and the repeat factor never exceeds 6.
 *
 *       feed fps    Sharp   Default   Smooth
 *       5           5       15        30
 *       10          10      20        60
 *       20          20      20 (x1)   60
 *       3.3 UberSDR 3.3     10        20
 *       ~9 OWRX     9       20        30        (54 snaps to 30)
 *
 * ★★ HYSTERESIS ON THE FEED, NOT THE OUTPUT. The target is the rule applied to a LATCHED feed rate, and the latch moves
 *    only when the measured rate (the caller's smoothed estimate) has really moved: at once past ±15 %, or after 20
 *    frames in a row more than 4 % away. So a feed wobbling between 9 and 10 fps cannot flip Smooth between 30 and 60,
 *    a governor step 10 → 5 changes the target ONCE, and a feed that settles at 10 after climbing from 5 still ends on
 *    the 60 a fresh start at 10 gives (hysteresis on the output got stuck at 30 there — 6 x 10 is exactly 60).
 */

/* ★★ V12 ECO (Stuart 2026-10-28, src/assets/wfSpeedCap.ts): "a good blend of smooth and data savings" — EXACTLY 10 rows/s.
 *  The LINK is capped at 10 (a 20 fps server sends half); a slower feed is filled in to 10 by the same over-generate-and-
 *  discard pacing Default and Smooth use ("the interpolation always over interpolates and discards leaving headroom to
 *  account for link rate judder") — 9 fps → 10, 5 → 10, 3.3 → 10, never 9 × 2 = 18 or a feed multiple. */
/* ★ V12 LOW (was MINIMUM until 2026-10-30 — stored 'low' now; an old saved 'minimum' loads as LOW, wfSpeedCap.ts
 *  loadWfSpeed): the server is ASKED for 5 fps and the rows are actual rows — EXCEPT on a feed below 5 (a 2 fps server,
 *  UberSDR's 3.3 emergency rung), which is filled in to an even 5 rows/s by the same rule. 5 is LOW's one rate. */
/* ★★ V12 MINIMUM, stored 'min2' (GitHub #57, Stuart 2026-10-30): 2 rows a second, the floor ("1fps would look like no
 *  motion at all"). The server is ASKED for 2 fps; a feed it cannot slow (a 3.3 fps UberSDR shared channel, an OpenWebRX
 *  ~9, a Kiwi's 5) is THINNED to an even 2 by the same rule, and a slower one filled in. 2 divides 60. */
export type WfScroll = 'min2' | 'low' | 'sharp' | 'eco' | 'default' | 'smooth';

/** Rates that divide 60 evenly (and so 120): each is a whole number of frames per row on 60/120 Hz panels. */
export const ROW_RATES = [10, 12, 15, 20, 30, 60] as const;
/** The cap and the multiplier for each setting. */
export const ROW_RULE: Record<Exclude<WfScroll, 'sharp'>, { cap: number; mult: number }> = {
  min2:    { cap: 2,  mult: 6 },   // MINIMUM
  low:     { cap: 5,  mult: 6 },   // LOW
  eco:     { cap: 10, mult: 6 },   // ★ mult 6 (the repeat limit) so even a 1.7 fps feed is lifted to 10
  default: { cap: 30, mult: 3 },   // ★ 30, was 20 (Stuart, 2026-10-29, a 10 fps UberSDR: "incoming is 10 we should be smoothing to 30")
  smooth:  { cap: 60, mult: 6 },
};
/** ±15 %: how far the measured feed may move before the target follows it. */
export const ROW_HYST = 0.15;

/** The scroll mode for a row-rate cap (WaterfallView carries the cap as targetRows): 0 sharp, 2 min2 (MINIMUM), 5 low,
 *  10 eco, 20 default, 60 smooth. */
export const scrollForCap = (cap: number): WfScroll =>
  (cap >= 60 ? 'smooth' : cap >= 20 ? 'default' : cap >= 10 ? 'eco' : cap >= 5 ? 'low' : cap > 0 ? 'min2' : 'sharp');
/** A target computed from a feed 3 % short still snaps up to the rate it was meant to be (3.33 fps x 6 = 19.98 → 20). */
const SNAP_SLACK = 1.03;

/** The snapped target for a feed of `fps`, before the actual-rows test: the largest ROW_RATE ≤ min(cap, mult × fps),
 *  or 0 when even 10 is too many. */
export function snappedTarget(scroll: WfScroll, fps: number): number {
  if (scroll === 'sharp' || !(fps > 0)) return 0;
  const { cap, mult } = ROW_RULE[scroll];
  const raw = Math.min(cap, mult * fps) * SNAP_SLACK;
  let best = 0;
  // ★★ DEFAULT NEVER RUNS AT 20 (Stuart, 2026-10-29: "its the 20 that is causing the frame pacing issues"; "incoming is
  //    10 we should be smoothing to 30"): its rates are 10, 15 and 30, and 3 × the feed goes UP to the next of them —
  //    3.3 fps → 10, 5 → 15, 6–30 → 30 — never past 6 × the feed (the repeat limit).
  if (scroll === 'default') {
    if (mult * fps * SNAP_SLACK < 10) return 0;   // ★ the idle floor: a 2 fps feed stays actual rows (3 × 2 < 10)
    const want = Math.min(cap, mult * fps) / SNAP_SLACK;
    for (const r of [30, 15, 10]) if (r >= want && r <= 6 * fps * SNAP_SLACK) best = r;
    return best || (raw >= 10 ? 10 : 0);
  }
  // ★ LOW's one rate, 5, is not a ROW_RATE (Default must not start lifting a 1.7 fps feed to 5); 5 divides 60 too.
  //   MINIMUM's one rate, 2, the same (V12 #57).
  for (const r of scroll === 'low' ? [5] : scroll === 'min2' ? [2] : ROW_RATES) if (r <= raw && r <= cap) best = r;
  return best;
}

/**
 * ★★★ ROWS FOR ONE ARRIVAL (WaterfallView handleFrame; the Android engine's twin in spectrum_engine.cpp). The rows a
 * data frame earns at `target` rows/s over `durMs` (the smoothed gap between arrivals), with a FRACTIONAL carry: the
 * arrival is row 1, the rest are repeats spread over the interval. The AVERAGE is exactly the target on any feed — 9 fps
 * at 10 rows/s is 1,1,1,1,1,1,1,1,2 — never a whole multiple of the feed (9 × 2 = 18). The display driver then releases
 * the rows on the display's beat from a playout buffer (utils/wfPace.ts rowPace), so the pace on screen is the target,
 * evenly. At most 8 extras (a stall must not repay as a burst). Returns the extras (rows beyond the arrival).
 */
export function carryRows(carry: { current: number }, durMs: number, target: number): number {
  if (!(target > 0) || !(durMs > 0)) return 0;
  carry.current += durMs / (1000 / target);
  let extras = Math.max(0, Math.floor(carry.current) - 1);
  carry.current -= Math.floor(carry.current);
  if (extras > 8) extras = 8;
  return extras;
}

/**
 * ★★★ ROWS FOR ONE ARRIVAL, FILLED IN OR THINNED (2026-10-29): the rows this data frame becomes at `target` rows/s over
 * `durMs` (the smoothed gap between arrivals), with a fractional carry — 0 = this frame is DISCARDED (a feed faster than
 * the rate: 23 fps at 20 rows/s keeps 6 frames in 7, evenly), 1 = the frame itself, more = the frame and repeats of it.
 * The average is exactly the target on any feed. `target` 0 (actual rows) = always 1. At most 9 (a stall must not repay
 * as a burst). carryRows (above) is the same count less the arrival, for a caller that always keeps the frame.
 */
export function rowsForArrival(carry: { current: number }, durMs: number, target: number): number {
  if (!(target > 0) || !(durMs > 0)) return 1;
  carry.current += durMs / (1000 / target);
  const n = Math.floor(carry.current);
  carry.current -= n;
  return Math.min(n, 9);
}

/** Latch state for one waterfall. `latched` = the feed rate the target was decided from (0 = none yet). */
export interface RowTargetState { latched: number; away: number; target: number; frames: number }
export const newRowTargetState = (): RowTargetState => ({ latched: 0, away: 0, target: 0, frames: 0 });
/** Frames at the start during which the latch simply follows the estimate: the caller's average starts from a guess
 *  (150 ms) and takes ~20 frames to forget it — latching that guess would hold a 10 fps feed at 30 for seconds. */
export const ROW_WARMUP_FRAMES = 20;
export const ROW_LATCH_FAST = ROW_HYST;   // relatch at once past ±15 %
export const ROW_LATCH_SLOW = 0.04;       // …or after ROW_LATCH_FRAMES frames in a row more than 4 % away
export const ROW_LATCH_FRAMES = 20;

/* ★★★ EVERY RATE DIVIDES 60 — FILLED IN OR THINNED (Stuart, 2026-10-29, a 23 fps KiwiSDR showing "ECO 13 · NATIVE 23 ·
 *  DEFAULT 23": "on Kiwi those FPS numbers look weird and won't line up with any refresh rate, so we need to interpolate
 *  and discard"). Every choice but NATIVE now runs at a rate that divides 60 (and 120), so each row gets a whole number
 *  of refreshes:
 *    MINIMUM  2   — a faster feed is THINNED to 2 (V12 #57: a server that cannot be asked for 2 fps), a slower one
 *                   filled in.
 *    LOW      5   — a faster feed is THINNED to 5 (rows discarded evenly), a slower one filled in.
 *    ECO      10  — the same: a 13 fps Kiwi (its wf_speed 3) is thinned to 10, a 5 fps server filled in to 10.
 *    DEFAULT  min(20, 3 × feed), snapped — but the interpolated pair NEVER drops real frames: a feed faster than that
 *             goes UP to the next rate that divides 60 (23 fps → 30, filled in), and only a feed above 60 is thinned.
 *    SMOOTH   min(60, 6 × feed), snapped; above 60, thinned to 60.
 *    NATIVE   unchanged — the server's own rate, every row a real frame, no snapping (Stuart: "native will stay at a
 *             weird fps").
 *  A feed within 2.5 % of the rate is simply shown as it comes (actual rows, ROW_ACTUAL_TOL).
 *    feed      MIN LOW  ECO  DEFAULT  SMOOTH        feed      MIN LOW  ECO  DEFAULT  SMOOTH
 *    3.3       2   5    10   10       20            20        2   5    10   20 (x1) 60
 *    5         2   5(x1)10   15       30            23 Kiwi   2   5    10   30       60
 *    9 OWRX    2   5    10   20       30            30        2   5    10   30 (x1) 60
 *    10        2   5    10(x1)20      60            ~13 Kiwi ECO          10 (thinned)          */
/** ★ A feed this close to the rate is shown as it comes (actual rows): 2.5 % — a 20 fps server measured at 19.8 is not
 *  worth a repeat every 5 s, but a 21 fps one (the Lenovo) is a slip every second and is thinned/filled to a divisor. */
export const ROW_ACTUAL_TOL = 0.025;
/** The rate a choice runs at on a feed of `fps` (rows/s, always a divisor of 60 — 0 for NATIVE or no feed), before the
 *  actual-rows test. This is the number the button shows. */
export function nominalRowRate(scroll: WfScroll, fps: number): number {
  if (scroll === 'sharp' || !(fps > 0)) return 0;
  let t = snappedTarget(scroll, fps);
  if ((scroll === 'default' || scroll === 'smooth') && t > 0 && t < fps / (1 + ROW_ACTUAL_TOL)) {   // (0 = a feed too slow to fill: actual rows)
    t = 60;                                                   // the interpolated pair never thins below the feed…
    for (let i = ROW_RATES.length - 1; i >= 0; i--) if (ROW_RATES[i] >= fps / (1 + ROW_ACTUAL_TOL)) t = ROW_RATES[i];   // …the next rate up
  }
  return t;
}

/** The rule on a settled feed rate: rows/s, or 0 = actual rows (a rate within 2.5 % of the feed is the feed). Above the
 *  feed the rows are filled in (repeats); below it they are THINNED (rowsForArrival discards evenly). */
export function rowTargetFor(scroll: WfScroll, fps: number): number {
  const s = nominalRowRate(scroll, fps);
  return s > 0 && Math.abs(s - fps) > fps * ROW_ACTUAL_TOL ? s : 0;
}

/**
 * Advance one waterfall's target by one data frame. `feedFps` = the caller's SMOOTHED feed estimate (avgFrameMs).
 * Returns rows per second, or 0 for ACTUAL ROWS (one row per received frame — no repeats).
 */
export function stepRowTarget(st: RowTargetState, scroll: WfScroll, feedFps: number): number {
  if (!(feedFps > 0)) return st.target;
  if (st.latched <= 0 || ++st.frames <= ROW_WARMUP_FRAMES) { st.latched = feedFps; st.away = 0; }
  else {
    const dev = Math.abs(feedFps - st.latched) / st.latched;
    if (dev > ROW_LATCH_FAST) { st.latched = feedFps; st.away = 0; }
    else if (dev > ROW_LATCH_SLOW) { if (++st.away >= ROW_LATCH_FRAMES) { st.latched = feedFps; st.away = 0; } }
    else st.away = 0;
  }
  st.target = rowTargetFor(scroll, st.latched);
  return st.target;
}

/** ★★ UNDER A DISPLAY CAP BELOW 60 (Stuart, 2026-10-28: "in 30fps lock mode no point running a 60fps waterfall"): the
 *  target drops to the largest ROW_RATE that is ≤ the target, ≤ the cap and DIVIDES the cap — 60 → 30, 20 → 15 (20 on a
 *  30 Hz display alternates one refresh and two), 30/15/10 kept, 12 → 10 — so every row gets the same number of
 *  refreshes. 0 (actual rows) when that is no more than the feed. `capHz` 0 or ≥ 60 = unchanged (every ROW_RATE
 *  divides 60 and 120). */
export function rowTargetUnderCap(target: number, feedFps: number, capHz: number): number {
  if (!(target > 0) || !(capHz > 0) || capHz >= 60) return target;
  let best = 0;
  for (const r of ROW_RATES) if (r <= target && r <= capHz && capHz % r === 0) best = r;
  // ★ 2026-10-29: below the feed too — the rows are thinned to it (Smooth 60 on a 45 fps feed under a 30 cap → 30).
  return best > 0 && Math.abs(best - feedFps) > feedFps * ROW_ACTUAL_TOL ? best : 0;
}
