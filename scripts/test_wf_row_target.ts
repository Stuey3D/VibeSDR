/**
 * THE WATERFALL ROW TARGET FOLLOWS THE FEED (src/assets/wfRowTarget.ts) — Stuart 10-20: Sharp = actual rows,
 * Default = min(20, 3 x feed), Smooth = min(60, 6 x feed), snapped DOWN to {10, 12, 15, 20, 30, 60}, never below the
 * feed itself. One rule for the app, the web client and the native Android engine (wf_row_target.h — its host test
 * in test_spectrum_engine.cpp runs the same table).
 * Run: node --no-warnings scripts/test_wf_row_target.ts
 */
import { stepRowTarget, newRowTargetState, rowTargetFor, snappedTarget, rowTargetUnderCap, nominalRowRate, rowsForArrival, type WfScroll } from '../src/assets/wfRowTarget.ts';
let fails = 0, passes = 0;
const ok = (what: string, c: boolean, d = '') => { if (c) passes++; else { fails++; console.log(`  FAIL ${what}${d ? `  (${d})` : ''}`); } };
const shown = (t: number, fps: number) => (t === 0 ? fps : t);   // 0 = actual rows

/** Drive a waterfall with frame gaps from `gapAt(i)`, through the caller's EMA (0.8 / 0.2, as WaterfallView and the
 *  native engine; starting at their 150 ms). Returns every target it emitted. */
function drive(scroll: WfScroll, frames: number, gapAt: (i: number) => number, st = newRowTargetState(), ema = { ms: 150 }) {
  const out: number[] = [];
  for (let i = 0; i < frames; i++) { ema.ms = ema.ms * 0.8 + gapAt(i) * 0.2; out.push(stepRowTarget(st, scroll, 1000 / ema.ms)); }
  return { out, st, ema, last: out[out.length - 1], changes: out.filter((t, i) => i > 0 && t !== out[i - 1]).length };
}

// ── The table, every row — from a cold start (the 150 ms estimate), as a real session begins ──
const TABLE: [string, number, number, number, number][] = [
  ['5 fps', 5, 5, 15, 30],
  ['10 fps', 10, 10, 20, 60],
  ['20 fps', 20, 20, 20, 60],
  ['UberSDR 3.33 fps', 10 / 3, 10 / 3, 10, 20],
  ['OWRX ~9 fps', 9, 9, 20, 30],
];
for (const [label, fps, sh, de, sm] of TABLE) {
  const g = () => 1000 / fps;
  const r = { sharp: shown(drive('sharp', 200, g).last, fps), def: shown(drive('default', 200, g).last, fps), smo: shown(drive('smooth', 200, g).last, fps) };
  ok(`${label}: ${sh.toFixed(2)} / ${de} / ${sm}`, Math.abs(r.sharp - sh) < 1e-9 && r.def === de && r.smo === sm, JSON.stringify(r));
}
ok('20 fps Default is ACTUAL rows (x1), not a repeat to 20', rowTargetFor('default', 20) === 0 && rowTargetFor('default', 19.8) === 0);
ok('Sharp is always actual rows', [2, 5, 10, 20, 30].every(f => rowTargetFor('sharp', f) === 0));
ok('the 2 fps idle floor: Default is actual rows (3 x 2 < 10) …', rowTargetFor('default', 2) === 0);
ok('… and Smooth gives 12 (6 x 2), a divisor of 60', rowTargetFor('smooth', 2) === 12);
// ── Never more than x6, every target divides 60; Default/Smooth never below the feed (they fill, never thin) ──
let bad = '';
for (let f = 1; f <= 60; f += 0.25) for (const s of ['default', 'smooth'] as const) {
  const t = rowTargetFor(s, f);
  if (t !== 0 && (60 % t !== 0 || t / f > 6.2 || t <= f)) bad ||= `f=${f} ${s} → ${t}`;
}
ok('every Default/Smooth target from 1 to 60 fps divides 60, is at most x6, and is above the feed', !bad, bad);
// ★★★ 2026-10-29 (Stuart, a 23 fps Kiwi: "interpolate and discard") — every rate but NATIVE divides 60, FILLED or THINNED.
{ const want: [number, WfScroll, number][] = [
    // feed, choice, rows/s on screen (0 = actual rows)
    [23, 'default', 30], [23, 'smooth', 60], [23, 'eco', 10], [23, 'minimum', 5], [23, 'sharp', 0],
    [13, 'eco', 10], [13, 'default', 20], [13, 'smooth', 60], [21, 'default', 30], [20.3, 'default', 0], [25, 'default', 30],
    [30, 'default', 0], [40, 'default', 60], [40, 'smooth', 60], [75, 'smooth', 60], [75, 'default', 60],
    [5.6, 'minimum', 5], [12, 'eco', 10], [10.2, 'eco', 0], [10.6, 'eco', 10], [4.4, 'minimum', 5], [2, 'default', 0]];
  const b = want.filter(([f, s, w]) => rowTargetFor(s, f) !== w).map(([f, s, w]) => `${f} fps ${s} → ${rowTargetFor(s, f)} want ${w}`);
  ok('the snap table: Eco/Minimum thin a faster feed to 10/5; Default/Smooth go UP to the next divisor of 60; Native as it comes', !b.length, b.join('; '));
  let nd = '';
  for (let f = 1; f <= 80; f += 0.5) for (const s of ['minimum', 'eco', 'default', 'smooth'] as const) {
    const t = nominalRowRate(s, f); if (t > 0 && 60 % t !== 0) nd ||= `${f} ${s} → ${t}`;
  }
  ok('every choice but NATIVE runs at a divisor of 60 (5…60) on any feed from 1 to 80 fps', !nd, nd); }
// ── Discard and fill counts (rowsForArrival): the average is the rate, the drops are EVEN ──
{ const count = (feed: number, target: number, frames = 2300) => {
    const c = { current: 0 }, k: number[] = [];
    for (let i = 0; i < frames; i++) k.push(rowsForArrival(c, 1000 / feed, target));
    return { rows: k.reduce((a, b) => a + b, 0), dropped: k.filter(x => x === 0).length, k };
  };
  const a = count(23, 20);   // 100 s of a 23 fps feed thinned to 20
  ok('thinning 23 → 20 rows/s: 2000 rows from 2300 frames (300 discarded)', Math.abs(a.rows - 2000) <= 1 && Math.abs(a.dropped - 300) <= 1, `${a.rows} rows, ${a.dropped} dropped`);
  let run = 0, maxRun = 0; for (const x of a.k) { run = x === 0 ? 0 : run + 1; maxRun = Math.max(maxRun, run); }
  const gaps: number[] = []; a.k.forEach((x, i) => { if (x === 0) gaps.push(i); });
  const spacing = gaps.slice(1).map((g, i) => g - gaps[i]);
  ok('…evenly: a discard every 7 or 8 frames, never two in a row', spacing.every(s => s === 7 || s === 8), `${[...new Set(spacing)]}`);
  const e = count(13, 10, 1300);
  ok('Eco on a 13 fps Kiwi: 1000 rows from 1300 frames', Math.abs(e.rows - 1000) <= 1 && Math.abs(e.dropped - 300) <= 1, `${e.rows} rows, ${e.dropped} dropped`);
  const d = count(23, 30);
  ok('Default 30 from 23 fps: 3000 rows from 2300 frames, none discarded, at most 2 a frame', Math.abs(d.rows - 3000) <= 1 && d.dropped === 0 && Math.max(...d.k) === 2, `${d.rows} ${d.dropped} ${Math.max(...d.k)}`);
  const n = count(23, 0);
  ok('Native (target 0): every frame, one row', n.rows === 2300 && n.dropped === 0); }
// ── Hysteresis ──
{
  const r = drive('smooth', 600, i => (i < 100 ? 100 : i % 2 ? 111 : 100));
  // Its true mean is 9.5 fps (6 x 9.5 = 57 → 30), so ONE change to 30 is right; flapping back to 60 is not.
  ok('a feed wobbling 9 <-> 10 fps frame to frame changes Smooth at most once, then holds', r.out.slice(100).filter((t, i, a) => i > 0 && t !== a[i - 1]).length <= 1 && new Set(r.out.slice(160)).size === 1, ` targets ${[...new Set(r.out.slice(100))]}`);
  const r2 = drive('smooth', 600, i => (i < 100 ? 100 : 100 + 11 * (Math.floor(i / 7) % 2)));
  ok('… nor in slow runs of 9 and 10 (7 frames each)', new Set(r2.out.slice(150)).size === 1, `targets ${[...new Set(r2.out.slice(150))]}`);
}
for (const [s, from, to, a, b] of [['smooth', 10, 5, 60, 30], ['default', 10, 5, 20, 15], ['default', 20, 10, 0, 20], ['smooth', 5, 10, 30, 60], ['default', 5, 10, 15, 20]] as const) {
  const first = drive(s, 200, () => 1000 / from);
  const r = drive(s, 400, () => 1000 / to, first.st, first.ema);
  ok(`a governor step ${from} -> ${to} fps changes ${s} ONCE, ${a || 'actual'} -> ${b || 'actual'}`,
     first.last === a && r.last === b && r.changes + (r.out[0] !== first.last ? 1 : 0) === 1, `ends ${r.last}, changes ${r.changes}`);
}
{
  const r = drive('default', 400, i => 1000 / (17 + (i % 3)));
  ok('a feed wobbling 17-19 fps holds one Default target', new Set(r.out.slice(100)).size === 1, `${[...new Set(r.out.slice(100))]}`);
}
ok('snappedTarget: 3 x 5 = 15, 6 x 9 = 54 → 30, 6 x 3.33 = 20', snappedTarget('default', 5) === 15 && snappedTarget('smooth', 9) === 30 && snappedTarget('smooth', 10 / 3) === 20);
// ★ Under a FRAME RATE cap below 60 the target divides the cap (Stuart 2026-10-28: "in 30fps lock mode no point running a 60fps waterfall")
{ const c: [number, number, number, number][] = [[60, 10, 30, 30], [20, 10, 30, 15], [20, 20, 30, 15], [30, 5, 30, 30], [15, 5, 30, 15],
    [12, 3, 30, 10], [60, 10, 60, 60], [20, 10, 0, 20], [20, 10, 120, 20], [0, 20, 30, 0]];
  const bad = c.filter(([t, f, cap, want]) => rowTargetUnderCap(t, f, cap) !== want).map(([t, f, cap, want]) => `${t}@${f}fps cap ${cap} → ${rowTargetUnderCap(t, f, cap)} want ${want}`);
  ok('rowTargetUnderCap: 60 → 30, 20 → 15 (a 20 fps feed thinned to 15 — 2026-10-29), unchanged at 60/120/no cap', bad.length === 0, bad.join('; ')); }
console.log(`wf row target: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
