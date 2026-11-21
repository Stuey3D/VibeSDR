// test_digi_map.ts — V12 beta90 THE DIGI MODES MAP's client halves (docs/v12/DIGI-MODES-MAP.md), in node:
//   · src/services/digiStations.ts — the station store's answer cleaned (markup and junk never pass), a live poll merged,
//     a sliding window, only stations with a locator become map rows;
//   · src/utils/digiMapShared.ts — a caller's overlay cut to numbers, the APRS points (our SVG only, weather words, newest
//     first, bounded), the window words;
//   · src/services/aviationMap.ts avPlaneAdsb — an AvPlane in the ADS-B layer's shape (ICAO hex, a stable '~' address
//     otherwise, sparse = a datalink position, the card's own lines).
//   npx tsx scripts/test_digi_map.ts
import { digiStationsSanitise, digiStationsMerge, digiStationRows, digiWindowSec } from '../src/services/digiStations';
import { digiOverlayClean, digiAprsPoints, digiSvgOk, digiAprsWx, digiWindowOf } from '../src/utils/digiMapShared';
import { avPlaneAdsb, type AvPlane } from '../src/services/aviationMap';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };

console.log('── stations.json, cleaned ──');
const ans = {
  now: 1795000000, total: 3, count: 3, truncated: 0, byMode: { FT8: 2, CW: 1 },
  stations: [
    { call: 'G4ABC', grid: 'IO91', mode: 'FT8', band: '20m', freq: 14075000, count: 4, snr: -3, lastSnr: -8, first: 1794990000, last: 1794999000, src: 3, modes: ['FT8', 'FT4'], bands: ['20m'], country: 'England', lat: 51.5, lon: -1, km: 0 },
    { call: '<img src=x onerror=1>', grid: 'IO91', mode: 'FT8', band: '20m', freq: 1, count: 1, snr: 0, last: 1794999000 },
    { call: 'DL1ABC', grid: '', mode: 'CW', band: '40m', freq: 7021000, count: 2, snr: 12, last: 1794999500, src: 2, country: 'Germany"<' },
  ],
};
const s = digiStationsSanitise(ans)!;
ok(!!s && s.stations.length === 3, `three rows (${s?.stations.length})`);
ok(s.stations[1].call === 'imgsrcxonerror1', `markup in a call is cut to its allow-list (${s.stations[1].call})`);
ok(s.stations[2].country === 'Germany', 'free text loses the characters that matter to a parser');
ok(s.stations[0].lat === 51.5 && s.stations[2].lat === undefined, 'a position only where the server placed one');
ok(digiStationsSanitise({ off: 1, now: 5 })?.off === true, 'the store switched off says so');
ok(digiStationsSanitise({ stations: 'nope' }) === null, 'a broken answer is refused');
let held = digiStationsMerge(new Map(), s, true, '24h');
ok(held.size === 3, 'a full answer fills the map');
const rows = digiStationRows(held);
ok(rows.length === 2 && rows.every((r) => !!r.grid) && rows[0].time === 1794999000 * 1000, `only stations with a locator become map rows (${rows.map((r) => r.call).join(' ')})`);
held = digiStationsMerge(held, { now: 1795000000 + 86400, total: 1, truncated: false, off: false,
  stations: [{ ...s.stations[0], last: 1795000000 + 86000, count: 9 }] }, false, '24h');
ok(held.size === 1 && held.get('G4ABC')!.count === 9, `a live poll a day later: the newer row replaces, the aged ones leave the sliding window (${[...held.keys()].join(' ')})`);
ok(digiWindowSec('1h') === 3600 && digiWindowSec('all') === 0 && digiWindowOf('nonsense') === '24h', 'the window words');

console.log('── a caller\'s overlay ──');
const ov = digiOverlayClean({ label: 'ISS <b>PASS</b>', colour: 'red; x', lines: [[[51, 0], [52, 'x'], [53, 2]], [[1, 1]]],
  circles: [{ lat: 51, lon: 0, km: 99999 }, { lat: 'a' }], points: [{ lat: 51, lon: 0, label: 'ISS' }] })!;
ok(!!ov && ov.label === 'ISS bPASS/b' && !ov.colour, `label cut, a bad colour dropped (${ov?.label})`);
ok(ov.lines!.length === 1 && ov.lines![0].length === 2, 'a line keeps its numeric points; a one-point line goes');
ok(ov.circles!.length === 1 && ov.circles![0].km === 20000, 'a footprint is bounded (20 000 km)');
ok(digiOverlayClean({ label: 'x' }) === null && digiOverlayClean(null) === null, 'an overlay with nothing to draw is none');

console.log('── APRS points ──');
const svg = '<svg width="22" height="22" viewBox="0 0 22 22"><circle cx="11" cy="11" r="10" fill="#7fd"/></svg>';
ok(digiSvgOk(svg) === svg, 'our own symbol passes');
for (const bad of ['<svg onload="x()"></svg>', '<svg><script>1</script></svg>', '<svg><a href="javascript:1"/></svg>', '<b>x</b>'])
  ok(digiSvgOk(bad) === '', `refused: ${bad}`);
ok(digiAprsWx({ tempC: 10, hum: 80, windKmh: 8, windDir: 90, gustKmh: 16, hPa: 1012 }) === '10.0 °C · 80 % RH · wind 8 km/h E · gust 16 · 1012.0 hPa', 'the weather words (tracks.ts aprsCard\'s order)');
const pts = digiAprsPoints([
  { key: 'a', from: 'G4ABC-9', lat: 51, lon: -1, age: 30, wx: { hPa: 1000 } },
  { key: 'b', from: 'M0XYZ', lat: 52, lon: 0, age: 5 },
  { key: 'c', from: 'NOPOS', age: 1 },
  { key: 'd', from: 'GONE', lat: 50, lon: 0, killed: 1, age: 1 },
], () => svg, (k) => (k === 'a' ? [[51, -1.1], [51, -1]] : undefined), () => 12);
ok(pts.length === 2 && pts[0].call === 'M0XYZ', `positioned, not killed, newest first (${pts.map((p) => p.call).join(' ')})`);
ok(pts[1].trail.length === 2 && pts[1].wx === '1000.0 hPa' && pts[1].km === 12 && pts[1].svg === svg, 'its track, weather, distance and symbol');

console.log('── an aircraft in the ADS-B layer\'s shape ──');
const base: AvPlane = { key: '4CA97B', icao: '4CA97B', flight: 'FR1234', reg: 'EI-EST', lat: 51, lon: -1, alt: 36000, trk: 90, gs: 450,
  posSrc: 'ADS-B', posAge: 2, src: ['ADS-B', 'VDL2'], srcAge: { 'ADS-B': 2, VDL2: 40 }, age: 2, n: 3 };
const a = avPlaneAdsb(base)!;
ok(a.hex === '4ca97b' && a.flight === 'FR1234' && a.r === 'EI-EST' && a.track === 90 && a.gs === 450 && a.alt_baro === 36000, 'ADS-B fields');
ok(a.sparse === false, 'an ADS-B position is not sparse');
ok(a.rowsExtra!.some(([k, v]) => k === 'Heard on' && /ADS-B/.test(v) && /VDL2/.test(v)), 'the card says which sources heard it');
const d = avPlaneAdsb({ ...base, key: 'r:GABCD', icao: undefined, reg: 'G-ABCD', posSrc: 'VDL2', trk: undefined, gs: undefined, posAge: 300, src: ['VDL2'] })!;
ok(/^~[0-9a-f]{6}$/.test(d.hex) && d.hex === avPlaneAdsb({ ...base, key: 'r:GABCD', icao: undefined })!.hex, `no ICAO: a stable '~' address (${d.hex})`);
ok(d.sparse === true && d.track === undefined, 'a datalink position is sparse, with no track (the layer takes the bearing of its fixes)');
ok(avPlaneAdsb({ ...base, lat: undefined, lon: undefined }) === null, 'no position, nothing to draw');
ok(avPlaneAdsb({ ...base, ground: true })!.alt_baro === 'ground', 'on the ground');
const ls = avPlaneAdsb({ ...base, lastSeen: '12:00', posAge: 90 })!;
ok(ls.sparse === true && (ls.seen_pos ?? 0) > 600, 'an ADS-B "last seen" fix is drawn old, never live');

console.log(`\n${fail ? 'FAILED' : 'all passed'} — ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
