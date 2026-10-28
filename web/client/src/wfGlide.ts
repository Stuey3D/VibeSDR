/* wfGlide.ts — THE GLIDE: a moving row is drawn at its own in-between placement (Stuart, 2026-10-28).
 *
 * THE REFERENCE is the first native build (~/Desktop/ScreenRecording_06-10-2026 00-02-18_1.mov, MW sweep 860→670 kHz):
 * during a tune the WHOLE row moves — the noise texture bends along the same curves as the stations — and every trail
 * is a thick, smooth, continuous curve with no steps, no dashes and no hard seams between rows. The same for a zoom:
 * the whole history fans smoothly in and out.
 *
 * What made it smooth there: every row on screen sat at ITS OWN placement, and placements changed a little from row to
 * row. Here the renderer emits several rows between two frames (pacing: rows/s > frames/s, always so on a slow server);
 * they used to all be one frame's content snapped to one frame's placement — a run of rows at one place, then a jump
 * to the next: the "chunky broken diagonal lines" (Stuart, 02:02).
 * So each row emitted t of the way between frame A (lo0, span0) and frame B (lo1, span1) is now drawn against the view
 * t of the way between them — L = lo0 + t·(lo1 − lo0), S = span0 + t·(span1 − span0) — with BOTH frames' content
 * resampled to that view and cross-faded (1 − t, t). A fixed carrier lands at one column (both frames agree where it
 * is), sub-pixel, so a tune draws a curve and a zoom a fan; the noise moves with it.
 *   ★ STEADY ROWS ARE UNTOUCHED: when A and B share a placement, the caller copies a whole frame exactly as before.
 *   ★ A JUMP (the centre moved more than GLIDE_MAX_FRAC of the span) is not interpolated — the gap is honest.
 *   ★ ✗ The first glide (0fa2c5d2f) slid ONE frame's row and smeared strong bins across the gap — no cross-fade, the
 *     floor static under a smear; Stuart: "the sharpness on tuning looks broken". Gone.
 * Cost: only rows emitted while the view moves — one pass over the row (two linear samples a bin, or the max over the
 * bins a column covers when the view is wider than a frame). Steady rows: nothing new.
 * PURE — scripts/test_wf_glide.ts. */

/** A centre move larger than this fraction of the span between two frames is a jump, not a glide. */
export const GLIDE_MAX_FRAC = 1 / 8;

/** True when two frames' placements differ (a tune or a zoom between them) — not float noise. */
export function placementMoved(lo0: number, span0: number, lo1: number, span1: number, n: number): boolean {
  if (!(span0 > 0 && span1 > 0)) return false;
  return Math.abs(lo1 - lo0) > span0 / n * 0.01 || Math.abs(span1 - span0) > span0 * 1e-6;
}

/** True when the move between two frames is small enough to draw rows in between (not a jump). */
export function canGlide(lo0: number, span0: number, lo1: number, span1: number): boolean {
  if (!(span0 > 0 && span1 > 0) || !isFinite(lo0) || !isFinite(lo1)) return false;
  const c0 = lo0 + span0 / 2, c1 = lo1 + span1 / 2;
  return Math.abs(c1 - c0) <= Math.min(span0, span1) * GLIDE_MAX_FRAC;
}

/** One frame's row resampled onto the view (L, S): column c of dst covers [L + c·S/n, L + (c+1)·S/n). Linear between
 *  bins when the view is as fine as the frame or finer; the MAX of the covered bins when it is any coarser (a one-bin carrier
 *  must survive a zoom out). Columns outside the frame are NaN (no data). */
function sampleInto(src: Uint8Array, lo: number, span: number, n: number, L: number, S: number, out: Float32Array): void {
  const m = src.length;
  const k = (S / n) / (span / m);           // source bins per output column
  const b = (L - lo) / span * m;            // source bin (continuous) at column 0's left edge
  if (k <= 1.0001) {
    for (let c = 0; c < n; c++) {
      const u = b + (c + 0.5) * k - 0.5;    // centre of column c, in source-bin coordinates
      if (u < -0.5 || u > m - 0.5) { out[c] = NaN; continue; }
      const i = Math.floor(u), f = u - i;
      const a0 = src[i < 0 ? 0 : i], a1 = src[i + 1 >= m ? m - 1 : i + 1];
      out[c] = a0 + (a1 - a0) * f;
    }
  } else {
    for (let c = 0; c < n; c++) {
      const u0 = b + c * k, u1 = u0 + k;
      if (u1 <= 0 || u0 >= m) { out[c] = NaN; continue; }
      let v = 0;
      for (let i = Math.max(0, Math.floor(u0)), e = Math.min(m, Math.ceil(u1)); i < e; i++) if (src[i] > v) v = src[i];
      out[c] = v;
    }
  }
}

export interface GlideScratch { a: Float32Array; b: Float32Array }
export function newGlideScratch(n: number): GlideScratch { return { a: new Float32Array(n), b: new Float32Array(n) }; }

/** The in-between row: dst (n columns) = frame A and frame B resampled onto the view t of the way between them and
 *  cross-faded. Where only one frame covers a column it is used alone; where neither does, 0 (the palette floor).
 *  Returns the view the row was drawn against, [L, S] — the zoom fan places the history by it. */
export function glideRowAt(prev: Uint8Array, lo0: number, span0: number, cur: Uint8Array, lo1: number, span1: number,
                           t: number, dst: Uint8Array, st: GlideScratch): [number, number] {
  const n = dst.length;
  if (st.a.length < n) { st.a = new Float32Array(n); st.b = new Float32Array(n); }
  const tt = t < 0 ? 0 : t > 1 ? 1 : t;
  const L = lo0 + (lo1 - lo0) * tt, S = span0 + (span1 - span0) * tt;
  sampleInto(prev, lo0, span0, n, L, S, st.a);
  sampleInto(cur, lo1, span1, n, L, S, st.b);
  const A = st.a, B = st.b;
  for (let c = 0; c < n; c++) {
    const a = A[c], b = B[c];
    const v = a === a ? (b === b ? a + (b - a) * tt : a) : (b === b ? b : 0);
    dst[c] = v < 0 ? 0 : v > 255 ? 255 : Math.round(v);
  }
  return [L, S];
}
