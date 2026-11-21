// test_spot_ranks.ts — the gallery's ranking card (src/utils/spotRanks.ts): the best place per measure, the source and
// its age named, nothing at all when reporting is off, the answer is for another call, or nothing is ranked.
import { rankCard } from '../src/utils/spotRanks';
let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const now = 1_800_003_600;
const j = { on: true, call: 'G4ABC', fetched: now - 600, ranks: { call: 'G4ABC',
  psk: { fetched: now - 840, window: 'last 24 hours', source: 'PSK Reporter', bands: { '40m': { reports: 5, reportsN: 3000, countries: 9 }, '60m': { reports: 12, countries: 2 } } },
  wspr: { fetched: now - 840, window: 'last 24 hours', source: 'wspr.live (WSPRnet spots)', bands: { '40m': { rank: 3, n: 431 }, '20m': { rank: 7, n: 800 } } } } };
const c = rankCard(j, now)!;
ok(c.title === 'G4ABC · PSK Reporter: No. 5 on 40 m for reports · No. 2 on 60 m for countries · WSPR: No. 3 on 40 m (431 stations heard)', 'the best place per measure, by band');
ok(c.src === 'PSK Reporter’s statistics, last 24 hours, 14 min ago · wspr.live (WSPRnet spots), last 24 hours, 14 min ago', 'each source named, with its window and age');
ok(rankCard({ ...j, on: false }, now) === null, 'reporting off: no card');
ok(rankCard({ on: true }, now) === null && rankCard(null, now) === null, 'not fetched yet: no card');
ok(rankCard({ ...j, ranks: { ...j.ranks, call: 'M0XYZ' } }, now) === null, 'an answer for another callsign: no card');
ok(rankCard({ ...j, ranks: { call: 'G4ABC', psk: null, wspr: null } }, now) === null, 'not ranked anywhere: no card');
const onlyW = rankCard({ ...j, ranks: { call: 'G4ABC', psk: null, wspr: j.ranks.wspr } }, now)!;
ok(onlyW.title === 'G4ABC · WSPR: No. 3 on 40 m (431 stations heard)' && !onlyW.src.includes('PSK'), 'WSPR only (the WSPRnet switch on, not in PSK Reporter’s top 50)');
ok(rankCard({ ...j, ranks: { call: 'G4ABC', psk: { bands: { 'All': { reports: 1 }, '40m': { reports: -3 } } } } }, now) === null, 'no band name, a nonsense place: ignored');
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
