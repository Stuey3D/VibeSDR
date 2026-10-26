/**
 * The app's AIS and APRS (src/services/tracks.ts) against the web client's (web/client/src/tracks.ts) — ONE RULE, TWO
 * READERS (2026-10-26): the same reports through both sanitisers, the same icons and cards, status lines, the live list's
 * order and wording, the APRS box, packets between lists, the capability, the decoder's settings, and the app's wiring
 * (the decoder name, the attach, the map kind, the mode key, the copy).
 *
 * Run: npx tsx scripts/test_ais_aprs_app.ts   (run-tests.sh does)
 */
import * as web from '../web/client/src/tracks.ts';
import * as app from '../src/services/tracks.ts';
import { readFileSync } from 'node:fs';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const ok = (what: string, c: boolean) => eq(what, c, true);

const rx = { lat: 50.9, lon: -1.4 };
const aisMsg = {
  type: 'ais', centre: 162e6, rate: 2.4e6, now: 1761480000, msgs: 412, msgRate: 37.5, chA: 210, chB: 202, repaired: 9, other: 3,
  dropped: 0, capture: 92.4, noise: -31,
  vessels: [
    { mmsi: 235012345, kind: 'A', name: 'WILSON GOOLE', call: 'MXYZ7', dest: 'ROTTERDAM', imo: 9123456, flag: 'GB', country: 'United Kingdom',
      stype: 70, stypeName: 'cargo', family: 'cargo', nav: 0, navName: 'under way using engine', lat: 50.81, lon: -1.31, sog: 12.3, cog: 225,
      hdg: 224, rot: -3, draught: 5.6, len: 89, beam: 14, eta: '10-27 06:00', trend: 1, line: 'WILSON GOOLE · cargo', ch: 'A', snr: 21, seen: 3, age: 3, posAge: 3, count: 40 },
    { mmsi: 244123456, kind: 'B', name: 'SEA BREEZE', flag: 'NL', family: 'leisure', stypeName: 'sailing', lat: 50.70, lon: -1.10, sog: 0.2,
      nav: 15, ch: 'B', seen: 50, age: 50, posAge: 50, count: 3, trend: -1 },
    { mmsi: 992351234, kind: 'aton', atonName: 'cardinal mark N', virtual: 1, lat: 50.75, lon: -1.2, seen: 120, age: 120, posAge: 120, count: 2 },
    { mmsi: 2320123, kind: 'base', name: '', lat: 50.95, lon: -1.42, seen: 6, age: 6, posAge: 6, count: 99 },
    { mmsi: 111232506, kind: 'sar', lat: 50.6, lon: -1.0, cog: 80, alt: 300, seen: 1, age: 1, posAge: 1, count: 5 },
    { mmsi: 970123456, kind: 'sart', name: 'SART TEST', seen: 2, age: 2, posAge: -1, count: 1 },
    { mmsi: 'x', kind: 'A' }, { mmsi: -3 }, { mmsi: 211000000, kind: 'A', name: '<b>EVIL</b> & "CO"', flag: 'gb', lat: 91, lon: 'bad',
      sog: {}, seen: 900, age: 900, count: 1 },
  ],
  safety: [{ age: 30, mmsi: 970123456, name: 'SART TEST', text: 'SART ACTIVE' }, { age: 400, mmsi: 235012345, name: '', text: 'NAV WARNING <x>' }],
  gain: 33.8, gainAuto: 1,
};

// ── AIS sanitiser, icons, cards, status ──
const wa = web.aisSanitise(aisMsg)!, aa = app.aisSanitise(aisMsg)!;
eq('AIS sanitise = web', aa, wa);
eq('AIS sanitise drops bad MMSIs', aa.vessels.length, 7);
eq('AIS not an ais message', app.aisSanitise({ type: 'ism', vessels: [] }), null);
for (const v of aa.vessels) {
  eq(`AIS icon ${v.mmsi}`, app.aisIcon(v), web.aisIcon(v));
  eq(`AIS name ${v.mmsi}`, app.aisName(v), web.aisName(v));
  eq(`AIS card ${v.mmsi} (receiver)`, app.aisCard(v, rx), web.aisCard(v, rx));
  eq(`AIS card ${v.mmsi} (no receiver)`, app.aisCard(v, null), web.aisCard(v, null));
}
ok('AIS card escapes off-air text', !app.aisCard(aa.vessels[6], rx).includes('<b>EVIL'));
eq('AIS status', app.aisStatus(aa), web.aisStatus(wa));
eq('AIS status before a report', app.aisStatus(null), web.aisStatus(null));
eq('AIS status no capture line at 100 %', app.aisStatus({ ...aa, capture: 100 }), web.aisStatus({ ...wa, capture: 100 }));
eq('AIS colours', app.AIS_COLOURS, web.AIS_COLOURS);

// ── the live list: order and wording against the web's rows ──
const keysOf = (html: string) => [...html.matchAll(/data-key="([^"]*)"/g)].map((m) => m[1]);
const ar = app.aisListRows(aa, rx);
eq('AIS list order = web (nearest first, no position last)', ar.map((r) => r.key), keysOf(web.aisRows(wa, rx)));
eq('AIS list order without a receiver = web', app.aisListRows(aa, null).map((r) => r.key), keysOf(web.aisRows(wa, null)));
const wr = web.aisRows(wa, rx);
for (const r of ar) {
  ok(`AIS row ${r.key}: name in the web's row`, wr.includes(web.esc(r.name)));
  ok(`AIS row ${r.key}: second line in the web's row`, wr.includes(web.esc(r.second.split(' · ')[0])));
  if (r.sp) ok(`AIS row ${r.key}: speed "${r.sp}" in the web's row`, wr.includes(web.esc(r.sp)));
  if (r.range && r.range !== 'no position') ok(`AIS row ${r.key}: range "${r.range}" in the web's row`, wr.includes(r.range));
}
eq('AIS row: the SART is an emergency row', ar.find((r) => r.key === '970123456')?.em, true);
eq('AIS row: no position', ar.find((r) => r.key === '970123456')?.range, 'no position');
eq('AIS row: trend', ar.find((r) => r.key === '235012345')?.trend, 1);
eq('AIS safety strip: newest first, at most three', app.aisSafetyLines(aa), ['⚠ MMSI 235012345: NAV WARNING <x> · 7 min ago', '⚠ SART TEST: SART ACTIVE · 30 s ago']);
eq('AIS safety strip empty', app.aisSafetyLines({ ...aa, safety: [] }), []);

// ── what the map is handed ──
const items = app.aisItems(aa, rx);
eq('AIS items: only those with a position', items.map((i) => i.key), aa.vessels.filter((v) => v.lat != null && v.lon != null).map((v) => String(v.mmsi)));
eq('AIS item carries its icon, label and card', [items[0]._ic, items[0]._lb, items[0]._cd],
   [web.aisIcon(wa.vessels[0]), web.aisName(wa.vessels[0]), web.aisCard(wa.vessels[0], rx)]);

// ── the trail ──
eq('AIS trail', app.aisTrailSanitise({ type: 'ais_trail', mmsi: 235012345, pts: [[50.8, -1.3, 60], [50.81, -1.31, 0], [99, 0, 0], 'x'] }),
   { mmsi: 235012345, pts: [[50.8, -1.3], [50.81, -1.31]] });
eq('AIS trail refused without an MMSI', app.aisTrailSanitise({ pts: [] }), null);

// ── the capability ──
eq('AIS cap live', app.parseAisCapability({ ais: true }), { ok: true, why: '' });
eq('AIS cap greyed with the reason', app.parseAisCapability({ ais: false, aisWhy: "this receiver's centre frequency is locked by its owner" }),
   { ok: false, why: "this receiver's centre frequency is locked by its owner" });
eq('AIS cap greyed, no reason', app.parseAisCapability({ ais: false }), { ok: false, why: 'AIS is not available on this receiver' });
eq('AIS cap absent (an HF+) = no control', app.parseAisCapability({ adsb: true }), undefined);
eq('APRS cap', app.parseAprsCapability({ aprs: true, aprsHz: 144390000 }), { on: true, hz: 144390000 });
eq('APRS cap absent (older server)', app.parseAprsCapability({ cw: true }), { on: false, hz: 144.8e6 });
eq('APRS cap: a nonsense channel falls back', app.parseAprsCapability({ aprs: true, aprsHz: 5 }), { on: true, hz: 144.8e6 });

// ── APRS ──
const aprsData = {
  baud: 1200, frames: 31, repaired: 2, dupes: 7,
  stations: [
    { key: 'M0ABC-9', from: 'M0ABC-9', kind: 'position', sym: '/>', symName: 'car', lat: 50.95, lon: -1.35, course: 90, speed: 48, alt: 120,
      comment: 'mobile', path: 'WIDE1-1', via: 'GB7XX', raw: 'M0ABC-9>APRS,WIDE1-1:!5057.00N/00121.00W>090/026', line: 'M0ABC-9 · car · 48 km/h E', seen: 4, age: 4, count: 6,
      posAge: 4, trail: [[50.94, -1.36], [50.95, -1.35], ['x', 1]] },
    { key: 'G4WX', from: 'G4WX', kind: 'weather', sym: '/_', symName: 'weather station', lat: 51.2, lon: -0.8,
      wx: { tempC: 11.25, hum: 87, windDir: 225, windKmh: 14, gustKmh: 30, rain1hMm: 0.4, hPa: 1012.3, bogus: 5 }, line: 'G4WX · weather · 11.3 °C', seen: 400, age: 400, count: 2, posAge: 400 },
    { key: 'obj:NET', from: 'G0OBJ', name: 'NET', kind: 'object', sym: '\\n', killed: 1, lat: 51, lon: -1, seen: 30, age: 30, count: 1, posAge: 30 },
    { key: 'B5ALL', from: 'B5ALL', kind: 'status', sym: 'D&', status: '<script>', seen: 4000, age: 4000, count: 1, posAge: -1 },
    { key: '', from: 'NOKEY' },
  ],
  messages: [{ age: 20, from: 'M0ABC', to: 'G4WX', text: 'hello', kind: 'message', id: '12' }, { age: 10, from: 'G4WX', to: 'M0ABC', text: 'ack', kind: 'ack', id: '12' }],
};
const wl = web.aprsSanitise(aprsData)!, al = app.aprsSanitise(aprsData)!;
eq('APRS sanitise = web', al, wl);
eq('APRS sanitise drops a keyless station', al.stations.length, 4);
for (const s of al.stations) {
  eq(`APRS icon ${s.key}`, app.aprsIcon(s), web.aprsIcon(s));
  eq(`APRS colour ${s.key}`, app.aprsColour(s), web.aprsColour(s));
  eq(`APRS name ${s.key}`, app.aprsName(s), web.aprsName(s));
}
// ★ the card's altitude cue is per reader (each keeps its own last altitude) — the first card from each must agree
for (const s of al.stations) eq(`APRS card ${s.key}`, app.aprsCard(s, rx), web.aprsCard(s, rx));
eq('APRS status', app.aprsStatus(al, 144.8e6), web.aprsStatus(wl, 144.8e6));
eq('APRS status before a list', app.aprsStatus(null, 144.39e6), web.aprsStatus(null, 144.39e6));
eq('APRS list order = web', app.aprsListRows(al, rx).map((r) => r.key), keysOf(web.aprsRows(wl, rx)));
{
  const html = web.aprsRows(wl, rx);
  for (const r of app.aprsListRows(al, rx)) {
    ok(`APRS row ${r.key}: name in the web's row`, html.includes(web.esc(r.name)));
    if (r.sp) ok(`APRS row ${r.key}: "${r.sp}" in the web's row`, html.includes(web.esc(r.sp)));
    if (r.range && r.range !== 'no position') ok(`APRS row ${r.key}: range in the web's row`, html.includes(r.range));
    ok(`APRS row ${r.key}: age in the web's row`, html.includes(web.esc(r.age)));
  }
}
eq('APRS items: positioned, not removed', app.aprsItems(al, rx).map((i) => i.key), ['M0ABC-9', 'G4WX']);
eq('APRS box rows', app.aprsBoxRows(al, rx).map((r) => [r.call, r.line, r.right, r.noPos]),
   [['M0ABC-9', 'car · 48 km/h E', '6.6 km · 4s', false], ['G4WX', 'weather · 11.3 °C', '54 km · 7m', false],
    ['NET', 'object', '30 km · 30s', false], ['B5ALL', 'status', '1h', true]]);
eq('APRS messages newest first, an ack with its id', app.aprsMessageLines(al), ['G4WX → M0ABC: ack 12', 'M0ABC → G4WX: hello']);

// ── packets between lists ──
const pk = app.aprsPacketSanitise({ key: 'M0ABC-9', from: 'M0ABC-9', kind: 'position', line: 'M0ABC-9 · car', raw: 'r', lat: 50.96, lon: -1.34, to: '', text: 7 })!;
eq('APRS packet sanitised', pk, { key: 'M0ABC-9', from: 'M0ABC-9', kind: 'position', line: 'M0ABC-9 · car', raw: 'r', lat: 50.96, lon: -1.34, to: undefined, text: undefined });
eq('APRS packet without a key', app.aprsPacketSanitise({ from: 'X' }), null);
const trails = new Map<string, [number, number][]>([['M0ABC-9', [[50.95, -1.35]]]]);
const copy = app.aprsSanitise(aprsData)!;
eq('APRS packet moves its station', app.aprsApplyPacket(copy, trails, pk), true);
eq('… to the new position, heard now', [copy.stations[0].lat, copy.stations[0].lon, copy.stations[0].seen], [50.96, -1.34, 0]);
eq('… and grows its track', trails.get('M0ABC-9'), [[50.95, -1.35], [50.96, -1.34]]);
app.aprsApplyPacket(copy, trails, pk);
eq('… the same fix twice is one point', trails.get('M0ABC-9')!.length, 2);
eq('APRS packet without a position moves nothing', app.aprsApplyPacket(copy, trails, { ...pk, lat: undefined }), false);
let log: { t: number; line: string }[] = [];
for (let i = 0; i < 70; i++) log = app.aprsLogPush(log, { ...pk, line: 'p' + i }, i);
eq('APRS packet log: newest first, 60 kept', [log.length, log[0].line, log[59].line], [60, 'p69', 'p10']);
eq('APRS packet log falls back to the raw packet', app.aprsLogPush([], { ...pk, line: '' }, 0)[0].line, 'r');

// ── the decoder's settings ──
eq('APRS baud', [app.aprsBaudOf(300), app.aprsBaudOf('1200'), app.aprsBaudOf(9600)], [300, 1200, 1200]);
eq('APRS attach params (the web sends baud_rate always)', [app.aprsParams(300), app.aprsParams(1200)], [{ baud_rate: 300 }, { baud_rate: 1200 }]);
eq('APRS TUNE VHF', app.aprsTuneFor(1200, 144.39e6), { hz: 144390000, mode: 'nfm', label: 'TUNE 144.390 NFM' });
eq('APRS TUNE HF', app.aprsTuneFor(300, 144.8e6), { hz: 10147600, mode: 'usb', label: 'TUNE 10.1476 USB' });
ok('APRS hint names the channel', app.aprsHintText(1200, 144.8e6).includes('144.800 MHz NFM'));

// ── the app's wiring (source checks) ──
const src = (p: string) => readFileSync(new URL('../' + p, import.meta.url), 'utf8');
const dc = src('src/services/DecoderClient.ts');
ok('DecoderClient: aprs is a decoder name', /DecoderName[^;]*'aprs'/.test(dc));
ok('DecoderClient: attaches extension aprs with its baud', /case 'aprs':[^\n]*extension_name: 'aprs'/.test(dc));
ok('DecoderClient: aprs_list and aprs_packet only while APRS is attached', /aprs_list[\s\S]{0,200}this\.active !== 'aprs'/.test(dc));
const vc = src('src/services/VibeServerClient.ts');
ok('VibeServerClient: sends {type:ais,on}', /type: 'ais', on: on \? 1 : 0/.test(vc));
ok('VibeServerClient: ais_off / ais_error / ais_trail handled', /case 'ais_off'/.test(vc) && /case 'ais_error'/.test(vc) && /case 'ais_trail'/.test(vc));
const mo = src('src/components/MapOverlay.tsx');
ok('MapOverlay: ais and aprs map kinds', /MapKind = [^;]*'ais'[^;]*'aprs'/.test(mo));
ok('MapOverlay: the shared track layer injected', /TRACK_LAYER_JS/.test(mo) && /VibeTrackLayer\.create/.test(mo));
ok('MapOverlay: EXIT AIS leaves the mode', /EXIT AIS/.test(mo));
const ms = src('src/components/ModeSelector.tsx');
ok('ModeSelector: the AIS key beside ISM', /aisAvail/.test(ms) && /label="AIS"/.test(ms));
ok('ModeSelector: the APRS key only where offered', /aprsAvail/.test(ms) && /onDecToggle\('aprs'\)/.test(ms));
const sd = src('src/screens/SDRScreen.tsx');
ok('SDRScreen: audio let go in AIS', /const audioQuiet = [^;]*aisOn/.test(sd));
ok('SDRScreen: no AIS on Apple TV (no map there)', /aisAvail:[^\n]*IS_APPLE_TV/.test(sd));
const gal = src('src/components/GalleryOverlay.tsx');
ok('Gallery: AIS and APRS chips (the web\'s)', /id: 'ais', label: 'AIS'/.test(gal) && /id: 'aprs', label: 'APRS'/.test(gal));
const about = src('src/components/AboutOverlay.tsx');
ok('About: the V12 note names AIS and APRS', /AIS/.test(about) && /APRS/.test(about));
const tour = src('src/screens/SDRScreen.tsx');
ok('Tour: the Mode card names AIS and EXIT AIS', /ISM and AIS, which take the whole receiver[^']*EXIT AIS/.test(tour));
ok('Tour: the Mode card names APRS and its MAP', /APRS \(packet stations, with MAP in its box\)/.test(tour));

console.log(`${fails ? 'FAIL' : 'ok'}  test_ais_aprs_app — ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
