/**
 * The app's NDB (src/services/ndb.ts) against the web client's (web/client/src/ndb.ts) — ONE RULE, TWO READERS
 * (2026-10-26): the same reports through both sanitisers, every cell rule (ident ✓ / ≈ / plain / ?, harmonic, station,
 * NM + bearing, bars, tiles, the list's order, what goes on the map), the map layer's own barsFor, and the app-only
 * parts — the capability, the map feed (the web's renderMap as data), Find / stats parses, the readings' memory.
 *
 * Run: npx tsx scripts/test_ndb_app.ts   (run-tests.sh does)
 */
import * as web from '../web/client/src/ndb.ts';
import * as app from '../src/services/ndb.ts';
import { readFileSync } from 'node:fs';
import { runInNewContext } from 'node:vm';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const ok = (what: string, c: boolean) => eq(what, c, true);

const NOW = 1_793_000_000;   // seconds
const report = {
  type: 'ndb', centre: 393_000, lo: 255_000, hi: 526_500, rate: 384_000, now: NOW, carriers: 9, identified: 4,
  published: 20, heardIn: 7, pos: true, far: { ident: 'CHT', km: 180.4 }, bin: 23.4, source: 'Airspy HF+', starting: 0,
  beacons: [
    { id: 7, hz: 433_500, snr: 24.2, lvl: -60, tone: 1020, tsnr: 18, a1a: false, wpm: 7.1, ident: 'HEN', votes: 3, ballots: 4, read: 'HEN',
      active: true, first: NOW - 300, last: (NOW - 2) * 1000, idAt: NOW - 280, lastId: NOW - 20,
      db: { k: 'match', ident: 'HEN', name: 'Henton', cc: 'GB', hz: 433_500, lat: 51.76, lon: -0.79, km: 64.2, brg: 190 }, copy: 'HEN HEN' },
    { id: 3, hz: 352_000, snr: 12.0, lvl: -70, tone: 400, tsnr: 10, a1a: false, wpm: 8, ident: 'WOD', votes: 0, ballots: 5, read: '',
      active: true, first: NOW - 300, last: (NOW - 1) * 1000, idAt: NOW - 270, lastId: NOW - 10,
      db: { k: 'near', ident: 'WOB', name: 'Woodley', cc: 'GB', hz: 352_000, lat: 51.45, lon: -0.88, km: 93.1, brg: 181 }, copy: 'WOD' },
    { id: 11, hz: 393_000, snr: 6.5, lvl: -80, tone: 0, tsnr: 0, a1a: true, wpm: 0, ident: '', votes: 0, ballots: 1, read: 'EM',
      active: true, first: NOW - 60, last: (NOW - 1) * 1000, idAt: 0, lastId: 0,
      db: { k: 'cand', ident: 'EMW', name: 'East Midlands', cc: 'GB', hz: 393_000, lat: 52.83, lon: -1.33, km: 70, brg: 340 },
      alt: [{ ident: 'XYZ', name: 'Somewhere', cc: 'FR', km: 400 }], copy: 'E?' },
    { id: 2, hz: 316_000, snr: 30, lvl: -50, tone: 1020, tsnr: 22, a1a: false, wpm: 7, ident: 'EPM', votes: 4, ballots: 4, read: 'EPM',
      active: false, first: NOW - 900, last: (NOW - 120) * 1000, idAt: NOW - 800, lastId: NOW - 130, copy: '' },
    { id: 4, hz: 632_000, snr: 3, lvl: -90, tone: 0, tsnr: 0, a1a: false, wpm: 0, ident: '', votes: 0, ballots: 0, read: '',
      active: true, first: NOW - 30, last: NOW * 1000, idAt: 0, lastId: 0, harm: { id: 3, ident: 'WOD', hz: 316_000 }, copy: '' },
    { id: 5, hz: 300_000, snr: 2, lvl: -95, tone: 0, tsnr: 0, a1a: false, wpm: 0, ident: '', votes: 0, ballots: 0, read: '',
      active: false, first: NOW - 30, last: (NOW - 25) * 1000, idAt: 0, lastId: 0, copy: '' },
    // ★ hostile strings: lower case and markup are stripped from idents and copy, names bounded
    { id: 9, hz: 400_000, snr: 15, ident: 'ab<script>', read: 'x"y', active: true, last: NOW * 1000, lastId: 0, copy: '<b>OK</b>',
      db: { k: 'match', ident: 'zz', name: 'N'.repeat(80), cc: 'gb', hz: 400_000, lat: 0, lon: 0, km: 5, brg: 0 } },
    { id: 'bad', hz: 'x', ident: 42 },
  ],
  log: [{ ident: 'HEN', hz: 433_500, name: 'Henton', cc: 'GB', km: 64.2, brg: 190, lat: 51.76, lon: -0.79, best: 26, first: NOW - 280, last: NOW - 20 }],
  copy: [{ s: 1, t: NOW - 5, hz: 433_500, id: 7, text: 'HEN', q: 'high' }, { s: 2, t: NOW - 3, hz: 352_000, id: 3, text: 'WOD', q: 'medium' },
         { s: 3, t: NOW - 2, hz: 393_000, id: 11, text: 'E?', q: 'poor' }, { s: 4, t: NOW - 1, hz: 393_000, id: 11, text: 'EM', q: 'weird' }],
};

/* ── the sanitiser, field for field ── */
const w = web.ndbSanitise(report)!, a = app.ndbSanitise(report)!;
eq('sanitise: the whole report', a, w);
eq('sanitise: not an ndb message', app.ndbSanitise({ type: 'ism' }), web.ndbSanitise({ type: 'ism' }));
eq('sanitise: null', app.ndbSanitise(null), null);
eq('sanitise: hostile ident stripped', a.beacons.find((b) => b.id === 9)!.ident, '');
eq('sanitise: hostile copy stripped', a.beacons.find((b) => b.id === 9)!.copy, 'OK');
eq('sanitise: unknown q → low', a.copy[3].q, 'low');
eq('sanitise: starting 0 → false', a.starting, false);
eq('sanitise: a bare report', app.ndbSanitise({ type: 'ndb' }), web.ndbSanitise({ type: 'ndb' }));

/* ── every cell, every beacon ── */
for (const b of a.beacons) {
  eq(`ident cell ${b.id}`, app.ndbIdentCell(b), web.ndbIdentCell(b));
  eq(`station ${b.id}`, app.ndbStation(b), web.ndbStation(b));
  eq(`on map ${b.id}`, app.ndbOnMap(b), web.ndbOnMap(b));
  for (const live of [true, false]) eq(`bars ${b.id} ${live}`, app.ndbBars(b.snr, live), web.ndbBars(b.snr, live));
}
eq('ident ✓', app.ndbIdentCell(a.beacons[0]).text, '✓ HEN');
eq('ident ≈', app.ndbIdentCell(a.beacons[1]).text, '≈ WOD');
eq('ident ? candidate', app.ndbIdentCell(a.beacons[2]).text, 'EMW?');
eq('ident plain unlisted', app.ndbIdentCell(a.beacons[3]).cls, 'unl');
eq('ident harmonic', app.ndbIdentCell(a.beacons[4]).cls, 'harm');
eq('station with flag and NM', app.ndbStation(a.beacons[0]), { name: '🇬🇧 Henton', dist: '35 NM S' });
eq('candidate station has ?', app.ndbStation(a.beacons[2]).name, '🇬🇧 East Midlands?');
eq('the zero position is not on the map', app.ndbOnMap(a.beacons.find((b) => b.id === 9)!), false);

/* ── the small rules ── */
for (const [km, brg] of [[0, 0], [5, 359], [18.52, 11.25], [93.1, 181], [-1, 0], [1852, 90]] as [number, number][]) {
  eq(`nmText ${km} ${brg}`, app.nmText(km, brg), web.nmText(km, brg));
  eq(`nmText ${km}`, app.nmText(km), web.nmText(km));
}
for (const snr of [-3, 0, 9.9, 10, 19.9, 20, 45]) eq(`bars ${snr}`, app.ndbBars(snr, true), web.ndbBars(snr, true));
for (const d of [0, 11.24, 11.26, 180, 359, -10, 725]) eq(`compass ${d}`, app.compass(d), web.compass(d));
for (const c of ['GB', 'gb', 'G', 'G1', '']) eq(`flag ${c}`, app.flag(c), web.flag(c));
for (const h of [255_000, 393_050, 526_500]) eq(`khz ${h}`, app.khz(h), web.khz(h));
for (const d of [0, 59, 61, 3599, 3601, 90000]) eq(`ago ${d}`, app.ndbAgo(NOW, NOW - d), web.ndbAgo(NOW, NOW - d));
eq('utc', app.utc(NOW), web.utc(NOW));
eq('csv', app.csv(['a', 'b'], [['x,y', 'q"r'], [1, 'line\nbreak']]), web.csv(['a', 'b'], [['x,y', 'q"r'], [1, 'line\nbreak']]));
eq('tiles', app.ndbTiles(a), web.ndbTiles(a));
eq('tiles with nothing published', app.ndbTiles({ ...a, published: 0, far: undefined }), web.ndbTiles({ ...a, published: 0, far: undefined }));
eq('rows: the same, lowest frequency first', app.ndbRows(a).map((b) => b.id), web.ndbRows(w).map((b) => b.id));
eq('rows: quiet unidentified carriers dropped, quiet identified kept', app.ndbRows(a).map((b) => b.hz),
   [0, 316_000, 352_000, 393_000, 400_000, 433_500, 632_000].filter((h) => h !== 0));

/* ── the map layer reads the same bars (web/mapkit/ndblayer.js barsFor) ── */
{
  const src = readFileSync(new URL('../web/mapkit/ndblayer.js', import.meta.url), 'utf8');
  const ctx: any = { window: {}, console };
  ctx.globalThis = ctx.window;
  runInNewContext(src, ctx);
  const L = ctx.window.VibeNdbLayer;
  for (const snr of [-3, 9.9, 10, 19.9, 20, 45]) for (const live of [true, false]) eq(`layer barsFor ${snr} ${live}`, L.barsFor(snr, live), app.ndbBars(snr, live));
  eq('layer colours = the list\'s', L.BAR_ON, app.NDB_BAR_ON);
  const gen = readFileSync(new URL('../src/generated/ndbLayerSource.ts', import.meta.url), 'utf8');
  ok('the app\'s layer copy is current', gen.includes(JSON.stringify(src).replace(/<\/script/gi, '<\\/script')));
}

/* ── the map feed (the web's renderMap, as data) ── */
{
  const found = [
    { ident: 'HEN', hz: 433_500, name: 'Henton', cc: 'GB', lat: 51.76, lon: -0.79, km: 64, brg: 190, heard: true },   // placed → not unheard
    { ident: 'CAM', hz: 332_500, name: 'Cambridge', cc: 'GB', lat: 52.2, lon: 0.18, km: 80, brg: 60, heard: false },  // in band → unheard
    { ident: 'FAR', hz: 600_000, name: 'Out of band', cc: 'FR', lat: 48, lon: 2, km: 400, brg: 150, heard: false },   // outside lo/hi
  ];
  const f = app.ndbMapFeed(a, found);
  eq('feed: beacons placed, lowest first', f.beacons.map((b) => b.key), ['3', '11', '7']);
  eq('feed: ✓ HEN', f.beacons[2], { key: '7', ident: 'HEN', label: 'HEN', lat: 51.76, lon: -0.79, bars: 3, stale: false, kind: 'match' });
  eq('feed: ≈ is near', f.beacons[0].kind, 'near');
  eq('feed: the candidate is labelled ? and dimmed', [f.beacons[1].label, f.beacons[1].stale, f.beacons[1].kind], ['EMW?', true, 'cand']);
  eq('feed: unheard = published, in the band, not placed', f.unheard, [{ lat: 52.2, lon: 0.18, ident: 'CAM' }]);
  eq('found key', app.ndbFoundKey({ ident: 'HEN', hz: 433_400 }), 'HEN@867');
}

/* ── Find and stats ── */
{
  const fr = app.parseNdbFound({ type: 'ndb_found', q: 'hen', pos: true, list: [{ ident: 'HEN', hz: 433_500, name: 'Henton', cc: 'GB', lat: 51.7, lon: -0.8, km: 64, brg: 190, heard: true }, { ident: 'x<y' }] });
  eq('found: parsed', fr.list[0], { ident: 'HEN', hz: 433_500, name: 'Henton', cc: 'GB', lat: 51.7, lon: -0.8, km: 64, brg: 190, heard: true });
  eq('found: a hostile ident cleaned, missing km = -1', [fr.list[1].ident, fr.list[1].km, fr.list[1].heard], ['', -1, false]);
  eq('found: q and pos', [fr.q, fr.pos], ['hen', true]);
  eq('found: nothing', app.parseNdbFound(null), { q: '', pos: undefined, list: [] });
  const st = app.parseNdbStats({ type: 'ndb_stats', since: NOW - 7200,
    hours: [{ t: NOW - 7200, heard: 4, far: 120 }, { t: NOW - 3600, heard: 6, far: 300 }],
    beacons: [{ ident: 'HEN', hz: 433_500, name: 'Henton', cc: 'GB', km: 64, minutes: 90, mean: 21.2, best: 28, last: NOW, snr: [[NOW - 60, 20], [NOW, 22]] },
              { ident: 'CHT', hz: 277_000, name: 'Chiltern', cc: 'GB', km: 40, minutes: 3, mean: 8, best: 11, last: NOW - 50, snr: [] }] });
  eq('stats: least heard first', st.beacons.map((b) => b.ident), ['CHT', 'HEN']);
  eq('stats: hours', st.hours.length, 2);
  const hod = app.ndbByHourOfDay(st.hours);
  eq('stats: by hour of day has 24', hod.length, 24);
  eq('stats: the hours land in their UTC hours', hod.filter((v) => v > 0).sort(), [4, 6]);
  const c = app.ndbStatsCsv(st).split('\n');
  eq('stats: CSV head (the web\'s)', c[0], 'ident,khz,station,country,nm,minutes_heard,mean_snr_db,best_snr_db,last_heard_utc');
  eq('stats: CSV row', c[1].split(',').slice(0, 7), ['CHT', '277.0', 'Chiltern', 'GB', '22', '3', '8.0']);
  eq('stats: nothing', app.parseNdbStats(undefined), { since: 0, hours: [], beacons: [] });
}

/* ── the readings' memory (the web's NdbView.update) ── */
{
  const t = new app.NdbTracks();
  t.update(a);
  eq('tracks: words per beacon', t.words(11).map((x) => x.text), ['E?', 'EM']);
  eq('tracks: a word keeps its confidence', t.words(11).map((x) => x.q), ['poor', 'low']);
  t.update(a);   // ★ the same report again: nothing doubled
  eq('tracks: a sequence number is taken once', t.words(11).length, 2);
  eq('tracks: SNR sampled for the active only', [t.snr(7).length, t.snr(2).length], [1, 0]);
  t.update({ ...a, now: NOW + 5 });
  eq('tracks: SNR at most every 10 s', t.snr(7).length, 1);
  t.update({ ...a, now: NOW + 11 });
  eq('tracks: …and again after 10 s', t.snr(7).length, 2);
  const many = { ...a, copy: Array.from({ length: 20 }, (_, i) => ({ s: 100 + i, t: NOW, hz: 433_500, id: 7, text: `W${i}`, q: 'high' as const })) };
  t.update(many);
  eq('tracks: twelve words kept', t.words(7).length, 12);
  eq('tracks: the newest kept', t.words(7)[11].text, 'W19');
  t.clear();
  eq('tracks: cleared', t.words(7).length, 0);
}

/* ── the capability (the radio's vibeserver.json) and the prefs ── */
eq('cap: absent = no control', app.parseNdbCapability({ cw: true }), undefined);
eq('cap: ok', app.parseNdbCapability({ ndb: true, ndbLo: 190_000, ndbHi: 535_000 }), { ok: true, why: '', lo: 190_000, hi: 535_000 });
eq('cap: greyed with the server\'s words', app.parseNdbCapability({ ndb: false, ndbWhy: " this receiver's centre frequency is locked by its owner " }),
   { ok: false, why: "this receiver's centre frequency is locked by its owner", lo: 0, hi: 0 });
eq('cap: greyed without words', app.parseNdbCapability({ ndb: false }).why, 'NDB is not available on this receiver');
eq('cap: a string is not a yes', app.parseNdbCapability({ ndb: 'true' }), undefined);
eq('prefs: default', app.parseNdbMapPrefs(null), { rings: true, unheard: true, autofit: true });
eq('prefs: kept', app.parseNdbMapPrefs('{"rings":false,"unheard":false,"autofit":true}'), { rings: false, unheard: false, autofit: true });
eq('prefs: junk', app.parseNdbMapPrefs('{"rings":"no"'), { rings: true, unheard: true, autofit: true });
eq('stat line', app.ndbStatText(a), '255.0–526.5 kHz · 9 carriers');
eq('stat line starting', app.ndbStatText({ ...a, starting: true }), '255.0–526.5 kHz · 9 carriers · starting…');
ok('empty text before a report', app.ndbEmptyText(null).startsWith('Skimming the whole beacon band'));
eq('empty text once running', app.ndbEmptyText(a), 'No beacons heard yet.');
eq('live: unidentified active carrier', app.ndbRowLive(a.beacons[2], NOW * 1000), true);
eq('live: identified, heard 2 s ago', app.ndbRowLive(a.beacons[0], NOW * 1000), true);
eq('live: quiet', app.ndbRowLive(a.beacons[3], NOW * 1000), false);

/* ── the wiring the app depends on, read from source (the protocol names, the key, the map kind) ── */
{
  const vc = readFileSync(new URL('../src/services/VibeServerClient.ts', import.meta.url), 'utf8');
  for (const t of ["type: 'ndb', on:", "type: 'ndb_find'", "type: 'ndb_stats'", "case 'ndb':", "case 'ndb_off':", "case 'ndb_error':", "case 'ndb_found':", "case 'ndb_stats':"])
    ok(`VibeServerClient speaks ${t}`, vc.includes(t));
  const ws = readFileSync(new URL('../src/services/VibeServerWsClient.ts', import.meta.url), 'utf8');
  eq('every hold gate holds NDB too', (ws.match(/this\.ismHeld \|\| this\.ndbHeld/g) || []).length >= 5, true);
  const ms = readFileSync(new URL('../src/components/ModeSelector.tsx', import.meta.url), 'utf8');
  ok('Mode sheet: the NDB key, its reason line, blockable', ms.includes('ndbKey(false)') && ms.includes('NDB unavailable') && ms.includes("isBlocked('ndb')"));
  const mo = readFileSync(new URL('../src/components/MapOverlay.tsx', import.meta.url), 'utf8');
  ok('map: EXIT NDB', mo.includes('>EXIT NDB<'));
  ok('map: the NDB layer goes in after the ADS-B layer', mo.indexOf('${isAdsb || isNdb ? `<script>${ADSB_LAYER_JS}') >= 0
     && mo.indexOf('${isAdsb || isNdb ? `<script>${ADSB_LAYER_JS}') < mo.indexOf('${isNdb ? `<script>${NDB_LAYER_JS}'));
  ok('map: the layer gets the GPU map or Leaflet, never the compat adapter', mo.includes('VibeNdbLayer.create(GLVM||map'));
  const sc = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
  // ★ db0ef3ec1 moved the rule into sessionHoldNotice.audioQuietFor (shared with the paused note) — check both halves.
  const shn = readFileSync(new URL('../src/services/sessionHoldNotice.ts', import.meta.url), 'utf8');
  ok('screen: audio released in NDB', sc.includes('const audioQuiet = audioQuietFor({ adsb: adsbOn, ism: ismOn, ndb: ndbOn')
     && /export function audioQuietFor[^{]*\{\s*return [^;]*m\.ndb/.test(shn));
  ok('screen: no NDB key on Apple TV', sc.includes('ndbAvail: !IS_APPLE_TV'));
  ok('screen: a mode pick leaves NDB', sc.includes('c.ndb?.(false);\n      ndbUiOff();'));
  const gal = readFileSync(new URL('../src/components/GalleryOverlay.tsx', import.meta.url), 'utf8');
  ok('gallery: the NDB chip', gal.includes("{ id: 'ndb', label: 'NDB' }"));
}

console.log(`test_ndb_app: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
