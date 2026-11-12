/**
 * The R36S's DAB meter (src/dab_quality.h) against the app's and the web's (src/utils/dabQuality.ts): scripted `dab`
 * report sequences run through the TS meter; every push and what it said is written as one JSON line for
 * dab_quality_test.cpp to replay through the port. Run by run_dab_quality_test.sh (and scripts/run-tests.sh).
 *   node --no-warnings handheld-ui/test/gen_dab_quality_golden.ts > out.jsonl
 * ★ To stdout, not node:fs — so the app's type check (no @types/node) has nothing new to say about this file.
 */
import { DabQualityMeter, dabQualityLine, type DabQualityReport } from '../../src/utils/dabQuality.ts';

const out: string[] = [];
function run(name: string, reports: Array<[number, DabQualityReport]>) {
  const m = new DabQualityMeter();
  for (const [t, r] of reports) {
    const q = m.push(r, t);
    out.push(JSON.stringify({ name, t, r, level: q.level, label: q.label, short: q.short, advice: q.advice, detail: q.detail ?? '',
                              starting: !!q.starting, mer: q.merDb ?? null, line: dabQualityLine(q) }));
  }
}
const L = (o: DabQualityReport): DabQualityReport => ({ channel: '11D', locked: true, fibRate: 0.99, mer: 22, ...o });

// ★★★ 2026-11-12, Stuart's RSP1A: no station chosen, an older server's garbage counters → "Choose a station", strong
{
  const seq: Array<[number, DabQualityReport]> = [[0, { channel: '7D', locked: false }]];
  for (let i = 1; i <= 6; i++) seq.push([i * 1000, L({ channel: '7D', fibRate: 0.985, mer: 23.4, sid: 0, sfTried: 8 * i, sfOk: 0, auIn: 20 * i, auBad: 20 * i, frames: 40 * i, erased: 0 })]);
  run('no-service', seq);
}
// …a service chosen: starting, the grace running out, the first unit, clean, then real loss
{
  const seq: Array<[number, DabQualityReport]> = [];
  let tried = 0, ok = 0, t = 0;
  const p = (dt: number, dk: number) => { tried += dt; ok += dk; seq.push([t += 1000, L({ sid: 0xc479, mscBer: 0.004, sfTried: tried, sfOk: ok, frames: 40 * t / 1000, erased: 0 })]); };
  for (let i = 0; i < 7; i++) p(8, 0);
  p(8, 1);
  for (let i = 0; i < 6; i++) p(8, 8);
  for (let i = 0; i < 4; i++) p(8, 4);
  run('starting-then-loss', seq);
}
// a service change (counters backwards) into a Layer II service that starts on its second report
{
  const seq: Array<[number, DabQualityReport]> = [];
  for (let i = 1; i <= 4; i++) seq.push([i * 1000, L({ channel: '12B', sid: 1, mp2In: 42 * i, mp2Bad: 0, scfConcealed: i })]);
  seq.push([5000, L({ channel: '12B', sid: 2, mp2In: 20, mp2Bad: 20 })]);
  for (let i = 1; i <= 5; i++) seq.push([5000 + i * 1000, L({ channel: '12B', sid: 2, mp2In: 20 + 42 * i, mp2Bad: 20 + (i === 1 ? 2 : 3), scfConcealed: 2 * i })]);
  run('mp2-change', seq);
}
// weak signal, never starts: MER 8.5 → weak on signal alone; FIB 97 % moderate; the BER ceiling after a start
{
  const seq: Array<[number, DabQualityReport]> = [];
  for (let i = 1; i <= 6; i++) seq.push([i * 1000, L({ channel: '10D', mer: 8.5, fibRate: 0.97, sid: 5, mscBer: 0.09, sfTried: 8 * i, sfOk: 0 })]);
  for (let i = 1; i <= 6; i++) seq.push([6000 + i * 1000, L({ channel: '10D', mer: 10.6, fibRate: 0.98, sid: 5, mscBer: 0.05, sfTried: 48 + 8 * i, sfOk: 8 * i, auIn: 30 * i, auBad: 0 })]);
  run('weak-and-ber', seq);
}
// no FIC (false lock), then unlocked
{
  const seq: Array<[number, DabQualityReport]> = [];
  for (let i = 1; i <= 3; i++) seq.push([i * 1000, L({ channel: '5A', fibRate: 0.01, sid: 0 })]);
  for (let i = 4; i <= 6; i++) seq.push([i * 1000, { channel: '5A', locked: false }]);
  run('false-lock', seq);
}
console.log(out.join('\n'));
