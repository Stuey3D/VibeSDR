// test_vormap_app.ts — V12 THE VOR / ILS MAP, the client half (src/services/vorMap.ts — shared by the app and the web client):
// the server's report sanitised, the list (lowest frequency first, names earned on air), the status words, the map's items
// (named beacons only, a VOR's radial drawn from the station towards the receiver, bright / faded), and the receiver FIX
// two (or three) radials make — where it lands, how sure it says it is, and that a shallow crossing says so. Then the
// wiring in both clients, both setup screens and the copy. Run: npx tsx scripts/test_vormap_app.ts
import { readFileSync } from 'node:fs';
import { vorMapSanitise, vorMapRowsOrdered, vorMapStatus, vorMapItems, vorMapTrackRows, vorFix, vorFixLine, fixRadials, vorReading,
         vorName, vorStationWords, vorRadialPath, vorSweepWords, vorIdentWhy, vorCard, VORMAP_HINT, VORMAP_EMPTY } from '../src/services/vorMap';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { if (c) pass++; else { fail++; console.log(`FAIL ${what}`); } };
const read = (p: string) => readFileSync(new URL('../' + p, import.meta.url), 'utf8');
const R = 6371, rad = Math.PI / 180;
const dist = (a: { lat: number; lon: number }, b: { lat: number; lon: number }) => {
  const p1 = a.lat * rad, p2 = b.lat * rad, dl = (b.lon - a.lon) * rad;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(Math.sin((p2 - p1) / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2)));
};
const brg = (a: { lat: number; lon: number }, b: { lat: number; lon: number }) => {
  const p1 = a.lat * rad, p2 = b.lat * rad, dl = (b.lon - a.lon) * rad;
  return (Math.atan2(Math.sin(dl) * Math.cos(p2), Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl)) / rad + 360) % 360;
};

// ★ the receiver at IO92nh's centre; three real UK VORs (OurAirports): DTY Daventry, HON Honiley, BNN Bovingdon
const RX = { lat: 52.3125, lon: -0.875 };
const ST = {
  DTY: { ident: 'DTY', name: 'Daventry', cc: 'GB', lat: 52.18030, lon: -1.11389, var: -4.007, dme: '111X', hz: 116400000 },
  HON: { ident: 'HON', name: 'Honiley', cc: 'GB', lat: 52.35670, lon: -1.66361, var: -4.001, dme: '083Y', hz: 113650000 },
  BNN: { ident: 'BNN', name: 'Bovingdon', cc: 'GB', lat: 51.72610, lon: -0.54972, var: -4.001, dme: '084Y', hz: 113750000 },
};
const beacon = (k: keyof typeof ST, extra: any = {}) => {
  const s = ST[k];
  const tb = brg(s, RX);
  return { hz: s.hz, kind: 'vor', win: 2, ident: s.ident, reading: s.ident, votes: 3, held: 1, radial: (tb - s.var + 360) % 360, sigma: 0.3,
           trueBrg: tb, level: -30, first: 1000, last: 1100, fresh: true, wpm: 8,
           station: { ident: s.ident, name: s.name, cc: s.cc, lat: s.lat, lon: s.lon, var: s.var, dme: s.dme, km10: Math.round(dist(s, RX) / 10) * 10, brg: brg(RX, s) },
           ...extra };
};
const report = (beacons: any[], extra: any = {}) => ({ type: 'vormap', now: 1120, rate: 2400000, window: 2, windows: 5, wlo: 111975000, whi: 113975000,
  centre: 112975000, sweep: 1, inWin: 18.4, dwell: 30, progress: 0.52, pos: true,
  wins: [[107975000, 109975000], [109975000, 111975000], [111975000, 113975000], [113975000, 115975000], [115975000, 117975000]],
  beacons, heard: beacons.length, identified: beacons.filter((b) => b.ident).length, dropped: 0, ...extra });

// ── the report ──
const r0 = vorMapSanitise(report([beacon('DTY'), beacon('HON'),
  { hz: 110900000, kind: 'loc', win: 1, ident: 'ILF', reading: 'ILF', votes: 3, held: 2, ddm: 0.08, sdm: 0.4, level: -25, first: 1, last: 1110, fresh: true },
  { hz: 112300000, kind: 'vor', win: 2, ident: '', reading: 'TS', votes: 0, held: 1, radial: 200, sigma: 0.5, level: -40, first: 1, last: 1115, fresh: true },
  { hz: 'x' }, { hz: 99e6 }, { hz: 115000000, kind: 'vor', ident: 'BAD!<script>', station: { lat: 200, lon: 0 } }], { heard: 5, identified: 4 }));
ok(!!r0 && r0.beacons.length === 5 && r0.window === 2 && r0.windows === 5 && r0.wins.length === 5, 'sanitise: the fields, a broken beacon and one off the band dropped');
ok(!!r0 && r0.beacons.find((b) => b.hz === 115000000)?.ident === 'BAD' && r0.beacons.find((b) => b.hz === 115000000)?.station === null,
   'an ident is A–Z / 0–9 only; a station off the globe is none');
ok(vorMapSanitise({ type: 'ndb' }) === null && vorMapSanitise(null) === null, 'not a vormap report: null');
const rows = vorMapRowsOrdered(r0);
ok(rows.map((b) => b.hz).join() === '110900000,112300000,113650000,115000000,116400000', 'the list: lowest frequency first');
ok(vorName(rows[1]) === 'unidentified' && vorStationWords(rows[1]) === 'reading “TS”…', '★ no ident read: "unidentified", never a listed name');
ok(vorStationWords(rows[0]) === 'not in the open data (most runways are not)' && /DDM \+0\.080 · left of the course \(52 %\)/.test(vorReading(rows[0])),
   `a localiser: its DDM words (${vorReading(rows[0])})`);
ok(/^0\d\d° FROM$/.test(vorReading(rows.find((b) => b.ident === 'DTY')!)), `a VOR: "${vorReading(rows.find((b) => b.ident === 'DTY')!)}"`);
ok(vorMapStatus(r0) === 'Window 3 of 5 · 112.00–113.95 MHz · 18 of 30 s · sweep 52 % · 5 heard, 4 identified', `status: "${vorMapStatus(r0)}"`);
ok(vorMapStatus(null).startsWith('starting the sweep') && /108–118 MHz/.test(VORMAP_EMPTY) && /radial/.test(VORMAP_HINT), 'the words before a report');
ok(vorSweepWords(5, 30) === '2.5 min' && vorSweepWords(2, 30) === '60 s', 'how long a sweep takes');
// ★★★ beta94 WHY IT WAS TRUSTED (vorIdentWhy): the server's idBy / reads / expRadial / idRadial, in words on the card
{
  const why = vorMapSanitise(report([beacon('DTY', { votes: 0, reads: 1, idBy: 'radial', radial: 54.0, expRadial: 59.7, idRadial: 54.0 }),
    beacon('HON', { votes: 0, reads: 2, idBy: 'reads' }), beacon('BNN', { idBy: 'votes', votes: 3, reads: 0 }),
    { hz: 112300000, kind: 'vor', win: 2, ident: '', reading: 'TS', votes: 0, held: 1, radial: 200, level: -40, first: 1, last: 1115, fresh: true, idBy: 'radial' },
    beacon('DTY', { hz: 116450000, idBy: 'bogus<', expRadial: 999 })]));
  const w = (hz: number) => vorIdentWhy(why!.beacons.find((b) => b.hz === hz)!);
  ok(w(116400000) === 'ident read once · radial matches (054° vs 060° expected)', `one read + the radial: "${w(116400000)}"`);
  ok(w(113650000) === 'ident read twice · listed on this channel', `two reads: "${w(113650000)}"`);
  ok(w(113750000) === 'read 3 times alike', `consensus: "${w(113750000)}"`);
  ok(w(112300000) === '', 'no ident: no reason, whatever the field says');
  const bogus = why!.beacons.find((b) => b.hz === 116450000)!;
  ok(bogus.idBy === '' && bogus.expRadial === null && w(116450000) === '', 'an unknown idBy / an expected radial off the compass: dropped (an older server says nothing)');
  const card = vorCard(why!.beacons.find((b) => b.hz === 116400000)!, 1120);
  ok(card.includes('DTY — ident read once · radial matches (054° vs 060° expected)'), 'the card\'s Ident row says why');
  ok(/radial matches where you are/.test(VORMAP_EMPTY), 'the empty list says one read can do, with the radial');
}

// ── the map ──
const items = vorMapItems(r0);
const keys = items.map((i) => i.key);
ok(keys.includes('116400000') && keys.includes('113650000') && !keys.includes('112300000') && !keys.includes('110900000'),
   '★ the map holds the NAMED beacons only (an unidentified one, an unlisted localiser: the list only)');
const dty = items.find((i) => i.key === '116400000')!;
ok(Math.abs(dty.lat - ST.DTY.lat) < 1e-6 && Math.abs(dty.lon - ST.DTY.lon) < 1e-6 && dty._lb === 'DTY', 'DTY at its published position, labelled with its ident');
const sh = dty._sh as any;
ok(sh && sh.kind === 'route' && sh.pts.length > 10 && Math.abs(sh.pts[0][0] - ST.DTY.lat) < 1e-6, 'its radial is a line FROM the station');
// ★ every segment walked finely (the line is drawn as 24 straight pieces of a great circle)
const along: number[] = [];
for (let i = 1; i < sh.pts.length; i++) for (let k = 0; k <= 20; k++) {
  const a = sh.pts[i - 1], b = sh.pts[i];
  along.push(dist({ lat: a[0] + (b[0] - a[0]) * k / 20, lon: a[1] + (b[1] - a[1]) * k / 20 }, RX));
}
ok(Math.min(...along) < 0.5, `…along the true bearing, through the receiver (closest ${Math.min(...along).toFixed(2)} km)`);
ok(/Daventry/.test(dty._cd) && /True/.test(dty._cd) && !/<script/.test(JSON.stringify(items)), 'its card; nothing unescaped');
const faded = vorMapItems(vorMapSanitise(report([beacon('DTY', { fresh: false })])));
ok((faded[0]._sh as any).faded === true && /opacity="0.5"/.test(faded[0]._ic.html), '★ not heard this sweep: faded, its line dashed');
const tr = vorMapTrackRows(r0);
ok(tr.length === 5 && tr[0].noPos && tr.find((x) => x.name === 'DTY')?.range === '~10 NM' && tr.find((x) => x.name === 'DTY')?.flag === '🇬🇧', 'the app\'s rows');

// ── the FIX ──
const f2 = vorFix(vorMapSanitise(report([beacon('DTY'), beacon('HON')])));
ok(!!f2 && dist(f2!, RX) < 0.5, `★ two radials cross at the receiver: ${f2 && dist(f2, RX).toFixed(3)} km off`);
ok(!!f2 && f2.km > 0.5 && f2.km < 5 && f2.stations.join() === 'DTY,HON' && /within about [0-9.]+ NM \(they cross at \d+°\)/.test(f2.words),
   `…said in plain words: "${f2?.words}"`);
const f3 = vorFix(vorMapSanitise(report([beacon('DTY'), beacon('HON'), beacon('BNN')])));
ok(!!f3 && f3.crossings === 3 && dist(f3!, RX) < 0.5 && /3 VORs \(BNN, DTY, HON\)/.test(f3.words), `three: a cluster, its centre (${f3?.words})`);
// ★ a reading 1° out: the fix moves, and stays inside what it claims (about one sigma; allow two)
const off = vorFix(vorMapSanitise(report([beacon('DTY', { trueBrg: (brg(ST.DTY, RX) + 1) % 360 }), beacon('HON')])));
ok(!!off && dist(off!, RX) > 0.1 && dist(off!, RX) < 2 * off!.km, `one radial 1° out: ${off && dist(off, RX).toFixed(2)} km off, claims ±${off?.km.toFixed(1)} km`);
// ★ the same station heard twice (one ident) is ONE radial; a faded radial is not used; one radial is not a fix
ok(fixRadials(vorMapSanitise(report([beacon('DTY'), beacon('DTY', { hz: 116450000 }), beacon('HON', { fresh: false })]))).length === 1, 'one radial per station, this sweep only');
ok(vorFix(vorMapSanitise(report([beacon('DTY'), beacon('HON', { fresh: false })]))) === null
   && /second station/.test(vorFixLine(vorMapSanitise(report([beacon('DTY')])))), 'one radial: no fix, and the words say why');
ok(vorFix(vorMapSanitise(report([beacon('DTY'), { ...beacon('HON'), station: null, ident: '' }]))) === null, '★ an unnamed radial has no station to draw from: no fix');
// ★ two stations almost in line: a shallow crossing says so
const far = { lat: 52.30, lon: -2.60 };
const shallowB = beacon('HON', { trueBrg: brg(ST.HON, RX) });
const lineB = { ...beacon('DTY'), hz: 117000000, station: { ...beacon('DTY').station, ident: 'XXX', lat: far.lat, lon: far.lon }, ident: 'XXX', trueBrg: brg(far, RX) };
const fs = vorFix(vorMapSanitise(report([shallowB, lineB])));
ok(!!fs && fs.angle < 20 && /shallow angle/.test(fs.words), `nearly in line (${fs?.angle.toFixed(0)}°): "${fs?.words}"`);
const items2 = vorMapItems(vorMapSanitise(report([beacon('DTY'), beacon('HON')])));
const fixIt = items2.find((i) => i.key === 'fix');
ok(!!fixIt && (fixIt._sh as any).kind === 'area' && /where the radials cross/.test(fixIt._cd), 'the map: the FIX and its circle of uncertainty');
ok(/^FIX ±[0-9.]+ NM from DTY \+ HON$/.test(vorFixLine(vorMapSanitise(report([beacon('DTY'), beacon('HON')])))), 'the fix line');
ok(vorRadialPath({ ...beacon('DTY'), kind: 'loc' } as any).length === 0, 'a localiser has no radial');

// ── the wiring: both clients, the server's words, both setup screens ──
const web = read('web/client/src/main.ts'), spec = read('web/client/src/spectrum.ts'), mob = read('web/client/src/mobile.ts');
ok(/vormap\(on: boolean\) \{\n    if \(this\.askBlocked\(\)\) return;/.test(spec) && /case 'vormap': \{/.test(spec) && /case 'vormap_off':/.test(spec) && /case 'vormap_error':/.test(spec),
   'web: {"type":"vormap"} sent behind ask-before-tuning; its report, off and refusal read');
ok((spec.match(/this\.ndbHeld \|\| this\.vorHeld\)/g) || []).length >= 6, 'web: every hold NDB has, the VOR map has (tune, mode, passband, zoom, rate, IF filter)');
ok(/function vmSetMode\(on: boolean\) \{\n  if \(!on && onlyModeRefuses\('vormap'\)\) return;/.test(web) && /if \(spec\?\.isTuneLocked\(\)\) \{ spec\.vormap\(true\); return; \}/.test(web),
   'web: the only mode and ask before tuning guard it');
// ★★★ beta96 MENU CONSOLIDATION (Stuart 11-26): VOR MAP is a TUNE row of AIR & SEA LIVE (the one key) — not a key of its own
ok(/add\('airsea', 'AIR & SEA LIVE', airSeaCap\(\), airSeaOn\(\), toggleAirSeaLive,/.test(web) && /if \(asAllowed\('vormap'\)\) out\.push\('vormap'\)/.test(web)
   && /if \(id === 'vormap'\) \{ if \(!vmOn\) toggleVmMode\(\); return; \}/.test(web) && /setModeLabel\(asRow, 'adsb', 'AIR & SEA LIVE'\)/.test(mob),
   'web: under AIR & SEA LIVE (TUNES THE RADIO ITSELF and the mode menu) — the map\'s TUNE list enters it');
ok(/if \(j\?\.vormap === true\) vmCap = \{ ok: true, why: '' \};/.test(web) && /\} else vmCap = null;/.test(web), 'web: three states — live, greyed with why, absent (no control at all)');
// ★★★ beta96 stage 8: the VOR map is a live state of the AIR & SEA map (EXIT VOR MAP from liveChrome; its items keep _sh)
ok(/asOpen\(\{ by: 'mode', tab: 'navaids'/.test(web) && /if \(vmOn\) \{ vmSetMode\(false\); return; \}/.test(web) && /vorMapItems\(vmR\)\.map\(\(it: any\) => \(\{ \.\.\.it, key: `vor:\$\{it\.key\}`/.test(web) && /shape: \(it: any\) => it\._sh/.test(web),
   'web: its map — the AIR & SEA map, EXIT VOR MAP, the radials and the fix drawn');
ok(/!anyOpen && \(vmOn \|\| sdOn( \|\| ndbOn)?\) && asView\?\.open && asBy === 'mode'/.test(web), 'web: a gamepad / TV Back leaves it, as NDB\'s');   // ★ beta96 stage 8: NDB shares the line
const app = read('src/screens/SDRScreen.tsx'), ms = read('src/components/ModeSelector.tsx'), mo = read('src/components/MapOverlay.tsx');
const vsc = read('src/services/VibeServerClient.ts'), wsc = read('src/services/VibeServerWsClient.ts'), types = read('src/services/sdrTypes.ts');
ok(/vormap\(on: boolean\) \{\n    if \(this\.askBlocked\(\)\) return;/.test(vsc) && /case 'vormap': \{/.test(vsc) && /case 'vormap_error':/.test(vsc), 'app: sent behind ask-before-tuning; report, off, refusal');
ok((wsc.match(/this\.ndbHeld \|\| this\.vormapHeld/g) || []).length >= 5, 'app: every hold NDB has');
ok(/vormap:    j\.vormap === true \|\| j\.vormap === false/.test(types), 'app: the capability\'s three states from vibeserver.json');
ok(/vormapAvail: !asMenuOne && \(vmCap\?\.ok === true \|\| vmOn\) && !!\(client\.current as any\)\?\.isVibe && !IS_APPLE_TV,/.test(app) && /const asMenuOne = !IS_APPLE_TV && /.test(app),
   'app: its own key only where AIR & SEA LIVE does not stand in (never), and never on the Apple TV (no maps), as NDB — beta96 a TUNE row');
ok((app.match(/onlyRefuses\('vormap'/g) || []).length >= 2 && /askLocked\(dialStateRef\.current\)\) \{ setVmMapOpen\(false\); askGate\(\); return; \}/.test(app),
   'app: EXIT and the Mode key refuse to leave the only mode; ask before tuning');
ok(/case 'vormap': return \{ id, node: vmShown \? vmKey\(\) : null \};/.test(ms) && /addLegend\('VOR MAP', 'vormap'\)/.test(ms), 'app: the VOR MAP key in TUNES THE RADIO ITSELF, beside NDB');
ok(/'EXIT VOR MAP'|EXIT VOR MAP<\/button>/.test(mo) && /KIND==='vormap'/.test(mo) && /kind === 'vormap' \? 'NAVAIDS'/.test(mo), 'app: its map — EXIT VOR MAP, the shared track layer, the NAVAIDS list');
ok(/audioQuietFor\(\{ adsb: adsbOn, ism: ismOn, ndb: ndbOn, ais: aisOn, vormap: vmOn \}\)/.test(app), 'app: nothing to hear — the audio session let go, as NDB');
const setup = read('android/app/src/main/cpp/vibe_setup_page.h'), sms = read('src/screens/ServerModeScreen.tsx');
ok(/\{ id: "vormap", label: "VOR \/ ILS map \(the whole navaid band, swept\)" \}/.test(setup) && /\{ id: 'vormap', label: 'VOR \/ ILS map \(the whole navaid band, swept\)' \}/.test(sms),
   '★ the owner\'s switch on the setup page AND the Android server screen, the same words');
ok(/\["dec_vormap", "VOR \/ ILS map"\]/.test(setup), 'the benchmark\'s row named on the setup page');

console.log(`test_vormap_app: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
