// test_digi_map.ts — V12 beta90 THE DIGI MODES MAP's client halves (docs/v12/DIGI-MODES-MAP.md), in node:
//   · src/services/digiStations.ts — the station store's answer cleaned (markup and junk never pass), a live poll merged,
//     a sliding window, only stations with a locator become map rows;
//   · src/utils/digiMapShared.ts — a caller's overlay cut to numbers, the APRS points (our SVG only, weather words, newest
//     first, bounded), the window words;
//   · src/services/aviationMap.ts avPlaneAdsb — an AvPlane in the ADS-B layer's shape (ICAO hex, a stable '~' address
//     otherwise, sparse = a datalink position, the card's own lines).
//   npx tsx scripts/test_digi_map.ts
import { digiStationsSanitise, digiStationsMerge, digiStationRows, digiWindowSec, digiTotalsOf } from '../src/services/digiStations';
import { digiGalCard, digiGalItemWindow, digiGalQuery } from '../src/utils/digiMapShared';
import { digiOverlayClean, digiAprsPoints, digiSvgOk, digiAprsWx, digiWindowOf } from '../src/utils/digiMapShared';
import { avPlaneAdsb, type AvPlane } from '../src/services/aviationMap';
import { DIGI_FIT_JS, DIGI_FIT_MAX_ZOOM } from '../src/utils/digiMapShared';
import { aprsIcon, aprsGlyphOf, aprsIsInfra, APRS_GLYPHS, APRS_CODE_GLYPH, APRS_LEGEND, APRS_ICON_PX } from '../src/utils/aprsSymbols';
import { aprsIcon as appAprsIcon } from '../src/services/tracks';
import { aprsIcon as webAprsIcon } from '../web/client/src/tracks';
import { readFileSync } from 'node:fs';

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

console.log('── beta92 APRS symbols: clear, our own, a fallback ──');
{
  const common: [string, string][] = [['/>', 'car'], ['/-', 'house'], ['/#', 'digi'], ['/&', 'gateway'], ['/_', 'weather'], ['/[', 'person'],
    ['/k', 'truck'], ['/Y', 'yacht'], ['/s', 'boat'], ['/O', 'balloon'], ['/^', 'plane'], ["/'", 'plane'], ['/X', 'heli'], ['/b', 'bike'],
    ['/v', 'truck'], ['/j', 'car'], ['/u', 'truck'], ['/R', 'truck'], ['/r', 'antenna'], ['/=', 'train'], ['/;', 'tent'], ['/n', 'node'],
    ['\\#', 'digi'], ['I&', 'gateway'], ['\\-', 'house'], ['\\>', 'car'], ['\\_', 'weather']];
  for (const [sym, want] of common) ok(aprsGlyphOf(sym) === want && !!APRS_GLYPHS[want], `${sym} → ${want}`);
  ok(Object.values(APRS_CODE_GLYPH).every((g) => !!APRS_GLYPHS[g]), 'every mapped code has a drawing');
  ok(aprsGlyphOf('\\O') === '' && aprsGlyphOf('\\^') === '', 'the alternate table means something else for most codes — the character, not a guess');
  ok(aprsGlyphOf('/!') === '' && aprsGlyphOf('/$') === '', 'an undrawn code has no glyph');
  const fb = aprsIcon({ kind: 'position', sym: '/$' }).html;
  ok(/<text[^>]*font-size="20"[^>]*>\$<\/text>/.test(fb) && !/<circle cx="14" cy="14"/.test(fb), 'the fallback is the symbol character itself, large, outlined — no badge');
  ok(/>&lt;<\/text>/.test(aprsIcon({ kind: 'position', sym: '\\<' }).html), 'a fallback character is escaped');
  const house = aprsIcon({ kind: 'position', sym: '/-' }), digi = aprsIcon({ kind: 'position', sym: '/#' });
  ok(house.w === APRS_ICON_PX && house.h === 28 && APRS_ICON_PX >= 28, `large enough to read (${house.w} px, was 22)`);
  // ★★ Stuart 11-23: "aprs icons dont need to be circular as they will confuse with digi spots, so for a car show a little car icon"
  const car = aprsIcon({ kind: 'position', sym: '/>' });
  ok([house, digi, car].every((i) => !/<circle cx="14" cy="14"/.test(i.html) && !/<rect x="(1|2\.6)"/.test(i.html)),
    'NO BADGE — the symbol is the icon (a car is a little car), nothing round to read as a digi-mode spot');
  ok(digi.html !== house.html && /#c58cff/.test(digi.html) && !/#c58cff/.test(house.html), 'a digipeater and a house differ by silhouette and colour');
  ok(aprsIsInfra('/#') && aprsIsInfra('I&') && aprsIsInfra('/r') && !aprsIsInfra('/-') && !aprsIsInfra('/>'), 'the network\'s furniture is known (its own colour)');
  ok(/stroke="rgba\(255,255,255,0.8\)"/.test(house.html) && /stroke="#0b1014"/.test(house.html), 'a light halo and a dark outline round the silhouette: contrast on the dark map and the satellite map');
  ok(/>3<\/text>/.test(aprsIcon({ kind: 'position', sym: '3#' }).html), 'an overlay keeps its digit in a small tag');
  ok(/>I<\/text>/.test(aprsIcon({ kind: 'position', sym: 'I&' }).html), 'an overlay character in its tag');
  for (const [sym] of common) ok(digiSvgOk(aprsIcon({ kind: 'position', sym }).html) !== '', `${sym} passes the map pages' SVG check`);
  ok(APRS_LEGEND.length >= 8 && APRS_LEGEND.every((r) => digiSvgOk(aprsIcon(r, 16).html) !== ''), 'the legend rows draw');
  const st = { key: 'x', from: 'G4ABC', kind: 'position', sym: '/>', speed: 40 } as any;
  ok(appAprsIcon(st).html === webAprsIcon(st).html && appAprsIcon(st).html === aprsIcon(st).html, 'the app and the web page draw from the one set');
}

console.log('── beta92 the FIT key: the filtered stations only, and the receiver (the shared auto fit) ──');
{
  const fns = new Function(DIGI_FIT_JS + '\nreturn { digiShownPoints };')() as {
    digiShownPoints: (sp: any[], isShown: (s: any) => boolean, ap: any[], on: boolean) => [number, number][] };
  const spots = [{ call: 'A', mode: 'FT8', plat: 50, plon: -2 }, { call: 'B', mode: 'CW', plat: -30, plon: 150 }, { call: 'C', mode: 'FT8', plat: 52, plon: 1 }];
  const aprsP = [{ lat: 51, lon: 0 }];
  const ft8 = (s: any) => s.mode === 'FT8';
  const pts = fns.digiShownPoints(spots, ft8, aprsP, false);
  ok(pts.length === 2 && !pts.some((p) => p[0] === -30), `only the stations the filter shows (${JSON.stringify(pts)})`);
  ok(fns.digiShownPoints(spots, ft8, aprsP, true).length === 3, 'APRS counted while its key is on');
  ok(DIGI_FIT_MAX_ZOOM <= 10, `one station: a regional view, not a street (cap ${DIGI_FIT_MAX_ZOOM})`);
  ok(!/[`\\]/.test(DIGI_FIT_JS), 'no backtick or backslash (it is embedded in the pages\' template literals)');
  const app = readFileSync('src/components/MapOverlay.tsx', 'utf8'), web = readFileSync('web/client/src/main.ts', 'utf8');
  ok(/isSpots \? '<button id="snap" class="digifit"[^']*>FIT<\/button>'/.test(app), 'the app\'s spots maps: FIT in the top bar, where the AIS / APRS maps have it');
  // ★★★ beta96 stage 7 THE AMATEUR MAP: APRS through the HEARD IN window, then MESH and BEACONS (amateurMap.ts AMATEUR_JS)
  ok(/VibeAutoFit\.attach\(map,\{\s*points:function\(\)\{return digiShownPoints\(spots,function\(s\)\{return !!markers\[key\(s\)\];\},aprsPts\.filter\(function\(a\)\{return amKeep\(a\.age,amWin\(\)\);\}\),aprsOn\)[^]*?receiver:function\(\)\{return \(RX_LAT\|\|RX_LON\)\?\{lat:RX_LAT,lon:RX_LON\}:null;\}/.test(app),
     'the app: the AUTO FIT over the spots with a marker, APRS while on, AND the receiver');
  ok(/button:document\.getElementById\('snap'\),what:'station shown',maxZoom:\$\{DIGI_FIT_MAX_ZOOM\}/.test(app), 'the app: the FIT key is its key, capped at DIGI_FIT_MAX_ZOOM');
  // ★★★ b97 the web window's AUTO / FIT are the MAP CARD SYSTEM's (MAP CONTROLS): the same auto fit, given these points
  ok(/return digiShownPoints\(show\.spots \? spots : \[\], \(\) => true, aprsIn\(\), show\.aprs\)/.test(web) && /points: digiFitPoints, receiver: \(\) => me, maxZoom: \$\{DIGI_FIT_MAX_ZOOM\}/.test(web),
     'the web page: AUTO / FIT (MAP CONTROLS) frame the layers shown AND the receiver, capped at DIGI_FIT_MAX_ZOOM');
  ok(!/digiFitApply|digiFitView|map\.fitBounds\(all/.test(app + web.slice(web.indexOf('function spotsMapHtml'))), 'no one-shot fit left on either digi map');
}

console.log('── beta92 the gallery row and the map\'s TOTALS ──');
{
  const card = digiGalCard({ total: 5000, byMode: { FT8: 4175, WSPR: 286, 'x<y': 9, FT4: 300, CW: 239 } })!;
  ok(!!card && card.title === 'DIGI MODES MAP · 5,000 stations heard · FT8 4175 · FT4 300 · WSPR 286 · CW 239', `the web's words (${card?.title})`);
  ok(digiGalCard({ total: 0 }) === null && digiGalCard({ off: 1, total: 9 }) === null && digiGalCard(null) === null, 'nothing heard / store off / no answer: no row (no dead MAP key)');
  ok(digiGalCard({ total: 1 })!.title === 'DIGI MODES MAP · 1 station heard', 'one station: singular');
  const now = 1795000000;
  ok(digiGalItemWindow(now - 3600, now) === '24h' && digiGalItemWindow(now - 3 * 86400, now) === '7d' && digiGalItemWindow(now - 20 * 86400, now) === '30d'
     && digiGalItemWindow(now - 90 * 86400, now) === 'all', 'a spots card opens the window holding its day');
  ok(/window=all&since=1795086400$/.test(digiGalQuery(now)), 'the row asks for the totals only (since tomorrow)');
  const g = readFileSync('src/components/GalleryOverlay.tsx', 'utf8');
  ok(/digiGalCard\(j\)/.test(g) && /onDigiMap\('all'\)/.test(g) && /DIGI_GAL_KINDS\.includes\(it\.k\)/.test(g), 'the app\'s gallery: the row (ALL page 1) and a spots card\'s MAP');
  const sc = digiStationsSanitise({ now: 5, total: 2, truncated: 1, decodesToday: 1234, notKeptToday: 3, falseToday: '2', spotsSent: 15000,
    stations: [{ call: 'G4ABC', grid: 'IO91', lat: 51, lon: 0, radios: ['a1'] }, { call: 'DL1ABC', grid: '', radios: ['a1', 'b2'] }, { call: 'F5XYZ' }] })!;
  ok(sc.counts?.decodesToday === 1234 && sc.counts?.falseToday === 2 && sc.counts?.spotsSent === 15000, 'the day\'s counts are kept');
  ok(digiStationsSanitise({ stations: [] })!.counts?.decodesToday === -1, 'an older server: decodes today unknown (-1), not 0');
  const tt = digiTotalsOf(digiStationsMerge(new Map(), sc, true, 'all'), sc.counts, { total: 9, trunc: true });
  ok(tt.drawn === 3 && tt.total === 9 && tt.trunc && tt.unplaced.all === 2 && tt.unplaced.none === 1 && tt.unplaced.by.a1 === 1 && tt.unplaced.by.b2 === 1,
     `unplaced by radio (${JSON.stringify(tt.unplaced)})`);
}

console.log(`\n${fail ? 'FAILED' : 'all passed'} — ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
