// test_aviation_map.ts — V12 2026-11-16 THE AVIATION MAP's client rules (src/services/aviationMap.ts — the web client and the
// app both draw from them): the shared table parsed defensively, the live DATALINK report and the table joined (each
// aircraft once), the layers present (no dead controls, AIRPORTS always, on by default), an aircraft shown while any of
// its sources is on, the basemap profile, the items / rows / card / KEY, and never a message body.
//   npx tsx scripts/test_aviation_map.ts
import { avLayersOf, avLayersPresent, avToggles, avOn, avProfile, sharedSanitise, avMerge, avHave, avVisible, avPlaneItems, avPlaneRows,
         avPlaneCard, avStationItems, avMapStatus, avLegendRows, avCompose, planeFromLive, AVIATION_TITLE, avNowRule, avAdsbNote, AV_ADSB_NOW_SEC, AV_ADSB_EXPIRE_SEC, avPlaneAdsb, avShowsData, avCreditHtml, AV_DATA_CREDIT,
         type AvHave, avFixBetter } from '../src/services/aviationMap';
import { parseAvReport } from '../src/services/aviation';

let passes = 0, fails = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  ok ? passes++ : fails++;
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}${ok ? '' : `\n     got  ${JSON.stringify(got)}\n     want ${JSON.stringify(want)}`}`);
};
const yes = (what: string, c: boolean) => eq(what, c, true);
// ★ b97-integrate: the 16:31 no-bounce block (from b96-mapcards 197bfae48) calls ok(cond, what) — it was never defined (ReferenceError)
const ok = (c: boolean, what: string) => yes(what, c);

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
yes('the card: from ADS-B, the range from here', card.includes('from ADS-B') && card.includes(' NM '));   // ★ beta95 NM on the aviation map
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

console.log('── beta88 ADS-B only while heard (Stuart: "326 adsb aircraft on the map when i am in ACARS mode … there should be 0") ──');
// the hour the server keeps: 4 ADS-B heard now, 322 more ADS-B-only heard 2–59 min ago, one ADS-B + VDL2 whose ADS-B is 20 min old
const NOW = 1800000000;
const hourRows: any[] = [];
for (let i = 0; i < 326; i++) {
  const age = i < 4 ? 1 + i : 120 + i * 10;
  hourRows.push({ key: (0x400000 + i).toString(16).toUpperCase(), icao: (0x400000 + i).toString(16).toUpperCase(), flight: `T${i}`, lat: 52 + i / 1000, lon: -1, posSrc: 'ADS-B', posAge: age, trk: 90, gs: 400,
                  src: ['ADS-B'], srcAge: { 'ADS-B': age }, age, n: 5 });
}
hourRows.push({ key: '4CA97B', icao: '4CA97B', reg: 'EI-EST', flight: 'RYR12AB', lat: 52.6, lon: -1.3, alt: 36000, trk: 90, gs: 450, posSrc: 'ADS-B', posAge: 1200,
                src: ['ADS-B', 'VDL2'], srcAge: { 'ADS-B': 1200, VDL2: 30 }, age: 30, n: 30 });
// ★ V12 11-24: one ADS-B + VDL2 whose ADS-B is 3 min old — inside the five-minute fade (Stuart)
hourRows.push({ key: '4CA97C', icao: '4CA97C', reg: 'EI-ESU', flight: 'RYR34CD', lat: 52.7, lon: -1.2, alt: 34000, trk: 90, gs: 450, posSrc: 'ADS-B', posAge: 180,
                src: ['ADS-B', 'VDL2'], srcAge: { 'ADS-B': 180, VDL2: 20 }, age: 20, n: 12 });
const tbl = (extra: object) => sharedSanitise({ now: NOW, count: hourRows.length, aircraft: hourRows, stations: [], ...extra })!;
const live1 = tbl({ adsbLive: 1, adsbLast: 1, adsbNow: 4 });
eq('the server\'s ADS-B state parsed', live1.adsb, { live: true, last: 1, off: false });
eq('an older server: live unknown (null)', sharedSanitise({ aircraft: [] })!.adsb, { live: null, last: -1, off: false });
eq('the window is a minute', AV_ADSB_NOW_SEC, 60);
const r1 = avNowRule(avMerge(null, live1), live1);
eq('ADS-B decoding: only the 4 heard in the last minute count (and 4CA97B + 4CA97C, for their VDL2)', r1.planes.length, 6);
eq('…ADS-B 4 in the status, not 326', /ADS-B 4\b/.test(avMapStatus(r1.planes)), true);
const ei2 = r1.planes.find((p) => p.icao === '4CA97B')!;
eq('ADS-B + VDL2, ADS-B 20 min old: kept for its VDL2, ADS-B out of its sources', ei2.src, ['VDL2']);
eq('…its ADS-B history kept for the card', ei2.adsbWas, 1200);
yes('…its 20-minute-old ADS-B position EXPIRED (V12 11-24: removed past five minutes)', ei2.lat == null && ei2.lon == null && ei2.posSrc == null && ei2.lastSeen == null);
const ec2 = r1.planes.find((p) => p.icao === '4CA97C')!;
eq('ADS-B 3 min old: "last seen" (UTC HH:MM), faded, never a live fix', ec2.lastSeen, new Date((NOW - 180) * 1000).toISOString().slice(11, 16));
yes('…no track or speed drawn for it', ec2.trk == null && ec2.gs == null && ec2.lat === 52.7);
eq('the fade ends at five minutes', AV_ADSB_EXPIRE_SEC, 300);
const c88 = avCompose(null, live1, 'datalink', avLayersOf(null), { lat: 52, lon: -1 }, 'web');
yes('…the expired one is not on the map', !c88.items.some((i) => i.key === 'a:4CA97B'));
const eiItem = c88.items.find((i) => i.key === 'a:4CA97C')!;
yes('…drawn faded', eiItem._ic.html.includes('opacity=".45"'));
yes('…the card says Last seen and not since, and ADS-B as history', /Last seen/.test(eiItem._cd) && /not heard there since/.test(eiItem._cd) && /ADS-B 3 min ago \(not since\)/.test(eiItem._cd));
yes('…the row says ADS-B last seen', c88.rows.find((r) => r.key === 'a:4CA97C')!.second.includes(`ADS-B last seen ${ec2.lastSeen} UTC`));
eq('the status line counts now', c88.status, '6 aircraft (5 on the map) · ADS-B 4 · VDL2 2');
// Stuart's case: the ADS-B radio is in ACARS mode — the server says no radio is decoding ADS-B
const off1 = tbl({ adsbLive: 0, adsbLast: 300, adsbNow: 0 });
const cOff = avCompose(null, off1, 'datalink', avLayersOf(null), null, 'web');
eq('no radio decoding ADS-B: ADS-B count 0 — only 4CA97B + 4CA97C (their VDL2) remain', cOff.planes.map((p) => p.key).sort(), ['4CA97B', '4CA97C']);
yes('…even the 4 heard seconds ago are gone', !cOff.items.some((i) => i.key === 'a:400000'));
eq('…the status says why, in plain words', cOff.status, '2 aircraft (1 on the map) · VDL2 2 · ADS-B: this server\'s ADS-B radio is in use for something else');
yes('…the ADS-B layer toggle is absent (nothing to show)', !cOff.present.includes('adsb'));
const none = avCompose(null, tbl({ adsbLive: 0, adsbLast: -1 }), 'datalink', avLayersOf(null), null, 'web');
yes('never decoded ADS-B (but rows from before): "no radio on this server is decoding it"', /ADS-B: no radio on this server is decoding it/.test(none.status));
const old = avCompose(null, tbl({}), 'datalink', avLayersOf(null), null, 'web');
eq('an older server (cannot tell): the age alone — 4 ADS-B now', /ADS-B 4\b/.test(old.status), true);
const quiet = avCompose(null, sharedSanitise({ now: NOW, aircraft: hourRows.slice(10, 20), stations: [], adsbLive: 1, adsbLast: 0 })!, 'adsb', avLayersOf(null), null, 'web');
eq('decoding but nothing heard for a minute: "none heard in the last minute"', quiet.status, 'ADS-B: none heard in the last minute');
eq('the owner switched ADS-B off', avAdsbNote([], { live: false, last: 10, off: true }, 3, 'datalink'), 'ADS-B: switched off on this server');
eq('a datalink-only server that never had ADS-B: no ADS-B words at all', avAdsbNote([], { live: false, last: -1, off: false }, 0, 'hfdl'), '');
eq('ADS-B aircraft on the map: no note', avAdsbNote(r1.planes, live1.adsb, 322, 'datalink'), '');
yes('the input rows are not changed by the rule', avMerge(null, live1).length === 328 && live1.aircraft[326].src.includes('ADS-B'));
yes('the KEY explains "last seen"', avLegendRows(['adsb', 'airports']).some((r) => /last seen/.test(r[1])));

console.log('── beta91: the ADS-B aircraft data on every source (vibe_aircraft_info.h) ──');
{
  // the server's beta91 row: VDL2 only, the ADS-B data's names beside the row's own (vibe_aircraft_share.h toJson + Extra)
  const t91 = sharedSanitise({ now: 1800000000, aircraft: [
    { key: '4CA2D6', icao: '4CA2D6', reg: 'EI-DLJ', type: 'B738', flight: 'RYR8CK', desc: 'BOEING 737-800', wtc: 'M', country: 'IE', countryName: 'Ireland',
      op: 'RYR', opName: 'Ryanair', logo: 'RYR', sil: 'B738', lat: 53.2, lon: -6.1, posSrc: 'VDL2', posAge: 30, src: ['VDL2'], srcAge: { VDL2: 30 }, age: 30, n: 4 },
    { key: 'f:U21', flight: 'U21', op: 'EZY', opName: 'easyJet', route: { from: 'EHAM', to: 'EGKK', fromIata: 'AMS', toIata: 'LGW', plausible: true },
      lat: 52.0, lon: 2.0, posSrc: 'ACARS', posAge: 60, src: ['ACARS'], srcAge: { ACARS: 60 }, age: 60, n: 1 },
    { key: 'AE1234', icao: 'AE1234', reg: '03-3123', country: 'US', mil: 1, op: '<b>', logo: '../../x', lat: 50, lon: -20, posSrc: 'HFDL', posAge: 100,
      src: ['HFDL'], srcAge: { HFDL: 100 }, age: 100, n: 1 },
    { key: 'r:GABCD', reg: 'G-ABCD', src: ['ACARS'], srcAge: { ACARS: 5 }, age: 5, n: 1 },
  ], stations: [] })!;
  const [v, a, m, plain] = t91.aircraft;
  eq('the info is kept (r / t stay the row\'s reg / type)', v.info, { desc: 'BOEING 737-800', wtc: 'M', country: 'IE', countryName: 'Ireland', op: 'RYR', opName: 'Ryanair', logo: 'RYR', sil: 'B738' });
  eq('a hostile operator badge and a traversal logo key are dropped; mil kept', m.info, { country: 'US', mil: 1 });
  eq('a row the server sent no data for has none', plain.info, undefined);
  const sv = avPlaneAdsb(v)!;
  eq('VDL2-only → the ADS-B layer gets t (its silhouette), r, op (the badge), logo / sil keys, the flag', [sv.hex, sv.r, sv.t, sv.op, sv.opName, sv.logo, sv.sil, sv.country, sv.desc],
     ['4ca2d6', 'EI-DLJ', 'B738', 'RYR', 'Ryanair', 'RYR', 'B738', 'IE', 'BOEING 737-800']);
  yes('…and stays a datalink (sparse) aircraft', sv.sparse === true);
  const sa = avPlaneAdsb(a)!;
  eq('an ACARS flight-only aircraft: the operator and the database route (it named no airports)', [sa.op, sa.route?.from, sa.route?.to], ['EZY', 'EHAM', 'EGKK']);
  const sa2 = avPlaneAdsb({ ...a, from: 'EGLL', to: 'LEMD' })!;
  yes('…the datalink\'s own airports win: no database route', sa2.route === undefined && sa2.rowsExtra!.some(([k, val]) => k === 'Route' && val === 'EGLL → LEMD'));
  eq('military over HFDL: mil for the layer\'s MIL tone', avPlaneAdsb(m)!.mil, 1);
  const rows = avPlaneRows([v, a, m], null);
  yes('the list: the flag, the operator, the type', rows[0].flag === '🇮🇪' && /Ryanair/.test(rows[0].second) && /B738/.test(rows[0].second));
  yes('the list: the database route where the aircraft named none', /AMS → LGW/.test(rows[1].second) && /easyJet/.test(rows[1].second));
  // ★ b97-integrate: MIL is a tag beside the name now (acTags.ts, Stuart 11-27) — the row carries mil, the subtitle no longer says it
  yes('the list: MIL (the row\'s mil tag, not the subtitle)', (rows[2] as { mil?: boolean }).mil === true && !/\bMIL\b/.test(rows[2].second) && rows[2].flag === '🇺🇸');
  // the live DATALINK row (no data) joined with the table's row: the table's data carried over
  const live = parseAvReport({ type: 'avdata', aircraft: [{ icao: '4CA2D6', flight: 'RYR8CK', via: 'VDL2 136.975', age: 2, n: 1 }] });
  const merged = avMerge(live, t91).find((p) => p.icao === '4CA2D6')!;
  eq('the live row + the table: the aircraft data comes with the table', [merged.live, merged.info?.op, merged.reg, merged.type], [true, 'RYR', 'EI-DLJ', 'B738']);
  const c91 = avCompose(null, t91, 'datalink', avLayersOf(null), null, 'web');
  const it = c91.items.find((x: any) => x._adsb?.hex === '4ca2d6') as any;
  yes('avCompose hands the layer the data', it && it._adsb.op === 'RYR' && it._adsb.t === 'B738');
  // ★★ the ODC-By notice on the list: while any row shows the aircraft data, never otherwise
  yes('the list carries the notice while a row shows the data', c91.credit === true && avShowsData([v]) && avShowsData([{ ...plain, type: 'A320' }]));
  yes('…not when no row does (registrations the datalink heard itself)', !avShowsData([plain]) && avCompose(null, sharedSanitise({ now: 1, aircraft: [{ key: 'r:GABCD', reg: 'G-ABCD', lat: 51, lon: 0, posSrc: 'ACARS', src: ['ACARS'], srcAge: { ACARS: 1 }, age: 1, n: 1 }], stations: [] }), 'datalink', avLayersOf(null), null).credit === false);
  const ch = avCreditHtml();
  yes('the card\'s words and links', ch.includes('href="https://opendatacommons.org/licenses/by/1-0/"') && ch.includes('href="https://github.com/Mictronics/aircraft-database"')
      && ch.includes('contains information from Mictronics aircraft database, which is made available under the') && ch.includes('VRS standing data</a> (CC0)'));
  yes('the RN words are the same text', `${AV_DATA_CREDIT.lead}${AV_DATA_CREDIT.mic}${AV_DATA_CREDIT.mid}${AV_DATA_CREDIT.odc}`.length > 60 && ch.includes(AV_DATA_CREDIT.mid));
}

console.log('── b96-rangefeed: an ADS-B position is never sparse, whoever heard it; a datalink one always is (Stuart 11-27) ──');
{
  // the Lenovo's AIR & SEA map, 11-27: ADS-B from a server-level Beast feed (PiAware) — aircraft.json rows as it sent them
  const NOW2 = 1795770661;
  const feedTbl = sharedSanitise({ now: NOW2, count: 5, adsbLive: 1, adsbLast: 0, adsbNow: 3,
    feeds: [{ title: 'PiAware', label: 'ADS-B', state: 'connected', aircraft: 3 }], aircraft: [
      // a fresh feed fix (radios ["feed"])
      { key: '406A99', icao: '406A99', flight: 'VKG520', lat: 53.7, lon: -0.3, posAge: 1, posSrc: 'ADS-B', alt: 36000, trk: 20, gs: 450, src: ['ADS-B'], srcAge: { 'ADS-B': 0 }, age: 0, n: 900, radios: ['feed'], first: 300 },
      // at the edge of range: still answering (DF11/DF4), but its position 90 s old — "last seen"
      { key: '484C5A', icao: '484C5A', flight: 'KLM1523', lat: 53.9, lon: 2.4, posAge: 90, posSrc: 'ADS-B', alt: 37000, src: ['ADS-B'], srcAge: { 'ADS-B': 4 }, age: 4, n: 300, radios: ['feed'] },
      // another radio on this server, heard on ADS-B
      { key: '4CA97B', icao: '4CA97B', flight: 'RYR12AB', lat: 52.6, lon: -1.3, posAge: 0, posSrc: 'ADS-B', src: ['ADS-B'], srcAge: { 'ADS-B': 0 }, age: 0, n: 30, radios: ['59a1a7d0'] },
      // datalink positions: VDL2, and an ADS-C report over HFDL
      { key: '406D4D', icao: '406D4D', flight: 'M38506', lat: 51.0, lon: 3.3, posAge: 40, posSrc: 'VDL2', src: ['VDL2'], srcAge: { VDL2: 40 }, age: 40, n: 2 },
      { key: 'A1B2C3', icao: 'A1B2C3', flight: 'UAL901', lat: 55.0, lon: -20.0, posAge: 300, posSrc: 'ADS-C', src: ['HFDL', 'ADS-C'], srcAge: { HFDL: 300, 'ADS-C': 300 }, age: 300, n: 1 },
    ] })!;
  const planes = avNowRule(avMerge(null, feedTbl), feedTbl).planes;
  const shape = (icao: string) => avPlaneAdsb(planes.find((p) => p.icao === icao)!)!;
  eq('a Beast feed\'s fresh ADS-B fix: NOT sparse (it dead-reckons and grows the live outline)', shape('406A99').sparse, false);
  eq('…another radio\'s ADS-B fix: not sparse', shape('4CA97B').sparse, false);
  const ls = planes.find((p) => p.icao === '484C5A')!;
  yes('a "last seen" ADS-B fix (90 s old, still answering) stays "last seen"', !!ls.lastSeen && ls.trk == null && ls.gs == null);
  eq('…and is NOT sparse either: an ADS-B position, ADS-B\'s stale tone (seen_pos past the fade), never the datalink glide', [shape('484C5A').sparse, (shape('484C5A').seen_pos ?? 0) > 600], [false, true]);
  eq('a VDL2 position: sparse (glides, never grows the ADS-B outline)', shape('406D4D').sparse, true);
  eq('an ADS-C position over HFDL: sparse', shape('A1B2C3').sparse, true);
}

console.log('── b96-2 "Not at this site": an off-site feed\'s aircraft are drawn, never offered to the live outline (Stuart 11-27) ──');
{
  const NOW3 = 1795770661;
  const tbl = sharedSanitise({ now: NOW3, count: 3, adsbLive: 1, adsbLast: 0, adsbNow: 3, aircraft: [
    // the server marked it: its position is an off-site feed's only (vibe_aircraft_share.h offsiteOnly)
    { key: '4B1A2C', icao: '4B1A2C', flight: 'SWR38', lat: 47.4, lon: 8.5, posAge: 1, posSrc: 'ADS-B', alt: 31000, src: ['ADS-B'], srcAge: { 'ADS-B': 0 }, age: 0, n: 50, radios: ['feed'], offsite: 1 },
    // an at-site feed's: no mark
    { key: '406A99', icao: '406A99', flight: 'VKG520', lat: 53.7, lon: -0.3, posAge: 1, posSrc: 'ADS-B', src: ['ADS-B'], srcAge: { 'ADS-B': 0 }, age: 0, n: 900, radios: ['feed'] },
    // junk in the field is not a mark
    { key: '4CA97B', icao: '4CA97B', flight: 'RYR12AB', lat: 52.6, lon: -1.3, posAge: 0, posSrc: 'ADS-B', src: ['ADS-B'], srcAge: { 'ADS-B': 0 }, age: 0, n: 30, offsite: 'yes' },
  ] })!;
  const planes = avNowRule(avMerge(null, tbl), tbl).planes;
  const shape = (icao: string) => avPlaneAdsb(planes.find((p) => p.icao === icao)!)!;
  eq('the off-site row reads offsite and is still drawn (an ADS-B aircraft, not sparse)', [planes.find((p) => p.icao === '4B1A2C')?.offsite, !!shape('4B1A2C'), shape('4B1A2C').sparse], [true, true, false]);
  eq('…and the layer is told: offsite, so its live outline skips it', shape('4B1A2C').offsite, true);
  eq('an at-site feed\'s row: no mark (it grows the outline)', [planes.find((p) => p.icao === '406A99')?.offsite, shape('406A99').offsite], [undefined, undefined]);
  eq('junk ("yes") is not a mark', shape('4CA97B').offsite, undefined);
  // the live report (this radio, now) joined with the table's off-site row: a newer table fix keeps the mark, this radio's own clears it
  const live = { aircraft: [{ icao: '4B1A2C', flight: 'SWR38', lat: 47.39, lon: 8.49, posAge: 30, posSrc: 'ADS-C', via: 'HFDL', age: 30, n: 1 }] } as any;
  const m = avMerge(live, tbl).find((p) => p.icao === '4B1A2C')!;
  eq('merged with a live datalink row: the table\'s newer off-site fix brings its mark', [m.posSrc, m.offsite], ['ADS-B', true]);
}

// ── ★★ Stuart 16:31: a fresh ADS-B fix is never replaced by a datalink report of the same aircraft (no bouncing) ──
{
  const yes2 = (c: boolean, w: string) => yes(w, c);
  yes2(!avFixBetter({ posSrc: 'ADS-C', posAge: 0 }, { posSrc: 'ADS-B', posAge: 1 }), 'an ADS-C report (0 s) does not replace a live ADS-B fix (1 s)');
  yes2(!avFixBetter({ posSrc: 'VDL2', posAge: 0 }, { posSrc: 'ADS-B', posAge: 30 }), 'nor an FMS / label-16 one');
  yes2(avFixBetter({ posSrc: 'ADS-B', posAge: 2 }, { posSrc: 'ADS-C', posAge: 0 }), 'a live ADS-B fix replaces a datalink report');
  yes2(avFixBetter({ posSrc: 'ADS-C', posAge: 5 }, { posSrc: 'ADS-B', posAge: 600 }), 'an ADS-B fix gone stale (10 min) gives way to a newer report');
  yes2(avFixBetter({ posSrc: 'ADS-B', posAge: 1 }, { posSrc: 'ADS-B', posAge: 4 }) && !avFixBetter({ posSrc: 'ADS-B', posAge: 4 }, { posSrc: 'ADS-B', posAge: 1 }), 'two ADS-B fixes: the newer');
  // interleaved: the track never flips back to the report while ADS-B is live
  let cur = { posSrc: 'ADS-B', posAge: 0, at: 'adsb' };
  let flips = 0;
  for (let t = 0; t < 40; t++) {
    const nxt = t % 3 === 0 ? { posSrc: 'ADS-C', posAge: 0, at: 'report' } : { posSrc: 'ADS-B', posAge: t % 2, at: 'adsb' };
    if (avFixBetter(nxt, cur)) { if (nxt.at !== cur.at) flips++; cur = nxt; } else cur = { ...cur, posAge: (cur.posAge ?? 0) + 1 };
  }
  yes2(flips === 0 && cur.at === 'adsb', `interleaved ADS-B and ADS-C reports: the position never leaves ADS-B (${flips} flips)`);
}
// ── ★★ 11-28 the heading DERIVED from two fixes (the server's "trk" + "trkSrc":"derived") — turns the icon only ──
{
  const t = sharedSanitise({ now: 1800000000, aircraft: [
    { key: '406D4D', icao: '406D4D', flight: 'BAW1', lat: 55.5, lon: -19.1, trk: 45, trkSrc: 'derived', gs: 470, posSrc: 'HFDL', posAge: 30, src: ['HFDL'], srcAge: { HFDL: 30 }, age: 30, n: 3 },
    { key: '4CA97B', icao: '4CA97B', flight: 'RYR1', lat: 52.6, lon: -1.3, trk: 90, gs: 450, posSrc: 'ADS-B', posAge: 1, src: ['ADS-B'], srcAge: { 'ADS-B': 1 }, age: 1, n: 9 },
    { key: '400001', icao: '400001', flight: 'EZY1', lat: 51, lon: 0, posSrc: 'VDL2', posAge: 10, src: ['VDL2'], srcAge: { VDL2: 10 }, age: 10, n: 1 },
  ] })!;
  const hf = t.aircraft.find((x) => x.key === '406D4D')!, ad = t.aircraft.find((x) => x.key === '4CA97B')!, vd = t.aircraft.find((x) => x.key === '400001')!;
  yes('the server\'s derived heading is read and MARKED', hf.trk === 45 && hf.trkDerived === true);
  yes('a reported track is not marked', ad.trk === 90 && !ad.trkDerived);
  const sh = avPlaneAdsb(hf)!, sa = avPlaneAdsb(ad)!, sv = avPlaneAdsb(vd)!;
  yes('the layer gets it as trkSrc derived (turn the icon, never fly on it), with the position\'s source', sh.track === 45 && sh.trkSrc === 'derived' && sh.posSrc === 'HFDL');
  yes('…a reported track goes as a track', sa.track === 90 && sa.trkSrc === undefined);
  yes('…no heading at all: the layer derives its own from two fixes of the same source (posSrc given)', sv.track === undefined && sv.posSrc === 'VDL2');
  yes('the card says where a derived heading came from, never "track"', /Heading<\/?[^>]*>?[^]*45° \(from its last two positions\)/.test(avPlaneCard(hf, null)) && !/track 45/.test(avPlaneCard(hf, null)));
  const live = parseAvReport({ type: 'avdata', aircraft: [{ icao: '406D4D', flight: 'BAW1', lat: 55.5, lon: -19.1, posAge: 5, posSrc: 'HFDL', trk: 45, trkSrc: 'derived', via: 'HFDL 8977', level: -20, n: 2, age: 5 },
                                                          { icao: '400002', lat: 50, lon: 0, posAge: 5, posSrc: 'ACARS', trk: 10, via: 'ACARS 131.725', level: -20, n: 1, age: 5 }] } as any);
  const lp = live ? live.aircraft.map(planeFromLive) : [];
  yes('the live datalink report\'s derived heading reaches the plane, marked', lp[0]?.trk === 45 && lp[0]?.trkDerived === true);
  yes('…a live row\'s unmarked trk is never taken (the report sends only derived ones)', lp[1]?.trk === undefined);
  const mg = avMerge(null, t).find((x) => x.key === '406D4D')!;
  yes('the merge keeps the mark with the heading', mg.trk === 45 && mg.trkDerived === true);
}
console.log(`\n${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
