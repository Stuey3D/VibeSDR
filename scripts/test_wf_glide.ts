/**
 * WEB WATERFALL ROW PLACEMENT (web/client/src/wfGlide.ts) — the app's zoom/tune ease and its shader join, on the web's rows
 * (Stuart 2026-10-29: "yes please we want to preserve the history across tunes and zooms"). Proves on the pure rows:
 *   - THE JOIN: a row drawn away from the row below is lit all the way across the move (one solid trail), its noise
 *     averaged (no brightened floor); a steady row is its frame exactly;
 *   - THE EASE: a zoom eases over the app's zoomEaseRows (4 a ×2), ease-out, about its fixed point; each device row of a
 *     Retina canvas its own step; a tune step over tuneEaseRows; a tiny move at once; a whole view or more a JUMP; a
 *     step mid-ease carries on from where it had got to;
 *   - easeView (shared with the app) starts on the old view and lands on the new; a zoom-in row's edges continue the row
 *     below.
 * The picture on the real renderer: scripts/test-web-wf-fan.mjs.
 * Run: node --no-warnings scripts/test_wf_glide.ts
 */
import { newGlideScratch, zoomEaseView, easedRowAt, joinedRowAt, viewMoveCols, newRowEase, rowView } from '../web/client/src/wfGlide.ts';
import { cappedEaseRows, zoomEaseRows, tuneEaseRows, EASE_CEIL_S } from '../src/utils/wfRowPlace.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const N = 1024, F = 1050000;
// A frame of the band (lo, span): noise that is a fixed function of FREQUENCY (so it should move with the view) + a carrier at F.
const frame = (lo: number, span: number) => {
  const r = new Uint8Array(N);
  for (let i = 0; i < N; i++) { const f = lo + (i + 0.5) * span / N; r[i] = 40 + (Math.floor(f / 1000) * 7919 % 13); }
  const c = Math.floor((F - lo) / span * N); if (c >= 0 && c < N) r[c] = 250;
  return r;
};
const peak = (d: Uint8Array) => { let b = 0, bi = -1; for (let i = 0; i < N; i++) if (d[i] > b) { b = d[i]; bi = i; } return bi; };
const st = newGlideScratch(N), d = new Uint8Array(N);

// ── THE JOIN (the app's shader motion taps, done when the row is written): a moving row is ONE solid line to the row below ──
{ // a tune: this row at 905 kHz, the row below at 900 kHz — the carrier is at column 742 here and 768 below
  const cur = frame(905000, 200000);
  const K = joinedRowAt(cur, 905000, 200000, 905000, 200000, 900000, 200000, d, null, st);
  const lit: number[] = []; for (let i = 0; i < N; i++) if (d[i] >= 120) lit.push(i);
  ok(K >= 25 && lit[0] <= 743 && lit[lit.length - 1] >= 766 && lit.length === lit[lit.length - 1] - lit[0] + 1,
     `a tune: the row is lit all the way from its own column to the row below's (${lit[0]}..${lit[lit.length - 1]}, a move of ${K.toFixed(0)} columns) — no gap, no dash`);
  // the noise is averaged along the move, never max-held (no brightened floor)
  let hi = 0; for (let i = 0; i < 700; i++) if (d[i] > 52) hi++;
  ok(hi === 0, `the floor along the move is averaged, not brightened (${hi} columns above the floor's top)`);
  // a steady row (no move) is the frame exactly
  joinedRowAt(cur, 905000, 200000, 905000, 200000, 905000, 200000, d, null, st);
  ok(d.every((v, i) => v === cur[i]), 'a row at the same view as the row below is the frame exactly (one tap)');
}
{ // a zoom: this row at the frame's own view (120 kHz), the row below at 200 kHz — joined across the fan
  const cur = frame(940000, 120000), below = new Uint8Array(N).fill(77);
  joinedRowAt(cur, 940000, 120000, 940000, 120000, 900000, 200000, d, below, st);
  const c0 = Math.floor((F - 940000) / 120000 * N), c1 = Math.floor((F - 900000) / 200000 * N);
  let gap = 0, run = 0; for (let i = Math.min(c0, c1); i <= Math.max(c0, c1); i++) { if (d[i] >= 120) run = 0; else gap = Math.max(gap, ++run); }
  ok(gap <= 3, `a zoom: the trail is solid from column ${c1} (the row below) to ${c0} (this row) — largest gap ${gap}`);
  ok(d[N - 1] !== 77 && d[0] !== 77, 'the frame covers the row, so nothing is borrowed from below');
}
ok(Math.abs(viewMoveCols(900000, 200000, 910000, 200000, N) - 51.2) < 1e-6 && viewMoveCols(1e6, 5e4, 1e6, 5e4, N) === 0,
   'viewMoveCols: a 10 kHz move on a 200 kHz view is 51.2 columns; none is 0');

// ── THE EASE (the app's fanNoteRow / fanAfterRow, counted in device rows) ──
{ const e = newRowEase(), v = { lo: 0, span: 0, jumpHz: 0 };
  rowView(e, 900000, 200000, 1, v);
  ok(v.lo === 900000 && v.span === 200000 && v.jumpHz === 0, 'the first row is drawn where its frame is');
  // a ×2 zoom in about the centre: zoomEaseRows(0.5) rows (BY TIME — 0.33 s at the default 30), each further, landing exactly
  // ★★★ + THE SLOPE CAP (wfRowPlace.ts cappedEaseRows): the ×2's edges move half a view, so the ease runs to its ceiling
  const NZ = cappedEaseRows(900000, 200000, 950000, 100000, zoomEaseRows(0.5), 30);
  ok(NZ > zoomEaseRows(0.5) && NZ <= EASE_CEIL_S * 30, `the slope cap lengthens a ×2 from ${zoomEaseRows(0.5)} to ${NZ} rows (≤ ${EASE_CEIL_S} s)`);
  const spans: number[] = [], ctr: number[] = [];
  for (let i = 0; i < NZ + 2; i++) { rowView(e, 950000, 100000, 1, v); spans.push(v.span); ctr.push(v.lo + v.span / 2); }
  ok(NZ >= 8 && spans[NZ - 1] === 100000 && spans[NZ - 2] > 100000 && spans[0] < 200000 && spans.slice(0, NZ).every((x, i) => i === 0 || x < spans[i - 1]),
     `a ×2 zoom eases over ${NZ} rows (${spans.map((s) => (s / 1000).toFixed(1)).join(' ')} kHz)`);
  ok(ctr.every((c) => Math.abs(c - 1000000) < 1e-6), 'the zoom\'s fixed point (the centre) holds through the ease');
  const r0 = Math.log2(200000 / spans[0]);
  ok(r0 >= 1 / NZ && r0 < 0.3, `it MOVES FROM THE FIRST ROW, at a steady pace (${r0.toFixed(3)} of an octave; a fair share is ${(1 / NZ).toFixed(3)}) — no 75 % first-row stair`);
  // on a Retina canvas (rowPx 2) each device row is its own step — twice as many
  const e2 = newRowEase(); rowView(e2, 900000, 200000, 2, v);
  const s2: number[] = []; for (let i = 0; i < 2 * NZ + 1; i++) { rowView(e2, 950000, 100000, 2, v); s2.push(v.span); }
  ok(s2[2 * NZ - 1] === 100000 && s2[2 * NZ - 2] > 100000 && new Set(s2.slice(0, 2 * NZ - 1)).size === 2 * NZ - 1, `rowPx 2: the ease takes ${2 * NZ} device rows, each at its own view`);
  // ★ the device row rate given (the pacer's easeRowRate): counted in it directly — 60 device rows/s = 20 rows for a ×2
  const e3 = newRowEase(); rowView(e3, 900000, 200000, 1, v, 60);
  let n3 = 0; for (let i = 0; i < 90; i++) { rowView(e3, 950000, 100000, 1, v, 60); if (v.span > 100000) n3++; }
  const N3 = cappedEaseRows(900000, 200000, 950000, 100000, zoomEaseRows(0.5, 60), 60);
  ok(n3 + 1 === N3 && (n3 + 1) / 60 <= EASE_CEIL_S + 1e-9, `at 60 device rows/s a ×2 eases over ${n3 + 1} rows = ${((n3 + 1) / 60).toFixed(2)} s (≤ ${EASE_CEIL_S} s)`);
  // a 9 kHz tune step on a 100 kHz view: tuneEaseRows(0.09) rows sliding lo, the span untouched
  const NT = cappedEaseRows(950000, 100000, 959000, 100000, tuneEaseRows(0.09), 30);
  const los: number[] = []; for (let i = 0; i < NT + 2; i++) { rowView(e, 959000, 100000, 1, v); los.push(v.lo); }
  ok(NT >= 6 && los[0] > 950000 && los[0] < 959000 && los[NT - 1] === 959000 && los[NT - 2] < 959000 && v.span === 100000 && los.every((l, i) => i === 0 || l >= los[i - 1]),
     `a tune step glides across over ${NT} rows (${los.map((l) => ((l - 950000) / 1000).toFixed(2)).join(' ')} kHz of 9)`);
  // a tiny move (under two columns of 1024): drawn at once, no ease
  rowView(e, 959100, 100000, 1, v); ok(v.lo === 959100, 'a move under two columns is drawn where it is (the join covers it)');
  // a jump of a whole view or more: no ease, the history moves by the centre's Hz
  rowView(e, 1100000, 100000, 1, v);
  ok(v.jumpHz === 140900 && v.lo === 1100000, `a move of a whole view is a JUMP (history moved by ${v.jumpHz} Hz), drawn where it is`);
  // a step mid-ease starts from where the ease had got to — never a snap back
  rowView(e, 1100000, 50000, 1, v); const mid = v.span; rowView(e, 1100000, 25000, 1, v);
  ok(v.span < mid && v.span > 25000, `a zoom step mid-ease carries on from the view it had reached (${(mid / 1000).toFixed(1)} → ${(v.span / 1000).toFixed(1)} kHz)`);
}

{ // A SPIN whose steps come unevenly (a radio catching up: 1.5 kHz after 2 rows, then after 4 — repeated): the trail moves
  // at an even pace, never standing still between steps (an ease-out per step would land at rest each time — a staircase)
  const e = newRowEase(), v = { lo: 0, span: 0, jumpHz: 0 }; let lo = 900000, prev = NaN; const steps: number[] = [];
  rowView(e, lo, 200000, 1, v);
  for (let r = 0; r < 60; r++) { if (r % 6 === 0 || r % 6 === 2) lo -= 1500; rowView(e, lo, 200000, 1, v);
    const x = (1e6 - v.lo) / 200000 * 1280; if (r >= 12) steps.push(x - prev); prev = x; }
  const still = steps.filter((d) => d < 0.5).length;
  ok(still === 0 && Math.max(...steps) < 3 * (3000 / 200000 * 1280 / 6), `a spin moves evenly: ${still} rows standing still, steps ${steps.map((d) => d.toFixed(1)).join(' ')}`);
}

// ── THE EASE'S GEOMETRY (easeView, shared with the app — src/utils/wfRowPlace.ts) ──
{ // a ×2 zoom in about the dial (1.0 MHz): the carrier's column across the ease leaves and lands with small steps, monotone
  const lo0 = 900000, s0 = 200000, lo1 = 950000, s1 = 100000, cols: number[] = [];
  for (let k = 0; k <= 16; k++) { const [L, S] = zoomEaseView(lo0, s0, lo1, s1, k / 16); cols.push((F - L) / S * N); }
  const steps = cols.slice(1).map((c, i) => c - cols[i]);
  ok(Math.abs(cols[0] - (F - lo0) / s0 * N) < 1e-6 && Math.abs(cols[16] - (F - lo1) / s1 * N) < 1e-6, 'the ease starts at the old view and lands on the new');
  const mean = steps.reduce((a, b) => a + b, 0) / steps.length;
  ok(steps.every((d) => d > 0) && steps[0] > 0.8 * mean && Math.max(...steps) < 1.6 * mean && steps[15] < steps[0],
     `steady with a soft landing: moves from the first step, no step over 1.6× the mean (${steps.map((d) => d.toFixed(1)).join(' ')})`);
  const [Lm, Sm] = zoomEaseView(lo0, s0, lo1, s1, 0.5);
  ok(Math.abs(Lm + Sm / 2 - 1000000) < 1e-3, 'the zoom\'s fixed point (the dial) stays put mid-ease');
}
{ // a zoom-in row drawn wider than its data continues the row below at the edges — never black
  const cur = frame(950000, 100000), below = new Uint8Array(N).fill(77);
  easedRowAt(cur, 900000 + 50000, 100000, 900000, 200000, d, below, st);
  ok(d[0] === 77 && d[N - 1] === 77 && d[N / 2] !== 77, 'an eased zoom-in row: edges continue the row below, the middle is the frame');
}
console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
