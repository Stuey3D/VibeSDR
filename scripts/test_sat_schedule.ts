// test_sat_schedule.ts — ★★★ beta96 SCHEDULE A PASS, the pure half (src/services/satSchedule.ts, docs/v12/SAT-SCHEDULE.md):
// which key (shared with others / alone on a shared dial / single user / locked range in and out of range / own radio /
// older server), the book (add, refuse, cap, store round trip, refresh, prioritise), the plan (overlaps with and without
// PRIORITISE, remainders, three-way), the runner (AOS tune, back after LOS, back-to-back + bridge, take-over, degrade to
// reminders) and the words.
process.env.TZ = 'UTC';
import { parseSatList, satModeRows, type SatRow } from '../src/services/satellites';
import {
  SAT_SCHED_BRIDGE_S, SAT_SCHED_IDLE, SAT_SCHED_MAX, SAT_SCHED_MIN_S, SatSchedule, satPassKey, satSchedDegradedText, satSchedNowText,
  satSchedPlan, satSchedRemindDue, satSchedRowText, satSchedStep, satSchedTimeline, satSchedWhy, satSession, satSessionLine,
  type SatSchedEntry, type SatSchedPrev, type SatSchedRun,
} from '../src/services/satSchedule';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };

const now = Date.UTC(2026, 10, 26, 18, 0, 0) / 1000;   // 18:00 UTC
const P = (aosMin: number, durMin: number, maxEl: number) => ({ aos: now + aosMin * 60, los: now + (aosMin + durMin) * 60, maxEl, tca: now + (aosMin + durMin / 2) * 60, aosAz: 0, losAz: 180 });
const raw = (over: Record<string, unknown> = {}) => ({
  type: 'sat_list', time: 'ok', located: true, now,
  source: { orbits: 'x', via: 'vibesdr.net', pickPrivate: true },
  sats: [
    { id: 'M24', name: 'METEOR-M2 4', norad: 59051, note: '', age: 1, ageState: 'fresh', tx: [{ label: 'LRPT', hz: 137900000, mode: 'raw', loop: 'none', reach: true }],
      passes: [P(10, 14, 62), P(110, 12, 25)] },
    { id: 'M23', name: 'METEOR-M2 3', norad: 57166, note: '', age: 1, ageState: 'fresh', tx: [{ label: 'LRPT', hz: 137900000, mode: 'raw', loop: 'none', reach: true }],
      passes: [P(18, 12, 30)] },
    { id: 'ISS', name: 'ISS', norad: 25544, note: '', age: 1, ageState: 'fresh',
      tx: [{ label: 'Voice', hz: 145800000, mode: 'nfm', loop: 'fll', reach: true }, { label: 'APRS', hz: 145825000, mode: 'nfm', loop: 'fll', reach: false, why: 'Out of this receiver’s range — it is locked to 136–140 MHz' }],
      passes: [P(40, 10, 70)] },
  ],
  ...over,
});
const L = parseSatList(raw())!;
const rows = satModeRows(L, now);
const row = (norad: number, i = 0, later = false): SatRow => rows.find((r) => r.sat.norad === norad && r.i === i && !!r.later === later)!;

console.log('1. which key');
{
  ok(satSession({ pickPrivate: false, sharedDial: true, listeners: 3 }).kind === 'remind', 'a shared dial with others on it → REMIND');
  ok(satSession({ pickPrivate: false, sharedDial: true }).kind === 'remind', 'a shared dial, listeners not known → REMIND (never guessed into tuning)');
  const al = satSession({ pickPrivate: false, sharedDial: true, listeners: 1 });
  ok(al.kind === 'schedule' && al.why === 'alone', 'a shared dial with nobody else on it now → SCHEDULE (alone)');
  ok(satSession({ pickPrivate: true }).why === 'single' && satSession({ pickPrivate: true }).kind === 'schedule', 'a single-user radio → SCHEDULE');
  ok(satSession({ pickPrivate: true, locked: true }).why === 'locked', 'a locked range → SCHEDULE (locked)');
  ok(satSession({ pickPrivate: true, localHw: true }).why === 'local', 'this device\'s own radio → SCHEDULE (local)');
  ok(satSession({ localHw: true }).kind === 'schedule', 'own radio, server says nothing → SCHEDULE');
  ok(satSession({ localHw: true, sharedDial: true, listeners: 2 }).kind === 'remind', 'own radio SERVED on a shared dial with a listener → REMIND');
  ok(satSession({}).kind === 'none', 'an older server (no pickPrivate) → no key');
  const sch = satSession({ pickPrivate: true }).kind;
  ok(satPassKey(L, row(59051), sch) === 'schedule', 'a hearable pass to come → SCHEDULE');
  ok(satPassKey(L, row(59051, 0, true), sch) === 'schedule', 'SCHEDULE on a LATER pass too (pick the good one)');
  ok(satPassKey(L, row(25544, 1), sch) === null, 'locked range: a downlink OUTSIDE the range (greyed) → no key');
  ok(satPassKey(L, row(25544, 0), sch) === 'schedule', 'locked range: a downlink inside → SCHEDULE');
  ok(satPassKey(L, row(59051), 'remind') === 'remind' && satPassKey(L, row(59051, 0, true), 'remind') === null, 'REMIND: current-or-next pass only');
  ok(satPassKey(L, row(59051), 'none') === null, 'none → no key');
  ok(satPassKey(L, { ...row(59051), up: true, next: undefined }, sch) === null, 'a pass already up → no key');
  ok(/only while this page is open/.test(satSessionLine(al, 'web')) && /only while the app is open/.test(satSessionLine(al, 'app')), 'the line says it runs only while open (web / app)');
  ok(/become reminders/.test(satSessionLine(al, 'app')), 'alone: says it becomes reminders if someone joins');
  ok(/REMINDS you/.test(satSessionLine({ kind: 'remind', why: 'shared' }, 'web')), 'shared: says why it is REMIND');
}

const srv = 'pi500:8073';
console.log('2. the book');
{
  const b = new SatSchedule();
  ok(b.add(srv, row(59051), now), 'add METEOR-M2 4 18:10');
  ok(!b.add(srv, row(59051), now), 'the same pass twice → refused');
  ok(b.add(srv, row(59051, 0, true), now), 'its later pass is a separate entry');
  ok(!b.add(srv, row(59051), now + 11 * 60), 'AOS gone → refused');
  ok(b.find(srv, 59051, 0, now + 10 * 60 + 20)?.maxEl === 62, 'find by pass (AOS within a minute)');
  const big = new SatSchedule();
  for (let k = 0; k < SAT_SCHED_MAX; k++) big.add(srv, { ...row(59051), next: P(10 + k * 20, 10, 40) }, now);
  ok(big.size === SAT_SCHED_MAX && !big.add(srv, { ...row(59051), next: P(900, 10, 40) }, now), `capped at ${SAT_SCHED_MAX} a server`);
  ok(big.add('other', row(59051), now), 'the cap is per server');
  // store round trip
  b.add(srv, row(57166), now);
  const k3 = b.find(srv, 57166, 0, now + 18 * 60)!.key;
  b.prioritise(k3);
  const back = SatSchedule.fromJSON(b.toJSON(), now);
  ok(back.size === 3 && back.get(k3)?.prio === true && back.get(k3)?.name === 'METEOR-M2 3', 'stored and read back (priority kept)');
  ok(SatSchedule.fromJSON(b.toJSON(), now + 200 * 60).size === 0, 'passes over by the time it is read are not loaded');
  ok(SatSchedule.fromJSON('{"e":[{"server":"x","norad":"a"}]}', now).size === 0 && SatSchedule.fromJSON('nonsense', now).size === 0, 'malformed → nothing');
  // refresh with a new element set
  const moved = parseSatList(raw({ sats: raw().sats.map((s) => s.norad === 57166 ? { ...s, passes: [P(18.5, 12, 31)] } : s) }))!;
  b.refresh(srv, moved);
  const m = b.find(srv, 57166, 0, now + 18.5 * 60);
  ok(!!m && m.aos === now + 18.5 * 60 && m.maxEl === 31 && m.prio === true, 'a new element set moves the entry with its pass (priority kept)');
  b.prune(now + 31 * 60);
  ok(b.size === 1, 'prune drops passes that are over');
}

console.log('3. the plan — overlaps');
{
  const b = new SatSchedule();
  b.add(srv, row(59051), now);   // 18:10–18:24, 62°
  b.add(srv, row(57166), now);   // 18:18–18:30, 30°
  const es = b.all(srv);
  const [a, c] = [es.find((e) => e.norad === 59051)!, es.find((e) => e.norad === 57166)!];
  let plan = satSchedPlan(es);
  ok(plan.get(a.key)!.state === 'full' && plan.get(c.key)!.state === 'part', 'no choice: the higher pass (62°) wins, the other runs part');
  ok(plan.get(c.key)!.segs[0][0] === a.los && plan.get(c.key)!.segs[0][1] === c.los, 'the loser runs only after the winner\'s LOS');
  ok(plan.get(a.key)!.over.includes(c.key) && plan.get(c.key)!.over.includes(a.key), 'both know they overlap (PRIORITISE on each)');
  ok(/18:24–18:30 only — METEOR-M2 4 is the higher pass/.test(satSchedRowText(c, plan.get(c.key), es)), `loser's words: ${satSchedRowText(c, plan.get(c.key), es)}`);
  ok(/wins the overlap with METEOR-M2 3 \(the higher pass\)/.test(satSchedRowText(a, plan.get(a.key), es)), `winner's words: ${satSchedRowText(a, plan.get(a.key), es)}`);
  b.prioritise(c.key);
  plan = satSchedPlan(b.all(srv));
  ok(plan.get(c.key)!.state === 'full' && plan.get(a.key)!.state === 'part', 'PRIORITISE the lower one: it wins');
  ok(plan.get(a.key)!.segs.length === 1 && plan.get(a.key)!.segs[0][1] === c.aos, 'the other runs only before it');
  ok(/is prioritised/.test(satSchedRowText(b.get(a.key)!, plan.get(a.key), b.all(srv))), 'the loser says the other is prioritised');
  b.prioritise(a.key);
  ok(b.get(a.key)!.prio === true && !b.get(c.key)!.prio, 'PRIORITISE the other: one winner per overlap');
  b.prioritise(a.key);
  ok(!b.get(a.key)!.prio && !b.get(c.key)!.prio, 'PRIORITISE again: off (back to the higher pass)');
  // fully swallowed → skipped
  const s = new SatSchedule();
  s.add(srv, { ...row(59051), next: P(10, 20, 70) }, now);
  s.add(srv, { ...row(57166), next: P(15, 5, 20) }, now);
  const sp = satSchedPlan(s.all(srv));
  const lo = s.all(srv).find((e) => e.norad === 57166)!;
  ok(sp.get(lo.key)!.state === 'skipped' && /skipped — METEOR-M2 4 is the higher pass/.test(satSchedRowText(lo, sp.get(lo.key), s.all(srv))), 'inside the higher pass → skipped, said');
  // the higher one inside the lower: the lower runs in two pieces
  const t = new SatSchedule();
  t.add(srv, { ...row(59051), next: P(10, 20, 20) }, now);
  t.add(srv, { ...row(57166), next: P(15, 5, 70) }, now);
  const tp = satSchedPlan(t.all(srv));
  const lo2 = t.all(srv).find((e) => e.norad === 59051)!;
  ok(tp.get(lo2.key)!.segs.length === 2, 'a higher pass in the middle: the lower runs before and after');
  // a remainder too short to be worth it
  const u = new SatSchedule();
  u.add(srv, { ...row(59051), next: P(10, 10, 70) }, now);
  u.add(srv, { ...row(57166), next: P(19.5, 10, 20) }, now);
  const up = satSchedPlan(u.all(srv), SAT_SCHED_MIN_S);
  ok(up.get(u.all(srv).find((e) => e.norad === 57166)!.key)!.segs[0][1] - up.get(u.all(srv).find((e) => e.norad === 57166)!.key)!.segs[0][0] >= SAT_SCHED_MIN_S, 'the remainder kept is a real one');
  const v = new SatSchedule();
  v.add(srv, { ...row(59051), next: P(10, 10, 70) }, now);
  v.add(srv, { ...row(57166), next: P(10.5, 10.4, 20) }, now);   // 24 s left after
  ok(satSchedPlan(v.all(srv)).get(v.all(srv).find((e) => e.norad === 57166)!.key)!.state === 'skipped', `a remainder under ${SAT_SCHED_MIN_S} s → skipped`);
  // three-way
  const w = new SatSchedule();
  w.add(srv, { ...row(59051), next: P(10, 10, 30) }, now);
  w.add(srv, { ...row(57166), next: P(15, 10, 60) }, now);
  w.add(srv, { ...row(25544), next: P(22, 10, 45) }, now);
  const wp = satSchedPlan(w.all(srv));
  const tl = satSchedTimeline(wp);
  let clash = false;
  for (let i = 1; i < tl.length; i++) if (tl[i].start < tl[i - 1].end) clash = true;
  ok(!clash && tl.length === 3, 'three overlapping passes: the timeline never has two at once');
}

console.log('4. the runner');
{
  const here: SatSchedPrev = { hz: 101100000, mode: 'wfm', sat: false, pick: null };
  const b = new SatSchedule();
  b.add(srv, row(59051), now);   // 18:10–18:24
  const es = b.all(srv); const e = es[0];
  let run: SatSchedRun = SAT_SCHED_IDLE;
  let st = satSchedStep(run, es, now + 9 * 60, { pick: null }, here, 'schedule');
  ok(st.act.act === 'none', 'before AOS: nothing');
  st = satSchedStep(st.run, es, e.aos, { pick: null }, here, 'schedule');
  ok(st.act.act === 'tune' && st.act.e.key === e.key && st.run.prev?.hz === 101100000, 'at AOS: tune, and remember where the listener was');
  run = st.run;
  st = satSchedStep(run, es, e.aos + 5, { pick: null }, { ...here, hz: 137900000 }, 'schedule');
  ok(st.act.act === 'none' && st.run.key === e.key, 'tuned once, not every second');
  st = satSchedStep(st.run, es, e.aos + 10, { pick: { norad: 59051, tx: 0 } }, here, 'schedule');
  ok(st.run.seen, 'the pick confirmed');
  st = satSchedStep(st.run, es, e.los, { pick: { norad: 59051, tx: 0 } }, here, 'schedule');
  ok(st.act.act === 'back' && st.act.prev.hz === 101100000 && st.act.prev.mode === 'wfm' && st.run.key === null, 'after LOS: back to 101.1 WFM');
  // take-over
  let r2 = satSchedStep(SAT_SCHED_IDLE, es, e.aos, { pick: null }, here, 'schedule').run;
  r2 = satSchedStep(r2, es, e.aos + 10, { pick: { norad: 59051, tx: 0 } }, here, 'schedule').run;
  r2 = satSchedStep(r2, es, e.aos + 60, { pick: { norad: 25544, tx: 0 } }, here, 'schedule').run;
  ok(r2.took && !r2.prev, 'the listener picked something else mid-pass: taken over');
  ok(/taken over/.test(satSchedNowText(r2, es, satSchedPlan(es), e.aos + 70)), 'the status line says so');
  const end = satSchedStep(r2, es, e.los + 1, { pick: { norad: 25544, tx: 0 } }, here, 'schedule');
  ok(end.act.act === 'end' && (end.act as { why: string }).why === 'took', 'after LOS: nothing put back');
  // back to back (bridge)
  const bb = new SatSchedule();
  bb.add(srv, { ...row(59051), next: P(10, 10, 50) }, now);   // 18:10–18:20
  bb.add(srv, { ...row(57166), next: P(21, 10, 40) }, now);   // 18:21–18:31 (60 s gap)
  const be = bb.all(srv);
  let rb = satSchedStep(SAT_SCHED_IDLE, be, now + 10 * 60, { pick: null }, here, 'schedule').run;
  let sb = satSchedStep(rb, be, now + 20 * 60 + 5, { pick: { norad: 59051, tx: 0 } }, { ...here, hz: 1 }, 'schedule');
  ok(sb.act.act === 'none' && sb.run.key === be[0].key, `a gap under ${SAT_SCHED_BRIDGE_S} s: stays on, no back-and-forth`);
  sb = satSchedStep(sb.run, be, now + 21 * 60, { pick: { norad: 59051, tx: 0 } }, { ...here, hz: 2 }, 'schedule');
  ok(sb.act.act === 'tune' && sb.act.e.norad === 57166 && sb.run.prev?.hz === 101100000, 'the next one tunes; the place to go back to is still the first');
  sb = satSchedStep(sb.run, be, now + 31 * 60, { pick: { norad: 57166, tx: 0 } }, here, 'schedule');
  ok(sb.act.act === 'back' && sb.act.prev.hz === 101100000, 'after the last: back');
  // overlapping: the loser starts at the winner's LOS, without going back in between
  const ov = new SatSchedule();
  ov.add(srv, row(59051), now); ov.add(srv, row(57166), now);
  const oe = ov.all(srv);
  let ro = satSchedStep(SAT_SCHED_IDLE, oe, now + 18 * 60 + 30, { pick: null }, here, 'schedule');
  ok(ro.act.act === 'tune' && ro.act.e.norad === 59051, 'in the overlap: the winner is on');
  ro = satSchedStep(ro.run, oe, now + 24 * 60, { pick: { norad: 59051, tx: 0 } }, here, 'schedule');
  ok(ro.act.act === 'tune' && ro.act.e.norad === 57166, 'at the winner\'s LOS the other takes its remaining minutes');
  // a gap longer than the bridge goes back
  const gp = new SatSchedule();
  gp.add(srv, { ...row(59051), next: P(10, 10, 50) }, now);
  gp.add(srv, { ...row(57166), next: P(40, 10, 40) }, now);
  const ge = gp.all(srv);
  const rg = satSchedStep(SAT_SCHED_IDLE, ge, now + 10 * 60, { pick: null }, here, 'schedule').run;
  ok(satSchedStep(rg, ge, now + 20 * 60, { pick: { norad: 59051, tx: 0 } }, here, 'schedule').act.act === 'back', 'a long gap: back in between');
  // degrade
  const rd = satSchedStep(SAT_SCHED_IDLE, es, e.aos, { pick: null }, here, 'schedule').run;
  const dg = satSchedStep(rd, es, e.aos + 60, { pick: { norad: 59051, tx: 0 } }, here, 'remind');
  ok(dg.act.act === 'end' && (dg.act as { why: string }).why === 'shared' && dg.run.key === null, 'someone joins mid-pass: the run ends, nothing tuned back');
  ok(satSchedStep(SAT_SCHED_IDLE, es, e.aos, { pick: null }, here, 'remind').act.act === 'none', 'shared at AOS: no tune');
  const fired = new Set<string>();
  ok(satSchedRemindDue(es, e.aos - 3 * 60, 2, fired).length === 0, 'degraded: not before the lead');
  const due = satSchedRemindDue(es, e.aos - 2 * 60, 2, fired);
  ok(due.length === 1 && due[0].norad === 59051 && due[0].los === e.los, 'degraded: fires as a reminder at AOS − lead');
  ok(satSchedRemindDue(es, e.aos - 60, 2, fired).length === 0, '…once');
  ok(satSchedRemindDue(es, e.los, 2, new Set()).length === 0, 'an over pass never fires');
  ok(/your 1 scheduled pass will remind you/.test(satSchedDegradedText(1)) && satSchedDegradedText(0) === '', 'the degraded line');
  // fire-time check
  ok(satSchedWhy(L, { norad: 25544, tx: 1, name: 'ISS' }).startsWith('Out of this receiver'), 'fire-time: out of range says why');
  ok(/no longer listed/.test(satSchedWhy(L, { norad: 1, tx: 0, name: 'X' })) && satSchedWhy(L, e) === '', 'fire-time: gone / fine');
  ok(/next: METEOR-M2 4 at 18:10 \(in 0:10\)/.test(satSchedNowText(SAT_SCHED_IDLE, es, satSchedPlan(es), now)), 'the next line');
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
