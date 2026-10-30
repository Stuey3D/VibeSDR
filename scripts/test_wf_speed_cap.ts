/**
 * THE V12 WATERFALL SPEEDS (display only) AND THE DATA CONTROL (src/assets/wfSpeedCap.ts — Stuart 2026-10-30: "we cannot
 * slow the incoming data rate down automatically when the waterfall is slowed down as that will effect the spectrum too
 * … we need a separate data control but dont mention frame rates, just call it low auto full").
 *
 *  1. The speed keys: labels, the 'minimum' → LOW migration, and THEY NEVER TOUCH THE LINK (the app, the web and the
 *     Kiwi lever read the DATA control only — checked in the source).
 *  1b. DATA · LOW · AUTO · FULL on both link managers (UberSDR's linkManager.ts, VibeServer's vibeLinkManager.ts — the
 *     web runs the latter too): LOW = the 5 fps floor, FULL = the top, AUTO climbs on a good link and steps down on a
 *     poor one but NEVER below 5. Kiwi: LOW = wf_speed 2, AUTO/FULL = 4. OpenWebRX: no control. Saved values map.
 *  2. The ROWS: each speed's rate from any feed (a faster one combined down — wfCombine.ts — a slower one filled in).
 *  3. THE PACE ON SCREEN is the TARGET, evenly, through both clients' real code with jittered feeds.
 *  4. The live numbers on the keys (from the rate the server sends).
 * Run: node --no-warnings scripts/test_wf_speed_cap.ts
 */
import { readFileSync } from 'node:fs';
import { capLadder, speedNumbers, uberDivisorFor, isWfSpeedSetting, MIN_WF_FPS, DATA_FLOOR_FPS, loadWfSpeed, WF_SPEED_LABEL,
         DATA_LABEL, DATA_NOTE, linkModeFor, dataModeFor, dataModeFromWebRate, lowDataRungFor, floorLadder } from '../src/assets/wfSpeedCap.ts';
import { rowTargetFor, stepRowTarget, newRowTargetState, rowsForArrival, type WfScroll } from '../src/assets/wfRowTarget.ts';
import { LinkManager as UberLM, LADDERS } from '../src/services/linkManager.ts';
import { LinkManager as VibeLM } from '../src/services/vibeLinkManager.ts';
import { rowRate, rowPace } from '../src/utils/wfPace.ts';
import { RowPacer } from '../web/client/src/wfPacing.ts';

let fails = 0, passes = 0;
const ok = (what: string, c: boolean, d = '') => { if (c) passes++; else { fails++; console.log(`  FAIL ${what}${d ? `  (${d})` : ''}`); } };
const eq = (a: readonly number[], b: readonly number[]) => a.length === b.length && a.every((x, i) => Math.abs(x - b[i]) < 1e-9);
const src = (f: string) => readFileSync(new URL(`../${f}`, import.meta.url), 'utf8');

// ── 1. The speed keys: display only ──────────────────────────────────────────────────────────────────────────────
ok('2 is the slowest waterfall (no 1 fps); the data floor is 5', MIN_WF_FPS === 2 && DATA_FLOOR_FPS === 5);
ok('labels, slowest first: MINIMUM · LOW · ECO · NATIVE · DEFAULT · SMOOTH',
   ['min2', 'low', 'eco', 'sharp', 'default', 'smooth'].map(k => WF_SPEED_LABEL[k as WfScroll]).join(' ') === 'MINIMUM LOW ECO NATIVE DEFAULT SMOOTH');
ok("migration: a saved 'minimum' (the old 5 fps) loads as LOW; the new MINIMUM is stored 'min2'", loadWfSpeed('minimum') === 'low' && loadWfSpeed('min2') === 'min2');
ok("migration: every current value loads unchanged; junk / null → null (keep the default)",
   (['min2', 'low', 'eco', 'sharp', 'default', 'smooth'] as const).every(v => loadWfSpeed(v) === v)
   && loadWfSpeed('half') === null && loadWfSpeed(null) === null && loadWfSpeed(undefined) === null && loadWfSpeed(5) === null);
ok("'minimum' is no longer a CURRENT value (so it is always migrated, never kept)", !isWfSpeedSetting('minimum') && isWfSpeedSetting('sharp'));
{ // ★★ THE SPEED KEYS NEVER TOUCH THE LINK — in the source of every path that sets the incoming rate.
  const sdr = src('src/screens/SDRScreen.tsx'), web = src('web/client/src/main.ts'), kiwi = src('src/services/KiwiAdapter.ts');
  ok('app: the speed key sets no link ceiling (no wfLinkCap / linkCapFor in SDRScreen)', !/wfLinkCap|linkCapFor/.test(sdr));
  const cafps = web.slice(web.indexOf('function computeActiveFps'), web.indexOf('}', web.indexOf('function computeActiveFps')));
  ok('web: the rate asked for (computeActiveFps) reads the DATA controller only, never the speed key', /linkFps/.test(cafps) && !/wfScroll/.test(cafps));
  const lever = kiwi.slice(kiwi.indexOf('private wfSpeedLever'), kiwi.indexOf('\n', kiwi.indexOf('private wfSpeedLever')));
  ok("Kiwi: wf_speed follows the DATA control only (LOW → 2, AUTO/FULL → 4)", /_linkMode === 'lowData' \? 2 : 4/.test(lever), lever);
  ok('OpenWebRX: no DATA control (no rate lever) — the app draws it only where serverType is not owrx',
     /const dataOk = route\.params\.serverType !== 'owrx'/.test(sdr) && /onLinkMode=\{!dataOk \? undefined : onLinkMode\}/.test(sdr));
}

// ── 1b. DATA · LOW · AUTO · FULL ─────────────────────────────────────────────────────────────────────────────────
ok('labels are words only (no frame rates): LOW · AUTO · FULL, and the note names no rate',
   DATA_LABEL.low === 'LOW' && DATA_LABEL.auto === 'AUTO' && DATA_LABEL.full === 'FULL' && !/\d|fps|frame/i.test(DATA_NOTE));
ok('app saves map as they were: LOW DATA ↔ LOW, FULL ↔ FULL, AUTO ↔ AUTO (junk → AUTO)',
   dataModeFor('lowData') === 'low' && dataModeFor('full') === 'full' && dataModeFor('adaptive') === 'auto' && dataModeFor('x') === 'auto'
   && linkModeFor('low') === 'lowData' && linkModeFor('auto') === 'adaptive' && linkModeFor('full') === 'full');
ok("web's old data rate maps: 5 → LOW, 20 → FULL, 0 / 10 → AUTO",
   dataModeFromWebRate(5) === 'low' && dataModeFromWebRate(20) === 'full' && dataModeFromWebRate(0) === 'auto' && dataModeFromWebRate(10) === 'auto');
ok('UberSDR has no rung below the floor any more ([10, 5] — the 3.3 emergency rung is gone)', eq(LADDERS.ubersdr, [10, 5]));
ok('the Low Data rung is the floor on every ladder: VibeServer [20,10,5] → 3, UberSDR [10,5] → 2, a 30 fps web ladder → 3',
   lowDataRungFor(LADDERS.vibeserver) === 3 && lowDataRungFor(LADDERS.ubersdr) === 2 && lowDataRungFor([30, 10, 5]) === 3);
ok('floorLadder drops anything under 5 ([10,5,3.3] → [10,5]; [3] → [3], a server that only has that)',
   eq(floorLadder([10, 5, 3.3]), [10, 5]) && eq(floorLadder([3]), [3]));
ok('UberSDR divisor from the rung FPS: 10→1, 5→2', uberDivisorFor(10, 10) === 1 && uberDivisorFor(10, 5) === 2);
ok('the link managers\' generic ceiling still works (unused by the speed keys): [20,10,5] under 10 → [10,5]', eq(capLadder([20, 10, 5], 3, 10).ladder, [10, 5]));
for (const [name, LM, ladder] of [['UberSDR', UberLM, LADDERS.ubersdr], ['VibeServer (and the web)', VibeLM, LADDERS.vibeserver],
                                  ['VibeServer at 30 (web ladder)', VibeLM, [30, 10, 5]]] as const) {
  const sent: number[] = [];
  const mk = (mode: 'full' | 'adaptive' | 'lowData') =>
    new LM({ ladder: [...ladder], lowDataRung: lowDataRungFor(ladder), mode, apply: (_r: number, f: number) => sent.push(f) });
  { sent.length = 0; const m = mk('lowData'); m.tick(0, true, true);
    ok(`${name}: LOW asks the floor, 5`, sent[sent.length - 1] === 5, `sent ${sent}`); }
  { sent.length = 0; const m = mk('full'); m.tick(0, true, true);
    ok(`${name}: FULL asks the server's full rate, ${ladder[0]}`, sent[sent.length - 1] === ladder[0], `sent ${sent}`); }
  { sent.length = 0; const m = mk('adaptive'); let max = 0;
    for (let i = 0; i < 80; i++) { m.tick(m.expectedFps, true, true); max = Math.max(max, m.expectedFps); }
    ok(`${name}: AUTO climbs to ${ladder[0]} on a good link`, max === ladder[0], `max ${max}`);
    let min = Infinity; for (let i = 0; i < 40; i++) { m.tick(0, true, true); min = Math.min(min, m.expectedFps); }
    ok(`${name}: AUTO steps down on a poor link — but never below the floor (lowest ${min})`, min === 5 && sent.every(f => f >= 5), `sent ${sent}`); }
}
// ★ The watch's own Low Data pin is separate (Swift LinkManager): it pins 10 fps on UberSDR (rung 1) and 13 on Kiwi —
//   above the floor (spike/WristSDR LinkManager.swift / KiwiClient.swift). Not changed here.

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

// ── 4. The live numbers (from the rate the server SENDS — its own, or the floor under DATA LOW) ────────────────────
const n20 = speedNumbers(20)!, n5 = speedNumbers(5)!, n2 = speedNumbers(2)!;
ok('20 fps feed: MINIMUM 2 · LOW 5 · ECO 10 · NATIVE 20 · DEFAULT 30 · SMOOTH 60',
   n20.min2 === 2 && n20.low === 5 && n20.eco === 10 && n20.sharp === 20 && n20.default === 30 && n20.smooth === 60, JSON.stringify(n20));
ok('5 fps feed (DATA LOW): MINIMUM 2 · LOW 5 · ECO 10 · NATIVE 5 · DEFAULT 15 · SMOOTH 30',
   n5.min2 === 2 && n5.low === 5 && n5.eco === 10 && n5.sharp === 5 && n5.default === 15 && n5.smooth === 30, JSON.stringify(n5));
ok('2 fps server: LOW 5 (filled in) · NATIVE 2', n2.low === 5 && n2.sharp === 2, JSON.stringify(n2));
ok('unknown feed → no numbers', speedNumbers(0) === null);
// every key but NATIVE shows a divisor of 60, on every client type's own rate (Kiwi ~23, UberSDR 10, OWRX ~9, VibeServer 20/21/30)
for (const [name, f] of [['Kiwi', 23], ['UberSDR', 10], ['OpenWebRX', 9], ['VibeServer', 20], ['VibeServer (Lenovo)', 21], ['VibeServer', 30]] as const) {
  const k = speedNumbers(f)!;
  ok(`${name} at ${f} fps: every number but NATIVE divides 60 — ${JSON.stringify(k)}`,
     [k.min2, k.low, k.eco, k.default, k.smooth].every(v => 60 % v === 0) && k.sharp === f);
}

console.log(`${fails ? 'FAIL' : 'PASS'}: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
