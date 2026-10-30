/**
 * THE APP'S FULL-CAPTURE LAYER (src/utils/wfWide.ts) — V12, docs/v12/WATERFALL-FULL-CAPTURE.md; the web's twin is
 * scripts/test_wf_wide.ts (web/client/src/wfWide.ts), the Android engine's is cpp/spectrum/test_spectrum_engine.cpp.
 * Proves on the pure parts:
 *   - an SPWB v2 frame parses: centre (u64, past 4 GHz), span, the floor (0 byte = no data), its spread, the peaks
 *     delta-coded across an escape; v1 and a SPEC frame are refused (isWideFrame tells them apart BEFORE the SPEC parser);
 *   - the floor's colour is its speckle's average (floorIdx); the median of a row (quickselect) matches a sort;
 *   - the time blend: halfway between two rows; a cell in only one fades from the floor; another centre = no blend; stale
 *     after 4 s; the floor offset puts the layer's floor on the detailed rows' median;
 *   - a layer row: k and m from the row's placement; none when the row covers the capture;
 *   - the fill: a ONE-cell carrier is ONE texel at its true column, magnified 12× or shrunk; a wide peak as wide as its
 *     shape; black beyond the capture; a soft edge only where the detailed row's edge is on screen.
 * Run: node --no-warnings scripts/test_wf_wide_app.ts
 */
import { floorIdx, isWideFrame, layerFillRow, layerWriteRow, makeToIdx, medianOf, newLayerRing, newWideState, parseWide, rowInCapture,
         wideArrive, wideBlend, wideCapture, WIDE_HDR, WIDE_STALE_MS } from '../src/utils/wfWide.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string, detail = '') => { c ? pass++ : fail++; if (!c) console.log(`   FAIL ${what}${detail ? ` — ${detail}` : ''}`); };
const CAP = 2_400_000, CEN = 1_500_000, LO = CEN - CAP / 2, C = 1024, F = 128;

/** Build a v2 frame: floor bytes, spread bytes, peaks [cell, levelByte, shape]. */
export function spwb(floorB: number[], peaks: [number, number, number][], centre = CEN, span = CAP, ver = 2, spreadB: number[] = floorB.map(() => 28)): Uint8Array {
  const trip: number[] = []; let prev = -1;
  for (const [c, l, s] of peaks) { let d = c - prev; while (d > 255) { trip.push(255, 0, 0); d -= 255; } trip.push(d, l, s); prev = c; }
  const u = new Uint8Array(WIDE_HDR + 2 * floorB.length + trip.length), dv = new DataView(u.buffer);
  u.set([0x53, 0x50, 0x57, 0x42, ver, 0]);
  dv.setUint32(14, centre % 4294967296, true); dv.setUint32(18, Math.floor(centre / 4294967296), true); dv.setUint32(22, span, true);
  dv.setUint16(26, floorB.length, true); dv.setUint16(28, C, true); dv.setUint16(30, trip.length / 3, true);
  u.set(floorB, WIDE_HDR); u.set(spreadB, WIDE_HDR + floorB.length); u.set(trip, WIDE_HDR + 2 * floorB.length); return u;
}
const cellOf = (f: number) => Math.floor((f - LO) / CAP * C);
const floorB = Array.from({ length: F }, (_, i) => i === 0 ? 0 : 146);   // −110 dBFS, the first bin no data
/** The shape byte's offset nibble for a carrier at f (where in its cell it is, to 1/16), as the server encodes it. */
const offOf = (f: number) => Math.min(15, Math.floor(((f - LO) / CAP * C - cellOf(f)) * 16));
const peaks: [number, number, number][] = [[cellOf(1_050_000), 200, offOf(1_050_000)], [cellOf(1_400_000), 200, offOf(1_400_000)],
  [cellOf(1_850_000), 200, offOf(1_850_000)], [cellOf(2_100_000), 180, (11 << 4) | offOf(2_100_000)]];

// ── parse ──
const w = parseWide(spwb(floorB, peaks))!;
ok(!!w && w.centreHz === CEN && w.spanHz === CAP && w.floor.length === F && w.floor[1] === -110 && Number.isNaN(w.floor[0]) && w.cells === C,
   'SPWB v2 parses: centre, span, the floor (dB = byte − 256), a 0 byte is no data');
ok(w.nPk === 4 && w.pkCell[3] === cellOf(2_100_000) && w.pkDb[0] === -56 && w.pkShape[3] === ((11 << 4) | offOf(2_100_000)), 'the peaks, delta-coded across an escape');
ok(Math.abs(w.spread[5] - 2.8) < 1e-6, 'the floor\'s spread parses (σ in 0.1 dB)');
ok(parseWide(spwb(floorB, peaks, 10_368_000_000))!.centreHz === 10_368_000_000, 'a centre past 4 GHz (u64) parses');
ok(parseWide(spwb(floorB, peaks, CEN, CAP, 1)) === null, 'a v1 frame is refused');
{ const b = spwb(floorB, peaks); b[3] = 0x43; ok(!isWideFrame(b) && parseWide(b) === null, 'a SPEC frame is not mistaken for one'); }
ok(isWideFrame(spwb(floorB, peaks)) && parseWide(spwb(floorB, peaks).subarray(0, 40)) === null, 'isWideFrame by magic; a truncated frame is refused');

// ── the mapping, the floor's colour, the median ──
const toIdx0 = makeToIdx(-120, -70, 0, 0);
ok(toIdx0(-120) === 0 && toIdx0(-70) === 255 && toIdx0(-118) === 0, 'the dB → index mapping clips the floor (signalProcessor step 7)');
{ const t = makeToIdx(-112, -82, 0, 0);
  ok(t(-111) === 0 && floorIdx(-111, 3, t) > 0 && floorIdx(-111, 0, t) === 0, `the floor's colour is its speckle's average (E[idx] ${floorIdx(-111, 3, t)} where idx(m) = 0)`); }
{ let seed = 3; const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
  const a = new Float32Array(4097); for (let i = 0; i < a.length; i++) a[i] = -110 + 20 * rnd(); a[7] = NaN;
  const s = [...a].filter((v) => v === v).sort((x, y) => x - y);
  ok(medianOf(a, { buf: new Float32Array(1) }) === s[s.length >> 1], 'the median (quickselect) is the sort\'s');
  ok(Number.isNaN(medianOf(new Float32Array(5), { buf: new Float32Array(1) })), 'the median needs 8 values'); }

// ── the time blend and the floor match ──
{
  const st = newWideState();
  wideArrive(st, parseWide(spwb(floorB, [[100, 160, 7]]))!, 1000, 1_000_000, 100_000, -104);
  ok(st.offSet && Math.abs(st.off - 6) < 1e-6, `the floor offset puts the layer's floor on the detailed median (+${st.off} dB)`);
  wideArrive(st, parseWide(spwb(floorB.map((b) => b ? b + 10 : 0), [[100, 180, 7], [300, 170, 7]]))!, 1400, 1_000_000, 100_000, -104);
  ok(wideBlend(st, 1600) && Math.abs(st.bFloor[5] - (-105)) < 0.6 && Math.abs(st.bCell[100] - (-86)) < 0.6,
     `halfway between two rows: floor ${st.bFloor[5].toFixed(1)} (−110 → −100), a cell in both ${st.bCell[100].toFixed(1)} (−96 → −76)`);
  ok(st.bCell[300] > st.bFloor[5] && st.bCell[300] < -86, `a cell in only the new row fades in from the floor (${st.bCell[300].toFixed(1)})`);
  ok(wideBlend(st, 2000) && st.bCell[300] === -86, 'at the end of the interval the newest row as sent');
  ok(!wideBlend(st, 1400 + WIDE_STALE_MS + 1) && wideCapture(st, 1400 + WIDE_STALE_MS + 1) === null, 'stale after 4 s: no layer, no capture window');
  wideArrive(st, parseWide(spwb(floorB, [], CEN + 50_000))!, 2100, 1_000_000, 100_000, -104);
  ok(wideBlend(st, 2150) && st.bFloor[5] === -110 && Number.isNaN(st.bCell[100]), 'another centre (a retune): no blend, the new row at once');
  const cw = wideCapture(st, 2150)!; ok(cw.lo === LO + 50_000 && cw.hi === LO + 50_000 + CAP, 'the capture window is the newest row\'s extent');
}

// ── a layer row and its fill ──
{
  const st = newWideState();
  wideArrive(st, parseWide(spwb(floorB, peaks))!, 0, 1_350_000, 100_000, -110);
  const L = newLayerRing(4, F, C);
  const toIdx = makeToIdx(-125, -50, 0, st.off);
  const k = layerWriteRow(L, 1, st, 1_350_000, 100_000, 10, toIdx);
  ok(Math.abs(k - 100_000 / CAP) < 1e-12 && Math.abs(L.m[1] - (1_350_000 - LO) / CAP) < 1e-12 && L.pkN[1] === 4, `a layer row: k = ${k.toFixed(4)}, m, 4 peaks`);
  ok(layerWriteRow(L, 2, st, LO, CAP, 10, toIdx) === 0 && L.pkN[2] === 0, 'a row covering the capture has no layer');
  const N = 1024, disp = new Uint8Array(4 * N), lo = new Int32Array(N);
  // the row zoomed OUT 12× about its own centre (a = 12, b = −5.5): the detailed strip is the middle 1/12 of the screen
  const fill = (a: number, b: number) => {
    disp.fill(0);
    for (let x = 0; x < N; x++) { const c = Math.floor(a * (x + 0.5) + b * N); lo[x] = c >= 0 && c < N ? c : -1; if (lo[x] >= 0) disp[N + x] = 99; }
    layerFillRow(L, 1, a, b, N, disp, N, lo);
  };
  const colOfHz = (a: number, b: number, f: number) => ((f - 1_350_000) / 100_000 - b) / a * N;   // the screen texel a frequency lands at
  fill(12, -5.5);
  const lit = (f: number, a: number, b: number) => { const c = colOfHz(a, b, f); const r: number[] = []; for (let x = Math.floor(c) - 6; x <= Math.ceil(c) + 6; x++) if (disp[N + x] > 150) r.push(x); return [c, r] as const; };
  { const [c, r] = lit(1_050_000, 12, -5.5);
    ok(r.length === 1 && Math.abs(r[0] + 0.5 - c) <= 1, `zoomed out 12×: a one-cell carrier is ONE texel at its true column (${r.join(',')} for ${c.toFixed(1)})`); }
  { const [c, r] = lit(1_850_000, 12, -5.5); ok(r.length === 1 && Math.abs(r[0] + 0.5 - c) <= 1, 'and the one on the other side of the strip'); }
  { const fl = disp[N + 100]; ok(fl > 0 && fl === floorIdx(-110, 2.8, toIdx), `the floor between signals is the floor's colour (${fl}), not black`); }
  ok(disp[N + 512] === 99, 'inside the detailed strip the detailed row is kept (its edges are faded only in the edge zone)');
  { let edgeMixed = 0; const XL = 5.5 / 12 * N; for (let x = Math.ceil(XL); x < XL + L.fadeTx - 1; x++) if (disp[N + x] !== 99) edgeMixed++;
    ok(edgeMixed > 0, `the on-screen edge cross-fades over ${L.fadeTx} texels`); }
  // magnified: a row zoomed IN 2× where its own data covers the screen — nothing is touched (no edges on screen)
  fill(0.5, 0.25);
  { let changed = 0; for (let x = 0; x < N; x++) if (disp[N + x] !== 99) changed++; ok(changed === 0, 'a row covering the screen: the layer touches nothing (off-screen edges never fade)'); }
  // panned far: the row's own data off screen to the right; the screen shows 1.20–1.30 MHz of the capture
  fill(1, -1.5);
  { const [c, r] = lit(1_249_000 + 0, 1, -1.5); void c; void r;
    let zeros = 0; for (let x = 0; x < N; x++) if (disp[N + x] === 0) zeros++; ok(zeros === 0, `a jump inside the capture: no black (${zeros})`); }
  // beyond the capture: black
  fill(60, -30);
  { let z = 0, nz = 0; for (let x = 0; x < N; x++) { const f = 1_350_000 + (60 * (x + 0.5) / N - 30) * 100_000; if (f < LO - 5_000 || f > LO + CAP + 5_000) { if (disp[N + x]) nz++; else z++; } }
    ok(z > 50 && nz === 0, `beyond the capture stays black (${z} texels; ${nz} drawn)`); }
  // a wide peak (12/16 of a cell) magnified (a 100 kHz screen: 24 texels a cell) is as wide as it says (~18 texels)
  fill(1, 7.0);
  { const c = colOfHz(1, 7.0, 2_100_000), per = CAP / C / (100_000 / N); let n = 0;
    for (let x = Math.floor(c) - 40; x <= Math.ceil(c) + 40; x++) if (x >= 0 && x < N && disp[N + x] > 140) n++;
    ok(Math.abs(n - 0.75 * per) <= 2, `a wide peak is as wide as its shape: ${n} texels (${(0.75 * per).toFixed(1)} expected)`); }
  // cost
  { const RR = 1024, L2 = newLayerRing(RR, F, C), t = makeToIdx(-125, -50, 0, 0); const many: [number, number, number][] = [];
    for (let i = 0; i < 110; i++) many.push([i * 9 + 3, 190, 7]);
    const s2 = newWideState(); wideArrive(s2, parseWide(spwb(floorB, many))!, 0, 1_350_000, 100_000, -110);
    let t0 = performance.now(); for (let r = 0; r < RR; r++) layerWriteRow(L2, r, s2, 1_350_000, 100_000, 10, t);
    const tw = (performance.now() - t0) / RR;
    const d2 = new Uint8Array(RR * N), lo2 = new Int32Array(N); for (let x = 0; x < N; x++) { const c = Math.floor(12 * (x + 0.5) - 5.5 * N); lo2[x] = c >= 0 && c < N ? c : -1; }
    t0 = performance.now(); for (let r = 0; r < RR; r++) layerFillRow(L2, r, 12, -5.5, N, d2, r * N, lo2);
    const tf = (performance.now() - t0) / RR;
    console.log(`   layer cost (this JS engine): ${(tw * 1000).toFixed(1)} µs a row written, ${(tf * 1000).toFixed(1)} µs a row filled (12× zoom out); RAM ${((F + 4 * L2.P + 16) * RR / 1024).toFixed(0)} KiB for ${RR} rows`); }
}

// ── ★★★ THE WHITE BAR (2026-10-29, RSP1A on the Lenovo): a row the server built OUTSIDE its capture gets no layer ──
{
  // The capture is 7.115 MHz ± 1.5 MHz (the tuner after the recentre). The zoom channel was left at +1678 kHz, so the rows
  // claimed ~8.79 MHz: the IF filter's stopband, auto-ranged ~30 dB down (−145..−115). The 40 m floor in the layer is −110.
  const cen = 7_115_000, span = 3_000_000, rowSpan = 74_409;
  const st = newWideState();
  wideArrive(st, parseWide(spwb(floorB, [], cen, span))!, 0, 8_793_325 - rowSpan / 2, rowSpan, -140);
  const L = newLayerRing(4, F, C), stop = makeToIdx(-145, -115, 0, 0), real = makeToIdx(-114, -84, 0, 0);
  const kOut = layerWriteRow(L, 0, st, 8_793_325 - rowSpan / 2, rowSpan, 10, stop);
  ok(kOut === 0 && L.pkN[0] === 0, 'a row placed outside its own capture (8.79 MHz on 5.6–8.6) gets NO layer — it would be filled white');
  ok(stop(-110) === 255, `(the guard is load-bearing: the 40 m floor through that row's range is index ${stop(-110)} — white)`);
  const kIn = layerWriteRow(L, 1, st, 7_100_000 - rowSpan / 2, rowSpan, 10, real);
  ok(kIn > 0, 'a row inside the capture (7.1 MHz) still gets its layer');
  ok(rowInCapture(cen + span / 2 - rowSpan, rowSpan, cen, span) && !rowInCapture(cen + span / 2 + 1, rowSpan, cen, span),
     'rowInCapture: a view at the capture\'s edge is in; one whose centre is past it is out');
}

console.log(`   app full-capture layer: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
