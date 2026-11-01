// test_navtex_map.ts — V12 the NAVTEX stations map's client rule (src/services/navtexMap.ts — the web client and the app
// both draw from it): the sanitiser, co-sited letters as one site, the why list, the schedule slot, the app's pins.
//   npx tsx scripts/test_navtex_map.ts
import { navSanitise, navSites, navWhy, navSlot, navLabel, navCard, navItems, navStatus, navRangeOf, navSince } from '../src/services/navtexMap';

let passes = 0, fails = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  ok ? passes++ : fails++;
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}${ok ? '' : `\n     got  ${JSON.stringify(got)}\n     want ${JSON.stringify(want)}`}`);
};
const st = (b1: string, name: string, lat: number, lon: number, khz: number, n: number, extra: Record<string, unknown> = {}) => ({
  b1, name, iso: 'GB', country: 'United Kingdom', lat, lon, conf: 'confident', km: 195, khz: [khz], first: 1000, last: 2000, n, uniq: Math.ceil(n / 2),
  night: 1, folded: 0, subj: { A: n }, recent: [{ t: 2000, id: `${b1}A01`, f: 'x.txt' }], outright: n, inSlot: n - 1, share: 2,
  near: [{ name, iso: 'GB', km: 195 }, { name: 'Far', iso: 'XX', km: 2400 }], ...extra });
const r = navSanitise({ now: 2600, rx: { lat: 52.3, lon: -0.85 }, messages: 30, unplaced: 2, oldest: 1000, stations: [
  st('E', 'Niton', 50.583, -1.3, 518, 10), st('K', 'Niton', 50.583, -1.3, 518, 4), st('I', 'Niton', 50.583, -1.3, 490, 6),
  st('P', 'Kerkyra', 39.6, 19.9, 490, 1), st('K', 'Kerkyra', 39.62, 19.92, 518, 1),
  st('E', 'Corsen', 48.4, -4.8, 490, 1, { conf: 'ambiguous', cands: [{ name: 'Corsen', iso: 'FR', lat: 48.4, lon: -4.8, km: 520 }, { name: 'Valentia', iso: 'IE', lat: 51.9, lon: -10.3, km: 648 }] }),
  { b1: 'é', name: 'bad', lat: 999, lon: 0 }, null,
] })!;
eq('sanitise: the broken ones dropped', r.stations.length, 6);
const sites = navSites(r);
eq('co-sited letters are one site: Niton E K I, Kerkyra P K (within 50 km), Corsen? on its own', sites.map((g) => `${g.name}:${g.letters.map((x) => x.b1).join('')}`),
   ['Niton:EIK', 'Kerkyra:PK', 'Corsen:E']);
eq('…a site sums its letters and keeps both bands', [sites[0].n, sites[0].khz], [20, [518, 490]]);
eq('the label: "Niton · E I K · 195 km"', navLabel(sites[0]), 'Niton · E I K · 195 km');
eq('…a "?" site says so', navLabel(sites[2]).startsWith('Corsen? · E'), true);
eq('the schedule slot: A 00:00, E 00:40, M 02:00, X 03:50', ['A', 'E', 'M', 'X'].map(navSlot), ['00:00', '00:40', '02:00', '03:50']);
const why = navWhy(r.stations[0]);
eq('the why: named, the letter shared, the slot, the night', [/named in 100 %/.test(why[0]), /2 stations use letter E on 518 kHz — nearest Niton 195 km, then Far 2,400 km/.test(why[1]),
   /90 % ended inside E’s schedule slot \(00:40 UTC/.test(why[2]), /10 % heard with the path in darkness/.test(why[3])], [true, true, true, true]);
eq('the why of a "?": the candidates by distance', navWhy(r.stations[5]).some((w) => /could be Corsen \d+ % or Valentia \d+ %/.test(w)), true);
const card = navCard(sites[0]);
eq('the card lists each letter with its band and counts', (card.match(/<b>[A-Z]<\/b> · \d+ kHz · \d+ received, \d+ different/g) || []).length, 3);
eq('the card escapes what came off the air', /<script/.test(navCard({ ...sites[0], name: '<script>' })), false);
const items = navItems(r);
eq('the app\'s pins: one a site, with a line from the receiver', [items.length, items.every((i) => i._arc === true)], [3, true]);
eq('the status', navStatus(r), '3 sites · 6 stations · 30 messages · furthest Niton 195 km · 2 not placed');
eq('ranges: ALL by default; since', [navRangeOf(null), navRangeOf('7d'), navSince('24h'), navSince('all')], ['all', '7d', 86400, 0]);
console.log(`navtex map: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
