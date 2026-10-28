/* wfRowPlace.ts — where WaterfallView puts one spectrum row in its history ring (moved out of the component, unchanged, so
 * it can be tested in node: scripts/test_wf_row_place.ts — GitHub #57).
 *
 * ★★★ THE HARD RULE (Stuart, 2026-10-27, verbatim): "the waterfall is what I always wanted to make sure was perfect, it was
 *     a fully buffered waterfall which when rotated or resized never lost any content it expanded or shrunk as needed.
 *     Zooming and tuning also never lost the history and it was seemless to the history too so signals always glided in
 *     from the sides and settled and when zooming in and out you had a cool fanning in and out of the signals. Those 2
 *     things were the major things that we nailed in our first native build and I made a hard rule that these must be
 *     preserved at all costs".
 *  ★★ HOW THAT LOOK IS MADE: every row is written ONCE, placed against the view at the moment it lands, and the stored
 *     history is never rewritten — so a station's trail bends while the dial moves ("diagonal when tuning then straight
 *     again with the signals in the new spot") and fans on a zoom. Tearing comes from a row whose bins do not belong to
 *     the centre it is placed by; that was the server (#57, local_sdr_shim.cpp onSpectrum), and this placement is what
 *     turns a correct row into a continuous trail. Do not "re-align" the history on a tune: that is a different picture
 *     (every trail vertical), and absolute placement was built and reverted once already (BRIEF-waterfall-frame-alignment
 *     §9 — it restarted the ring on every zoom).
 * PURE. */

/** Bins to slide a row by so it lands under the CURRENT view: the gap between the row's own centre and the view's, in the
 *  row's bins. 0 when it cannot be placed honestly — unknown centres, a different bin scale (a frame from another span
 *  would draw the right signal at the wrong width), or no overlap at all. */
export function rowViewOffset(rowCenterHz: number, rowHzPerBin: number, viewCenterHz: number, viewBwHz: number, n: number): number {
  if (!(rowHzPerBin > 0 && rowCenterHz > 0 && viewCenterHz > 0 && viewBwHz > 0 && n > 0)) return 0;
  const viewHzPerBin = viewBwHz / n;
  if (Math.abs(rowHzPerBin - viewHzPerBin) > viewHzPerBin * 1e-6) return 0;
  const off = Math.round((rowCenterHz - viewCenterHz) / rowHzPerBin);
  return Math.abs(off) >= n ? 0 : off;
}

/** Write `row` (n bins) into `ring` at `slot`, slid by `off` bins; the vacated edge is the floor (0) — the "slight gap"
 *  at a newly exposed edge, never a smear of the edge bin. */
export function writeRow(ring: Uint8Array, slot: number, row: Uint8Array, n: number, off: number): void {
  if (off === 0) { ring.set(row.subarray(0, n), slot); return; }
  ring.fill(0, slot, slot + n);
  if (off > 0) ring.set(row.subarray(0, n - off), slot + off);
  else         ring.set(row.subarray(-off, n), slot);
}

/* ── GLIDE: one continuous trail through a tune (Stuart, 2026-10-28, build 501 on a Mac at 20 fps: "the signal glide into
 *    place looks broken") ───────────────────────────────────────────────────────────────────────────────────────────────
 *  Placement above is honest but SPARSE in time: a row is one device-pixel row (2 px on a Retina Mac) and a fast tune
 *  moves a station tens of bins between two rows, so its diagonal trail was drawn as separate dots/dashes with gaps
 *  between them. A row is a moment, not a line — so when a row's placement differs from the row before it, the SIGNAL in
 *  it is drawn as a short horizontal streak from where it sat in the previous row to where it sits now: consecutive
 *  streaks join end to end and the trail reads as one continuous diagonal, however few rows the move spanned.
 *  ★ Only the new row is touched, once, as it is written: nothing already in the ring is rewritten (the hard rule) and
 *    nothing runs per display frame. Steady rows (same placement as the one before) are byte-for-byte what they were.
 *  ★ Only SIGNAL streaks, never the floor: a column streaks only when it stands GLIDE_MARGIN above the row's median, so
 *    the noise floor is not max-filtered into a bright band.
 *  ★ Zoom falls out of the same maths: the map from this row's columns to the previous row's is affine (scale a, shift b),
 *    so a small scale change (a pinch) fans each signal along its own short path. A big step (the zoom drum's ×2) or a
 *    move of more than GLIDE_MAX_FRAC of the width (a bookmark, a typed frequency, a spin faster than the radio) is NOT
 *    streaked — a gap there is what Stuart accepts, a bar across the whole picture is not.
 *  ★ Synthesised pixels: a streak draws the signal at columns between two measurements. That is the point (frame
 *    generation for the trail), and it only ever happens while the placement is moving. */

/** Index units above the row's median a column must stand to streak (the ring is 0..255 over the display range). */
export const GLIDE_MARGIN = 24;
/** The longest streak, as a fraction of the row width: past it the move is a jump, left as a gap. */
export const GLIDE_MAX_FRAC = 1 / 8;
/** The largest scale change between two rows that still streaks (a pinch); a zoom-drum step is far beyond it. */
export const GLIDE_MAX_SCALE = 0.25;

/** The previous row's column map plus reusable scratch — allocated once, never per row. */
export interface GlideState {
  has: boolean; startHz: number; hzPerCol: number;
  mask: Uint8Array; dq: Int32Array; hist: Uint32Array;
}
export function newGlideState(n: number): GlideState {
  return { has: false, startHz: 0, hzPerCol: 0, mask: new Uint8Array(n), dq: new Int32Array(n), hist: new Uint32Array(256) };
}

/** Where the row's bins went: column c of the WRITTEN row is startHz + c × hzPerCol. `off` is what writeRow slid it by.
 *  NaN when the row's own centre/scale is unknown (it cannot be related to its neighbours). */
export function placedStartHz(rowCenterHz: number, rowHzPerBin: number, off: number, n: number): number {
  if (!(rowCenterHz > 0 && rowHzPerBin > 0 && n > 0)) return NaN;
  return rowCenterHz - (off + n / 2) * rowHzPerBin;
}

/** The map from a column of the NEW row to where the same frequency sat in the PREVIOUS row: p(c) = a·c + b. Null when
 *  the two rows sit in the same place (steady — draw as today), or the move is not one to streak (see the guards). */
export function glideMap(prevStartHz: number, prevHzPerCol: number, startHz: number, hzPerCol: number, n: number): [number, number] | null {
  if (!(prevHzPerCol > 0 && hzPerCol > 0 && n > 0) || !isFinite(prevStartHz) || !isFinite(startHz)) return null;
  const a = hzPerCol / prevHzPerCol;
  if (Math.abs(a - 1) > GLIDE_MAX_SCALE) return null;
  const b = (startHz - prevStartHz) / prevHzPerCol;
  const s0 = Math.abs(b), s1 = Math.abs((a - 1) * (n - 1) + b);
  const span = Math.max(s0, s1);
  if (span < 0.5) return null;                    // steady: untouched
  if (span > n * GLIDE_MAX_FRAC) return null;     // a jump: a gap is honest, a bar across the picture is not
  return [Math.abs(a - 1) < 1e-9 ? 1 : a, b];
}

/** The row's median (its noise floor, in index units) — histogram, O(n), no allocation. */
function rowMedian(ring: Uint8Array, at: number, n: number, hist: Uint32Array): number {
  hist.fill(0);
  for (let i = 0; i < n; i++) hist[ring[at + i]]++;
  const half = n >> 1;
  let acc = 0;
  for (let v = 0; v < 256; v++) { acc += hist[v]; if (acc > half) return v; }
  return 255;
}

/** Streak the signal in the row at ring[at .. at+n) along p(c) = a·c + b (see glideMap), in place. Returns the number
 *  of columns raised (0 = nothing stood above the floor). */
export function glideStreak(ring: Uint8Array, at: number, n: number, a: number, b: number, st: GlideState): number {
  const thr = rowMedian(ring, at, n, st.hist) + GLIDE_MARGIN;
  const mask = st.mask;
  let any = false;
  for (let i = 0; i < n; i++) { const v = ring[at + i]; if (v > thr) { mask[i] = v; any = true; } else mask[i] = 0; }
  if (!any) return 0;
  let raised = 0;
  if (a === 1) {
    // A pure tune: every column moved by the same s. Column x is covered by the signal columns c whose path [c, c+s]
    // contains it — a sliding-window max of the mask (monotonic deque, O(n) whatever s is).
    const s = Math.round(b);
    if (s === 0) return 0;
    const dq = st.dq;
    let h = 0, t = 0;
    if (s > 0) {
      for (let x = 0; x < n; x++) {                     // window c ∈ [x − s, x]
        while (t > h && mask[dq[t - 1]] <= mask[x]) t--;
        dq[t++] = x;
        while (dq[h] < x - s) h++;
        const m = mask[dq[h]];
        if (m > ring[at + x]) { ring[at + x] = m; raised++; }
      }
    } else {
      const k = -s;
      for (let x = n - 1; x >= 0; x--) {                // window c ∈ [x, x + k]
        while (t > h && mask[dq[t - 1]] <= mask[x]) t--;
        dq[t++] = x;
        while (dq[h] > x + k) h++;
        const m = mask[dq[h]];
        if (m > ring[at + x]) { ring[at + x] = m; raised++; }
      }
    }
    return raised;
  }
  // A scale change (a pinch): each signal column draws its own short path. Spans are capped by glideMap, and only
  // columns above the floor (at most half the row, by the median) do any work.
  for (let c = 0; c < n; c++) {
    const v = mask[c];
    if (!v) continue;
    const p = a * c + b;
    const lo = Math.max(0, Math.floor(Math.min(c, p))), hi = Math.min(n - 1, Math.ceil(Math.max(c, p)));
    for (let x = lo; x <= hi; x++) if (v > ring[at + x]) { ring[at + x] = v; raised++; }
  }
  return raised;
}

/** pushRow's one call after writeRow: streak the row just written if its placement moved since the previous row, and
 *  remember this row's placement for the next. Returns the streak span in columns (0 = drawn as written). */
export function glideRow(ring: Uint8Array, at: number, n: number,
                         rowCenterHz: number, rowHzPerBin: number, off: number, st: GlideState): number {
  const start = placedStartHz(rowCenterHz, rowHzPerBin, off, n);
  if (!isFinite(start)) { st.has = false; return 0; }
  let span = 0;
  if (st.has) {
    const m = glideMap(st.startHz, st.hzPerCol, start, rowHzPerBin, n);
    if (m) {
      glideStreak(ring, at, n, m[0], m[1], st);
      span = Math.max(Math.abs(m[1]), Math.abs((m[0] - 1) * (n - 1) + m[1]));
    }
  }
  st.has = true; st.startHz = start; st.hzPerCol = rowHzPerBin;
  return span;
}
