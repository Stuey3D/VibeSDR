/**
 * THE FULL-CAPTURE LAYER (web/client/src/wfWide.ts) — V12, Stuart 2026-10-28: "the waterfall should be buffering outside the
 * zoomed area so that the black doesn't show at all"; v2 after his review of v1 ("the same waterfall, just less sharp").
 * Proves on the pure rows:
 *   - an SPWB v2 frame parses: centre, span, the floor (0 byte = no data), the peaks with delta-coded cells and the escape;
 *     v1 and anything else is refused;
 *   - the dense row is the FLOOR between signals (not their peak) and a one-cell carrier is NARROW (≤ 2 of 4096 bins), at
 *     its frequency to 1/16 of a cell; a wide peak (a voice) is as wide as its shape says;
 *   - the 2D twin: carriers at their frequencies inside and outside the view, 0 only beyond the capture;
 *   - m round-trips through its 24-bit encoding to under a hertz across 2.4 MHz.
 * Run: node --no-warnings scripts/test_wf_wide.ts
 */
import { parseWide, floorIdx, denseRow, cellsOf, wideExtent, wideRowInto, percentile, encodeM, decodeM, peakGeom, WIDE_HDR, WIDE_TEX } from '../web/client/src/wfWide.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const CAP = 2400000, CEN = 1015000, LO = CEN - CAP / 2, C = 1024, F = 128;
/** Build a v2 frame: floor bytes, peaks [cell, levelByte, shape]. */
const frame = (floorB: number[], peaks: [number, number, number][], ver = 2, spreadB: number[] = floorB.map(() => 28)) => {
  const trip: number[] = []; let prev = -1;
  for (const [c, l, s] of peaks) { let d = c - prev; while (d > 255) { trip.push(255, 0, 0); d -= 255; } trip.push(d, l, s); prev = c; }
  const b = new ArrayBuffer(WIDE_HDR + 2 * floorB.length + trip.length), dv = new DataView(b), u = new Uint8Array(b);
  u.set([0x53, 0x50, 0x57, 0x42, ver, 0]); dv.setBigUint64(14, BigInt(CEN), true); dv.setUint32(22, CAP, true);
  dv.setUint16(26, floorB.length, true); dv.setUint16(28, C, true); dv.setUint16(30, trip.length / 3, true);
  u.set(floorB, WIDE_HDR); u.set(spreadB, WIDE_HDR + floorB.length); u.set(trip, WIDE_HDR + 2 * floorB.length); return b;
};
const cellOf = (f: number) => Math.floor((f - LO) / CAP * C);
const floorB = Array.from({ length: F }, (_, i) => i === 0 ? 0 : 140);   // -116 dBFS, the first bin no data
const peaks: [number, number, number][] = [[cellOf(700000), 200, 7], [cellOf(1050000), 200, 7], [cellOf(1400000), 200, 7], [cellOf(2100000), 180, (11 << 4) | 8]];
const w = parseWide(frame(floorB, peaks), -256)!;
ok(!!w && w.centreHz === CEN && w.spanHz === CAP && w.floor.length === F && w.floor[1] === -116 && Number.isNaN(w.floor[0]) && w.cells === C,
   'SPWB v2 parses: centre, span, the floor (dB = byte − 256), a 0 byte is no data');
ok(w.nPk === 4 && w.pkCell[3] === cellOf(2100000) && w.pkDb[0] === -56 && w.pkShape[3] === ((11 << 4) | 8),
   `the peaks, delta-coded across an escape (cells ${[...w.pkCell.subarray(0, w.nPk)].join(', ')})`);
ok(Math.abs(w.spread[5] - 2.8) < 1e-6, 'the floor\'s spread parses (σ in 0.1 dB)');
{ // the floor as the detailed rows LOOK: a speckle of σ round a median just under the palette's clip averages ABOVE black
  const toIdx = (db: number) => { const n = Math.max(0, Math.min(1, (db + 90) / 30)) * 255; return Math.round(n < 14.97 ? 0 : (n - 14.97) / (240.03) * 255); };
  ok(toIdx(-89) === 0 && floorIdx(-89, 2.8, toIdx) > 0 && floorIdx(-89, 0, toIdx) === 0 && Math.abs(floorIdx(-70, 2.8, toIdx) - toIdx(-70)) <= 1,
     `the floor's colour is its speckle's average: idx(−89) = 0 but E[idx] = ${floorIdx(-89, 2.8, toIdx)}; well above the clip it is idx(m)`); }
ok(parseWide(frame(floorB, peaks, 1), -256) === null, 'a v1 frame is refused (it was never released)');
{ const b = frame(floorB, peaks); new Uint8Array(b)[3] = 0x43; ok(parseWide(b, -256) === null, 'a SPEC frame is not mistaken for one'); }

const cellDb = new Float32Array(C), shape = new Uint8Array(C);
cellsOf(w, cellDb, shape);
const dense = new Float32Array(4096);
denseRow(w.floor, cellDb, shape, dense);
{
  const at = (f: number) => Math.floor((f - LO) / CAP * 4096);
  const lit = (f: number) => { let n = 0; for (let i = at(f) - 8; i <= at(f) + 8; i++) if (dense[i] > -80) n++; return n; };
  ok(Math.abs(dense[at(900000)] + 116) < 0.01 && Math.abs(dense[at(1800000)] + 116) < 0.01, 'between signals the dense row is the floor (−116), not a peak');
  ok(lit(1050000) >= 1 && lit(1050000) <= 2 && lit(700000) <= 2, `a one-cell carrier is NARROW: ${lit(1050000)} of 4096 bins (a 2.34 kHz cell, drawn to 1/16 of it)`);
  const [c0] = peakGeom(cellOf(1050000), 7, C);
  ok(Math.abs(LO + c0 * CAP - 1050000) <= CAP / C, `…at its frequency: ${((LO + c0 * CAP) / 1e3).toFixed(1)} kHz (carrier 1050)`);
  let wide = 0; for (let i = at(2100000) - 12; i <= at(2100000) + 12; i++) if (dense[i] > -90) wide++;
  ok(wide >= 2 && wide <= 4, `a wide peak (a voice, 12/16 of a cell) is as wide as it says: ${wide} bins`);
  ok(Number.isNaN(dense[0]) === false, 'the first floor bin has no data, the floor next to it carries the edge');
}
ok(wideExtent(CAP, CAP) === 0 && wideExtent(CAP, 200000) === 12, 'the 2D twin\'s extent X = capture ÷ view, 0 when the view covers it');
{ // the 2D twin for a 200 kHz row at 1.000–1.200 MHz
  const lo = 1000000, span = 200000, X = wideExtent(CAP, span), dst = new Uint8Array(WIDE_TEX);
  wideRowInto(dst, { centreHz: CEN, spanHz: CAP, db: dense }, lo, span, X, 0, (db) => Math.round(db + 160));
  const fOf = (t: number) => lo + (-X + (t + 0.5) * (1 + 2 * X) / WIDE_TEX) * span;
  const peaksAt: number[] = []; for (let t = 1; t < WIDE_TEX - 1; t++) if (dst[t] > 80 && dst[t] >= dst[t - 1] && dst[t] > dst[t + 1]) peaksAt.push(Math.round(fOf(t) / 1000));
  const near = (k: number) => peaksAt.some((p) => Math.abs(p - k) <= 3);
  ok(near(1050) && near(1400) && near(700), `2D twin: carriers at their frequencies, inside the view and outside it both ways (${peaksAt.join(', ')} kHz)`);
  const outside = dst.filter((v, t) => fOf(t) < LO - 3000 || fOf(t) > LO + CAP + 3000);
  ok(outside.length > 50 && outside.every((v) => v === 0), '2D twin: beyond the capture 0, no data (black is allowed only there)');
}
{ const b = new Uint8Array(4); let worst = 0;
  for (const m of [-1.7, -0.25, 0, 0.123456789, 0.5, 1.9]) { encodeM(m, b, 0); worst = Math.max(worst, Math.abs(decodeM(b, 0) - m) * CAP); }
  ok(worst < 1, `m round-trips through 24 bits: worst ${worst.toFixed(2)} Hz across the capture`); }
{ const sc = { buf: new Float32Array(4) }; const a = new Float32Array([5, NaN, 1, 2, 3, 4, 6, 7, 8, 9, 10]);
  ok(percentile(a, 0, a.length, 0.2, sc) === 3 && Number.isNaN(percentile(a, 0, 4, 0.2, sc)), 'percentile skips no-data and needs 8 values'); }

console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
