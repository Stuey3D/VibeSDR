/**
 * A FAST RUN OF TUNE STEPS, AND ZOOM STEPS, IN THE WATERFALL (V12 2026-10-29, branch v12-wf-spin).
 *
 * Stuart, the Pi 500's RTL-SDR V4 on the AM band, web client, 10 fps feed: "the smaller step rate tuning is now landing
 * nicely" (500 Hz steps) — but "higher step rates turn into sharp lines with no detail" (a run of 9 kHz steps), and "same
 * with zooming". Then: "I think the waterfall rate needs to increase during interaction to allow for smoothness, but we
 * need to do it in a way that we dont try to pull down more data than a server can supply even if it means a 12x frame
 * generation" — and "if the app is set to 120 we do 120".
 *
 * Proves, on the pure code every client runs (app src/utils/wfRowPlace.ts + src/assets/wfClock.ts + src/utils/wfPace.ts;
 * web web/client/src/wfGlide.ts + wfPacing.ts; the engine's twins are proved in test_wf_motion.cpp /
 * test_spectrum_engine.cpp):
 *   A. THE BOOST RATE: min(the display's maximum as set, 120, 12 × the feed), on a whole-frame rate, never below the
 *      setting's; NATIVE 0 — the table is printed.
 *   B. THE SLOPE CAP: an ease is long enough that no carrier moves more than SLOPE_CAP_FRAC of the view a screen row (up
 *      to EASE_CEIL_S); small steps are EXACTLY as before; the dial's run of steps no longer compresses; settles at the
 *      true frequency; no blend (every displayed byte is from its own row, or the row below at an edge).
 *   C. THE WEB PACER: the rate rises only while the picture moves (+ the tail), to the boost and no higher, and comes back
 *      down on a whole row — no burst, no stall, at 60 Hz, 120 Hz and the Mac's 90–100 Hz; NATIVE untouched (one row a
 *      frame, no boost, no generated rows).
 *   D. THE APP UI: the clock's rows released by contPace (a gesture) and clockPace (at rest), across the boost up and
 *      down — the scroll never goes backwards, never jumps, never stalls.
 *   E. NOTHING IS SENT: the boost's code holds no socket, request or speed command.
 * Run: node --no-warnings scripts/test_wf_spin.ts
 */
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { cappedEaseRows, easeView, newFanState, newMotionState, placeRow, tuneEaseRows, viewMoveFrac, zoomEaseRows,
         EASE_CEIL_S, MOTION_ROWS, SLOPE_CAP_FRAC } from '../src/utils/wfRowPlace.ts';
import { boostActive, boostNote, boostRowRate, clockArrive, clockRun, clockSetRate, newBoost, newRowClock, BOOST_TAIL_MS }
  from '../src/assets/wfClock.ts';
import { clockPace, contAtWholeRow, contPace, paceToCont, paceToSettled } from '../src/utils/wfPace.ts';
import { newRowEase, rowView, type RowView } from '../web/client/src/wfGlide.ts';
import { RowPacer } from '../web/client/src/wfPacing.ts';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
let fails = 0, passes = 0;
const ok = (what: string, c: boolean, d = '') => { if (c) passes++; else { fails++; console.log(`  FAIL ${what}${d ? `  (${d})` : ''}`); } };
const sv = (v: number) => ({ value: v });

// ── A. THE BOOST RATE ───────────────────────────────────────────────────────────────────────────────────────────────
{
  console.log('  A. the interaction boost — rows/s while a tune/zoom is in flight (rest → boost)');
  const speeds: [string, number][] = [['Minimum', 5], ['Eco', 10], ['Default', 30], ['Smooth', 60]];
  const feeds = [5, 10, 20, 23];
  for (const [disp, hz] of [['120 Hz (Auto / 120 on ProMotion)', 120], ['60 Hz (or FRAME RATE 60)', 60], ['FRAME RATE 30', 30], ['Mac 90–100 Hz', 95]] as [string, number][]) {
    const rows = speeds.map(([n, r]) => `${n.padEnd(8)} ${feeds.map((f) => `${String(r).padStart(2)}→${String(boostRowRate(r, f, hz)).padStart(3)}`).join('  ')}`);
    console.log(`     ${disp}: feeds ${feeds.map((f) => `${f} fps`.padStart(7)).join('  ')}`);
    for (const r of rows) console.log(`       ${r}`);
  }
  let bad = 0, under = 0, frames = 0;
  for (const [, r] of speeds) for (const f of [3.3, 5, 9, 10, 11, 20, 23, 30]) for (const hz of [30, 60, 90, 95, 100, 120, 144]) {
    const b = boostRowRate(r, f, hz);
    if (b > 12 * f + 1e-9 && b !== r) bad++;
    if (b < r) under++;
    if (b > Math.max(r, Math.min(120, hz * 1.02))) bad++;
    if (b !== r && !(60 % b === 0 || (b === 120 && hz >= 119))) frames++;
  }
  ok('the boost never exceeds 12 × the feed, the display as set, or 120 (and is never below the setting)', bad === 0 && under === 0, `${bad} over, ${under} under`);
  ok('the boost is a whole-frame rate (divides 60, or 120 on a 120 Hz panel)', frames === 0);
  ok('120 at 10 fps on a 120 Hz panel; 60 at 5 fps; 60 under a 60 setting; 30 under a 30 setting',
     boostRowRate(30, 10, 120) === 120 && boostRowRate(30, 5, 120) === 60 && boostRowRate(30, 10, 60) === 60 && boostRowRate(30, 10, 30) === 30);
  ok('the Mac\'s 90–100 Hz panel boosts to 60 (120 needs a 120 Hz panel)', boostRowRate(30, 10, 95) === 60 && boostRowRate(30, 10, 100) === 60);
  ok('NATIVE (0 = actual rows) is never boosted', boostRowRate(0, 10, 120) === 0 && boostRowRate(0, 23, 60) === 0);
  const b = newBoost();
  ok('no boost before any move', !boostActive(b, 1000, false));
  boostNote(b, 1000);
  ok('the boost holds through the tail, ends after it — and holds while an ease is still drawing',
     boostActive(b, 1000 + BOOST_TAIL_MS - 1, false) && !boostActive(b, 1000 + BOOST_TAIL_MS + 1, false) &&
     (boostNote(b, 5000), boostActive(b, 5000 + BOOST_TAIL_MS + 400, true)) && !boostActive(b, 5000 + BOOST_TAIL_MS + 500, false));
}

// ── B. THE SLOPE CAP, on the app's placement (placeRow) and the web's (rowView) ─────────────────────────────────────────
type Ev = { t: number; c: number; s: number };
/** The scenarios, in ms from t = 0: (when, centre Hz, span Hz). Steady 2 s before, 3 s after. */
const C0 = 1_052_000, S0 = 200_000;
const steps = (n: number, every: number, dc: number, t0 = 2000, c = C0, s = S0): Ev[] => Array.from({ length: n }, (_, i) => ({ t: t0 + i * every, c: c + (i + 1) * dc, s }));
const zooms = (ks: number[], every: number, t0 = 2000): Ev[] => { let s = S0; return ks.map((k, i) => ({ t: t0 + i * every, c: C0, s: (s *= k) })); };
export const SCENARIOS: { name: string; ev: Ev[]; small?: boolean }[] = [
  { name: '500 Hz steps (3 a second)', ev: steps(8, 330, -500), small: true },
  { name: 'one 9 kHz step', ev: steps(1, 1, -9_000) },
  { name: 'a fast 9 kHz spin (15 steps, 8 a second)', ev: steps(15, 125, -9_000) },
  { name: 'a slow 9 kHz spin (2 a second)', ev: steps(6, 500, -9_000) },
  { name: 'a 100 kHz step', ev: steps(1, 1, -100_000) },
  { name: 'one ×√2 zoom key', ev: zooms([Math.SQRT1_2], 1) },
  { name: '×√2 ×4 in quick succession', ev: zooms([Math.SQRT1_2, Math.SQRT1_2, Math.SQRT1_2, Math.SQRT1_2], 150) },
  { name: '×2 rungs (UberSDR)', ev: zooms([0.5, 0.5, 2, 2], 700) },
  { name: 'a ×4 zoom', ev: zooms([0.25], 1) },
];
/** The APP: the fixed clock at `rate0` (boosted while a move is in flight when `boost`), each row the newest frame,
 *  placed by placeRow. Returns each row's DRAWN view, its own placement and the rate it came at. */
export function appSim(ev: Ev[], rate0: number, boost: boolean, feedFps = 10, hz = 120, N = 1024, R = 2048, row?: (c: number, s: number) => Uint8Array) {
  const ring = new Uint8Array(N * (R + MOTION_ROWS)), ms = newMotionState(R, N), fan = newFanState(R), clk = newRowClock(), bs = newBoost();
  const frames: { c: number; s: number }[] = [];
  const out: { lo: number; span: number; pLo: number; pSpan: number; t: number; rate: number; slot: number }[] = [];
  let view = { c: C0, s: S0 }, ei = 0, frames0 = 0, lastEase = 0, lastIn = 0;
  const end = (ev.length ? ev[ev.length - 1].t : 0) + 4000;
  const tick: number[] = [];
  for (let t = 0; t <= end; t += 1) {
    while (ei < ev.length && ev[ei].t <= t) { view = { c: ev[ei].c, s: ev[ei].s }; lastIn = t; ei++; }
    if (t % Math.round(1000 / feedFps) === 0) {   // a frame: the view as it is now (a VibeServer row for its view)
      const id = frames.push({ ...view }) - 1;
      const easeLeft = fan.easeLeft, started = easeLeft > lastEase; lastEase = easeLeft;
      if (boost && lastIn > bs.lastMove && t - lastIn < BOOST_TAIL_MS) boostNote(bs, lastIn);
      if (boost && started) boostNote(bs, t);
      const on = boost && boostActive(bs, t, easeLeft > 0);
      const rate = on ? boostRowRate(rate0, feedFps, hz) : rate0;
      fan.rowRate = rate;
      clockSetRate(clk, rate, t);
      clockArrive(clk, id, t, t - lastIn < 1000);
    }
    tick.length = 0; clockRun(clk, t, tick);
    for (const id of tick) {
      const f = frames[id], r = row ? row(f.c, f.s) : new Uint8Array(N).fill(40);
      const slot = placeRow(ring, R, N, frames0, r, f.c, f.s / N, f.c, f.s, ms, fan);
      frames0++;
      const a = fan.xfA[slot], b = fan.xfB[slot];
      out.push({ lo: fan.lastLo + b * fan.lastSpan, span: a * fan.lastSpan, pLo: fan.lastLo, pSpan: fan.lastSpan, t, rate: clk.rate, slot });
    }
  }
  return { rows: out, ring, fan, ms, N, R, frames: frames0 };
}
/** The WEB: rowView per device row at `devRate` device rows/s (rowPx device rows a screen row), frames at feedFps. */
function webSim(ev: Ev[], devRate: number, px: number, feedFps = 10) {
  const e = newRowEase(), v: RowView = { lo: 0, span: 0, jumpHz: 0 };
  const out: { lo: number; span: number; pLo: number; pSpan: number; jump: number }[] = [];
  let view = { c: C0, s: S0 }, frame = { c: C0, s: S0 }, ei = 0, nextF = 0, acc = 0;
  const end = (ev.length ? ev[ev.length - 1].t : 0) + 4000;
  for (let t = 0; t <= end; t += 1) {
    while (ei < ev.length && ev[ei].t <= t) { view = { c: ev[ei].c, s: ev[ei].s }; ei++; }
    if (t >= nextF) { frame = { ...view }; nextF += 1000 / feedFps; }
    acc += devRate / 1000;
    while (acc >= 1) { acc -= 1; rowView(e, frame.c - frame.s / 2, frame.s, px, v, devRate);
      out.push({ lo: v.lo, span: v.span, pLo: frame.c - frame.s / 2, pSpan: frame.s, jump: v.jumpHz }); }
  }
  return out;
}
/** Per-row moves of the drawn view (fractions of the view, screen-row scale ×px): the largest, and how many rows moved. */
function slope(rows: { lo: number; span: number }[], px = 1) {
  let worst = 0, moving = 0;
  for (let i = 1; i < rows.length; i++) {
    const d = viewMoveFrac(rows[i - 1].lo, rows[i - 1].span, rows[i].lo, rows[i].span) * px;
    if (d >= 1) continue;   // a jump (a whole view): moved as a block, not an ease
    if (d > 1e-9) moving++;
    worst = Math.max(worst, d);
  }
  return { worst, moving };
}
{
  console.log('  B. the slope cap — the largest on-screen move a row (% of the view), the rows the move is drawn over, the time');
  // the rule itself: unbound (small) steps are EXACTLY the time rule's rows, at every rate and dpr
  let same = 0, tot = 0;
  for (const rate of [5, 10, 15, 30, 60, 120]) for (const px of [1, 2, 3]) for (const dHz of [100, 300, 500]) {
    const base = tuneEaseRows(dHz / S0, rate * px) ; tot++;
    if (cappedEaseRows(C0, S0, C0 - dHz, S0, base, rate * px, px) === base) same++;
  }
  ok(`small steps (≤ 500 Hz on a 200 kHz view): the ease is exactly the time rule's, at every rate and dpr (${same}/${tot})`, same === tot);
  ok('a ×2 zoom: the cap lengthens the ease, never past the 1 s ceiling',
     cappedEaseRows(C0 - S0 / 2, S0, C0 - S0 / 4, S0 / 2, zoomEaseRows(0.5, 60), 60) === Math.round(EASE_CEIL_S * 60));
  for (const sc of SCENARIOS) {
    const before = appSim(sc.ev, 30, false), after = appSim(sc.ev, 30, true);
    const sb = slope(before.rows), sa = slope(after.rows);
    // the move's rows and time: from the first row drawn away from its own placement to the last
    const span = (rows: typeof after.rows) => { let a = -1, b = -1; rows.forEach((r, i) => { if (Math.abs(r.lo - r.pLo) > 1 || Math.abs(r.span / r.pSpan - 1) > 1e-6) { if (a < 0) a = i; b = i; } });
      return a < 0 ? { n: 0, s: 0 } : { n: b - a + 1, s: (rows[b].t - rows[a].t) / 1000 }; };
    const mb = span(before.rows), ma = span(after.rows);
    console.log(`     ${sc.name.padEnd(42)} before ${(sb.worst * 100).toFixed(2).padStart(5)} %  ${String(mb.n).padStart(3)} rows ${mb.s.toFixed(2)} s   after ${(sa.worst * 100).toFixed(2).padStart(5)} %  ${String(ma.n).padStart(3)} rows ${ma.s.toFixed(2)} s  (boost to ${Math.max(...after.rows.map((r) => r.rate))})`);
    // at rest, every row is drawn at its own placement (its true frequency)
    const last = after.rows.slice(-30);
    ok(`${sc.name}: settled at the true frequency`, last.every((r) => Math.abs(r.lo - r.pLo) < 1e-6 && Math.abs(r.span - r.pSpan) < 1e-6));
    // the cap: past it only where the 1 s ceiling binds (reported), and never by more than the ceiling allows
    const capRows = SLOPE_CAP_FRAC * 1.0001;
    if (sc.small) {
      // ★ SMALL STEPS: the same ease as before, row for row, at the same rate (the boost only changes the rate)
      const b30 = appSim(sc.ev, 30, false), a30 = appSim(sc.ev, 30, false);
      ok(`${sc.name}: the cap never binds (the look Stuart likes is untouched)`, sa.worst <= capRows && sb.worst <= capRows, `${(sb.worst * 100).toFixed(3)} %`);
      ok(`${sc.name}: at the same rate, row for row the same views`, b30.rows.length === a30.rows.length && b30.rows.every((r, i) => r.lo === a30.rows[i].lo && r.span === a30.rows[i].span));
    } else {
      ok(`${sc.name}: no row moves past the cap unless the 1 s ceiling binds`, sa.worst <= capRows || ma.s >= EASE_CEIL_S * 0.8,
         `${(sa.worst * 100).toFixed(2)} % over ${ma.s.toFixed(2)} s`);
      ok(`${sc.name}: flatter than before (more rows, a smaller move a row)`, sa.worst <= sb.worst + 1e-9 && ma.n >= mb.n, `${(sb.worst * 100).toFixed(2)} → ${(sa.worst * 100).toFixed(2)} %, ${mb.n} → ${ma.n} rows`);
    }
  }
  // NO BLEND: every displayed byte of an eased row is a byte of its OWN stored row (or the row below, at an edge)
  { const N = 1024; let seed = 5; const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
    const mk = (c: number, s: number) => { const r = new Uint8Array(N); for (let i = 0; i < N; i++) r[i] = 1 + Math.floor(rnd() * 250); return r; };
    const sim = appSim(SCENARIOS[2].ev, 30, true, 10, 120, N, 2048, mk);
    const disp = sim.fan.disp ?? sim.ring; let foreign = 0;
    for (let k = 1; k < Math.min(300, sim.rows.length); k++) { const r = sim.rows[sim.rows.length - k], below = sim.rows[sim.rows.length - k - 1];
      const own = new Set(sim.ring.subarray(r.slot * N, r.slot * N + N)), bel = new Set(disp.subarray(below.slot * N, below.slot * N + N));
      for (let x = 0; x < N; x++) { const v = disp[r.slot * N + x]; if (!own.has(v) && !bel.has(v)) foreign++; } }
    ok('no blend: every displayed byte is its own row\'s (or, at an eased edge, the row below\'s)', foreign === 0, `${foreign}`); }
  // THE WEB's rowView: the same cap per device row — dpr 1 and 2, at the boosted device rate
  for (const px of [1, 2]) {
    let worst = 0, bad = 0;
    for (const sc of SCENARIOS) {
      const rows = webSim(sc.ev, 120 * px, px), s = slope(rows, px);
      const lastOk = rows.slice(-20).every((r) => Math.abs(r.lo - r.pLo) < 1e-6 && r.span === r.pSpan);
      if (!lastOk) bad++;
      if (!sc.small) worst = Math.max(worst, s.worst);
      if (sc.small) ok(`web dpr ${px}: ${sc.name} — the cap does not bind`, s.worst <= SLOPE_CAP_FRAC * 1.0001, `${(s.worst * 100).toFixed(3)} %`);
    }
    ok(`web dpr ${px}: every scenario settles at the true frequency`, bad === 0, `${bad}`);
    console.log(`     web dpr ${px} at 120 screen rows/s: the largest move a screen row over all scenarios ${(worst * 100).toFixed(2)} % (cap ${(SLOPE_CAP_FRAC * 100).toFixed(2)} %)`);
  }
}

// ── C. THE WEB PACER across the boost, up and down ──────────────────────────────────────────────────────────────────
function mkDisp(hzAt: (t: number) => number, secs: number) { const ts: number[] = []; let s = 7; const r = () => ((s = (s * 1664525 + 1013904223) >>> 0) / 0x100000000);
  for (let t = 5.3; t < secs * 1000; t += 1000 / hzAt(t)) ts.push(t + (r() * 2 - 1) * 0.15); return ts; }
const DISPS: [string, (t: number) => number][] = [['60 Hz', () => 60], ['120 Hz', () => 120],
  ['Mac 90–100', (t) => Math.max(90, Math.min(100, 95 + 4 * Math.sin(2 * Math.PI * t / 6000) + 1.5 * Math.sin(2 * Math.PI * t / 1700)))]];
{
  console.log('  C. the web pacer — rows/s at rest, during a 1.5 s spin, after; the largest release gap and burst at the transitions');
  const T_ON = 8000, T_OFF = 9500, SECS = 14;
  for (const [dn, hzAt] of DISPS) for (const scroll of ['default', 'eco', 'smooth', 'sharp'] as const) for (const fps of [10, 5]) {
    const ts = mkDisp(hzAt, SECS), p = new RowPacer(); p.setScroll(scroll, 1);
    const arr: number[] = []; for (let t = 31.7; t < SECS * 1000; t += 1000 / fps) arr.push(t);
    let ai = 0; const rel: { t: number; n: number; i: number }[] = []; let maxBurst = 0, hasRow = false;
    ts.forEach((t, i) => {
      while (ai < arr.length && arr[ai] <= t) { if (arr[ai] >= T_ON && arr[ai] < T_OFF) p.noteMove(arr[ai]); p.onArrival(arr[ai]); ai++; hasRow = ai > 1; }
      const f = p.onFrame(t, hasRow);
      if (f > 0 && p.rowsNow > 0) { rel.push({ t, n: p.rowsNow, i }); maxBurst = Math.max(maxBurst, p.rowsNow); }
    });
    const rate = (a: number, b: number) => rel.filter((x) => x.t >= a && x.t < b).reduce((s, x) => s + x.n, 0) / ((b - a) / 1000);
    const rest = rate(5000, 8000), during = rate(8300, 9400), after = rate(11000, 13500);
    const fpsMeas = fps;
    const want = scroll === 'sharp' ? 0 : boostRowRate(rest > 0 ? Math.round(rest) : 0, fpsMeas, Math.min(120, Math.round(hzAt(9000) / 5) * 5 >= 119 ? 120 : hzAt(9000)));
    // gaps between releases, in display frames, around the transitions (7.5–11 s)
    let worstGap = 0; for (let k = 1; k < rel.length; k++) if (rel[k].t > 7500 && rel[k].t < 11000) worstGap = Math.max(worstGap, rel[k].i - rel[k - 1].i);
    const hz = hzAt(9000), slowFrames = Math.ceil(hzAt(7000) / Math.max(1, rest) - 1e-6);
    const label = `${dn.padEnd(10)} ${scroll.padEnd(7)} ${fps} fps`;
    console.log(`     ${label}: rest ${rest.toFixed(1)} → during ${during.toFixed(1)} → after ${after.toFixed(1)} rows/s; worst gap ${worstGap} frames, most rows a frame ${maxBurst}`);
    if (scroll === 'sharp') {
      ok(`${label}: NATIVE untouched — one row a frame received, before, during and after; no boost`, Math.abs(rest - fps) < 0.6 && Math.abs(during - fps) < 1.2 && Math.abs(after - fps) < 0.6 && !p.boosted, `${rest} ${during} ${after}`);
      continue;
    }
    ok(`${label}: the rate rises only during the move — to the boost (${want}), back to the setting after`,
       Math.abs(during - want) <= want * 0.06 + 1 && Math.abs(after - rest) <= rest * 0.06 + 0.5 && during <= 12 * fps + 1, `want ${want}`);
    ok(`${label}: no stall across the transitions (no gap longer than the resting beat)`, worstGap <= slowFrames + 1, `${worstGap} > ${slowFrames}`);
    ok(`${label}: no burst (never more rows in a frame than the boost's share)`, maxBurst <= Math.max(1, Math.ceil(want / hz - 1e-6)), `${maxBurst}`);
  }
}

// ── D. THE APP UI: clock rows released by contPace (gesture) / clockPace (rest), across the boost ───────────────────────
{
  console.log('  D. the app UI — the shown position through a gesture: boost up, tail, back down, hand-over to whole rows');
  const SECS = 14, T_ON = 8000, T_OFF = 9500, TAIL_CONT = 1000;   // WaterfallView: continuous while interacting (1 s tail)
  for (const [dn, hzAt] of DISPS) for (const [sn, base] of [['Default', 30], ['Eco', 10], ['Smooth', 60]] as [string, number][]) {
    const ts = mkDisp(hzAt, SECS), clk = newRowClock(), bs = newBoost();
    const rowAt: number[] = []; let pTgt = base / 1000, fid = 0, lastIn = 0;
    const shown = sv(0), acc = sv(0), lead = sv(0), cool = sv(0), mode = sv(0), pos = sv(0), cLead = sv(0), cTgt = sv(1);
    let cont = false, prevPos = 0, backwards = 0, worstJump = 0, worstStall = 0, lastMoveT = 0, slowest = Infinity;
    let nextFrame = 31.7, tq = 0; const out: number[] = [];
    const hzBoost = Math.round(hzAt(0)) >= 119 ? 120 : 60;
    ts.forEach((t, i) => {
      // the producer up to this display frame: frames at 10 fps, the clock's rows
      for (; tq <= t; tq += 0.5) {
        if (tq >= nextFrame) {
          nextFrame += 100;
          const moving = nextFrame >= T_ON && nextFrame < T_OFF; if (moving) lastIn = tq;
          if (lastIn > bs.lastMove && tq - lastIn < BOOST_TAIL_MS) boostNote(bs, lastIn);
          const r = boostActive(bs, tq, false) ? boostRowRate(base, 10, hzBoost) : base;
          clockSetRate(clk, r, tq); clockArrive(clk, fid++, tq, tq - lastIn < TAIL_CONT); pTgt = r / 1000;
        }
        out.length = 0; clockRun(clk, tq, out); for (const _ of out) rowAt.push(tq);
      }
      if (i === 0) return;
      const avail = rowAt.length, gap = t - ts[i - 1];
      const wantCont = lastIn > 0 && t - lastIn < TAIL_CONT;
      if (wantCont && !cont) { paceToCont(shown, pos, lead, cLead, cTgt); cont = true; }
      let p: number;
      if (cont) {
        const land = !wantCont && contAtWholeRow(pos.value, pTgt, gap);
        p = contPace(avail, pos, cLead, pTgt, gap, cTgt.value);
        if (land) { paceToSettled(avail, pos, shown, acc, lead, cLead, mode); acc.value = Math.min(0.5, pTgt * gap / 2); cont = false; p = shown.value; }
      } else p = clockPace(avail, shown, acc, lead, cool, mode, pTgt, gap, true);
      if (t > 5000) {
        const adv = p - prevPos;
        if (adv < -1e-9) backwards++;
        worstJump = Math.max(worstJump, adv - pTgt * gap);   // rows past the rate's share this frame
        slowest = Math.min(slowest, pTgt);                    // the slowest rate in effect since the last advance (a change mid-beat)
        if (adv > 1e-9) { if (lastMoveT > 0) worstStall = Math.max(worstStall, (t - lastMoveT) * slowest); lastMoveT = t; slowest = pTgt; }
      }
      prevPos = p;
    });
    const label = `${dn.padEnd(10)} ${sn.padEnd(7)}`;
    console.log(`     ${label}: rest ${base} → ${boostRowRate(base, 10, hzBoost)} rows/s; the largest advance a frame ${worstJump.toFixed(2)} rows past the rate's share, longest still ${worstStall.toFixed(2)} rows' time, ${backwards} backwards`);
    ok(`${label}: the shown position never goes backwards`, backwards === 0);
    ok(`${label}: no jump — no frame advances more than the rate's share + 1 row (a whole row at rest)`, worstJump <= 1.05, worstJump.toFixed(2));
    ok(`${label}: no stall — never still for more than about a row and a half's time`, worstStall <= 2.05, worstStall.toFixed(2));
  }
}

// ── E. NOTHING IS SENT: the boost's code holds no socket, request, speed or fps command ──────────────────────────────
{
  const grab = (file: string, from: string, to: string) => { const s = readFileSync(join(ROOT, file), 'utf8'); const i = s.indexOf(from), a = i < 0 ? -1 : Math.max(s.lastIndexOf('/*', i) > s.lastIndexOf('*/', i) ? s.lastIndexOf('/*', i) : 0, s.lastIndexOf('\n', i)); const b = s.indexOf(to, i + from.length); return a >= 0 && b > a ? s.slice(a, b) : ''; };
  const parts = [
    grab('src/assets/wfClock.ts', 'THE INTERACTION BOOST', '\nexport function boostActive'),
    grab('web/client/src/wfPacing.ts', 'THE INTERACTION BOOST', 'setScroll(scroll'),
    grab('src/components/WaterfallView.tsx', 'THE INTERACTION BOOST', 'const repeatsCarry'),
    grab('android/app/src/main/cpp/spectrum/spectrum_engine.cpp', 'THE INTERACTION BOOST', 'const bool repeatsCarry'),
    grab('web/client/src/waterfall.ts', 'THE INTERACTION BOOST', 'this.pacer.onArrival(now);'),
  ];
  const net = /\b(send|ws\.|socket|fetch|request|setSpeed|setFps|subscribe|postMessage|emit\()/i;
  ok('every boost block is found', parts.every((p) => p.length > 100), parts.map((p) => p.length).join(','));
  ok('nothing is sent: the boost blocks hold no socket, request, speed or fps command', parts.every((p) => !net.test(p.replace(/\/\/.*$/gm, '').replace(/\/\*[\s\S]*?\*\//g, ''))));
}

console.log(`  wf spin: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
