#!/usr/bin/env node
/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * gen-river-areas.mjs — RIVERS AS WATER for the GPU map's detail pack.
 *
 *   node scripts/gen-river-areas.mjs        -> build/maptiles/riverareas.ndjson   (then gen-map-tiles.mjs)
 *   node scripts/gen-river-areas.mjs --overlay[=150] -> build/maptiles/vibemap-water.pmtiles (the directory's water overlay)
 *
 * ★★★ WHY. Stuart, 2026-11-20: "wisbech port is on land" — and Mac Ha (Vietnam) the same day: AIS ships "float like
 *     over land" on the Saigon river. The land at z10 is the OSM COASTLINE, which stops at the river mouths; the
 *     rivers were Natural Earth CENTRE LINES only. So every port up a river — Wisbech on the tidal Nene, Saigon,
 *     Cat Lai, Hamburg's Elbe, Rotterdam's Maas — sat on solid land with a thin line through it.
 *
 * ★★ THE SOURCE: OpenStreetMap's river AREAS (the riverbanks: water=river / waterway=riverbank, plus ship canals,
 *    tidal channels and docks) as Overture Maps publishes them (theme base, type water) — one GeoParquet set on a
 *    public bucket, read by bounding box and column, so this never needs the 80 GB planet. ODbL, the same licence and
 *    credit as the OSM coastline already in the detail pack ("© OpenStreetMap contributors").
 *    The release is PINNED (RELEASE below): a map built twice from the same tree is the same map.
 *
 * ★★ WHAT IS KEPT: polygons whose mean width (2 × area ÷ perimeter) is at least MIN_WIDTH_M. 20 m keeps the Nene
 *    through Wisbech (its pieces measure 24–88 m) and drops the field ditches and drains (canals' median is 9 m).
 *    Simplified to a quarter of the river's own width, 10–60 m, and kept at 4 decimals (~11 m) — at the tiers'
 *    3 dp a 60 m river's two banks round onto each other.
 *
 * ★ SIZE (2026-11-20, release 2026-11-18.0): 351 k polygons; at z10 only (MIN_ZOOM) +52 MB on the 169 MB detail
 *   pack; from z8 it was +127 MB. Below z10 the river lines carry on as before.
 *
 * ★ NOT IN the JSON tiers (assets/mapdata, committed — 20 M points do not belong in git) and so not on the Leaflet
 *   fallback map: this is a GPU-pack layer only. Needs `duckdb` (brew install duckdb); ~15 min, most of it download.
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, renameSync, rmSync, statSync, writeFileSync } from 'node:fs';
import path from 'node:path';

const RELEASE = '2026-11-18.0';
const MIN_WIDTH_M = 20;
export const MIN_ZOOM = 10;   // ★ gen-map-tiles reads the zooms from each feature; the style's riverareas-d matches
/* ★ v4 (2026-11-20, Stuart: "is there no way we can make the rivers appear even roughly on the more zoomed out views?"):
 *  the WIDE rivers from z8 — a 150 m river is a few pixels at z8, a 374 m Saigon reach a clear band — simplified to 60 m
 *  (sub-pixel there). Measured: ≥150 m at z8–9 = +16 MB, 35 k polygons (≥300 m +7.6 MB, ≥100 m +27 MB). */
export const MID_MIN_WIDTH_M = 150, MID_ZOOM = 8;

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const OUT = path.join(root, 'build/maptiles');
const CACHE = path.join(root, '.mapdata-cache');
mkdirSync(OUT, { recursive: true }); mkdirSync(CACHE, { recursive: true });
const parquet = path.join(CACHE, `overture-riverareas-${RELEASE}.parquet`);
const ndjson = path.join(OUT, 'riverareas.ndjson');

try { execFileSync('duckdb', ['--version'], { stdio: 'ignore' }); }
catch { console.error('gen-river-areas: needs duckdb (brew install duckdb)'); process.exit(1); }

const run = (sql) => execFileSync('duckdb', ['-no-agent'], { input: sql, stdio: ['pipe', 'inherit', 'inherit'] });
const head = `LOAD httpfs; LOAD spatial; SET s3_region='us-west-2'; SET geometry_always_xy=true; SET memory_limit='8GB'; SET threads=6;
SET preserve_insertion_order=false;`;
try { run('INSTALL httpfs; INSTALL spatial;'); } catch { /* already installed */ }

// ── 1. the areas, once per release (cached): only the water polygons that are rivers, ship canals, tidal channels, docks
if (!existsSync(parquet)) {
  console.log(`gen-river-areas: reading Overture ${RELEASE} (theme=base/type=water) — about 15 minutes…`);
  run(`${head}
COPY (
  SELECT id, class, geometry,
         ST_Area_Spheroid(geometry) / 1e6 AS km2,
         ST_Perimeter_Spheroid(geometry) / 1e3 AS perim_km
  FROM read_parquet('s3://overturemaps-us-west-2/release/${RELEASE}/theme=base/type=water/*')
  WHERE (subtype = 'river' OR class IN ('canal', 'tidal_channel', 'dock'))
    AND ST_GeometryType(geometry) IN ('POLYGON', 'MULTIPOLYGON')
) TO '${parquet}.part' (FORMAT parquet, COMPRESSION zstd);`);
  renameSync(`${parquet}.part`, parquet);
}

/* ── 2a. --overlay[=<min width m>]: THE DIRECTORY'S WATER OVERLAY PACK, build/maptiles/vibemap-water.pmtiles ──────────
 * ★★ Stuart, 2026-11-25 (the community map): "make sure the map has the waterways for shipping as we agreed". The
 *    directory draws the BASIC pack only (Natural Earth river lines), so Rotterdam, the Humber and the Saigon river were
 *    land and the ships sat on it. The detail pack (250 MB) cannot go on a web page, and the basic pack is shared with the
 *    app and the servers (changing it is a beta95 decision) — so a SEPARATE small overlay, one layer `water`, drawn by the
 *    directory pages only (directory/public/mapgl/vibemap-water.js).
 * ★★★ HARD LIMIT: Workers static assets cap a file at 25 MiB — the pack must stay under 20 MB. The width cut-off is the
 *    knob. MEASURED 2026-11-25 (release 2026-11-18.0): >= 150 m to z10 = 28.4 MB (too big); >= 200 m to z10 = 20.9 MB
 *    (too big); >= 150 m to z9 = 17.3 MB SHIPPED (--overlay=150 --maxzoom=9; the page over-zooms z9, edges ~20 m coarse).
 * ★ Zoom by width, so a z5 tile is not asked to carry a 150 m river: ≥ 1500 m from z5, ≥ 750 m z6, ≥ 400 m z7, the rest
 *   from z8; all to --maxzoom (default 10). Simplified to a quarter of the river's width, 15–100 m. */
const overlayArg = process.argv.find((a) => a.startsWith('--overlay'));
if (overlayArg) {
  const minW = Number(overlayArg.split('=')[1] || 150);
  const maxZ = Number((process.argv.find((a) => a.startsWith('--maxzoom=')) || '=10').split('=')[1]);
  const nd = path.join(OUT, 'water-overlay.ndjson'), pm = path.join(OUT, 'vibemap-water.pmtiles');
  run(`${head.replace("'8GB'", "'6GB'").replace('threads=6', 'threads=4')}
COPY (
  SELECT '{"type":"Feature","tippecanoe":{"layer":"water","minzoom":'
         || (CASE WHEN w >= 1500 THEN 5 WHEN w >= 750 THEN 6 WHEN w >= 400 THEN 7 ELSE 8 END)
         || ',"maxzoom":' || ${maxZ} || '},"properties":{},"geometry":'
         || ST_AsGeoJSON(ST_ReducePrecision(ST_SimplifyPreserveTopology(geometry, least(greatest(w / 4, 15), 100) / 111000.0), 0.0001))
         || '}' AS j
  FROM (SELECT geometry, 2000 * km2 / perim_km AS w FROM '${parquet}' WHERE perim_km > 0)
  WHERE w >= ${minW}
) TO '${nd}.part' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '', DELIMITER '\t');`);
  renameSync(`${nd}.part`, nd);
  try {
    execFileSync('tippecanoe', ['-o', pm, '--force', '-Z5', `-z${maxZ}`, '--no-feature-limit', '--no-tile-size-limit', '--quiet',
      '-n', 'VibeSDR water overlay', '-A', '© OpenStreetMap contributors (rivers via Overture Maps) · ODbL', '-P', nd], { stdio: 'inherit' });
  } finally { rmSync(nd, { force: true }); }   // ★ the intermediate is big — never left behind (mac_disk_cleanup, 09-28)
  console.log(`gen-river-areas: ${pm} ${(statSync(pm).size / 1048576).toFixed(2)} MB (width ≥ ${minW} m)`);
  process.exit(0);
}

// ── 2. the tippecanoe features: wide enough, simplified to the river's width, 4 dp
const part = `${ndjson}.part`;
run(`${head}
COPY (
  SELECT '{"type":"Feature","tippecanoe":{"layer":"riverareas","minzoom":${MIN_ZOOM},"maxzoom":10},"properties":{},"geometry":'
         || ST_AsGeoJSON(ST_ReducePrecision(ST_SimplifyPreserveTopology(geometry,
                least(greatest(2000 * km2 / perim_km / 4, 10), 60) / 111000.0), 0.0001)) || '}' AS j
  FROM '${parquet}'
  WHERE perim_km > 0 AND 2000 * km2 / perim_km >= ${MIN_WIDTH_M}
  UNION ALL
  SELECT '{"type":"Feature","tippecanoe":{"layer":"riverareas","minzoom":${MID_ZOOM},"maxzoom":${MIN_ZOOM - 1}},"properties":{},"geometry":'
         || ST_AsGeoJSON(ST_ReducePrecision(ST_SimplifyPreserveTopology(geometry, 60 / 111000.0), 0.0001)) || '}' AS j
  FROM '${parquet}'
  WHERE perim_km > 0 AND 2000 * km2 / perim_km >= ${MID_MIN_WIDTH_M}
) TO '${part}' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '', DELIMITER '\t');`);
renameSync(part, ndjson);
writeFileSync(path.join(OUT, 'riverareas.json'), JSON.stringify({ release: RELEASE, minWidthM: MIN_WIDTH_M, minZoom: MIN_ZOOM,
  midMinWidthM: MID_MIN_WIDTH_M, midZoom: MID_ZOOM,
  licence: 'ODbL', attribution: '© OpenStreetMap contributors (via Overture Maps)' }, null, 1));
console.log(`gen-river-areas: ${ndjson} ${(statSync(ndjson).size / 1048576).toFixed(0)} MB`);
