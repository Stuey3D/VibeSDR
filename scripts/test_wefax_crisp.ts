// test_wefax_crisp.ts — the gold-standard WEFAX rendering (src/utils/wefaxCrisp.ts) on a synthetic speckled chart.
import { addToHist, crispLevels, crispLine, newHist } from '../src/utils/wefaxCrisp.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };
const W = 600, H = 60;
let seed = 3; const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
// Paper ~138 ± 60 speckle (as measured), 4-px black lines every 40 px (a chart is ~10 % ink — the 2nd-percentile ink
// level needs real ink to find), a light-grey shaded band at 100..160 (~90).
const rows: Uint8Array[] = [];
for (let y = 0; y < H; y++) { const r = new Uint8Array(W); for (let x = 0; x < W; x++) {
  let v = 138 + (rnd() - 0.5) * 120; if (x % 40 >= 20 && x % 40 < 24) v = 5; if (x >= 100 && x < 160) v = 90 + (rnd() - 0.5) * 60;
  r[x] = Math.max(0, Math.min(255, Math.round(v))); } rows.push(r); }
const h = newHist(); rows.forEach((r) => addToHist(h, r));
const lv = crispLevels(h)!;
ok(!!lv, 'levels found');
const out = new Uint8Array(W); crispLine((j) => rows[j], 30, W, lv, out);
const mean = (a: number, b: number) => { let s = 0; for (let x = a; x < b; x++) s += out[x]; return s / (b - a); };
ok(mean(405, 415) > 235, `paper comes out white (${mean(405, 415).toFixed(0)})`);
ok(out[301] < 60 && out[302] < 60, `the line stays black (${out[301]}, ${out[302]})`);
ok(mean(110, 150) > 40 && mean(110, 150) < 200, `light shading stays GREY, not erased or blackened (${mean(110, 150).toFixed(0)})`);
const flat = newHist(); for (let i = 0; i < 5000; i++) flat[200]++;
ok(crispLevels(flat) === null, 'a flat page is left alone');
ok(crispLevels(newHist()) === null, 'nothing received yet → no levels');
console.log(`wefaxCrisp: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
