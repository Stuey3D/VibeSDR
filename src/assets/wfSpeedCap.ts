/**
 * wfSpeedCap.ts — THE WATERFALL SPEED KEYS (display only) AND THE DATA CONTROL (what the server sends) — V12.
 *
 * ★★★ STUART, 2026-10-30 (the design as it now stands): "we cannot slow the incoming data rate down automatically when the
 *     waterfall is slowed down as that will effect the spectrum too. We need a minimum of 5FPS incoming for the spectrum
 *     to feel smooth still, interpolation only goes so far on that and even at 5FPS incoming you see the limitation of
 *     the lack of raw data. So we need a separate data control but dont mention frame rates, just call it low auto full".
 *
 * 1. WATERFALL SPEED — how many rows a second the waterfall DRAWS. Never changes what the server sends.
 *
 *     MINIMUM · LOW · ECO · NATIVE · [ DEFAULT · SMOOTH ]   (the bracketed pair outlined: "interpolated for smoothness")
 *
 *   MINIMUM  2 rows a second — stored 'min2'. GitHub #57 (Gesiima, on a phone: "the waterfall is too fast" to find an
 *            intermittent signal in the history). Stuart chose a slower speed over a rewind/pause feature: "VibeSDR is
 *            meant to be an easy to use app, simple and intuitive to use but powerful at the same time". 2 is the floor
 *            ("1fps would look like no motion at all"); no slider — every rate divides 60 to stay judder-free.
 *   LOW      5 rows a second — stored 'low'. This WAS "MINIMUM" (stored 'minimum') until 2026-10-30: a saved 'minimum'
 *            loads as LOW, never silently as the new 2 (loadWfSpeed).
 *   ECO      10 rows a second.
 *   NATIVE   the server's own frame rate, every row a real frame. (Was SHARP — the stored value is still 'sharp'.)
 *   DEFAULT / SMOOTH  real frames with rows repeated between them for a smoother scroll (wfRowTarget.ts).
 *   A feed faster than the speed is COMBINED down to it (wfCombine.ts — linear power, peaks kept), a slower one filled in.
 *
 * 2. DATA · LOW · AUTO · FULL — how much spectrum data the server sends (no frame rates on screen — Stuart's words).
 *   LOW   the floor, 5 frames a second (DATA_FLOOR_FPS) — the least that keeps the spectrum smooth.
 *   AUTO  adaptive link management (linkManager.ts / vibeLinkManager.ts): as much as the connection carries, stepping
 *         down on a poor one — but never below the floor (UberSDR's old 3.3 fps emergency rung is gone).
 *   FULL  the server's own full rate.
 *   App: the link mode ('lowData' / 'adaptive' / 'full', lsv_link_mode — old saves load as they were). Web: main.ts runs
 *   the same controller (vibeLinkManager) on its own socket. Kiwi has no controller: LOW = wf_speed 2 (≈ 5 fps), AUTO and
 *   FULL its full wf_speed 4. OpenWebRX has no lever at all — no data control there.
 *
 * Pure, so scripts/test_wf_speed_cap.ts proves it.
 */
import { nominalRowRate, type WfScroll } from './wfRowTarget.ts';

/** The waterfall speed the user picks — a row mode (wfRowTarget); 'sharp' is labelled NATIVE, 'min2' MINIMUM, 'low' LOW.
 *  Display only. */
export type WfSpeedSetting = WfScroll;

/** The slowest waterfall a USER may choose (MINIMUM), rows a second — display only (V12 #57). */
export const MIN_WF_FPS = 2;
/** ★★ THE DATA FLOOR: the incoming rate never goes below this from any control (LOW, AUTO, the idle saver) — "We need a
 *  minimum of 5FPS incoming for the spectrum to feel smooth still" (Stuart, 2026-10-30). */
export const DATA_FLOOR_FPS = 5;

/** The DATA control's three choices (words only on screen). */
export type DataMode = 'low' | 'auto' | 'full';
export const DATA_LABEL: Readonly<Record<DataMode, string>> = { low: 'LOW', auto: 'AUTO', full: 'FULL' };
/** The one-line note, the same on the app and the web. */
export const DATA_NOTE = "How much spectrum data the server sends. Low saves data; Auto adapts to your connection; Full is the server's full rate.";
/** DataMode ↔ the link managers' LinkMode (the app keeps storing the LinkMode strings, so old saves load unchanged). */
export const linkModeFor = (d: DataMode): 'lowData' | 'adaptive' | 'full' => (d === 'low' ? 'lowData' : d === 'full' ? 'full' : 'adaptive');
export const dataModeFor = (m: unknown): DataMode => (m === 'lowData' ? 'low' : m === 'full' ? 'full' : 'auto');
/** ★ The web's old WATERFALL DATA RATE pref (AUTO 0 / 20 / 10 / 5) → the DATA control: 5 → LOW, 20 → FULL, 0 and 10 → AUTO
 *  (10 was "less than full but not the floor" — AUTO gives that on any link that needs it). */
export const dataModeFromWebRate = (v: unknown): DataMode => (v === 5 ? 'low' : v === 20 ? 'full' : 'auto');
/** The Low Data rung of a ladder: the first rung at or below the floor (VibeServer [20, 10, 5] → 3, UberSDR [10, 5] → 2). */
export function lowDataRungFor(ladder: readonly number[]): number {
  const i = ladder.findIndex(f => f <= DATA_FLOOR_FPS + 1e-9);
  return i >= 0 ? i + 1 : ladder.length;
}
/** A ladder with nothing below the floor (AUTO never drops under it). */
export const floorLadder = (ladder: readonly number[]): number[] => {
  const out = ladder.filter(f => f >= DATA_FLOOR_FPS - 1e-9);
  return out.length ? out : [Math.max(...ladder)];
};

/** Every key's name on screen, slowest to smoothest. */
export const WF_SPEED_LABEL: Readonly<Record<WfSpeedSetting, string>> =
  { min2: 'MINIMUM', low: 'LOW', eco: 'ECO', sharp: 'NATIVE', default: 'DEFAULT', smooth: 'SMOOTH' };

/** A CURRENT stored value. ★ The old 'minimum' is NOT one — it is a 5 fps save; loadWfSpeed turns it into LOW. */
export const isWfSpeedSetting = (v: unknown): v is WfSpeedSetting =>
  v === 'sharp' || v === 'eco' || v === 'default' || v === 'smooth' || v === 'low' || v === 'min2';

/** ★★ A SAVED SPEED, MIGRATED (2026-10-30): the old 'minimum' (5 fps, before the 2 fps MINIMUM existed) → 'low', the 5
 *  fps choice it always was — never the new 2. Anything else unknown → null (keep the default). Callers re-save the
 *  result when it differs from what was stored, so the old value is gone after one load. */
export function loadWfSpeed(v: unknown): WfSpeedSetting | null {
  if (v === 'minimum') return 'low';
  return isWfSpeedSetting(v) ? v : null;
}

/**
 * A link manager's ladder (fps per rung, full rate first, the last rung the adaptive floor) under a ceiling of `cap` fps
 * (the link managers' generic ceiling — userCapFps; since 2026-10-30 no speed key sets one, the speed keys are display
 * only). Every rung is clamped to the cap and rungs that collapse together become one — VibeServer [20, 10, 5] under 10 →
 * [10, 5]. The Low Data rung keeps its RATE, clamped the same way. `cap` 0 = untouched.
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
 * The LIVE NUMBER on each speed key: rows per second it draws from a feed of `feedFps` (the rate the server is sending —
 * its own, or the floor under DATA LOW). Every one but NATIVE is a rate that divides 60 (wfRowTarget.ts nominalRowRate):
 * MINIMUM 2, LOW 5, ECO 10 (a faster feed combined down, a slower one filled in); NATIVE = the feed, as it comes;
 * DEFAULT / SMOOTH = the row rule, up to the next rate above a faster feed. Unknown feed → null.
 *   20 fps → 2 · 5 · 10 · 20 · 30 · 60     5 fps → 2 · 5 · 10 · 5 · 15 · 30     23 fps Kiwi → 2 · 5 · 10 · 23 · 30 · 60
 */
export function speedNumbers(feedFps: number): Record<WfSpeedSetting, number> | null {
  if (!(feedFps > 0)) return null;
  const r = (x: number) => Math.round(x * 10) / 10;
  const rows = (s: WfScroll) => { const t = nominalRowRate(s, feedFps); return r(t > 0 ? t : feedFps); };
  return { min2: rows('min2'), low: rows('low'), eco: rows('eco'), sharp: r(feedFps), default: rows('default'), smooth: rows('smooth') };
}
