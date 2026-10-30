/**
 * wfCombine.ts — WHEN THE WATERFALL THINS A FEED, COMBINE THE FRAMES INSTEAD OF DROPPING THEM (V12, Stuart 2026-10-30).
 * ONE rule for the app's JS path (WaterfallView pumpClock), the web client (web/client/src/waterfall.ts, via WfRowMerge
 * below) and — ported line for line — the Android engine (android/app/src/main/cpp/spectrum/wf_row_combine.h).
 * Pure, so scripts/test_wf_combine.ts proves it.
 *
 * ★★★ STUART: "with the frame rates that cut the rate where the server cannot send a lower rate rather than simply
 *     dropping the excess frames can we average them". Where a feed is faster than the chosen row rate (a server that
 *     cannot send less — OpenWebRX's fixed ~9 fps, a Kiwi's 5 fps step under MINIMUM, an UberSDR shared channel's ÷3, a
 *     23 fps Kiwi under ECO), the fixed clock used to show the newest frame and discard the rest. Now the frames that
 *     fall into one row are COMBINED into it:
 *
 *       per bin:  out = max( mean of the frames' LINEAR power,  the strongest frame's power − 6 dB )
 *
 *   ★★ LINEAR POWER, never dB: the mean of 0 dB and 10 dB is 7.4 dB, not 5 — a dB mean under-reads every signal that
 *      comes and goes. ★★ PEAKS KEPT: a 1-frame burst among 5 would be diluted by 7 dB in a plain power mean (1/5); the
 *      −6 dB floor under the strongest frame keeps it within 6 dB of its own level, so it stays plainly visible. On noise
 *      the mean wins (the strongest of 5 noise frames is ~3.6 dB over their mean, so peak − 6 dB sits ~2.4 dB under it):
 *      the floor comes out SMOOTHER than any single frame — the averaging Stuart asked for, without losing the bursts
 *      someone thinning the waterfall to find an intermittent signal (#57) is looking for.
 *   ★ A row made of ONE frame is that frame, byte for byte — so a feed at (or below) the row rate, NATIVE, and every path
 *     where the server already sends the chosen rate are untouched.
 *   ★ Frames are combined only with frames of the same geometry (bin count, low edge, span): across a tune or zoom the
 *     group restarts from the frame that moved — never a smear of two views.
 *
 * THE BYTES. A waterfall row is 0–255 intensity from signalProcessor (and its C++ twin): byte = (dB − floor) scaled over
 * the live range, with the bottom 14.97 units clipped to 0 and the rest re-stretched — LINEAR IN dB, at
 *   dB per unit = (dbMax − dbMin) × (255 − 14.97) / 255²
 * so a unit difference is a fixed dB difference and the offset cancels: power ∝ 10^(byte × dBperUnit / 10). A clipped 0 is
 * treated as the floor itself (it is below it — the combined floor reads at most a hair high, never a signal). The group
 * uses its first frame's range: the auto-range moves by a dB or two a second, far less than the gaps being combined.
 *
 * COST: one pass per frame (a 256-entry power table, an add and a max per bin into Float32 buffers that are reused), one
 * pass per row out (a log per bin, at the row rate — 2 to 10 a second). No allocation per frame. No latency: the row is
 * made when the clock shows it, from frames already presented.
 */

/** The bottom of the processor's byte scale that is clipped to 0 (signalProcessor CLIP_THRESHOLD). */
export const WF_CLIP_UNITS = 14.97;
/** How far under the strongest frame a combined bin may fall (dB) — the peak-keep rule. */
export const COMBINE_PEAK_DB = 6;
/** The most frames the app and the Android engine KEEP for one row (their frame stores are bounded — 2 rows/s from a 48
 *  fps feed); frames older than that in one row are dropped, as before. The combiner itself has no limit. */
export const COMBINE_MAX_FRAMES = 24;

/** dB per byte unit for a row the processor made over [dbMin, dbMax]. */
export const dbPerUnit = (dbMin: number, dbMax: number): number =>
  (dbMax > dbMin ? dbMax - dbMin : 60) * (255 - WF_CLIP_UNITS) / (255 * 255);

const PEAK_FRAC = Math.pow(10, -COMBINE_PEAK_DB / 10);

export interface RowCombiner {
  sum: Float32Array;     // per bin: the frames' linear power, summed
  peak: Float32Array;    // per bin: the strongest frame's linear power
  n: number;             // frames in the group (0 = empty)
  len: number; lo: number; span: number;   // the group's geometry
  k: number;             // the group's dB per unit
  lut: Float32Array;     // 10^(b·k/10) for b = 0…255, for `lutK`
  lutK: number;
  one: Uint8Array | null;   // the group's only frame so far, kept exact (n = 1 → out is it, byte for byte)
}
export const newRowCombiner = (): RowCombiner => ({
  sum: new Float32Array(0), peak: new Float32Array(0), n: 0, len: 0, lo: 0, span: 0, k: 0,
  lut: new Float32Array(256), lutK: -1, one: null,
});
export function combineReset(c: RowCombiner): void { c.n = 0; }

function ensure(c: RowCombiner, len: number) {
  if (c.sum.length < len) { c.sum = new Float32Array(len); c.peak = new Float32Array(len); }
  if (!c.one || c.one.length < len) c.one = new Uint8Array(len);
}
function setLut(c: RowCombiner, k: number) {
  if (c.lutK === k) return;
  for (let b = 0; b < 256; b++) c.lut[b] = Math.pow(10, b * k / 10);
  c.lutK = k;
}
function fold(c: RowCombiner, row: Uint8Array, len: number) {
  const lut = c.lut, sum = c.sum, peak = c.peak;
  for (let i = 0; i < len; i++) { const p = lut[row[i]]; sum[i] += p; if (p > peak[i]) peak[i] = p; }
}

/** Add a frame (`row`, its geometry, its dB per unit). A different geometry RESTARTS the group from this frame (returns
 *  false then — the frames before it are dropped). */
export function combineAdd(c: RowCombiner, row: Uint8Array, lo: number, span: number, k: number): boolean {
  const len = row.length;
  let kept = true;
  if (c.n > 0 && (len !== c.len || lo !== c.lo || span !== c.span)) { c.n = 0; kept = false; }
  ensure(c, len);
  if (c.n === 0) {
    c.len = len; c.lo = lo; c.span = span; c.k = k > 0 ? k : dbPerUnit(0, 0);
    c.one!.set(row);            // exact, until a second frame arrives
    c.n = 1;
    return kept;
  }
  setLut(c, c.k);
  if (c.n === 1) {              // the first frame goes into the sums only now — a lone frame never pays for the pass
    c.sum.fill(0, 0, len); c.peak.fill(0, 0, len);
    fold(c, c.one!, len);
  }
  fold(c, row, len);
  c.n++;
  return true;
}

/** The combined row into `out` (length = the group's), then the group is EMPTY. n = 1 → the frame exactly. Returns the
 *  frames combined (0 = nothing to write; `out` untouched). */
export function combineTake(c: RowCombiner, out: Uint8Array): number {
  const n = c.n, len = c.len;
  c.n = 0;
  if (n === 0 || out.length !== len) return 0;
  if (n === 1) { out.set(c.one!.subarray(0, len)); return 1; }
  const inv = 1 / n, scale = 10 / c.k, sum = c.sum, peak = c.peak;
  for (let i = 0; i < len; i++) {
    const m = sum[i] * inv, pk = peak[i] * PEAK_FRAC;
    const v = Math.round(Math.log10(m > pk ? m : pk) * scale);
    out[i] = v < 0 ? 0 : v > 255 ? 255 : v;
  }
  return n;
}

/**
 * ★★ THE WEB'S MERGE (waterfall.ts). The web draws the newest real row (cur) or the one before it (prev, while cur waits
 * for its presentation time) on the display's frames; a row that was never drawn before a newer one landed is the frame
 * the thinning used to throw away. This keeps those PENDING frames (never drawn, not cur) in a combiner:
 *   • arrive(): before cur rolls to prev — an undrawn cur joins the pending group (only on the clock; off it — NATIVE,
 *     actual rows — nothing is ever pending).
 *   • drawCur(): cur's FIRST draw — pending + cur are combined INTO cur (so its repeats, and its later turn as prev, show
 *     the combined row); the group empties.
 *   • drawPrev(): prev drawn while pending holds it (prev was undrawn when it rolled) — the group is combined INTO prev.
 */
export class WfRowMerge {
  private pend = newRowCombiner();
  private curDrawn = true;
  /** Frames in the last combined row (diagnostics / tests). */
  lastN = 0;
  /** A new frame is about to replace cur (`cur` = the outgoing one, still unrolled). `clocked` = rows come off the
   *  fixed clock (thinning possible). */
  arrive(clocked: boolean, cur: Uint8Array | null, lo: number, span: number, k: number): void {
    if (!clocked) combineReset(this.pend);
    else if (!this.curDrawn && cur) combineAdd(this.pend, cur, lo, span, k);
    this.curDrawn = false;
  }
  /** cur is being drawn. */
  drawCur(cur: Uint8Array, lo: number, span: number, k: number): void {
    if (this.curDrawn) return;
    this.curDrawn = true;
    if (this.pend.n === 0) { this.lastN = 1; return; }
    combineAdd(this.pend, cur, lo, span, k);
    this.lastN = combineTake(this.pend, cur);
  }
  /** prev is being drawn (cur not yet presented). */
  drawPrev(prev: Uint8Array): void {
    if (this.pend.n === 0) return;
    // (the group's geometry is prev's, or later frames restarted it — then prev is not in it and stays as it is)
    if (this.pend.len === prev.length) this.lastN = combineTake(this.pend, prev); else combineReset(this.pend);
  }
  reset(): void { combineReset(this.pend); this.curDrawn = true; }
}

/** A real frame kept for the clock (the app's clockFrames, the engine's clockRows_): its row, the geometry it was placed
 *  by (centre, Hz per bin) and its dB per unit. */
export interface CombFrame { row: Uint8Array; c: number; h: number; k: number }

/**
 * ★★ THE APP'S MERGE (WaterfallView pumpClock; the engine's runClock in C++). Frames get sequential ids and the clock
 * shows frame `id` at a tick; every frame after the last one SHOWN (`afterId`) and up to `id` fell into this row — the
 * ones the clock passed over (superseded before a tick, or pushed out of its queue) are combined INTO frame `id`'s row,
 * so its repeats show the combined row too. Frames no longer kept are simply missing (at most COMBINE_MAX_FRAMES).
 * A tick showing the frame right after the last shown (the feed at or below the row rate) touches nothing.
 * Returns the frames combined (1 = the frame as it came).
 */
export function combineInto(comb: RowCombiner, frames: { get(id: number): CombFrame | undefined }, afterId: number, id: number): number {
  const tgt = frames.get(id);
  if (!tgt) return 0;
  if (id - afterId <= 1) return 1;
  combineReset(comb);
  for (let j = Math.max(afterId + 1, id - COMBINE_MAX_FRAMES + 1); j <= id; j++) {
    const f = frames.get(j);
    if (f) combineAdd(comb, f.row, f.c, f.h, f.k);
  }
  return combineTake(comb, tgt.row);
}
