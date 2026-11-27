// test_am_cards.ts — ★★★ beta97 THE AMATEUR MAP ON THE MAP CARDS in the app (src/utils/amCards.ts; the web's b97-maps-port):
// the page's statistics checked and turned into the STATS card's sections, a station's row finds it (its spot key), the
// LIST's one sort key; and the app's source: the old chrome deleted, the cards drawn.   npx tsx scripts/test_am_cards.ts
import { readFileSync, existsSync } from 'node:fs';
import { amStatsClean, amStatsSections, amSortRows, pageRowsClean, pageRowsAsTrack, ageShort } from '../src/utils/amCards';
import { amSpotKey } from '../src/utils/amateurMap';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };

const msg = { t: 'amStats', n: 120, nCountries: 30, nBands: 4, closest: { call: 'G4ABC', band: '20m', mode: 'FT8', km: 120, country: 'England', t: 1 },
  farthest: { call: 'VK2XY', band: '20m', mode: 'FT8', km: 16900, country: 'Australia', t: 2 }, latest: { call: 'K1ABC', band: '40m', mode: 'FT4', km: 5300, country: 'USA', t: 3 },
  rarest: ['Tonga', 1], modes: [['FT8', 100], ['FT4', 20]], bands: [['20m', 80], ['40m', 40]], countries: [['England', 40]], bearing: [{ d: 'N', n: 10, pct: 8 }, { d: 'NE', n: 0, pct: 0 }],
  aprs: { call: 'M0XYZ-9', km: 85 }, aprsN: 3 };
const st = amStatsClean(msg)!;
ok(!!st && st.n === 120 && st.farthest?.call === 'VK2XY' && st.modes.length === 2, 'the page’s statistics read back');
ok(amStatsClean({ t: 'other' }) === null && amStatsClean(null) === null, 'anything else is not statistics');
const evil = amStatsClean({ t: 'amStats', n: 'x', closest: { call: '<b>\u0000' + 'X'.repeat(50) }, modes: Array(50).fill(['M', 1]) })!;
ok(evil.n === 0 && (evil.closest?.call.length ?? 0) <= 16 && !/\u0000/.test(evil.closest?.call ?? '') && evil.modes.length === 8, 'trusted for nothing: numbers, bounded strings, capped lists');
const secs = amStatsSections(st);
ok(secs.map((x) => x.mode).join() === 'SUMMARY,RANGE,MODES,BANDS,COUNTRIES', `a section per subject (${secs.map((x) => x.mode).join(' · ')})`);
ok(secs[0].rows[0].label === 'STATIONS' && secs[0].rows[0].value === '120' && /30 countries · 4 bands/.test(secs[0].rows[0].name), 'SUMMARY: stations, countries, bands');
const far = secs[1].rows.find((r) => r.label === 'FURTHEST')!;
ok(far.value === '16,900 km' && far.key === amSpotKey('VK2XY', '20m', 'FT8'), 'FURTHEST: its distance, and its row finds the station (its spot key)');
ok(secs[1].rows.some((r) => r.label === 'APRS FURTHEST' && r.name === 'M0XYZ-9') && secs[1].rows.some((r) => r.label === 'BEARING N') && !secs[1].rows.some((r) => r.label === 'BEARING NE'),
   'RANGE: APRS furthest and the bearing sectors heard (an empty sector left out)');
ok(secs[2].rows[0].label === 'FT8' && /83% of the stations/.test(secs[2].rows[0].name), 'MODES: counts with their share');
ok(amStatsSections(null).length === 0 && amStatsSections({ ...st, n: 0 }).length === 0, 'nothing heard: no STATS (the chip is not offered)');
const rows = [{ key: 'a', range: '5,300 km' }, { key: 'b', range: '' }, { key: 'c', range: '120 km' }];
ok(amSortRows(rows, 'latest').map((r) => r.key).join() === 'a,b,c', 'LATEST: the rows’ own order (newest first)');
ok(amSortRows(rows, 'nearest').map((r) => r.key).join() === 'c,a,b', 'NEAREST: by distance, none last');

// ── the source: the AMATEUR map is on the cards; its old chrome is gone ──
const mo = readFileSync('src/components/MapOverlay.tsx', 'utf8');
ok(!existsSync('src/components/TrackLiveList.tsx') && !/TrackLiveList/.test(mo.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '')), 'TrackLiveList DELETED (its last user was the AMATEUR map)');
ok(!existsSync('src/components/DigiMapKeys.tsx') && !/DigiMapKeys|__digiPanels|__digiKeysH|digiTop|digiPanel'/.test(mo), 'DigiMapKeys DELETED with its panels’ keys and reporters (every map is on the cards)');
ok(!/<div id="topbar">|<div id="filters">|id="legbtn"|id="dstats"|id="dsum"|getElementById\('snap'\)|getElementById\('back'\)/.test(mo), 'no map page has a bar, a filter row, the legend’s (i), STATS / SUMMARY panels, SNAP / FIT or back keys');
ok(/const CARDS = !\(kind === 'ais'/.test(mo) && /window\.__mapAuto=\{state:function\(\)\{return \{on:snapOn,paused:false\};\}/.test(mo) && /function pageRowsTell\(fn\)/.test(mo),
   'HFDL: SNAP is MAP CONTROLS\u2019 AUTO; every map off the track host tells React Native its LIST rows');
ok(/\{amCards && !!amList && !!pageUri && !packError && \(\s*<MapChrome /.test(mo) && /servers=\{null\} tune=\{null\} chat=\{null\} shared=\{null\}/.test(mo),
   'the AMATEUR map draws MapChrome — no SERVERS, TUNE, CHAT or waterfall');
ok(/ranges: \[\.\.\.DIGI_WINDOWS, \.\.\.\(amateur \? \[SESSION_RANGE\] : \[\]\)\]/.test(mo) && /extra: <AmFilters /.test(mo) && /pageCall\('__digiSet', k, v\)/.test(mo), 'MAP CONTROLS: TIME (windows + THIS SESSION), MODE / BAND / RADIO');
ok(/amStatsTell\(st,ak\);/.test(mo) && /button:null,what:'station shown'/.test(mo) && /window\.__mapAuto=\{state:function\(\)\{return digiFit\?digiFit\.state\(\)/.test(mo),
   'the spots maps: the numbers go to the STATS card; AUTO / FIT = the shared auto fit from MAP CONTROLS (no key in the page)');

// ── the LIST of the maps off the track host: the page's own rows ──
{
  const now = 1_000_000_000_000;
  const rows = pageRowsClean([{ key: 'G4ABC|20m|FT8', name: 'G4ABC', second: 'FT8 · 20m · England', t: now - 30_000, km: 120.4, mode: 'FT8' },
    { key: '', name: 'nokey' }, 'junk', { key: 'X'.repeat(500), name: '<b>'.repeat(20), t: 'x', km: -5 }]);
  ok(rows.length === 2 && rows[0].km === 120.4 && rows[1].key.length === 120 && rows[1].km === null && rows[1].t === 0, 'page rows trusted for nothing: keys required, strings bounded, a bad distance dropped');
  const tr = pageRowsAsTrack(rows, now);
  ok(tr[0].range === '120 km' && tr[0].age === 'now' && tr[0].name === 'G4ABC' && tr[1].range === '', 'drawn as the cards\u2019 LIST rows (distance, age)');
  ok(ageShort(now - 5 * 60_000, now) === '5 min' && ageShort(now - 3 * 3_600_000, now) === '3 h', 'ages read short');
  ok(amSortRows(tr, 'nearest')[0].key === 'G4ABC|20m|FT8', 'nearest first sorts by the row\u2019s distance');
}

console.log(`\n${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
