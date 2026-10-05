// test_wefax_align.ts — WEFAX SHIFT / SLANT and the per-chart margin finder (src/utils/wefaxAlign.ts).
// Synthetic charts: a speckled page with a few curved "isobars", plus (or not) a black margin line that drifts with
// the station's slant — the shape measured on Northwood 4610, 2026-10-04 (margin 40 px in on one chart, ~370 on another).
import { findMargin, findMarginSlant, findGutter, findChartAlign, chartAlignStep, wefaxOffset, wefaxPreset, MARGIN_AFTER_LINES,
         type ChartAlignState } from '../src/utils/wefaxAlign.ts';
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

// ★ 2026-10-05 — slant MEASURED per chart (MadPsy: "depends on the frequency accuracy of the particular hardware").
{ const m = findMarginSlant(chart(300, 42, -0.09), W, -0.06, 0, 300);   // Northwood + ~16 ppm of receiver clock
  ok(!!m && near(m.col, 42) && Math.abs(m.slant + 0.09) <= 0.003, `slant measured, not preset: -0.09 found as ${m?.slant}`); }
{ const m = findMarginSlant(chart(300, 400, 0.03), W, 0, 0, 300);
  ok(!!m && near(m.col, 400) && Math.abs(m.slant - 0.03) <= 0.003, `a fast receiver clock on a straight station: +0.03 found as ${m?.slant}`); }
// ★ DDK-style: a map in a thin black FRAME on a white border (no margin). Phased → border at the edges.
function bordered(lines: number, roll: number): Uint8Array[] {
  const rows: Uint8Array[] = [];
  for (let y = 0; y < lines; y++) {
    const r = new Uint8Array(W).fill(252);
    for (let x = 55; x <= 1745; x++) r[x] = 215 + Math.floor(rnd() * 40) - (rnd() < 0.08 ? 150 : 0);  // the map
    r[55] = r[56] = r[1744] = r[1745] = 10;                                                             // the frame
    for (const c of [500, 1100]) { const cx = Math.round(c - 0.17 * y); r[cx] = r[cx + 1] = 20; }      // meridians
    const o = new Uint8Array(W);
    for (let x = 0; x < W; x++) o[x] = r[(x + roll) % W];
    rows.push(o);
  }
  return rows;
}
ok(findChartAlign(bordered(300, 0), W, 0, 300) === null, 'DDK phased: frame is NOT a margin, border already at the edge → not moved');
{ const a = findChartAlign(bordered(300, 900), W, 0, 300);
  // joined mid-way: line starts 900 px late, so the border (orig 1746…54) sits at 846…963 — centre ≈ 900 → moved to the edge
  ok(!!a && Math.abs(a.shift - 900) <= 6 && a.slant === 0, `DDK joined mid-chart: the white border is cut at ${a?.shift} (≈900)`); }
ok(findGutter(chart(300, 42, -0.06), W, -0.06, 0, 300) === null, 'a Northwood-style chart has no blank band');
{ const a = findChartAlign(chart(300, 42, -0.06), W, -0.06, 300);
  ok(!!a && near(a.shift, 40), 'Northwood: margin path still wins'); }
// ★ chartAlignStep: decided at 300 lines WITH CONTENT, refines at 600 only a chart it moved; phased DDK untouched.
const feed = (st: ChartAlignState, rows: Uint8Array[], slant: number, upto: number) => {
  let moved = 0;
  for (let y = 0; y < upto; y++) if (chartAlignStep(st, () => rows.slice(0, y + 1), W, slant, rows[y])) moved++;
  return moved;
};
{ const rows = chart(700, 42, -0.06); const st: ChartAlignState = {};
  ok(feed(st, rows, -0.06, 299) === 0 && st.al === undefined, 'nothing before 300 lines');
  const st2: ChartAlignState = {}; ok(feed(st2, rows, -0.06, 300) === 1 && !!st2.al && near(st2.al.shift, 40), 'decided at 300 → one redraw, margin to the edge');
  const st3: ChartAlignState = {}; feed(st3, rows, -0.06, 650);
  ok(st3.refined === true, 'second look taken once, by 600'); }
{ const rows = bordered(700, 0); const st: ChartAlignState = {};
  ok(feed(st, rows, 0, 700) === 0 && st.al === null, 'phased DDK: left alone at 300, still alone at 600'); }
// ★ DDK's steady tone before the chart (off air, 2026-10-05): ~270 flat grey lines, then a chart joined without
//   phasing. The grey must not hide the border, and the flat lines must not count towards the 300.
{ const tone = Array.from({ length: 270 }, () => { const r = new Uint8Array(W); for (let x = 0; x < W; x++) r[x] = 100 + Math.floor(rnd() * 20); return r; });
  const rows = [...tone, ...bordered(600, 900)]; const st: ChartAlignState = {};
  ok(feed(st, rows, 0, 300) === 0 && st.al === undefined && (st.n ?? 0) < 300, 'tone lines are not counted (no decision at line 300)');
  const st2: ChartAlignState = {}; let moved = 0;
  for (let y = 0; y < rows.length && st2.al === undefined; y++) if (chartAlignStep(st2, () => rows.slice(0, y + 1), W, 0, rows[y])) moved++;
  ok(moved === 1 && !!st2.al && Math.abs(st2.al.shift - 900) <= 6, `after the tone, the border is still found: cut at ${st2.al?.shift} (≈900)`); }
// The arithmetic the canvases use
ok(wefaxOffset({ shift: 40, slant: -0.06 }, 0, W) === 40 && wefaxOffset({ shift: 40, slant: -0.06 }, 1000, W) === W - 20, 'offset wraps');
ok(wefaxPreset(4608100).slant === -0.06 && wefaxPreset(4608100).shift === 0, 'Northwood preset = slant only');
ok(wefaxPreset(7878100).slant === 0, 'DWD 7880 = no correction');
console.log(`wefaxAlign: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
