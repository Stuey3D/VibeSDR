/**
 * Tune pacing on the client (src/services/tunePace.ts) — the app and the web page send fewer tunes to a
 * server that says it is struggling, latest wins, and the last tune always goes.
 *
 * ★★★ Stuart, 2026-10-05: "same as we do for if the ping increases. Basically if the server's CPU is
 *     reporting that it is struggling we need to slow down the amount of tune commands so that we don't
 *     overload it" — the Pi 2 in the garage, a slow link and a bogged-down server.
 *
 * Run: node --no-warnings scripts/test_tune_pace.ts   (run-tests.sh does)
 */
import { TunePacer, tunePaceMs, healthThrottled, TUNE_PACE_LOADED_MS, TUNE_PACE_CHOKED_MS } from '../src/services/tunePace.ts';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};

// A hand-cranked clock: timers fire only when we advance it.
let now = 0;
let queue: { at: number; fn: () => void; id: number }[] = [];
let nextId = 1;
const timers = {
  set: (fn: () => void, ms: number) => { const id = nextId++; queue.push({ at: now + ms, fn, id }); return id; },
  clear: (h: unknown) => { queue = queue.filter(t => t.id !== h); },
  now: () => now,
};
const advance = (ms: number) => {
  const end = now + ms;
  for (;;) {
    queue.sort((a, b) => a.at - b.at);
    const t = queue[0];
    if (!t || t.at > end) break;
    queue.shift(); now = t.at; t.fn();
  }
  now = end;
};

// ── The thresholds ──
eq('no news = full rate', tunePaceMs({}), 0);
eq('CPU WARM is not load', tunePaceMs({ cpuLevel: 1 }), 0);
eq('CPU HIGH = loaded', tunePaceMs({ cpuLevel: 2 }), TUNE_PACE_LOADED_MS);
eq('the snail = loaded', tunePaceMs({ throttled: true }), TUNE_PACE_LOADED_MS);
eq('CPU CRITICAL = choked', tunePaceMs({ cpuLevel: 3 }), TUNE_PACE_CHOKED_MS);
eq('ping 249 ms is fine', tunePaceMs({ rttMs: 249 }), 0);
eq('ping 250 ms = loaded', tunePaceMs({ rttMs: 250 }), TUNE_PACE_LOADED_MS);
eq('ping 600 ms = choked', tunePaceMs({ rttMs: 600 }), TUNE_PACE_CHOKED_MS);
eq('the worse signal wins', tunePaceMs({ cpuLevel: 2, rttMs: 700 }), TUNE_PACE_CHOKED_MS);
eq('garbage reads as fine (an older or hand-rolled server)', tunePaceMs({ cpuLevel: NaN as any, rttMs: undefined }), 0);
eq('health kinds that mean throttled', ['thermal', 'power', 'throttle', 'sensor', 'none', undefined].map(healthThrottled),
   [true, true, true, false, false, false]);

// ── Gap 0: every push is sent synchronously, as before ──
{
  const sent: number[] = [];
  const p = new TunePacer<number>((v) => sent.push(v), timers);
  for (let i = 0; i < 5; i++) p.push(i);
  eq('gap 0: all five sent at once, in order, with no timer', [sent, queue.length], [[0, 1, 2, 3, 4], 0]);
}

// ── Loaded: 50 tunes in a second ──
{
  now = 0; queue = [];
  const sent: number[] = [];
  const p = new TunePacer<number>((v) => sent.push(v), timers);
  p.setGap(TUNE_PACE_LOADED_MS);
  for (let i = 0; i < 50; i++) { p.push(i); advance(20); }
  advance(1000);
  eq('loaded: the first tune goes AT ONCE (a single click never waits)', sent[0], 0);
  eq('loaded: the LAST tune always lands', sent[sent.length - 1], 49);
  eq('loaded: about one per 150 ms — 8 sends for 50 tunes over 1 s', sent.length, 8);
  eq('loaded: never a backwards step (latest wins, in order)', sent.every((v, i) => i === 0 || v > sent[i - 1]), true);
}

// ── Recovery releases a held tune at once ──
{
  now = 0; queue = [];
  const sent: number[] = [];
  const p = new TunePacer<number>((v) => sent.push(v), timers);
  p.setGap(TUNE_PACE_CHOKED_MS);
  p.push(1); p.push(2);
  eq('choked: the second tune is held', [sent, p.holding()], [[1], true]);
  advance(100);
  p.setGap(0);
  eq('the server recovers: the held tune goes NOW, not 250 ms later', [sent, p.holding()], [[1, 2], false]);
}

// ── Flush, cancel ──
{
  now = 0; queue = [];
  const sent: number[] = [];
  const p = new TunePacer<number>((v) => sent.push(v), timers);
  p.setGap(TUNE_PACE_CHOKED_MS);
  p.push(1); p.push(2); p.push(3);
  p.flush();
  eq('flush (leaving, closing) sends the held tune — the last tune is never dropped', sent, [1, 3]);
  p.push(4);
  p.cancel();
  advance(1000);
  eq('cancel (DAB took the dial) drops a held tune unsent', sent, [1, 3]);
}

console.log(`tune pace: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
