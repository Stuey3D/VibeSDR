/* test_airsea_standalone.ts — ★★★ V12 beta96 the app's STANDALONE AIR & SEA MAP composition (src/services/airSeaStandalone.ts,
 * used by src/screens/AirSeaScreen.tsx): no radio, no live mode — what the server shares over HTTP only.
 *   · the title is the map's, the exit CLOSE MAP; no chip is marked live (●) and nothing offers TUNE;
 *   · HFDL aircraft from a feed: the HFDL chip, the AIRCRAFT tab with their rows, opening on the tab asked for;
 *   · ships from the store: the SHIPS chip and tab, the maritime time windows; no ships → no time windows (aircraft keep the
 *     table's hour);
 *   · remembered sondes / VOR stations: their chips, tabs and items;
 *   · a chip switched off removes its items; the handlers are the screen's (toggle, tab, sort, MESSAGES filter);
 *   · the time limit's clock leads the status line.
 * Silent, no network. */
import { composeAirSeaStandalone, newMsgFeed, type AirSeaState } from '../src/services/airSeaStandalone';
import { sharedSanitise, avLayersOf } from '../src/services/aviationMap';
import { storeSanitise, shipLayersOf } from '../src/services/shippingMap';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const now = 1790000000;
const shared = sharedSanitise({ now, count: 2, aircraft: [
  { key: 'AB12CD', icao: 'AB12CD', flight: 'BAW123', lat: 51, lon: -20, posAge: 60, posSrc: 'HFDL', src: ['HFDL'], srcAge: { HFDL: 60 }, age: 60, n: 3, label: 'H1' },
  { key: 'AB12CE', icao: 'AB12CE', flight: 'DLH400', lat: 48, lon: -30, posAge: 200, posSrc: 'HFDL', src: ['HFDL'], srcAge: { HFDL: 200 }, age: 200, n: 2 },
], stations: [], feeds: [{ title: 'UberSDR HFDL', label: 'HFDL', state: 'connected', aircraft: 2 }] });
const ships = storeSanitise({ now, count: 1, total: 1, vessels: [{ mmsi: 235000001, name: 'TEST SHIP', lat: 50.8, lon: -1.2, seen: 120, sog: 10, cog: 90 }] });
const base = (o: Partial<AirSeaState> = {}): AirSeaState => ({
  shared, shipStore: null, dscGal: [], navReport: null, navWarnRep: null, navMetRep: null, navRange: 'all', sdStored: [], vmStored: [],
  avLayers: avLayersOf(null), shipLayers: shipLayersOf(null), rx: { lat: 51.5, lon: -0.1 }, view: null, picked: null, opened: null,
  sort: 'nearest', msgKey: '', clock: '', nowMs: now * 1000, ...o,
});
const calls: string[] = [];
const h = {
  setPicked: (t: string) => calls.push(`tab:${t}`), setSort: (s: string) => calls.push(`sort:${s}`), setMsgKey: (k: string) => calls.push(`msg:${k}`),
  setAvLayers: (f: any) => calls.push(`av:${JSON.stringify(f(avLayersOf(null)))}`), setShipLayers: (f: any) => calls.push('ship'),
  setNavRange: (r: string) => calls.push(`range:${r}`), onView: () => {}, cardText: async () => ({ ok: true, text: '' }),
};

console.log('── the standalone AIR & SEA map, composed ──');
{
  const t = composeAirSeaStandalone(base(), h as any, newMsgFeed());
  ok(t.exitLabel === 'CLOSE MAP' && /AVIATION/.test(t.title), `CLOSE MAP, the map's own title ("${t.title}")`);
  ok(t.toggles.every((c) => !c.label.startsWith('●')), 'no chip is marked live (no radio, no mode)');
  ok(t.toggles.some((c) => c.id === 'hfdl') && !t.toggles.some((c) => c.id === 'ships' || c.id === 'adsb'), `HFDL from a feed: the HFDL chip, no SHIPS or ADS-B chip (${t.toggles.map((c) => c.id).join(',')})`);
  ok(t.toggled.areas === false, 'aviation only: SEA AREAS starts off (nothing maritime here)');
  ok(t.tab === 'aircraft' && t.rows.length === 2 && t.rows.some((r) => r.name === 'BAW123'), 'the AIRCRAFT tab lists both feed aircraft');
  ok(t.items.filter((i) => /^a:[0-9A-F]{6}$/.test(String(i.key))).length === 2, 'both aircraft on the map');
  ok(t.ranges === undefined, 'no maritime layer: no time windows (the aircraft keep the table\'s hour)');
  ok(!('onTune' in t) && !JSON.stringify(Object.keys(t)).includes('tune'), 'nothing offers TUNE');
  t.onToggle('hfdl', false);
  ok(calls.some((c) => c.startsWith('av:') && c.includes('"hfdl":false')), 'a chip switched off goes to the screen\'s layers');
  const off = composeAirSeaStandalone(base({ avLayers: { ...avLayersOf(null), hfdl: false } }), h as any, newMsgFeed());
  ok(off.items.filter((i) => /^a:[0-9A-F]{6}$/.test(String(i.key))).length === 0, 'HFDL off: its aircraft leave the map');
}
{
  const t = composeAirSeaStandalone(base({ shipStore: ships, opened: 'ships' }), h as any, newMsgFeed());
  ok(t.toggles.some((c) => c.id === 'ships') && t.tab === 'ships' && t.rows.some((r) => /TEST SHIP/.test(r.name)), 'ships from the store: the SHIPS chip, opening on SHIPS with the ship');
  ok(Array.isArray(t.ranges) && t.ranges.length > 0, 'a maritime layer: the normal time windows');
  t.onRange?.('24h');
  ok(calls.includes('range:24h'), 'a time window chosen goes to the screen');
  ok((t.tabs ?? []).some((x) => x.id === 'aircraft') && (t.tabs ?? []).some((x) => x.id === 'ships'), 'both tabs offered');
}
{
  const sd = [{ id: 'S1234567', name: 'S1234567', type: 'RS41', cc: '', lat: 52, lon: -1, alt: 20000, top: 30000, mhz: 403, first: now - 7200, last: now - 3600 }];
  const t = composeAirSeaStandalone(base({ shared: null, sdStored: sd, opened: 'sondes' }), h as any, newMsgFeed());
  ok(t.toggles.some((c) => c.id === 'sondes') && t.tab === 'sondes' && t.rows.length === 1, 'remembered sondes alone: the chip, opening on SONDES');
  ok(t.items.some((i) => String(i.key).startsWith('sds:')), 'the sonde on the map (faded by age)');
}
{
  const t = composeAirSeaStandalone(base({ clock: 'Map time ends in 12:34' }), h as any, newMsgFeed());
  ok(t.status.startsWith('Map time ends in 12:34'), `the time limit's clock leads the status line ("${t.status.slice(0, 60)}")`);
  t.onSort?.('latest'); t.onTab?.('messages');
  ok(calls.includes('sort:latest') && calls.includes('tab:messages'), 'sort and tabs go to the screen');
  ok(t.onPick?.('a:AB12CD') === 'a:AB12CD' && calls.includes('msg:a:AB12CD'), 'an aircraft chosen filters MESSAGES to it');
}
console.log(`\ntest_airsea_standalone: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
