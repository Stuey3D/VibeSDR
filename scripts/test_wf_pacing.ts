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
 *   • the APP's speeds (Default 20, Smooth 60 screen rows/s) at dpr 1 and 2, 60 and 120 Hz, on 20, 10
 *     and 5 fps feeds: a fixed number of device rows every fixed number of frames — Default on Retina
 *     @ 60 Hz is a screen row (2 device rows) every 3 frames, never the 1.5-frame knife-edge;
 *   • SHARP: one real row per arrival, one screen row tall, on a 3-frame cadence at 20 fps;
 *   • a refresh measured either side of 60 (59.9 / 60.1) gives the SAME N; dropped vsyncs don't move it;
 *   • a speed change re-decides N (Smooth → Default does not keep Smooth's N = 1);
 *   • renderEvery: the render throttle is a whole number of frames and never drops one at render = display.
 *
 * Run: node --no-warnings scripts/test_wf_pacing.ts
 */
import { RowPacer, framesPerRowFor, renderEvery, nominalRefreshHz, rowsForPair, emitShape } from '../web/client/src/wfPacing.ts';

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, detail = '') {
  if (cond) { passes++; return; }
  fails++; console.log(`  FAIL ${what}${detail ? `\n       ${detail}` : ''}`);
}

function lcg(seed: number) { let s = seed >>> 0; return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 0x100000000); }

interface Sim { rowsPerSec: number; sharp?: boolean; rowPx?: number; scroll?: 'sharp' | 'default' | 'smooth'; fpsAt?: (ms: number) => number; hz?: number; fps?: number; frameJit?: number;
                arrJit?: number; dropVsync?: number; secs?: number; seed?: number }
/** Returns rows drawn per display SLOT (dropped vsyncs are slots with no frame). */
function simulate(o: Sim) {
  const hz = o.hz ?? 60, fps = o.fps ?? 20, secs = o.secs ?? 30, rnd = lcg(o.seed ?? 7);
  const p = new RowPacer();
  if (o.scroll) p.setScroll(o.scroll, o.rowPx ?? 1); else p.setRate(o.rowsPerSec, !!o.sharp, o.rowPx ?? 1);
  const arrivals: number[] = [];
  for (let t = 37.3; t < secs * 1000; t += 1000 / (o.fpsAt ? o.fpsAt(t) : fps)) arrivals.push(t + (rnd() * 2 - 1) * (o.arrJit ?? 0));
  arrivals.sort((a, b) => a - b);
  let ai = 0, have = false;
  const slots: number[] = [];
  const nSlots = Math.floor(secs * hz);
  for (let k = 1; k < nSlots; k++) {
    const ts = k * 1000 / hz + (rnd() * 2 - 1) * (o.frameJit ?? 0);
    while (ai < arrivals.length && arrivals[ai] <= ts) { p.onArrival(arrivals[ai++]); have = true; }
    if (o.dropVsync && rnd() < o.dropVsync) { slots.push(0); continue; }
    slots.push(p.onFrame(ts, have) > 0 ? p.rowsNow : 0);
  }
  const s = slots.slice(hz * 3);                       // settle
  const rows = s.reduce((a, b) => a + b, 0);
  const gaps: number[] = []; let last = -1;
  s.forEach((r, i) => { if (r) { if (last >= 0) gaps.push(i - last); last = i; } });
  const hist: Record<number, number> = {}; for (const g of gaps) hist[g] = (hist[g] || 0) + 1;
  const perEmit: Record<number, number> = {}; for (const r of s) if (r) perEmit[r] = (perEmit[r] || 0) + 1;
  return { rowsPerSec: rows / (s.length / hz), gaps, hist, fpr: p.framesPerRow, perEmit, target: p.screenTarget, slots: s };
}
const describe = (r: ReturnType<typeof simulate>) => `rows/s ${r.rowsPerSec.toFixed(2)} gaps ${JSON.stringify(r.hist)} rows/emission ${JSON.stringify(r.perEmit)} N=${r.fpr}`;
const offCadence = (r: ReturnType<typeof simulate>, n: number) => r.gaps.filter((g) => g !== n).length;

// Realistic: rAF timestamps ±1 ms, arrivals ±12 ms (Pi 500 over Wi-Fi/LAN), 30 s.
const REAL = { frameJit: 1, arrJit: 12 };

// The presets are the APP's (WaterfallView.tsx WF_TARGET_ROWS): Default 20, Smooth 60 SCREEN rows/s;
// the pacer is fed DEVICE rows/s (screen × dpr) and the screen-row height (round(dpr)).
type Case = [name: string, screen: number, dpr: number, hz: number, per: number, n: number];
const CASES: Case[] = [
  ['DEFAULT dpr2 @60  (the reported case)', 20, 2, 60, 2, 3],   // 40 device rows/s: a screen row every 3 frames
  ['DEFAULT dpr2 @120', 20, 2, 120, 1, 3],
  ['DEFAULT dpr1 @60', 20, 1, 60, 1, 3],
  ['SMOOTH dpr2 @60', 60, 2, 60, 2, 1],                          // 120 device rows/s: two a frame
  ['SMOOTH dpr2 @120', 60, 2, 120, 1, 1],
  ['SMOOTH dpr1 @60', 60, 1, 60, 1, 1],
];
for (const [name, screen, dpr, hz, per, n] of CASES) for (const fps of [20, 10, 5]) {
  const r = simulate({ rowsPerSec: screen * dpr, rowPx: dpr, hz, fps, ...REAL });
  const want = screen * dpr;
  ok(`${name}, ${fps} fps feed: ${per} device row(s) every ${n} frame(s), ${want} device rows/s (${screen} screen rows/s)`,
     r.fpr === n && Math.abs(r.rowsPerSec - want) < want * 0.01 + 0.3 && JSON.stringify(Object.keys(r.perEmit)) === JSON.stringify([String(per)]),
     describe(r));
  ok(`${name}, ${fps} fps feed: on cadence (≤0.2 hitches/s)`, offCadence(r, n) / 27 <= 0.2, describe(r));
}
for (const seed of [1, 2, 3, 4, 5]) {
  const r = simulate({ rowsPerSec: 40, rowPx: 2, frameJit: 1.5, arrJit: 20, seed });
  ok(`DEFAULT dpr2 @60, heavy jitter seed ${seed}: ≤0.2 hitches/s`, offCadence(r, 3) / 27 <= 0.2, describe(r));
}
// ── SHARP: one real row per arrival, one SCREEN row tall (the app's one point), on a cadence ──
for (const dpr of [1, 2]) {
  const r = simulate({ rowsPerSec: 40, sharp: true, rowPx: dpr, ...REAL });
  ok(`SHARP dpr${dpr}: 20 screen rows/s from 20 fps`, Math.abs(r.rowsPerSec - 20 * dpr) < 0.3 * dpr, describe(r));
  ok(`SHARP dpr${dpr}: on a 3-frame cadence (≤0.3 hitches/s)`, offCadence(r, 3) / 27 <= 0.3, describe(r));
}
// ── SHARP with arrivals sitting ON a vsync boundary (±2 ms around it): still one row every 3 frames ──
for (const seed of [11, 12, 13]) {
  const hz = 60, p = new RowPacer(); p.setRate(40, true, 2); const rnd = lcg(seed);
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
// ── Dropped vsyncs (the browser missing frames) must not move the cadence ──
{
  const r = simulate({ rowsPerSec: 40, rowPx: 2, dropVsync: 0.05, ...REAL });
  ok('DEFAULT dpr2 @60 with 5 % dropped vsyncs: N stays 3', r.fpr === 3, describe(r));
}
// ── FOLLOW THE FEED (Stuart 10-20, src/assets/wfRowTarget.ts): the web client scrolls exactly as the app ──
for (const [scroll, fps, screen] of [['default', 5, 15], ['default', 10, 20], ['default', 20, 20], ['smooth', 5, 30], ['smooth', 10, 60],
                                     ['smooth', 20, 60], ['sharp', 10, 10], ['default', 10 / 3, 10], ['smooth', 9, 30]] as const) {
  const r = simulate({ rowsPerSec: 0, scroll, rowPx: 2, fps, ...REAL });
  const want = screen * 2, n = Object.keys(r.hist).sort((a, b) => r.hist[+b] - r.hist[+a])[0];
  ok(`FEED ${scroll} @ ${fps.toFixed(2)} fps, dpr 2, 60 Hz: ${screen} screen rows/s, on cadence`,
     Math.abs(r.rowsPerSec - want) < want * 0.02 + 0.3 && offCadence(r, +n) / 27 <= 0.25, describe(r));
}
{
  // A governor step 10 -> 5 fps half way: Smooth 60 -> 30 ONCE, and the scroll keeps going (no frame without a row
  // at 30/s after the change — one row every 2 frames).
  const r = simulate({ rowsPerSec: 0, scroll: 'smooth', rowPx: 2, fps: 10, fpsAt: (ms) => (ms < 15000 ? 10 : 5), ...REAL });
  const tail = r.slots.slice(-60 * 10);   // the last 10 s
  const rows = tail.reduce((a, b) => a + b, 0) / 10;
  ok('governor 10 -> 5 fps: Smooth settles at 30 screen rows/s (60 device), one device row every 2 frames',
     r.target === 30 && Math.abs(rows - 60) < 2, `target ${r.target}, last 10 s ${rows} device rows/s`);
}

// ── emitShape ──
ok('emitShape: 40 device rows/s @60, rowPx 2 → 2 rows every 3 frames', JSON.stringify(emitShape(60, 40, 2)) === '{"per":2,"fpr":3}');
ok('emitShape: 120 @60 → 2 a frame; 120 @120 → 1 a frame', JSON.stringify(emitShape(60, 120, 2)) === '{"per":2,"fpr":1}' && JSON.stringify(emitShape(120, 120, 2)) === '{"per":1,"fpr":1}');
ok('emitShape: 20 @60 → 1 every 3; 40 @120 → 1 every 3', JSON.stringify(emitShape(60, 20, 1)) === '{"per":1,"fpr":3}' && JSON.stringify(emitShape(120, 40, 2)) === '{"per":1,"fpr":3}');

// ── The knife-edge itself ──
ok('59.9 Hz and 60.1 Hz both snap to 60', nominalRefreshHz(59.9) === 60 && nominalRefreshHz(60.1) === 60);
ok('60 Hz / 40 rows/s → N = 2 (ties round to the slower, documented cadence)', framesPerRowFor(60, 40, 0) === 2);
ok('unsnapped 59.5 / 60.5 keep an existing N (hysteresis)', framesPerRowFor(59.5, 40, 2) === 2 && framesPerRowFor(60.5, 40, 2) === 2);
ok('120 Hz / 40 → 3, 120 / 60 → 2', framesPerRowFor(120, 40, 0) === 3 && framesPerRowFor(120, 60, 0) === 2);
ok('a 90 Hz panel / 40 → 2 (2.25 rounds down)', framesPerRowFor(90, 40, 0) === 2);
{
  const p = new RowPacer(); p.setRate(60, false, 1);
  for (let k = 1; k < 200; k++) { const ts = k * 1000 / 60; if (k % 3 === 0) p.onArrival(ts - 1); p.onFrame(ts, true); }
  ok('SMOOTH dpr1 settles N = 1', p.framesPerRow === 1, `N=${p.framesPerRow}`);
  p.setRate(30, false, 1);
  for (let k = 200; k < 260; k++) { const ts = k * 1000 / 60; if (k % 3 === 0) p.onArrival(ts - 1); p.onFrame(ts, true); }
  ok('a slower rate re-decides N = 2 (does not keep the old 1)', p.framesPerRow === 2, `N=${p.framesPerRow}`);
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
