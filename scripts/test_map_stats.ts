// test_map_stats.ts — ★★★ beta97 THE STATS CARD in the app (src/utils/mapStats.ts — the web's asStatsSync rule): per mode,
// NEWEST / FURTHEST / FURTHEST TRACKED / LONGEST TRACKED; modes never compete; bounded and forgotten.
//   npx tsx scripts/test_map_stats.ts
import { statsSeen, statsSections, statsObsOfShared, nmBetween, STATS_CAP, type StatsStore } from '../src/utils/mapStats';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const rx = { lat: 52, lon: 0 };
const t0 = 1_000_000;
const st: StatsStore = new Map();
// the map opens: what is already there is not NEWEST
statsSeen(st, [{ mode: 'ADS-B', key: 'a:1', lat: 52.5, lon: 0, name: 'BAW1' }, { mode: 'VDL2', key: 'a:9', lat: 55, lon: 2, name: 'DLH9' }], t0);
let s = statsSections(st, rx, t0, t0);
ok(s.length === 2 && s[0].mode === 'ADS-B' && s[1].mode === 'VDL2', 'a section per mode present, ADS-B first');
ok(!s[0].rows.some((r) => r.label === 'NEWEST'), 'what was there when the map opened is not NEWEST');
// later: a new aircraft, the first one moved
statsSeen(st, [{ mode: 'ADS-B', key: 'a:2', lat: 54, lon: 0, name: 'RYR2' }, { mode: 'ADS-B', key: 'a:1', lat: 52.9, lon: 0, name: 'BAW1' }], t0 + 3 * 60000);
s = statsSections(st, rx, t0 + 3 * 60000, t0);
const a = s[0].rows;
ok(a.find((r) => r.label === 'NEWEST')?.name === 'RYR2', 'NEWEST: the one first heard since');
ok(a.find((r) => r.label === 'FURTHEST')?.name === 'RYR2' && a.find((r) => r.label === 'FURTHEST')?.value === '120 NM', `FURTHEST from the receiver (${a.find((r) => r.label === 'FURTHEST')?.value})`);
ok(a.find((r) => r.label === 'FURTHEST TRACKED')?.name === 'BAW1', 'FURTHEST TRACKED: the distance flown while heard');
ok(a.find((r) => r.label === 'LONGEST TRACKED')?.name === 'BAW1' && /\+$/.test(a.find((r) => r.label === 'LONGEST TRACKED')!.value), 'LONGEST TRACKED: there since the map opened (+)');
ok(s.every((x) => x.mode === 'ADS-B' || !x.rows.some((r) => r.name === 'RYR2')), 'modes never compete');
// a 300 NM jump is a bad fix
statsSeen(st, [{ mode: 'ADS-B', key: 'a:2', lat: 60, lon: 0, name: 'RYR2' }], t0 + 4 * 60000);
ok(st.get('ADS-B')!.get('a:2')!.nm < 1, 'a jump of 300+ NM is a bad fix, never distance flown');
// the server's tracked_nm wins when larger
statsSeen(st, [{ mode: 'ADS-B', key: 'a:1', lat: 52.9, lon: 0, name: 'BAW1', trackedNm: 250 }], t0 + 5 * 60000);
ok(st.get('ADS-B')!.get('a:1')!.nm === 250, 'the server’s own tracked distance is taken when it is larger');
// stale: off the card after 2 min, forgotten after 10
s = statsSections(st, rx, t0 + 8 * 60000, t0);
ok(!s.some((x) => x.mode === 'VDL2'), 'a mode heard nothing for 2 min leaves the card');
statsSections(st, rx, t0 + 20 * 60000, t0);
ok(!st.get('VDL2')?.size, 'forgotten 10 min after the last sighting');
// bounded
const big: StatsStore = new Map();
statsSeen(big, Array.from({ length: STATS_CAP + 50 }, (_, i) => ({ mode: 'ADS-B', key: `k${i}`, lat: 50, lon: i / 1000, name: '' })), t0);
ok(big.get('ADS-B')!.size === STATS_CAP, `≤ ${STATS_CAP} a mode`);
ok(Math.abs(nmBetween({ lat: 0, lon: 0 }, { lat: 1, lon: 0 }) - 60) < 0.2, 'a degree of latitude ≈ 60 NM');
ok(statsSections(new Map(), null, t0, t0).length === 0, 'nothing heard: no sections (the STATS chip is not offered)');
// ★★ 11-28 each mode its OWN fixes: heard on ADS-B (near), then moved to HFDL far away — ADS-B's FURTHEST never takes the HFDL fix
{
  const fx: StatsStore = new Map();
  statsSeen(fx, statsObsOfShared([{ key: '4CA123', lat: 52.5, lon: 0, posSrc: 'ADS-B', flight: 'EIN1', src: ['ADS-B'] } as any]), t0);
  statsSeen(fx, statsObsOfShared([{ key: '4CA123', lat: 40, lon: -40, posSrc: 'HFDL', flight: 'EIN1', src: ['ADS-B', 'HFDL'] } as any]), t0 + 60000);
  const sx = statsSections(fx, rx, t0 + 60000, t0);
  const adsb = sx.find((x) => x.mode === 'ADS-B')?.rows.find((r) => r.label === 'FURTHEST');
  const hfdl = sx.find((x) => x.mode === 'HFDL')?.rows.find((r) => r.label === 'FURTHEST');
  ok(adsb?.value === '30 NM' && adsb.lat === 52.5, `★★ ADS-B FURTHEST keeps the ADS-B fix (${adsb?.value}), never the HFDL one`);
  ok(!!hfdl && Number.parseInt(hfdl.value, 10) > 1000, `★★ the HFDL fix is HFDL's FURTHEST (${hfdl?.value})`);
  ok(statsObsOfShared([{ key: 'X', lat: 50, lon: 0, src: ['ADS-B', 'HFDL'] } as any]).length === 0, 'a row with no posSrc is nobody\u2019s fix');
}
console.log(`\n${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
