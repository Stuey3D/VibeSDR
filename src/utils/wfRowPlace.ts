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
 *  ★ A ZOOM IS NOT STREAKED (2026-10-28): a change of SPAN re-maps the whole history to the new view (THE ZOOM FAN,
 *    below), so the previous row already shows each signal where this one does — a streak on top of that would be the
 *    fan drawn twice. (Before the fan, a small pinch was streaked here as a stand-in for it; a ×2 step left a gap.)
 *    A move of more than GLIDE_MAX_FRAC of the width (a bookmark, a typed frequency, a spin faster than the radio) is
 *    NOT streaked either — a gap there is what Stuart accepts, a bar across the whole picture is not.
 *  ★ Synthesised pixels: a streak draws the signal at columns between two measurements. That is the point (frame
 *    generation for the trail), and it only ever happens while the placement is moving. */

/** Index units above the row's median a column must stand to streak (the ring is 0..255 over the display range). */
export const GLIDE_MARGIN = 24;
/** The longest streak, as a fraction of the row width: past it the move is a jump, left as a gap. */
export const GLIDE_MAX_FRAC = 1 / 8;

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

/** The map from a column of the NEW row to where the same frequency sat in the PREVIOUS row: p(c) = c + b (a is always
 *  1 — a change of scale is a zoom, which the fan handles). Null when the two rows sit in the same place (steady — draw
 *  as today), the span changed (a zoom: the fan re-mapped the history), or the move is a jump (see the guards). */
export function glideMap(prevStartHz: number, prevHzPerCol: number, startHz: number, hzPerCol: number, n: number): [number, number] | null {
  if (!(prevHzPerCol > 0 && hzPerCol > 0 && n > 0) || !isFinite(prevStartHz) || !isFinite(startHz)) return null;
  if (spanChanged(prevHzPerCol, hzPerCol)) return null;   // ★ a zoom: the fan placed the history — never streak it too
  const b = (startHz - prevStartHz) / prevHzPerCol;
  const span = Math.abs(b);
  if (span < 0.5) return null;                    // steady: untouched
  if (span > n * GLIDE_MAX_FRAC) return null;     // a jump: a gap is honest, a bar across the picture is not
  return [1, b];
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
  // A scale change is a zoom — the fan's job (glideMap never asks for one). Nothing to streak.
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
      span = Math.abs(m[1]);
    }
  }
  st.has = true; st.startHz = start; st.hzPerCol = rowHzPerBin;
  return span;
}

/* ── THE ZOOM FAN: a zoom re-maps the whole history to the new view (Stuart, 2026-10-27/28: zooming left the history in
 *    horizontal SLICES — every old row kept the scale it was drawn at, so the waterfall read as bands of different zoom;
 *    the web client was fixed first on V12 — web/client/src/wfZoomFan.ts there — and this is the same design for the ring)
 *  The STORED ring (what writeRow + glideRow make) is never rewritten. Each slot carries ONE affine map from the screen's
 *  x (0..1 across the view) to its own u (0..1 across the row as stored): u = a·x + b, the identity when it is written.
 *  When a row arrives at a different SPAN than the row before (a zoom — drum, pinch, buttons, all read off the rows), every
 *  slot's map composes the screen move that zoom made (applyViewChange, the web's maths) and the DISPLAYED ring is
 *  regenerated from the stored one through the maps: the history stretches out on a zoom in and draws in toward the
 *  anchor on a zoom out, blank only where it never had data.
 *  ★ A change of CENTRE alone (a tune) does NOT touch the maps — that is the trail that bends and then runs straight
 *    (#57). A zoom that also moves the centre takes its translation with it, so the anchor stays put.
 *  ★ LOSSLESS: the display is always made from the stored rows, so zoom in then back out gives the edges back exactly
 *    (the maps return to the identity and the displayed ring is the stored one, byte for byte).
 *  ★ THIN CARRIERS SURVIVE a zoom out: a pixel that covers several stored bins takes their MAX (as the resample into the
 *    ring does), never a sample that could step over a one-bin carrier.
 *  ★ COST (the trade, stated — feedback_state_efficiency_tradeoffs):
 *    RAM — the displayed ring is a second RING_W × RING_ROWS bytes (1 MiB) + 16 KiB of maps, allocated at the FIRST zoom
 *          and dropped again once every row on it is back at the identity (zoomed back, or RING_ROWS rows since the last
 *          zoom). An unzoomed session costs nothing; the image made per row is the same 1 MiB either way.
 *    CPU — only on a zoom step: every row that can be on screen re-sampled once (~1 M byte ops for a full 1024-row ring;
 *          the per-pixel bin ranges are worked out once per distinct map, not per row). JS path (iOS/Mac, Hermes is an
 *          interpreter): ~20 ms a zoom-in step / ~30 ms a zoom-out step for a FULL ring, measured with V8's interpreter
 *          only on an M-series Mac (node --no-opt --no-sparkplug --no-maglev) — a phone shows half the ring or less. Steady rows and tunes pay one 1 KiB copy a row
 *          while a zoomed history is on screen, nothing otherwise. Native (the Android engine, wf_zoom_fan.h, the
 *          whole ring): 0.4–0.75 ms a step on an M-series host, so a few ms on a slow phone — on the engine's own thread. The GPU pays NOTHING — the shader is unchanged (the XCover 4s is GPU-bound; a per-row map in the shader
 *          would cost every pixel of every display frame, and could not take the max under a pixel).
 *  PURE — scripts/test_wf_row_place.ts (zoom-fan section), the native twin is cpp/spectrum/wf_zoom_fan.h. */

/** True when two rows' bin scales differ enough to count as a zoom (not float noise). */
export function spanChanged(span0: number, span1: number): boolean {
  return span0 > 0 && span1 > 0 && Math.abs(span1 - span0) > span0 * 1e-6;
}

/** Every map after the view moved from (lo0, span0) to (lo1, span1), in Hz: screen x in the new view sits at
 *  x_old = (lo1 − lo0)/span0 + x·span1/span0 in the old one, so a' = a·k and b' = a·c + b. A map that comes back to the
 *  identity (a zoom undone) snaps to it exactly, so the restore is byte-for-byte. Returns true if any map is not the
 *  identity afterwards. */
export function applyViewChange(xfA: Float64Array, xfB: Float64Array, lo0: number, span0: number, lo1: number, span1: number): boolean {
  if (!(span0 > 0 && span1 > 0)) return false;
  const k = span1 / span0, c = (lo1 - lo0) / span0;
  let any = false;
  for (let i = 0; i < xfA.length; i++) {
    const a = xfA[i];
    let na = a * k, nb = a * c + xfB[i];
    if (Math.abs(na - 1) < 1e-9 && Math.abs(nb) < 1e-9) { na = 1; nb = 0; } else any = true;
    xfA[i] = na; xfB[i] = nb;
  }
  return any;
}

/** The fan's state: the last row's placement, the per-slot maps, and the displayed ring (null = the stored ring IS the
 *  display — no zoomed history on it). */
export interface FanState {
  lastLo: number; lastSpan: number;
  xfA: Float64Array; xfB: Float64Array;
  disp: Uint8Array | null;
  sinceZoom: number;
  /** The oldest frame whose displayed row is current — older ones were off screen at the last fan (fanRefresh). */
  validFrom: number;
  lo: Int32Array | null; hi: Int32Array | null;
}
export function newFanState(rows: number): FanState {
  return { lastLo: 0, lastSpan: 0, xfA: new Float64Array(rows).fill(1), xfB: new Float64Array(rows), disp: null, sinceZoom: 0,
           validFrom: 0, lo: null, hi: null };
}

/** Per output pixel x of an n-wide row, the stored bins [lo[x], hi[x]] it shows under map (a, b) — lo = -1 where the
 *  row has no data (blank). Magnified (a ≤ 1): the one bin under the pixel's centre. Shrunk: every bin it covers. */
export function fanTable(a: number, b: number, n: number, lo: Int32Array, hi: Int32Array): void {
  const bn = b * n;
  if (a <= 1) {
    for (let x = 0; x < n; x++) {
      const c = Math.floor(a * (x + 0.5) + bn);
      if (c >= 0 && c < n) { lo[x] = c; hi[x] = c; } else { lo[x] = -1; hi[x] = -1; }
    }
    return;
  }
  for (let x = 0; x < n; x++) {
    const l = Math.max(0, Math.floor(a * x + bn)), h = Math.min(n - 1, Math.ceil(a * (x + 1) + bn) - 1);
    if (l <= h) { lo[x] = l; hi[x] = h; } else { lo[x] = -1; hi[x] = -1; }
  }
}

/** Regenerate the displayed rows of the newest `count` frames (of `frames` written into a `rows`-slot ring) from the
 *  stored ring through their maps. Only what can be ON SCREEN is made (the caller's visible depth): on the JS path a
 *  zoom step is ~20–30 ms of Hermes interpreter for a whole 1024-row ring, and the visible depth is half that or less on
 *  a phone. Rows below it are made by fanRefresh if a resize ever shows them. */
export function fanRegen(st: FanState, ring: Uint8Array, n: number, rows: number, frames: number, count: number): void {
  const disp = st.disp!;
  if (!st.lo) { st.lo = new Int32Array(n); st.hi = new Int32Array(n); }
  const lo = st.lo, hi = st.hi!;
  const k = Math.min(count, frames, rows);
  st.validFrom = frames - k;
  let ta = NaN, tb = NaN;
  for (let f = frames - 1; f >= frames - k; f--) {
    const r = f % rows;
    const a = st.xfA[r], b = st.xfB[r], at = r * n;
    if (a === 1 && b === 0) { disp.set(ring.subarray(at, at + n), at); continue; }
    if (a !== ta || b !== tb) { fanTable(a, b, n, lo, hi); ta = a; tb = b; }   // rows zoomed together share a map
    if (a <= 1) {                                                              // magnified: one gather per pixel
      for (let x = 0; x < n; x++) { const l = lo[x]; disp[at + x] = l < 0 ? 0 : ring[at + l]; }
      continue;
    }
    for (let x = 0; x < n; x++) {                                              // shrunk: the max under the pixel
      const l = lo[x];
      if (l < 0) { disp[at + x] = 0; continue; }
      const h = hi[x];
      let m = ring[at + l];
      for (let k = l + 1; k <= h; k++) { const v = ring[at + k]; if (v > m) m = v; }
      disp[at + x] = m;
    }
  }
}

/** BEFORE a row placed at (lo, span) Hz goes in: a change of span fans the history. `frames` = rows written so far;
 *  `visible` = how many of the newest can be on screen. Returns true when it fanned. */
export function fanNoteRow(st: FanState, ring: Uint8Array, n: number, rows: number, frames: number, visible: number,
                           lo: number, span: number): boolean {
  if (!(span > 0)) return false;
  if (!isFinite(lo)) {                         // centre unknown: assume it held (a zoom about the centre)
    if (!(st.lastSpan > 0)) return false;
    lo = st.lastLo + (st.lastSpan - span) / 2;
  }
  let fanned = false;
  if (spanChanged(st.lastSpan, span) && frames > 0) {
    const zoomed = applyViewChange(st.xfA, st.xfB, st.lastLo, st.lastSpan, lo, span);
    if (zoomed) {
      if (!st.disp) st.disp = new Uint8Array(ring.length);
      fanRegen(st, ring, n, rows, frames, visible);
      st.sinceZoom = 0;
    } else st.disp = null;                    // every row back at the identity: the stored ring is the display again
    fanned = true;
  }
  st.lastLo = lo; st.lastSpan = span;
  return fanned;
}

/** AFTER the row at `slot` (0..rows-1) is written (and glided): it is the identity, and the display gets it as is. */
export function fanAfterRow(st: FanState, ring: Uint8Array, slot: number, n: number, rows: number): void {
  st.xfA[slot] = 1; st.xfB[slot] = 0;
  if (!st.disp) return;
  const at = slot * n;
  st.disp.set(ring.subarray(at, at + n), at);
  if (++st.sinceZoom >= rows) st.disp = null;  // every slot has been rewritten since the zoom: all identity again
}

/** A resize that shows MORE rows than the last fan made: make the ones now on screen. No-op when nothing is zoomed or
 *  they are already current. Returns true when it re-made anything (publish a new image). */
export function fanRefresh(st: FanState, ring: Uint8Array, n: number, rows: number, frames: number, visible: number): boolean {
  if (!st.disp || frames - Math.min(visible, frames, rows) >= st.validFrom) return false;
  fanRegen(st, ring, n, rows, frames, visible);
  return true;
}

/** The bytes to make the waterfall image from. */
export function fanImage(st: FanState, ring: Uint8Array): Uint8Array { return st.disp ?? ring; }

/** pushRow's whole placement, in one place (WaterfallView's JS path; the Android engine's twin is wf_zoom_fan.h
 *  placeRow): slide the row onto the view (rowViewOffset/writeRow), fan the history if the span changed, streak a moved
 *  row (glideRow), keep the display in step. `frames` = rows written before this one; `visible` = how many of the newest
 *  rows can be on screen (the fan makes only those). Returns the slot written. */
export function placeRow(ring: Uint8Array, rows: number, n: number, frames: number, row: Uint8Array,
                         rowCenterHz: number, rowHzPerBin: number, viewCenterHz: number, viewBwHz: number,
                         glide: GlideState, fan: FanState, visible = rows): number {
  const slot = frames % rows;
  const off = rowViewOffset(rowCenterHz, rowHzPerBin, viewCenterHz, viewBwHz, n);
  if (rowHzPerBin > 0) fanNoteRow(fan, ring, n, rows, frames, visible, placedStartHz(rowCenterHz, rowHzPerBin, off, n), rowHzPerBin * n);
  writeRow(ring, slot * n, row, n, off);
  glideRow(ring, slot * n, n, rowCenterHz, rowHzPerBin, off, glide);
  fanAfterRow(fan, ring, slot, n, rows);
  return slot;
}
