// test_sea_areas.ts — V12 2026-11-02 THE SHIPPING MAP's SEA AREAS (services/seaAreas.ts, src/utils/seaAreas.generated.ts):
// the dataset's integrity (every UK shipping forecast area, closed rings, each label point inside its own outline and the
// generated files current), the presence rule (no SEA AREAS toggle where no area is near), the tint (amber 7, red gale),
// the names (hidden under a forecast, yielding, by zoom) and the arrow moved inside its outline.
//   npx tsx scripts/test_sea_areas.ts
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import { seaAreas, seaAreaById, seaAreasFor, seaCompose, seaSeverity, seaPlaceMet, seaLabel, seaLegendRows, seaAttribution, SEA_NAME_ZOOM } from '../src/services/seaAreas.ts';
import { shipLayersPresent, shipToggles } from '../src/services/shippingMap.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, w: string) => { console.log(`  [${c ? 'PASS' : 'FAIL'}] ${w}`); c ? pass++ : fail++; };
const inRing = (p: [number, number], r: Array<[number, number]>) => {
  let inside = false;
  for (let i = 0, j = r.length - 1; i < r.length; j = i++) {
    const [yi, xi] = r[i], [yj, xj] = r[j];
    if (((yi > p[0]) !== (yj > p[0])) && p[1] < ((xj - xi) * (p[0] - yi)) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
};

console.log('test_sea_areas — the SHIPPING MAP sea areas');
console.log('── the dataset ──');
{
  const all = seaAreas();
  // ★ every UK shipping forecast area of the met table is drawn (the hand-made table's 's' rows)
  const metH = fs.readFileSync('android/app/src/main/cpp/vibe_navtex_met.h', 'utf8');
  const uk = [...metH.matchAll(/\{"([a-z]+)", "([^"]+)", -?[0-9.]+, -?[0-9.]+, 's'/g)].map((m) => m[1]);
  ok(uk.length === 31, `the met table has the 31 shipping forecast areas (${uk.length})`);
  const missing = uk.filter((id) => !seaAreaById(id));
  ok(!missing.length, 'every UK area has an outline' + (missing.length ? ': missing ' + missing.join(', ') : ''));
  for (const id of ['dogger', 'wight', 'plymouth', 'trafalgar', 'fitzroy', 'seiceland']) ok(!!seaAreaById(id) && seaAreaById(id)!.src === 'uk', `${id}: present, from the Met Office's corner points`);
  const ids = new Set<string>();
  let dup = 0, open = 0, small = 0, outside: string[] = [];
  for (const a of all) {
    if (ids.has(a.id)) dup++; ids.add(a.id);
    for (const r of a.rings) if (r.length < 3) small++;
    // a ring is closed by definition (the last point joins the first): it must have area, i.e. not be a line
    for (const r of a.rings) { let s = 0; for (let i = 0, j = r.length - 1; i < r.length; j = i++) s += (r[j][1] - r[i][1]) * (r[j][0] + r[i][0]); if (Math.abs(s) < 1e-9) open++; }
    if (!a.rings.some((r) => inRing([a.lat, a.lon], r))) outside.push(a.id);
  }
  ok(dup === 0, 'every id once');
  ok(small === 0 && open === 0, `every ring closed with an area (${small} short, ${open} degenerate)`);
  ok(outside.length === 0, `every label point inside its own outline` + (outside.length ? `: ${outside.length} outside (${outside.slice(0, 6).join(', ')})` : ''));
  const by: Record<string, number> = {};
  for (const a of all) by[a.src] = (by[a.src] || 0) + 1;
  ok(by.uk === 31 && by.us > 400 && by.ca > 100 && by.de >= 5, `sources: ${JSON.stringify(by)}`);
  // the Irish Sea's label is in open water, not on the Isle of Man (54.15 N, 4.5 W)
  const is = seaAreaById('irishsea')!;
  ok(!(Math.abs(is.lat - 54.2) < 0.25 && Math.abs(is.lon + 4.5) < 0.35), `Irish Sea label off the Isle of Man (${is.lat}, ${is.lon})`);
  // generated files current (no network: the generator re-reads its committed cache)
  let current = true;
  try { execFileSync('node', ['scripts/gen-sea-areas.mjs', '--check'], { stdio: 'pipe' }); } catch { current = false; }
  ok(current, 'gen-sea-areas.mjs --check: the generated files are current');
  const size = fs.statSync('src/utils/seaAreas.generated.ts').size;
  ok(size < 260 * 1024, `the bundled file is modest (${(size / 1024).toFixed(0)} KB)`);
}
console.log('── presence: absent where no area is near ──');
{
  const london = { lat: 51.5, lon: -0.1 }, tokyo = { lat: 35.6, lon: 139.7 }, halifax = { lat: 44.65, lon: -63.57 };
  const nUk = seaAreasFor(london, null).length, nJp = seaAreasFor(tokyo, null).length;
  ok(nUk >= 31 && nUk < 120, `a London receiver: the UK and nearby areas (${nUk})`);
  ok(nJp === 0, 'a Tokyo receiver: none — no SEA AREAS toggle');
  ok(seaAreasFor(tokyo, { areas: [{ id: 'dogger', lat: 0, lon: 0, stale: false, fc: [] }] }).length === 1, 'a forecast names an area far away: that one is drawn');
  const nHx = seaAreasFor(halifax, null);
  ok(nHx.some((a) => a.src === 'ca') && nHx.some((a) => a.src === 'us'), `Halifax: Canadian and US areas (${nHx.length})`);
  const base = { ships: 0, dsc: 0, stations: 1, warnings: 0, weather: 1, aisMode: false };
  ok(shipLayersPresent({ ...base, areas: nUk }, 'navtex').join() === 'stations,weather,areas', 'NAVTEX map near the UK: STATIONS, WEATHER, SEA AREAS');
  ok(!shipLayersPresent({ ...base, areas: 0 }, 'navtex').includes('areas'), 'no area near: no SEA AREAS layer');
  ok(shipToggles(['stations', 'weather', 'areas']).some((t) => t.label === 'SEA AREAS'), 'the toggle is labelled SEA AREAS');
}
console.log('── the tint and the names ──');
{
  ok(seaSeverity({ gale: 8, fmax: 6 }) === 2 && seaSeverity({ gale: 0, fmax: 9 }) === 2, 'a gale word or force 8+: red');
  ok(seaSeverity({ gale: 0, fmax: 7 }) === 1, 'force 7: amber');
  ok(seaSeverity({ gale: 0, fmax: 6, wind: { sev: 0, sev2: 2 } }) === 2, 'a later gale in the arrows: red');
  ok(seaSeverity({ gale: 0, fmax: 5 }) === 0 && seaSeverity(null) === 0, 'below 7, or no forecast: no tint');
  const met = { areas: [
    { id: 'dogger', lat: 54.8, lon: 2.2, stale: false, fc: [{ gale: 9, fmax: 9 }] },
    { id: 'wight', lat: 50.3, lon: -1.0, stale: false, fc: [{ gale: 0, fmax: 7 }] },
    { id: 'thames', lat: 52.2, lon: 2.3, stale: true, fc: [{ gale: 9, fmax: 9 }] },
  ] };
  const it = seaCompose({ lat: 51.5, lon: -0.1 }, met, true).items;
  const of = (id: string) => it.find((x: any) => x.key === 'a:' + id);
  ok(of('dogger')._sh.colour === '#ff5a4a' && of('dogger')._sh.fillOpacity > 0, 'Dogger (severe gale 9): tinted red');
  ok(of('wight')._sh.colour === '#ffb74d', 'Wight (7): amber');
  ok(of('thames')._sh.fillOpacity === 0, 'Thames (a stale gale): no tint — never its old conditions');
  ok(of('dogger')._ic.html === '' && of('portland')._ic.html.includes('PORTLAND'), 'a forecast area shows no name of its own (the arrow label names it); the others their name in capitals');
  ok(it.every((x: any) => x._y === true), 'every name yields to the other icons and labels');
  ok(of('portland')._mz === SEA_NAME_ZOOM.s, 'names drop before outlines at low zoom (a name from zoom 4)');
  const off = seaCompose({ lat: 51.5, lon: -0.1 }, met, false).items;
  ok(off.every((x: any) => x._sh.fillOpacity === 0), 'WEATHER off: plain outlines, no tint');
  const moved = seaPlaceMet([{ id: 'dogger', lat: 0, lon: 0 }, { id: 'i-minch', lat: 58, lon: -5.9 }]);
  const d = seaAreaById('dogger')!;
  ok(moved[0].lat === d.lat && moved[0].lon === d.lon && moved[1].lat === 58, "a forecast's arrow moves to its area's label point; an inshore stretch (no outline) keeps the table's");
  ok(seaLabel(seaAreaById('us-anz250')!) === 'ANZ250' || seaLabel(seaAreaById('us-anz250')!).length <= 26, 'a US coastal zone is named by its code on the map');
  ok(seaLegendRows(true).length === 3 && seaLegendRows(false).length === 1, 'KEY: the outline, amber and red rows (tint rows only with WEATHER)');
  ok(/Met Office/.test(seaAttribution(seaAreasFor({ lat: 51.5, lon: -0.1 }, null))), 'attribution names the Met Office definitions for UK areas');
  ok(/Open Government Licence/.test(seaAttribution(seaAreasFor({ lat: 44.65, lon: -63.57 }, null))), 'attribution carries the OGL-Canada line for Canadian areas');
}
console.log(`${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
