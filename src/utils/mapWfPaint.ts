/**
 * mapWfPaint.ts — the map drawer's live waterfall, its PURE half (MapMiniWaterfall.tsx draws it with Skia): the ring of
 * palette indices, a row re-levelled into it, and the RGBA the drawer paints. Here so a test can run real captured frames
 * through exactly what the drawer does (scripts/test_map_wf.ts).
 */
export interface MwfStore {
  cols: number; rows: number;
  /** palette index per cell; 255 = the grey gap line, 0 = nothing */
  ring: Uint8Array;
  /** rows written so far (the newest is (seq - 1) % rows) */
  seq: number;
  key: string;
  floor: number;
  stamps: Array<{ seq: number; x: number; text: string }>;
  hiddenAt: number;
  /** ★★★ beta97 hotfix the span actually drawn (the model's, widened to MWF_MIN_BINS of the frame's bins) — the markers
   *  and the stamps are placed on it, so they line up with the picture */
  vlo?: number; vhi?: number;
}
export function mwfStoreNew(): MwfStore { return { cols: 0, rows: 0, ring: new Uint8Array(0), seq: 0, key: '', floor: NaN, stamps: [], hiddenAt: 0 }; }
export type MwfFrame = { bins: Float32Array; centreHz: number; spanHz: number };

const GAP = 255;
/** The grey gap line's index (the time the drawer was hidden). */
export const GAP_ROW = GAP;
export const LEVEL_IDX = 70, LEVEL_SPAN_DB = 40;
/** ★★★ beta97 hotfix (Stuart, TestFlight 589 on the Mac: "full-saturation rainbow speckle") THE PALETTE IS RGBA — getColorLUT
 *  gives FOUR bytes an entry; this read it three at a time, so every level picked bytes straddling two entries and a
 *  2-dB wobble jumped between unrelated colours. One stride, here, for every read. */
export const LUT_STRIDE = 4;

/** ★ The visible rows, newest at the top, as RGBA: each palette index its own colour (LUT_STRIDE), the grey gap line, and
 *  where the history has not reached yet BLACK (Stuart, TF 590: "it looks better being black until it starts to draw in");
 *  a cell with no data (index 0) black too. Pure: the drawer paints this, the fixture test checks it. */
export function mwfPaint(s: MwfStore, h: number, lut: Uint8Array): Uint8Array {
  const rgba = new Uint8Array(s.cols * h * 4);
  const n = Math.min(h, s.seq);
  for (let y = 0; y < n; y++) {
    const src = ((s.seq - 1 - y) % s.rows) * s.cols, dst = y * s.cols * 4;
    for (let c = 0; c < s.cols; c++) {
      const v = s.ring[src + c], p = dst + c * 4;
      if (v === GAP) { rgba[p] = 90; rgba[p + 1] = 90; rgba[p + 2] = 90; }
      else if (v !== 0) { const li = v * LUT_STRIDE; rgba[p] = lut[li]; rgba[p + 1] = lut[li + 1]; rgba[p + 2] = lut[li + 2]; }
      rgba[p + 3] = 255;
    }
  }
  for (let i = n * s.cols * 4 + 3; i < rgba.length; i += 4) rgba[i] = 255;   // black, opaque
  return rgba;
}

/** ★★★ THE DRAWER'S SPAN INSIDE THE CAPTURE (Stuart, TF 590: a black strip down the left edge — the span began below the
 *  frame's lowest bin): slid inside the frame when it fits, else the frame's whole width. Markers are placed on the result. */
export function mwfClampToCapture(v: { lo: number; hi: number }, centreHz: number, spanHz: number): { lo: number; hi: number } {
  if (!(spanHz > 0) || !(v.hi > v.lo)) return v;
  const f0 = centreHz - spanHz / 2, f1 = centreHz + spanHz / 2, w = v.hi - v.lo;
  if (w >= f1 - f0) return { lo: f0, hi: f1 };
  if (v.lo < f0) return { lo: f0, hi: f0 + w };
  if (v.hi > f1) return { lo: f1 - w, hi: f1 };
  return v;
}
/** One row into the ring: palette indices re-levelled (floor → LEVEL_IDX), or the grey gap line (null). */
export function pushRow(s: MwfStore, row: Float32Array | null) {
  if (!s.cols || !s.rows) return;
  const off = (s.seq % s.rows) * s.cols;
  if (!row) { s.ring.fill(GAP, off, off + s.cols); s.seq++; return; }
  // the slice's noise floor: its 25th percentile, eased so a burst does not pump the picture
  const vals: number[] = [];
  for (let c = 0; c < row.length; c += Math.max(1, Math.floor(row.length / 128))) if (Number.isFinite(row[c])) vals.push(row[c]);
  if (vals.length) {
    vals.sort((a, b) => a - b);
    const f = vals[Math.floor(vals.length * 0.25)];
    s.floor = Number.isFinite(s.floor) ? s.floor + (f - s.floor) * 0.15 : f;
  }
  const scale = (255 - LEVEL_IDX) / LEVEL_SPAN_DB;
  for (let c = 0; c < s.cols; c++) {
    const v = row[c];
    s.ring[off + c] = Number.isFinite(v) && Number.isFinite(s.floor) ? Math.max(1, Math.min(254, Math.round(LEVEL_IDX + (v - s.floor) * scale))) : 0;
  }
  s.seq++;
}
