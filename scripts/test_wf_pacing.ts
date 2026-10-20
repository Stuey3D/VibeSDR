/**
 * WEB WATERFALL ROW PACING (web/client/src/wfPacing.ts) — does the screen get ONE whole row every N
 * display frames, N fixed, on a real-looking feed?
 *
 * Stuart, 2026-10-20, Pi 500 (20 fps) in Safari on a 60 Hz MacBook Air: "Waterfall on the Pi500 is a
 * bit sticky, seems to judder a little." Measured in headless Edge at dpr 2 before the fix: DEFAULT
 * scrolled 43-60 rows/s second to second, 24 hitches/s, a quarter of its rows two-in-one-frame —
 * 60 Hz / 40 device rows/s is exactly 1.5, and N = round(EMA refresh / 40) flipped 1 ↔ 2.
 *
 * Proves, against a simulated display (rAF timestamps with jitter) and a simulated feed (arrivals
 * with jitter, uncorrelated with vsync):
 *   • DEFAULT on Retina @ 60 Hz: exactly 30 rows/s, one row every 2 frames, never 2 rows in a frame;
 *   • SMOOTH on Retina @ 60 Hz: 60 rows/s, one per frame; DEFAULT/SMOOTH at dpr 1 and @ 120 Hz too;
 *   • SHARP: one row per arrival, never two in a frame;
 *   • a refresh measured either side of 60 (59.9 / 60.1) gives the SAME N; dropped vsyncs don't move it;
 *   • a speed change re-decides N (Smooth → Default does not keep Smooth's N = 1);
 *   • renderEvery: the render throttle is a whole number of frames and never drops one at render = display.
 *
 * Run: node --no-warnings scripts/test_wf_pacing.ts
 */
import { RowPacer, framesPerRowFor, renderEvery, nominalRefreshHz, rowsForPair } from '../web/client/src/wfPacing.ts';

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, detail = '') {
  if (cond) { passes++; return; }
  fails++; console.log(`  FAIL ${what}${detail ? `\n       ${detail}` : ''}`);
}

function lcg(seed: number) { let s = seed >>> 0; return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 0x100000000); }

interface Sim { rowsPerSec: number; sharp?: boolean; hz?: number; fps?: number; frameJit?: number;
                arrJit?: number; dropVsync?: number; secs?: number; seed?: number }
/** Returns rows drawn per display SLOT (dropped vsyncs are slots with no frame). */
function simulate(o: Sim) {
  const hz = o.hz ?? 60, fps = o.fps ?? 20, secs = o.secs ?? 30, rnd = lcg(o.seed ?? 7);
  const p = new RowPacer(); p.setRate(o.rowsPerSec, !!o.sharp);
  const arrivals: number[] = [];
  for (let t = 37.3; t < secs * 1000; t += 1000 / fps) arrivals.push(t + (rnd() * 2 - 1) * (o.arrJit ?? 0));
  arrivals.sort((a, b) => a - b);
  let ai = 0, have = false;
  const slots: number[] = [];
  const nSlots = Math.floor(secs * hz);
  for (let k = 1; k < nSlots; k++) {
    const ts = k * 1000 / hz + (rnd() * 2 - 1) * (o.frameJit ?? 0);
    while (ai < arrivals.length && arrivals[ai] <= ts) { p.onArrival(arrivals[ai++]); have = true; }
    if (o.dropVsync && rnd() < o.dropVsync) { slots.push(0); continue; }
    slots.push(p.onFrame(ts, have) > 0 ? 1 : 0);
  }
  const s = slots.slice(hz * 3);                       // settle
  const rows = s.reduce((a, b) => a + b, 0);
  const gaps: number[] = []; let last = -1;
  s.forEach((r, i) => { if (r) { if (last >= 0) gaps.push(i - last); last = i; } });
  const hist: Record<number, number> = {}; for (const g of gaps) hist[g] = (hist[g] || 0) + 1;
  return { rowsPerSec: rows / (s.length / hz), gaps, hist, fpr: p.framesPerRow };
}
const describe = (r: ReturnType<typeof simulate>) => `rows/s ${r.rowsPerSec.toFixed(2)} gaps ${JSON.stringify(r.hist)} N=${r.fpr}`;
const offCadence = (r: ReturnType<typeof simulate>, n: number) => r.gaps.filter((g) => g !== n).length;

// Realistic: rAF timestamps ±1 ms, arrivals ±12 ms (Pi 500 over Wi-Fi/LAN), 30 s.
const REAL = { frameJit: 1, arrJit: 12 };

// ── DEFAULT, Retina (20 screen rows/s × dpr 2 = 40 device rows/s) on 60 Hz — the reported case ──
{
  const r = simulate({ rowsPerSec: 40, ...REAL });
  ok('DEFAULT dpr2 @60: N = 2', r.fpr === 2, describe(r));
  ok('DEFAULT dpr2 @60: 30 rows/s', Math.abs(r.rowsPerSec - 30) < 0.3, describe(r));
  ok('DEFAULT dpr2 @60: every row 2 frames after the last (no hitches)', offCadence(r, 2) === 0, describe(r));
}
for (const seed of [1, 2, 3, 4, 5]) {
  const r = simulate({ rowsPerSec: 40, frameJit: 1.5, arrJit: 20, seed });
  ok(`DEFAULT dpr2 @60, heavy jitter seed ${seed}: never two rows in a frame, ≤0.2 hitches/s`,
     !r.gaps.includes(0) && offCadence(r, 2) / 27 <= 0.2, describe(r));
}
// ── SMOOTH, Retina (30 × 2 = 60) on 60 Hz ──
{
  const r = simulate({ rowsPerSec: 60, ...REAL });
  ok('SMOOTH dpr2 @60: one row every frame, 60 rows/s', r.fpr === 1 && Math.abs(r.rowsPerSec - 60) < 0.5, describe(r));
  ok('SMOOTH dpr2 @60: ≤0.5 hitches/s, never two rows in a frame', !r.gaps.includes(0) && offCadence(r, 1) / 27 <= 0.5, describe(r));
}
// ── dpr 1 and 120 Hz ──
for (const [name, rps, hz, n] of [['DEFAULT dpr1 @60', 20, 60, 3], ['SMOOTH dpr1 @60', 30, 60, 2],
                                   ['DEFAULT dpr2 @120', 40, 120, 3], ['SMOOTH dpr2 @120', 60, 120, 2]] as const) {
  const r = simulate({ rowsPerSec: rps, hz, ...REAL });
  ok(`${name}: N = ${n}, ${hz / n} rows/s, on cadence`, r.fpr === n && Math.abs(r.rowsPerSec - hz / n) < 0.5 && offCadence(r, n) / 27 <= 0.2, describe(r));
}
// ── SHARP: one row per arrival, never two in a frame ──
{
  const r = simulate({ rowsPerSec: 40, sharp: true, ...REAL });
  ok('SHARP: 20 rows/s from 20 fps', Math.abs(r.rowsPerSec - 20) < 0.3, describe(r));
  ok('SHARP: never two rows in a frame', !r.gaps.includes(0), describe(r));
  ok('SHARP: on a 3-frame cadence (≤0.3 hitches/s)', offCadence(r, 3) / 27 <= 0.3, describe(r));
}
// ── SHARP with arrivals sitting ON a vsync boundary (±2 ms around it): still one row every 3 frames ──
for (const seed of [11, 12, 13]) {
  const hz = 60, p = new RowPacer(); p.setRate(40, true); const rnd = lcg(seed);
  const slots: number[] = []; let have = false; let next = 50 + (rnd() * 4 - 2);
  for (let k = 1; k < 60 * 30; k++) {
    const ts = k * 1000 / hz;
    while (next <= ts) { p.onArrival(next); have = true; next = Math.round((next + 50) / 50) * 50 + (rnd() * 4 - 2); }
    slots.push(p.onFrame(ts, have) > 0 ? 1 : 0);
  }
  const s2 = slots.slice(180); const gaps: number[] = []; let last = -1;
  s2.forEach((r, i) => { if (r) { if (last >= 0) gaps.push(i - last); last = i; } });
  const off = gaps.filter((g) => g !== 3).length;
  ok(`SHARP, arrivals on the vsync boundary seed ${seed}: one row every 3 frames (≤0.2 hitches/s)`, off / 27 <= 0.2,
     `off-cadence ${off} of ${gaps.length}`);
}
// ── Slow feed (UberSDR-ish 10 fps): DEFAULT still a steady 30 rows/s of repeated real rows ──
{
  const r = simulate({ rowsPerSec: 40, fps: 10, ...REAL });
  ok('DEFAULT dpr2 @60, 10 fps feed: 30 rows/s, on cadence', Math.abs(r.rowsPerSec - 30) < 0.5 && offCadence(r, 2) / 27 <= 0.3, describe(r));
}
// ── Dropped vsyncs (the browser missing frames) must not move N ──
{
  const r = simulate({ rowsPerSec: 40, dropVsync: 0.05, ...REAL });
  ok('DEFAULT dpr2 @60 with 5 % dropped vsyncs: N stays 2', r.fpr === 2, describe(r));
}

// ── The knife-edge itself ──
ok('59.9 Hz and 60.1 Hz both snap to 60', nominalRefreshHz(59.9) === 60 && nominalRefreshHz(60.1) === 60);
ok('60 Hz / 40 rows/s → N = 2 (ties round to the slower, documented cadence)', framesPerRowFor(60, 40, 0) === 2);
ok('unsnapped 59.5 / 60.5 keep an existing N (hysteresis)', framesPerRowFor(59.5, 40, 2) === 2 && framesPerRowFor(60.5, 40, 2) === 2);
ok('120 Hz / 40 → 3, 120 / 60 → 2', framesPerRowFor(120, 40, 0) === 3 && framesPerRowFor(120, 60, 0) === 2);
ok('a 90 Hz panel / 40 → 2 (2.25 rounds down)', framesPerRowFor(90, 40, 0) === 2);
{
  const p = new RowPacer(); p.setRate(60, false);
  for (let k = 1; k < 200; k++) { const ts = k * 1000 / 60; if (k % 3 === 0) p.onArrival(ts - 1); p.onFrame(ts, true); }
  ok('SMOOTH settles N = 1', p.framesPerRow === 1, `N=${p.framesPerRow}`);
  p.setRate(40, false);
  for (let k = 200; k < 260; k++) { const ts = k * 1000 / 60; if (k % 3 === 0) p.onArrival(ts - 1); p.onFrame(ts, true); }
  ok('SMOOTH → DEFAULT re-decides N = 2 (does not keep Smooth\'s 1)', p.framesPerRow === 2, `N=${p.framesPerRow}`);
}
ok('pair sized UP + 1: a 55 ms worst gap at 33.3 ms/row holds 3 rows', rowsForPair(50, 55, 1000 / 30) === 3);
ok('pair sized UP + 1: a 70 ms worst gap at 33.3 ms/row holds 4 rows', rowsForPair(50, 70, 1000 / 30) === 4);
ok('SHARP-rate pair (1 row/arrival) never sized 0', rowsForPair(5, 0, 1000) >= 1);

// ── Render throttle: whole frames ──
ok('renderEvery: 60 on 60 = every frame', renderEvery(60, 60) === 1);
ok('renderEvery: 40 on 60 = every frame (never below the asked rate)', renderEvery(60, 40) === 1);
ok('renderEvery: 20 on 60 = every 3rd', renderEvery(60, 20) === 3);
ok('renderEvery: 30 on 60 = every 2nd; 60 on 120 = every 2nd', renderEvery(60, 30) === 2 && renderEvery(120, 60) === 2);

console.log(`wf pacing: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
