// test_aviation_map.ts — V12 2026-11-16 THE AVIATION MAP's client rules (src/services/aviationMap.ts — the web client and the
// app both draw from them): the shared table parsed defensively, the live DATALINK report and the table joined (each
// aircraft once), the layers present (no dead controls, AIRPORTS always, on by default), an aircraft shown while any of
// its sources is on, the basemap profile, the items / rows / card / KEY, and never a message body.
//   npx tsx scripts/test_aviation_map.ts
import { avLayersOf, avLayersPresent, avToggles, avOn, avProfile, sharedSanitise, avMerge, avHave, avVisible, avPlaneItems, avPlaneRows,
         avPlaneCard, avStationItems, avMapStatus, avLegendRows, avCompose, planeFromLive, AVIATION_TITLE, type AvHave } from '../src/services/aviationMap';
import { parseAvReport } from '../src/services/aviation';

let passes = 0, fails = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  ok ? passes++ : fails++;
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}${ok ? '' : `\n     got  ${JSON.stringify(got)}\n     want ${JSON.stringify(want)}`}`);
};
const yes = (what: string, c: boolean) => eq(what, c, true);

// the server's shape (vibe_aircraft_share.h toJson): A on ADS-B, B on VDL2, one HFDL station
const table = sharedSanitise({
  now: 1800000000, count: 4, aircraft: [
    { key: '4CA97B', icao: '4CA97B', reg: 'EI-EST', flight: 'RYR12AB', type: 'B738', lat: 52.6, lon: -1.3, alt: 36000, trk: 90, gs: 450, posSrc: 'ADS-B', posAge: 1,
      src: ['ADS-B', 'VDL2'], srcAge: { 'ADS-B': 1, VDL2: 40 }, age: 1, n: 30, label: 'H1', labelName: 'message to/from crew or ops', via: 'VDL2 136.975' },
    { key: '400F11', icao: '400F11', flight: 'BAW117', lat: 52.4, lon: -1.6, alt: 31000, posSrc: 'ADS-B', posAge: 2, src: ['ADS-B'], srcAge: { 'ADS-B': 2 }, age: 2, n: 9 },
    { key: '406D4D', icao: '406D4D', flight: 'M38506', lat: 51.0, lon: 3.3, posSrc: 'HFDL', posAge: 900, src: ['HFDL'], srcAge: { HFDL: 900 }, age: 900, n: 2 },
    { key: 'r:GEZWK', reg: 'G-EZWK', src: ['ACARS', 'BOGUS'], srcAge: { ACARS: 5 }, age: 5, n: 1 },
    { key: '<script>', lat: 999, lon: 0, src: ['ADS-B'], srcAge: {}, age: 0, n: 0 },
  ],
  stations: [{ id: 3, name: 'Reykjavik', lat: 64.1, lon: -21.9, khz: 8977, age: 30 }, { id: 'x', lat: 1 }],
});

console.log('── the table, parsed defensively ──');
yes('parsed', !!table);
eq('five rows (a hostile one kept only as text, its position dropped)', table!.aircraft.length, 5);
eq('an unknown source name is dropped', table!.aircraft[3].src, ['ACARS']);
eq('a position off the globe is dropped', table!.aircraft[4].lat, undefined);
eq('one valid station', table!.stations.map((s) => s.name), ['Reykjavik']);
eq('not an object → null', sharedSanitise('nope'), null);

console.log('── the live DATALINK report and the table: each aircraft once ──');
const live = parseAvReport({ type: 'avdata', band: 'vdl2', bandName: 'VDL2', hz: 136862500, msgs: 3, dropped: 0, aircraft: [
  { icao: '4CA97B', reg: 'EI-EST', flight: 'FR1234', label: 'H1', level: -20, n: 3, age: 0, via: 'VDL2 136.975', text: 'OWNER ONLY TEXT' },
  { reg: 'G-EZWK', flight: 'U22468', from: 'EGLL', to: 'LEMD', oooi: 'out', oooiTime: '10:15', label: 'QP', level: -25, n: 1, age: 3, via: 'ACARS 131.725' },
] });
const planes = avMerge(live, table);
const ei = planes.filter((p) => p.icao === '4CA97B');
eq('4CA97B once', ei.length, 1);
eq('its sources: the live VDL2 + the table\'s ADS-B', ei[0].src.slice().sort(), ['ADS-B', 'VDL2']);
eq('the live row\'s flight wins (VDL2 FR1234)', ei[0].flight, 'FR1234');
yes('ADS-B\'s position joins it (the live row had none)', ei[0].lat === 52.6 && ei[0].posSrc === 'ADS-B' && ei[0].trk === 90);
eq('G-EZWK joined by registration (live ACARS, the table\'s ACARS row): once', planes.filter((p) => p.reg === 'G-EZWK').length, 1);
eq('…its key the table\'s registration key', planes.find((p) => p.reg === 'G-EZWK')!.key, 'r:GEZWK');
eq('the other radios\' aircraft are there too', planes.filter((p) => !p.live).map((p) => p.key).sort(), ['400F11', '406D4D', '<script>'].sort());
eq('planeFromLive keys by address, else registration', [planeFromLive(live!.aircraft[0], 0).key, planeFromLive(live!.aircraft[1], 1).key], ['4CA97B', 'r:GEZWK']);

console.log('── the layers present (no dead controls) ──');
const have = avHave(planes, table!.stations);
eq('what each source has', have, { adsb: 3, acars: 1, vdl2: 1, hfdl: 1, adsc: 0, stations: 1 } as AvHave);
eq('DATALINK: VDL2 + ACARS first, then the others with aircraft, stations, AIRPORTS last', avLayersPresent(have, 'datalink'), ['vdl2', 'acars', 'adsb', 'hfdl', 'stations', 'airports']);
eq('ADS-B first from ADS-B\'s key', avLayersPresent(have, 'adsb')[0], 'adsb');
const empty: AvHave = { adsb: 0, acars: 0, vdl2: 0, hfdl: 0, adsc: 0, stations: 0 };
eq('an empty server, HFDL MAP: HFDL (the one asked for), its stations, AIRPORTS — nothing else', avLayersPresent(empty, 'hfdl'), ['hfdl', 'stations', 'airports']);
eq('ADS-C absent while no aircraft has an ADS-C position', avLayersPresent(have, 'datalink').includes('adsc'), false);
eq('one layer alone draws no toggle row', avToggles(['adsb']), []);
eq('AIRPORTS is a toggle', avToggles(avLayersPresent(have, 'datalink')).map((t) => t.label).slice(-2), ['HFDL STATIONS', 'AIRPORTS']);
const def = avLayersOf(null);
yes('every layer on by default — AIRPORTS too', Object.values(def).every(Boolean));
eq('saved toggles read back', avLayersOf('{"adsb":false,"airports":false,"junk":1}').adsb, false);
eq('a corrupt save = the defaults', avLayersOf('{oops').airports, true);
const present = avLayersPresent(have, 'datalink');
eq('the basemap: aero with airports', avProfile(present, def), 'aero');
eq('…aero-plain when AIRPORTS is off', avProfile(present, { ...def, airports: false }), 'aero-plain');

console.log('── shown while ANY source that heard it is on ──');
const offAdsb = { ...def, adsb: false };
yes('4CA97B (ADS-B + VDL2) stays with ADS-B off', avVisible(ei[0], present, offAdsb));
yes('400F11 (ADS-B only) goes with ADS-B off', !avVisible(planes.find((p) => p.key === '400F11')!, present, offAdsb));
yes('avOn: a layer not present is never on', !avOn('adsc', present, def));

console.log('── items, rows, card, status, KEY ──');
const items = avPlaneItems(planes, { lat: 52, lon: -1 });
eq('only aircraft with a position are on the map', items.map((i) => i.key).sort(), ['a:400F11', 'a:406D4D', 'a:4CA97B'].sort());
yes('the glyph points along the track (rotate 90)', items.find((i) => i.key === 'a:4CA97B')!._ic.html.includes('rotate(90 12 12)'));
yes('…and carries the number of sources (2)', />2<\/text>/.test(items.find((i) => i.key === 'a:4CA97B')!._ic.html));
yes('a 15-minute-old position is faded', items.find((i) => i.key === 'a:406D4D')!._ic.html.includes('opacity=".45"'));
const card = avPlaneCard(ei[0], { lat: 52, lon: -1 });
yes('the card: heard on ADS-B and VDL2, with when', /Heard on<\/span><span class="v">ADS-B now · VDL2/.test(card) || /Heard on.*ADS-B.*VDL2/.test(card));
yes('the card: from ADS-B, the range from here', card.includes('from ADS-B') && card.includes('km'));
yes('★ never a message body (the live row\'s owner-only text)', !card.includes('OWNER ONLY TEXT') && !JSON.stringify(items).includes('OWNER ONLY TEXT') && !JSON.stringify(avPlaneRows(planes, null)).includes('OWNER ONLY TEXT'));
yes('a hostile key is escaped in the card', !avPlaneCard(planes.find((p) => p.key === '<script>')!, null).includes('<script>'));
const rows = avPlaneRows(planes, { lat: 52, lon: -1 });
yes('the row: registration · address, the sources joined', rows.find((r) => r.key === 'a:4CA97B')!.second.startsWith('EI-EST · 4CA97B · B738 · VDL2 + ADS-B'));
yes('the OOOI row keeps its route and OUT', rows.find((r) => r.key === 'a:r:GEZWK')!.second.includes('EGLL → LEMD · OUT 10:15 UTC'));
eq('FL360 for 36,000 ft', rows.find((r) => r.key === 'a:4CA97B')!.sp, 'FL360');
yes('the status counts each source and the aircraft heard on more than one', /ADS-B 3 · VDL2 1 · ACARS 1 · HFDL 1 · 1 heard on more than one/.test(avMapStatus(planes)));
eq('the HFDL station item', avStationItems(table!.stations).map((i) => [i.key, i._lb]), [['g:3', 'HFDL Reykjavik']]);
const key = avLegendRows(present).map((r) => r[1]).join(' | ');
yes('the KEY explains the number and the fade, the stations and the airports', /that many sources/.test(key) && /ten minutes/.test(key) && /HFDL ground station/.test(key) && /Airports and runways/.test(key));

console.log('── one composition for both clients ──');
const c = avCompose(live, table, 'datalink', offAdsb, { lat: 52, lon: -1 }, 'app');
yes('ADS-B off: 400F11 neither on the map nor in the list', !c.items.some((i) => i.key === 'a:400F11') && !c.rows.some((r) => r.key === 'a:400F11'));
yes('the station item drawn (HFDL STATIONS on)', c.items.some((i) => i.key === 'g:3'));
yes('the app\'s card wrapper', c.items[0]._cd.includes('max-width:320px'));
eq('the title', AVIATION_TITLE, 'AVIATION MAP');

console.log(`\n${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
