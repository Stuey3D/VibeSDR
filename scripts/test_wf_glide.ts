/**
 * WEB WATERFALL GLIDE (web/client/src/wfGlide.ts) — Stuart 2026-10-28: "refer back to the video for the glide". A row
 * emitted t of the way between two frames whose view moved is drawn against the in-between view, both frames resampled
 * there and cross-faded. Proves on the pure rows:
 *   - a fixed carrier lands where the in-between view puts it (sub-pixel: the curve, not a staircase), on a tune AND a zoom;
 *   - the WHOLE row moves (the noise texture travels with the carrier — no streak over a static floor);
 *   - t = 0 / t = 1 give back frame A / frame B exactly (a moving pair joins the steady rows either side seamlessly);
 *   - a one-bin carrier survives a zoom OUT (the max of the bins a column covers);
 *   - outside both frames is the floor (0), and a jump is not a glide.
 * Run: node --no-warnings scripts/test_wf_glide.ts
 */
import { glideRowAt, newGlideScratch, canGlide, placementMoved, zoomEaseMs, zoomEaseView, easedRowAt } from '../web/client/src/wfGlide.ts';

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

{ const A = frame(900000, 200000), B = frame(910000, 200000);
  glideRowAt(A, 900000, 200000, B, 900000, 200000, 0, d, st); const e0 = d.every((v, i) => v === A[i]);
  glideRowAt(A, 900000, 200000, B, 910000, 200000, 0, d, st); const a0 = d.every((v, i) => v === A[i]);
  glideRowAt(A, 900000, 200000, B, 910000, 200000, 1, d, st); const b1 = d.every((v, i) => v === B[i]);
  ok(e0 && a0 && b1, 't = 0 is frame A, t = 1 is frame B, exactly — the moving rows join the steady rows either side');
  const cols: number[] = [];
  for (const t of [0, 0.25, 0.5, 0.75, 1]) { glideRowAt(A, 900000, 200000, B, 910000, 200000, t, d, st); cols.push(peak(d)); }
  ok(cols.join() === '768,755,742,730,716' || cols.every((c, i) => i === 0 || (c < cols[i - 1] && cols[i - 1] - c <= 14)),
     `a tune: the carrier walks across in even steps, one row each — a curve, not a staircase (${cols.join(' ')})`);
  // The noise is a function of frequency: at t = 0.5 the view is 905000..1105000, so column c shows frequency 905000 + c·span/N.
  const [L, S] = glideRowAt(A, 900000, 200000, B, 910000, 200000, 0.5, d, st);
  const want = frame(L, S); let same = 0; for (let i = 0; i < N; i++) if (Math.abs(d[i] - want[i]) <= 6) same++;
  ok(L === 905000 && S === 200000 && same > N * 0.95, `the WHOLE row moves: the noise texture sits where the in-between view puts it (${same}/${N})`);
}
{ const A = frame(900000, 200000), B = frame(940000, 120000);   // a zoom in about 1.0 MHz
  const cols: number[] = [];
  for (const t of [0, 0.5, 1]) { const [L, S] = glideRowAt(A, 900000, 200000, B, 940000, 120000, t, d, st);
    cols.push(peak(d)); ok(Math.abs(peak(d) - Math.floor((F - L) / S * N)) <= 1, `a zoom, t = ${t}: the carrier at the in-between view's column (${peak(d)})`); }
}
{ const A = frame(1030000, 40000), B = frame(1010000, 80000);   // a zoom OUT about the carrier: a column covers ~1.6 bins of A
  glideRowAt(A, 1030000, 40000, B, 1010000, 80000, 0.6, d, st);
  ok(d[peak(d)] >= 200, `a one-bin carrier survives a zoom out (${d[peak(d)]})`);
  // the in-between view (1018000..1082000) reaches past A — B covers it; and nothing is NaN-garbage
  ok(d.every((v) => v >= 40 || v === 0) && d[0] >= 40, 'edges one frame does not cover are filled from the other');
}
{ const A = frame(1000000, 50000), B = frame(1000000, 50000);
  glideRowAt(A, 1000000, 50000, B, 1000000, 50000, 0.5, d, st); ok(d.every((v, i) => v === A[i]), 'a steady pair is the frame exactly'); }
ok(!placementMoved(1e6, 2e5, 1e6, 2e5, N) && placementMoved(1e6, 2e5, 1e6 + 1000, 2e5, N) && placementMoved(1e6, 2e5, 1e6, 1.9e5, N),
   'placementMoved: steady no, tune yes, zoom yes');
ok(canGlide(1e6, 2e5, 1e6 + 20000, 2e5) && !canGlide(1e6, 2e5, 1e6 + 30000, 2e5), 'a move past 1/8 of the span is a jump, not a glide');

// ── THE ZOOM EASE (Stuart 2026-10-28): the history stays as drawn; the new rows ease the zoom on an S-curve ──
{ // a ×2 zoom in about the dial (1.0 MHz): the carrier's column across the ease leaves and lands with small steps, monotone
  const lo0 = 900000, s0 = 200000, lo1 = 950000, s1 = 100000, cols: number[] = [];
  for (let k = 0; k <= 16; k++) { const [L, S] = zoomEaseView(lo0, s0, lo1, s1, k / 16); cols.push((F - L) / S * N); }
  const steps = cols.slice(1).map((c, i) => c - cols[i]);
  ok(Math.abs(cols[0] - (F - lo0) / s0 * N) < 1e-6 && Math.abs(cols[16] - (F - lo1) / s1 * N) < 1e-6, 'the ease starts at the old view and lands on the new');
  ok(steps.every((d) => d > 0) && steps[0] < steps[8] / 4 && steps[15] < steps[8] / 4, `an S-curve: small steps at both ends (${steps.map((d) => d.toFixed(1)).join(' ')})`);
  const [Lm, Sm] = zoomEaseView(lo0, s0, lo1, s1, 0.5);
  ok(Math.abs(Lm + Sm / 2 - 1000000) < 1e-3, 'the zoom\'s fixed point (the dial) stays put mid-ease');
  ok(Math.abs(zoomEaseMs(2) - 1000) < 1e-9 && zoomEaseMs(1 / 64) === 1500 && zoomEaseMs(0.95) < 80 && zoomEaseMs(0.99) === 40, 'the duration scales with the step (×2 1 s, capped 1.5 s, a pinch step under 80 ms, floor 40)');
}
{ // a zoom-in row drawn wider than its data continues the row below at the edges — never black
  const cur = frame(950000, 100000), below = new Uint8Array(N).fill(77);
  easedRowAt(cur, 900000 + 50000, 100000, 900000, 200000, d, below, st);
  ok(d[0] === 77 && d[N - 1] === 77 && d[N / 2] !== 77, 'an eased zoom-in row: edges continue the row below, the middle is the frame');
}
console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
