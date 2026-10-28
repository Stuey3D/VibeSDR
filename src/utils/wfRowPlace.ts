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

import { layerFillRow, type LayerRing } from './wfWide.ts';   // ★ .ts: node runs this file directly (the tests)

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

/* ── MOTION: a tune drawn as ONE continuous trail, by the SHADER (Stuart, 2026-10-28: "I just wanted the chunky broken
 *    diagonal lines joined up so they looked solid"; the reference is the first native build's MW sweep) ────────────────
 *  Placement above is honest but SPARSE in time: a ring row is one display line (devRowPx device px) and a tune moves a
 *  station several columns between two rows, so its diagonal was drawn as dashes. The first build never had that because
 *  its rows were thick (a 256-row ring stretched over the whole height, 2–3 lines a frame) and the shader blended
 *  neighbouring frames across them — the trail was soft and solid. Rows are one device line now (the scroll pace and
 *  the history depth Stuart settled on), so the blend alone has nothing to blend across.
 *  ★★ THE FIX IS MOTION-COMPENSATED INTERPOLATION, in the shader: each ring slot carries how far its trail moves (in
 *    DISPLAY columns) to the next newer row — `h`, from the two rows' own placement maps — and a line drawn between
 *    frames f and f+1 at fraction t samples f at x − t·h and f+1 at x + (1−t)·h. A moving station (and the noise with it:
 *    the WHOLE row moves, as in the video) slides continuously between data rows instead of cross-fading between two
 *    places. Settled (whole-row steps) the same warp is applied to the ONE frame a row shows, so a row bends into the next
 *    instead of jumping — a real measurement, displaced, never an average of two moments.
 *  ★ It replaces the GLIDE STREAK (glideRow: a moved row's signal max-smeared back to the previous row's place). That
 *    drew the motion twice once the shader interpolates, and it was the "over-sharpened, smeared" look Stuart rejected.
 *  ★ NOTHING STORED IS REWRITTEN: `h` is metadata beside the ring (two extra IMAGE rows, MOTION_ROWS, made with the ring
 *    image so the two can never be out of step). Steady rows have h = 0 and draw exactly as before.
 *  ★ Both cases from one rule — each row's OWN placement map: a view-only pan over a fixed capture (UberSDR/Kiwi/OWRX:
 *    every row arrives already for its view) and a TUNER that moves (VibeServer FREE tuning, a locked VFO retuning the
 *    dongle — #57: each row placed at the tuner position its samples came from). h is the difference of the two maps,
 *    whatever moved them.
 *  ★ A move of more than MOTION_MAX_FRAC of the width between two rows is a JUMP (a bookmark, a typed frequency, a spin
 *    faster than the radio): not interpolated — the history is RE-MAPPED to the new view instead (THE FAN below, with a
 *    translation), so it stays visible, shifted into place, black only where the radio has not captured yet.
 *  ★ A ZOOM is never motion: the fan re-maps every row to the current scale, so two neighbouring rows always share a
 *    scale on screen (g = 1) and h across a zoom step is 0.
 *  ★ FOR A SECOND (COARSE) RING: the maps are in Hz (column x of slot r shows P_r + Q_r·x), not in another ring's
 *    columns, so a coarse full-capture ring with its own slots and maps can be related to the same view and drawn
 *    under this one where it is blank — BUILT (V12): wfWide.ts, the full-capture layer, filled in by fanRegen.
 *  PURE — scripts/test_wf_row_place.ts (motion section); the native twin is cpp/spectrum/wf_glide.h. */

/** Extra image rows below the ring: row RING_ROWS = the high byte of each slot's h, RING_ROWS+1 = the low byte. */
export const MOTION_ROWS = 2;
/** h is stored as round(h × MOTION_SCALE) + MOTION_ZERO in 16 bits: ±512 columns at 1/64 of a column. */
export const MOTION_SCALE = 64;
export const MOTION_ZERO = 32768;
/** The largest move between two rows that is interpolated, as a fraction of the width; past it is a jump (re-mapped). */
export const MOTION_MAX_FRAC = 1 / 8;

/** True when two rows' bin scales differ enough to count as a zoom (not float noise). */
export function spanChanged(span0: number, span1: number): boolean {
  return span0 > 0 && span1 > 0 && Math.abs(span1 - span0) > span0 * 1e-6;
}

/** Where the row's bins went: column c of the WRITTEN row is startHz + c × hzPerCol. `off` is what writeRow slid it by.
 *  NaN when the row's own centre/scale is unknown (it cannot be related to its neighbours). */
export function placedStartHz(rowCenterHz: number, rowHzPerBin: number, off: number, n: number): number {
  if (!(rowCenterHz > 0 && rowHzPerBin > 0 && n > 0)) return NaN;
  return rowCenterHz - (off + n / 2) * rowHzPerBin;
}

/** Per-slot placement + the move to the next newer row, and the image's motion rows. Allocated once. */
export interface MotionState {
  /** Column c of the STORED row in slot r is startHz[r] + c·hzPerCol[r] Hz (NaN = unknown). */
  startHz: Float64Array; hzPerCol: Float64Array;
  /** Slot r's trail moves h[r] DISPLAY columns between it and the row written after it (0 = steady / none). */
  h: Float32Array;
  /** MOTION_ROWS × n bytes: the image's tail rows (column r = slot r). */
  bytes: Uint8Array;
  /** Absolute frames f whose pair (f → f+1) moves: [lo, hi]; lo > hi = none. The shader fetches h only inside it. */
  lo: number; hi: number;
}
export function newMotionState(rows: number, n: number): MotionState {
  const bytes = new Uint8Array(MOTION_ROWS * n);
  for (let r = 0; r < n; r++) { bytes[r] = MOTION_ZERO >> 8; bytes[n + r] = MOTION_ZERO & 255; }
  return { startHz: new Float64Array(rows).fill(NaN), hzPerCol: new Float64Array(rows), h: new Float32Array(rows),
           bytes, lo: 1, hi: 0 };
}

export function encodeMotion(h: number): number {
  return Math.max(0, Math.min(65535, Math.round(h * MOTION_SCALE) + MOTION_ZERO));
}
/** What the shader reads back (hi, lo bytes → columns). */
export function decodeMotion(hi: number, lo: number): number {
  return ((hi * 256 + lo) - MOTION_ZERO) / MOTION_SCALE;
}

/** The DISPLAY map of slot r: screen column x (0..n) shows P + Q·x Hz — its stored placement through its fan map
 *  (stored column u = a·x + b·n). Null when the slot's placement is unknown. */
export function displayMap(ms: MotionState, xfA: Float64Array, xfB: Float64Array, r: number, n: number): [number, number] | null {
  const s = ms.startHz[r], hz = ms.hzPerCol[r];
  if (!isFinite(s) || !(hz > 0)) return null;
  return [s + hz * xfB[r] * n, hz * xfA[r]];
}

/** How far a trail moves on screen from slot `a` (older) to slot `b` (the next newer), in display columns: a station at
 *  column x in a is at x + h in b. 0 when steady, unknown, a change of scale on screen (cannot be — the fan — but never
 *  guessed) or a jump. */
export function pairMotion(ms: MotionState, xfA: Float64Array, xfB: Float64Array, a: number, b: number, n: number): number {
  const ma = displayMap(ms, xfA, xfB, a, n), mb = displayMap(ms, xfA, xfB, b, n);
  if (!ma || !mb) return 0;
  if (Math.abs(ma[1] / mb[1] - 1) > 1e-3) return 0;
  const h = (ma[0] - mb[0]) / mb[1];
  if (!isFinite(h) || Math.abs(h) < 1 / MOTION_SCALE || Math.abs(h) > n * MOTION_MAX_FRAC) return 0;
  return h;
}

function setMotion(ms: MotionState, slot: number, h: number, n: number): void {
  ms.h[slot] = h;
  const v = encodeMotion(h);
  ms.bytes[slot] = v >> 8; ms.bytes[n + slot] = v & 255;
}

/** Recompute every pair among the newest `rows` frames (after a fan re-mapped them all) and the moving range. */
export function motionRecompute(ms: MotionState, xfA: Float64Array, xfB: Float64Array, n: number, rows: number, frames: number): void {
  ms.lo = 1; ms.hi = 0;
  const first = Math.max(0, frames - rows);
  for (let f = first; f < frames; f++) {
    const r = f % rows;
    const h = f + 1 < frames ? pairMotion(ms, xfA, xfB, r, (f + 1) % rows, n) : 0;
    setMotion(ms, r, h, n);
    if (h !== 0) { if (ms.lo > ms.hi) ms.lo = f; ms.hi = f; }
  }
}

/** AFTER frame `f` (absolute) is written into `slot` at placement (startHz, hzPerCol): note it, set the move of the pair
 *  (f−1 → f), clear this slot's own (it has no newer row yet) — or recompute all when the fan just re-mapped. */
export function motionAfterRow(ms: MotionState, xfA: Float64Array, xfB: Float64Array, n: number, rows: number, f: number,
                               startHz: number, hzPerCol: number, refanned: boolean): void {
  const slot = f % rows;
  ms.startHz[slot] = isFinite(startHz) && hzPerCol > 0 ? startHz : NaN;
  ms.hzPerCol[slot] = hzPerCol > 0 ? hzPerCol : 0;
  if (refanned) { motionRecompute(ms, xfA, xfB, n, rows, f + 1); return; }
  setMotion(ms, slot, 0, n);
  if (f > 0) {
    const p = (f - 1) % rows;
    const h = pairMotion(ms, xfA, xfB, p, slot, n);
    setMotion(ms, p, h, n);
    if (h !== 0) { if (ms.lo > ms.hi) ms.lo = f - 1; ms.hi = f - 1; }
  }
  // Pairs older than the ring are gone: keep the range inside it (conservative — it may span steady pairs).
  const oldest = f + 1 - rows;
  if (ms.hi < oldest) { ms.lo = 1; ms.hi = 0; } else if (ms.lo < oldest) ms.lo = oldest;
}

/** The shader's motion-compensated sample, on the CPU — the reference the tests (and the comparison render) run.
 *  `rowF(r, x)` reads the DISPLAYED ring at slot r, column x (0 outside 0..n: a vacated edge is the floor).
 *  Between frames f (slot ra) and f+1 (slot rb) at fraction t, a trail moving h columns: f sampled at x − t·h, f+1 at
 *  x + (1−t)·h, blended by t. `blend` false = settled: frame f alone, warped toward f+1 by t. */
export function motionSample(rowF: (r: number, x: number) => number, ra: number, rb: number, x: number, t: number, h: number,
                             blend: boolean): number {
  const a = rowF(ra, x - t * h);
  if (!blend) return a;
  return a + (rowF(rb, x + (1 - t) * h) - a) * t;
}

/* ── THE ZOOM FAN: a zoom re-maps the whole history to the new view (Stuart, 2026-10-27/28: zooming left the history in
 *    horizontal SLICES — every old row kept the scale it was drawn at, so the waterfall read as bands of different zoom;
 *    the web client was fixed first, web/client/src/wfZoomFan.ts, and this is the same design for the app's ring) ─────────
 *  The STORED ring (what writeRow makes) is never rewritten. Each slot carries ONE affine map from the screen's
 *  x (0..1 across the view) to its own u (0..1 across the row as stored): u = a·x + b, the identity when it is written.
 *  When a row arrives at a different SPAN than the row before (a zoom — drum, pinch, buttons, all read off the rows), every
 *  slot's map composes the screen move that zoom made (applyViewChange, the web's maths) and the DISPLAYED ring is
 *  regenerated from the stored one through the maps: the history stretches out on a zoom in and draws in toward the
 *  anchor on a zoom out, blank only where it never had data.
 *  ★ A change of CENTRE alone (a tune) does NOT touch the maps — that is the trail that bends and then runs straight
 *    (#57), drawn continuous by the shader's motion compensation (MOTION above). A zoom that also moves the centre takes
 *    its translation with it, so the anchor stays put.
 *  ★★ A JUMP IS RE-MAPPED LIKE A ZOOM (2026-10-28, Stuart's spec: "NO SLICING when zooming or tuning … MINIMAL black gaps
 *    ONLY when absolutely necessary due to the SDR catching up from a large move or rapid tuning"): a row placed more than
 *    MOTION_MAX_FRAC of the width from the row before (a bookmark, a typed frequency, a spin faster than the radio) is too
 *    far to draw as a bend, so the whole history is translated to the new view — still visible, shifted into place, black
 *    only at the newly exposed edge. A bend is continuous; a jump left in place would be a slice.
 *  ★ THE SCREEN EASES THE STEP (stepK/stepC, takeFanStep): each fan composes the move it made, and the UI thread eases
 *    the shader from the old geometry to the new over one row interval at display rate — so a pinch, a drum step or a
 *    jump fans/slides in smoothly at 120 Hz instead of snapping once per data row. Two uniforms; no texture reads.
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
  /** The screen move every fan since the last takeFanStep made, composed: x_old = stepC + stepK·x_new (0..1 units). */
  stepK: number; stepC: number;
  /** ★ V12 THE FULL-CAPTURE LAYER (wfWide.ts): one layer row per slot, in capture space — null until the first SPWB (an
   *  older server never allocates it). fanRegen fills each regenerated row's texels outside its detailed extent from it. */
  layer: LayerRing | null;
}
export function newFanState(rows: number): FanState {
  return { lastLo: 0, lastSpan: 0, xfA: new Float64Array(rows).fill(1), xfB: new Float64Array(rows), disp: null, sinceZoom: 0,
           validFrom: 0, lo: null, hi: null, stepK: 1, stepC: 0, layer: null };
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
    } else {
      for (let x = 0; x < n; x++) {                                            // shrunk: the max under the pixel
        const l = lo[x];
        if (l < 0) { disp[at + x] = 0; continue; }
        const h = hi[x];
        let m = ring[at + l];
        for (let k = l + 1; k <= h; k++) { const v = ring[at + k]; if (v > m) m = v; }
        disp[at + x] = m;
      }
    }
    // ★★ V12: outside the detailed extent, the row's own full-capture layer through the same map (black only where
    //    the radio was not capturing) — wfWide.ts layerFillRow.
    if (st.layer) layerFillRow(st.layer, r, a, b, n, disp, at, lo);
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
  if (frames > 0 && st.lastSpan > 0 && (spanChanged(st.lastSpan, span) || Math.abs(lo - st.lastLo) > st.lastSpan * MOTION_MAX_FRAC)) {
    const k = span / st.lastSpan, c = (lo - st.lastLo) / st.lastSpan;
    st.stepC += st.stepK * c; st.stepK *= k;   // x0 = c1 + k1·(c2 + k2·x2)
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

/** AFTER the row at `slot` (0..rows-1) is written: it is the identity, and the display gets it as is. */
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

/** The screen move the fans made since the last call (x_old = c + k·x_new), reset to none. The UI eases from it. */
export function takeFanStep(st: FanState): [number, number] {
  const r: [number, number] = [st.stepK, st.stepC];
  st.stepK = 1; st.stepC = 0;
  return r;
}

/** The bytes to make the waterfall image from: the displayed ring with the motion rows written into its tail (the buffer
 *  is RING_W × (RING_ROWS + MOTION_ROWS); the image is made that tall). Copies 2 × n bytes. */
export function wfImageBytes(st: FanState, ms: MotionState, ring: Uint8Array, n: number, rows: number): Uint8Array {
  const b = st.disp ?? ring;
  if (b.length >= (rows + MOTION_ROWS) * n) b.set(ms.bytes, rows * n);
  return b;
}

/** The bytes to make the waterfall image from. */
export function fanImage(st: FanState, ring: Uint8Array): Uint8Array { return st.disp ?? ring; }

/** pushRow's whole placement, in one place (WaterfallView's JS path; the Android engine's twin is wf_zoom_fan.h
 *  placeRow): slide the row onto the view (rowViewOffset/writeRow), fan the history if the span changed or the row
 *  jumped, keep the display in step, note the row's placement and the move from the row before (MOTION). `frames` = rows
 *  written before this one; `visible` = how many of the newest rows can be on screen (the fan makes only those);
 *  `placed` (optional) receives the row's placement (column 0's Hz, its span) — the full-capture layer's k and m. Returns
 *  the slot written. */
export function placeRow(ring: Uint8Array, rows: number, n: number, frames: number, row: Uint8Array,
                         rowCenterHz: number, rowHzPerBin: number, viewCenterHz: number, viewBwHz: number,
                         motion: MotionState, fan: FanState, visible = rows,
                         placed?: { startHz: number; spanHz: number }): number {
  const slot = frames % rows;
  const off = rowViewOffset(rowCenterHz, rowHzPerBin, viewCenterHz, viewBwHz, n);
  const start = placedStartHz(rowCenterHz, rowHzPerBin, off, n);
  if (placed) { placed.startHz = start; placed.spanHz = rowHzPerBin * n; }   // ★ V12: where the layer row goes (wfWide.ts)
  const fanned = rowHzPerBin > 0 ? fanNoteRow(fan, ring, n, rows, frames, visible, start, rowHzPerBin * n) : false;
  writeRow(ring, slot * n, row, n, off);
  fanAfterRow(fan, ring, slot, n, rows);
  motionAfterRow(motion, fan.xfA, fan.xfB, n, rows, frames, start, rowHzPerBin, fanned);
  return slot;
}
