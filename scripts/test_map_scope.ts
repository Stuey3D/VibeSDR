// test_map_scope.ts — beta96 the maps' scope (src/services/mapScope.ts): the OTHER RADIOS ON THIS SERVER switch and THIS
// SESSION, and the sanitisers that carry each entry's radios / first-heard time from the server's answers (aircraft.json,
// ais-ships.json, navtex-stations.json, the gallery's DSC alerts, the remembered sondes / VOR cards, the ISM node cards).
//   npx tsx scripts/test_map_scope.ts
import {
  otherRadiosOffered, sessionOffered, scopeRadioOk, scopeSessionOk, scopeOk, scopeList, FirstSeen, sessionStart, sessionWindow,
  withSession, radiosSeen, otherRadiosOf, OTHER_RADIOS_LABEL, SESSION_LABEL, SESSION_ID, SCOPE_ALL, type MapScope,
} from '../src/services/mapScope';
import { sharedSanitise } from '../src/services/aviationMap';
import { storeSanitise, dscPinsFromGallery } from '../src/services/shippingMap';
import { navSanitise } from '../src/services/navtexMap';
import { storedMerge, storedLoader } from '../src/services/storedLayers';
import { ismGalMerge } from '../src/services/ism';
import { amMeshLoader } from '../src/utils/amateurMap';

let pass = 0, fail = 0;
const ok = (c: unknown, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

(async () => {
  console.log('── the switch: when it is drawn ──');
  ok(!otherRadiosOffered({ mine: 'aaaa0001', radioCount: 1, viewer: false }), 'one radio, nothing from anyone else: no switch');
  ok(otherRadiosOffered({ mine: 'aaaa0001', radioCount: 2, viewer: false }), 'two radios on the machine: the switch');
  ok(!otherRadiosOffered({ mine: 'aaaa0001', radioCount: 3, viewer: true }), 'never on the standalone viewer');
  ok(!otherRadiosOffered({ mine: '', radioCount: 3, viewer: false }), 'no id here (the app\'s own radio, an older server): no switch');
  ok(otherRadiosOffered({ mine: 'aaaa0001', radioCount: 1, viewer: false, seen: ['aaaa0001', 'feed'] }), 'one radio + a feed\'s entries: the switch');
  ok(!otherRadiosOffered({ mine: 'aaaa0001', radioCount: 1, viewer: false, seen: ['aaaa0001'] }), 'only our own tag seen: no switch');
  ok(otherRadiosOf(null) && otherRadiosOf('1') && !otherRadiosOf('0'), 'remembered: on by default, "0" = off');
  ok(OTHER_RADIOS_LABEL === 'OTHER RADIOS ON THIS SERVER' && SESSION_LABEL === 'THIS SESSION', 'the words Stuart used');
  ok(sessionOffered({ viewer: false, onRadio: true }) && !sessionOffered({ viewer: true, onRadio: true }) && !sessionOffered({ viewer: false, onRadio: false }),
     'THIS SESSION: a map on a radio only, never the viewer');

  console.log('── what passes ──');
  const off: MapScope = { mine: 'aaaa0001', others: false, t0: 0 };
  ok(scopeRadioOk(['aaaa0001'], off) && scopeRadioOk(['bbbb0002', 'aaaa0001'], off), 'OFF: ours, and one we heard with another radio, pass');
  ok(!scopeRadioOk(['bbbb0002'], off) && !scopeRadioOk(['feed'], off), 'OFF: another radio\'s, and a feed\'s, are hidden');
  ok(scopeRadioOk(undefined, off) && scopeRadioOk([], off), 'OFF: an untagged (unknown) entry is shown — the logs\' rule');
  ok(scopeRadioOk(['bbbb0002'], { ...off, others: true }), 'ON: everything');
  const sess: MapScope = { mine: 'aaaa0001', others: true, t0: 1000 };
  ok(scopeSessionOk(1000, sess) && scopeSessionOk(999, sess) && !scopeSessionOk(990, sess), 'THIS SESSION: first heard since the start (2 s slack)');
  ok(!scopeSessionOk(null, sess) && scopeSessionOk(null, SCOPE_ALL), 'a first-heard time unknown: not in the session; no session: everything');
  ok(!scopeOk(['bbbb0002'], 1200, { ...sess, others: false }) && scopeOk(['aaaa0001'], 1200, { ...sess, others: false }), 'both rules at once');
  const L = [{ r: ['aaaa0001'], f: 1500 }, { r: ['bbbb0002'], f: 1500 }, { r: undefined, f: 10 }];
  ok(scopeList(L, SCOPE_ALL, (x) => x.r, (x) => x.f) === L, 'nothing filtering: the same list back');
  ok(scopeList(L, { mine: 'aaaa0001', others: false, t0: 1000 }, (x) => x.r, (x) => x.f).length === 1, 'OFF + session: one left');

  console.log('── the session clock ──');
  const fs = new FirstSeen(3);
  ok(fs.first('a', 100) === 100 && fs.first('a', 150) === 100, 'first seen by this viewer, remembered');
  ok(fs.first('b', 200, 50) === 50, 'the server\'s time wins');
  fs.first('c', 1); fs.first('d', 2); fs.first('e', 3);
  ok(fs.size <= 3, `bounded (${fs.size})`);
  let st = sessionStart(null, 'adsb', 100, true);
  ok(st.t0 === 100, 'opened: the start');
  st = sessionStart(st, 'adsb', 200, false);
  ok(st.t0 === 100, 'the same mode: kept');
  st = sessionStart(st, 'ais', 300, false);
  ok(st.t0 === 300 && st.mode === 'ais', 'another mode: re-entry starts again');
  st = sessionStart(st, 'ais', 400, true);
  ok(st.t0 === 400, 'the map re-opened: starts again');
  ok(sessionWindow(1000, 1000 + 600) === '1h' && sessionWindow(0, 5 * 3600) === '24h' && sessionWindow(0, 3 * 86400) === '7d' && sessionWindow(0, 400 * 86400) === 'all', 'the polls\' window covers the session');
  const R = [{ id: '1h', label: '1 H', words: 'x' }];
  ok(withSession(R, false) === R, 'not offered: the window unchanged');
  ok(withSession(R, true).map((r) => r.id).join() === `1h,${SESSION_ID}`, 'beside the window');
  ok(withSession([], true).map((r) => r.id).join() === `all,${SESSION_ID}`, 'a map with no window: ALL | THIS SESSION');

  console.log('── the sanitisers carry the radios and first-heard ──');
  const av = sharedSanitise({ now: 2000, count: 3, aircraft: [
    { key: '111111', icao: '111111', src: ['ADS-B'], srcAge: { 'ADS-B': 1 }, age: 1, first: 30, n: 3, radios: ['aaaa0001'] },
    { key: '222222', icao: '222222', src: ['VDL2'], srcAge: { VDL2: 5 }, age: 5, first: 900, n: 1, radios: ['bbbb0002', 'BAD ID', 'feed'] },
    { key: '333333', icao: '333333', src: ['ADS-B'], srcAge: { 'ADS-B': 2 }, age: 2, n: 1 },
  ], stations: [] });
  ok(av?.aircraft[0].radios?.join() === 'aaaa0001' && av?.aircraft[0].first === 30, 'aircraft.json: radios + first');
  ok(av?.aircraft[1].radios?.join() === 'bbbb0002,feed', 'a bad id is dropped');
  ok(av?.aircraft[2].radios === undefined && av?.aircraft[2].first === undefined, 'an older server: neither (unknown)');
  ok(radiosSeen([av?.aircraft]).has('feed'), 'the ids seen (a feed counts as another source)');
  const ships = storeSanitise({ now: 2000, count: 2, total: 2, vessels: [
    { mmsi: 235000001, kind: 'A', seen: 10, age: 100, posAge: 10, count: 3, radios: ['aaaa0001'] },
    { mmsi: 235000002, kind: 'A', seen: 10, age: 100, posAge: 10, count: 3 },
  ] });
  ok(ships?.vessels.find((v) => v.mmsi === 235000001)?.radios?.join() === 'aaaa0001', 'ais-ships.json: a ship\'s radios');
  ok(ships?.vessels.find((v) => v.mmsi === 235000002)?.radios === undefined, 'an untagged ship: unknown');
  const nav = navSanitise({ now: 2000, stations: [{ b1: 'K', name: 'Niton', lat: 50.6, lon: -1.3, conf: 'confident', first: 1500, last: 1900, radios: ['bbbb0002'] }] });
  ok(nav?.stations[0].radios?.join() === 'bbbb0002' && nav?.stations[0].first === 1500, 'navtex-stations.json: radios + first');
  const pins = dscPinsFromGallery({ items: [{ k: 'dsc', plat: 50, plon: -1, t: 1990, f: 'a.txt', title: 'DISTRESS 235000001', ri: 'aaaa0001,bbbb0002' }, { k: 'dsc', plat: 50, plon: -1, t: 1991, f: 'b.txt', title: 'x' }] });
  ok(pins[0].radios?.join() === 'aaaa0001,bbbb0002' && pins[1].radios === undefined, 'a gallery DSC alert: its "ri"');
  const gp = (id: string, radios?: string[]) => ({ id, name: id, type: '', cc: '', lat: 50, lon: 0, alt: null, top: null, mhz: null, first: 1000, last: 1900, radios });
  const m1 = storedMerge([[gp('S1', ['aaaa0001'])], [gp('S1', ['bbbb0002'])], [gp('S2')]], 'sondes', 2000);
  ok(m1.find((x) => x.id === 'S1')?.radios?.sort().join() === 'aaaa0001,bbbb0002' && m1.find((x) => x.id === 'S2')?.radios === undefined, 'remembered sondes: the cards\' radios merged');
  const load = storedLoader('sondes', {
    list: async () => [{ s: 'sondes-2026-11-26', t: Math.floor(Date.UTC(2026, 10, 26) / 1000), f: 'c.txt', ri: 'aaaa0001' }],
    text: async () => null,
  });
  ok(Array.isArray(await load(Math.floor(Date.UTC(2026, 10, 26, 12) / 1000))), 'the loader takes the items\' "ri" (no card text here: an empty list)');
  const node = (k: string, radios?: string[]) => ({ key: k, kind: 'meshcore', node: k, name: k, first: 1000, last: 1900, origin: 'live', count: 1, radios } as any);
  const ism = ismGalMerge([[node('N1', ['aaaa0001'])], [node('N1', ['bbbb0002'])], [node('N2', ['aaaa0001']), node('N3')]], 2000);
  ok(ism.find((n) => n.node === 'N1')?.radios?.sort().join() === 'aaaa0001,bbbb0002', 'ISM node cards: the radios merged');
  ok(ism.find((n) => n.node === 'N3')?.radios === undefined, 'an untagged card: unknown');
  const now = Math.floor(Date.UTC(2026, 10, 26, 12) / 1000);
  const ml = amMeshLoader<{ last: number; radios?: string[] }>({
    list: async () => [{ s: 'ismnodes-868-2026-11-26', t: Math.floor(Date.UTC(2026, 10, 26) / 1000), f: 'n.txt', ri: 'bbbb0002' }],
    card: async () => [{ last: now - 60 }],
    merge: (per) => per.flat(),
  });
  const mn = await ml('24h', now);
  ok(mn?.[0]?.radios?.join() === 'bbbb0002', `the AMATEUR map's MESH loader tags each node with its card's radios (${JSON.stringify(mn)})`);

  console.log(`test_map_scope: ${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
})();
