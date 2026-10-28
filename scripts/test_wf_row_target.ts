/**
 * THE WATERFALL ROW TARGET FOLLOWS THE FEED (src/assets/wfRowTarget.ts) — Stuart 10-20: Sharp = actual rows,
 * Default = min(20, 3 x feed), Smooth = min(60, 6 x feed), snapped DOWN to {10, 12, 15, 20, 30, 60}, never below the
 * feed itself. One rule for the app, the web client and the native Android engine (wf_row_target.h — its host test
 * in test_spectrum_engine.cpp runs the same table).
 * Run: node --no-warnings scripts/test_wf_row_target.ts
 */
import { stepRowTarget, newRowTargetState, rowTargetFor, snappedTarget, rowTargetUnderCap, type WfScroll } from '../src/assets/wfRowTarget.ts';
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
// ── Never more than x6, every target divides 60, always above the feed ──
let bad = '';
for (let f = 1; f <= 30; f += 0.25) for (const s of ['default', 'smooth'] as const) {
  const t = rowTargetFor(s, f);
  if (t !== 0 && (60 % t !== 0 || t / f > 6.2 || t <= f)) bad ||= `f=${f} ${s} → ${t}`;
}
ok('every target from 1 to 30 fps divides 60, is at most x6, and is above the feed', !bad, bad);
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
{ const c: [number, number, number, number][] = [[60, 10, 30, 30], [20, 10, 30, 15], [20, 20, 30, 0], [30, 5, 30, 30], [15, 5, 30, 15],
    [12, 3, 30, 10], [60, 10, 60, 60], [20, 10, 0, 20], [20, 10, 120, 20], [0, 20, 30, 0]];
  const bad = c.filter(([t, f, cap, want]) => rowTargetUnderCap(t, f, cap) !== want).map(([t, f, cap, want]) => `${t}@${f}fps cap ${cap} → ${rowTargetUnderCap(t, f, cap)} want ${want}`);
  ok('rowTargetUnderCap: 60 → 30, 20 → 15 (or actual rows), unchanged at 60/120/no cap', bad.length === 0, bad.join('; ')); }
console.log(`wf row target: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
