// test_digi_map_stats.ts — ★★ beta92 THE DIGI MODES MAP's STATS / LEGEND / SUMMARY (src/utils/digiMapStats.ts), in node.
//   The very text both map pages embed (the web's window — main.ts spotsMapHtml — and the app's WebView — MapOverlay.tsx)
//   is run here: the numbers for a known set of stations, the words, the order-independence (each page feeds its spots in
//   its own order), mode FAMILIES (FST4W-1 and FST4W-3 are one FST4W row, Stuart 11-23), and that both pages embed it
//   rather than a copy of their own.
//   npx tsx scripts/test_digi_map_stats.ts
import { readFileSync } from 'node:fs';
import { DIGI_STATS_JS, digiPanelsDefault, digiPanelsOf, DIGI_PANELS, type DigiStatItem, type DigiStats } from '../src/utils/digiMapStats';
import { digiModeFamily } from '../src/utils/digiModes';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };

ok(!/[`\\]/.test(DIGI_STATS_JS) && !/<\/script/i.test(DIGI_STATS_JS), 'the shared text has no backtick, backslash or closing script tag (it sits inside the pages)');
// eslint-disable-next-line no-new-func
const lib = new Function(`${DIGI_STATS_JS}; return { digiStats: digiStats, digiStatsHtml: digiStatsHtml };`)() as {
  digiStats: (items: DigiStatItem[], me: { lat: number; lon: number } | null) => DigiStats;
  digiStatsHtml: (st: DigiStats, o: unknown) => { stats: string; legend: string; summary: string };
};

const me = { lat: 51.5, lon: -0.1 };   // London
const it = (call: string, country: string, band: string, mode: string, km: number | null, lat: number, lon: number, t: number, snr = -10): DigiStatItem =>
  ({ call, grid: 'AA00', country, band, mode, fam: digiModeFamily(mode), snr, km, lat, lon, t });
const items: DigiStatItem[] = [
  it('G4AAA', 'England', '20m', 'FT8', 50, 52.0, -0.1, 1000),          // north
  it('F5BBB', 'France', '20m', 'FT8', 340, 48.8, 2.3, 2000),           // south-east
  it('DL1CC', 'Germany', '40m', 'FT4', 930, 52.5, 13.4, 3000, 5),      // east
  it('K1DDD', 'United States', '20m', 'FST4W-3', 5300, 42.0, -71.0, 4000),   // west
  it('W2EEE', 'United States', '30m', 'FST4W-1', 5500, 40.7, -74.0, 5000),   // west
  it('VK3FFF', 'Australia', '20m', 'WSPR', 16900, -37.8, 145.0, 6000, -25),  // east-ish (great circle)
];
const st = lib.digiStats(items, me);
ok(st.n === 6 && st.withKm === 6, `six stations, six with a distance (${st.n}/${st.withKm})`);
ok(st.dist.length === 4 && st.dist.reduce((a, d) => a + d.n, 0) === 6, `four distance bands holding every station (${st.dist.map((d) => `${d.lo}-${d.hi}:${d.n}`).join(' ')})`);
ok(st.dist[0].lo === 50 && st.dist[3].hi === 16900, 'the bands run from the closest to the farthest');
ok(st.closest?.call === 'G4AAA' && st.farthest?.call === 'VK3FFF', 'closest G4AAA, farthest VK3FFF');
ok(!!st.bearing && st.bearing.length === 8 && st.bearing.reduce((a, b) => a + b.n, 0) === 6, `eight bearing sectors (${st.bearing?.map((b) => b.d + b.n).join(' ')})`);
ok(st.bearing![0].n === 1 && st.bearing![6].n === 2, 'G4AAA north, the two Americans west');
ok(st.modes[0][0] === 'FST4W' && st.modes[1][0] === 'FT8' && st.modes.some(([m, n]) => m === 'FST4W' && n === 2) && !st.modes.some(([m]) => /FST4W-/.test(m)),
   `modes by FAMILY — FST4W-1 and FST4W-3 are one FST4W row (${st.modes.map(([m, n]) => m + ' ' + n).join(', ')})`);
ok(st.countries[0][0] === 'United States' && st.countries[0][1] === 2 && st.nCountries === 5, 'top country, five countries');
ok(st.rarest?.[0] === 'Australia', `rarest country: ties broken by name (${st.rarest?.join(' ')})`);
ok(st.bands[0][0] === '20m' && st.bands[0][1] === 4 && st.nBands === 3, 'bands');
ok(st.latest?.call === 'VK3FFF', 'latest');

// ★ the same stations in any order: the same answer (each page feeds its own order)
const shuffled = [items[3], items[5], items[0], items[4], items[2], items[1]];
ok(JSON.stringify(lib.digiStats(shuffled, me)) === JSON.stringify(st), 'order does not change a single number');
const html = lib.digiStatsHtml(st, {}), html2 = lib.digiStatsHtml(lib.digiStats(shuffled, me), {});
ok(JSON.stringify(html) === JSON.stringify(html2), 'nor a single word of the panels');

console.log('── the panels ──');
const pan = lib.digiStatsHtml(st, {
  modes: [{ m: 'FT8', colour: '#E69F00', ink: '#000', letter: 'F' }], modeColour: () => '#E69F00', rx: '<svg width="12"></svg>',
  aprs: { n: 2, on: true, far: { km: 80, call: 'M0XYZ-9' }, leg: [{ svg: '<svg width="16"></svg>', words: 'car' }], only: false, newest: '' },
  svgOk: (x: string) => x, overlay: { label: 'ISS PASS', colour: '#88CCEE' },
  tot: { srvOk: true, winWords: 'the last 24 hours', drawn: 6, total: 9, trunc: true, unplaced: 3, decodesToday: 1234, falseToday: 2, notKeptToday: 0, spotsSent: 15000 },
});
for (const w of ['DISTANCE', 'BEARING', 'RANGE', 'TOP COUNTRIES', 'BY MODE', 'BY BAND', 'APRS', 'TOTALS', 'Decodes today (UTC)',
                 'Heard without a locator', 'the newest 6 of 9', 'Set aside as false decodes'])
  ok(pan.stats.includes(w), `stats: ${w}`);
for (const w of ['Receiver', 'FT8', 'FST4W', 'APRS (its symbol where it sends one)', 'ISS PASS', 'Colour + letter = mode · size = SNR'])
  ok(pan.legend.includes(w), `legend: ${w}`);
ok(/data-mode="FST4W"[^]*?<span class="lc">2<\/span>/.test(pan.legend), 'a mode the server did not list still gets its row, with its count');
for (const w of ['LATEST', 'RAREST COUNTRY', 'Australia (1)', 'SPOTS', '15,000', 'from 6 transmitters · 5 countries'])
  ok(pan.summary.includes(w), `summary: ${w}`);
const noRx = lib.digiStatsHtml(lib.digiStats(items.map((x) => ({ ...x, km: null })), null), {});
ok(noRx.stats.includes('No receiver location published') && !noRx.stats.includes('BEARING') && !noRx.stats.includes('DISTANCE'),
   'no receiver: no distances, no bearing, and it says why');
const empty = lib.digiStatsHtml(lib.digiStats([], me), {});
ok(empty.summary.includes('WAITING') && empty.stats.includes('Nothing yet'), 'nothing heard: WAITING, "Nothing yet"');
// ★ beta93 (Stuart 11-24): APRS on the map and no digital spots — the card names the APRS station, not WAITING
const aprsOnly = lib.digiStatsHtml(lib.digiStats([], me), { aprs: { n: 1, on: true, far: null, leg: [], only: false, newest: 'M0UNC-9' } });
ok(!aprsOnly.summary.includes('WAITING') && aprsOnly.summary.includes('1 station') && aprsOnly.summary.includes('M0UNC-9'),
   'APRS only: "APRS · 1 station · newest M0UNC-9", not WAITING');
const evil = lib.digiStatsHtml(lib.digiStats([it('<img src=x onerror=1>', '<b>X</b>', '20m', 'FT8', 10, 51, 0, 1)], me), {});
ok(!/<img|<b>X/.test(evil.stats + evil.summary + evil.legend), 'off-air text is escaped wherever it lands');

console.log('── both pages run this text ──');
const app = readFileSync('src/components/MapOverlay.tsx', 'utf8'), web = readFileSync('web/client/src/main.ts', 'utf8');
ok(/\$\{kind === 'digi' \? DIGI_STATS_JS : ''\}/.test(app) && /digiStatsHtml\(/.test(app), 'the app\'s map embeds DIGI_STATS_JS and draws from it');
ok(/\$\{DIGI_STATS_JS\}/.test(web) && /digiStatsHtml\(/.test(web), 'the web\'s map embeds DIGI_STATS_JS and draws from it');
ok(!/function barList\(|function tally\(/.test(web), 'the web keeps no copy of its own (barList / tally gone)');
ok(!/var ct=Object\.keys\(cm\)[^\n]*slice\(0,3\)/.test(app) || /kind === 'cw' \? `\s*<div id="statsbtnwrap">/.test(app),
   'the app\'s old three-country sheet is CW\'s only');

console.log('── which panels open ──');
ok(JSON.stringify(digiPanelsDefault(true)) === '{"stats":true,"summary":true,"legend":true}', 'a wide window: all three, beside the map (the web\'s)');
ok(JSON.stringify(digiPanelsDefault(false)) === '{"stats":false,"summary":true,"legend":false}', 'a narrow window: the summary strip; the rest a key away');
ok(digiPanelsOf('{"stats":true}', false).stats === true && digiPanelsOf('junk', true).legend === true, 'a remembered choice, and junk → the default');
ok(DIGI_PANELS.map((p) => p.label).join(' ') === 'STATS SUMMARY LEGEND', 'the keys: the web\'s checkboxes');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
