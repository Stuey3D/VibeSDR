/* wfGlide.ts — GLIDE SMOOTHING for the web waterfall's new rows (Stuart, 2026-10-28, for 11.0.15).
 *
 * At 20 fps a fast tune moves a station many pixels between one row and the next, so its diagonal trail drew as
 * disconnected dashes — a run of rows at one place, then a jump, then a run at the next. The hard rule wants ONE
 * continuous trail ("diagonal when tuning then straight again", memory feedback_waterfall_core_rules).
 *
 * Each row the renderer emits between two frames is drawn against the view INTERPOLATED between them (the frames'
 * own centres, t of the way across), and a strong signal is also smeared back to where the PREVIOUS row put it — so
 * the trail is a slanted strip, continuous from row to row, however far it moves. Only bins well above the row's mean
 * are smeared: the noise floor is left exactly as it was, so a tune does not brighten the background.
 * ★ Steady rows (same centre) are untouched — copyRow is never even called for them.
 * ★ Cost: only on the new row — one pass over its bins, plus a sliding max no wider than SMEAR_MAX bins.
 * PURE — scripts/test_wf_glide.ts. */

/** Widest smear, in bins. A move wider than this between two rows is a jump, not a glide: the gap is left (a jump may
 *  leave "a slight gap", the hard rule's own words). */
export const SMEAR_MAX = 96;
/** How far above the row's mean (palette index, 0..255) a bin must be to be smeared — signals, not noise. */
export const SMEAR_ABOVE_MEAN = 32;

/** dst = src slid right by `shift` bins (rounded; vacated edge = 0, the floor), with every bin more than
 *  SMEAR_ABOVE_MEAN over the row's mean also drawn at every slide between `shift` and `prevShift`. */
export function glideRow(src: Uint8Array, dst: Uint8Array, n: number, shift: number, prevShift: number): void {
  const k0 = Math.round(shift);
  for (let i = 0; i < n; i++) { const j = i - k0; dst[i] = j >= 0 && j < n ? src[j] : 0; }
  const k1 = Math.round(prevShift);
  const lo = Math.min(k0, k1), hi = Math.max(k0, k1);
  if (hi === lo || hi - lo > SMEAR_MAX) return;
  let sum = 0;
  for (let i = 0; i < n; i++) sum += src[i];
  const thr = sum / Math.max(1, n) + SMEAR_ABOVE_MEAN;
  // For each strong source bin, paint it across its slides [lo, hi] — strong bins are few, so this is cheap.
  for (let j = 0; j < n; j++) {
    const v = src[j];
    if (v <= thr) continue;
    const a = Math.max(0, j + lo), b = Math.min(n - 1, j + hi);
    for (let i = a; i <= b; i++) if (dst[i] < v) dst[i] = v;
  }
}
