/**
 * THE BOOST TAPERS OUT (src/assets/wfClock.ts boostTaper, Stuart 2026-10-31: "the sudden FPS drop from 120→30 is causing
 * what appears to be a slight pause"). Drives the boost's state the way WaterfallView's pumpClock / onFrame do (a move,
 * BOOST_TAIL_MS, then the taper) and checks:
 *   • the rate never drops more than 2× in one step, and every step is a rate the panel draws on whole frames;
 *   • each rung lasts at least a few rows; the taper lands on the base within BOOST_TAPER_MS;
 *   • an input during the taper is the boost again at once (no fight), and a rest with no boost is never touched;
 *   • NATIVE (base 0) and SPEED UP WHILE TUNING off (boosted = base) are never tapered.
 * WF_TAPER_MS=<ms> sweeps another length (prints the rungs). Run: node --no-warnings scripts/test_wf_boost_taper.ts
 */
import { BOOST_TAIL_MS, BOOST_TAPER_MS, boostActive, boostNote, boostRowRate, boostTaper, newBoost } from '../src/assets/wfClock.ts';

let passes = 0, fails = 0;
const ok = (what: string, c: boolean) => { if (c) passes++; else { fails++; console.log(`  FAIL ${what}`); } };

/** One input at t=0; the boost's rate sampled every ms until 3 s. Returns the runs [rate, from, to]. */
function run(base: number, feedFps: number, hz: number, inputs: number[] = [0], enabled = true) {
  const b = newBoost(); const runs: [number, number, number][] = [];
  const T0 = 1000;   // (the boost's clock: 0 means "never moved")
  for (let t = 0; t < 3000; t++) {
    if (inputs.includes(t)) boostNote(b, T0 + t);
    boostActive(b, T0 + t, false);
    const r = boostTaper(b, base, boostRowRate(base, feedFps, hz, enabled), T0 + t, hz);
    if (!runs.length || runs[runs.length - 1][0] !== r) runs.push([r, t, t]); else runs[runs.length - 1][2] = t;
  }
  return runs;
}
const CASES = [
  { name: '120 Hz Mac/ProMotion, Default 30, 10 fps', base: 30, fps: 10, hz: 120 },
  { name: '120 Hz, Default 15 (5 fps feed)', base: 15, fps: 5, hz: 120 },
  { name: '60 Hz phone, Default 30, 10 fps', base: 30, fps: 10, hz: 60 },
  { name: '60 Hz, Default 15, 5 fps', base: 15, fps: 5, hz: 60 },
  { name: '120 Hz, Smooth 60, 20 fps', base: 60, fps: 20, hz: 120 },
  { name: '120 Hz, Eco 10, 10 fps', base: 10, fps: 10, hz: 120 },
];
for (const c of CASES) {
  const runs = run(c.base, c.fps, c.hz);
  const boosted = boostRowRate(c.base, c.fps, c.hz);
  console.log(`${c.name.padEnd(42)} ${runs.map(([r, a, z]) => `${r}@${a}${z < 2999 ? `-${z}` : '…'}`).join('  ')}`);
  const whole = c.hz >= 119 ? 120 : 60;
  let worst = 1, shortest = Infinity;
  for (let k = 1; k < runs.length; k++) worst = Math.max(worst, runs[k - 1][0] / runs[k][0]);
  for (let k = 1; k < runs.length - 1; k++) shortest = Math.min(shortest, (runs[k][2] - runs[k][1] + 1) * runs[k][0] / 1000);
  ok(`${c.name}: boosted to ${boosted} during the move`, runs[0][0] === boosted);
  ok(`${c.name}: no step down bigger than 2× (worst ${worst.toFixed(2)}×)`, worst <= 2 + 1e-9);
  ok(`${c.name}: every rate draws on whole frames`, runs.every(([r]) => whole % r === 0));
  if (runs.length > 2) ok(`${c.name}: every rung lasts ≥ 4 rows (shortest ${shortest.toFixed(1)})`, shortest >= 3.95);
  const end = runs[runs.length - 1];
  ok(`${c.name}: lands on the base (${c.base}) by ${BOOST_TAIL_MS + BOOST_TAPER_MS} ms (at ${end[1]})`, end[0] === c.base && end[1] <= BOOST_TAIL_MS + BOOST_TAPER_MS + 1);
  ok(`${c.name}: the boost held through its tail (${runs[0][2] + 1} ms)`, runs[0][2] + 1 === BOOST_TAIL_MS);
}
// an input during the taper: straight back to the boost, then a fresh taper
{
  const runs = run(30, 10, 120, [0, BOOST_TAIL_MS + 200]);
  const at = runs.find(([, a]) => a === BOOST_TAIL_MS + 200);
  ok(`an input in the taper is the boost again at once (${at?.[0]} at ${BOOST_TAIL_MS + 200} ms)`, at?.[0] === 120);
  ok('…and tapers again after its own tail', runs[runs.length - 1][0] === 30 && runs[runs.length - 1][1] <= BOOST_TAIL_MS + 200 + BOOST_TAIL_MS + BOOST_TAPER_MS + 1);
}
ok('NATIVE (base 0) is never boosted or tapered', run(0, 10, 120).every(([r]) => r === 0));
ok('SPEED UP WHILE TUNING off: the base throughout', run(30, 10, 120, [0], false).every(([r]) => r === 30));
ok('no input: the base throughout', run(30, 10, 120, []).every(([r]) => r === 30));
console.log(`wf boost taper: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
