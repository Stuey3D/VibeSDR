/**
 * THE FULL-CAPTURE LAYER (web/client/src/wfWide.ts) — V12, Stuart 2026-10-28: "the waterfall should be buffering outside the
 * zoomed area so that the black doesn't show at all". Proves on the pure rows:
 *   - an SPWB frame parses (centre, span, natural-order bins, 0 = no data → NaN), anything else is refused;
 *   - X covers the capture whatever the view's place in it, and is 0 when the view already covers the capture;
 *   - the coarse row for a detailed row puts a carrier at the frequency it really is in the detailed row's own extended
 *     u-space — inside the view, and outside it on both sides;
 *   - a one-bin carrier survives when a texel covers several coarse bins (max), outside the capture is 0 (no data), and
 *     real data is never 0 (so it cannot read as "no data").
 * Run: node --no-warnings scripts/test_wf_wide.ts
 */
import { parseWide, wideExtent, wideRowInto, percentile, WIDE_HDR, WIDE_TEX } from '../web/client/src/wfWide.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const frame = (centre: number, span: number, bins: Uint8Array) => {
  const b = new ArrayBuffer(WIDE_HDR + bins.length), dv = new DataView(b), u = new Uint8Array(b);
  u.set([0x53, 0x50, 0x57, 0x42, 1, 0]); dv.setBigUint64(6, 0n, true); dv.setBigUint64(14, BigInt(centre), true); dv.setUint32(22, span, true);
  u.set(bins, WIDE_HDR); return b;
};
const N = 1024, CAP = 2400000, CEN = 1015000;
const bins = new Uint8Array(N).fill(140); bins[0] = 0;
const carrierAt = (f: number) => Math.floor((f - (CEN - CAP / 2)) / CAP * N);
bins[carrierAt(1400000)] = 200; bins[carrierAt(1050000)] = 200; bins[carrierAt(700000)] = 200;
const w = parseWide(frame(CEN, CAP, bins), -256)!;
ok(!!w && w.centreHz === CEN && w.spanHz === CAP && w.db.length === N && w.db[1] === -116 && Number.isNaN(w.db[0]),
   'SPWB parses: centre, span, dB = byte − 256, a 0 byte is no data');
{ const bad = frame(CEN, CAP, bins); new Uint8Array(bad)[3] = 0x43; ok(parseWide(bad, -256) === null, 'a SPEC frame is not mistaken for one'); }
ok(wideExtent(CAP, CAP) === 0 && wideExtent(CAP, 3000000) === 0, 'X = 0 when the view covers the capture (no layer)');
ok(wideExtent(CAP, 200000) === 12 && wideExtent(CAP, 20000) === 120, `X = capture ÷ view, rounded up (${wideExtent(CAP, 200000)}, ${wideExtent(CAP, 20000)})`);
{ // a 200 kHz detailed row at 1.000–1.200 MHz: its coarse twin, texel t ↔ u = −X + (t + ½)(1 + 2X)/W
  const lo = 1000000, span = 200000, X = wideExtent(CAP, span), dst = new Uint8Array(WIDE_TEX);
  wideRowInto(dst, w, lo, span, X, 0, (db) => db + 160);   // index = dB + 160: floor 44, carrier 104
  const fOf = (t: number) => lo + (-X + (t + 0.5) * (1 + 2 * X) / WIDE_TEX) * span;
  const peaks: number[] = []; for (let t = 1; t < WIDE_TEX - 1; t++) if (dst[t] > 80 && dst[t] >= dst[t - 1] && dst[t] >= dst[t + 1]) peaks.push(Math.round(fOf(t) / 1000));
  const near = (k: number) => peaks.some((p) => Math.abs(p - k) <= 3);
  ok(near(1050) && near(1400) && near(700), `carriers at their frequencies, inside the view and outside it both ways (${peaks.join(', ')} kHz)`);
  const outside = dst.filter((v, t) => fOf(t) < CEN - CAP / 2 - 3000 || fOf(t) > CEN + CAP / 2 + 3000);
  ok(outside.length > 50 && outside.every((v) => v === 0), 'beyond the capture: 0, no data (black is allowed only there)');
  const inside = dst.filter((v, t) => fOf(t) > CEN - CAP / 2 + 5000 && fOf(t) < CEN + CAP / 2 - 5000);
  ok(inside.every((v) => v >= 1), 'inside the capture every texel has data (≥ 1 — never the no-data 0)');
}
{ // a 2 kHz row: X = 1200, a texel covers ~1.17 coarse bins → linear; a 100 Hz row: X huge, a texel covers many bins → max
  const dst = new Uint8Array(WIDE_TEX);
  wideRowInto(dst, w, 1049000, 400, wideExtent(CAP, 400), 0, (db) => db + 160);
  ok(Math.max(...dst) >= 100, `zoomed far in (a 400 Hz view, X = ${wideExtent(CAP, 400)}): the one-bin carriers survive the max (${Math.max(...dst)})`);
}
{ const sc = { buf: new Float32Array(4) }; const a = new Float32Array([5, NaN, 1, 2, 3, 4, 6, 7, 8, 9, 10]);
  ok(percentile(a, 0, a.length, 0.2, sc) === 3 && Number.isNaN(percentile(a, 0, 4, 0.2, sc)), 'percentile skips no-data and needs 8 values'); }

console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
