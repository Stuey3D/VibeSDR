/**
 * THE V12 WATERFALL SPEEDS — MINIMUM · LOW · ECO · NATIVE · [DEFAULT · SMOOTH] (src/assets/wfSpeedCap.ts, Stuart 2026-10-28;
 *  the 2 fps MINIMUM 2026-10-30, GitHub #57 — the old 5 fps MINIMUM is LOW, and a saved 'minimum' loads as LOW).
 *
 *  1. The LINK CAP: LOW 5 / ECO 10 is a CEILING on both link managers (UberSDR's linkManager.ts, VibeServer's
 *     vibeLinkManager.ts) — Auto still steps down under it, Full is capped, Low Data keeps its rate (5 stays 5), lifting
 *     it restores the normal ceiling at once, and on a server already at or below the cap it changes nothing.
 *  2. The ROWS: ECO is exactly 10 rows/s from any slower feed; LOW is actual rows unless the feed is below 5, then 5.
 *  3. THE PACE ON SCREEN is the TARGET, evenly — never a whole multiple of the feed (Stuart: "a 9fps incoming data rate
 *     when in 30fps … 4x interpolation and discard … rather than 3x and sitting at 27fps"). Driven through BOTH clients'
 *     real code with jittered feeds: the app (wfRowTarget carryRows → utils/wfPace rowRate/rowPace at 60 Hz) and the web
 *     (web/client/src/wfPacing RowPacer at 60 Hz).
 *  4. The live numbers on the buttons.
 * Run: node --no-warnings scripts/test_wf_speed_cap.ts
 */
import { capLadder, capFps, capHasEffect, linkCapFor, speedNumbers, uberDivisorFor, isWfSpeedSetting, KIWI_SPEED_CAPS,
         MIN_WF_FPS, LOW_WF_FPS, loadWfSpeed, migrateWfDataRate, migrateLinkMode, offeredWithoutLever, WF_SPEED_LABEL } from '../src/assets/wfSpeedCap.ts';
import { rowTargetFor, stepRowTarget, newRowTargetState, rowsForArrival, type WfScroll } from '../src/assets/wfRowTarget.ts';
import { LinkManager as UberLM, LADDERS } from '../src/services/linkManager.ts';
import { LinkManager as VibeLM } from '../src/services/vibeLinkManager.ts';
import { rowRate, rowPace } from '../src/utils/wfPace.ts';
import { RowPacer } from '../web/client/src/wfPacing.ts';

let fails = 0, passes = 0;
const ok = (what: string, c: boolean, d = '') => { if (c) passes++; else { fails++; console.log(`  FAIL ${what}${d ? `  (${d})` : ''}`); } };
const eq = (a: readonly number[], b: readonly number[]) => a.length === b.length && a.every((x, i) => Math.abs(x - b[i]) < 1e-9);

// ── 1. The pure cap ───────────────────────────────────────────────────────────────────────────────────────────────
ok('MINIMUM cap is 2, LOW 5, ECO 10, the rest none', linkCapFor('min2') === 2 && linkCapFor('low') === 5 && linkCapFor('eco') === 10
   && linkCapFor('sharp') === 0 && linkCapFor('default') === 0 && linkCapFor('smooth') === 0);
ok('2 is the minimum waterfall rate (the floor — no 1 fps); LOW is 5', MIN_WF_FPS === 2 && LOW_WF_FPS === 5);
ok('labels, slowest first: MINIMUM · LOW · ECO · NATIVE · DEFAULT · SMOOTH',
   ['min2', 'low', 'eco', 'sharp', 'default', 'smooth'].map(k => WF_SPEED_LABEL[k as WfScroll]).join(' ') === 'MINIMUM LOW ECO NATIVE DEFAULT SMOOTH');
// ★★ THE MIGRATION: an old saved 'minimum' was the 5 fps choice — it lands on LOW, never silently on the new 2.
ok("migration: a saved 'minimum' (the old 5 fps) loads as LOW, at 5 fps", loadWfSpeed('minimum') === 'low' && linkCapFor(loadWfSpeed('minimum')!) === 5);
ok("migration: the new MINIMUM is stored as 'min2' and loads as itself (2 fps)", loadWfSpeed('min2') === 'min2' && linkCapFor('min2') === 2);
ok("migration: every current value loads unchanged; junk / null → null (keep the default)",
   (['min2', 'low', 'eco', 'sharp', 'default', 'smooth'] as const).every(v => loadWfSpeed(v) === v)
   && loadWfSpeed('half') === null && loadWfSpeed(null) === null && loadWfSpeed(undefined) === null && loadWfSpeed(5) === null);
// ★★ The web's removed WATERFALL DATA RATE row (#57): a saved rate never caps the speed keys again; NATIVE + 5 / 10 is
//    exactly LOW / ECO and is carried over; every other speed is left as the listener chose it.
ok('saved data rate: NATIVE + 5 → LOW, NATIVE + 10 → ECO (the same rows and data)',
   migrateWfDataRate('sharp', 5) === 'low' && migrateWfDataRate('sharp', 10) === 'eco');
ok('saved data rate: NATIVE + AUTO/20 stays NATIVE; DEFAULT / SMOOTH / MINIMUM / ECO + 5 keep their speed (the cap just stops)',
   migrateWfDataRate('sharp', 0) === 'sharp' && migrateWfDataRate('sharp', 20) === 'sharp'
   && migrateWfDataRate('default', 5) === 'default' && migrateWfDataRate('smooth', 10) === 'smooth'
   && migrateWfDataRate('min2', 5) === 'min2' && migrateWfDataRate('eco', 5) === 'eco' && migrateWfDataRate('sharp', 'x') === 'sharp');
// ★★ The app's removed LINK MANAGEMENT row: LOW DATA (a 5 fps pin) migrates as a data rate of 5; FULL / AUTO keep the speed.
ok('saved link mode: LOW DATA on NATIVE → LOW; on DEFAULT stays DEFAULT; FULL / AUTO / junk keep the speed',
   migrateLinkMode('sharp', 'lowData') === 'low' && migrateLinkMode('default', 'lowData') === 'default'
   && migrateLinkMode('sharp', 'full') === 'sharp' && migrateLinkMode('smooth', 'adaptive') === 'smooth' && migrateLinkMode('eco', null) === 'eco');
ok("'minimum' is no longer a CURRENT value (so it is always migrated, never kept)", !isWfSpeedSetting('minimum'));
ok('OpenWebRX (no lever): MINIMUM offered (thinned on the client), LOW / ECO hidden, the rest offered',
   offeredWithoutLever('min2') && !offeredWithoutLever('low') && !offeredWithoutLever('eco')
   && offeredWithoutLever('sharp') && offeredWithoutLever('default') && offeredWithoutLever('smooth'));
ok('VibeServer [20,10,5] under MINIMUM → [2] (fftRate 2), Low Data → 2', eq(capLadder([20, 10, 5], 2, 2).ladder, [2]) && capLadder([20, 10, 5], 2, 2).lowDataRung === 1);
ok('UberSDR [10,5,3.3] under MINIMUM → [2], set_rate ÷5', eq(capLadder([10, 5, 3.3], 2, 2).ladder, [2]) && uberDivisorFor(10, 2) === 5);
ok('VibeServer [20,10,5] under LOW → [5], Low Data rung 1 (5 fps)', eq(capLadder([20, 10, 5], 3, 5).ladder, [5]) && capLadder([20, 10, 5], 3, 5).lowDataRung === 1);
ok('VibeServer [20,10,5] under ECO → [10,5]', eq(capLadder([20, 10, 5], 2, 10).ladder, [10, 5]));
ok('ECO: Low Data at 10 stays 10 (rung 1); at 5 stays 5 (rung 2)',
   capLadder([20, 10, 5], 2, 10).lowDataRung === 1 && capLadder([20, 10, 5], 3, 10).lowDataRung === 2);
ok('UberSDR [10,5,3.3] under LOW → [5,3.3]: Auto keeps its emergency rung', eq(capLadder([10, 5, 3.3], 2, 5).ladder, [5, 3.3]));
ok('UberSDR under LOW: Low Data (5) stays 5 — never 2.5', capLadder([10, 5, 3.3], 2, 5).ladder[capLadder([10, 5, 3.3], 2, 5).lowDataRung - 1] === 5);
ok('UberSDR under ECO → unchanged (its top is 10)', eq(capLadder([10, 5, 3.3], 2, 10).ladder, [10, 5, 3.3]));
ok('no cap → ladder untouched', eq(capLadder([20, 10, 5], 2, 0).ladder, [20, 10, 5]));
ok('a cap is a ceiling: capFps(20,5)=5, capFps(5,5)=5, capFps(3,5)=3, capFps(20,0)=20',
   capFps(20, 5) === 5 && capFps(5, 5) === 5 && capFps(3, 5) === 3 && capFps(20, 0) === 20);
ok('LOW has no effect on a 5 fps server; ECO none on a 10 or 5', !capHasEffect(5, 5) && !capHasEffect(10, 10) && !capHasEffect(10, 5)
   && capHasEffect(5, 20) && capHasEffect(10, 20));
ok('UberSDR divisor from the rung FPS: 10→1, 5→2, 3.3→3', uberDivisorFor(10, 10) === 1 && uberDivisorFor(10, 5) === 2 && uberDivisorFor(10, 3.3) === 3);
ok("a saved 'sharp' still loads (NATIVE's stored value)", isWfSpeedSetting('sharp') && isWfSpeedSetting('low') && isWfSpeedSetting('min2') && isWfSpeedSetting('eco') && !isWfSpeedSetting('half'));

// ── 1b. The link managers, both of them ──────────────────────────────────────────────────────────────────────────
for (const [name, LM, ladder] of [['UberSDR', UberLM, LADDERS.ubersdr], ['VibeServer', VibeLM, LADDERS.vibeserver]] as const) {
  const sent: number[] = [];
  const mk = (mode: 'full' | 'adaptive' | 'lowData', cap = 0) =>
    new LM({ ladder: [...ladder], lowDataRung: 2, mode, userCapFps: cap, apply: (_r: number, f: number) => sent.push(f) });
  // FULL under LOW asks 5 every tick; lifting asks the top at once.
  { sent.length = 0; const m = mk('full', 5); m.tick(5, true, true);
    ok(`${name}: FULL under LOW asks 5`, sent[sent.length - 1] === 5, `sent ${sent}`);
    m.setUserCap(0); ok(`${name}: lifting LOW in FULL asks ${ladder[0]} at once`, sent[sent.length - 1] === ladder[0], `sent ${sent}`);
    m.setUserCap(10); ok(`${name}: FULL under ECO asks ${Math.min(10, ladder[0])}`, sent[sent.length - 1] === Math.min(10, ladder[0]), `sent ${sent}`); }
  // LOW DATA: unchanged by LOW (5 stays 5) — and never below 5.
  { sent.length = 0; const m = mk('lowData'); m.tick(0, true, true); const before = sent[sent.length - 1];
    m.setUserCap(5); m.tick(0, true, true); const after = sent[sent.length - 1];
    ok(`${name}: LOW DATA under LOW ≥ 5 and ≤ its own rate (${before} → ${after})`, after >= 5 && after <= before && after === Math.min(before, 5)); }
  // AUTO under LOW: starts ≤ 5, never climbs above it, and on UberSDR still steps DOWN to 3.3 on a starving link.
  { sent.length = 0; const m = mk('adaptive', 5);
    let max = 0;
    for (let i = 0; i < 60; i++) { m.tick(5, true, true); max = Math.max(max, m.expectedFps); }
    ok(`${name}: AUTO under LOW never above 5 on a healthy link`, max <= 5, `max ${max}`);
    for (let i = 0; i < 12; i++) m.tick(1, true, true);
    const floor = ladder[ladder.length - 1];
    ok(`${name}: AUTO under LOW still steps down on a poor link (→ ${floor < 5 ? floor : 5})`, m.expectedFps === Math.min(5, floor), `at ${m.expectedFps}`); }
  // ★ MINIMUM (2): FULL, LOW DATA and AUTO all ask 2 — one rung, nothing below it to step to.
  { sent.length = 0; const m = mk('full', 2); m.tick(2, true, true);
    ok(`${name}: FULL under MINIMUM asks 2`, sent[sent.length - 1] === 2, `sent ${sent}`);
    m.setUserCap(5); ok(`${name}: MINIMUM → LOW in FULL asks 5 at once`, sent[sent.length - 1] === 5, `sent ${sent}`); }
  { sent.length = 0; const m = mk('lowData'); m.setUserCap(2); m.tick(0, true, true);
    ok(`${name}: LOW DATA under MINIMUM asks 2`, sent[sent.length - 1] === 2, `sent ${sent}`); }
  { const m = mk('adaptive', 2); let max = 0;
    for (let i = 0; i < 40; i++) { m.tick(2, true, true); max = Math.max(max, m.expectedFps); }
    for (let i = 0; i < 12; i++) m.tick(0, true, true);
    ok(`${name}: AUTO under MINIMUM holds 2, healthy or starved`, max === 2 && m.expectedFps === 2, `max ${max} now ${m.expectedFps}`); }
  // AUTO under ECO: never above 10 healthy; steps down to 5 on a poor link.
  { const m = mk('adaptive', 10); let max = 0;
    for (let i = 0; i < 60; i++) { m.tick(m.expectedFps, true, true); max = Math.max(max, m.expectedFps); }
    ok(`${name}: AUTO under ECO climbs to ${Math.min(10, ladder[0])}, never above`, max === Math.min(10, ladder[0]), `max ${max}`);
    for (let i = 0; i < 12; i++) m.tick(1, true, true);
    ok(`${name}: AUTO under ECO steps down on a poor link`, m.expectedFps < Math.min(10, ladder[0]), `at ${m.expectedFps}`); }
  // The owner's ceiling still applies, and a cap at/above it changes nothing (a 5 fps server ignores LOW).
  { sent.length = 0; const m = mk('full'); m.applyServerCeiling(5); const n = sent.length;
    m.setUserCap(5); m.tick(5, true, true);
    ok(`${name}: on a 5 fps server LOW has no effect (still 5)`, sent[sent.length - 1] === 5 && m.rungs[0] === 5, `rungs ${m.rungs} sent ${sent.slice(n)}`);
    ok(`${name}: nativeFps under a 5 fps owner ceiling is 5`, m.nativeFps === 5); }
  { const m = mk('full'); ok(`${name}: nativeFps is the top rung (${ladder[0]}) whatever the cap`, m.nativeFps === ladder[0] && (m.setUserCap(5), m.nativeFps === ladder[0])); }
}

// ── 2. Row targets ────────────────────────────────────────────────────────────────────────────────────────────────
const rt = (s: WfScroll, f: number) => { const t = rowTargetFor(s, f); return t > 0 ? t : f; };
for (const [f, want] of [[20, 10], [13, 10], [10, 10], [9, 10], [7, 10], [6, 10], [5, 10], [10 / 3, 10], [2, 10]] as const)
  ok(`ECO at ${f.toFixed(1)} fps → ${want} rows/s`, Math.abs(rt('eco', f) - want) < 0.01, `got ${rt('eco', f)}`);
// (★ 2026-10-29: a feed the link cap could not bring down — Kiwi's ~13 fps ECO step — is THINNED to 10.)
for (const [f, want] of [[20, 5], [5, 5], [10 / 3, 5], [2, 5]] as const)
  ok(`LOW at ${f.toFixed(1)} fps → ${want} rows/s`, Math.abs(rt('low', f) - want) < 0.01, `got ${rt('low', f)}`);
ok('LOW on a 5 fps feed is actual rows (same as NATIVE)', rowTargetFor('low', 5) === 0);
// ★ MINIMUM: 2 rows/s from ANY feed — a server that could not be asked for 2 (OWRX ~9, Kiwi 5, UberSDR shared ÷3) thinned.
for (const [f, want] of [[23, 2], [20, 2], [9, 2], [5, 2], [10 / 3, 2], [2, 2], [1, 2]] as const)
  ok(`MINIMUM at ${f.toFixed(1)} fps → ${want} rows/s`, Math.abs(rt('min2', f) - want) < 0.01, `got ${rt('min2', f)}`);
ok('MINIMUM on a 2 fps feed is actual rows (the server sends exactly 2 — nothing to combine)', rowTargetFor('min2', 2) === 0);
ok('Default / Smooth: 9 fps → 30 / 30 (Default never 20 — Stuart 2026-10-29)', rt('default', 9) === 30 && rt('smooth', 9) === 30);

// ── 3. The pace on screen: the TARGET, evenly, both clients ───────────────────────────────────────────────────────
function lcg(seed: number) { let s = seed; return () => { s = (s * 1103515245 + 12345) & 0x7fffffff; return s / 0x7fffffff; }; }
/** Arrival times for a feed of `fps` with ±`jit` ms network jitter. */
function feed(fps: number, ms: number, jit: number, seed = 7): number[] {
  const rnd = lcg(seed), out: number[] = [];
  for (let k = 0, t = 10; t < ms; k++, t = 10 + k * 1000 / fps + (rnd() - 0.5) * 2 * jit) out.push(t);
  return out.sort((a, b) => a - b);
}
/** THE APP: WaterfallView handleFrame (the EMA'd feed → stepRowTarget → rowsForArrival → extra rows spread over the
 *  interval; a discarded frame pushes nothing) into the display driver (rowRate + rowPace, settled) at `hz`. Returns the display-frame index of each step. */
function appRun(scroll: WfScroll, fps: number, jit: number, hz = 60, ms = 16000) {
  const arr = feed(fps, ms, jit);
  const st = newRowTargetState(), carry = { current: 0 };
  let avgFrameMs = 150, avgGap = 0, last = 0;
  const pushes: number[] = [];                       // times rows land in the ring
  const tgts: [number, number][] = [];               // (arrival, target) — WaterfallView sets pTgt per arrival
  for (const t of arr) {
    if (last > 0) { const d = t - last; avgFrameMs = avgFrameMs * 0.8 + d * 0.2; avgGap = avgGap > 0 ? avgGap * 0.8 + d * 0.2 : d; }
    last = t;
    const target = stepRowTarget(st, scroll, 1000 / avgFrameMs);
    tgts.push([t, target]);
    const dur = Math.max(15, Math.min(1000, avgGap > 0 ? avgGap : avgFrameMs));
    const rows = rowsForArrival(carry, dur, target);   // ★ 2026-10-29: 0 = the frame is discarded (thinned)
    if (rows > 0) pushes.push(t);
    const extras = Math.max(0, rows - 1);
    for (let k = 1; k <= extras; k++) pushes.push(t + k * dur / (extras + 1));   // scheduleExtraRows
  }
  pushes.sort((a, b) => a - b);
  const sv = (v: number) => ({ value: v });
  const shown = sv(0), acc = sv(0), lead = sv(0), cool = sv(0), mode = sv(0), rate = sv(0.03), rT = sv(0), rN = sv(0);
  const gap = 1000 / hz, steps: { f: number; n: number }[] = [];
  let pi = 0, prev = 0, ti = 0, tgt = 0;
  for (let f = 0; f * gap < ms; f++) {
    const t = f * gap;
    while (pi < pushes.length && pushes[pi] <= t) pi++;
    while (ti < tgts.length && tgts[ti][0] <= t) tgt = tgts[ti++][1];
    rowRate(pi, t, rate, rN, rT, false);
    // paceTick: the TARGET when filling in (pTgt), else the measured rate
    const s = rowPace(pi, shown, acc, lead, cool, mode, tgt > 0 ? tgt / 1000 : rate.value, gap, true);
    if (s !== prev) { steps.push({ f, n: s - prev }); prev = s; }
  }
  return steps;
}
/** THE WEB: RowPacer (dpr 1) at `hz`. Returns the display-frame index of each emission. */
function webRun(scroll: WfScroll, fps: number, jit: number, hz = 60, ms = 16000) {
  const arr = feed(fps, ms, jit), p = new RowPacer(); p.setScroll(scroll, 1);
  const steps: { f: number; n: number }[] = []; let ai = 0, have = false;
  for (let f = 1; f * 1000 / hz < ms; f++) {
    const ts = f * 1000 / hz;
    while (ai < arr.length && arr[ai] <= ts) { p.onArrival(arr[ai++]); have = true; }
    if (p.onFrame(ts, have) > 0) steps.push({ f, n: 1 });
  }
  return steps;
}
/** Rows/s over the settled window (after 4 s) and how even the steps are: the share of gaps that are the modal gap. */
function judge(steps: { f: number; n: number }[], hz = 60) {
  const s = steps.filter(x => x.f / hz >= 4);
  const rows = s.reduce((a, x) => a + x.n, 0), secs = (s[s.length - 1].f - s[0].f) / hz;
  const gaps = s.slice(1).map((x, i) => x.f - s[i].f), hist = new Map<number, number>();
  for (const g of gaps) hist.set(g, (hist.get(g) ?? 0) + 1);
  const modal = Math.max(...hist.values()) / Math.max(1, gaps.length);
  return { rate: (rows - s[0].n) / secs, even: modal };
}
const cases: [WfScroll, number, number][] = [
  ['smooth', 9, 30], ['default', 9, 30], ['eco', 9, 10],
  ['eco', 5, 10], ['eco', 10 / 3, 10], ['default', 10 / 3, 10], ['smooth', 5, 30],
  ['low', 2, 5], ['low', 10 / 3, 5], ['default', 20, 30], ['default', 10, 30],
  // ★ 2026-10-29 — THINNED and FILLED to a divisor of 60 (Stuart, a 23 fps Kiwi): ECO's ~13 fps Kiwi step → 10, LOW
  //   on a 6 fps feed → 5, Default on 23 → 30, Smooth on 23 → 60, Default on 22 → 30.
  ['eco', 13, 10], ['low', 6.5, 5], ['default', 23, 30], ['smooth', 23, 60], ['default', 22, 30],
  // ★ 2026-10-30 MINIMUM: thinned to 2 from a Kiwi's 5, an UberSDR shared 3.3, an OpenWebRX 9, a 23 fps Kiwi.
  ['min2', 5, 2], ['min2', 10 / 3, 2], ['min2', 9, 2], ['min2', 23, 2],
];
for (const [scroll, fps, want] of cases) {
  for (const [who, run] of [['app', appRun], ['web', webRun]] as const) {
    const j = judge(run(scroll, fps, 12));
    ok(`${who}: ${scroll} on a ${fps.toFixed(1)} fps jittery feed scrolls at ${want} rows/s (got ${j.rate.toFixed(2)})`,
       Math.abs(j.rate - want) / want < 0.03);
    // ★ MINIMUM on the APP model here: this harness drives the per-arrival path (rowsForArrival — the app's path only when
    //   it is off the fixed clock), where 2 kept frames out of 5 / 9 / 23 are 2 or 3 frames apart by construction. The
    //   app's live path for every mode but NATIVE is the fixed clock — even ticks, proved for MINIMUM in
    //   scripts/test_wf_fixed_clock.ts. Rate asserted here; evenness on the web (which runs the clock) and there.
    if (!(who === 'app' && scroll === 'min2'))
      ok(`${who}: ${scroll} on ${fps.toFixed(1)} fps is evenly paced (${(j.even * 100).toFixed(0)} % of steps the same gap)`, j.even >= 0.9);
  }
}

// ECO on a 10 fps feed is ACTUAL ROWS (NATIVE's path — nothing filled in), so its rate is the feed's. ★ Its evenness is
// NATIVE's and is not asserted here: on the app, a 10 fps feed with ±12 ms jitter holds the 6-frame beat on ~2/3 of steps
// (the measured-rate pacer, unchanged by this work — the web's RowPacer holds it). Reported, not hidden.
for (const [who, run] of [['app', appRun], ['web', webRun]] as const) {
  const j = judge(run('eco', 10, 12));
  ok(`${who}: eco on a 10 fps feed is actual rows at 10/s (got ${j.rate.toFixed(2)}, ${(j.even * 100).toFixed(0)} % on the beat)`, Math.abs(j.rate - 10) / 10 < 0.03);
}

// ── 4. The live numbers ──────────────────────────────────────────────────────────────────────────────────────────
const n20 = speedNumbers(20)!, n5 = speedNumbers(5)!, n2 = speedNumbers(2)!;
ok('20 fps server: MINIMUM 2 · LOW 5 · ECO 10 · NATIVE 20 · DEFAULT 30 · SMOOTH 60',
   n20.min2 === 2 && n20.low === 5 && n20.eco === 10 && n20.sharp === 20 && n20.default === 30 && n20.smooth === 60, JSON.stringify(n20));
ok('5 fps server: LOW 5 · ECO 10 · NATIVE 5 · DEFAULT 15 · SMOOTH 30',
   n5.low === 5 && n5.eco === 10 && n5.sharp === 5 && n5.default === 15 && n5.smooth === 30, JSON.stringify(n5));
ok('2 fps server: LOW 5 (filled in) · NATIVE 2', n2.low === 5 && n2.sharp === 2, JSON.stringify(n2));
ok('unknown feed → no numbers', speedNumbers(0) === null);
{ const k = speedNumbers(23, KIWI_SPEED_CAPS)!;
  ok('Kiwi (23 fps, ECO step ≈13): MINIMUM 2 (5 thinned) · LOW 5 · ECO 10 (thinned) · NATIVE 23 · DEFAULT 30 · SMOOTH 60',
     k.min2 === 2 && k.low === 5 && k.eco === 10 && k.sharp === 23 && k.default === 30 && k.smooth === 60, JSON.stringify(k)); }
// every button but NATIVE shows a divisor of 60, on every client type's own rate (Kiwi ~23, UberSDR 10, OWRX ~9, VibeServer 20/21/30)
for (const [name, f, caps] of [['Kiwi', 23, KIWI_SPEED_CAPS], ['UberSDR', 10, undefined], ['OpenWebRX', 9, undefined],
                               ['VibeServer', 20, undefined], ['VibeServer (Lenovo)', 21, undefined], ['VibeServer', 30, undefined]] as const) {
  const k = speedNumbers(f, caps)!;
  ok(`${name} at ${f} fps: every number but NATIVE divides 60 — ${JSON.stringify(k)}`,
     [k.min2, k.low, k.eco, k.default, k.smooth].every(v => 60 % v === 0) && k.sharp === f);
}

console.log(`${fails ? 'FAIL' : 'PASS'}: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
