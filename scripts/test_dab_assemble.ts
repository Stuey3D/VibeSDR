// The DAB report reassembler (src/services/dabAssemble.ts) — the rules the end-to-end test cannot pin exactly:
// ageing, the tuned service at age 0, legacy pass-through, never another multiplex's list, the resync throttle.
import { DabAssembler } from '../src/services/dabAssemble.ts';

let pass = 0, fail = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  const ok = JSON.stringify(got) === JSON.stringify(want); ok ? pass++ : fail++;
  if (!ok) console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const list = { type: 'dab_list', rev: 'aaaa', channel: '12D', eid: 1, blocks: { '12D': 'Bench' },
  services: [{ sid: 1, label: 'One' }, { sid: 2, label: 'Two' }, { sid: 3, label: 'Three' }] };
const dls = { type: 'dab_dls', rev: 'bbbb', channel: '12D',
  dls: { '1': { dls: 'Now on One', dlsAge: 0, dlpRunning: true, dlp: { title: 'T' } }, '2': { dls: 'Two text', dlsAge: 30 } } };
const live = (o: Record<string, unknown> = {}) => ({ type: 'dab', v: 2, channel: '12D', sid: 1, listRev: 'aaaa', dlsRev: 'bbbb', ...o });

{
  const a = new DabAssembler();
  a.ingest(list, 1000); a.ingest(dls, 1000);
  const r = a.ingest(live(), 11_000);
  const s = (r.report as any).services;
  eq('the list comes back, in order', s.map((x: any) => x.label), ['One', 'Two', 'Three']);
  eq('the tuned service: text + DL Plus, age stays 0 (its label is live)', [s[0].dls, s[0].dlsAge, s[0].dlp], ['Now on One', 0, { title: 'T' }]);
  eq('another service ages on the CLIENT between sends: 30 s + 10 s', [s[1].dls, s[1].dlsAge], ['Two text', 40]);
  eq('a service with no text has none', 'dls' in s[2], false);
  eq('blocks from the list', (r.report as any).blocks, { '12D': 'Bench' });
  eq('revisions match: no resync', r.resync, false);
  // the tuned service changes: service 1 is now a scanned row with an age
  eq('a service that is no longer tuned ages', (a.ingest(live({ sid: 2 }), 11_000).report as any).services[0].dlsAge, 10);
}
{
  const a = new DabAssembler();
  const legacy = { type: 'dab', channel: '12D', services: [{ sid: 9, label: 'Old' }] };
  eq('an older server\'s full report passes straight through', a.ingest(legacy, 0).report, legacy);
  const trunc = { type: 'dab', channel: '12D', truncated: 3000, locked: true };
  eq('the truncation guard\'s short object passes through', a.ingest(trunc, 0).report, trunc);
}
{
  const a = new DabAssembler();
  a.ingest({ ...list, channel: '11D' }, 0);
  const r = a.ingest(live(), 0);
  eq('another multiplex\'s list is NEVER used — an empty list, not a stale one', (r.report as any).services, []);
  eq('…and the client asks for the right one', r.resync, true);
  eq('…but not again within 5 s', a.ingest(live(), 4000).resync, false);
  eq('…and again after 5 s if still missing', a.ingest(live(), 5000).resync, true);
}
{
  const a = new DabAssembler();
  a.ingest(list, 0); a.ingest(dls, 0);
  a.ingest({ type: 'dab_off' }, 0);
  eq('dab_off forgets everything', (a.ingest(live(), 0).report as any).services, []);
}
console.log(`dab assemble: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
