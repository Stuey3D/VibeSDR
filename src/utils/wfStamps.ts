/* wfStamps.ts — SCALE STAMPS on the waterfall history (GitHub discussion #57). Shared by the app's JS path
 * (WaterfallView.tsx), the web client (web/client/src/waterfall.ts); the Android engine's twin is cpp/spectrum/wf_stamps.h.
 *
 * ★★★ STUART'S DESIGN (2026-10-30, #57 — Gesiima asked for the old history to be re-mapped to the new scale, dark where it
 *     has no data): "No I want to avoid blank waterfalls where possible, so I wonder if adding a stamped history on the
 *     waterfall as it scrolls away would be better. zoom in and a stamp of the previous frequency scale as it was is added
 *     onto the old waterfall position and it gently drops off the bottom of the screen, same with tuning too if needed".
 *  ★★ SO THE HISTORY STAYS AS DRAWN (wfRowPlace.ts — never re-mapped, never blanked), and when a zoom or a tune that
 *     shifts the scale LANDS, a thin ruler of the PREVIOUS scale — ticks and a few labels, in the live scale's own style
 *     and units — is laid on the history at the top of the rows it describes (just below the first rows at the new scale).
 *     It scrolls down WITH those rows and fades as it drops off the bottom. An OVERLAY only: no row is touched.
 *  ★ WHEN: the detector watches each row's DISPLAYED geometry (Hz at the left edge, Hz across) as it is written. A row
 *    that differs from the settled scale starts a MOVE; the move has LANDED when STAMP_SETTLE_S of rows (≥ STAMP_MIN_ROWS)
 *    share one geometry — the ease is over and the dial has stopped. Only then, and only if the old scale MEANINGFULLY
 *    differs from the new one (stampMeaningful: the span by more than STAMP_ZOOM_FRAC, or an edge by more than
 *    STAMP_SHIFT_FRAC of the view), is ONE stamp placed — so a drag or a pinch sweep makes one stamp, not one per row,
 *    and a nudge whose history visibly continues makes none.
 *  ★ WHERE: a stamp is {the newest row at the old scale, that scale}. Its y is "rows newer than it × row height", read
 *    from the same counter that scrolls the picture, so it moves with the history exactly, through a resize or rotation
 *    too (the history is kept; so is the stamp, at its rows).
 *  ★ A JUMP (a bookmark, a typed frequency — the one move that DOES move the history into place, wfRowPlace.ts fanNoteRow)
 *    moves the stamps' scales by the same Hz (stampJump): they describe what is drawn, and the drawing moved.
 *  ★ COST: nothing while there are none — one compare a row. A handful of labels a frame at most (STAMP_MAX).
 * PURE — scripts/test_wf_stamps.ts. */

/** ★★ The most stamps kept; a new one past it drops the oldest (Stuart, 2026-10-30: rapid tuning, panning and zooming
 *  must never crowd the screen or load the CPU — a hard cap of 3 on screen). */
export const STAMP_MAX = 3;
/** ★★ …and the closest two may be, in label heights: a new stamp within this of the one below REPLACES it instead of
 *  adding a second (rapid separate gestures). The caller sets StampState.minGapRows from its label height in rows. */
export const STAMP_MIN_GAP_LABELS = 3;
/** How long a geometry must hold before a move counts as landed (the ease is over, the dial stopped). */
export const STAMP_SETTLE_S = 0.5;
/** …and never fewer rows than this (Native on a slow feed). */
export const STAMP_MIN_ROWS = 3;
/** A stamp is worth placing when the span differs by more than this fraction (any real zoom — the smallest step is ×√2)… */
export const STAMP_ZOOM_FRAC = 0.02;
/** …or an edge of the view moved by more than this fraction of it (labels a twentieth of the view out). */
export const STAMP_SHIFT_FRAC = 1 / 20;
/** Two geometries closer than this fraction of the span are the same (float noise, a sub-column wobble). */
export const STAMP_SAME_FRAC = 2e-4;
/** Rows over which a new stamp fades in (so it never pops). */
export const STAMP_FADE_IN_ROWS = 8;
/** The fraction of the waterfall's height over which a stamp fades out as it nears the bottom. */
export const STAMP_FADE_OUT_FRAC = 0.3;

export interface WfStamp {
  id: number;
  /** The newest row (absolute row index) at this scale — the stamp sits on its top edge. */
  frame: number;
  /** The scale those rows were drawn at: Hz at the left edge, Hz across the view. */
  lo: number; span: number;
  /** The newest row index when it was placed (its fade in). */
  born: number;
  /** ★ When it was placed (ms since the epoch) — printed at its left end, so the old scale says WHEN as well as WHERE
   *  (Stuart, 2026-10-30: "can we add a timestamp to the frequency stamp on the waterfall"). Optional: the Android
   *  engine's stamps get it when they first reach JS. */
  at?: number;
}

/** A stamp's time label: local 24-hour HH:MM:SS. */
export function stampTimeLabel(at: number): string {
  const d = new Date(at);
  const p = (n: number) => (n < 10 ? '0' : '') + n;
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

export interface StampState {
  /** The settled scale of the rows being written, and the newest row at it (NaN = none yet). */
  refLo: number; refSpan: number; refFrame: number;
  /** A move is under way: the candidate geometry and the first row at it. */
  moving: boolean; candLo: number; candSpan: number; candFrom: number;
  /** Newest first. */
  stamps: WfStamp[];
  nextId: number;
  /** The fewest rows allowed between two stamps (STAMP_MIN_GAP_LABELS label heights, in rows — set by the caller, which
   *  knows its row and label heights). */
  minGapRows: number;
  /** ★★ A LONG TUNE/PAN SWEEP: rows between mid-run stamps (the caller's max(STAMP_SWEEP_S of rows, a quarter of the
   *  waterfall)); 0 = never. Run state: the first row of the move, the newest stamp of this run, whether the pre-run
   *  stamp is already down, and whether the run changed the span (a zoom run never gets mid-run stamps). */
  sweepRows: number;
  moveFrom: number; runStampFrame: number; preDone: boolean; runZoomed: boolean;
  /** Bumped on every change to `stamps` (a stamp added, dropped, or moved by a jump). */
  gen: number;
}

export function newStampState(): StampState {
  return { refLo: NaN, refSpan: NaN, refFrame: -1, moving: false, candLo: NaN, candSpan: NaN, candFrom: -1,
           stamps: [], nextId: 1, gen: 0, minGapRows: 45,
           sweepRows: 0, moveFrom: -1, runStampFrame: -1, preDone: false, runZoomed: false };
}

/** Forget everything (the history was cleared). */
export function stampReset(st: StampState): void {
  const had = st.stamps.length > 0;
  st.refLo = NaN; st.refSpan = NaN; st.refFrame = -1; st.moving = false; st.candLo = NaN; st.candSpan = NaN; st.candFrom = -1;
  st.stamps.length = 0;
  if (had) st.gen++;
}

/** ★★ A long continuous tune or pan sweep gets an occasional stamp on its slanted history (Stuart, 2026-10-30: "the only
 *  time a label should be on a long slanted run should be if a user is doing a long sweep tune or pan and only every now
 *  and then so it doesnt overwhelm things") — no more often than this… */
export const STAMP_SWEEP_S = 2.5;
/** …and only once the history has scrolled this fraction of the waterfall since the last one. */
export const STAMP_SWEEP_VIEW_FRAC = 0.25;
/** The rows between mid-sweep stamps, at `rowRate` rows/s on a waterfall `viewRows` rows tall. */
export function stampSweepRows(rowRate: number, viewRows: number): number {
  return Math.max(Math.ceil((rowRate > 0 ? rowRate : 30) * STAMP_SWEEP_S), Math.ceil(viewRows * STAMP_SWEEP_VIEW_FRAC));
}

function addStamp(st: StampState, frame: number, lo: number, span: number, born: number): void {
  // ★★ Never crowded: a stamp within minGapRows of the one below REPLACES it (one label for that stretch, not two).
  if (st.stamps.length && frame - st.stamps[0].frame < st.minGapRows) st.stamps.shift();
  st.stamps.unshift({ id: st.nextId++, frame, lo, span, born, at: Date.now() });
  if (st.stamps.length > STAMP_MAX) st.stamps.length = STAMP_MAX;
  st.gen++;
}

/** Rows a move must hold still for before it counts as landed, at `rowRate` rows/s. */
export function stampSettleRows(rowRate: number): number {
  return Math.max(STAMP_MIN_ROWS, Math.ceil((rowRate > 0 ? rowRate : 30) * STAMP_SETTLE_S));
}

function same(lo0: number, span0: number, lo1: number, span1: number): boolean {
  const tol = Math.max(span0, span1) * STAMP_SAME_FRAC;
  return Math.abs(lo0 - lo1) <= tol && Math.abs((lo0 + span0) - (lo1 + span1)) <= tol;
}

/** True when rows drawn at (lo0, span0) read meaningfully differently from the scale (lo1, span1): the span differs by
 *  more than STAMP_ZOOM_FRAC, or either edge is more than STAMP_SHIFT_FRAC of the view out. */
export function stampMeaningful(lo0: number, span0: number, lo1: number, span1: number): boolean {
  if (!(span0 > 0 && span1 > 0)) return false;
  if (Math.abs(span0 - span1) > span1 * STAMP_ZOOM_FRAC) return true;
  const edge = Math.max(Math.abs(lo0 - lo1), Math.abs((lo0 + span0) - (lo1 + span1)));
  return edge > span1 * STAMP_SHIFT_FRAC;
}

/** AFTER row `frame` (absolute, increasing by one a row) is written, drawn at (lo, span) — its DISPLAYED geometry: Hz at
 *  the left edge of the screen and across it. Returns true when the stamps changed. An unknown geometry is skipped. */
export function stampNoteRow(st: StampState, frame: number, lo: number, span: number, settleRows: number): boolean {
  if (!(span > 0) || !isFinite(lo)) return false;
  if (!(st.refSpan > 0)) { st.refLo = lo; st.refSpan = span; st.refFrame = frame; st.moving = false; return false; }
  if (!st.moving) {
    if (same(lo, span, st.refLo, st.refSpan)) { st.refFrame = frame; return false; }
    st.moving = true; st.candLo = lo; st.candSpan = span; st.candFrom = frame;
    st.moveFrom = frame; st.runStampFrame = -1; st.preDone = false; st.runZoomed = false;
    if (Math.abs(span - st.refSpan) > st.refSpan * STAMP_ZOOM_FRAC) st.runZoomed = true;
    return false;
  }
  if (Math.abs(span - st.refSpan) > st.refSpan * STAMP_ZOOM_FRAC) st.runZoomed = true;
  // ★★ A LONG TUNE/PAN SWEEP (never a zoom run): every sweepRows of continuous motion, one stamp on the slanted history
  //    at the row being drawn now — with the pre-run stamp put down first, the moment the sweep proves long.
  if (st.sweepRows > 0 && !st.runZoomed && frame - Math.max(st.moveFrom, st.runStampFrame) >= st.sweepRows
      && stampMeaningful(st.refLo, st.refSpan, lo, span)) {
    if (!st.preDone && st.refFrame >= 0) addStamp(st, st.refFrame, st.refLo, st.refSpan, frame);
    addStamp(st, frame, lo, span, frame);
    st.preDone = true; st.runStampFrame = frame;
    if (!same(lo, span, st.candLo, st.candSpan)) { st.candLo = lo; st.candSpan = span; st.candFrom = frame; }
    return true;
  }
  if (!same(lo, span, st.candLo, st.candSpan)) { st.candLo = lo; st.candSpan = span; st.candFrom = frame; return false; }
  if (frame - st.candFrom + 1 < settleRows) return false;
  // ★ LANDED: the rows have held one geometry long enough. One stamp for the whole move, describing the rows below it.
  st.moving = false;
  let changed = false;
  // ★ (a long sweep already put its pre-run stamp down; the live scale at the top is the landed one — no stamp for it)
  if (!st.preDone && stampMeaningful(st.refLo, st.refSpan, lo, span) && st.refFrame >= 0) {
    addStamp(st, st.refFrame, st.refLo, st.refSpan, frame);
    changed = true;
  }
  st.preDone = false; st.runZoomed = false; st.runStampFrame = -1;
  st.refLo = lo; st.refSpan = span; st.refFrame = frame;
  return changed;
}

/** A JUMP moved every row of the history by `dHz` (each in its own scale — wfRowPlace.ts fanNoteRow): the stamps, and
 *  the scale being watched, describe what is drawn, so they move with it. */
export function stampJump(st: StampState, dHz: number): boolean {
  if (!(dHz !== 0) || !isFinite(dHz)) return false;
  if (st.refSpan > 0) st.refLo += dHz;
  if (st.moving && st.candSpan > 0) st.candLo += dHz;
  if (!st.stamps.length) return false;
  for (const s of st.stamps) s.lo += dHz;
  st.gen++;
  return true;
}

/** Drop stamps whose rows have scrolled out of the last `keepRows` (off the bottom of the screen, or out of the ring).
 *  `newest` = the newest row written. Returns true when any went. */
export function stampPrune(st: StampState, newest: number, keepRows: number): boolean {
  let n = st.stamps.length;
  while (n > 0 && newest - st.stamps[n - 1].frame > keepRows) n--;
  if (n === st.stamps.length) return false;
  st.stamps.length = n;
  st.gen++;
  return true;
}

/** Which stamps to draw, newest first: one whose block is narrower than the stamp itself (the next newer stamp sits on
 *  top of it — a move that landed and moved again at once) is hidden; the newer one describes what is visible there.
 *  `rowPx` = one row's height, `bandPx` = a stamp's height. */
export function stampsShown(stamps: readonly WfStamp[], rowPx: number, bandPx: number): WfStamp[] {
  const out: WfStamp[] = [];
  let prev: WfStamp | null = null;
  for (const s of stamps) {
    if (prev && (prev.frame - s.frame) * rowPx < bandPx) continue;
    out.push(s); prev = s;
  }
  return out;
}

/** A stamp's opacity (0..1, times its own style's alpha): fading in over its first STAMP_FADE_IN_ROWS rows and out over
 *  the last STAMP_FADE_OUT_FRAC of the waterfall. `y` = its top, `bandPx` its height, `viewH` the waterfall's height,
 *  `rowsSinceBorn` = rows written since it was placed. */
export function stampAlpha(y: number, bandPx: number, viewH: number, rowsSinceBorn: number): number {
  'worklet';
  const fin = Math.max(0, Math.min(1, rowsSinceBorn / STAMP_FADE_IN_ROWS));
  const fade = Math.max(1, viewH * STAMP_FADE_OUT_FRAC);
  const fout = Math.max(0, Math.min(1, (viewH - y - bandPx) / fade));
  return fin * fout;
}

/** Ticks for a stamp's scale (lo, span) across `width` px, from the live scale's own step and label functions — so the
 *  stamp reads in exactly the units the live scale uses at that span. `step(span)` = Hz between ticks; `label(hz, step)`.
 *  Labels closer than `minGapPx` are dropped (the tick stays). */
export function stampTicks(lo: number, span: number, width: number, step: (span: number) => number,
                           label: (hz: number, step: number) => string, minGapPx: number): { x: number; label: string | null }[] {
  const out: { x: number; label: string | null }[] = [];
  if (!(span > 0 && width > 0)) return out;
  const s = step(span);
  if (!(s > 0)) return out;
  const pxPerHz = width / span;
  let last = -1e9;
  for (let f = Math.ceil(lo / s) * s; f <= lo + span && out.length < 64; f += s) {
    const x = (f - lo) * pxPerHz;
    const show = x - last >= minGapPx;
    if (show) last = x;
    out.push({ x, label: show ? label(f, s) : null });
  }
  return out;
}

/** THE APP's shader (WaterfallView WF_SKSL): the display LINE (0 = the top; × the row height = y) where row `frame`'s top
 *  edge is drawn, from the very uniforms that scroll the picture — so the stamp moves with its rows on every display
 *  frame, with no drift. The shader draws line L from A = (head − 2)·uN + R − L, frame floor(A / uN); settled (quant) L
 *  and R are whole, while gliding they are continuous. */
export function stampLine(head: number, frame: number, frac: number, uN: number, quant: boolean): number {
  'worklet';
  const R = quant ? Math.floor(frac * uN + 0.0001) : frac * uN;
  return (head - 3 - frame) * uN + R + (quant ? 1 : 0);
}

/** ★★ THE STAMP'S COLOUR (Stuart, 2026-10-30: "the colour of the labels must be shadowed and be an opposite colour to the
 *  waterfall so they are readable, on my personal setup i'd simply use orange like my VFO"): the user's own VFO/needle
 *  colour, drawn with a dark shadow. THE ONE FALLBACK, kept simple: when that colour's HUE is within STAMP_HUE_CLASH° of
 *  the palette's own at any of its busy levels (LUT 96, 160, 224 — what most of a waterfall is drawn in), and both are
 *  saturated, the stamp is white instead (a green VFO over a green palette). `lut` = 256 × RGBA. Returns '#rrggbb'. */
export const STAMP_HUE_CLASH = 35;
export function stampColour(needleHex: string, lut: ArrayLike<number> | null): string {
  const h = /^#?([0-9a-f]{6})$/i.exec(needleHex || '');
  if (!h) return '#ffffff';
  const n = parseInt(h[1], 16);
  const hs = (r: number, g: number, b: number): [number, number] => {
    const mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn;
    if (mx <= 0 || d === 0) return [0, 0];
    const hue = mx === r ? ((g - b) / d + 6) % 6 : mx === g ? (b - r) / d + 2 : (r - g) / d + 4;
    return [hue * 60, d / mx];
  };
  const [nh, ns] = hs((n >> 16) & 255, (n >> 8) & 255, n & 255);
  if (lut && lut.length >= 1024 && ns > 0.3) {
    for (const i of [96, 160, 224]) {
      const [ph, ps] = hs(lut[i * 4], lut[i * 4 + 1], lut[i * 4 + 2]);
      const dh = Math.min(Math.abs(ph - nh), 360 - Math.abs(ph - nh));
      if (ps > 0.3 && dh < STAMP_HUE_CLASH) return '#ffffff';
    }
  }
  return '#' + h[1].toLowerCase();
}

/** ★★ The row drawn at display line L (the inverse of stampLine): a tap at y on the waterfall is on this row — and tunes by
 *  ITS scale (Stuart, 2026-10-30, #57 — Gesiima: "I look back in the waterfall to see where exactly the signal was, and
 *  then I tune onto that position"). Same uniforms as the shader. */
export function rowAtLine(head: number, L: number, frac: number, uN: number, quant: boolean): number {
  const Lq = quant ? Math.floor(L) : L;
  const R = quant ? Math.floor(frac * uN + 0.0001) : frac * uN;
  return Math.floor(((head - 2) * uN + R - Lq) / uN);
}

/** The frequency at x (0..1 across the view) on a row displayed at (lo, span) — the live scale when the row's own is
 *  unknown. */
export function hzOnRow(x: number, rowGeom: readonly [number, number] | null, liveLo: number, liveSpan: number): number {
  if (rowGeom && rowGeom[1] > 0 && isFinite(rowGeom[0])) return rowGeom[0] + x * rowGeom[1];
  return liveLo + x * liveSpan;
}
