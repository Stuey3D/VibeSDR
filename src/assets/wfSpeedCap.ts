/**
 * wfSpeedCap.ts — THE WATERFALL SPEED CHOICES THAT ASK THE SERVER FOR LESS: MINIMUM and ECO (V12, Stuart 2026-10-28).
 *
 * From a GitHub discussion: someone wanted the waterfall slower, and on a 20 fps server even Sharp scrolled 20 rows a
 * second. The control is now, slowest to smoothest:
 *
 *     MINIMUM · ECO · NATIVE · [ DEFAULT · SMOOTH ]   (the bracketed pair outlined: "interpolated for smoothness")
 *
 *   MINIMUM  caps the LINK at 5 fps  — the minimum waterfall rate anyone chooses ("low data rate minimum is 5fps").
 *            Actual rows: on a 5 fps server it is the same as NATIVE; a server slower than 5 (2 fps, UberSDR's 3.3
 *            emergency rung) is filled in to an even 5 rows/s.
 *   ECO      10 rows a second: caps the LINK at 10 fps (a 20 fps server sends half) AND fills a slower feed in to 10
 *            (9, 5, 3.3 fps → 10) by over-generating and discarding to an even 10/s, as Default and Smooth do.
 *   NATIVE   the server's own frame rate, every row a real frame. (Was SHARP — the stored value is still 'sharp', so a
 *            saved setting loads unchanged.)
 *   DEFAULT / SMOOTH  real frames with rows repeated between them for a smoother scroll (wfRowTarget.ts).
 *
 *   ★★★ The caps exist to SAVE DATA, so the SERVER sends fewer frames — the client never throws frames away. Rows under
 *       MINIMUM draws like NATIVE (one row per received frame) unless the feed is below 5.
 *   ★★  A cap is a CEILING on the link manager, not a mode and not a divider: AUTO keeps managing the link underneath
 *       it (still steps down on a poor link — UberSDR's 3.3 fps emergency rung included), FULL is capped to it, LOW
 *       DATA keeps its own rate up to the cap (5 stays 5, never 2.5). Choosing NATIVE/DEFAULT/SMOOTH lifts it at once.
 *   ★   On a server whose own rate is already at or below a cap, that choice has no effect — nothing less to give.
 *
 * ONE rule for the app (linkManager.ts / vibeLinkManager.ts, KiwiAdapter, MenuSheet) and the web client
 * (web/client/src/main.ts). Pure, so scripts/test_wf_speed_cap.ts proves it.
 */
import { rowTargetFor, type WfScroll } from './wfRowTarget.ts';

/** The waterfall speed the user picks — every one is also a row mode (wfRowTarget); 'sharp' is labelled NATIVE, and
 *  'minimum' / 'eco' are link caps as well. */
export type WfSpeedSetting = WfScroll;

/** The minimum waterfall rate — the floor a USER may choose (Low Data's rate), and MINIMUM's ceiling. */
export const MIN_WF_FPS = 5;
/** ECO's ceiling. */
export const ECO_WF_FPS = 10;

/** The link ceiling each capped choice asks for (fps). */
export const LINK_CAP: Readonly<Record<'minimum' | 'eco', number>> = { minimum: MIN_WF_FPS, eco: ECO_WF_FPS };

export const isWfSpeedSetting = (v: unknown): v is WfSpeedSetting =>
  v === 'sharp' || v === 'eco' || v === 'default' || v === 'smooth' || v === 'minimum';

/** The link ceiling for a setting, fps — 0 = none (NATIVE / DEFAULT / SMOOTH). */
export const linkCapFor = (s: WfSpeedSetting): number => (s === 'minimum' || s === 'eco' ? LINK_CAP[s] : 0);

/** Does a cap change anything on a server whose own top rate is `serverTopFps`? (MINIMUM on a 5 fps server: no.) */
export const capHasEffect = (capFps: number, serverTopFps: number): boolean => capFps > 0 && serverTopFps > capFps;

/** The fps to ask for under a cap — a CEILING: never above the cap, never above what was wanted. 0 = no cap. */
export const capFps = (wantedFps: number, cap: number): number => (cap > 0 ? Math.min(wantedFps, cap) : wantedFps);

/**
 * A link manager's ladder (fps per rung, full rate first, the last rung the adaptive floor) under a ceiling of `cap` fps.
 * Every rung is clamped to the cap and rungs that collapse together become one — VibeServer [20, 10, 5] → MINIMUM [5],
 * ECO [10, 5]; UberSDR [10, 5, 3.3] → MINIMUM [5, 3.3] (Auto may still use the 3.3 emergency rung below the cap), ECO
 * unchanged. The Low Data rung keeps its RATE, clamped the same way: 5 stays 5. `cap` 0 = untouched.
 */
export function capLadder(ladder: number[], lowDataRung: number, cap: number): { ladder: number[]; lowDataRung: number } {
  const low = Math.min(Math.max(1, lowDataRung), ladder.length);
  if (!(cap > 0) || ladder.length === 0) return { ladder: ladder.slice(), lowDataRung: low };
  const clamped = ladder.map(f => Math.min(f, cap));
  const out = clamped.filter((f, i) => i === 0 || f < clamped[i - 1]);
  const lowFps = Math.min(ladder[low - 1], cap);
  const idx = out.findIndex(f => f <= lowFps);
  return { ladder: out, lowDataRung: idx >= 0 ? idx + 1 : out.length };
}

/** UberSDR's lever is a DIVISOR off its full rate, so a rung's fps → divisor (10 → 1, 5 → 2, 3.3 → 3). The rung INDEX
 *  stops being the divisor once the ladder is capped ([5, 3.3]: rung 1 is 5 fps = divisor 2). */
export const uberDivisorFor = (fullFps: number, fps: number): number =>
  fps > 0 && fullFps > 0 ? Math.max(1, Math.round(fullFps / fps)) : 1;

/**
 * The LIVE NUMBER on each button: rows per second that choice gives on a server whose own rate is `nativeFps`.
 * MINIMUM = min(cap, native), filled in to 5 below 5; ECO = the row rule on its capped feed (10 whenever a feed is known) — `caps` lets a backend
 * whose lever is coarser say what it really gets (Kiwi's ECO step is ~13 fps); NATIVE = native; DEFAULT / SMOOTH = the
 * row-target rule (wfRowTarget.ts), or native when that is actual rows. Unknown native (0) → null: no number.
 *   20 fps server → 5 · 10 · 20 · 20 · 60       5 fps server → 5 · 10 · 5 · 15 · 30
 */
export function speedNumbers(nativeFps: number, caps: { minimum: number; eco: number } = LINK_CAP):
    Record<WfSpeedSetting, number> | null {
  if (!(nativeFps > 0)) return null;
  const r = (x: number) => Math.round(x * 10) / 10;
  const rows = (s: WfScroll) => { const t = rowTargetFor(s, nativeFps); return r(t > 0 ? t : nativeFps); };
  return {
    minimum: (() => { const f = Math.min(caps.minimum, nativeFps), t = rowTargetFor('minimum', f); return r(t > 0 ? t : f); })(),
    eco:     (() => { const f = Math.min(caps.eco, nativeFps), t = rowTargetFor('eco', f); return r(t > 0 ? t : f); })(),
    sharp:   r(nativeFps),
    default: rows('default'),
    smooth:  rows('smooth'),
  };
}

/** What MINIMUM / ECO really get on a Kiwi, whose lever is wf_speed (2 ≈ 5 fps, 3 ≈ 13 fps — KiwiAdapter.wfSpeedLever). */
export const KIWI_SPEED_CAPS = { minimum: 5, eco: 13 } as const;
