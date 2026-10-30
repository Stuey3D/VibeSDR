/* wfWide.ts — THE FULL-CAPTURE LAYER under the web waterfall's history (V12, Stuart 2026-10-28 02:00, beta26, Lenovo RSP1A,
 * MW, zoomed in then out: "NO NO NO this isn't right — the waterfall should be buffering outside the zoomed area so that
 * the black doesn't show at all").
 *
 * The client only ever received the zoomed view, so a zoom out showed the rows recorded meanwhile as a narrow strip in a
 * black frame. Stuart's spec: "a FULLY BUFFERED WATERFALL with a RING BUFFER … MINIMAL black gaps ONLY when absolutely
 * necessary due to the SDR catching up from a large move or rapid tuning".
 *
 * HOW: while this listener's view is narrower than the capture, the server follows every 4th spectrum frame with a row of
 * the WHOLE capture — SPWB v2 (android/app/src/main/cpp/vibe_wide_row.h, docs/PROTOCOL.md): its FLOOR (128 medians) and
 * its PEAKS (each cell of 1024 whose strongest bin stands 6 dB out: level, position to 1/16 of a cell, width). Every ring
 * row the renderer writes gets the layer at the SAME ring row, in CAPTURE space; the row's own (k, m) — u_capture =
 * k·u + m, u the detailed row's own u — puts it under the detailed row through the SAME per-row map as the zoom fan, so
 * the fan, the tune's bend and a resize move both together.
 * ★★ v2, "the same waterfall, just less sharp" (Stuart's review of v1's max-held bins: big hot blobs, a hotter floor,
 *    vertical blocks): the floor is the floor (medians, matched to the detailed rows' own floor and drawn through their
 *    own dB → palette mapping), a thin carrier is a thin line at its frequency, a voice is the band it occupies, and the
 *    rows come every 4th frame with the floor and the peaks interpolated in time between them.
 *   ★ Black only where the radio was not capturing (outside the row, or no SPWB yet — an older server).
 * PURE — scripts/test_wf_wide.ts. */

export const WIDE_MAGIC = 0x42575053;   // 'SPWB' little-endian
export const WIDE_HDR = 32;
/** The 2D fallback's coarse twin width (texels across the row's extended range — see wideRowInto). */
export const WIDE_TEX = 2048;

/** One full-capture row, decoded: dB, NaN = no data. */
export interface WideRow {
  centreHz: number; spanHz: number;
  floor: Float32Array;              // F floor bins, low → high
  spread: Float32Array;             // the floor's σ per floor bin (dB) — the speckle the detailed rows show round it
  cells: number;                    // C — the peak grid across the capture
  pkCell: Int32Array; pkDb: Float32Array; pkShape: Uint8Array; nPk: number;
}
/** A dense dB row across the capture (the 2D fallback's source, and the tests'). */
export interface DenseRow { centreHz: number; spanHz: number; db: Float32Array }

/** Parse an SPWB v2 frame (dB = byte + offsetDb; a 0 byte is no data), or null if it is not one. */
export function parseWide(buf: ArrayBuffer, offsetDb: number): WideRow | null {
  if (buf.byteLength < WIDE_HDR) return null;
  const dv = new DataView(buf);
  if (dv.getUint32(0, true) !== WIDE_MAGIC || dv.getUint8(4) !== 2) return null;
  const F = dv.getUint16(26, true), C = dv.getUint16(28, true), P = dv.getUint16(30, true);
  if (!F || !C || buf.byteLength < WIDE_HDR + 2 * F + P * 3) return null;
  const u8 = new Uint8Array(buf);
  const floor = new Float32Array(F);
  for (let i = 0; i < F; i++) { const b = u8[WIDE_HDR + i]; floor[i] = b === 0 ? NaN : b + offsetDb; }
  const spread = new Float32Array(F);
  for (let i = 0; i < F; i++) spread[i] = u8[WIDE_HDR + F + i] / 10;
  const pkCell = new Int32Array(P), pkDb = new Float32Array(P), pkShape = new Uint8Array(P);
  let n = 0, cell = -1, o = WIDE_HDR + 2 * F;
  for (let i = 0; i < P; i++, o += 3) {
    const d = u8[o], lvl = u8[o + 1];
    cell += d;
    if (lvl === 0) continue;   // an escape: advance only
    if (cell >= 0 && cell < C) { pkCell[n] = cell; pkDb[n] = lvl + offsetDb; pkShape[n] = u8[o + 2]; n++; }
  }
  return { centreHz: Number(dv.getBigUint64(14, true)), spanHz: dv.getUint32(22, true), floor, spread, cells: C, pkCell, pkDb, pkShape, nPk: n };
}

/** The floor at capture fraction x (0..1) — linear between floor-bin centres, NaN where there is none. */
export function floorAt(floor: Float32Array, x: number): number {
  const F = floor.length, p = x * F - 0.5;
  let i = Math.floor(p), t = p - i;
  if (i < 0) { i = 0; t = 0; } if (i >= F - 1) { i = F - 2; t = 1; }
  if (F < 2) return floor[0];
  const a = floor[i], b = floor[i + 1];
  return a === a ? (b === b ? a + (b - a) * t : a) : b;
}

/** A peak's centre (capture fraction) and half-width (capture fraction) from its cell and shape byte. */
export function peakGeom(cell: number, shape: number, C: number): [number, number] {
  const off = shape & 15, w16 = (shape >> 4) + 1;
  return [(cell + (off + 0.5) / 16) / C, w16 / 32 / C];
}

/** Per-cell peak levels (dB, NaN = none) and shapes of a row — for the time interpolation and the GPU row. */
export function cellsOf(w: WideRow, db: Float32Array, shape: Uint8Array): void {
  db.fill(NaN);
  for (let i = 0; i < w.nPk; i++) { db[w.pkCell[i]] = w.pkDb[i]; shape[w.pkCell[i]] = w.pkShape[i]; }
}

/** The dense row: N bins across the capture — the floor, linear, with every peak drawn at its place and width (at least
 *  one bin). */
export function denseRow(floor: Float32Array, cellDb: Float32Array, cellShape: Uint8Array, out: Float32Array): void {
  const N = out.length, C = cellDb.length;
  for (let i = 0; i < N; i++) out[i] = floorAt(floor, (i + 0.5) / N);
  for (let c = 0; c < C; c++) {
    const v = cellDb[c];
    if (v !== v) continue;
    const [x0, h] = peakGeom(c, cellShape[c], C);
    const a = Math.max(0, Math.floor((x0 - h) * N)), b = Math.min(N - 1, Math.max(a, Math.ceil((x0 + h) * N) - 1));
    for (let i = a; i <= b; i++) if (!(out[i] >= v)) out[i] = v;
  }
}

/** ★★ The floor as the detailed rows LOOK: a detailed row's floor is a speckle of σ round the median, and through the
 *  palette's clip that speckle is most of what the eye sees — so the layer's floor colour is the speckle's AVERAGE
 *  index (the same field, blurred), E[idx(m + σZ)], by 5-point Gauss–Hermite. σ = 0 is idx(m). */
const GH_Z = [0, 1.3556262, -1.3556262, 2.8569700, -2.8569700], GH_W = [0.5333333, 0.2220759, 0.2220759, 0.0112574, 0.0112574];
export function floorIdx(m: number, sigma: number, toIdx: (db: number) => number): number {
  if (!(sigma > 0.05)) return toIdx(m);
  let s = 0;
  for (let k = 0; k < 5; k++) s += GH_W[k] * toIdx(m + sigma * GH_Z[k]);
  return Math.round(s);
}

/** ★★★ A row the server placed OUTSIDE its own capture gets no layer — the app's src/utils/wfWide.ts rowInCapture (the
 *  white bar, 2026-10-29: rows built at a stale zoom offset, auto-ranged on the IF filter's stopband, then filled from the
 *  real floor ~30 dB above that range when the view came back). */
export function rowInCapture(rowLo: number, rowSpan: number, capCentreHz: number, capSpanHz: number): boolean {
  return Math.abs(rowLo + rowSpan / 2 - capCentreHz) <= capSpanHz / 2;
}

/** The extended range X for a row `spanHz` wide of a capture `capHz` wide (the 2D twin): 0 = the row covers the capture. */
export function wideExtent(capHz: number, spanHz: number): number {
  if (!(capHz > 0 && spanHz > 0) || spanHz >= capHz * 0.98) return 0;
  return Math.min(4096, Math.ceil(capHz / spanHz));
}

/** The p-th percentile (0..1) of the finite values of a[from..to) — `scratch` reused. NaN if there are fewer than 8. */
export function percentile(a: ArrayLike<number>, from: number, to: number, p: number, scratch: { buf: Float32Array }): number {
  const lo = Math.max(0, Math.floor(from)), hi = Math.min(a.length, Math.ceil(to));
  if (scratch.buf.length < hi - lo) scratch.buf = new Float32Array(Math.max(64, hi - lo));
  let m = 0;
  for (let i = lo; i < hi; i++) { const v = a[i]; if (v === v) scratch.buf[m++] = v; }
  if (m < 8) return NaN;
  const s = scratch.buf.subarray(0, m).sort();
  return s[Math.min(m - 1, Math.floor(p * m))];
}

/** The 2D fallback's coarse twin for a detailed row placed at (rowLo, rowSpan) with extent X: texel t covers the row's
 *  own u = −X + (t + ½)(1 + 2X)/W. Linear between dense bins when a texel is finer than a bin; the max of the bins whose
 *  centres it covers when coarser. `toIdx` maps dB to the palette index; data is stored ≥ 1, 0 = no data. */
export function wideRowInto(dst: Uint8Array, w: DenseRow, rowLo: number, rowSpan: number, X: number,
                            offDb: number, toIdx: (db: number) => number): void {
  const W = dst.length, n = w.db.length;
  if (!(X > 0) || !(rowSpan > 0) || !(w.spanHz > 0) || !n) { dst.fill(0); return; }
  const ext = 1 + 2 * X;
  const wLo = w.centreHz - w.spanHz / 2;
  const k = (ext * rowSpan / W) / (w.spanHz / n);
  const c0 = ((rowLo + (-X + 0.5 * ext / W) * rowSpan) - wLo) / w.spanHz * n - 0.5;
  const db = w.db;
  for (let t = 0; t < W; t++) {
    const c = c0 + t * k;
    let v: number;
    if (k <= 1) {
      if (c < -0.5 || c > n - 0.5) { dst[t] = 0; continue; }
      const i = Math.floor(c), f = c - i;
      const a0 = db[i < 0 ? 0 : i], a1 = db[i + 1 >= n ? n - 1 : i + 1];
      v = a0 === a0 ? (a1 === a1 ? a0 + (a1 - a0) * f : a0) : a1;
    } else {
      const lo = Math.max(0, Math.ceil(c - k / 2)), hi = Math.min(n, Math.floor(c + k / 2) + 1);
      if (hi <= lo) { dst[t] = 0; continue; }
      v = NaN;
      for (let i = lo; i < hi; i++) { const d = db[i]; if (d === d && !(d <= v)) v = d; }
    }
    if (v !== v) { dst[t] = 0; continue; }
    const idx = toIdx(v + offDb);
    dst[t] = idx < 1 ? 1 : idx > 255 ? 255 : idx;
  }
}

/** Encode m (the capture fraction where the detailed row's u = 0 sits) for the GPU's per-row texture: 24 bits over
 *  [−2, 2) — 0.6 Hz across 2.4 MHz. The shader's rowM() decodes it. */
export function encodeM(m: number, out: Uint8Array, at: number): void {
  const v = Math.max(0, Math.min(16777215, Math.round((m + 2) / 4 * 16777215)));
  out[at] = (v >> 16) & 255; out[at + 1] = (v >> 8) & 255; out[at + 2] = v & 255; out[at + 3] = 255;
}
export function decodeM(b: Uint8Array, at: number): number {
  return ((b[at] << 16) | (b[at + 1] << 8) | b[at + 2]) / 16777215 * 4 - 2;
}
