/* wfZoomFan.ts — THE ZOOM FAN for the web waterfall's history ring (Stuart, 2026-10-27 23:51, Lenovo RSP1A, MW 648 kHz:
 * zooming did not fan the history — old rows kept their old scale, new rows started at the new one, and the waterfall
 * read as horizontal SLICES of different zoom).
 *
 * ★★★ THE HARD RULE (memory feedback_waterfall_core_rules, quoted in full in src/utils/wfRowPlace.ts): tuning and
 *     zooming never lose the history; "when zooming in and out you had a cool fanning in and out of the signals".
 *
 * HOW: every ring row carries ONE affine map from the screen's x (0..1 across the view) to its OWN texture u (0..1 across
 * the row as it was stored): u = a·x + b. A new row is the identity (it was drawn against the view it landed in). When
 * the rows arriving change SPAN (a zoom — wheel, pinch, the +/- buttons, all of them, because it is read off the frames
 * themselves), every existing row's map is composed with the screen move that zoom made, so the history stretches
 * outward on a zoom in and draws in toward the anchor on a zoom out, blank only where it never had data.
 * ★ A change of CENTRE alone (a tune, a pan) does NOT touch the history — that is the trail that bends while the dial
 *   moves and runs straight after (#57; re-aligning on a tune was built and reverted once, see wfRowPlace.ts). A zoom
 *   that also moves the centre (a wheel zoom about the pointer) takes its translation with it, so the anchor stays put.
 * ★ COST: the ring's stored pixels are never rewritten (lossless — zoom in then out gives back the edges exactly). A zoom
 *   step is two multiply-adds per ring row in JS (2560 rows); each frame the renderer walks the visible rows once to
 *   find runs that share a map and draws each run with its own uniforms — one draw when nobody has zoomed, a handful
 *   after a pinch, and the old runs scroll away.
 * PURE — scripts/test_wf_zoom_fan.ts. */

/** Rows' maps after the view's span/position changed from (lo0, span0) to (lo1, span1), in Hz. Only call it when the
 *  SPAN changed (see the note above). Screen x in the new view sits at x_old = (lo1 − lo0)/span0 + x·span1/span0 in the
 *  old one, so a' = a·k and b' = a·c + b. */
export function applyViewChange(xfA: Float64Array, xfB: Float64Array, lo0: number, span0: number, lo1: number, span1: number): void {
  if (!(span0 > 0 && span1 > 0)) return;
  const k = span1 / span0, c = (lo1 - lo0) / span0;
  for (let i = 0; i < xfA.length; i++) {
    const a = xfA[i];
    xfB[i] = a * c + xfB[i];
    xfA[i] = a * k;
  }
}

/** True when a new frame's span differs from the last row's enough to count as a zoom (not float noise). */
export function spanChanged(span0: number, span1: number): boolean {
  return span0 > 0 && span1 > 0 && Math.abs(span1 - span0) > span0 * 1e-6;
}

/** Runs of visible rows sharing one map, newest first. Writes [j0, j1, a, b] quads into `out` (display rows j0 ≤ j < j1,
 *  j = 0 the newest at the top) and returns how many. Rows that were written together went through the same sequence
 *  of compositions from the identity, so their maps are bit-identical and an exact compare groups them. `out` grows. */
export function mapRuns(xfA: Float64Array, xfB: Float64Array, head: number, visible: number, out: { buf: Float64Array }): number {
  const rows = xfA.length;
  if (!rows || visible <= 0) return 0;
  let n = 0, j0 = 0;
  let a = xfA[head % rows], b = xfB[head % rows];
  const push = (j1: number) => {
    if ((n + 1) * 4 > out.buf.length) { const g = new Float64Array(out.buf.length * 2 + 64); g.set(out.buf); out.buf = g; }
    out.buf[n * 4] = j0; out.buf[n * 4 + 1] = j1; out.buf[n * 4 + 2] = a; out.buf[n * 4 + 3] = b; n++;
  };
  const vis = Math.min(visible, rows);
  for (let j = 1; j < vis; j++) {
    const r = (head + j) % rows;
    const ra = xfA[r], rb = xfB[r];
    if (ra !== a || rb !== b) { push(j); j0 = j; a = ra; b = rb; }
  }
  push(vis);
  return n;
}

/** The screen x-range (0..1) a row with map (a, b) covers, and the u-range it shows there: [x0, x1, u0, u1], or null when
 *  none of the row is on screen (it is then blank). For the 2D path's drawImage. */
export function visibleSpan(a: number, b: number): [number, number, number, number] | null {
  if (!(a > 0)) return null;
  const u0 = Math.max(0, b), u1 = Math.min(1, a + b);
  if (!(u1 > u0)) return null;
  return [(u0 - b) / a, (u1 - b) / a, u0, u1];
}
