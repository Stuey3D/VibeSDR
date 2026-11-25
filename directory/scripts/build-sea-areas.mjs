#!/usr/bin/env node
// build-sea-areas.mjs — THE COMMUNITY MAP's SEA AREAS: one static, simplified GeoJSON file (public/community-sea-areas.json)
// made from the SAME dataset the server's NAVTEX / shipping map draws — V12's src/utils/seaAreas.generated.ts, written by
// scripts/gen-sea-areas.mjs from scripts/mapdata/sea-areas/ (the sources and their licences are in that generator's
// header; each source's attribution travels in this file and is credited on the map).
//
// ★★ Stuart (2026-11-25): "can we draw all the shipping areas onto it like we do for navtex?"
// ★ NO NEW DATASET: the rows are read as V12 wrote them, decoded with its own decodeRing, the names by its own seaLabel rule
//   (src/services/seaAreas.ts), the label point its generator's (inside the outline, never on land).
// ★ SMALL: Douglas–Peucker (TOL_FINE / TOL_WIDE below), coordinates at 0.01° (~1 km — an outline on a world map); a ring
//   that collapses, or an island speck, is dropped. Served beside the page and fetched ONCE, only while SEA AREAS is drawn (community-map.html).
//
// usage: node directory/scripts/build-sea-areas.mjs [path to the V12 checkout]     (default ../VibeSDR-v12 beside this repo)
//        node directory/scripts/build-sea-areas.mjs --check [v12]                    exit 1 if the committed file is stale
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '..', '..');
const CHECK = process.argv.includes('--check');
const v12 = path.resolve(process.argv.slice(2).find((a) => !a.startsWith('--')) || path.join(repo, '..', 'VibeSDR-v12'));
const OUT = path.join(here, '..', 'public', 'community-sea-areas.json');
const GEN = path.join(v12, 'scripts', 'gen-sea-areas.mjs');
const ROWS = path.join(v12, 'src', 'utils', 'seaAreas.generated.ts');
if (!fs.existsSync(GEN) || !fs.existsSync(ROWS)) { console.error(`not a V12 checkout (no ${path.relative(v12, ROWS)}): ${v12}`); process.exit(2); }
const { decodeRing, SOURCES } = await import(pathToFileURL(GEN).href);

// ── the rows, as V12 wrote them: [id, kind, source, name, labelLat, labelLon, rings] — one JSON array a line ──
const rows = fs.readFileSync(ROWS, 'utf8').split('\n').filter((l) => l.startsWith('["')).map((l) => JSON.parse(l.replace(/,\s*$/, '')));
if (rows.length < 100) { console.error(`only ${rows.length} sea areas read from ${ROWS}`); process.exit(1); }

/** src/services/seaAreas.ts seaLabel: the area's own name; a US zone's long description gives way to its zone code. */
const label = (id, kind, src, name) => (src === 'us' ? (kind === 'o' && name.length <= 26 ? name : id.slice(3).toUpperCase()) : name).toUpperCase();
/** Douglas–Peucker (iterative), in degrees. */
function simplify(pts, tol) {
  if (pts.length < 5) return pts;
  const keep = new Uint8Array(pts.length); keep[0] = keep[pts.length - 1] = 1;
  const st = [[0, pts.length - 1]], t2 = tol * tol;
  while (st.length) {
    const [a, b] = st.pop();
    const [ax, ay] = pts[a], [bx, by] = pts[b], dx = bx - ax, dy = by - ay, L = dx * dx + dy * dy;
    let md = -1, mi = -1;
    for (let i = a + 1; i < b; i++) {
      const [px, py] = pts[i];
      let t = L ? ((px - ax) * dx + (py - ay) * dy) / L : 0; t = Math.max(0, Math.min(1, t));
      const d = (px - ax - t * dx) ** 2 + (py - ay - t * dy) ** 2;
      if (d > md) { md = d; mi = i; }
    }
    if (md > t2) { keep[mi] = 1; st.push([a, mi], [mi, b]); }
  }
  return pts.filter((_, i) => keep[i]);
}
/* ★ The tolerance, in degrees: the UK's, Ireland's and Europe's areas at ~1 km (they follow the coast, and the NAVTEX forecasts
 *  name them); the NOAA, ECCC and JMA zones — ragged coasts and fjords, most of the file's weight — at ~4 km. An island speck
 *  inside an area (a ring under MIN_RING deg², not the area's largest) is dropped: an outline, not a land mask. */
const TOL_FINE = 0.01, TOL_WIDE = 0.04, WIDE = new Set(['us', 'ca', 'jp']), MIN_RING = 0.004;
const ringArea = (r) => { let a = 0; for (let i = 0, j = r.length - 1; i < r.length; j = i++) a += (r[j][0] - r[i][0]) * (r[j][1] + r[i][1]); return Math.abs(a / 2); };
const r2 = (v) => Math.round(v * 100) / 100;
let points = 0;
const features = [], labels = [], srcs = new Set();
for (const [id, kind, src, name, lat, lon, enc] of rows) {
  const polys = [];
  const raw = enc.split(' ').filter(Boolean).map(decodeRing);
  const big = Math.max(...raw.map(ringArea));
  for (const r of raw) {
    if (r !== raw.find((x) => ringArea(x) === big) && ringArea(r) < MIN_RING) continue;
    const ring = [];
    for (const [x, y] of simplify(r, WIDE.has(src) ? TOL_WIDE : TOL_FINE)) {
      const p = [r2(x), r2(y)], q = ring[ring.length - 1];
      if (!q || q[0] !== p[0] || q[1] !== p[1]) ring.push(p);
    }
    if (ring.length > 1 && ring[0][0] === ring[ring.length - 1][0] && ring[0][1] === ring[ring.length - 1][1]) ring.pop();
    if (ring.length < 3) continue;
    ring.push(ring[0]);
    points += ring.length;
    polys.push([ring]);
  }
  if (!polys.length) continue;
  srcs.add(src);
  features.push({ type: 'Feature', properties: { id, k: kind }, geometry: { type: 'MultiPolygon', coordinates: polys } });
  labels.push({ type: 'Feature', properties: { id, k: kind, n: label(id, kind, src, name) }, geometry: { type: 'Point', coordinates: [lon, lat] } });
}
const order = Object.keys(SOURCES).filter((s) => srcs.has(s));
let v12rev = '';
try { v12rev = fs.readFileSync(path.join(v12, '.git', 'HEAD'), 'utf8').trim(); } catch { /* not a checkout */ }
const out = {
  type: 'FeatureCollection',
  about: 'VibeSDR community map: the sea areas (shipping / marine forecast areas). GENERATED by directory/scripts/build-sea-areas.mjs from the V12 '
    + 'checkout\'s src/utils/seaAreas.generated.ts (scripts/gen-sea-areas.mjs) — do not edit. Outlines simplified: ~1 km (UK, Ireland, Europe, the high seas), ~4 km (NOAA, ECCC, JMA).',
  // ★ who to credit, per source drawn (the map's attribution line: SEA_AREA_SOURCES[s].attribution, as the server map's)
  attribution: Object.fromEntries(order.map((s) => [s, SOURCES[s].attribution])),
  licence: Object.fromEntries(order.map((s) => [s, `${SOURCES[s].name}: ${SOURCES[s].what} — ${SOURCES[s].licence}`])),
  // the polygons, then the label points (kind 's' UK shipping forecast area, 'o' offshore / marine forecast area, 'c' coastal zone)
  features: features.concat(labels),
};
const text = JSON.stringify(out) + '\n';
if (CHECK) {
  const have = fs.existsSync(OUT) ? fs.readFileSync(OUT, 'utf8') : '';
  if (have !== text) { console.error(`${path.relative(repo, OUT)} is stale — run node directory/scripts/build-sea-areas.mjs`); process.exit(1); }
  console.log(`${path.relative(repo, OUT)}: up to date`);
  process.exit(0);
}
fs.writeFileSync(OUT, text);
console.log(`${path.relative(repo, OUT)}: ${features.length} areas, ${points} points, ${text.length} bytes (gzip ${zlib.gzipSync(text, { level: 9 }).length}, brotli ${zlib.brotliCompressSync(text).length}) — from V12 ${v12rev.slice(0, 40)}`);
