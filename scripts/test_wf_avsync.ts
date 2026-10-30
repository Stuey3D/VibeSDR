/**
 * THE WATERFALL WAITS FOR THE SOUND (src/assets/wfAvSync.ts + wfClock.ts clockArrive's holdMs, V12 2026-10-30).
 *
 * The audio plays a cushion behind its arrival (iOS: the 220 ms pre-roll, deeper after a hiccup up to the 0.4 s live-edge
 * bound; Android: the AudioTrack + packet queue), the waterfall ~80 ms. The web client has held its rows by the audio's
 * depth since 2026-08-10 (waterfall.ts setHoldMs); this is the app's twin — and Stuart's condition (2026-10-30) is that
 * nothing about HOW the waterfall runs changes: no new judder, no slicing on a tune or zoom, no stalls.
 *
 * Simulates the app's waterfall end to end, BEFORE (no hold — the pipeline as merged) against AFTER (the hold), feed →
 * screen, with the audio's playout delay as the native getter reports it (polled at 4 Hz, saw-toothing by a packet):
 *   220 ms steady → a ratchet to 400 ms at 15 s (a hiccup re-queued it deeper) → a DRAG (a tune in hand) 25–27 s → rest
 *   → the queue drains back to 250 ms at 34 s (the hold SHRINKS).
 * Pipelines: the fixed output clock (Default 30 / Smooth 60) and NATIVE through the jitter buffer; the JS path (timers late
 * 0–2 ms, 2 % of them 10–40 ms) and the engine (precise timers — its C++ twin is proved in test_spectrum_engine.cpp).
 * Feeds 10 fps ±15 ms + 3 % late · 20 · 23 fps; panels 60 and 120 Hz.
 * Measured: arrival → first on screen against the audio's delay for the same moment (the A/V offset: + = the picture
 * after the sound), STALLS (a release gap the rate does not allow), BURSTS (more rows in one display frame than the rate's
 * share), the hold during the drag, how long each change takes to line up again, and how it got there.
 * ★★★ Stuart (2026-10-30): "to prevent scrolling judder whilst the jitter buffer enables we can simply duplicate the
 *     previous lines if needed and then the waterfall still reacts to the audio in time but still stays smooth". On the
 *     fixed clock the hold STEPS: growth is carried by REPEATS of the row on screen (zero stalls), a shrink by DISCARDS
 *     (zero bursts). NATIVE (no clock, no repeats by rule) is NOT held: it is run here to show it unchanged — and that its
 *     own route already sits level with or behind the audio. WF_AV_NATIVE_SLEW=<ms per ms> runs the eased Native hold that
 *     was tried and rejected (a pre-queue in front of the jitter buffer), to show the stalls it costs.
 *
 * Run: node --no-warnings scripts/test_wf_avsync.ts        (WF_AV_TABLE=1 prints every case)
 */
import { newRowTargetState, type WfScroll } from '../src/assets/wfRowTarget.ts';
import { clockArrive, clockIdle, clockNextAt, clockRateUnderCap, clockRowRate, clockRun, clockSetRate,
         newRowClock } from '../src/assets/wfClock.ts';
import { AV_DEAD_MS, avHold, avNoteAudio, newAvSync, clockOwnMs } from '../src/assets/wfAvSync.ts';
import { rowPace, rowRate } from '../src/utils/wfPace.ts';

let fails = 0, passes = 0;
const ok = (what: string, c: boolean, d = '') => { if (c) passes++; else { fails++; console.log(`  FAIL ${what}${d ? `  (${d})` : ''}`); } };
const sv = (v: number) => ({ value: v });
function lcg(seed: number) { let s = seed >>> 0; return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 0x100000000); }

const SECS = 46, SETTLE = 5000;
const RATCHET = 15000, DRAG: [number, number] = [25000, 27000], SHRINK = 34000;
const TABLE = !!process.env.WF_AV_TABLE;
const NATIVE_SLEW = +(process.env.WF_AV_NATIVE_SLEW ?? 0);
/** The audio's TRUE playout delay for audio arriving at `t` (ms). */
const audioAt = (t: number) => (t < RATCHET ? 220 : t < SHRINK ? 400 : 250);
/** What the native getter reports at `t`: the true depth, saw-toothing by a 20 ms packet as buffers are queued and played. */
const audioReport = (t: number) => audioAt(t) + 20 * ((t / 20) % 1) - 10;

type Feed = { name: string; fps: number; arr: number[] };
function mkFeed(name: string, fps: number, jit: number, late: number, seed: number): Feed {
  const r = lcg(seed); const arr: number[] = [];
  for (let n = 31.7; n < SECS * 1000; n += 1000 / fps) {
    let a = n + (r() * 2 - 1) * jit;
    if (late > 0 && r() < late) a += 40 + r() * 40;
    arr.push(a);
  }
  arr.sort((a, b) => a - b);
  return { name, fps, arr };
}
const FEEDS = [mkFeed('10 ±15 ms + late', 10, 15, 0.03, 1), mkFeed('20', 20, 5, 0, 4), mkFeed('23', 23, 5, 0, 3)];
type Disp = { name: string; ts: number[]; hz: number };
function mkDisp(name: string, hz: number, seed: number): Disp {
  const r = lcg(seed); const ts: number[] = [];
  for (let t = 5.3; t < SECS * 1000; t += 1000 / hz) ts.push(t + (r() * 2 - 1) * 0.15);
  return { name, ts, hz };
}
const DISPS = [mkDisp('60 Hz', 60, 11), mkDisp('120 Hz', 120, 12)];

type Ev = { t: number; k: number; f: () => void };
interface Shown { rel: { i: number; t: number; n: number }[]; first: Map<number, number>; holds: { t: number; h: number }[]; rowIds: { t: number; id: number }[] }

/** The app's waterfall: `hold` false = BEFORE (as merged), true = AFTER (the waterfall waits for the sound). */
function appRun(feed: Feed, d: Disp, scroll: WfScroll, hold: boolean, engine: boolean, seed = 21): Shown {
  const rnd = lcg(seed);
  const lag = () => engine ? rnd() * 0.3 : (rnd() < 0.02 ? 10 + 30 * rnd() : 2 * rnd());
  const evs: Ev[] = []; let seq = 0;
  const less = (a: Ev, b: Ev) => a.t < b.t || (a.t === b.t && a.k < b.k);
  const at = (t: number, f: () => void): Ev => {
    const e = { t, k: seq++, f }; let i = evs.length; evs.push(e);
    while (i > 0) { const pi = (i - 1) >> 1; if (!less(evs[i], evs[pi])) break; [evs[i], evs[pi]] = [evs[pi], evs[i]]; i = pi; }
    return e;
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
  const rowId: number[] = [], rowAt: number[] = [];
  const push = (t: number, id: number) => { rowId.push(id); rowAt.push(t); };
  const out: Shown = { rel: [], first: new Map(), holds: [], rowIds: [] };
  // ── the audio getter, polled at 4 Hz (WaterfallView's poll) ──
  const av = newAvSync();
  const origin = new Map<number, number>();   // (the rejected Native hold) when each frame entered the route
  let own = -1, natH = 0, lastNat = 0;
  let lastInteract = 0;
  const interacting = (t: number) => t >= DRAG[0] && t < DRAG[1];
  // ── the jitter buffer (WaterfallView enqueueFrame/drainFrame — NATIVE) ──
  const jb = (() => {
    const q: number[] = []; let arrMs = 150, lastArr = 0, prefill = true, timer: Ev | null = null;
    const drain = (t: number) => {
      timer = null;
      if (prefill) { if (q.length >= 1) prefill = false; else return; }
      if (q.length === 0) { prefill = true; return; }
      push(t, q.shift()!);
      const dur = Math.max(40, Math.min(1000, q.length > 1 ? arrMs * 0.6 : arrMs));
      const when = t + dur + lag(); const e = at(when, () => { if (timer === e) drain(when); }); timer = e;
    };
    return {
      get arrMs() { return arrMs; },
      arrive(a: number, id: number) {
        if (lastArr > 0) arrMs = arrMs * 0.8 + (a - lastArr) * 0.2;
        lastArr = a; q.push(id); if (q.length > 3) q.shift();
        if (!timer) { const when = a + lag(); const e = at(when, () => { if (timer === e) drain(when); }); timer = e; }
      },
      bypass(a: number) { cancel(timer); timer = null; q.length = 0; prefill = true; lastArr = a; },
    };
  })();
  // ── NATIVE's hold: frames enter the jitter buffer `hold` ms after they arrived (WaterfallView avPre) ──
  const pre: { id: number; due: number }[] = []; let preEv: Ev | null = null;
  const preRun = (t: number) => {
    preEv = null;
    while (pre.length && pre[0].due <= t + 1) { const f = pre.shift()!; jb.arrive(t, f.id); }
    if (pre.length) { const when = pre[0].due + lag(); const e = at(when, () => { if (preEv === e) preRun(when); }); preEv = e; }
  };
  // ── the fixed clock (WaterfallView pumpClock) ──
  const st = newRowTargetState(); const clk = newRowClock(); let pumpEv: Ev | null = null;
  let avgFrameMs = 150, lastFrameTs = 0, pTgt = 0;
  const pump = (t: number) => {
    const o: number[] = []; clockRun(clk, t, o);
    for (const id of o) push(t, id);
    cancel(pumpEv); pumpEv = null;
    const nx = clockIdle(clk, t) ? -1 : clockNextAt(clk);
    if (nx >= 0) { const when = Math.max(nx, t) + lag(); const e = at(when, () => { if (pumpEv === e) { pumpEv = null; pump(when); } }); pumpEv = e; }
  };
  feed.arr.forEach((a, id) => at(a, () => {
    const t = a + lag();
    at(t, () => {
      if (interacting(t)) lastInteract = t;
      const inHand = t - lastInteract < 1000 && lastInteract > 0;          // SMOOTH_TUNE_TAIL_MS
      if (scroll === 'sharp') {
        // NATIVE: through the jitter buffer as merged. (WF_AV_NATIVE_SLEW: the rejected eased hold, for the record.)
        if (NATIVE_SLEW > 0 && hold) {
          const want = av.aud >= 0 && own >= 0 ? Math.max(0, av.aud - own) : 0;
          const dt = lastNat > 0 ? t - lastNat : 0; lastNat = t;
          natH = inHand ? 0 : (want > natH ? Math.min(want, natH + NATIVE_SLEW * dt) : Math.max(want, natH - NATIVE_SLEW * dt));
          out.holds.push({ t, h: natH }); origin.set(id, t + natH);
          if (inHand) { pre.length = 0; cancel(preEv); preEv = null; jb.bypass(t); push(t, id); return; }
          if (natH > 0 || pre.length) {
            pre.push({ id, due: t + natH });
            if (!preEv) { const when = pre[0].due + lag(); const e = at(when, () => { if (preEv === e) preRun(when); }); preEv = e; }
            return;
          }
          jb.arrive(t, id); return;
        }
        out.holds.push({ t, h: 0 });
        if (inHand) { jb.bypass(t); push(t, id); return; }
        jb.arrive(t, id); return;
      }
      if (lastFrameTs > 0 && !inHand) avgFrameMs = avgFrameMs * 0.8 + (t - lastFrameTs) * 0.2;
      lastFrameTs = t;
      const rate = clockRateUnderCap(clockRowRate(st, scroll, 1000 / avgFrameMs), 0);
      if (rate <= 0) { push(t, id); pTgt = 0; return; }
      clockSetRate(clk, rate, t);
      const h = hold ? avHold(av, clockOwnMs(clk.ivl, rate), inHand) : 0;   // (a passthrough is a shrink to 0: clockArrive discards what is held)
      out.holds.push({ t, h });
      clockArrive(clk, id, t, inHand, h);
      pTgt = rate / 1000;
      pump(t);
    });
  }));
  // ── the UI thread (paceTick): one release per display frame ──
  const shown = sv(0), acc = sv(0), lead = sv(0), cool = sv(0), mode = sv(0), rr = sv(0.03), rT = sv(0), rN = sv(0);
  let avail = 0, prev = 0;
  // ── the 4 Hz poll (WaterfallView): the audio's delay, and (NATIVE) the age of the row on screen ──
  for (let t = 100; t < SECS * 1000; t += 250) at(t, () => {
    avNoteAudio(av, audioReport(t), t);
    if (NATIVE_SLEW > 0 && scroll === 'sharp' && prev > 0) {
      const o = origin.get(rowId[prev - 1]);
      if (o !== undefined) { const x = Math.max(0, t - o - 0.5 * jb.arrMs); own = own < 0 ? x : own + (x - own) * 0.04; }
    }
  });
  d.ts.forEach((ts, i) => at(ts, () => {
    while (avail < rowAt.length && rowAt[avail] <= ts) avail++;
    if (avail <= 0 || i === 0) return;
    const gap = ts - d.ts[i - 1];
    rowRate(avail, ts, rr, rN, rT, false);
    const s = rowPace(avail, shown, acc, lead, cool, mode, pTgt > 0 ? pTgt : rr.value, gap, true, pTgt > 0);
    if (s > prev) {
      out.rel.push({ i, t: ts, n: s - prev });
      for (let r = prev; r < s; r++) { if (!out.first.has(rowId[r])) out.first.set(rowId[r], ts); out.rowIds.push({ t: ts, id: rowId[r] }); }
    }
    prev = s;
  }));
  let guard = 0;
  while (evs.length) { popEv().f(); if (++guard > 5e6) throw new Error('runaway events'); }
  return out;
}

/** The A/V offset (mean, |p95|) over frames ARRIVING in [from, to): first on screen − arrival − the audio's delay. */
function avOff(sh: Shown, feed: Feed, from: number, to: number) {
  const e: number[] = [], lat: number[] = [];
  feed.arr.forEach((a, id) => {
    if (a < from || a >= to) return;
    const f = sh.first.get(id); if (f === undefined) return;
    lat.push(f - a); e.push(f - a - audioAt(a));
  });
  const m = (v: number[]) => v.reduce((x, y) => x + y, 0) / Math.max(1, v.length);
  const abs = e.map(x => Math.abs(x - m(e))).sort((x, y) => x - y);
  return { off: m(e), lat: m(lat), spread95: abs[Math.floor(abs.length * 0.95)] ?? 0, n: e.length };
}
/** Stalls and bursts over [from, to) display time. Clocked (R > 0): a step longer than ceil(Hz / R) frames is a row due
 *  and not shown; more than ceil(R / Hz) rows in a frame is a burst. NATIVE (R = 0): a step longer than 1.6 frame
 *  intervals of the feed, and more than one row in a frame. */
function stallsBursts(sh: Shown, d: Disp, R: number, fps: number, from: number, to: number) {
  const rel = sh.rel.filter(x => x.t >= from && x.t < to);
  let stalls = 0, bursts = 0;
  const lim = R > 0 ? Math.ceil(d.hz / R - 1e-6) : Math.ceil(1.6 * d.hz / fps);
  for (let k = 1; k < rel.length; k++) if (rel[k].i - rel[k - 1].i > lim) stalls++;
  for (const x of rel) if (x.n > Math.max(1, Math.ceil((R > 0 ? R : fps) / d.hz - 1e-6))) bursts++;
  return { stalls, bursts };
}

const PASS_END = DRAG[1] + 1000;   // SMOOTH_TUNE_TAIL_MS after the last move
const WIN = {
  rest:    [SETTLE + 3000, RATCHET] as [number, number],          // 220 ms, the hold settled
  ratchet: [RATCHET + 2500, DRAG[0]] as [number, number],         // 400 ms
  drag:    [DRAG[0] + 200, DRAG[1]] as [number, number],          // a tune in hand
  after:   [PASS_END + 3000, SHRINK] as [number, number],         // passthrough over, the hold back in
  shrink:  [SHRINK + 2500, SECS * 1000 - 1500] as [number, number],   // 250 ms
};
/** How long after `T` the picture is back in step with the sound: the first arrival from which the frames of the next
 *  second are within ±20 ms on average and 90 % of them within ±30 (a late frame on a jittery feed is late either way)
 *  (ms after T; NaN = never). */
function alignedAfter(sh: Shown, feed: Feed, T: number, until: number): number {
  const e: { a: number; x: number }[] = [];
  feed.arr.forEach((a, id) => { if (a >= T && a < until) { const f = sh.first.get(id); if (f !== undefined) e.push({ a, x: f - a - audioAt(a) }); } });
  for (let i = 0; i < e.length; i++) {
    let n = 0, bad = 0, sum = 0;
    for (let j = i; j < e.length && e[j].a < e[i].a + 1000; j++) { n++; sum += e[j].x; if (Math.abs(e[j].x) > 30) bad++; }
    if (n > 0 && Math.abs(sum / n) <= 20 && bad <= 0.1 * n) return e[i].a - T;
  }
  return NaN;
}
/** Rows on screen in [from, to) that repeat the row before (the same frame again). */
const repeatsIn = (sh: Shown, from: number, to: number) => {
  let n = 0; for (let k = 1; k < sh.rowIds.length; k++) { const r = sh.rowIds[k]; if (r.t >= from && r.t < to && r.id === sh.rowIds[k - 1].id) n++; } return n;
};
/** Frames arriving in [from, to) never shown (discarded). */
const discardsIn = (sh: Shown, feed: Feed, from: number, to: number) => feed.arr.filter((a, id) => a >= from && a < to && !sh.first.has(id)).length;

type Acc = { st0: number; bu0: number; st1: number; bu1: number; lat0: number[]; lat1: number[]; off0: number[]; off1: number[];
             kOff0: number[]; kOff1: number[]; aOff: number[]; sOff0: number[]; sOff1: number[]; dLat0: number[]; dLat1: number[];
             tR: number[]; tE: number[]; tS: number[]; rep0: number; rep1: number; dis0: number; dis1: number };
const sum: Record<string, Acc> = {};
const avg = (v: number[]) => v.reduce((a, b) => a + b, 0) / Math.max(1, v.length);
const mx = (v: number[]) => Math.max(...v);

for (const feed of FEEDS) for (const d of DISPS) for (const scroll of ['default', 'smooth', 'sharp'] as const) for (const engine of [false, true]) {
  const R = scroll === 'default' ? 30 : scroll === 'smooth' ? 60 : 0;
  const native = scroll === 'sharp';
  const tag = `${engine ? 'engine' : 'app JS'} ${native ? 'native' : scroll} · ${feed.name} · ${d.name}`;
  const b = appRun(feed, d, scroll, false, engine), a = appRun(feed, d, scroll, true, engine);
  const sb = stallsBursts(b, d, R, feed.fps, SETTLE, SECS * 1000 - 1000), sa = stallsBursts(a, d, R, feed.fps, SETTLE, SECS * 1000 - 1000);
  const r0 = avOff(b, feed, ...WIN.rest), r1 = avOff(a, feed, ...WIN.rest);
  const k0 = avOff(b, feed, ...WIN.ratchet), k1 = avOff(a, feed, ...WIN.ratchet);
  const g0 = avOff(b, feed, ...WIN.drag), g1 = avOff(a, feed, ...WIN.drag);
  const q1 = avOff(a, feed, ...WIN.after);
  const s0 = avOff(b, feed, ...WIN.shrink), s1 = avOff(a, feed, ...WIN.shrink);
  const tR = alignedAfter(a, feed, RATCHET, DRAG[0]), tE = alignedAfter(a, feed, PASS_END, SHRINK), tS = alignedAfter(a, feed, SHRINK, SECS * 1000 - 1500);
  // growth (the re-engage after the drag): repeats; a shrink: discards — each against the same window before
  const rep0 = repeatsIn(b, PASS_END, PASS_END + 1500), rep1 = repeatsIn(a, PASS_END, PASS_END + 1500);
  const dis0 = discardsIn(b, feed, SHRINK - 500, SHRINK + 1500), dis1 = discardsIn(a, feed, SHRINK - 500, SHRINK + 1500);
  const srep0 = repeatsIn(b, SHRINK, SHRINK + 2000), srep1 = repeatsIn(a, SHRINK, SHRINK + 2000);
  const sbS = stallsBursts(a, d, R, feed.fps, SHRINK, SHRINK + 2500);
  let steep = 0;
  for (let k = 1; k < a.holds.length; k++) {
    const x = a.holds[k - 1], y = a.holds[k];
    if (y.h === 0 && x.h > 0 && y.t >= DRAG[0] && y.t < DRAG[1] + 50) continue;   // the passthrough's drop (intended)
    if (y.t > x.t) steep = Math.max(steep, Math.abs(y.h - x.h) / (y.t - x.t));
  }
  const dragHold = Math.max(0, ...a.holds.filter(x => x.t >= DRAG[0] + 1 && x.t < DRAG[1]).map(x => x.h));
  const key = `${engine ? 'engine' : 'app JS'} ${native ? 'native ' : scroll}`;
  const S = sum[key] ??= { st0: 0, bu0: 0, st1: 0, bu1: 0, lat0: [], lat1: [], off0: [], off1: [], kOff0: [], kOff1: [], aOff: [], sOff0: [], sOff1: [],
                           dLat0: [], dLat1: [], tR: [], tE: [], tS: [], rep0: 0, rep1: 0, dis0: 0, dis1: 0 };
  S.st0 += sb.stalls; S.bu0 += sb.bursts; S.st1 += sa.stalls; S.bu1 += sa.bursts;
  S.lat0.push(r0.lat); S.lat1.push(r1.lat); S.off0.push(r0.off); S.off1.push(r1.off); S.kOff0.push(k0.off); S.kOff1.push(k1.off);
  S.aOff.push(q1.off); S.sOff0.push(s0.off); S.sOff1.push(s1.off); S.dLat0.push(g0.lat); S.dLat1.push(g1.lat);
  S.tR.push(tR); S.tE.push(tE); S.tS.push(tS); S.rep0 += rep0; S.rep1 += rep1; S.dis0 += dis0; S.dis1 += dis1;
  if (TABLE) console.log(`${tag.padEnd(46)} A/V rest ${r0.off.toFixed(0).padStart(5)} → ${r1.off.toFixed(0).padStart(4)}  400 ${k0.off.toFixed(0).padStart(5)} → ${k1.off.toFixed(0).padStart(4)}` +
    `  250 ${s0.off.toFixed(0).padStart(5)} → ${s1.off.toFixed(0).padStart(4)}  after drag ${q1.off.toFixed(0).padStart(4)}  aligned ${tR.toFixed(0)}/${tE.toFixed(0)}/${tS.toFixed(0)} ms` +
    `  drag lat ${g0.lat.toFixed(0)} → ${g1.lat.toFixed(0)}  stalls ${sb.stalls}→${sa.stalls} bursts ${sb.bursts}→${sa.bursts}  repeats ${rep0}→${rep1} discards ${dis0}→${dis1}`);
  if (!native || NATIVE_SLEW > 0) ok(`${tag}: at rest the picture within ±30 ms of the sound (${r1.off.toFixed(1)} ms; before ${r0.off.toFixed(0)})`, Math.abs(r1.off) <= 30);
  if (!native || NATIVE_SLEW > 0) ok(`${tag}: after the ratchet to 400 ms, within ±30 ms (${k1.off.toFixed(1)} ms; before ${k0.off.toFixed(0)})`, Math.abs(k1.off) <= 30);
  if (!native || NATIVE_SLEW > 0) ok(`${tag}: once the drag has settled, within ±30 ms (${q1.off.toFixed(1)} ms)`, Math.abs(q1.off) <= 30);
  if (!native || NATIVE_SLEW > 0) ok(`${tag}: after the audio drains back to 250 ms, within ±30 ms (${s1.off.toFixed(1)} ms; before ${s0.off.toFixed(0)})`, Math.abs(s1.off) <= 30);
  if (!native || NATIVE_SLEW > 0) ok(`${tag}: no hold while the dial moves (max ${dragHold.toFixed(1)} ms)`, dragHold === 0);
  if (!native || NATIVE_SLEW > 0) ok(`${tag}: the drag exactly as immediate as before (${g1.lat.toFixed(1)} vs ${g0.lat.toFixed(1)} ms)`, Math.abs(g1.lat - g0.lat) <= 3);
  if (!native || NATIVE_SLEW > 0) ok(`${tag}: no extra stalls (${sb.stalls} → ${sa.stalls})`, sa.stalls <= sb.stalls);
  if (!native || NATIVE_SLEW > 0) ok(`${tag}: no extra bursts (${sb.bursts} → ${sa.bursts})`, sa.bursts <= sb.bursts);
  if (native) {
    // NATIVE runs exactly as it did: the same rows at the same moments
    const same = NATIVE_SLEW > 0 || (a.rel.length === b.rel.length && a.rel.every((x, k) => x.i === b.rel[k].i && x.n === b.rel[k].n));
    ok(`${tag}: NATIVE unchanged — every row released on the same display frame as before`, same);
  } else {
    // ★★★ Stuart's rule: growth → repeats of the row on screen; a shrink → discards
    ok(`${tag}: the re-engage is carried by REPEATS (${rep0} → ${rep1} in 1.5 s)`, rep1 > rep0);
    // a shrink is taken out of the picture by discards — whole frames dropped, or (a shrink under one interval) fewer
    // repeats of the frames — and never a burst
    ok(`${tag}: the shrink is DISCARDS, no burst (frames dropped ${dis0} → ${dis1}, repeats ${srep0} → ${srep1}, bursts ${sbS.bursts})`,
       (dis1 > dis0 || srep1 < srep0) && sbS.bursts === 0);
    ok(`${tag}: back in step quickly — ratchet ${tR.toFixed(0)} ms, re-engage ${tE.toFixed(0)} ms, shrink ${tS.toFixed(0)} ms`,
       tR <= 3000 && tE <= 2000 && tS <= 3000);   // (the ratchet and the shrink wait on the audio depth's own smoothing)
  }
}

console.log('\nSUMMARY (all feeds × panels): BEFORE → AFTER. A/V = the frame first on screen − arrival − the audio\'s delay (+ = picture after sound)');
for (const [k, S] of Object.entries(sum)) {
  console.log(`  ${k.padEnd(15)} arrival→screen at rest ${avg(S.lat0).toFixed(0).padStart(3)} → ${avg(S.lat1).toFixed(0).padStart(3)} ms · ` +
    `A/V at 220 ${avg(S.off0).toFixed(0).padStart(5)} → ${avg(S.off1).toFixed(0).padStart(3)} · at 400 ${avg(S.kOff0).toFixed(0).padStart(5)} → ${avg(S.kOff1).toFixed(0).padStart(3)} · ` +
    `at 250 ${avg(S.sOff0).toFixed(0).padStart(5)} → ${avg(S.sOff1).toFixed(0).padStart(3)} · after a drag ${avg(S.aOff).toFixed(0).padStart(3)}`);
  console.log(`  ${''.padEnd(15)} drag latency ${avg(S.dLat0).toFixed(0)} → ${avg(S.dLat1).toFixed(0)} ms · stalls ${S.st0} → ${S.st1} · bursts ${S.bu0} → ${S.bu1} · ` +
    `repeats on re-engage ${S.rep0} → ${S.rep1} · discards on shrink ${S.dis0} → ${S.dis1} · in step again (mean/max ms): ratchet ${avg(S.tR).toFixed(0)}/${mx(S.tR).toFixed(0)}, ` +
    `re-engage ${avg(S.tE).toFixed(0)}/${mx(S.tE).toFixed(0)}, shrink ${avg(S.tS).toFixed(0)}/${mx(S.tS).toFixed(0)}`);
}

// ── The rule itself ──
{
  const s = newAvSync();
  avNoteAudio(s, 300, 0);
  let h = avHold(s, 80, false);
  ok(`the hold steps straight to audio − own (${h} → 220)`, h === 220);
  avNoteAudio(s, 304, 250); h = avHold(s, 80, false);
  ok(`a change under ${AV_DEAD_MS} ms is let be (${h})`, h === 220);
  h = avHold(s, 80, true);
  ok('a passthrough holds nothing, at once', h === 0 && s.pass);
  avNoteAudio(s, -1, 3200); h = avHold(s, 80, false);
  ok('an unknown audio delay is no hold (the waterfall as it was)', h === 0);
  const c = newRowClock(); clockSetRate(c, 30, 0);
  const p0 = clockArrive(c, 1, 0); const p1 = clockArrive(c, 2, 100);
  const c2 = newRowClock(); clockSetRate(c2, 30, 0);
  const q0 = clockArrive(c2, 1, 0, false, 0); const q1 = clockArrive(c2, 2, 100, false, 0);
  ok('holdMs 0 is the clock exactly as it was', p0 === q0 && p1 === q1);
  const c3 = newRowClock(); clockSetRate(c3, 30, 0);
  for (let i = 0; i < 10; i++) clockArrive(c3, i, i * 40, false, 300);
  ok(`a held clock keeps the frames the hold makes wait (${c3.q.length} queued)`, c3.q.length >= 8);
  const n3 = c3.q.length; clockArrive(c3, 10, 400, true, 0);
  ok(`a passthrough (a shrink to 0, at once) discards what is still held (${n3} → ${c3.q.length})`, c3.q.every(f => f.p <= 400 + 1e-9) && c3.q[c3.q.length - 1].id === 10);
  // growth is repeats, never a dead-link stop: a 30 rows/s clock on a 10 fps feed, the hold stepping 0 → 400 ms
  const c4 = newRowClock(); clockSetRate(c4, 30, 0); const o4: number[] = []; let t4 = 0, id4 = 0, gaps = 0, lastRow = 0;
  for (; t4 < 6000; t4 += 1) {
    if (t4 % 100 === 0) clockArrive(c4, id4++, t4, false, t4 >= 3000 ? 400 : 0);
    const n = clockRun(c4, t4, o4); if (n > 0) { if (t4 > 500 && t4 - lastRow > 34) gaps++; lastRow = t4; }
  }
  ok(`the hold stepping 0 → 400 ms: every tick still makes a row (${gaps} missed)`, gaps === 0);
}

console.log(`wf avsync: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
