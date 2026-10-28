/* wfWide.ts — THE FULL-CAPTURE LAYER under the web waterfall's history (V12, Stuart 2026-10-28 02:00, beta26, Lenovo RSP1A,
 * MW, zoomed in then out: "NO NO NO this isn't right — the waterfall should be buffering outside the zoomed area so that
 * the black doesn't show at all").
 *
 * The client only ever received the zoomed view, so a zoom out showed the rows recorded meanwhile as a narrow strip in a
 * black frame — a stepped pyramid. Stuart's spec: "a FULLY BUFFERED WATERFALL with a RING BUFFER … MINIMAL black gaps ONLY
 * when absolutely necessary due to the SDR catching up from a large move or rapid tuning".
 *
 * HOW: while this listener's view is narrower than the capture, the server follows every 8th spectrum frame with a coarse
 * row of the WHOLE capture (SPWB, 1024 bins, max-held — docs/PROTOCOL.md). Every ring row the renderer writes also gets a
 * row in a second, COARSE ring at the same index: the newest full-capture row, resampled onto the detailed row's OWN
 * u-space extended to [−X, 1 + X] (X = capture ÷ view, rounded up — so the whole capture fits whatever the view's place
 * in it). The shader reads the coarse ring at uc = (u + X) / (1 + 2X) through the SAME per-row map as the detailed ring,
 * so the zoom fan, the tune's bend and a resize move both layers together; it draws the detailed row where it has data
 * and the coarse one everywhere else, cross-fading over a few pixels at the detailed row's edge (no hard block).
 *   ★ X depends only on the zoom (capture ÷ view), never on where the view sits in the capture, so a tune at one zoom
 *     keeps every row's X the same and the runs of rows sharing a map stay as few as they were.
 *   ★ Black only where the radio was not capturing (outside the coarse row, or no SPWB yet — an older server).
 *   ★ The coarse row's noise floor is matched to the detailed row's (the wide FFT's bins are wider, so its floor sits
 *     higher): a slow-following offset from the 20th percentiles of the two over the part of the band both cover.
 * PURE — scripts/test_wf_wide.ts. */

/** Texels per coarse ring row. Covers (1 + 2X) view widths; with X = ceil(capture / view) that is ≥ the coarse row's own
 *  resolution (1024 bins over capture ÷ view widths) at every zoom. */
export const WIDE_TEX = 2048;
/** The SPWB header (see the server's buildWideFrame). */
export const WIDE_HDR = 26;
export const WIDE_MAGIC = 0x42575053;   // 'SPWB' little-endian

export interface WideRow { centreHz: number; spanHz: number; db: Float32Array }

/** Parse an SPWB frame into dB (NaN = no data — the 0 byte), or null if it is not one. `into` is reused when it fits. */
export function parseWide(buf: ArrayBuffer, offsetDb: number, into?: Float32Array | null): WideRow | null {
  if (buf.byteLength <= WIDE_HDR) return null;
  const dv = new DataView(buf);
  if (dv.getUint32(0, true) !== WIDE_MAGIC || dv.getUint8(4) !== 1) return null;
  const centreHz = Number(dv.getBigUint64(14, true)), spanHz = dv.getUint32(22, true);
  const n = buf.byteLength - WIDE_HDR;
  const u8 = new Uint8Array(buf, WIDE_HDR, n);
  const db = into && into.length === n ? into : new Float32Array(n);
  for (let i = 0; i < n; i++) db[i] = u8[i] === 0 ? NaN : u8[i] + offsetDb;
  return { centreHz, spanHz, db };
}

/** The extended range X for a row `spanHz` wide of a capture `capHz` wide: 0 = the row covers the capture (no layer). */
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

/** The coarse ring row for a detailed row placed at (rowLo, rowSpan) with extent X: texel t covers the detailed row's own
 *  u = −X + (t + ½)(1 + 2X)/W. Linear between coarse bins when a texel is finer than a bin; the MAX of the bins it covers
 *  when coarser (a one-bin carrier survives). `toIdx` maps dB to the palette index (the detailed row's own mapping);
 *  data is stored ≥ 1, 0 = no data. */
export function wideRowInto(dst: Uint8Array, w: WideRow, rowLo: number, rowSpan: number, X: number,
                            offDb: number, toIdx: (db: number) => number): void {
  const W = dst.length, n = w.db.length;
  if (!(X > 0) || !(rowSpan > 0) || !(w.spanHz > 0) || !n) { dst.fill(0); return; }
  const ext = 1 + 2 * X;
  const wLo = w.centreHz - w.spanHz / 2;
  // coarse bin coordinate (bin centres at integers) of texel t's centre: c = c0 + t·k
  const k = (ext * rowSpan / W) / (w.spanHz / n);
  const c0 = ((rowLo + (-X + 0.5 * ext / W) * rowSpan) - wLo) / w.spanHz * n - 0.5;
  const db = w.db;
  for (let t = 0; t < W; t++) {
    const c = c0 + t * k;
    let v: number;
    if (k <= 1) {   // a texel finer than a bin: linear. Coarser: the max of the bins whose CENTRES it covers (1-2, never the 2-3 it touches — that drew blocks twice a bin wide)
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
