// test_wefax_align.ts — WEFAX SHIFT / SLANT and the per-chart margin finder (src/utils/wefaxAlign.ts).
// Synthetic charts: a speckled page with a few curved "isobars", plus (or not) a black margin line that drifts with
// the station's slant — the shape measured on Northwood 4610, 2026-10-04 (margin 40 px in on one chart, ~370 on another).
import { findMargin, wefaxOffset, wefaxPreset, MARGIN_AFTER_LINES } from '../src/utils/wefaxAlign.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };
const W = 1809;
let seed = 7; const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
function chart(lines: number, margin: number | null, slant: number): Uint8Array[] {
  const rows: Uint8Array[] = [];
  for (let y = 0; y < lines; y++) {
    const r = new Uint8Array(W);
    for (let x = 0; x < W; x++) r[x] = 200 + Math.floor(rnd() * 55) - (rnd() < 0.08 ? 150 : 0);   // speckled page
    for (const c of [300, 700, 1200]) { const cx = Math.round(c + 80 * Math.sin((y + c) / 90)); for (let k = -1; k <= 1; k++) r[(cx + k + W) % W] = 20; } // curved isobars
    if (margin !== null) { const m = Math.round(margin + slant * y); for (let k = -3; k <= 3; k++) r[((m + k) % W + W) % W] = 0; }
    rows.push(r);
  }
  return rows;
}
const near = (a: number | null, b: number, tol = 3) => a !== null && Math.abs(((a - b + W * 1.5) % W) - W / 2) <= tol;
ok(near(findMargin(chart(MARGIN_AFTER_LINES, 42, -0.06), W, -0.06), 42), 'margin 42 px in, Northwood slant → found at 42');
ok(near(findMargin(chart(MARGIN_AFTER_LINES, 368, -0.06), W, -0.06), 368), 'margin dead centre-ish (368) → found');
ok(near(findMargin(chart(MARGIN_AFTER_LINES, 5, 0), W, 0), 5), 'DWD-like: margin at the edge, no slant → found at 5 (a 3 px move)');
ok(findMargin(chart(MARGIN_AFTER_LINES, null, 0), W, 0) === null, 'no margin on the chart → null (nothing is moved)');
ok(findMargin(chart(40, 42, -0.06), W, -0.06) === null, 'too few lines yet → null');
// The arithmetic the canvases use
ok(wefaxOffset({ shift: 40, slant: -0.06 }, 0, W) === 40 && wefaxOffset({ shift: 40, slant: -0.06 }, 1000, W) === W - 20, 'offset wraps');
ok(wefaxPreset(4608100).slant === -0.06 && wefaxPreset(4608100).shift === 0, 'Northwood preset = slant only');
ok(wefaxPreset(7878100).slant === 0, 'DWD 7880 = no correction');
console.log(`wefaxAlign: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
