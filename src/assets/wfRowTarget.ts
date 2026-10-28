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
 *     Each target snaps DOWN to the nearest rate that divides 60 evenly — {10, 12, 15, 20, 30, 60} — and is never
 *     below the feed itself (then it is simply actual rows). Every row on screen is a REAL frame, repeated, never a
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

export type WfScroll = 'sharp' | 'default' | 'smooth';

/** Rates that divide 60 evenly (and so 120): each is a whole number of frames per row on 60/120 Hz panels. */
export const ROW_RATES = [10, 12, 15, 20, 30, 60] as const;
/** The cap and the multiplier for each setting. */
export const ROW_RULE: Record<Exclude<WfScroll, 'sharp'>, { cap: number; mult: number }> = {
  default: { cap: 20, mult: 3 },
  smooth:  { cap: 60, mult: 6 },
};
/** ±15 %: how far the measured feed may move before the target follows it. */
export const ROW_HYST = 0.15;
/** A target computed from a feed 3 % short still snaps up to the rate it was meant to be (3.33 fps x 6 = 19.98 → 20). */
const SNAP_SLACK = 1.03;

/** The snapped target for a feed of `fps`, before the actual-rows test: the largest ROW_RATE ≤ min(cap, mult × fps),
 *  or 0 when even 10 is too many. */
export function snappedTarget(scroll: WfScroll, fps: number): number {
  if (scroll === 'sharp' || !(fps > 0)) return 0;
  const { cap, mult } = ROW_RULE[scroll];
  const raw = Math.min(cap, mult * fps) * SNAP_SLACK;
  let best = 0;
  for (const r of ROW_RATES) if (r <= raw && r <= cap) best = r;
  return best;
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

/** The rule on a settled feed rate: rows/s, or 0 = actual rows — a target within 5 % of the feed is the feed (a
 *  20 fps server measured at 19.8 is not worth a repeated row every 2.5 s). */
export function rowTargetFor(scroll: WfScroll, fps: number): number {
  const s = snappedTarget(scroll, fps);
  return s > fps * 1.05 ? s : 0;
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
  return best > feedFps * 1.05 ? best : 0;
}
