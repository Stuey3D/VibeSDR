// test_live_maps.ts — V12 beta96 THE TWO LIVE MAPS' rules (src/services/liveMaps.ts, docs/v12/ONE-MAP.md): the live layer
// from the mode, the chrome COMPUTED from the current mode (Mac Ha's stale-title bug cannot happen), layers present /
// chips / groups (no dead controls), tabs and their defaults, NEAREST | LATEST with emergency + military pinned, the
// MESSAGES feed and its filter, the fade, the basemap profile, the old toggles carried over.
//   npx tsx scripts/test_live_maps.ts
import { liveOf, liveChrome, livePresent, liveChips, liveLayerOn, liveLayersOf, liveTabs, liveTabPick, openerOf, aircraftOrder,
         msgFeedUpdate, msgFilter, fadeOf, liveProfile, avLayersFrom, shipLayersFrom, avLayerOf, shipLayerOf, MAP_LAYERS, LIVE_LAYERS,
         MSG_CAP, type LiveMsg } from '../src/services/liveMaps';

let passes = 0, fails = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  ok ? passes++ : fails++;
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}${ok ? '' : `\n     got  ${JSON.stringify(got)}\n     want ${JSON.stringify(want)}`}`);
};
const yes = (what: string, c: boolean) => eq(what, c, true);

console.log('§1 the live layer from the mode');
eq('ADS-B → air/sea, ADS-B, AIRCRAFT, NEAREST', [liveOf('adsb').map, liveOf('adsb').layers, liveOf('adsb').tab, liveOf('adsb').sort], ['airsea', ['adsb'], 'aircraft', 'nearest']);
eq('DATALINK → MESSAGES, LATEST', [liveOf('datalink').tab, liveOf('datalink').sort], ['messages', 'latest']);
eq('AIS → SHIPS', [liveOf('ais').layers, liveOf('ais').tab], [['ships'], 'ships']);
eq('sondes sit on the air/sea map (Stuart 11-26)', liveOf('sondes').map, 'airsea');
eq('APRS and the mesh on the amateur map', [liveOf('aprs').map, liveOf('ism').map, liveOf('ism').layers], ['amateur', 'amateur', ['mesh']]);
eq('a mode on the OTHER map: nothing live here', liveOf('aprs', 'airsea').layers, []);
eq('no mode / an unknown one: nothing live', [liveOf('').layers, liveOf('usb').layers, liveOf(null).map], [[], [], null]);
yes('every live layer belongs to its own map', (['adsb', 'datalink', 'hfdl', 'ndb', 'vormap', 'ais', 'dsc', 'navtex', 'sondes', 'spots', 'aprs', 'ism', 'ncdxf'] as const)
  .every((m) => liveOf(m).layers.every((l) => MAP_LAYERS[liveOf(m).map!].includes(l))));
yes('every layer has words', LIVE_LAYERS.every((l) => l.words.length > 10 && /^[A-Z0-9 &/-]+$/.test(l.label)));

console.log('§2 the chrome follows the CURRENT mode (never the mode the map was made for)');
eq('AIS: title + EXIT AIS (the map is the mode)', liveChrome('airsea', 'ais'), { title: 'AVIATION & MARITIME · AIS 161.975 + 162.025 MHz', exitLabel: 'EXIT AIS', exitLeavesMode: true, live: liveOf('ais') });
// ★ Mac Ha 11-26: AIS map → exit → ADS-B. The same map, recomputed: ADS-B's title and exit, no SHIPPING anywhere
const after = liveChrome('airsea', 'adsb');
eq('…then ADS-B: its title and EXIT ADS-B', [after.title, after.exitLabel], ['AVIATION & MARITIME · ADS-B 1090 MHz', 'EXIT ADS-B']);
yes('…and nothing of AIS left in it', !/AIS|SHIPPING/.test(JSON.stringify([after.title, after.exitLabel, after.live.layers])));
eq('mode left (USB): the plain title, CLOSE MAP', [liveChrome('airsea', 'usb').title, liveChrome('airsea', 'usb').exitLabel], ['AVIATION & MARITIME', 'CLOSE MAP']);
eq('a box map (NAVTEX): CLOSE MAP, the mode carries on', [liveChrome('airsea', 'navtex').exitLabel, liveChrome('airsea', 'navtex').exitLeavesMode], ['CLOSE MAP', false]);
eq('THE ONLY MODE: CLOSE MAP even for AIS', liveChrome('airsea', 'ais', true).exitLabel, 'CLOSE MAP');
eq('amateur map while in ADS-B: not live, CLOSE MAP', [liveChrome('amateur', 'adsb').title, liveChrome('amateur', 'adsb').exitLabel], ['AMATEUR MAP', 'CLOSE MAP']);
eq('amateur map in APRS', liveChrome('amateur', 'aprs').title, 'AMATEUR MAP · APRS');

console.log('§3 layers present (no dead controls), chips, groups');
const have = { adsb: 12, vdl2: 3, ships: 40, navst: 5, warnings: 0, weather: 9 };
eq('ADS-B live first, then the map order; empty layers absent', livePresent('airsea', have, liveOf('adsb'), ['airports']), ['adsb', 'vdl2', 'airports', 'ships', 'navst', 'weather']);
eq('the live layer is present with nothing in it yet', livePresent('airsea', {}, liveOf('ais')), ['ships']);
eq('ADS-C only when it has aircraft (DATALINK)', livePresent('airsea', { vdl2: 1 }, liveOf('datalink')), ['vdl2', 'acars']);
eq('opened on DSC (not live): DSC next, present though empty', livePresent('airsea', { adsb: 2 }, liveOf(''), [], ['dsc']), ['dsc', 'adsb']);
eq('amateur layers never on air/sea', livePresent('airsea', { aprs: 9, mesh: 2 } as any, liveOf('')), []);
const chips = liveChips(livePresent('airsea', have, liveOf('ais'), ['airports']), liveOf('ais'));
eq('chips: SHIPS first + LIVE, groups named once each', chips.map((c) => [c.label, c.group, c.live]),
   [['SHIPS', 'MARITIME', true], ['ADS-B', 'AVIATION', false], ['VDL2', '', false], ['AIRPORTS', '', false], ['NAVTEX', 'MARITIME', false], ['WEATHER', '', false]]);
eq('one group drawn: no group names', liveChips(['adsb', 'vdl2'], liveOf('adsb')).map((c) => c.group), ['', '']);
eq('one layer alone: no chip row', liveChips(['ships'], liveOf('ais')), []);
yes('the live layer\'s chip says so', /live now/.test(chips[0].words));
eq('a layer switched off is off …', liveLayerOn('vdl2', ['adsb', 'vdl2'], { vdl2: false }, liveOf('adsb')), false);
eq('… but the live one cannot be', liveLayerOn('adsb', ['adsb', 'vdl2'], { adsb: false }, liveOf('adsb')), true);
eq('absent = off', liveLayerOn('dsc', ['adsb'], {}, liveOf('adsb')), false);
eq('the composers\' toggles: aviation', avLayersFrom({ hfdlst: false }, ['adsb', 'hfdlst', 'airports'], liveOf('adsb')),
   { adsb: true, acars: true, vdl2: true, hfdl: true, adsc: true, stations: false, airports: true });
eq('the composers\' toggles: shipping', shipLayersFrom({ weather: false }, ['ships', 'weather', 'navst'], liveOf('ais')),
   { ships: true, dsc: true, stations: true, warnings: true, weather: false, areas: true });
eq('the id maps both ways', [avLayerOf('hfdlst'), shipLayerOf('navst'), avLayerOf('ships'), shipLayerOf('adsb')], ['stations', 'stations', null, null]);
eq('old toggles carried over (aviation stations ≠ NAVTEX stations)',
   liveLayersOf(null, { json: '{"stations":false,"adsb":true}', map: { stations: 'hfdlst', adsb: 'adsb' } }, { json: '{"stations":true,"weather":false}', map: { stations: 'navst', weather: 'weather' } }),
   { hfdlst: false, adsb: true, navst: true, weather: false });
eq('our own saved toggles win, junk ignored', liveLayersOf('{"hfdlst":true,"bogus":false,"ships":"yes"}', { json: '{"stations":false}', map: { stations: 'hfdlst' } }), { hfdlst: true });

console.log('§4 the basemap');
const all = (_: string) => true;
eq('aviation only: aero', liveProfile('airsea', ['adsb', 'airports'], all), 'aero');
eq('with a maritime layer drawn: airsea (airports AND ports)', liveProfile('airsea', ['adsb', 'airports', 'ships'], all), 'airsea');
eq('AIRPORTS off: airsea-plain', liveProfile('airsea', ['ships', 'airports'], (id) => id !== 'airports'), 'airsea-plain');
eq('maritime layers all off: aero again', liveProfile('airsea', ['adsb', 'airports', 'ships'], (id) => id !== 'ships'), 'aero');
eq('amateur: spots', liveProfile('amateur', ['spots'], all), 'spots');

console.log('§5 tabs');
const tabs = liveTabs('airsea', { aircraft: 10, ships: 4, messages: 0 }, liveOf('adsb'));
eq('tabs with rows + the live one, in order', tabs, ['aircraft', 'ships']);
eq('DATALINK: MESSAGES drawn though empty (live)', liveTabs('airsea', { aircraft: 3 }, liveOf('datalink')), ['aircraft', 'messages']);
eq('opened on a tab: it is drawn', liveTabs('airsea', {}, liveOf(''), 'navtex'), ['navtex']);
eq('nothing picked: the live tab', liveTabPick(['aircraft', 'ships'], null, liveOf('ais')), 'ships');
eq('a mode change moves to the new live tab', liveTabPick(['aircraft', 'ships'], null, liveOf('adsb')), 'aircraft');
eq('…unless the viewer picked one', liveTabPick(['aircraft', 'ships'], 'ships', liveOf('adsb')), 'ships');
eq('a picked tab that vanished: the live one', liveTabPick(['aircraft'], 'ships', liveOf('adsb')), 'aircraft');
eq('no live mode: the tab opened on', liveTabPick(['aircraft', 'ships'], null, liveOf(''), 'ships'), 'ships');
eq('openers keep their words', [openerOf('adsb'), openerOf('datalink').tab, openerOf('navtex').tab, openerOf('aprs').map, openerOf('mesh').tab],
   [{ map: 'airsea', tab: 'aircraft', sort: 'nearest' }, 'messages', 'navtex', 'amateur', 'mesh']);

console.log('§6 the AIRCRAFT list order');
const rows = [
  { key: 'far', nm: 90, age: 1 }, { key: 'near', nm: 4, age: 50 }, { key: 'nopos', nm: null, age: 0 },
  { key: 'mil', nm: 200, age: 30, mil: true }, { key: 'sos', nm: 300, age: 100, em: true }, { key: 'mid', nm: 40, age: 5 },
];
eq('NEAREST: emergency, military, then nearest, no position last', aircraftOrder(rows, 'nearest').map((r) => r.key), ['sos', 'mil', 'near', 'mid', 'far', 'nopos']);
eq('LATEST: emergency, military, then newest', aircraftOrder(rows, 'latest').map((r) => r.key), ['sos', 'mil', 'nopos', 'far', 'mid', 'near']);
eq('the input untouched', rows[0].key, 'far');
eq('stable for equal rows', aircraftOrder([{ key: 'a', age: 1 }, { key: 'b', age: 1 }], 'latest').map((r) => r.key), ['a', 'b']);

console.log('§7 the MESSAGES feed');
const seen = new Map<string, number>();
let feed: LiveMsg[] = [];
const t0 = 1_800_000_000_000;
feed = msgFeedUpdate(feed, seen, [{ key: 'A', who: 'RYR1', src: 'VDL2', n: 3, age: 10, label: 'H1', labelName: 'crew' }, { key: 'B', who: 'BAW2', src: 'ACARS', n: 1, age: 2, label: '_d' },
                                  { key: 'C', who: 'X', src: 'ADS-B', n: 9, age: 0 }], t0);
eq('first sight: each aircraft\'s last message, newest first; no label = no message', feed.map((m) => [m.key, m.t]), [['B', 1_799_999_998], ['A', 1_799_999_990]]);
const same = msgFeedUpdate(feed, seen, [{ key: 'A', who: 'RYR1', src: 'VDL2', n: 3, age: 12, label: 'H1' }], t0 + 2000);
yes('the same count again: nothing new (same array)', same === feed);
feed = msgFeedUpdate(feed, seen, [{ key: 'A', who: 'RYR1', src: 'VDL2', n: 4, age: 0, label: 'Q0', labelName: 'link test' }], t0 + 5000);
eq('the count grew: a new message on top', [feed[0].id, feed[0].label, feed.length], ['A#4', 'Q0', 3]);
eq('tap an aircraft: its messages only', msgFilter(feed, 'A').map((m) => m.id), ['A#4', 'A#3']);
eq('no filter: all', msgFilter(feed, '').length, 3);
eq('⚖ text only where sent', [feed.some((m) => 'text' in m), msgFeedUpdate([], new Map(), [{ key: 'Z', who: 'Z', src: 'VDL2', n: 1, age: 0, label: 'H1', text: 'owner' }], t0)[0].text], [false, 'owner']);
let big: LiveMsg[] = []; const s2 = new Map<string, number>();
for (let i = 0; i < 500; i++) big = msgFeedUpdate(big, s2, [{ key: 'K' + (i % 7), who: 'K', src: 'VDL2', n: i, age: 0, label: 'H1' }], t0 + i * 1000);
eq('bounded', big.length, MSG_CAP);

console.log('§8 the fade');
eq('live: full', fadeOf(99999, true), 1);
eq('fresh: full', fadeOf(300, false), 1);
eq('old: 0.4', fadeOf(7 * 3600, false), 0.4);
yes('in between: between', fadeOf(3 * 3600, false) > 0.4 && fadeOf(3 * 3600, false) < 1);

console.log(`\n${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
