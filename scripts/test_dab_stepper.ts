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
 * ★★★ Stuart, 2026-10-05 23:06 (iPhone → Pi 2): "I pressed the Exit DAB button which took me back to MW …
 *     after a few seconds the DAB decoder box popped up again this time over the MW signal and needed to
 *     have exit DAB pressed again." — a late `dab` report re-opened the box, and its EXIT (a toggle) then
 *     ENTERED DAB. The ghost model at the bottom replays that night's order of events.
 *
 * Run: node --no-warnings scripts/test_dab_stepper.ts   (run-tests.sh does)
 */
import { DabBlockStepper, dabWrap, liveStationAfterDab, DabExitGuard, dabExitAction,
         DAB_EXIT_AWAIT_OFF_MS, DAB_EXIT_TAIL_MS } from '../src/services/dabStepper.ts';

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

// ── EXIT DAB STICKS (2026-10-05 23:06, Pi 2) ──
// The guard on its own.
{
  const g = new DabExitGuard();
  eq('never left: a report is believed', g.accept(0), true);
  g.left(1000);
  eq('report on the wire as we left: dropped', g.accept(1150), false);
  g.offConfirmed(1200);
  eq('straggler after dab_off (server that races): dropped', g.accept(1300), false);
  eq('after the tail: believed again (someone else may enter DAB)', g.accept(1200 + DAB_EXIT_TAIL_MS), true);
  g.left(5000);
  eq('dab_off never comes: still quiet just inside the bound', g.accept(5000 + DAB_EXIT_AWAIT_OFF_MS - 1), false);
  eq('dab_off never comes: the bound ends the quiet', g.accept(5000 + DAB_EXIT_AWAIT_OFF_MS), true);
  g.left(20000); g.entered();
  eq('our own re-entry: its first report is believed at once', g.accept(20001), true);
  const h = new DabExitGuard();
  h.offConfirmed(100);   // somebody else ended DAB — we never asked
  eq('a server-driven exit opens no quiet', h.accept(101), true);
}
eq('EXIT while in DAB leaves', dabExitAction(true), 'leave');
eq('EXIT on a box left open out of DAB only closes it — never enters', dabExitAction(false), 'close');

/* The screen, reduced to the three things the bug was made of: the DAB flag, the box, and what the
 * server was told. Mirrors SDRScreen's onDab (report / dab_off) and the box's EXIT. */
{
  const g = new DabExitGuard();
  const st = { dabOn: true, box: true, sent: [] as string[] };
  const exitPress = (t: number) => {
    if (dabExitAction(st.dabOn) === 'leave') { st.sent.push('off'); g.left(t); }
    st.dabOn = false; st.box = false;
  };
  const report = (t: number) => { if (!g.accept(t)) return; if (!st.dabOn) st.box = true; st.dabOn = true; };
  const dabOff = (t: number) => { g.offConfirmed(t); st.dabOn = false; st.box = false; };
  // 23:06:17 EXIT; a report already on the wire lands before dab_off, another (racing server) after it.
  exitPress(0); report(120); dabOff(200); report(260);
  eq('ghost reports after EXIT do not re-open the box', st.box, false);
  eq('…nor put the app back in DAB', st.dabOn, false);
  // 23:06:27 — had a box been on screen, the next EXIT press must not send `dab on`.
  st.box = true; exitPress(10_000);
  eq('EXIT pressed ten seconds later sends nothing that enters DAB', st.sent, ['off']);
}

console.log(`dab stepper: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
