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
         DAB_EXIT_AWAIT_OFF_MS, DAB_EXIT_TAIL_MS, DabAskGuard, DAB_ASK_HOLD_MS } from '../src/services/dabStepper.ts';

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

// ── there and back to where the burst began: the server is already on that block, so nothing is sent ──
{
  const s = mk();
  s.step(10, 1); s.step(10, 1); s.step(10, -1); s.step(10, -1);
  advance(500);
  eq('there and back to the start: label followed every press', shown, [11, 12, 11, 10]);
  eq('there and back to the start: NOTHING sent (no re-acquire of the block we are on)', sent, []);
  eq('…and nothing is left pending', s.pending(), -1);
}

/* ── 2026-10-21, Stuart's own case: "connect at 12b and rapidly tune down to 7D hammering the multiplex
 *    button multiple times a second … tune on final move. However if I am tuning a multiplex and holding
 *    there for a second to see the waterfall then tuning again we need to tune as normal". ── */
const B = ['5A','5B','5C','5D','6A','6B','6C','6D','7A','7B','7C','7D','8A','8B','8C','8D','9A','9B','9C','9D',
           '10A','10N','10B','10C','10D','11A','11N','11B','11C','11D','12A','12N','12B','12C','12D',
           '13A','13B','13C','13D','13E','13F'];
eq('the Band III table here has 41 blocks', B.length, N);
{
  const s = mk();
  const from = B.indexOf('12B'), to = B.indexOf('7D');
  for (let k = 0; k < from - to; k++) { s.step(from, -1); advance(150); }   // ~6.7 presses a second
  eq('12B → 7D hammered: the label walked every block', shown.length, from - to);
  eq('12B → 7D hammered: the label ends on 7D', B[shown[shown.length - 1]], '7D');
  eq('12B → 7D hammered: the SDR was not tuned on the way', sent, []);
  advance(300);
  eq('12B → 7D hammered: ONE tune, to 7D, on the final move', sent.map(i => B[i]), ['7D']);
}
{
  const s = mk();
  s.step(B.indexOf('12B'), -1); advance(1000);   // one press, hold a second to look at the waterfall
  s.step(B.indexOf('12N'), -1); advance(1000);   // …then the next
  eq('press, look a second, press: each tunes as normal', sent.map(i => B[i]), ['12N', '12A']);
}

// ── the web client's arrows CLAMP at the ends of Band III ──
{
  sent = []; shown = []; queue = []; now = 0;
  const s = new DabBlockStepper(N, i => sent.push(i), i => shown.push(i), 450, timers, 'clamp');
  s.step(N - 2, 1); s.step(N - 2, 1); s.step(N - 2, 1);
  advance(500);
  eq('clamp: three presses from 13E stop at 13F', shown, [N - 1]);
  eq('clamp: one send, 13F', sent, [N - 1]);
  s.step(N - 1, 1); advance(500);
  eq('clamp: pressing on at 13F shows and sends nothing', [shown.length, sent.length], [1, 1]);
  s.step(1, -5); advance(500);
  eq('clamp: a big step down stops at 5A', sent, [N - 1, 0]);
  const w = mk();
  w.step(N - 1, 1); advance(500);
  eq('wrap (the default — the app drum, the crown): 13F + 1 = 5A', sent, [0]);
}

// ── THE SNAP-BACK GUARD (2026-10-21, the Pi 2) ──
{
  const g = new DabAskGuard();
  const b12 = B.indexOf('12B'), d7 = B.indexOf('7D'), a9 = B.indexOf('9A');
  eq('nothing asked: every report is believed', g.accept(b12, 0), true);
  g.sent(d7, b12, 1000);
  eq('asked 7D: a report still naming 12B (the block we left) is dropped', g.accept(b12, 1200), false);
  eq('…and still dropped just inside the bound', g.accept(b12, 1000 + DAB_ASK_HOLD_MS - 1), false);
  eq('a report with no block is believed', g.accept(-1, 1300), true);
  eq('the server names 7D: believed, and the guard ends', [g.accept(d7, 1500), g.pending()], [true, -1]);
  eq('after that, 12B again is somebody else tuning — believed', g.accept(b12, 1600), true);

  g.sent(d7, b12, 2000);
  eq('a THIRD block (somebody else moved the shared dial): adopted at once', [g.accept(a9, 2100), g.pending()], [true, -1]);

  g.sent(d7, b12, 3000);
  eq('the server never names 7D: the bound ends the hold and the server is followed',
     [g.accept(b12, 3000 + DAB_ASK_HOLD_MS), g.pending()], [true, -1]);

  // Two asks in a row before the server caught up: neither block we left may drag the label back.
  g.sent(B.indexOf('10A'), b12, 10_000);
  g.sent(d7, B.indexOf('10A'), 10_500);
  eq('second ask in flight: a late report of the FIRST ask is dropped', g.accept(B.indexOf('10A'), 10_600), false);
  eq('…and so is one of the block we started on', g.accept(b12, 10_700), false);
  eq('…and the hold is timed from the LATEST ask', g.accept(b12, 10_500 + DAB_ASK_HOLD_MS - 1), false);
  eq('the newest ask is what ends it', g.accept(d7, 10_800), true);

  g.sent(d7, b12, 20_000); g.clear();
  eq('cleared (left DAB / a reconnect): the next report is believed', g.accept(b12, 20_100), true);
}

/* The app's/web's report handler reduced to the label: follow the server unless the stepper is mid-burst
 * or the guard holds. Replays the Pi 2: reports twice a second, the server naming the new block 1.8 s
 * after our send. Before the guard the label read 12B → 7D → 12B → 12B → … → 7D. */
{
  const g = new DabAskGuard();
  let label = B.indexOf('12B'), server = B.indexOf('12B');
  const labels: string[] = [];
  const s = new DabBlockStepper(N, i => { g.sent(i, server, now); }, i => { label = i; }, 450, timers);
  queue = []; now = 0;
  const report = (blk: number) => {
    if (s.pending() >= 0 && blk !== s.pending()) return;   // mid-burst: the report names where we are leaving
    if (!g.accept(blk, now)) return;
    label = blk; server = blk;
  };
  for (let k = 0; k < 21; k++) { s.step(label, -1); advance(150); if (k % 3 === 0) report(server); labels.push(B[label]); }
  advance(450);                       // the send
  for (let t = 0; t < 1800; t += 500) { advance(Math.min(500, 1800 - t)); report(B.indexOf('12B')); labels.push(B[label]); }
  report(B.indexOf('7D')); labels.push(B[label]);
  eq('Pi 2 replay: the label never went back to 12B after leaving it', labels.slice(1).includes('12B'), false);
  eq('Pi 2 replay: it ends on 7D, confirmed by the server', [B[label], g.pending()], ['7D', -1]);
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
