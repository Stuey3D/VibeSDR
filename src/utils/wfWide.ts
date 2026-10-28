/* wfWide.ts — THE FULL-CAPTURE LAYER under the app's waterfall history (V12, docs/v12/WATERFALL-FULL-CAPTURE.md).
 *
 * ★★★ Stuart, 2026-10-28 02:00 (beta26 web, zoomed in then out): "the waterfall should be buffering outside the zoomed
 *     area so that the black doesn't show at all." His acceptance spec: "NO SLICING when zooming or tuning, MINIMAL black
 *     gaps ONLY when absolutely necessary due to the SDR catching up from a large move or rapid tuning, and the waterfall
 *     PRESERVED on orientation change and resizing. A FULLY BUFFERED WATERFALL with a RING BUFFER." And of v1: it must
 *     look like "the same waterfall, just less sharp" — the same floor, a thin carrier a thin line, no hot blobs.
 *
 * THE HOLE: the app's zoom fan (wfRowPlace.ts THE ZOOM FAN) re-maps every stored row to the new view, but a row recorded
 * while zoomed in IS only the zoomed view — so a zoom out (or a jump across the band) showed it as a narrow strip in black.
 *
 * HOW: a VibeServer that advertises `"wide":2` in hwinfo, asked with `&wide=1`, follows every 4th spectrum frame with an
 * SPWB v2 row of the WHOLE capture (android/app/src/main/cpp/vibe_wide_row.h, docs/PROTOCOL.md): its FLOOR (F medians +
 * their spread σ) and its PEAKS (≤ 110 cells of C whose strongest bin stands out: level, position to 1/16 of a cell,
 * width). Every ring row the app writes also gets a LAYER ROW at the same slot, in CAPTURE space — the floor as palette
 * indices and the peaks as (cell, index, shape) — with the row's own k = rowSpan/capSpan and m = (rowLo − capLo)/capSpan.
 * The fan's per-row map (u = a·x + b) then puts the layer under the detailed row through the SAME map (uc = k·u + m), so a
 * zoom, a jump, a resize and a rotation move both together. Where the fan regenerates a row (fanRegen) the texels outside
 * the detailed extent are filled from its layer row, with a soft edge where the detailed row's edge is ON SCREEN.
 *   ★ Black only where the radio was not capturing — outside the capture, or no SPWB yet (an older server: nothing is
 *     allocated, nothing changes).
 *   ★ The FLOOR goes through the detailed rows' OWN dB → index mapping (brightness, auto range, the 14.97 clip) with the
 *     measured offset `off`, and as the speckle's AVERAGE index E[idx(m + σZ)] (floorIdx) — a detailed floor is a speckle
 *     round its median, and through the clip that speckle is what the eye sees; the median alone draws it black.
 *   ★ A THIN CARRIER STAYS THIN: a peak lights only the texels within max(½ texel, its half-width) of its centre.
 *   ★ Interpolated in TIME between SPWB rows (they come every 4th frame): floor + σ linearly, a cell in both linearly, a
 *     cell in only one fades between it and the floor under it. Stale after 4 s.
 *
 * ★ COST (the trade, stated — feedback_state_efficiency_tradeoffs). RAM: a layer ring of F + 4·P bytes + 16 a slot
 *   (128 + 640 + 16 = 784 B × 1024 rows ≈ 0.8 MB), allocated at the FIRST SPWB only — sparse peaks, not the web's dense
 *   2·C a row (2.2 MB), for the 1 GB phones (android_potato_phone_users). CPU: per row written while a layer is live, one
 *   blend (F + C) and F × 5 + ≤ P index mappings; per row the fan regenerates, one pass over the texels outside the
 *   detailed extent + its peaks. GPU: NOTHING — the shader is unchanged (the XCover 4s is GPU-bound).
 * PURE — scripts/test_wf_wide_app.ts (+ the layer cases in test_wf_row_place.ts and the real shader in
 * test_wf_shader_motion.ts). The Android engine's twin is android/app/src/main/cpp/spectrum/wf_wide.h — change one, change
 * both. The web's reference is web/client/src/wfWide.ts. */

export const WIDE_MAGIC = 0x42575053;   // 'SPWB' little-endian
export const WIDE_HDR = 32;
export const WIDE_VERSION = 2;
/** A full-capture row older than this is not drawn (the band has moved on, or the server stopped sending). */
export const WIDE_STALE_MS = 4000;
/** Per ring row, the most peaks the layer keeps (110 a frame + the cells fading in or out between two frames). */
export const LAYER_PEAKS = 160;
/** A row covering ≥ this fraction of the capture has no layer (the server sends none then either). */
export const LAYER_FULL = 0.98;

/** One full-capture row, decoded: dB, NaN = no data. */
export interface WideRow {
  centreHz: number; spanHz: number;
  floor: Float32Array;              // F floor bins, low → high
  spread: Float32Array;             // the floor's σ per floor bin (dB)
  cells: number;                    // C — the peak grid across the capture
  pkCell: Int32Array; pkDb: Float32Array; pkShape: Uint8Array; nPk: number;
}

/** Is this binary frame an SPWB row (by its magic)? Checked BEFORE the SPEC parser, which counts any other magic a fault. */
export function isWideFrame(u8: Uint8Array): boolean {
  return u8.length >= 4 && u8[0] === 0x53 && u8[1] === 0x50 && u8[2] === 0x57 && u8[3] === 0x42;
}

/** Parse an SPWB v2 frame (dB = byte + offsetDb; a 0 byte is no data), or null if it is not one (v1 was never released). */
export function parseWide(src: ArrayBuffer | Uint8Array, offsetDb = -256): WideRow | null {
  const u8 = src instanceof Uint8Array ? src : new Uint8Array(src);
  if (u8.length < WIDE_HDR || !isWideFrame(u8) || u8[4] !== WIDE_VERSION) return null;
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const F = dv.getUint16(26, true), C = dv.getUint16(28, true), P = dv.getUint16(30, true);
  if (!F || !C || u8.length < WIDE_HDR + 2 * F + P * 3) return null;
  const floor = new Float32Array(F);
  for (let i = 0; i < F; i++) { const b = u8[WIDE_HDR + i]; floor[i] = b === 0 ? NaN : b + offsetDb; }
  const spread = new Float32Array(F);
  for (let i = 0; i < F; i++) spread[i] = u8[WIDE_HDR + F + i] / 10;
  const pkCell = new Int32Array(P), pkDb = new Float32Array(P), pkShape = new Uint8Array(P);
  let n = 0, cell = -1, o = WIDE_HDR + 2 * F;
  for (let i = 0; i < P; i++, o += 3) {
    const lvl = u8[o + 1];
    cell += u8[o];
    if (lvl === 0) continue;   // an escape: advance only
    if (cell >= 0 && cell < C) { pkCell[n] = cell; pkDb[n] = lvl + offsetDb; pkShape[n] = u8[o + 2]; n++; }
  }
  // u64 centre: two u32 halves (no BigInt — Hermes has it, but there is no need)
  const centreHz = dv.getUint32(14, true) + dv.getUint32(18, true) * 4294967296;
  return { centreHz, spanHz: dv.getUint32(22, true), floor, spread, cells: C, pkCell, pkDb, pkShape, nPk: n };
}

/** The floor at capture fraction x (0..1) — linear between floor-bin centres, NaN where there is none. */
export function floorAt(floor: Float32Array, x: number): number {
  const F = floor.length;
  if (F < 2) return floor[0];
  const p = x * F - 0.5;
  let i = Math.floor(p), t = p - i;
  if (i < 0) { i = 0; t = 0; } if (i >= F - 1) { i = F - 2; t = 1; }
  const a = floor[i], b = floor[i + 1];
  return a === a ? (b === b ? a + (b - a) * t : a) : b;
}

/** A peak's centre and half-width (capture fractions) from its cell and shape byte. */
export function peakGeom(cell: number, shape: number, C: number): [number, number] {
  const off = shape & 15, w16 = (shape >> 4) + 1;
  return [(cell + (off + 0.5) / 16) / C, w16 / 32 / C];
}

/** ★★ The floor as the detailed rows LOOK: E[idx(m + σZ)] by 5-point Gauss–Hermite (σ = 0 is idx(m)). */
const GH_Z = [0, 1.3556262, -1.3556262, 2.8569700, -2.8569700], GH_W = [0.5333333, 0.2220759, 0.2220759, 0.0112574, 0.0112574];
export function floorIdx(m: number, sigma: number, toIdx: (db: number) => number): number {
  if (!(sigma > 0.05)) return toIdx(m);
  let s = 0;
  for (let k = 0; k < 5; k++) s += GH_W[k] * toIdx(m + sigma * GH_Z[k]);
  return Math.round(s);
}

/** The detailed rows' own dB → index mapping (signalProcessor.ts step 7: brightness, the auto range, the 14.97 clip),
 *  with the layer's floor offset added. */
export function makeToIdx(dbMin: number, dbMax: number, brightDb: number, offDb: number): (db: number) => number {
  const range = dbMax - dbMin > 0 ? dbMax - dbMin : 1, CLIP = 14.97;
  return (db: number) => {
    const nrm = Math.max(0, Math.min(1, (db + offDb + brightDb - dbMin) / range));
    const mag = nrm * 255;
    return Math.round(mag < CLIP ? 0 : (mag - CLIP) / (255 - CLIP) * 255);
  };
}

/** The median of the finite values of a[0..n) — O(n) quickselect on `scratch` (reused). NaN if fewer than 8. */
export function medianOf(a: ArrayLike<number>, scratch: { buf: Float32Array }): number {
  const N = a.length;
  if (scratch.buf.length < N) scratch.buf = new Float32Array(Math.max(64, N));
  const s = scratch.buf;
  let m = 0;
  for (let i = 0; i < N; i++) { const v = a[i]; if (v === v) s[m++] = v; }
  if (m < 8) return NaN;
  const k = m >> 1;
  let lo = 0, hi = m - 1;
  while (lo < hi) {
    const p = s[(lo + hi) >> 1];
    let i = lo, j = hi;
    while (i <= j) {
      while (s[i] < p) i++;
      while (s[j] > p) j--;
      if (i <= j) { const t = s[i]; s[i] = s[j]; s[j] = t; i++; j--; }
    }
    if (k <= j) hi = j; else if (k >= i) lo = i; else break;
  }
  return s[k];
}

/* ── THE NEWEST PICTURE OF THE CAPTURE, and its time blend (the web's pushWide / _blendLayer) ───────────────────────── */

export interface WideState {
  row: WideRow | null;
  at: number; gap: number;
  F: number; C: number;
  wFloor: Float32Array; wSpread: Float32Array; wCell: Float32Array; wShape: Uint8Array;   // newest (per cell, NaN = none)
  pFloor: Float32Array; pSpread: Float32Array; pCell: Float32Array; pShape: Uint8Array;   // previous (blend from)
  hasPrev: boolean;
  bFloor: Float32Array; bSpread: Float32Array; bCell: Float32Array; bShape: Uint8Array;   // as drawn now
  /** The floor offset (dB) that puts the layer's floor on the detailed rows' own: EMA 0.3, clamped ±30. */
  off: number; offSet: boolean;
}
export function newWideState(): WideState {
  const z = new Float32Array(0), u = new Uint8Array(0);
  return { row: null, at: 0, gap: 400, F: 0, C: 0, wFloor: z, wSpread: z, wCell: z, wShape: u, pFloor: z, pSpread: z, pCell: z,
           pShape: u, hasPrev: false, bFloor: z, bSpread: z, bCell: z, bShape: u, off: 0, offSet: false };
}

/** A full-capture row arrived at `now` (ms). `viewLo/viewSpan` = the latest detailed row's placement (Hz), `detP50` = the
 *  MEDIAN of its dB — for the floor match. */
export function wideArrive(st: WideState, row: WideRow, now: number, viewLo: number, viewSpan: number, detP50: number): void {
  const F = row.floor.length, C = row.cells, old = st.row;
  const same = !!old && old.centreHz === row.centreHz && old.spanHz === row.spanHz && st.F === F && st.C === C;
  if (st.F !== F) {
    st.F = F;
    st.wFloor = new Float32Array(F); st.wSpread = new Float32Array(F); st.pFloor = new Float32Array(F);
    st.pSpread = new Float32Array(F); st.bFloor = new Float32Array(F); st.bSpread = new Float32Array(F);
  }
  if (st.C !== C) {
    st.C = C;
    st.wCell = new Float32Array(C); st.wShape = new Uint8Array(C); st.pCell = new Float32Array(C);
    st.pShape = new Uint8Array(C); st.bCell = new Float32Array(C); st.bShape = new Uint8Array(C);
  }
  if (same) {
    // ★ from what is drawn NOW (the blend part-way), so a row that comes early does not jump
    const mid = st.hasPrev;
    st.pFloor.set(mid ? st.bFloor : st.wFloor); st.pSpread.set(mid ? st.bSpread : st.wSpread);
    st.pCell.set(mid ? st.bCell : st.wCell); st.pShape.set(mid ? st.bShape : st.wShape);
    st.hasPrev = true;
    const gap = now - st.at;
    if (gap > 0 && gap < 5000) st.gap += (gap - st.gap) * 0.3;
  } else st.hasPrev = false;
  st.wFloor.set(row.floor); st.wSpread.set(row.spread);
  st.wCell.fill(NaN);
  for (let i = 0; i < row.nPk; i++) { st.wCell[row.pkCell[i]] = row.pkDb[i]; st.wShape[row.pkCell[i]] = row.pkShape[i]; }
  st.row = row; st.at = now;
  // ★★ THE FLOOR MATCH: the layer's floor against the detailed rows' own (their median) over the view — so a row reads as
  //    ONE row across the detailed strip's edge. Followed slowly so the outside does not flicker.
  if (viewSpan > 0 && detP50 === detP50) {
    const wLo = row.centreHz - row.spanHz / 2;
    let sum = 0, n = 0;
    for (let i = 0; i < 16; i++) {
      const f = floorAt(row.floor, (viewLo + (i + 0.5) / 16 * viewSpan - wLo) / row.spanHz);
      if (f === f) { sum += f; n++; }
    }
    if (n >= 8) {
      const off = Math.max(-30, Math.min(30, detP50 - sum / n));
      st.off = st.offSet ? st.off + (off - st.off) * 0.3 : off;
      st.offSet = true;
    }
  }
}

/** The layer as drawn for a row written at `now`: blended from the previous row to the newest. False = nothing fresh. */
export function wideBlend(st: WideState, now: number): boolean {
  if (!st.row || now - st.at > WIDE_STALE_MS) return false;
  const F = st.F, C = st.C;
  const t = st.hasPrev ? Math.max(0, Math.min(1, (now - st.at) / Math.max(50, st.gap))) : 1;
  if (t >= 1) { st.bFloor.set(st.wFloor); st.bSpread.set(st.wSpread); st.bCell.set(st.wCell); st.bShape.set(st.wShape); return true; }
  for (let i = 0; i < F; i++) {
    const a = st.pFloor[i], b = st.wFloor[i];
    st.bFloor[i] = a === a && b === b ? a + (b - a) * t : b;
    st.bSpread[i] = st.pSpread[i] + (st.wSpread[i] - st.pSpread[i]) * t;
  }
  for (let c = 0; c < C; c++) {
    const p = st.pCell[c], q = st.wCell[c];
    if (p !== p && q !== q) { st.bCell[c] = NaN; continue; }
    const fl = floorAt(st.bFloor, (c + 0.5) / C);
    if (p === p && q === q) { st.bCell[c] = p + (q - p) * t; st.bShape[c] = st.wShape[c]; }
    else if (q === q) { st.bCell[c] = fl + Math.max(0, q - fl) * t; st.bShape[c] = st.wShape[c]; }
    else { st.bCell[c] = fl + Math.max(0, p - fl) * (1 - t); st.bShape[c] = st.pShape[c]; }
  }
  return true;
}

/** The band the newest fresh full-capture row covers (Hz), or null. */
export function wideCapture(st: WideState | null, now: number): { lo: number; hi: number } | null {
  const w = st?.row;
  if (!w || now - st!.at > WIDE_STALE_MS) return null;
  return { lo: w.centreHz - w.spanHz / 2, hi: w.centreHz + w.spanHz / 2 };
}

/* ── THE LAYER RING: one layer row per ring slot, in capture space ─────────────────────────────────────────────────── */

export interface LayerRing {
  rows: number; F: number; C: number; P: number;
  /** Per slot: k = rowSpan/capSpan (0 = no layer), m = (rowLo − capLo)/capSpan. */
  k: Float64Array; m: Float64Array;
  floor: Uint8Array;                          // rows × F: floor palette indices, 0 = no data
  pkN: Uint16Array;                           // rows: peaks kept
  pkCell: Uint16Array; pkIdx: Uint8Array; pkShape: Uint8Array;   // rows × P, by cell
  /** The soft edge where a detailed row's edge is on screen, in texels (~12 device px; WaterfallView sets it). */
  fadeTx: number;
  lay: Int16Array;                            // fill scratch (n)
  tmpC: Int32Array; tmpI: Uint8Array;         // write scratch (C)
}
export function newLayerRing(rows: number, F: number, C: number, P = LAYER_PEAKS): LayerRing {
  return { rows, F, C, P, k: new Float64Array(rows), m: new Float64Array(rows), floor: new Uint8Array(rows * F),
           pkN: new Uint16Array(rows), pkCell: new Uint16Array(rows * P), pkIdx: new Uint8Array(rows * P), pkShape: new Uint8Array(rows * P),
           fadeTx: 6, lay: new Int16Array(0), tmpC: new Int32Array(C), tmpI: new Uint8Array(C) };
}

/** The layer row for the ring row just written at `slot`, placed at (rowLo, rowSpan) Hz: the blended picture at `now`
 *  through `toIdx` (dB → index, the detailed rows' mapping incl. the floor offset). Returns k (0 = no layer for it). */
export function layerWriteRow(L: LayerRing, slot: number, st: WideState | null, rowLo: number, rowSpan: number, now: number,
                              toIdx: (db: number) => number): number {
  L.k[slot] = 0; L.pkN[slot] = 0;
  const w = st?.row;
  if (!st || !w || !(rowSpan > 0) || !isFinite(rowLo) || rowSpan >= w.spanHz * LAYER_FULL || st.F !== L.F || st.C !== L.C
      || !wideBlend(st, now)) return 0;
  const F = L.F, C = L.C, P = L.P, fo = slot * F;
  for (let i = 0; i < F; i++) { const v = st.bFloor[i]; L.floor[fo + i] = v === v ? Math.max(1, Math.min(255, floorIdx(v, st.bSpread[i], toIdx))) : 0; }
  let n = 0;
  for (let c = 0; c < C; c++) {
    const v = st.bCell[c];
    if (v !== v) continue;
    const ix = Math.min(255, toIdx(v));
    if (ix < 1) continue;
    L.tmpC[n] = c; L.tmpI[n] = ix; n++;
  }
  if (n > P) {   // keep the strongest P (rare: 110 a frame, more only while two frames' cells cross-fade)
    const order = Array.from({ length: n }, (_, i) => i).sort((a, b) => L.tmpI[b] - L.tmpI[a]).slice(0, P).sort((a, b) => a - b);
    const cs = order.map((i) => L.tmpC[i]), is = order.map((i) => L.tmpI[i]);
    for (let i = 0; i < P; i++) { L.tmpC[i] = cs[i]; L.tmpI[i] = is[i]; }
    n = P;
  }
  const po = slot * P;
  for (let i = 0; i < n; i++) { L.pkCell[po + i] = L.tmpC[i]; L.pkIdx[po + i] = L.tmpI[i]; L.pkShape[po + i] = st.bShape[L.tmpC[i]]; }
  L.pkN[slot] = n;
  L.k[slot] = rowSpan / w.spanHz;
  L.m[slot] = (rowLo - (w.centreHz - w.spanHz / 2)) / w.spanHz;
  return L.k[slot];
}

/** The layer's palette index at capture fraction uc of slot's row (floor only), or −1 (outside the capture / no data). */
function layerFloorAt(L: LayerRing, fo: number, uc: number): number {
  if (uc < 0 || uc > 1) return -1;
  const F = L.F, fl = L.floor;
  if (F < 2) return fl[fo] ? fl[fo] : -1;
  const p = uc * F - 0.5;
  let i = Math.floor(p), t = p - i;
  if (i < 0) { i = 0; t = 0; } if (i >= F - 1) { i = F - 2; t = 1; }
  const a = fl[fo + i], b = fl[fo + i + 1];
  if (!a && !b) return -1;
  if (!a) return b; if (!b) return a;
  return Math.round(a + (b - a) * t);
}

/** ★★ FILL a displayed row the fan just made: slot `slot`'s row drawn under map (a, b) into disp[at .. at+n). `lo[x] < 0`
 *  marks the texels the detailed row has no data for (fanTable's table for this map) — those take the layer; where the
 *  detailed row's edge is ON SCREEN the two cross-fade over L.fadeTx texels (off-screen edges never fade). The layer is
 *  read at uc = k·u + m, u = a·(x + ½)/n + b: the floor linear between its bins, raised by every peak within
 *  max(½ texel, its half-width) of its centre — a thin carrier one texel wide at any zoom. */
export function layerFillRow(L: LayerRing, slot: number, a: number, b: number, n: number, disp: Uint8Array, at: number,
                             lo: Int32Array): void {
  const k = L.k[slot];
  if (!(k > 0) || !(a > 0)) return;
  if (L.lay.length < n) L.lay = new Int16Array(n);
  const lay = L.lay, m = L.m[slot], fo = slot * L.F;
  const A = k * a / n, B = k * (a * 0.5 / n + b) + m;   // uc(x) = A·x + B
  const XL = -b / a * n, XR = (1 - b) / a * n, fade = L.fadeTx;
  const fadeL = XL > 0 && fade > 0, fadeR = XR < n && fade > 0;
  let any = false;
  for (let x = 0; x < n; x++) {
    let need = lo[x] < 0;
    if (!need && (fadeL || fadeR)) { const c = x + 0.5; need = (fadeL && c - XL < fade) || (fadeR && XR - c < fade); }
    if (!need) { lay[x] = -2; continue; }
    lay[x] = layerFloorAt(L, fo, A * x + B);
    any = true;
  }
  if (!any) return;
  const C = L.C, po = slot * L.P, np = L.pkN[slot], half = 0.5 * A;
  for (let i = 0; i < np; i++) {
    const cell = L.pkCell[po + i], sh = L.pkShape[po + i], ix = L.pkIdx[po + i];
    const c0 = (cell + ((sh & 15) + 0.5) / 16) / C, hw = Math.max(half, ((sh >> 4) + 1) / 32 / C);
    const x0 = Math.max(0, Math.ceil((c0 - hw - B) / A - 1e-9)), x1 = Math.min(n - 1, Math.floor((c0 + hw - B) / A + 1e-9));
    for (let x = x0; x <= x1; x++) if (lay[x] >= 0 && ix > lay[x]) lay[x] = ix;
  }
  for (let x = 0; x < n; x++) {
    const v = lay[x];
    if (v < 0) continue;
    if (lo[x] < 0) { disp[at + x] = v; continue; }
    const c = x + 0.5;
    let e = 1;
    if (fadeL) e = Math.min(e, (c - XL) / fade);
    if (fadeR) e = Math.min(e, (XR - c) / fade);
    if (e < 0) e = 0;
    if (e >= 1) continue;
    const d = disp[at + x];
    disp[at + x] = Math.round(v + (d - v) * e);
  }
}
