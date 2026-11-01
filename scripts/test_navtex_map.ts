// test_navtex_map.ts — V12 the NAVTEX stations map's client rule (src/services/navtexMap.ts — the web client and the app
// both draw from it): the sanitiser, co-sited letters as one site, the why list, the schedule slot, the app's pins.
//   npx tsx scripts/test_navtex_map.ts
import { navSanitise, navSites, navWhy, navName, navSlot, navLabel, navCard, navItems, navStatus, navRangeOf, navSince } from '../src/services/navtexMap';
import { navWarnSanitise, navWarnSummary, navWarnLabel, navWarnVisible, navWarnItems, navWarnCard, navWarnWhy, navWarnPoint, navBuilding, navMetListed, navMetRowWords,
         navMetSanitise, navFcLabel, navMetVisible, navMetItems, navMetCard, navLayersOf } from '../src/services/navtexWarn';
import { NAV_GLYPHS, NAV_HAZARDS, navGlyphSvg, navHazardMarker } from '../src/services/navtexIcons';

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
const why = navWhy(r.stations[0]).join(' | ');
eq('the why, in plain words and counts — never a score', [/Named by the message itself or its station label: 10 of 10 messages/.test(why),
   /Ended inside E’s schedule slot \(00:40 UTC, every 4 h\): 9 of 10/.test(why), /Nearest E station on 518 kHz: Niton, 195 km \(next Far, 2,400 km\) — 2 stations share this letter/.test(why),
   /Heard with the path in darkness: 1 of 10/.test(why), !/%/.test(why)], [true, true, true, true, true]);
eq('the why of a "?": its candidates', navWhy(r.stations[5]).some((w) => /Could be Corsen \(520 km\) or Valentia \(648 km\)/.test(w)), true);
eq('labels: plain / Suspected / ?', [navName('Niton', 'confident'), navName('Tallinn', 'likely'), navName('Corsen', 'ambiguous')], ['Niton', 'Suspected Tallinn', 'Corsen?']);
eq('a run-settled station says so', navWhy({ ...r.stations[0], ev: { run: 3, series: 1 } }).slice(1, 3), ['Its warning service is named in the text (e.g. AVURNAV BREST, ESTONIAN NAV WARN): 1 of 10 messages', 'Part of the same broadcast run as a message naming it: 3 of 10 messages']);
const card = navCard(sites[0]);
eq('the card lists each letter with its band and counts', (card.match(/<b>[A-Z]<\/b> · \d+ kHz · \d+ received, \d+ different/g) || []).length, 3);
eq('the card escapes what came off the air', /<script/.test(navCard({ ...sites[0], name: '<script>' })), false);
const items = navItems(r);
eq('the app\'s pins: one a site, with a line from the receiver', [items.length, items.every((i) => i._arc === true)], [3, true]);
eq('the status', navStatus(r), '3 sites · 6 stations · 30 messages · furthest Niton 195 km · 2 not placed');
eq('ranges: ALL by default; since', [navRangeOf(null), navRangeOf('7d'), navSince('24h'), navSince('all')], ['all', '7d', 86400, 0]);
// ── the warnings (navtexWarn.ts) ──
const wr = navWarnSanitise({ now: 1793560000, total: 3, shown: 3, live: 4515, indexed: 1200, pending: 3315, warnings: [
  { id: 'EA45', khz: 518, first: 1793400000, last: 1793550000, n: 7, lost: 0, unlit: 1, expired: 0, cls: 'navaid', f: 'a.txt', hit: 'FEU ETEINT', lang: 'fr',
    pts: [[50.1, -1.2]], st: 'Niton', sc: 'likely', iso: 'GB', q: '12 repaired', issued: 1793372400 },
  { id: 'EA46', khz: 518, first: 1793400000, last: 1793410000, n: 2, lost: 3, unlit: 0, expired: 1, cancelled: 1, cancelledBy: 'EA47', cls: 'firing', f: 'b.txt',
    shape: 'area', pts: [[50, 1], [50, 2], [51, 2], [51, 1]] },
  { id: 'EA48', cls: 'cable', shape: 'route', pts: [[50, 1], [50.5, 1.5], [51, 2]], first: 1, last: 2, f: 'c.txt' },
  { id: 'bad', pts: [[1, 1]] }, { id: 'XX01', pts: [[999, 0]] }, { id: '<b>', pts: [] },
] });
eq('warnings: the clean ones kept, a bad id or position dropped', wr?.list.map((w) => w.id), ['EA45', 'EA46', 'EA48']);
const [w1, w2, w3] = wr!.list;
eq('the summary: our words, the message\'s own word and language — never "translation"', navWarnSummary(w1), 'Unlit light (from French: FEU ETEINT)');
eq('…English wording: just ours', navWarnSummary({ cls: 'wreck', unlit: false, hit: '', lang: '' }), 'Wreck');
eq('the label tags a cancelled one', navWarnLabel(w2), 'EA46 · Firing or military exercise · cancelled');
eq('an area\'s marker at its centre, a route\'s at its middle point', [navWarnPoint(w2.pts, 'area'), [w3.lat, w3.lon]], [[50.5, 1.5], [50.5, 1.5]]);
// ★★ LATEST WINS (Stuart, 2026-11-01): a cancelled warning is drawn under NO filter, ALL included
eq('the time filter: a cancelled warning is not drawn under 24 H, nor under ALL', [navWarnVisible(wr, 'all').map((w) => w.id), navWarnVisible(wr, '24h').map((w) => w.id)], [['EA45', 'EA48'], ['EA45', 'EA48']]);
const wi = navWarnItems(wr!.list, wr!.now, 'web');
eq('items: clustered, a shape for the area and the route, faded grey when cancelled', [wi.every((i) => i._cl === 'w'), wi[1]._sh?.kind, wi[2]._sh?.kind, wi[1]._sh?.faded, /opacity:0.42/.test(wi[1]._ic.html)], [true, 'area', 'route', true, true]);
const wc = navWarnCard(w1, wr!.now);
eq('the card: id, issued, heard, times, station as the server says, quality, the text slot', [/EA45/.test(wc), /Issued/.test(wc), /7 times here/.test(wc), /Suspected Niton/.test(wc), /12 repaired/.test(wc), /class="nvTx raw" data-f="a.txt"/.test(wc), /class="nvTr" hidden/.test(wc)], [true, true, true, true, true, true, true]);
eq('the why: a language line only for the message in that language', [navWarnWhy(w1, wr!.now)[0], navWarnWhy(w3, wr!.now).some((l) => /French|Spanish|German/.test(l))], ['Its icon: this message says FEU ETEINT (French)', false]);
eq('the card escapes what came off the air', /<script/.test(navWarnCard({ ...w1, hit: '<script>', st: '<script>' }, 0)), false);
eq('building', navBuilding(wr), 'Building the map from 4,515 saved messages… 26 %');
eq('built: nothing said', navBuilding({ live: 10, indexed: 10, pending: 0 }), '');
// ── the met labels ──
const mr = navMetSanitise({ now: 1793560000, stale: 86400, areas: [
  { id: 'thames', name: 'Thames', lat: 52.2, lon: 2.3, kind: 's', repeats: 1, fc: [{ t: 1793550000, dir: 'SW', fmin: 6, fmax: 8, gale: 8, sea: 5, vis: 3, wx: 'rain', f: 'm.txt', txt: 'SW 6 TO GALE 8.', st: 'Niton', sc: 'confident' },
                                                                                 { t: 1793500000, dir: 'SW', fmin: 4, fmax: 6, gale: 0, sea: 3, vis: 4, wx: '', f: 'n.txt', txt: '' }] },
  { id: 'dover', name: 'Dover', lat: 51, lon: 1.3, kind: 's', fc: [{ t: 1793400000, dir: 'N', fmin: 3, fmax: 3, gale: 0, sea: 2, vis: 0, f: 'o.txt' }] },
  { id: 'BAD!', name: 'x', lat: 0, lon: 0, fc: [{ t: 1 }] },
] });
eq('met: the clean areas kept', mr?.areas.map((a) => a.id), ['thames', 'dover']);
eq('the label', navFcLabel(mr!.areas[0].fc[0]), 'SW 6–8 · GALES · ROUGH');
eq('…a single force, fog', navFcLabel(mr!.areas[1].fc[0]), 'N 3 · SMOOTH · FOG');
eq('…a gale word in another language: the flag only', navFcLabel({ ...mr!.areas[0].fc[0], gale: 8, native: true, fmin: 5, fmax: 6 }), 'SW 5–6 · GALE WORD · ROUGH');
eq('only the latest is drawn; over a day old = not drawn under any filter (ALL too), listed as "no current forecast"',
   [mr!.areas[1].stale, navMetVisible(mr, 'all').length, navMetVisible(mr, '7d').length, navMetListed(mr).length, navMetRowWords(mr!.areas[1])], [true, 1, 1, 2, 'no current forecast']);
// ★★ THAMES: last week's SEVERE GALE 9 and today's SW 4–5 → only SW 4–5, under every filter
{
  const now = 1793560000;
  const th = navMetSanitise({ now, stale: 86400, areas: [{ id: 'thames', name: 'Thames', lat: 52.2, lon: 2.3, kind: 's', fc: [
    { t: now - 3600, dir: 'SW', fmin: 4, fmax: 5, gale: 0, sea: 3, vis: 4, f: 'today.txt', txt: 'SOUTHWEST 4 OR 5.' },
    { t: now - 7 * 86400, dir: 'SW', fmin: 9, fmax: 9, gale: 9, sea: 6, vis: 3, f: 'lastweek.txt', txt: 'SOUTHWEST SEVERE GALE 9.' }] }] });
  for (const rng of ['1h', '24h', '7d', '30d', 'all']) {
    const items = navMetItems(navMetVisible(th, rng), now, 'web');
    eq(`Thames under ${rng}: only SW 4–5 is drawn (never last week's SEVERE GALE 9)`, [items.length, /SW 4–5/.test(items[0]?._ic.html ?? ''), /SEVERE/.test(items[0]?._ic.html ?? '')], [1, true, false]);
  }
  const old = navMetSanitise({ now, stale: 86400, areas: [{ id: 'thames', name: 'Thames', lat: 52.2, lon: 2.3, kind: 's', fc: [
    { t: now - 7 * 86400, dir: 'SW', fmin: 9, fmax: 9, gale: 9, sea: 6, vis: 3, f: 'lastweek.txt', txt: 'SOUTHWEST SEVERE GALE 9.' }] }] });
  eq('…and when last week’s gale is all there is: nothing drawn, not even under ALL', navMetItems(navMetVisible(old, 'all'), now).length, 0);
}
// ★★ v6: superseded and re-issued warnings; named lists; the new classes
{
  const now = 1793560000;
  const r = navWarnSanitise({ now, warnings: [
    { id: 'GA23', khz: 518, first: now - 7200, last: now - 7200, n: 1, cls: 'buoy', f: 'm1.txt', pts: [[53.8333, 1]], supersededBy: 'GA24', expired: 1 },
    { id: 'GA24', khz: 518, first: now - 3600, last: now - 3600, n: 1, cls: 'buoy', f: 'm2.txt', pts: [[53.8667, 1.0833]] },
    { id: 'PA11', khz: 518, first: now - 600, last: now - 600, n: 1, cls: 'platform', f: 'pa.txt', pts: [[54.1167, 4.0017], [53.6833, 3.7333], [53.4567, 4.2317]],
      items: [{ n: 'F16-A', d: 'NAV AIDS UNRELIABLE', u: 1 }, { n: 'K6-N', d: 'FOGHORN INOPERATIVE', u: 0 }, { n: 'L10-B', d: 'BLACK OUT', u: 1 }] },
    { id: 'GA30', khz: 518, first: now - 600, last: now - 600, n: 1, cls: 'pipeline_gas', f: 'g.txt', pts: [[53, 2]] },
    { id: 'GA31', khz: 518, first: now - 600, last: now - 600, n: 1, cls: 'submarine', f: 's.txt', pts: [[53, 3]] },
  ] });
  for (const rng of ['24h', 'all']) eq(`a re-issued warning with a moved position (${rng}): only the new position is drawn`,
    navWarnVisible(r, rng).filter((w) => w.id.startsWith('GA2')).map((w) => [w.id, w.lat]), [['GA24', 53.8667]]);
  const its = navWarnItems(navWarnVisible(r, 'all'), now, 'web').filter((x) => x.key.startsWith('w:PA11'));
  eq('PA11\'s three platforms: three markers, each its name and defect, the first keeps the warning\'s key',
     its.map((x) => [x.key, x._lb, x.lat]), [['w:PA11:' + (now - 600), 'F16-A · NAV AIDS UNRELIABLE', 54.1167], [`w:PA11:${now - 600}:1`, 'K6-N · FOGHORN INOPERATIVE', 53.6833], [`w:PA11:${now - 600}:2`, 'L10-B · BLACK OUT', 53.4567]]);
  eq('…the unlit mark per item (F16-A and L10-B, not K6-N); no shape across the North Sea', [its.map((x) => /M11 5l3.2-3.2/.test(x._ic.html)), its.every((x) => !x._sh)], [[true, false, true], true]);
  eq('…each card names its item and shares the warning', [/K6-N — FOGHORN INOPERATIVE/.test(its[1]._cd), /PA11/.test(its[1]._cd)], [true, true]);
  eq('the new classes keep their own words (gas pipeline, submarine operations)', navWarnVisible(r, 'all').filter((w) => w.id === 'GA30' || w.id === 'GA31').map((w) => navWarnSummary(w)), ['Gas pipeline', 'Submarine operations']);
  eq('the gas flame stays blue on a faded marker too', /fill="#3d9bff"/.test(navHazardMarker('pipeline_gas', false, true).html), true);
  eq('the platform carries the unlit mark', /M11 5l3.2-3.2/.test(navHazardMarker('platform', true, false).html), true);
  eq('the legend lists the five cables and pipelines and the platform', ['cable', 'cable_telecom', 'cable_power', 'pipeline', 'pipeline_gas', 'platform'].every((k) => NAV_HAZARDS.some((h) => h.id === k)), true);
}
// ★★ the tidied station card: essentials, then "More"
{
  const st = navSanitise({ now: 1793560000, rx: { lat: 52.3, lon: -1.5 }, stations: [{ b1: 'E', name: 'Niton', iso: 'GB', country: 'United Kingdom', lat: 50.58, lon: -1.25, conf: 'confident', km: 195, khz: [518],
    first: 1793000000, last: 1793559000, n: 40, uniq: 12, outright: 30, inSlot: 38, share: 6, subj: { A: 20, B: 10 }, ev: { series: 5 }, recent: [{ t: 1793559000, id: 'EA45', f: 'a.txt' }], near: [{ name: 'Niton', iso: 'GB', km: 195 }] }] });
  const card = navCard(navSites(st)[0], 'web');
  const [ess, more] = card.split('<details');
  eq('station card: the essentials before "More" (name, letters + band, distance, last heard, messages, top why)',
     [/Niton/.test(ess), /E · 518 kHz · 195 km/.test(ess), /Last heard 17 min ago · 40 messages \(12 different\)/.test(ess), (ess.match(/· Named|· Its warning|· Ended|· Nearest/g) || []).length <= 3], [true, true, true, true]);
  eq('…first heard, kinds and newest ids under "More"', [/<summary[^>]*>More<\/summary>/.test(more), /First heard/.test(more), /Newest: EA45/.test(more), /First heard/.test(ess)], [true, true, true, false]);
}
const mi = navMetItems(mr!.areas, mr!.now, 'web');
eq('the gale label in the warning colour', [/#5a1414/.test(mi[0]._ic.html), /#5a1414/.test(mi[1]._ic.html)], [true, false]);
eq('the met card: forecast, history, station, the text slot', [/Earlier forecasts/.test(navMetCard(mr!.areas[0], mr!.now)), /Niton/.test(navMetCard(mr!.areas[0], mr!.now)), /nvTx/.test(navMetCard(mr!.areas[0], mr!.now))], [true, true, true]);
eq('layers: all on by default, a saved choice kept', [navLayersOf(null), navLayersOf('{"weather":false}').weather, navLayersOf('junk').stations], [{ stations: true, warnings: true, weather: true }, false, true]);
// ── the icons ──
eq('an icon for every hazard on the sheet', NAV_HAZARDS.every((h) => NAV_GLYPHS[h.id].length > 20), true);
eq('the glyph is the health glyphs\' language (16 box, stroke 1.4, round)', /viewBox="0 0 16 16".*stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/.test(navGlyphSvg('wreck', 16, '#fff')), true);
eq('the unlit mark on a structure', /M11 5l3.2-3.2/.test(navHazardMarker('structure', true, false).html), true);
console.log(`navtex map: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
