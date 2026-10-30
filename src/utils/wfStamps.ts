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

/** The most stamps kept; a new one past it drops the oldest. */
export const STAMP_MAX = 6;
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
}

export interface StampState {
  /** The settled scale of the rows being written, and the newest row at it (NaN = none yet). */
  refLo: number; refSpan: number; refFrame: number;
  /** A move is under way: the candidate geometry and the first row at it. */
  moving: boolean; candLo: number; candSpan: number; candFrom: number;
  /** Newest first. */
  stamps: WfStamp[];
  nextId: number;
  /** Bumped on every change to `stamps` (a stamp added, dropped, or moved by a jump). */
  gen: number;
}

export function newStampState(): StampState {
  return { refLo: NaN, refSpan: NaN, refFrame: -1, moving: false, candLo: NaN, candSpan: NaN, candFrom: -1,
           stamps: [], nextId: 1, gen: 0 };
}

/** Forget everything (the history was cleared). */
export function stampReset(st: StampState): void {
  const had = st.stamps.length > 0;
  st.refLo = NaN; st.refSpan = NaN; st.refFrame = -1; st.moving = false; st.candLo = NaN; st.candSpan = NaN; st.candFrom = -1;
  st.stamps.length = 0;
  if (had) st.gen++;
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
    return false;
  }
  if (!same(lo, span, st.candLo, st.candSpan)) { st.candLo = lo; st.candSpan = span; st.candFrom = frame; return false; }
  if (frame - st.candFrom + 1 < settleRows) return false;
  // ★ LANDED: the rows have held one geometry long enough. One stamp for the whole move, describing the rows below it.
  st.moving = false;
  let changed = false;
  if (stampMeaningful(st.refLo, st.refSpan, lo, span) && st.refFrame >= 0) {
    st.stamps.unshift({ id: st.nextId++, frame: st.refFrame, lo: st.refLo, span: st.refSpan, born: frame });
    if (st.stamps.length > STAMP_MAX) st.stamps.length = STAMP_MAX;
    st.gen++; changed = true;
  }
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
