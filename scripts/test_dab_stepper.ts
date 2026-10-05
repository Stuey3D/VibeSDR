/**
 * DAB block stepping from the tuning keys, and what leaving DAB does to the live station
 * (src/services/dabStepper.ts).
 *
 * ★★★ Stuart, 2026-10-05: "In DAB mode with the rotary drum control the drum is too sensitive and causes
 *     super fast tunes which then cause the server to have a massive nightmare trying to keep up and it
 *     goes erratic switching to NFM etc." — a burst of presses must reach the server as ONE block, the
 *     one the user stopped on, while the label follows every press.
 * ★★★ Stuart, 2026-10-05: "After exiting DAB mode I went to the airband and the DAB VTS was stuck in
 *     place … it stayed and showed the last tuned station name." — leaving DAB drops a DAB station.
 *
 * Run: node --no-warnings scripts/test_dab_stepper.ts   (run-tests.sh does)
 */
import { DabBlockStepper, dabWrap, liveStationAfterDab } from '../src/services/dabStepper.ts';

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
};
const advance = (ms: number) => {
  now += ms;
  for (;;) {
    const due = queue.filter(t => t.at <= now).sort((a, b) => a.at - b.at)[0];
    if (!due) break;
    queue = queue.filter(t => t !== due);
    due.fn();
  }
};

const N = 41;
let sent: number[] = [];
let shown: number[] = [];
const mk = () => {
  sent = []; shown = []; queue = []; now = 0;
  return new DabBlockStepper(N, i => sent.push(i), i => shown.push(i), 450, timers);
};

// ── wrap ──
eq('wrap -1 → last', dabWrap(-1, N), N - 1);
eq('wrap N → 0', dabWrap(N, N), 0);
eq('wrap -43 → 39', dabWrap(-43, N), 39);

// ── one press: label at once, server after the settle ──
{
  const s = mk();
  s.step(32, 1);
  eq('single press: label moves immediately', shown, [33]);
  eq('single press: nothing sent yet', sent, []);
  eq('single press: pending is the target', s.pending(), 33);
  advance(449);
  eq('single press: still nothing at 449 ms', sent, []);
  advance(1);
  eq('single press: ONE send at the settle', sent, [33]);
  eq('single press: pending clears after send', s.pending(), -1);
}

// ── ten fast presses = ten blocks on the label, ONE tune on the server ──
{
  const s = mk();
  for (let k = 0; k < 10; k++) { s.step(32, 1); advance(100); }   // `current` lags — the server has not moved
  eq('burst: label walked ten blocks', shown, [33, 34, 35, 36, 37, 38, 39, 40, 0, 1]);
  eq('burst: nothing sent while pressing', sent, []);
  advance(450);
  eq('burst: exactly one send, the block stopped on', sent, [1]);
}

// ── back and forth inside a burst ──
{
  const s = mk();
  s.step(10, 1); s.step(10, 1); s.step(10, -1);
  advance(500);
  eq('there and back: one send, net +1', sent, [11]);
}

// ── a drum/wheel delta of several blocks at once ──
{
  const s = mk();
  s.step(5, 3);
  advance(500);
  eq('multi-step delta lands in one send', sent, [8]);
}

// ── unknown current block starts from 0 (as the old code did) ──
{
  const s = mk();
  s.step(-1, 1);
  advance(500);
  eq('no block known: steps from 0', sent, [1]);
}

// ── cancel: leaving DAB, or a pick from the wrist / a bookmark, kills the burst ──
{
  const s = mk();
  s.step(20, 1); s.step(20, 1);
  s.cancel();
  advance(1000);
  eq('cancelled burst sends nothing', sent, []);
  eq('cancelled burst is not pending', s.pending(), -1);
  s.step(20, 1);
  advance(500);
  eq('after cancel, steps from the real current block again', sent, [21]);
}

// ── two separate bursts, two sends ──
{
  const s = mk();
  s.step(0, 1); advance(500);
  s.step(1, 1); advance(500);
  eq('two settled presses = two sends', sent, [1, 2]);
}

// ── zero steps is a no-op ──
{
  const s = mk();
  s.step(4, 0); advance(500);
  eq('zero steps: nothing shown', shown, []);
  eq('zero steps: nothing sent', sent, []);
}

// ── leaving DAB ──
const dabSt = { name: 'Absolute 80s', badge: 'DAB', sid: 'C6D6', dabPlus: true };
const rdsSt = { name: 'BBC R2', badge: 'RDS', pi: 'C202' };
eq('exit DAB: a DAB station is dropped', liveStationAfterDab(true, false, dabSt), {});
eq('exit DAB: an RDS station that already replaced it is kept (same object)',
   liveStationAfterDab(true, false, rdsSt) === rdsSt, true);
eq('still in DAB: kept', liveStationAfterDab(true, true, dabSt) === dabSt, true);
eq('entering DAB: kept', liveStationAfterDab(false, true, dabSt) === dabSt, true);
eq('never in DAB: kept', liveStationAfterDab(false, false, rdsSt) === rdsSt, true);
eq('exit DAB with nothing live: unchanged', liveStationAfterDab(true, false, {}), {});

console.log(`dab stepper: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
