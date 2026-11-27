// test_sondes_app.ts — V12 radiosondes, the clients' half (src/services/sondes.ts): the SONDES report parsed defensively
// (bounded, control characters out, bad numbers and positions dropped), ★ NO TEMPERATURE unless the calibration is whole,
// the list's lines, the map's items / rows / card (escaped) and a chosen sonde's track, the bands. The server's own shapes
// are checked end to end in scripts/test-server-sondes.mjs, the web page in test-web-sondes.mjs.
import { sondeRanges } from '../src/services/sondes';
import { sondeRanges as webSondeRanges } from '../web/client/src/sondes';
import { readFileSync } from 'node:fs';
import { sondeSnrMode, parseSondeBands, parseSondeReport, sondeCard, sondeClimb, sondeDetail, sondeEmpty, sondeLine, sondeStatus, sondeTemp,
         sondeTrackItems, sondeTrackRows, sondeTrail, sondeMHz } from '../src/services/sondes';

let fail = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

const real = { type: 'sondes', band: 'mid', bandName: '402–404 MHz', hz: 403005000, lo: 402000000, hi: 404000000, dropped: 0,
  chans: [{ hz: 403299770, db: 18.0, n: 19 }], frames: 56, gain: 43.4, gainAuto: 1,
  sondes: [
    { serial: 'S3310411', kind: 'RS41', hz: 403299770, age: 1.2, n: 19, frame: 3019, snr: 5.5, cal: 19, txHz: 403300000, batt: 2.9,
      lat: 50.21851, lon: -5.32593, alt: 3499, climb: 5.2, speed: 5.0, heading: 53, sats: 9, posAge: 1.2, maxAlt: 3499,
      track: [[50.218, -5.327, 3400], [50.2185, -5.326, 3499]] },
    { serial: 'U0912345', kind: 'RS41', hz: 403849764, age: 0.4, n: 18, frame: 3053, cal: 51, temp: -41.3,
      lat: 52.3, lon: -1.1, alt: 25986, climb: -14.0, speed: 12, heading: 270, sats: 8, maxAlt: 31000, track: [] },
    { serial: 'T1520789', kind: 'RS41', hz: 402700000, age: 3, n: 4, frame: 10, cal: 4, enc: true, track: [] },
  ] };
const r = parseSondeReport(real);
ok(!!r && r.band === 'mid' && r.sondes.length === 3 && r.frames === 56 && r.chans.length === 1, 'a real report parses');
ok(!!r && sondeLine(r.sondes[0]) === 'RS41 S3310411 · 3,499 m · +5.2 m/s', `the line: ${r && sondeLine(r.sondes[0])}`);
ok(!!r && sondeLine(r.sondes[1]) === 'RS41 U0912345 · 25,986 m · −14.0 m/s', `a falling one: ${r && sondeLine(r.sondes[1])}`);
ok(!!r && sondeTemp(r.sondes[0]) === 'not yet calibrated (19 of 51)', `★ calibration 19 of 51: ${r && sondeTemp(r.sondes[0])}`);
ok(!!r && sondeTemp(r.sondes[1]) === '−41.3 °C', `a whole calibration: ${r && sondeTemp(r.sondes[1])}`);
// ★ a temperature sent beside an incomplete calibration is NOT shown (never a guessed PTU, whatever a server sends)
const liar = parseSondeReport({ type: 'sondes', sondes: [{ serial: 'S0000001', cal: 50, temp: 12.3, track: [] }] });
ok(!!liar && liar.sondes[0].temp === undefined && sondeTemp(liar.sondes[0]) === 'not yet calibrated (50 of 51)', '★ temp with cal 50 is dropped');
ok(!!r && /descending — burst near 31,000 m/.test(sondeDetail(r.sondes[1])), `after the burst: ${r && sondeDetail(r.sondes[1])}`);
ok(!!r && /encrypted/.test(sondeDetail(r.sondes[2])) && !/calibrated/.test(sondeDetail(r.sondes[2])), `an encrypted sonde: ${r && sondeDetail(r.sondes[2])}`);
ok(!!r && sondeDetail(r.sondes[0]).startsWith('403.300 MHz'), 'where it was heard, to the kHz');
ok(sondeMHz(402699771) === '402.700 MHz' && sondeClimb(-0.04) === '−0.0 m/s' && sondeClimb(undefined) === '', 'the units');
ok(!!r && /402–404 MHz · 3 sondes · listening on 1 signal · gain 43\.4 dB auto/.test(sondeStatus(r)), `the status: ${r && sondeStatus(r)}`);
ok(/Searching 402\.0–404\.0 MHz for radiosondes \(RS41, DFM, M10 and M20/.test(sondeEmpty({ ...r!, sondes: [] })) && /Taking the receiver/.test(sondeEmpty(null)), 'the empty list says what it is doing');
ok(parseSondeReport({ type: 'avdata', sondes: [] }) === null && parseSondeReport(null) === null && parseSondeReport({ type: 'sondes' }) === null, 'not a report: null');
const hostile = parseSondeReport({ type: 'sondes', sondes: [{ serial: 'S1<b>\u0000\u001b', lat: 99, lon: 5, alt: 'x', cal: 999, track: [[1, 2, 3], [100, 0, 0], 'x', [NaN, 1]] }, { lat: 1 }, 7, null] });
ok(!!hostile && hostile.sondes.length === 1 && hostile.sondes[0].serial === 'S1<b>' && hostile.sondes[0].lat === undefined && hostile.sondes[0].lon === undefined
   && hostile.sondes[0].alt === undefined && hostile.sondes[0].cal === 51 && hostile.sondes[0].track.length === 1, `hostile fields cleaned: ${JSON.stringify(hostile?.sondes[0])}`);
ok(!!hostile && !sondeCard(hostile.sondes[0]).includes('<b>S1<b>') && sondeCard(hostile.sondes[0]).includes('S1&lt;b&gt;'), 'the card escapes the serial');
const many = parseSondeReport({ type: 'sondes', sondes: Array.from({ length: 500 }, (_, i) => ({ serial: `S${i}`, track: Array.from({ length: 1000 }, () => [1, 1, 1]) })) });
ok(!!many && many.sondes.length === 64 && many.sondes[0].track.length === 400, 'bounded: 64 sondes, 400 track points (the server\'s own limits)');
// the app's map
const items = sondeTrackItems(r);
ok(items.length === 2 && items[0].key === 'S3310411' && /ellipse/.test(items[0]._ic.html) && /#ff8a4d/.test(items[1]._ic.html), 'map items: the two with a position, the falling one orange');
const rows = sondeTrackRows(r, { lat: 50.1, lon: -5.2 });
ok(rows.length === 3 && rows[0].key === 'S3310411' && /km$/.test(rows[0].range) && rows[2].noPos && rows[2].range === 'no position', `map rows nearest first: ${rows.map((x) => `${x.key} ${x.range}`).join(', ')}`);
ok(rows[0].trend === 1 && rows[1].trend === -1, 'climbing / falling arrows');
ok(sondeTrail(r, 'S3310411').length === 2 && sondeTrail(r, 'nope').length === 0, 'a chosen sonde\'s track from the report');
const b = parseSondeBands([{ id: 'lo', name: '400–402 MHz', hz: 401005000, lo: 400e6, hi: 402e6 }, { id: 'mid' }, 'x']);
ok(b.length === 1 && b[0].id === 'lo' && b[0].hi === 402e6, 'bands: a malformed one dropped');

// ── ★★ V12 2026-11-17 (Stuart): the app opens SONDES straight into its map, as the web does ──
ok(JSON.stringify(sondeRanges(b)) === JSON.stringify(webSondeRanges(b as any)) && sondeRanges(b)[0].label === '400–402',
   "the map's band keys: the web's, word for word");
const scr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
const map = readFileSync(new URL('../src/components/MapOverlay.tsx', import.meta.url), 'utf8');
ok(/if \(IS_APPLE_TV\) setSdBoxOpen\(true\); else setSdMapOpen\(true\);\s*sdOnRef\.current = true; setSdOn\(true\);\s*c\.sondes\(true\);/.test(scr),
   'SONDES opens into its map (the box only on the Apple TV, which has no maps)');
ok(/if \(!sdOnRef\.current\) \{ (?:wholeMapYield\('sondes'\); )?sdOnRef\.current = true; if \(IS_APPLE_TV\) setSdBoxOpen\(true\); else setSdMapOpen\(true\); \}/.test(scr),
   '…and so does joining a receiver somebody else put on SONDES');
ok(/shownMap === 'sondes' \? exitSondes\(\)/.test(scr) && /sondes: 'EXIT SONDES'/.test(map),
   "the map's key reads EXIT SONDES and leaves the mode");
ok(/ranges: sondeRanges\(sdCap\?\.bands \?\? \[\]\)/.test(scr), 'the band keys are on the map');
// ── ★★ V12 2026-11-19 the other types (docs/v12/SONDES-TYPES.md): the type leads the line; each type's own calibration ──
const t = parseSondeReport({ type: 'sondes', sondes: [
  { serial: '19012345', kind: 'DFM', model: 'DFM-09', hz: 402.7e6, cal: 4, calOf: 6, temp: -20.1, alt: 5000, climb: 5, lat: 52, lon: 0, track: [] },
  { serial: '19012346', kind: 'DFM', model: 'DFM-09', hz: 402.7e6, cal: 6, calOf: 6, temp: -20.1, track: [] },
  { serial: '903-2-24660', kind: 'M10', hz: 404.2e6, cal: 0, calOf: 0, temp: 15, alt: 700, track: [] },
  { serial: '504-2-01234', kind: 'M20', hz: 405.1e6, cal: 0, calOf: 0, track: [] },
  { serial: 'S0000002', cal: 51, temp: -3, track: [] },   // an older server: no calOf — the RS41's 51
] });
ok(!!t && sondeLine(t.sondes[0]) === 'DFM-09 19012345 · 5,000 m · +5.0 m/s', `a DFM's line: ${t && sondeLine(t.sondes[0])}`);
ok(!!t && t.sondes[0].temp === undefined && sondeTemp(t.sondes[0]) === 'not yet calibrated (4 of 6)', `★ a DFM part-way through: ${t && sondeTemp(t.sondes[0])}`);
ok(!!t && sondeTemp(t.sondes[1]) === '−20.1 °C', `a DFM calibrated: ${t && sondeTemp(t.sondes[1])}`);
ok(!!t && t.sondes[2].temp === undefined && sondeTemp(t.sondes[2]) === 'temperature not decoded for this type' && sondeLine(t.sondes[2]).startsWith('M10 903-2-24660'),
   `★ an M10's temperature is never shown (calOf 0): ${t && sondeTemp(t.sondes[2])}`);
ok(!!t && sondeLine(t.sondes[3]).startsWith('M20 504-2-01234'), 'an M20');
ok(!!t && t.sondes[4].calOf === 51 && sondeTemp(t.sondes[4]) === '−3.0 °C', 'an older server (no calOf) is an RS41');
{ // ★★ beta96 the frame SNR (S/N in the scan box) — only from a server that says snrv 2; each type its own scale
  const old = parseSondeReport({ type: 'sondes', sondes: [{ serial: 'S1', kind: 'RS41', snr: 5.4, track: [] }] });
  ok(!!old && old.sondes[0].snr === undefined, 'an older server\'s eye figure is not shown');
  const nw = parseSondeReport({ type: 'sondes', snrv: 2, sondes: [{ serial: 'S1', kind: 'RS41', snr: 12.5, track: [] }, { serial: 'M1', kind: 'M20', snr: 'x', track: [] },
    { serial: 'D1', kind: 'DFM', snr: 7, track: [] }] });
  ok(!!nw && nw.sondes[0].snr === 12.5 && nw.sondes[1].snr === undefined && nw.sondes[2].snr === 7, 'snrv 2: the number, bad ones dropped');
  ok(!!nw && sondeSnrMode(nw.sondes[0]) === 'rs41' && sondeSnrMode(nw.sondes[1]) === 'm20' && sondeSnrMode(nw.sondes[2]) === 'dfm', 'each type its scale');
  const rows = nw ? sondeTrackRows(nw, null) : [];
  ok(rows[0]?.snr === 12.5 && rows[0]?.snrMode === 'rs41' && rows[1]?.snrMode === undefined, 'the app\'s rows carry the chip');
}
console.log(fail ? `${fail} FAILED` : 'all passed');
process.exit(fail ? 1 : 0);
