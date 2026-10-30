/**
 * THE WATERFALL'S FIXED OUTPUT CLOCK — over-interpolate and discard (src/assets/wfClock.ts, V12 2026-10-29).
 *
 * Stuart: "if we are only doing a straight 2x 3x interpolation an incoming rate that drifts about will make it feel
 * juddery which is why I said to over interpolate and discard to avoid this issue"; his UberSDR is "switching between
 * 9-11fps"; "say an incoming drops to 9 then that is a 2 frame drop, the 60 probably hides it because of its smoothness
 * and it being within the VRR window of the mac".
 *
 * Simulates every client's waterfall pipeline end to end, feed → screen, and measures what the screen shows:
 *   APP JS path (iOS, the Mac, Android without the engine): the jitter buffer (enqueue/drain, unchanged) → handleFrame's
 *       rows (BEFORE: wfRowTarget rowsForArrival + the repeat-row timer; AFTER: the fixed clock, wfClock clockRun on a
 *       timer) → publishRing → the UI thread's pacer (wfPace rowRate/rowPace, unchanged) on the display's frames. JS
 *       timers fire late by 0–2 ms, and 2 % of them 10–40 ms (a busy JS thread).
 *   ENGINE (Android native): the same pipeline with the engine thread's precise timers.
 *   WEB: RowPacer (BEFORE: the snapshot in $WF_BEFORE_WEB, if given; AFTER: web/client/src/wfPacing.ts) at dpr 1.
 * Feeds: 10 fps ±15 ms with 3 % of frames 40–80 ms late · 9.7 fps · 23 fps · 20 fps · 10 fps dipping to 9 for 4 s ·
 *        9–11 fps wandering second to second and in runs of seconds, ±15 ms.
 * Displays: 60 Hz, 120 Hz, and the Mac's panel wandering 90–100 Hz.
 * Measured (after 5 s): rows/s (overall, and the min–max of 1 s windows), evenness (share of steps on the modal beat),
 *   STALLS (a release gap longer than the rate allows on that panel), BURSTS (more rows in one frame than the rate's
 *   share), latency (network arrival → the frame's row first on screen), and real frames never shown.
 *
 * Run: node --no-warnings scripts/test_wf_fixed_clock.ts           (asserts the AFTER pipelines; prints the table)
 *      WF_BEFORE_WEB=<dir>/wfPacing.ts WF_TABLE=1 node --no-warnings scripts/test_wf_fixed_clock.ts   (+ BEFORE web)
 *      WF_SWEEP=1 …   prints the delay sweep the 0.2 × interval / 40 ms delay was picked from.
 *      WF_ONLY='<feed> · <panel>' runs one case; WF_SECS sets the run length; WF_JSBLOCK the longest JS-thread block (40 ms).
 */
import { newRowTargetState, stepRowTarget, rowTargetUnderCap, rowsForArrival, type WfScroll } from '../src/assets/wfRowTarget.ts';
import { CLOCK_CFG, clockArrive, clockIdle, clockNextAt, clockRateUnderCap, clockRowRate, clockRun, clockSetRate, newRowClock } from '../src/assets/wfClock.ts';
import { rowPace, rowRate } from '../src/utils/wfPace.ts';
import { RowPacer } from '../web/client/src/wfPacing.ts';

let fails = 0, passes = 0;
const ok = (what: string, c: boolean, d = '') => { if (c) passes++; else { fails++; console.log(`  FAIL ${what}${d ? `  (${d})` : ''}`); } };
const sv = (v: number) => ({ value: v });
function lcg(seed: number) { let s = seed >>> 0; return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 0x100000000); }

const SECS = +(process.env.WF_SECS ?? 30), SETTLE = 5000;

// ── Feeds: network arrival times, ms ────────────────────────────────────────────────────────────────────────────────
type Feed = { name: string; fps: number; arr: number[] };
function mkFeed(name: string, fps: number, rateAt: (t: number) => number, jit: number, late: number, seed: number): Feed {
  const r = lcg(seed); const arr: number[] = [];
  for (let n = 31.7; n < SECS * 1000; n += 1000 / rateAt(n)) {
    let a = n + (r() * 2 - 1) * jit;
    if (late > 0 && r() < late) a += 40 + r() * 40;       // a late frame (only this one)
    arr.push(a);
  }
  arr.sort((a, b) => a - b);
  return { name, fps, arr };
}
const wanderRate = (() => {   // 9–11 fps: random second to second for 12 s, then runs of 3 s
  const r = lcg(99); const perSec: number[] = [];
  for (let s = 0; s < 12; s++) perSec.push(9 + 2 * r());
  const runs = [9, 11, 9.2, 10.8, 9, 11];
  return (t: number) => (t < 12000 ? perSec[Math.floor(t / 1000)] : runs[Math.floor((t - 12000) / 3000) % runs.length]);
})();
const FEEDS: Feed[] = [
  mkFeed('10 ±15 ms + late', 10, () => 10, 15, 0.03, 1),
  mkFeed('9.7', 9.7, () => 9.7, 5, 0, 2),
  mkFeed('23', 23, () => 23, 5, 0, 3),
  mkFeed('20', 20, () => 20, 5, 0, 4),
  mkFeed('10 → 9 dip', 10, (t) => (t >= 10000 && t < 14000 ? 9 : 10), 5, 0, 5),
  mkFeed('9–11 wander ±15', 10, wanderRate, 15, 0, 6),
];

// ── Displays: frame timestamps, ms ──────────────────────────────────────────────────────────────────────────────────
type Disp = { name: string; ts: number[]; hzAt: (t: number) => number };
function mkDisp(name: string, hzAt: (t: number) => number, seed: number): Disp {
  const r = lcg(seed); const ts: number[] = [];
  for (let t = 5.3; t < SECS * 1000; t += 1000 / hzAt(t)) ts.push(t + (r() * 2 - 1) * 0.15);
  return { name, ts, hzAt };
}
const DISPS: Disp[] = [
  mkDisp('60 Hz', () => 60, 11),
  mkDisp('120 Hz', () => 120, 12),
  mkDisp('Mac 90–100', (t) => Math.max(90, Math.min(100, 95 + 4 * Math.sin(2 * Math.PI * t / 6000) + 1.5 * Math.sin(2 * Math.PI * t / 1700))), 13),
];

// ── What the screen showed ──────────────────────────────────────────────────────────────────────────────────────────
interface Shown { rel: { i: number; t: number; n: number }[]; first: Map<number, number>; tgts: number[]; hfAt?: Map<number, number>; rowsOf?: number[] }
interface Stats { rate: number; rMin: number; rMax: number; even: number; stalls: number; bursts: number; latMean: number; latP95: number;
                  lost: number; tgtChanges: number; ids: number[]; jb: number; hold: number }
function judge(sh: Shown, d: Disp, feed: Feed, R: number): Stats {
  const ts = d.ts;
  const rel = sh.rel.filter(x => x.t >= SETTLE);
  const rows = rel.reduce((a, x) => a + x.n, 0);
  const span = ts[ts.length - 1] - SETTLE;
  // 1 s windows
  let rMin = Infinity, rMax = 0;
  for (let w = SETTLE; w + 1000 <= ts[ts.length - 1]; w += 1000) {
    const n = rel.filter(x => x.t >= w && x.t < w + 1000).reduce((a, x) => a + x.n, 0);
    rMin = Math.min(rMin, n); rMax = Math.max(rMax, n);
  }
  const hist = new Map<number, number>(); let stalls = 0, bursts = 0;
  for (let k = 1; k < rel.length; k++) {
    const g = rel[k].i - rel[k - 1].i;
    hist.set(g, (hist.get(g) ?? 0) + 1);
    // A STALL: a step longer than the panel's own beat allows — at a true ratio r = Hz / rate the steps are floor(r) and
    // ceil(r) frames (3 and 4 on a 95 Hz panel at 30), so anything past ceil(r) is a row that was due and not shown.
    const r = d.hzAt((ts[rel[k].i] + ts[rel[k - 1].i]) / 2) / R;
    if (g > Math.ceil(r - 1e-6)) stalls++;
  }
  for (const x of rel) if (x.n > Math.max(1, Math.ceil(R / d.hzAt(ts[x.i]) - 1e-6))) bursts++;
  const gaps = rel.length - 1;
  const even = gaps > 0 ? Math.max(...hist.values()) / gaps : 0;
  const lats: number[] = []; let lost = 0; const ids: number[] = [];
  feed.arr.forEach((a, id) => {
    if (a < SETTLE || a > ts[ts.length - 1] - 500) return;
    const f = sh.first.get(id);
    if (f === undefined) lost++; else lats.push(f - a);
  });
  for (const [id] of [...sh.first.entries()].sort((a, b) => a[1] - b[1])) ids.push(id);
  lats.sort((a, b) => a - b);
  // the jitter buffer's share: network arrival → handleFrame
  let jb = 0, jn = 0;
  if (sh.hfAt) for (const [id, t] of sh.hfAt) { const a = feed.arr[id]; if (a >= SETTLE) { jb += t - a; jn++; } }
  // HOLD: the share of real frames shown for the modal number of rows (3 at 10 fps → 30) — how evenly the repeats fall
  const per = new Map<number, number>();
  if (sh.rowsOf) for (const id of sh.rowsOf) per.set(id, (per.get(id) ?? 0) + 1);
  const ph = new Map<number, number>(); for (const n of per.values()) ph.set(n, (ph.get(n) ?? 0) + 1);
  const hold = per.size ? Math.max(...ph.values()) / per.size : 1;
  let tgtChanges = 0; for (let k = 1; k < sh.tgts.length; k++) if (sh.tgts[k] !== sh.tgts[k - 1]) tgtChanges++;
  return { rate: rows / (span / 1000), rMin, rMax, even, stalls, bursts, latMean: lats.reduce((a, b) => a + b, 0) / Math.max(1, lats.length),
           latP95: lats[Math.floor(lats.length * 0.95)] ?? 0, lost, tgtChanges, ids, jb: jn ? jb / jn : 0, hold };
}

/** The best evenness an exact `R` rows/s can have on this panel: each row shown on the first frame at or after its
 *  ideal time (the panel's own beat — 3 and 4 frames on 95 Hz at 30 — and nothing else). */
function idealEven(d: Disp, R: number): number {
  const rel: number[] = []; let k = Math.ceil(SETTLE * R / 1000);
  for (let i = 1; i < d.ts.length; i++) { let n = 0; while (k * 1000 / R <= d.ts[i]) { k++; n++; } if (n && d.ts[i] >= SETTLE) rel.push(i); }
  const h = new Map<number, number>(); for (let j = 1; j < rel.length; j++) { const g = rel[j] - rel[j - 1]; h.set(g, (h.get(g) ?? 0) + 1); }
  return Math.max(...h.values()) / Math.max(1, rel.length - 1);
}

// ── The APP pipeline (JS path, or the engine with precise timers) ───────────────────────────────────────────────────
type Ev = { t: number; k: number; f: () => void };
function appRun(feed: Feed, d: Disp, scroll: WfScroll, after: boolean, engine: boolean, seed = 21): Shown {
  const rnd = lcg(seed);
  const lag = () => engine ? rnd() * 0.3 : (rnd() < 0.02 ? 10 + (+(process.env.WF_JSBLOCK ?? 40) - 10) * rnd() : 2 * rnd());
  // a binary heap of events, earliest first (ties in the order they were made)
  const evs: Ev[] = []; let seq = 0;
  const less = (a: Ev, b: Ev) => a.t < b.t || (a.t === b.t && a.k < b.k);
  const at = (t: number, f: () => void) => {
    const e = { t, k: seq++, f }; let i = evs.length; evs.push(e);
    while (i > 0) { const pi = (i - 1) >> 1; if (!less(evs[i], evs[pi])) break; [evs[i], evs[pi]] = [evs[pi], evs[i]]; i = pi; }
  };
  const popEv = (): Ev => {
    const top = evs[0], last = evs.pop()!;
    if (evs.length) { evs[0] = last; let i = 0;
      for (;;) { const l = 2 * i + 1, r = l + 1; let m = i;
        if (l < evs.length && less(evs[l], evs[m])) m = l; if (r < evs.length && less(evs[r], evs[m])) m = r;
        if (m === i) break; [evs[i], evs[m]] = [evs[m], evs[i]]; i = m; } }
    return top;
  };
  const cancel = (e: Ev | null) => { if (e) e.f = () => {}; };
  // rows in the ring: their frame ids and when the UI can see them
  const rowId: number[] = [], rowAt: number[] = [];
  const push = (t: number, id: number) => { rowId.push(id); rowAt.push(t); };
  // ── the jitter buffer (WaterfallView enqueue/drain, unchanged) ──
  const q: number[] = []; let arrMs = 150, lastArr = 0, prefill = true, jbTimer: Ev | null = null;
  // ── handleFrame state ──
  let avgFrameMs = 150, lastFrameTs = 0, avgWfGap = 0, lastWfPush = 0, lastId = -1, pTgt = 0;
  const st = newRowTargetState(), carry = { current: 0 };
  let extraEv: Ev | null = null;
  const clk = newRowClock(); let pumpEv: Ev | null = null;
  const tgts: number[] = [];
  const hfAt = new Map<number, number>();
  const pump = (t: number) => {
    const out: number[] = []; clockRun(clk, t, out);
    for (const id of out) push(t, id);
    cancel(pumpEv); pumpEv = null;
    const nx = clockIdle(clk, t) ? -1 : clockNextAt(clk);
    if (nx >= 0) { const e: Ev = { t: 0, k: 0, f: () => {} }; pumpEv = e; const when = Math.max(nx, t) + lag(); at(when, () => { if (pumpEv === e) { pumpEv = null; pump(when); } }); }
  };
  const handleFrame = (t: number, id: number) => {
    if (lastFrameTs > 0) avgFrameMs = avgFrameMs * 0.8 + (t - lastFrameTs) * 0.2;
    lastFrameTs = t;
    const fps = 1000 / avgFrameMs;
    hfAt.set(id, t);
    if (!after || scroll === 'sharp') {
      const rowTarget = rowTargetUnderCap(stepRowTarget(st, scroll, fps), fps, 0);
      if (t >= SETTLE) tgts.push(rowTarget);
      if (lastWfPush > 0) { const dt = t - lastWfPush; if (dt > 0 && dt < 2000) avgWfGap = avgWfGap > 0 ? avgWfGap * 0.8 + dt * 0.2 : dt; }
      lastWfPush = t;
      const dur = Math.max(15, Math.min(1000, avgWfGap > 0 ? avgWfGap : avgFrameMs));
      const rowsNow = rowsForArrival(carry, dur, rowTarget);
      if (rowsNow > 0) push(t, id);
      lastId = id;
      const extras = Math.max(0, rowsNow - 1);
      pTgt = rowTarget > 0 ? rowTarget / 1000 : 0;
      // scheduleExtraRows: cancel the chain, then one every dur / (extras + 1)
      cancel(extraEv); extraEv = null;
      const spacing = dur / (extras + 1);
      let left = extras;
      const tick = (tt: number) => { push(tt, lastId); if (--left > 0) { const e2 = { t: 0, k: 0, f: () => {} } as Ev; extraEv = e2; const when = tt + spacing + lag(); at(when, () => { if (extraEv === e2) tick(when); }); } else extraEv = null; };
      if (extras > 0) { const e = { t: 0, k: 0, f: () => {} } as Ev; extraEv = e; const when = t + spacing + lag(); at(when, () => { if (extraEv === e) tick(when); }); }
      return;
    }
    // ★ AFTER: the fixed clock
    const rate = clockRateUnderCap(clockRowRate(st, scroll, fps), 0);
    if (t >= SETTLE) tgts.push(rate);
    if (rate <= 0) { push(t, id); pTgt = 0; return; }
    clockSetRate(clk, rate, t);
    clockArrive(clk, id, t);
    pTgt = rate / 1000;
    pump(t);
  };
  const drain = (t: number) => {
    jbTimer = null;
    if (prefill) { if (q.length >= 1) prefill = false; else return; }
    if (q.length === 0) { prefill = true; return; }
    const id = q.shift()!;
    handleFrame(t, id);
    const dur = Math.max(40, Math.min(1000, q.length > 1 ? arrMs * 0.6 : arrMs));
    const e = { t: 0, k: 0, f: () => {} } as Ev; jbTimer = e; const when = t + dur + lag(); at(when, () => { if (jbTimer === e) drain(when); });
  };
  feed.arr.forEach((a, id) => at(a, () => {
    if (lastArr > 0) arrMs = arrMs * 0.8 + (a - lastArr) * 0.2;
    lastArr = a; q.push(id); if (q.length > 3) q.shift();
    if (!jbTimer) { const e = { t: 0, k: 0, f: () => {} } as Ev; jbTimer = e; const when = a + lag(); at(when, () => { if (jbTimer === e) drain(when); }); }
  }));
  // ── the UI thread: one paceTick per display frame ──
  const shown = sv(0), acc = sv(0), lead = sv(0), cool = sv(0), mode = sv(0), rate = sv(0.03), rT = sv(0), rN = sv(0);
  const out: Shown = { rel: [], first: new Map(), tgts, hfAt };
  let avail = 0, prev = 0;
  d.ts.forEach((ts, i) => at(ts, () => {
    while (avail < rowAt.length && rowAt[avail] <= ts) avail++;
    if (avail <= 0 || i === 0) return;
    const gap = ts - d.ts[i - 1];
    rowRate(avail, ts, rate, rN, rT, false);
    const s = rowPace(avail, shown, acc, lead, cool, mode, pTgt > 0 ? pTgt : rate.value, gap, true, after && pTgt > 0);
    if (s > prev) {
      out.rel.push({ i, t: ts, n: s - prev });
      for (let r = prev; r < s; r++) { if (!out.first.has(rowId[r])) out.first.set(rowId[r], ts); if (ts >= SETTLE) (out.rowsOf ??= []).push(rowId[r]); }
    }
    prev = s;
  }));
  let guard = 0;
  while (evs.length) { popEv().f(); if (++guard > 5e6) throw new Error(`runaway events (${scroll} after=${after} engine=${engine})`); }
  return out;
}

// ── The WEB pipeline ────────────────────────────────────────────────────────────────────────────────────────────────
type PacerLike = { setScroll(s: WfScroll, dpr: number): void; onArrival(t: number): void; onFrame(ts: number, has: boolean): number;
                   rowsNow: number; screenTarget: number; useCur?: boolean };
function webRun(feed: Feed, d: Disp, scroll: WfScroll, P: new () => PacerLike, before: boolean): Shown {
  const p = new P(); p.setScroll(scroll, 1);
  const out: Shown = { rel: [], first: new Map(), tgts: [] };
  let ai = 0, prevId = -1, curId = -1;
  d.ts.forEach((ts, i) => {
    while (ai < feed.arr.length && feed.arr[ai] <= ts) { p.onArrival(feed.arr[ai]); prevId = curId; curId = ai; if (feed.arr[ai] >= SETTLE) out.tgts.push(p.screenTarget); ai++; }
    const t = p.onFrame(ts, curId >= 0 && prevId >= 0);
    if (t > 0 && p.rowsNow > 0) {
      const id = (before ? t >= 0.5 : p.useCur) ? curId : prevId;
      out.rel.push({ i, t: ts, n: p.rowsNow });
      if (!out.first.has(id)) out.first.set(id, ts);
      if (ts >= SETTLE) for (let k = 0; k < p.rowsNow; k++) (out.rowsOf ??= []).push(id);
    }
  });
  return out;
}

// ── Run everything ──────────────────────────────────────────────────────────────────────────────────────────────────
const WANT: Record<string, number> = { default: 30, smooth: 60 };
const fmt = (s: Stats) => `${s.rate.toFixed(1).padStart(5)} [${String(s.rMin).padStart(2)}–${String(s.rMax).padStart(2)}] ev ${(s.even * 100).toFixed(0).padStart(3)}% st ${String(s.stalls).padStart(3)} bu ${String(s.bursts).padStart(3)} lat ${s.latMean.toFixed(0).padStart(3)}/${s.latP95.toFixed(0).padStart(3)} lost ${String(s.lost).padStart(3)} jb ${s.jb.toFixed(0).padStart(3)} hold ${(s.hold * 100).toFixed(0).padStart(3)}%`;
const TABLE = !!process.env.WF_TABLE;
let BeforeWeb: (new () => PacerLike) | null = null;
if (process.env.WF_BEFORE_WEB) BeforeWeb = (await import(process.env.WF_BEFORE_WEB)).RowPacer;

let jbDrops = 0;
const summary: Record<string, { stalls: number; bursts: number; lat: number[]; even: number[]; n: number }> = {};
const add = (k: string, s: Stats) => { const x = summary[k] ??= { stalls: 0, bursts: 0, lat: [], even: [], n: 0 }; x.stalls += s.stalls; x.bursts += s.bursts; x.lat.push(s.latMean); x.even.push(s.even); x.n++; };

for (const feed of FEEDS) for (const d of DISPS) {
  if (process.env.WF_ONLY && !`${feed.name} · ${d.name}`.includes(process.env.WF_ONLY)) continue;
  // NATIVE baseline (unchanged path): evenness to compare against, at the feed's own rate
  const nat = judge(appRun(feed, d, 'sharp', true, false), d, feed, feed.fps);
  const natWeb = judge(webRun(feed, d, 'sharp', RowPacer as unknown as new () => PacerLike, false), d, feed, feed.fps);
  if (TABLE) console.log(`\n${feed.name} fps · ${d.name}\n  NATIVE app        ${fmt(nat)}\n  NATIVE web        ${fmt(natWeb)}`);
  add('native app', nat); add('native web', natWeb);
  for (const scroll of ['default', 'smooth'] as const) {
    const R = WANT[scroll];
    const rows: [string, Stats][] = [
      ['app JS  before', judge(appRun(feed, d, scroll, false, false), d, feed, R)],
      ['app JS  after ', judge(appRun(feed, d, scroll, true, false), d, feed, R)],
      ['engine  before', judge(appRun(feed, d, scroll, false, true), d, feed, R)],
      ['engine  after ', judge(appRun(feed, d, scroll, true, true), d, feed, R)],
      ['web     after ', judge(webRun(feed, d, scroll, RowPacer as unknown as new () => PacerLike, false), d, feed, R)],
    ];
    if (BeforeWeb) rows.splice(4, 0, ['web     before', judge(webRun(feed, d, scroll, BeforeWeb, true), d, feed, R)]);
    for (const [who, s] of rows) {
      add(`${who.trim()} ${scroll}`, s);
      if (TABLE) console.log(`  ${scroll.padEnd(7)} ${who} ${fmt(s)}`);
      if (!who.includes('after')) continue;
      const tag = `${who.trim()} ${scroll} · ${feed.name} · ${d.name}`;
      // ★ The 23 fps feed under Default runs at 30 (filled in); every case's rate is the mode's.
      ok(`${tag}: no stalls`, s.stalls === 0, fmt(s));
      ok(`${tag}: no bursts`, s.bursts === 0, fmt(s));
      ok(`${tag}: rows/s ${R} (got ${s.rate.toFixed(2)})`, Math.abs(s.rate - R) / R < 0.02, fmt(s));
      ok(`${tag}: every 1 s window within ±1 row of ${R}`, s.rMin >= R - 1 && s.rMax <= R + 1, fmt(s));
      ok(`${tag}: the output rate never moved (the latch held)`, s.tgtChanges === 0 || (s.tgtChanges <= 1 && feed.name !== '9–11 wander ±15'), `changes ${s.tgtChanges}`);
      // Evenness: at least Native's on the same panel (Native at 10 fps is 6/12/9.5 frames a row — whole-frame beats);
      // on 60/120 Hz every step is the same beat.
      if (d.name !== 'Mac 90–100') ok(`${tag}: every step on one beat (${(s.even * 100).toFixed(1)} %)`, s.even >= 0.995, fmt(s));
      else ok(`${tag}: evenness ≥ the best an exact ${R}/s can do on this panel (${(s.even * 100).toFixed(0)} % vs ${(idealEven(d, R) * 100).toFixed(0)} %)`, s.even >= idealEven(d, R) - 0.03, fmt(s));
    }
  }
  // ── NATIVE: pure unmodified lines (Stuart: "native is pure unmodified lines direct from the server no modification at
  //    all") — over the settled window, every frame received is shown as exactly ONE row, in order: none repeated,
  //    none dropped (app JS path; the engine's twin is test_spectrum_engine.cpp). ──
  {
    const sh = appRun(feed, d, 'sharp', true, false);
    const end = d.ts[d.ts.length - 1] - 1500;   // (Native holds ~3 rows + the jitter buffer: up to ~0.9 s at the end)
    // the frames the WATERFALL received (handleFrame). ★ The jitter buffer in front of it (unchanged) can drop a frame
    //   when 3 are queued — reported, not asserted: it is upstream of the waterfall and the same for every mode.
    const inWin = feed.arr.map((a, id) => [a, id] as const).filter(([a]) => a >= SETTLE && a <= end).map(([, id]) => id);
    const want = inWin.filter(id => sh.hfAt?.has(id));
    jbDrops += inWin.length - want.length;
    const count = new Map<number, number>();
    for (const id of sh.rowsOf ?? []) count.set(id, (count.get(id) ?? 0) + 1);
    const order = [...sh.first.entries()].filter(([id]) => want.includes(id)).sort((a, b) => a[1] - b[1] || a[0] - b[0]).map(x => x[0]);
    const bad = want.filter(id => count.get(id) !== 1);
    const inOrder = order.every((v, k) => k === 0 || v > order[k - 1]);
    ok(`NATIVE app · ${feed.name} · ${d.name}: one row per frame received, in order, none repeated or dropped`,
       bad.length === 0 && inOrder, `bad ${bad.slice(0, 8).map(id => `${id}×${count.get(id) ?? 0}`).join(' ')} inOrder ${inOrder}`);
  }
}

console.log(`\nNATIVE: frames the jitter buffer (upstream, unchanged) dropped before the waterfall, all cases: ${jbDrops}`);
console.log('\nSUMMARY (all feeds × panels): stalls / bursts / mean latency ms / mean evenness');
for (const [k, x] of Object.entries(summary)) {
  const lat = x.lat.reduce((a, b) => a + b, 0) / x.n, ev = x.even.reduce((a, b) => a + b, 0) / x.n;
  console.log(`  ${k.padEnd(22)} stalls ${String(x.stalls).padStart(5)}  bursts ${String(x.bursts).padStart(5)}  latency ${lat.toFixed(0).padStart(4)}  even ${(ev * 100).toFixed(1)} %`);
}

// ── The delay sweep (WF_SWEEP=1): what the presentation delay buys (repeats falling in the feed's own pattern — HOLD,
//    the share of real frames shown for the modal number of rows) and what it costs (latency). Stalls/bursts are the
//    output clock's and do not depend on it. ──────────────────────────────────────────────────────────────────────────
if (process.env.WF_SWEEP) {
  console.log('\nDELAY SWEEP (Default + Smooth, all feeds × panels): delay → mean hold / mean latency (app JS · web)');
  const keep = { ...CLOCK_CFG };
  for (const [f, cap] of [[0, 0], [0.1, 40], [0.2, 40], [0.3, 40], [0.5, 60]] as const) {
    CLOCK_CFG.delayFrac = f; CLOCK_CFG.delayMaxMs = cap;
    const acc = { ah: 0, al: 0, wh: 0, wl: 0, n: 0, st: 0 };
    for (const feed of FEEDS) for (const d of DISPS) for (const sc of ['default', 'smooth'] as const) {
      const a = judge(appRun(feed, d, sc, true, false), d, feed, WANT[sc]);
      const w = judge(webRun(feed, d, sc, RowPacer as unknown as new () => PacerLike, false), d, feed, WANT[sc]);
      acc.ah += a.hold; acc.al += a.latMean; acc.wh += w.hold; acc.wl += w.latMean; acc.n++; acc.st += a.stalls + w.stalls;
    }
    console.log(`  ${f.toFixed(1)} × interval (≤ ${cap} ms): hold ${(acc.ah / acc.n * 100).toFixed(1)} % / ${(acc.al / acc.n).toFixed(0)} ms · ` +
                `web ${(acc.wh / acc.n * 100).toFixed(1)} % / ${(acc.wl / acc.n).toFixed(0)} ms · stalls ${acc.st}`);
  }
  Object.assign(CLOCK_CFG, keep);
}

console.log(`wf fixed clock: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
