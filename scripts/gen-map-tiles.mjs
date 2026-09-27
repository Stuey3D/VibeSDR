#!/usr/bin/env node
/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * gen-map-tiles.mjs — the basemap as GPU VECTOR TILES (PMTiles), for MapLibre.
 *
 *   node scripts/gen-map-tiles.mjs            -> build/maptiles/vibemap-basic.pmtiles
 *                                                build/maptiles/vibemap-detail.pmtiles
 *
 * ★★★ WHY. Stuart, 2026-09-27: "that is what I've been after ever since we had the animated maps,
 *     I asked if we could GPU accelerate to improve animations and the tiles rendering in". Leaflet
 *     on a 2D canvas redraws the whole view on the CPU after every pan and holds whole layers in
 *     RAM; MapLibre reads only the tiles in view straight off disk and draws them on the GPU.
 *     See the memory note maplibre_gpu_maps_decision.
 *
 * ★★★ BUILT FROM OUR PROCESSED TIERS, NOT THE RAW SOURCES. assets/mapdata/v1 (gen-map-data.mjs) is
 *     where every cartographic decision already lives — which places earn which zoom, the OSM
 *     coastline that put Madeira's runway on land, the GeoNames towns that put Northampton on the
 *     map. Re-deriving from the raw downloads would be a second copy of all of that, and the two
 *     would drift. This only changes HOW it is drawn, never WHAT.
 *
 * ★★ THE TIER ZOOMS ARE vibemap.js's OWN: tier0 up to z4, then tier1, then tier2 from TIER_STEP.
 *    Copied, not guessed — a feature appearing at a different zoom would be a change of map.
 *
 * ★ TWO PACKS, the same split as today: BASIC (tier0 + tier1, bundled) and DETAIL (tier2, the
 *   optional download). MapLibre over-zooms the basic pack where the detail one is absent, so a
 *   device without it still draws a complete, coarser map — never a hole.
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */
import { readFileSync, writeFileSync, mkdirSync, readdirSync, existsSync, statSync } from 'node:fs';
import { createWriteStream } from 'node:fs';
import { execFileSync } from 'node:child_process';
import path from 'node:path';

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const SRC = [path.join(root, 'assets/mapdata/v1'), path.join(root, 'directory/public/mapdata/v1')]
  .find((d) => existsSync(path.join(d, 'index.json')));
if (!SRC) { console.error('no mapdata pack found — run scripts/gen-map-data.mjs first'); process.exit(1); }
const OUT = path.join(root, 'build/maptiles');
mkdirSync(OUT, { recursive: true });

/** vibemap.js TIER_STEP — the zoom at which tier2 takes over from tier1, per layer. */
const TIER_STEP = { countries: 10, cover: 8, urban: 8, roads: 8, rivers: 7, lakes: 7,
                    admin1: 8, shelf: 8, places: 8, airports: 8, islands: 7, reefs: 7, playas: 7 };
/* ★★ z10, NOT z12. The first build went to z12 and passed 1.2 GB still growing: at that zoom every
 *  large area (land, shelf, ice) is cut into millions of tiles. tier2 is ~3-decimal (~100 m), and a
 *  z10 tile addresses ~10 m, so tiles past z10 would carry nothing MapLibre's over-zoom does not
 *  already draw. Features that START deeper (runways at z9) are unaffected. */
const MAXZ = 10;

/** Every file for a tier/layer, sharded or not. */
function load(tier, layer) {
  const files = readdirSync(SRC).filter((f) => f === `${tier}-${layer}.json`
    || (f.startsWith(`${tier}-${layer}.s`) && f.endsWith('.json')));
  const out = [];
  for (const f of files) {
    const d = JSON.parse(readFileSync(path.join(SRC, f), 'utf8'));
    if (Array.isArray(d)) out.push(...d); else out.push(d);
  }
  return out;
}
/** The zoom range a tier covers for this layer, mirroring tierFor(). */
function range(tier, layer) {
  const step = TIER_STEP[layer] ?? 8;
  if (tier === 'tier0') return [0, 4];
  if (tier === 'tier1') return [5, step - 1];
  return [step, MAXZ];
}
const closed = (ring) => {
  if (ring.length < 3) return null;
  const a = ring[0], b = ring[ring.length - 1];
  return (a[0] === b[0] && a[1] === b[1]) ? ring : [...ring, a];
};

/* ── Writers: one NDJSON per pack, each feature carrying its own layer and zoom range ────────── */
const sinks = {};
let count = {};
function sink(pack) {
  if (!sinks[pack]) sinks[pack] = createWriteStream(path.join(OUT, `${pack}.ndjson`));
  return sinks[pack];
}
/* ★★★ LEAFLET ZOOM → MapLibre ZOOM IS MINUS ONE. Every zoom in this file (TIER_STEP, the tier
 *  ranges, "runways from z9") is a LEAFLET zoom — that is where the cartography was decided. Leaflet
 *  draws 256 px tiles; MapLibre draws 512 px, so MapLibre zoom z is the same SCALE as Leaflet z+1.
 *  Written unshifted, every layer would switch detail one zoom LATE — a change of map. Converted
 *  once, here, so nothing above has to think about it. The top stays MAXZ (over-zoom covers it). */
const toML = (z) => Math.max(0, z - 1);
function emit(pack, layer, [lminzoom, lmaxzoom], geometry, properties = {}) {
  const minzoom = toML(lminzoom), maxzoom = lmaxzoom >= MAXZ ? MAXZ : toML(lmaxzoom);
  if (minzoom > maxzoom) return;
  sink(pack).write(JSON.stringify({ type: 'Feature', tippecanoe: { layer, minzoom, maxzoom },
                                    properties, geometry }) + '\n');
  count[`${pack}/${layer}`] = (count[`${pack}/${layer}`] || 0) + 1;
}
const packFor = (tier) => (tier === 'tier2' ? 'vibemap-detail' : 'vibemap-basic');

/* ── Areas ───────────────────────────────────────────────────────────────────────────────── */
for (const tier of ['tier0', 'tier1', 'tier2']) {
  // Countries: { iso: [ring, ...] }. Below the coast tier the country outline IS the land.
  for (const obj of load(tier, 'countries'))
    for (const [iso, rings] of Object.entries(obj))
      for (const r of rings) { const c = closed(r); if (c) emit(packFor(tier), 'land', range(tier, 'countries'), { type: 'Polygon', coordinates: [c] }, { iso }); }
  // Cover: { class: [ring, ...] } — ice, desert, forest… what the ground is.
  for (const obj of load(tier, 'cover'))
    for (const [cls, rings] of Object.entries(obj))
      for (const r of rings) { const c = closed(r); if (c) emit(packFor(tier), 'cover', range(tier, 'cover'), { type: 'Polygon', coordinates: [c] }, { cls }); }
  for (const layer of ['lakes', 'urban', 'shelf', 'islands', 'playas'])
    for (const r of load(tier, layer)) { const c = closed(r); if (c) emit(packFor(tier), layer, range(tier, layer), { type: 'Polygon', coordinates: [c] }); }
  for (const layer of ['rivers', 'roads', 'rail', 'admin1', 'reefs'])
    for (const l of load(tier, layer)) if (l.length >= 2) emit(packFor(tier), layer, range(tier, layer === 'rail' ? 'roads' : layer), { type: 'LineString', coordinates: l });
}
// ★ The coastline tier: at z >= 10 the land comes from OSM coast rings, not country polygons.
for (const r of load('tier2', 'coast')) { const c = closed(r); if (c) emit('vibemap-detail', 'land', [10, MAXZ], { type: 'Polygon', coordinates: [c] }, { coast: 1 }); }
// ★ Borders have their own lines at every zoom where they are drawn (see drawBorders).
for (const l of load('tier2', 'borders')) if (l.length >= 2) emit('vibemap-detail', 'borders', [4, MAXZ], { type: 'LineString', coordinates: l });
for (const g of load('tier0', 'geolines'))
  for (const l of g.lines || []) emit('vibemap-basic', 'geolines', [2, 8], { type: 'LineString', coordinates: l }, { name: g.name });

/* ── Points ──────────────────────────────────────────────────────────────────────────────── */
for (const tier of ['tier0', 'tier1', 'tier2']) {
  for (const [name, lon, lat, rank, kind] of load(tier, 'places'))
    emit(packFor(tier), 'places', range(tier, 'places'), { type: 'Point', coordinates: [lon, lat] }, { name, rank, kind: kind ?? 0 });
  for (const [icao, lon, lat, cls, iata, name] of load(tier, 'airports'))
    emit(packFor(tier), 'airports', range(tier, 'airports'), { type: 'Point', coordinates: [lon, lat] }, { icao, cls, iata: iata || '', name: name || '' });
}
for (const [name, lon, lat, rank, local] of load('tier0', 'countrylabels'))
  emit('vibemap-basic', 'countrylabels', [0, MAXZ], { type: 'Point', coordinates: [lon, lat] }, { name, rank, local: local || '' });
for (const [name, lon, lat, rank, kind] of load('tier0', 'regions'))
  emit('vibemap-basic', 'regions', [0, MAXZ], { type: 'Point', coordinates: [lon, lat] }, { name, rank, kind: kind || '' });
for (const [name, lon, lat, code] of load('tier1', 'ports'))
  emit('vibemap-basic', 'ports', [6, MAXZ], { type: 'Point', coordinates: [lon, lat] }, { name, code: code || '' });
// ★ Runways as the real centreline between both thresholds, each end labelled with its own
//   designator — exactly what drawRunways does.
for (const [id, lon1, lat1, lon2, lat2, le, he, ft] of load('tier2', 'runways'))
  emit('vibemap-detail', 'runways', [9, MAXZ], { type: 'LineString', coordinates: [[lon1, lat1], [lon2, lat2]] }, { id, le: le || '', he: he || '', ft: ft || 0 });

await Promise.all(Object.values(sinks).map((s) => new Promise((r) => s.end(r))));
console.log('features:', count);

/* ── Tiles ───────────────────────────────────────────────────────────────────────────────── */
for (const pack of ['vibemap-basic', 'vibemap-detail']) {
  const out = path.join(OUT, `${pack}.pmtiles`);
  // ★ No tile-size or feature limits: tippecanoe's defaults DROP features to fit 500 kB tiles,
  //   which would be a silent change of map. Our tiers are already thinned by design.
  execFileSync('tippecanoe', ['-o', out, '--force', '-Z0', `-z${pack === 'vibemap-basic' ? 7 : MAXZ}`,
    '--no-feature-limit', '--no-tile-size-limit', '--quiet',
    '-P', path.join(OUT, `${pack}.ndjson`)], { stdio: 'inherit' });
  console.log(`${pack}.pmtiles  ${(statSync(out).size / 1048576).toFixed(1)} MB`);
}
