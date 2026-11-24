/**
 * gen-lrpt-coast.mjs — the coastlines, lakes and country borders the METEOR LRPT map overlay draws
 * (android/app/src/main/cpp/vibe_lrpt_coast.h, compiled into every server: VibeServer, the app's server, Lite).
 *
 *   node scripts/gen-lrpt-coast.mjs [--cache DIR]      -> android/app/src/main/cpp/vibe_lrpt_coast.h
 *   node scripts/gen-lrpt-coast.mjs --check [...]      -> fail if the checked-in header has drifted
 *
 * ★★ WHY COMPILED IN, when the basemap is files on disk (vibe_mapdata.h): the overlay is made by the SERVER at the end of
 *    a pass, on every kind of server — an Android phone, Lite on a TV, a Pi with no map pack installed — and "no maps"
 *    must not mean "no overlay". So the data is small and static: read-only bytes in the binary (paged in from the file
 *    when a pass is drawn, never copied to the heap), ~0.5 MB.
 * ★ SIZED FOR THE PICTURE, NOT FOR A MAP. MSU-MR's pixels are 1 km at nadir and 2–3 km at the swath's edge, so the lines
 *   are simplified (Douglas–Peucker, kTolKm), islands under kMinIslandKm2 dropped, and stored to 1/180° (≤ 0.31 km of rounding): well under a pixel.
 * ★ SOURCES — Natural Earth 1:10m (PUBLIC DOMAIN): coastline, minor-island coastlines, land border lines (admin 0); the
 *   1:50m lakes (outlines of the big ones only, ≥ kMinLakeKm2). The same files gen-map-data.mjs caches in .mapdata-cache/.
 *   Fails loudly: a missing file or an empty layer stops the build.
 */
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const check = args.includes('--check');
const ci = args.indexOf('--cache');
const cacheDir = ci >= 0 ? args[ci + 1] : path.join(root, '.mapdata-cache');
const out = path.join(root, 'android/app/src/main/cpp/vibe_lrpt_coast.h');

const kScale = 180;            // 1/180° units: ±180° = ±32400 fits int16
const kTolKm = 1.0;            // Douglas–Peucker tolerance — the picture's 1 km pixel at nadir
const kMinIslandKm2 = 25;      // a closed coastline (an island) smaller than this is a few pixels: left out
const kMinLakeKm2 = 2000;      // only lakes a 1–3 km/pixel picture shows

function die(m) { console.error('gen-lrpt-coast: ' + m); process.exit(1); }
async function load(name) {
  const p = path.join(cacheDir, name);
  let t; try { t = await readFile(p, 'utf8'); } catch { die(`missing ${p} (run gen-map-data.mjs, or pass --cache DIR)`); }
  try { return JSON.parse(t); } catch (e) { die(`${name}: not JSON (${e.message})`); }
}
function lines(geo) {
  const outL = [];
  for (const f of geo.features) {
    const g = f.geometry; if (!g) continue;
    if (g.type === 'LineString') outL.push(g.coordinates);
    else if (g.type === 'MultiLineString') outL.push(...g.coordinates);
    else if (g.type === 'Polygon') outL.push(...g.coordinates);
    else if (g.type === 'MultiPolygon') for (const p of g.coordinates) outL.push(...p);
  }
  return outL;
}
// Douglas–Peucker in local km (longitude scaled by cos(latitude))
function simplify(pts) {
  if (pts.length < 3) return pts;
  const keep = new Uint8Array(pts.length); keep[0] = keep[pts.length - 1] = 1;
  const km = 111.2;
  const stack = [[0, pts.length - 1]];
  while (stack.length) {
    const [a, b] = stack.pop();
    if (b - a < 2) continue;
    const c = Math.cos(((pts[a][1] + pts[b][1]) / 2) * Math.PI / 180);
    const ax = pts[a][0] * c * km, ay = pts[a][1] * km, bx = pts[b][0] * c * km, by = pts[b][1] * km;
    const dx = bx - ax, dy = by - ay, L2 = dx * dx + dy * dy;
    let best = -1, bi = -1;
    for (let i = a + 1; i < b; i++) {
      const px = pts[i][0] * c * km - ax, py = pts[i][1] * km - ay;
      let d;
      if (L2 === 0) d = Math.hypot(px, py);
      else { const t = Math.max(0, Math.min(1, (px * dx + py * dy) / L2)); d = Math.hypot(px - t * dx, py - t * dy); }
      if (d > best) { best = d; bi = i; }
    }
    if (best > kTolKm) { keep[bi] = 1; stack.push([a, bi], [bi, b]); }
  }
  return pts.filter((_, i) => keep[i]);
}
function quantise(pts) {
  const q = [];
  for (const [lon, lat] of pts) {
    const x = Math.round(lon * kScale), y = Math.round(lat * kScale);
    if (q.length && q[q.length - 1][0] === y && q[q.length - 1][1] === x) continue;
    q.push([y, x]);
  }
  return q;
}
function polyAreaKm2(ring) {
  let s = 0; const lat0 = ring[0][1] * Math.PI / 180, km = 111.2;
  for (let i = 0; i < ring.length; i++) {
    const [x1, y1] = ring[i], [x2, y2] = ring[(i + 1) % ring.length];
    s += (x1 * km * Math.cos(lat0)) * (y2 * km) - (x2 * km * Math.cos(lat0)) * (y1 * km);
  }
  return Math.abs(s) / 2;
}

const layers = [
  { kind: 0, name: 'coast', src: [await load('ne_10m_coastline.geojson'), await load('ne_10m_minor_islands_coastline.geojson')] },
  { kind: 1, name: 'lake', src: [await load('lakes50.geojson')], minArea: kMinLakeKm2 },
  { kind: 2, name: 'border', src: [await load('ne_10m_admin_0_boundary_lines_land.geojson')] },
];
const bytes = [], recs = [];
const count = {};
let points = 0;
for (const L of layers) {
  count[L.name] = 0;
  for (const g of L.src) for (const l of lines(g)) {
    if (L.minArea && polyAreaKm2(l) < L.minArea) continue;
    const closed = l.length > 3 && l[0][0] === l[l.length - 1][0] && l[0][1] === l[l.length - 1][1];
    if (L.kind === 0 && closed && polyAreaKm2(l) < kMinIslandKm2) continue;
    const q = quantise(simplify(l));
    if (q.length < 2) continue;
    // ★ long lines are split so each record's bounding box stays small (the server culls by box per pass)
    for (let s = 0; s < q.length - 1; s += 255) {
      const part = q.slice(s, Math.min(q.length, s + 256));
      if (part.length < 2) continue;
      let la0 = 1e9, la1 = -1e9, lo0 = 1e9, lo1 = -1e9;
      for (const [y, x] of part) { la0 = Math.min(la0, y); la1 = Math.max(la1, y); lo0 = Math.min(lo0, x); lo1 = Math.max(lo1, x); }
      recs.push([bytes.length, part.length, L.kind, part[0][0], part[0][1], la0, la1, lo0, lo1]);
      // ★ DELTAS: one signed byte each for lat and lon (±127 units = ±0.7°); a step that does not fit is the escape
      //   byte −128 followed by the step as two int16 (little-endian bytes), lat then lon
      for (let i = 1; i < part.length; i++) {
        const dy = part[i][0] - part[i - 1][0], dx = part[i][1] - part[i - 1][1];
        if (dy >= -127 && dy <= 127 && dx >= -127 && dx <= 127) bytes.push(dy, dx);
        else bytes.push(-128, dy & 0xff, (dy >> 8) & 0xff, dx & 0xff, (dx >> 8) & 0xff);
      }
      count[L.name] += part.length; points += part.length;
    }
  }
  if (!count[L.name]) die(`layer ${L.name} is empty`);
}
const sb = bytes.map(b => (b > 127 ? b - 256 : b));
const wrap = (arr, per) => { const o = []; for (let i = 0; i < arr.length; i += per) o.push('    ' + arr.slice(i, i + per).join(',') + ','); return o.join('\n'); };
const text = `// vibe_lrpt_coast.h — GENERATED by scripts/gen-lrpt-coast.mjs; do not edit. Natural Earth (public domain): 1:10m
// coastline + minor islands (islands ≥ ${kMinIslandKm2} km²), 1:50m lakes ≥ ${kMinLakeKm2} km², 1:10m land borders;
// Douglas–Peucker ${kTolKm} km, stored to 1/${kScale}° as byte deltas.
// ${count.coast} coast + ${count.lake} lake + ${count.border} border points in ${recs.length} records (${(bytes.length + recs.length * 20) / 1024 | 0} KB read-only).
#pragma once
#include <cstdint>
namespace vibelrpt {
namespace coast {
constexpr int kScale = ${kScale};      ///< units a degree
enum Kind : uint8_t { Coast = 0, Lake = 1, Border = 2 };
/** One polyline: n points from (lat0, lon0); kDelta[first …] holds the n − 1 steps (a signed byte each for lat and lon,
 *  or −128 then the step as two little-endian int16). Its bounding box in the same units. */
struct Rec { uint32_t first; uint16_t n; uint8_t kind; int16_t lat0, lon0, latMin, latMax, lonMin, lonMax; };
inline constexpr int8_t kDelta[] = {
${wrap(sb, 40)}
};
inline constexpr Rec kRecs[] = {
${recs.map(r => `    {${r.join(',')}},`).join('\n')}
};
inline constexpr unsigned kRecCount = ${recs.length};
inline constexpr unsigned kPointCount = ${points};
} // namespace coast
} // namespace vibelrpt
`;
if (check) {
  let cur = ''; try { cur = await readFile(out, 'utf8'); } catch {}
  if (cur !== text) die('vibe_lrpt_coast.h has drifted — re-run node scripts/gen-lrpt-coast.mjs');
  console.log('vibe_lrpt_coast.h up to date');
} else {
  await writeFile(out, text);
  console.log(`wrote ${path.relative(root, out)}: ${JSON.stringify(count)}, ${recs.length} records, ${text.length} bytes of source`);
}
