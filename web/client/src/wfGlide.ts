/* wfGlide.ts — WHERE THE WEB WATERFALL DRAWS EACH ROW on a zoom, a tune or a jump: the app's rule, on the web's rows
 * (Stuart, 2026-10-29: "yes please we want to preserve the history across tunes and zooms").
 *
 * ★★★ THE HARD RULE (memory feedback_waterfall_core_rules, Stuart 2026-10-28): "I want no slicing when zooming or tuning,
 *     I want minimal black gaps only when absolutely necessary due to the SDR catching up from a large move or rapid
 *     tuning, and I want the waterfall to be preserved on orientation change and resizing. This is a fully buffered
 *     waterfall with a ring buffer."
 *
 * THE APP'S MODEL (src/utils/wfRowPlace.ts fanNoteRow / fanAfterRow), shared here, not re-derived:
 *   • every row is drawn ONCE against a view and the history is never rescaled — a zoom leaves the old rows AS DRAWN
 *     ("a thin line signal should expand out to the width and the history remains the thin line");
 *   • the new rows after a zoom step, or ANY tune step past two columns, are drawn at in-between views — STEADY with a
 *     soft landing over a fixed TIME (easeView; the app's zoomEaseSec / tuneEaseSec, 2026-10-29: 0.33 s a ×2, 0.25 s a
 *     key or a tune step), counted in the device rows the pacer draws (easeRowRate) — so a station's trail curves out on
 *     a zoom in, in on a zoom out, and slides across on a tune;
 *   • a move of a whole view or more is a JUMP: the history is moved into place by Hz, each row in its OWN scale, and is
 *     black only at the newly exposed edge (waterfall.ts does the move — the per-row maps of wfZoomFan.ts).
 * ★★ THE JOIN (the app's shader motion taps, WaterfallView.tsx — "the chunky broken diagonal lines joined up so they
 *    looked solid"): a web row is ONE device line, and between two rows of an ease a station can move tens of pixels, so
 *    each moving row is drawn across its own footprint — its OWN frame sampled at K positions along the move from its
 *    view to the view of the row below (in the limit of one tap per column the trail crosses) — and where a tap finds a
 *    station the pixel takes it (a solid trail), the noise is averaged (no brightened floor): the app's
 *    mix(mean, max, smoothstep(0.03, 0.10, max − mean)). One frame, displaced — never a blend of two moments.
 *    ★ Done ONCE, when the row is written (it is stored as drawn): no per-pixel shader cost on a weak GPU, and the 2D
 *      fallback gets the same picture. Steady rows (no move) are the frame exactly, as always.
 *    ★ ✗ NOT the old cross-fade glide (two frames mixed by t): Stuart's rule is whole rows, never blended.
 * PURE — scripts/test_wf_glide.ts. */

import { cappedEaseRows, easeView, spanChanged, tuneEaseRows, viewMoveFrac, zoomEaseRows, EASE_ROW_RATE, TUNE_EASE_MIN_FRAC } from '../../../src/utils/wfRowPlace.ts';

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

export interface GlideScratch { a: Float32Array; pre: Float64Array; sp: Uint8Array }
export function newGlideScratch(n: number): GlideScratch {
  return { a: new Float32Array(n), pre: new Float64Array(n + 1), sp: new Uint8Array(0) };
}
function fit(st: GlideScratch, n: number) {
  if (st.a.length < n) st.a = new Float32Array(n);
}

/** The same fixed point as easeView — kept under its old name for the callers and the test. */
export const zoomEaseView = easeView;

/** `cur` (placed lo, span) resampled onto the view (L, S) into dst; a column the frame does not cover CONTINUES the row
 *  drawn before it (`below`, same columns) — the history runs on up the edges of a zoom-in instead of a black wedge. */
export function easedRowAt(cur: Uint8Array, lo: number, span: number, L: number, S: number, dst: Uint8Array,
                           below: Uint8Array | null, st: GlideScratch): void {
  const n = dst.length;
  fit(st, n);
  sampleInto(cur, lo, span, n, L, S, st.a);
  for (let c = 0; c < n; c++) {
    const v = st.a[c];
    dst[c] = v === v ? (v < 0 ? 0 : v > 255 ? 255 : Math.round(v)) : (below && below.length === n ? below[c] : 0);
  }
}

/** The largest move, in columns of an n-wide row, of any frequency on screen between views (L0, S0) and (L1, S1). */
export function viewMoveCols(L0: number, S0: number, L1: number, S1: number, n: number): number {
  // x(F) = (F − L)/S is linear in F, so the largest move across the screen is at one of its two edges (wfRowPlace.ts)
  return viewMoveFrac(L0, S0, L1, S1) * n;
}

/** ★★ THE JOIN: the row of `cur` (placed lo, span) drawn at the view (L0, S0), across its footprint toward the view of the
 *  row below it (L1, S1). The app's shader takes taps along the move (one per device px the trail crosses, up to 6 a line)
 *  and mixes their mean and max; here a row is one device line and a move can cross a hundred columns, so the same thing is
 *  taken in the LIMIT of many taps: the frame's bins the column's path crosses are one contiguous range, so the taps' mean
 *  is that range's mean (a prefix sum) and their max its max (a sparse table) — O(1) a column, the same answer at any
 *  length of move (~10 µs a row where 48 real taps cost ~180 µs on an M-series Mac, and an Atom is the user that matters).
 *  A pixel whose path finds a station takes it (a solid trail); the noise is averaged (no brightened floor) — the app's
 *  mix(mean, max, smoothstep(0.03, 0.10, max − mean)). A column that moves less than one column is the frame sampled at
 *  its own view exactly (easedRowAt); one whose path has no data continues `below`. Returns the largest move, in columns. */
export function joinedRowAt(cur: Uint8Array, lo: number, span: number, L0: number, S0: number, L1: number, S1: number,
                            dst: Uint8Array, below: Uint8Array | null, st: GlideScratch): number {
  const n = dst.length, m = cur.length;
  const mv = viewMoveCols(L0, S0, L1, S1, n);
  easedRowAt(cur, lo, span, L0, S0, dst, below, st);   // the frame at this row's own view: every column that barely moves
  if (mv < 1) return mv;
  // prefix sums (mean) and a sparse table (max) over the frame's bins
  if (st.pre.length < m + 1) st.pre = new Float64Array(m + 1);
  let lv = 1; while ((1 << lv) <= m) lv++;
  if (st.sp.length < lv * m) st.sp = new Uint8Array(lv * m);
  const pre = st.pre, sp = st.sp;
  pre[0] = 0;
  for (let i = 0; i < m; i++) { pre[i + 1] = pre[i] + cur[i]; sp[i] = cur[i]; }
  for (let k = 1; k < lv; k++) {
    const o = k * m, p = (k - 1) * m, h = 1 << (k - 1);
    for (let i = 0; i + (1 << k) <= m; i++) { const a = sp[p + i], b = sp[p + i + h]; sp[o + i] = a > b ? a : b; }
  }
  // column c's centre, in the frame's bins, at this row's view (u0) and the row below's (u1): u = bA + c·bK
  const bK0 = (S0 / n) / (span / m), bA0 = ((L0 - lo) / span) * m + 0.5 * bK0 - 0.5;
  const bK1 = (S1 / n) / (span / m), bA1 = ((L1 - lo) / span) * m + 0.5 * bK1 - 0.5;
  const hw = Math.max(0.5, Math.max(bK0, bK1) / 2);    // half a column's footprint, in bins (≥ half a bin)
  const lo3 = 0.03 * 255, hi3 = 0.10 * 255;
  for (let c = 0; c < n; c++) {
    const u0 = bA0 + c * bK0, u1 = bA1 + c * bK1;
    if (Math.abs(u1 - u0) < bK0) continue;            // under a column of move: the frame at its own view, as drawn above
    let i0 = Math.round(Math.min(u0, u1) - hw + 0.5), i1 = Math.round(Math.max(u0, u1) + hw - 0.5);
    if (i0 < 0) i0 = 0;
    if (i1 > m - 1) i1 = m - 1;
    if (i1 < i0) continue;                             // the path is off the frame: keep what easedRowAt drew there
    const cnt = i1 - i0 + 1, mean = (pre[i1 + 1] - pre[i0]) / cnt;
    let k = 0; while ((2 << k) <= cnt) k++;
    const a = sp[k * m + i0], b = sp[k * m + i1 - (1 << k) + 1], mx = a > b ? a : b;
    const d = mx - mean;
    const x = d <= lo3 ? 0 : d >= hi3 ? 1 : (d - lo3) / (hi3 - lo3), s = x * x * (3 - 2 * x);
    const v = mean + (mx - mean) * s;
    dst[c] = v < 0 ? 0 : v > 255 ? 255 : Math.round(v);
  }
  return mv;
}

/* ── THE EASE: which view each new row is drawn at (the app's fanNoteRow / fanAfterRow, in the web's terms) ─────────────
 *  Counted in ROWS, like the app: the waterfall calls rowView once per DEVICE row it writes, so a Retina canvas (rowPx 2)
 *  gets each of its two lines a screen row at its own place along the ease. */
export interface RowEase {
  /** The view the last row was DRAWN at (NaN before any). */
  dLo: number; dSpan: number;
  /** The last frame's own placement — what a change is measured against (the app's lastLo/lastSpan). */
  tLo: number; tSpan: number;
  /** The ease: its start view, the device rows done and the rows it takes (eN = 0: none running). */
  eLo0: number; eSpan0: number; eJ: number; eN: number;
  /** Device rows drawn since the frames' placement last changed, and whether that change was a tune step (a spin). */
  since: number; lastTune: boolean;
  /** The smoothed device rows between a spin's steps (0 = not spinning). */
  gap: number;
  /** The running ease is a spin's: linear in rows (an even pace), not ease-out. */
  lin: boolean;
}
export function newRowEase(): RowEase {
  return { dLo: NaN, dSpan: NaN, tLo: NaN, tSpan: NaN, eLo0: NaN, eSpan0: NaN, eJ: 0, eN: 0, since: 0, lastTune: false, gap: 0, lin: false };
}

/** Screen rows between two tune steps that still count as one spin (~0.4 s at the default 30 rows/s). */
export const SPIN_ROWS = 12;

export interface RowView { lo: number; span: number; /** ≠ 0: a jump — move the history by this many Hz first. */ jumpHz: number }

/** The view to draw the next row of a frame placed at (lo, span) at. `rowPx` = device rows a screen row. */
export function rowView(st: RowEase, lo: number, span: number, rowPx: number, out: RowView, devRowsPerSec = 0): RowView {
  out.jumpHz = 0;
  if (!(span > 0) || !isFinite(lo)) { out.lo = lo; out.span = span; return out; }
  if (!(st.tSpan > 0) || !(st.dSpan > 0)) {            // the first row
    st.tLo = st.dLo = lo; st.tSpan = st.dSpan = span; st.eN = 0;
    out.lo = lo; out.span = span; return out;
  }
  const px = Math.max(1, Math.round(rowPx));
  const zoom = spanChanged(st.tSpan, span);
  const moved = Math.abs(lo - st.tLo);
  const cMoved = Math.abs((lo + span / 2) - (st.tLo + st.tSpan / 2));
  if (zoom || moved > 0) {
    const left = st.eN > 0 ? st.eN - st.eJ : 0;
    // ★ A JUMP: a move of a whole view or more (a bookmark, a typed frequency, a spin faster than the radio) — nothing on
    //   screen in common with the new rows, so the history is moved into place instead (by the centre's Hz).
    if ((!zoom && moved >= st.tSpan) || (zoom && cMoved >= Math.max(span, st.tSpan))) {
      out.jumpHz = (lo + span / 2) - (st.tLo + st.tSpan / 2);
      st.eN = 0;
      st.tLo = st.dLo = lo; st.tSpan = st.dSpan = span;
      out.lo = lo; out.span = span; return out;
    }
    let rows = 0;
    // ★★ BY TIME (the app's zoomEaseSec / tuneEaseSec, 2026-10-29): counted in the DEVICE rows actually drawn when the
    //    caller says how many a second; the default rate's screen rows otherwise.
    const rr = devRowsPerSec > 0 ? devRowsPerSec : 0;
    if (zoom) { rows = rr ? zoomEaseRows(span / st.dSpan, rr) : zoomEaseRows(span / st.dSpan) * px; st.lin = false; }
    else if (moved > st.tSpan * TUNE_EASE_MIN_FRAC) {
      rows = rr ? tuneEaseRows(moved / st.tSpan, rr) : tuneEaseRows(moved / st.tSpan) * px;
      // ★ A SPIN (a step that follows the last within SPIN_ROWS screen rows): the ease spans the rows between steps, so
      //   the trail moves EVENLY all through it — a step that eased in 3 rows and then stood still until the next frame
      //   drew a fine staircase. An isolated step keeps the app's short ease.
      //   The gap is the worst recent one (a radio catching up sends its steps unevenly), over-covered a little, so the next step
      //   always lands while the last is still easing.
      if (st.lastTune && st.since <= SPIN_ROWS * px) {
        st.gap = Math.max(st.since, st.gap * 0.85);   // the WORST recent gap, decaying (as the pacer sizes for its slowGap)
        rows = Math.max(rows, Math.min(SPIN_ROWS * px, Math.ceil(st.gap * 1.25)));
        st.lin = true;   // ★ and at an even pace: an ease-out lands at rest, and a spin made of landings is a staircase
      } else { st.gap = 0; st.lin = false; }
    }
    // ★★★ THE SLOPE CAP (wfRowPlace.ts cappedEaseRows, Stuart 2026-10-29: "higher step rates turn into sharp lines with no
    //     detail" / "same with zooming"): long enough, from where the ease has got to, that no carrier on screen moves more
    //     than the cap a SCREEN row (px device rows), up to EASE_CEIL_S. A fast run of steps no longer squeezes a growing
    //     distance into the same few rows; the edges of a zoom curve instead of streaking. Unbound (small steps): as it was.
    if (rows > 0) rows = cappedEaseRows(st.dLo, st.dSpan, lo, span, rows, rr || EASE_ROW_RATE * px, px, st.lin);
    if (rows > 0) {
      // ★ a step mid-ease starts from where the ease has got to and keeps at least the time it had left — never a snap
      st.eLo0 = st.dLo; st.eSpan0 = st.dSpan; st.eJ = 0;
      st.eN = Math.max(rows, left);
    }
    st.lastTune = !zoom; st.since = 0;
    st.tLo = lo; st.tSpan = span;
  }
  st.since++;
  if (st.eN > 0) {
    st.eJ++;
    if (st.eJ >= st.eN) { st.eN = 0; st.dLo = lo; st.dSpan = span; }
    else if (st.lin) { st.dLo = st.eLo0 + (lo - st.eLo0) * (st.eJ / st.eN); st.dSpan = span; }
    else { const v = easeView(st.eLo0, st.eSpan0, lo, span, st.eJ / st.eN); st.dLo = v[0]; st.dSpan = v[1]; }
  } else { st.dLo = lo; st.dSpan = span; }
  out.lo = st.dLo; out.span = st.dSpan;
  return out;
}
