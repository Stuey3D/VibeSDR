// test_shipping_map.ts — V12 2026-11-02 THE SHIPPING MAP's client rules (src/services/shippingMap.ts, navtexWind.ts — the web
// client and the app both draw from them): which layers are present (no dead controls), the ships from the live report
// and the store, the DSC pins from the box and the gallery, the time filter, the KEY, and the weather arrows.
//   npx tsx scripts/test_shipping_map.ts
import { shipLayersPresent, shipToggles, shipOn, shipLayersOf, storeSanitise, shipShips, shipItems, shipRows, dscPinsFromCalls, dscPinsFromGallery,
         dscPinsMerge, dscInRange, dscPinItems, dscIcon, shipLegendRows, windLegendRows, SHIPPING_TITLE, type ShipHave } from '../src/services/shippingMap';
import { navWindSanitise, windSvg, windLabel, windColour, windLength, windWords, windBox } from '../src/services/navtexWind';
import { navMetSanitise, navMetIcon, navMetCard, navMetAria } from '../src/services/navtexWarn';

let passes = 0, fails = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  ok ? passes++ : fails++;
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}${ok ? '' : `\n     got  ${JSON.stringify(got)}\n     want ${JSON.stringify(want)}`}`);
};
const yes = (what: string, c: boolean) => eq(what, c, true);

console.log('── the layers present (no dead controls) ──');
const none: ShipHave = { ships: 0, dsc: 0, stations: 0, warnings: 0, weather: 0, aisMode: false };
eq('NAVTEX only: the NAVTEX layers present, no SHIPS or DSC', shipLayersPresent({ ...none, stations: 3, warnings: 2, weather: 5 }, 'navtex'), ['stations', 'warnings', 'weather']);
eq('NAVTEX with no warnings: WARNINGS absent', shipLayersPresent({ ...none, stations: 3, weather: 5 }, 'navtex'), ['stations', 'weather']);
eq('NAVTEX + a ship store + DSC alerts: all five, NAVTEX first', shipLayersPresent({ ...none, stations: 3, warnings: 1, weather: 1, ships: 12, dsc: 2 }, 'navtex'),
   ['stations', 'warnings', 'weather', 'ships', 'dsc']);
eq('AIS mode with nothing heard yet: SHIPS is there (the mode is the map)', shipLayersPresent({ ...none, aisMode: true }, 'ships'), ['ships']);
eq('AIS mode on a NAVTEX server: SHIPS first, then the NAVTEX layers with data', shipLayersPresent({ ...none, aisMode: true, stations: 4, weather: 9 }, 'ships'),
   ['ships', 'stations', 'weather']);
eq("DSC's MAP: DSC first, ships from the store next", shipLayersPresent({ ...none, dsc: 1, ships: 3 }, 'dsc'), ['dsc', 'ships']);
eq('one layer alone: no toggle row at all', shipToggles(['ships']), []);
eq('the toggles in the order present', shipToggles(['dsc', 'ships', 'weather']).map((t) => t.label), ['DSC', 'SHIPS', 'WEATHER']);
const L = shipLayersOf('{"ships":false}', '{"weather":false}');
eq('saved toggles: the shipping map\'s and the old NAVTEX map\'s', [L.ships, L.weather, L.dsc, L.stations], [false, false, true, true]);
yes('a layer alone is drawn even when its toggle was saved off', shipOn('ships', ['ships'], L));
yes('an absent layer is never drawn', !shipOn('dsc', ['ships', 'stations'], L));
eq('the title', SHIPPING_TITLE, 'SHIPPING MAP');

console.log('── SHIPS: the live report and the store ──');
const store = storeSanitise({ now: 1000, count: 3, total: 3, vessels: [
  { mmsi: 235000001, kind: 'A', name: 'LIVE ONE (stored)', lat: 50.8, lon: -1.3, seen: 30, age: 900, posAge: 30, count: 9, pts: [[50.79, -1.31, 600], [50.8, -1.3, 30]] },
  { mmsi: 235000002, kind: 'A', name: 'OLD SHIP', lat: 50.9, lon: -1.2, seen: 7200, age: 9000, posAge: 7200, count: 3 },
  { mmsi: 235000003, kind: 'B', name: 'DAY OLD', lat: 50.95, lon: -1.1, seen: 90000, age: 99000, posAge: 90000, count: 2 },
  { mmsi: -4, kind: 'A' }, { mmsi: 'x' },
] })!;
eq('the store sanitised: three ships, the broken ones dropped, the track kept', [store.vessels.length, store.pts.get(235000001)?.length], [3, 2]);
const live = { type: 'ais', vessels: [{ mmsi: 235000001, kind: 'A', name: 'LIVE ONE', lat: 50.81, lon: -1.29, seen: 2, age: 950, posAge: 2, count: 12 }] } as any;
let v = shipShips(live, store, 0);
eq('ALL: the live ship wins over its stored copy, then the store', v.map((x) => x.name), ['LIVE ONE', 'OLD SHIP', 'DAY OLD']);
v = shipShips(live, store, 3600);
eq('1 h: only what was heard in the hour (the live one is now)', v.map((x) => x.name), ['LIVE ONE']);
v = shipShips(null, store, 86400);
eq('24 h with no AIS here: the store alone', v.map((x) => x.name), ['LIVE ONE (stored)', 'OLD SHIP']);
const it = shipItems(shipShips(null, store, 0), { lat: 50.8, lon: -1.3 });
yes('a ship heard over an hour ago is drawn faded', it[1]._ic.html.startsWith('<div style="opacity:.45">') && !it[0]._ic.html.startsWith('<div style="opacity'));
yes('ship keys never collide with the NAVTEX keys', it.every((x) => x.key.startsWith('s:')));
eq('the rows: nearest first', shipRows(shipShips(null, store, 0), { lat: 50.8, lon: -1.3 }).map((r) => r.name), ['LIVE ONE (stored)', 'OLD SHIP', 'DAY OLD']);
eq('a server with the store switched off', storeSanitise({ vessels: [], count: 0, total: 0, off: 1 })?.off, true);

console.log('── DSC: the box and the gallery ──');
const calls = [{ id: 1, utc: 999_000, rf: 2187500, lat: 50.1, lon: -4.2, level: 3, line: 'DISTRESS 232001234 sinking', self: '232001234', distressId: '232001234' },
               { id: 2, utc: 500_000, rf: 2187500, lat: null, lon: null, level: 0, line: 'no position', self: '1', distressId: '' }] as any;
const live2 = dscPinsFromCalls(calls);
eq('the box: only calls with a position', live2.map((p) => [p.who, p.level, p.t]), [['232001234', 3, 999]]);
const gal = dscPinsFromGallery({ items: [
  { f: 'dsc-1.txt', k: 'dsc', t: 1000, title: 'DISTRESS 232001234 sinking', plat: 50.1, plon: -4.2, lv: 3 },
  { f: 'dsc-2.txt', k: 'dsc', t: 400, title: 'URGENCY 227000001 medical', plat: 47.3, plon: -3.1, lv: 2 },
  { f: 'nav.txt', k: 'navtex', t: 900, title: 'x', plat: 1, plon: 1 },
  { f: 'dsc-3.txt', k: 'dsc', t: 800, title: 'no pin' },
  { f: '../evil', k: 'dsc', t: 801, title: 'bad name', plat: 10, plon: 10, lv: 0 },
] });
eq('the gallery: the pinned DSC items only', gal.map((p) => [p.who, p.level, p.f]), [['232001234', 3, 'dsc-1.txt'], ['227000001', 2, 'dsc-2.txt'], ['', 0, '']]);
const both = dscPinsMerge(live2, gal);
eq('the same call heard both ways is ONE pin (box + gallery alert within 2 min)', both.length, 3);
eq('the time filter: the last 5 min at t=1000 keeps 999 and 801, not 400', dscInRange(both, 300, 1000).map((p) => p.t), [999, 801]);
const dItems = dscPinItems(both, null, 1000);
yes('a distress pin is bigger and ringed, ranked above everything', dItems[0]._ic.w === 30 && dItems[0]._ic.html.includes('SOS') && (dItems[0] as any)._rk > 2e13);
yes('the DSC glyph is a pin, not a ship', dscIcon(0).html.includes('<path d="M11 2 C7 2'));

console.log('── the KEY ──');
const keyAll = shipLegendRows(['ships', 'dsc']).map((r) => r[1]);
yes('ships and DSC glyphs in the KEY', keyAll.some((w) => w.startsWith('Ship under way')) && keyAll.some((w) => w.startsWith('DSC DISTRESS')));
eq('no ship rows when SHIPS is absent', shipLegendRows(['dsc']).some((r) => r[1].startsWith('Ship')), false);
eq('the weather arrows have their own KEY rows', windLegendRows().length, 6);

console.log('── the weather arrows ──');
const w1 = navWindSanitise({ k: 'a', bf: 8, sev: 2, from: 225, to: 45, chg: 'veer', k2: 'a', bf2: 9, sev2: 2, from2: 270, to2: 90 })!;
eq('sanitised', [w1.k, w1.bf, w1.to, w1.chg, w1.k2, w1.to2], ['a', 8, 45, 'veer', 'a', 90]);
eq('a broken wind is null', navWindSanitise({ k: 'zz' }), null);
eq('an arrow with no direction is drawn as none', navWindSanitise({ k: 'a', bf: 5, sev: 0 })!.k, 'n');
yes('bigger for a stronger wind', windLength(8) > windLength(4) && windLength(4) > windLength(0));
eq('colours: ink · amber · red', [windColour(0), windColour(1), windColour(2)], ['#cfe6ff', '#ffb74d', '#ff5a4a']);
const s1 = windSvg(w1);
yes('the arrow points downwind: rotated to 45° (SW wind)', s1.svg.includes('rotate(45 '));
yes('the later wind faint and dashed, BEHIND the first', s1.svg.indexOf('opacity="0.5"') >= 0 && s1.svg.indexOf('stroke-dasharray="3 3"') < s1.svg.lastIndexOf('rotate(45 '));
yes('the box fits the longest arrow', s1.size === windBox(w1) && s1.size >= windLength(9));
const vrb = windSvg(navWindSanitise({ k: 'v', bf: 3, sev: 0 })!), cyc = windSvg(navWindSanitise({ k: 'c', bf: 9, sev: 2 })!);
yes('VARIABLE: a dashed ring, no arrow', vrb.svg.includes('<circle') && vrb.svg.includes('VRB') && !vrb.svg.includes('rotate('));
yes('CYCLONIC: a curling arrow (an arc), red', cyc.svg.includes(' A') && cyc.svg.includes('#ff5a4a') && !cyc.svg.includes('rotate('));
eq('the compact label: "6–8 · ROUGH"', windLabel({ fmin: 6, fmax: 8, sea: 5, vis: 3 }, w1), '6–8 · ROUGH');
eq('…with fog', windLabel({ fmin: 4, fmax: 4, sea: 3, vis: 0 }, w1), '4 · SLIGHT · FOG');
eq('the words', windWords(w1), 'wind from the south-west, force 8, veering from the west, force 9 later — gale');
const met = navMetSanitise({ now: 2000, stale: 86400, areas: [{ id: 'thames', name: 'Thames', lat: 52.2, lon: 2.3, kind: 's', repeats: 0, fc: [
  { t: 1990, dir: 'SW', fmin: 6, fmax: 8, gale: 8, sea: 5, vis: 3, wx: 'rain', f: 'a.txt', txt: 'SW 6 TO 8. ROUGH.', wind: { k: 'a', bf: 8, sev: 2, from: 225, to: 45 } }] },
  { id: 'dover', name: 'Dover', lat: 51, lon: 1.4, kind: 's', repeats: 0, fc: [{ t: 1990, dir: 'E', fmin: 3, fmax: 4, gale: 0, sea: 3, vis: 4, wx: '', f: 'b.txt', txt: 'E 3 OR 4.' }] }] })!;
const ic = navMetIcon(met.areas[0]);
yes('the met marker IS the arrow + "6–8 · ROUGH" + the name', ic.html.includes('rotate(45 ') && ic.html.includes('6–8 · ROUGH') && ic.html.includes('THAMES'));
yes('the marker is centred on the arrow (name and label the same height)', ic.h === windBox(met.areas[0].fc[0].wind!) + 28);
yes('an older server (no wind): the pill as before', !navMetIcon(met.areas[1]).html.includes('<svg') && navMetIcon(met.areas[1]).html.includes('DOVER'));
yes('the card says what the arrows show', navMetCard(met.areas[0], 2000).includes('from the south-west, force 8'));
yes('…and the screen reader', navMetAria(met.areas[0]).includes('wind from the south-west'));

console.log(`\n${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
