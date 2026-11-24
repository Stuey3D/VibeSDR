// orbcommView.ts — ★★ V12 2026-11-24 WHAT AN ORBCOMM PICK LOOKS LIKE ON THE WATERFALL (both clients: the app's WaterfallView,
// the web page's waterfall.ts). Stuart: an ORBCOMM pick parked the dial on 137.500 with a narrow NFM passband — nothing showed
// the IQ slice the decoder actually searches, so it looked like it listened to an empty spot and he tuned by hand. "Its
// different to every other satellite": the decoder takes the WHOLE slice (one channel ± Doppler, or the band) and decodes
// every carrier in it at once. So while the pick runs the view shows:
//   • the slice as a shaded band, lo…hi from the server's orbcomm_status (it follows the dial on a one-pipeline radio — the
//     clients just draw what the status says), labelled "ORBCOMM · every carrier decoded here";
//   • each carrier the decoder follows at its own RF — LOCKED solid, still acquiring hollow — named once the bird is known.
// ★ Pure — scripts/test_orbcomm_app.ts. Colours: the satellite labels' sky blue (knownSignals.ts SAT_LABEL_COLOUR).
import type { OrbStatus } from '../services/orbcomm';

export const ORB_SLICE_LABEL = 'ORBCOMM · every carrier decoded here';
/** The SATELLITE box's plain line for an ORBCOMM pick. */
export const ORB_NO_TUNE = 'The whole ORBCOMM band is decoded at once — no need to tune.';
/** The overlay's ink — the satellite labels' sky blue. */
export const ORB_OVERLAY_COLOUR = '#a8d8ff';

export interface OrbOverlay {
  lo: number; hi: number;
  carriers: { rf: number; locked: boolean; label: string }[];
}
/** The overlay for a status — null when there is no slice to draw (an older server, before the first status). */
export function orbOverlayFrom(st: OrbStatus | null | undefined): OrbOverlay | null {
  if (!st || !st.slice) return null;
  const { lo, hi } = st.slice;
  const carriers = st.carriers
    .filter((c) => c.rf > 0)
    .map((c) => ({ rf: c.rf, locked: c.lock, label: c.sat ? c.sat.replace(/^ORBCOMM\s+/i, '') : '' }))
    .slice(0, 12);
  return { lo, hi, carriers };
}
/** A cheap identity for redraw decisions (changes when anything drawn changes, to 100 Hz). */
export function orbOverlayKey(ov: OrbOverlay | null): string {
  if (!ov) return '';
  return `${Math.round(ov.lo / 100)}|${Math.round(ov.hi / 100)}|${ov.carriers.map((c) => `${Math.round(c.rf / 100)}${c.locked ? 'L' : 'h'}${c.label}`).join(',')}`;
}

export interface OrbOverlayLayout {
  /** the band's on-screen edges, clamped to the view; null = the slice is off screen */
  band: { x0: number; x1: number; loIn: boolean; hiIn: boolean } | null;
  /** where the band's label starts (left-aligned in the band, kept on screen), null = no room */
  label: { x: number; w: number } | null;
  /** each carrier on screen: x, solid (locked) or hollow, its name ('' = not yet) */
  marks: { x: number; locked: boolean; label: string }[];
}
/** Lay the overlay out for a view: `hzToX` maps RF to x (either client's), `W` is the view's width, `labelW` the label's
 *  width (measured or estimated). A label that does not fit the visible band sits at the band's left, clamped on screen. */
export function orbOverlayLayout(ov: OrbOverlay | null, hzToX: (hz: number) => number, W: number, labelW = 0): OrbOverlayLayout {
  const out: OrbOverlayLayout = { band: null, label: null, marks: [] };
  if (!ov || !(W > 0)) return out;
  const a = hzToX(ov.lo), b = hzToX(ov.hi);
  if (!Number.isFinite(a) || !Number.isFinite(b)) return out;
  const x0 = Math.max(0, Math.min(a, b)), x1 = Math.min(W, Math.max(a, b));
  if (x1 - x0 >= 1) {
    out.band = { x0, x1, loIn: a >= 0 && a <= W, hiIn: b >= 0 && b <= W };
    const w = Math.max(0, labelW);
    out.label = { x: Math.max(0, Math.min(W - w, x0 + 4)), w };
  }
  for (const c of ov.carriers) {
    const x = hzToX(c.rf);
    if (Number.isFinite(x) && x >= 0 && x <= W) out.marks.push({ x, locked: c.locked, label: c.label });
  }
  return out;
}
