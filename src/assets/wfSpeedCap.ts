/**
 * wfSpeedCap.ts — THE WATERFALL SPEED CHOICES THAT ASK THE SERVER FOR LESS: MINIMUM, LOW and ECO (V12, Stuart
 * 2026-10-28; the 2 fps MINIMUM 2026-10-30).
 *
 * From a GitHub discussion: someone wanted the waterfall slower, and on a 20 fps server even Sharp scrolled 20 rows a
 * second. The control is now, slowest to smoothest:
 *
 *     MINIMUM · LOW · ECO · NATIVE · [ DEFAULT · SMOOTH ]   (the bracketed pair outlined: "interpolated for smoothness")
 *
 *   MINIMUM  caps the LINK at 2 fps — stored 'min2'. GitHub #57 (Gesiima, on a phone: "the waterfall is too fast" to find
 *            an intermittent signal in the history). Stuart chose a slower speed over a rewind/pause feature: "VibeSDR is
 *            meant to be an easy to use app, simple and intuitive to use but powerful at the same time". 2 is the floor
 *            ("1fps would look like no motion at all"), and there is no slider: every rate must divide 60 to stay
 *            judder-free. Every row is a real frame where the server can be asked (VibeServer fftRate 2, UberSDR set_rate
 *            ÷5); where it cannot go that low (a Kiwi's slowest useful step ≈ 5 fps, an UberSDR shared channel's fixed
 *            ÷3, OpenWebRX with no lever at all) the client THINS the feed to an even 2 (wfRowTarget.ts) — so it is
 *            offered on every server, OpenWebRX included: the point is the scroll speed, the data saving a bonus.
 *   LOW      caps the LINK at 5 fps — stored 'low'. This WAS "MINIMUM" (stored 'minimum') until 2026-10-30: a saved
 *            'minimum' is a 5 fps choice and loads as LOW, never silently as the new 2 (loadWfSpeed).
 *            Actual rows: on a 5 fps server it is the same as NATIVE; a server slower than 5 (2 fps, UberSDR's 3.3
 *            emergency rung) is filled in to an even 5 rows/s.
 *   ECO      10 rows a second: caps the LINK at 10 fps (a 20 fps server sends half) AND fills a slower feed in to 10
 *            (9, 5, 3.3 fps → 10) by over-generating and discarding to an even 10/s, as Default and Smooth do.
 *   NATIVE   the server's own frame rate, every row a real frame. (Was SHARP — the stored value is still 'sharp', so a
 *            saved setting loads unchanged.)
 *   DEFAULT / SMOOTH  real frames with rows repeated between them for a smoother scroll (wfRowTarget.ts).
 *
 *   ★★★ The caps exist to SAVE DATA, so the SERVER sends fewer frames; the client thins only what a server could not
 *       be asked to drop. Rows under LOW / MINIMUM draw like NATIVE (one row per received frame) on a server that sends
 *       exactly that rate.
 *   ★★  A cap is a CEILING on the link manager, not a mode and not a divider: AUTO keeps managing the link underneath
 *       it (still steps down on a poor link — UberSDR's 3.3 fps emergency rung included), FULL is capped to it, LOW
 *       DATA keeps its own rate up to the cap (5 stays 5, never 2.5; under MINIMUM it is 2). Choosing
 *       NATIVE/DEFAULT/SMOOTH lifts it at once.
 *   ★   On a server whose own rate is already at or below a cap, that choice has no effect — nothing less to give.
 *
 * ONE rule for the app (linkManager.ts / vibeLinkManager.ts, KiwiAdapter, MenuSheet) and the web client
 * (web/client/src/main.ts). Pure, so scripts/test_wf_speed_cap.ts proves it.
 */
import { nominalRowRate, type WfScroll } from './wfRowTarget.ts';

/** The waterfall speed the user picks — every one is also a row mode (wfRowTarget); 'sharp' is labelled NATIVE,
 *  'min2' MINIMUM, 'low' LOW, and 'min2' / 'low' / 'eco' are link caps as well. */
export type WfSpeedSetting = WfScroll;

/** The minimum waterfall rate — the floor a USER may choose: MINIMUM's ceiling (V12 #57; was 5, now LOW's).
 *  ★ NOT Low Data's rate: Low Data stays on its 5 fps rung (the link managers' lowDataRung — "the interpolation can hide
 *    5"), and the idle power-saver's 5 fps target is unchanged; both already follow a lower cap (min with the rung). */
export const MIN_WF_FPS = 2;
/** LOW's ceiling ("low data rate minimum is 5fps" — the old MINIMUM). */
export const LOW_WF_FPS = 5;
/** ECO's ceiling. */
export const ECO_WF_FPS = 10;

/** The link ceiling each capped choice asks for (fps). */
export const LINK_CAP: Readonly<Record<'min2' | 'low' | 'eco', number>> =
  { min2: MIN_WF_FPS, low: LOW_WF_FPS, eco: ECO_WF_FPS };

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

/** ★★ THE WEB'S OLD "WATERFALL DATA RATE" (AUTO/20/10/5 — removed 2026-10-30, GitHub #57: it overlapped the speed keys
 *  and on DEFAULT/SMOOTH its use looked like nothing happened). A saved rate is never applied again; this says what
 *  speed it leaves. Only where it was clearly the same choice is it carried over — NATIVE with 5 is exactly LOW (5 real
 *  rows a second, 5 asked of the server), NATIVE with 10 exactly ECO on a ≥ 10 fps server. Anything else keeps the speed
 *  the listener chose (the cap simply stops: on DEFAULT/SMOOTH it never changed the scroll, only the data). */
export function migrateWfDataRate(speed: WfSpeedSetting, savedRate: unknown): WfSpeedSetting {
  if (speed !== 'sharp') return speed;
  return savedRate === 5 ? 'low' : savedRate === 10 ? 'eco' : speed;
}

/** The link ceiling for a setting, fps — 0 = none (NATIVE / DEFAULT / SMOOTH). */
export const linkCapFor = (s: WfSpeedSetting): number =>
  (s === 'min2' || s === 'low' || s === 'eco' ? LINK_CAP[s] : 0);

/** ★ Is this choice offered on a server with NO rate lever (OpenWebRX)? LOW / ECO exist to save data, so there they
 *  would only thin a feed that costs the same — hidden (AGENTS.md: no control that is dead on one radio). MINIMUM exists
 *  to slow the SCROLL (#57), which the client's thinning gives on any feed — offered everywhere. */
export const offeredWithoutLever = (s: WfSpeedSetting): boolean => linkCapFor(s) === 0 || s === 'min2';

/** Does a cap change anything on a server whose own top rate is `serverTopFps`? (LOW on a 5 fps server: no.) */
export const capHasEffect = (capFps: number, serverTopFps: number): boolean => capFps > 0 && serverTopFps > capFps;

/** The fps to ask for under a cap — a CEILING: never above the cap, never above what was wanted. 0 = no cap. */
export const capFps = (wantedFps: number, cap: number): number => (cap > 0 ? Math.min(wantedFps, cap) : wantedFps);

/**
 * A link manager's ladder (fps per rung, full rate first, the last rung the adaptive floor) under a ceiling of `cap` fps.
 * Every rung is clamped to the cap and rungs that collapse together become one — VibeServer [20, 10, 5] → LOW [5],
 * ECO [10, 5], MINIMUM [2]; UberSDR [10, 5, 3.3] → LOW [5, 3.3] (Auto may still use the 3.3 emergency rung below the
 * cap), ECO unchanged, MINIMUM [2] (set_rate ÷5). The Low Data rung keeps its RATE, clamped the same way: 5 stays 5
 * (2 under MINIMUM). `cap` 0 = untouched.
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
 * Every one but NATIVE is a rate that divides 60 (wfRowTarget.ts nominalRowRate): MINIMUM 2, LOW 5 and ECO 10 (the
 * capped feed filled in or thinned — `caps` says what a coarser lever really sends, Kiwi's ECO step ~13 fps, thinned to
 * 10, its LOW step ~5 thinned to MINIMUM's 2); NATIVE = native, as it comes; DEFAULT / SMOOTH = the row rule, up to the
 * next rate above a faster feed. Unknown native → null.
 *   20 fps server → 2 · 5 · 10 · 20 · 30 · 60     5 fps → 2 · 5 · 10 · 5 · 15 · 30     23 fps Kiwi → 2 · 5 · 10 · 23 · 30 · 60
 */
export function speedNumbers(nativeFps: number, caps: { min2: number; low: number; eco: number } = LINK_CAP):
    Record<WfSpeedSetting, number> | null {
  if (!(nativeFps > 0)) return null;
  const r = (x: number) => Math.round(x * 10) / 10;
  // ★ 2026-10-29: the RATE each runs at (nominalRowRate — always a divisor of 60), not the feed it is made from: a 23 fps
  //   Kiwi showed "ECO 13 · DEFAULT 23" — its ECO lever gives ~13 fps, thinned to 10; Default fills 23 up to 30.
  const rows = (s: WfScroll, f: number) => { const t = nominalRowRate(s, f); return r(t > 0 ? t : f); };
  return {
    min2:    rows('min2', Math.min(caps.min2, nativeFps)),
    low:     rows('low', Math.min(caps.low, nativeFps)),
    eco:     rows('eco', Math.min(caps.eco, nativeFps)),
    sharp:   r(nativeFps),
    default: rows('default', nativeFps),
    smooth:  rows('smooth', nativeFps),
  };
}

/** What MINIMUM / LOW / ECO really get on a Kiwi, whose lever is wf_speed (2 ≈ 5 fps, 3 ≈ 13 fps —
 *  KiwiAdapter.wfSpeedLever). MINIMUM takes wf_speed 2 too and is thinned 5 → 2: Kiwi's 1 Hz step (wf_speed 1) is below
 *  the 2 floor, so it would be filled in with every other row a repeat. */
export const KIWI_SPEED_CAPS = { min2: 5, low: 5, eco: 13 } as const;
