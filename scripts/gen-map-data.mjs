/**
 * gen-map-data.mjs — the bundled vector basemap, in three zoom tiers, for every map in the product
 * (HFDL aircraft, digital spots, CW, the directory, admin, and ADS-B/AIS when they land).
 *
 *   node scripts/gen-map-data.mjs             -> assets/mapdata/v1/*.json
 *   node scripts/gen-map-data.mjs --check     -> fail if the checked-in output has drifted
 *   node scripts/gen-map-data.mjs --refresh   -> re-download the sources (otherwise the cache is used)
 *
 * ★ WHY THIS EXISTS AT ALL. We drew these maps on OpenStreetMap raster tiles, and that cost us: a
 *   dependency on a free service that BLOCKED us, black gaps across the map during the HFDL flyover
 *   animation while tiles were in flight, mobile data burned re-downloading the world, and a
 *   projected 150–583 MB of on-device tile cache. Vectors are ~19 MB bundled, draw instantly at any
 *   zoom, restyle to our dark amber palette, and need NO INTERNET AT ALL — which is the whole reason
 *   a Pi Zero 2W VibeServer in captive-hotspot mode is viable: no uplink, and the map still works.
 *
 * ★★★ WHY THREE TIERS AND NOT ONE BLOB. The binding constraint is NOT file size — it is ~948,000
 *   points for Leaflet to lay out on a 1 GB Xcover while the map flies to a new aircraft every few
 *   seconds. Most of those points are never on screen at the zoom being shown. So each layer is
 *   emitted per tier and the renderer loads only what the zoom justifies:
 *     tier0  world     z0–4   coarse outlines, the handful of places you can actually read
 *     tier1  regional  z5–7   medium outlines, all NE places, medium airports, sea ports
 *     tier2  local     z8+    fine outlines, admin-1 lines, lakes, towns, every airstrip
 *
 * ★ The `v1` in the output path is deliberate. A renderer caches these files; a versioned path means
 *   a cached client can never be handed a dataset with a different shape from the one it parses.
 *
 * SOURCES AND THEIR LICENCES — all free, and the attribution requirement is not uniform:
 *   • Natural Earth — PUBLIC DOMAIN. Countries (110m/50m/10m), admin-1 lines, lakes, populated
 *     places. Fetched from the nvkelso/natural-earth-vector GeoJSON mirror.
 *   • GeoNames cities5000 — CC BY 4.0. ★★ REQUIRES ATTRIBUTION wherever tier2 towns are drawn:
 *     the UI must credit GeoNames (https://www.geonames.org/). This is the one source we cannot
 *     ship silently, so the licence travels with the data in index.json.
 *   • OurAirports — PUBLIC DOMAIN (dedicated by David Megginson).
 *   • NGA World Port Index (Pub 150) — PUBLIC DOMAIN (US Government work).
 *
 * ★ Everything here fails LOUDLY. A generator that shrugs and emits an empty layer is exactly how a
 *   map ships blank — the app looks broken, nobody suspects the build step. Every fetch, every
 *   header lookup and every layer count is asserted before a byte is written.
 */
import { readFile, writeFile, mkdir, stat, readdir, unlink } from 'node:fs/promises';
import { readDbf, eachPolygon } from './lib/shapefile.mjs';
import { readEtopo, buildRelief, encodePng, rasteriseBiomes, MERC_LAT } from './lib/relief.mjs';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const outDir = path.join(root, 'assets/mapdata/v1');
// ★ Gitignored scratch dir: the raw sources are ~90 MB of somebody else's release artefacts, and
//   re-running the generator must be cheap (the tier/rounding rules get tuned far more often than
//   the world changes). Nothing in here is an input to a build — only to this script.
const cacheDir = path.join(root, '.mapdata-cache');

const CHECK = process.argv.includes('--check');
const REFRESH = process.argv.includes('--refresh');

const NE = 'https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson';
const SRC = {
  countries110: `${NE}/ne_110m_admin_0_countries.geojson`,
  countries50: `${NE}/ne_50m_admin_0_countries.geojson`,
  countries10: `${NE}/ne_10m_admin_0_countries.geojson`,
  admin1: `${NE}/ne_10m_admin_1_states_provinces_lines.geojson`,
  lakes: `${NE}/ne_10m_lakes.geojson`,
  places: `${NE}/ne_10m_populated_places_simple.geojson`,
  // ★★ LANDCOVER. Stuart, 2026-09-26: "make the built up areas grey with the rural green around
  //    it ... things like the sahara could be made sand coloured". Natural Earth already ships
  //    exactly this and it is PUBLIC DOMAIN, so it costs a download and no licence obligation.
  //    ★ 50m urban for the coarse tiers: the 10m set is 27 MB and 1.15 M points, which is more
  //      than the entire rest of the basemap, for detail invisible above z8.
  urban50: `${NE}/ne_50m_urban_areas.geojson`,
  urban10: `${NE}/ne_10m_urban_areas.geojson`,
  regions: `${NE}/ne_10m_geography_regions_polys.geojson`,
  glaciers: `${NE}/ne_10m_glaciated_areas.geojson`,
  // ★★ ROADS -- 48 MB raw, 56,600 features, so this one is filtered HARD. See buildRoads().
  roads: `${NE}/ne_10m_roads.geojson`,
  // ★ Rivers, coarse lakes and coarse admin-1 so the middle zooms are not a detail cliff: without
  //   these, z5-9 had countries and nothing inside them, then everything at once at z10.
  rivers50: `${NE}/ne_50m_rivers_lake_centerlines.geojson`,
  rivers10: `${NE}/ne_10m_rivers_lake_centerlines.geojson`,
  lakes50: `${NE}/ne_50m_lakes.geojson`,
  admin150: `${NE}/ne_50m_admin_1_states_provinces_lines.geojson`,
  // ★★ The 200 m depth contour: the CONTINENTAL SHELF. One polygon set that turns a flat blue sea
  //    into somewhere with a shape -- and for an HF listener the shelf edge is a real landmark.
  shelf: `${NE}/ne_10m_bathymetry_K_200.geojson`,
  /* ★★★ HYDROLAKES, because NATURAL EARTH SIMPLY DOES NOT HAVE SMALL WATER. Measured 2026-09-26:
   *  NE's European lake supplement holds 767 lakes for the whole continent and NOT ONE within
   *  35 km of Northampton -- so Pitsford Reservoir (2.58 km², and the thing Stuart noticed was
   *  missing) could never appear, at any tier, from any Natural Earth layer. HydroLAKES carries
   *  1,427,688 lakes down to 10 ha and has it.
   *  ★★ 820 MB download, and that is FINE: it lands only on the machine running this generator,
   *  in the gitignored cache, exactly like the 48 MB of Natural Earth already there. Stuart,
   *  2026-09-26: "if you need big downloads to compose the detail into the map that is fine, as
   *  long as our map remains compact and high performance."
   *  ★★ CC BY 4.0 -- attribution required, like GeoNames. It rides in index.json. */
  hydrolakes: 'https://data.hydrosheds.org/file/hydrolakes/HydroLAKES_polys_v10_shp.zip',
  /* ★★ ETOPO2v2c -- 2 arc-minute global elevation, 73 MB zipped, PUBLIC DOMAIN (NOAA). It is a
   *  plain int16 grid with no container format, which is why it was chosen over ETOPO 2022's
   *  netCDF: no GDAL, no netCDF library, no toolchain between anyone and a map rebuild. */
  etopo: 'https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2/ETOPO2v2-2006/ETOPO2v2c/raw_binary/ETOPO2v2c_i2_LSB.zip',
  /* ★★ RESOLVE Ecoregions 2017 -- 847 ecoregions in 14 biomes, the peer-reviewed global
   *  classification (Dinerstein et al. 2017). CC BY 4.0, attribution required. It is what makes
   *  the Amazon dark and the Sahara sand, and it retires the hand-picked Natural Earth classes. */
  ecoregions: 'https://storage.googleapis.com/teow2016/Ecoregions2017.zip',
  /* ★★★ ICE SHELVES ARE A CORRECTNESS FIX, NOT A GARNISH. Without them Antarctica's SHAPE is
   *  wrong: the Ross and Ronne shelves are each about the size of France and render as open sea.
   *  ★★ MINOR ISLANDS matter for the opposite reason -- Natural Earth's country polygons drop the
   *  small ones, and for a DX map those are precisely the wrong ones to lose: Ascension, Tristan,
   *  St Helena, Rockall are what a listener is hunting.
   *  ★ Geographic lines (equator, tropics, polar circles) are 60 kB and genuinely useful on a
   *  radio map -- the greyline's behaviour changes at exactly those latitudes. */
  iceShelves: `${NE}/ne_10m_antarctic_ice_shelves_polys.geojson`,
  minorIslands: `${NE}/ne_10m_minor_islands.geojson`,
  geoLines: `${NE}/ne_110m_geographic_lines.geojson`,
  reefs: `${NE}/ne_10m_reefs.geojson`,
  playas: `${NE}/ne_10m_playas.geojson`,
  cities5000: 'https://download.geonames.org/export/dump/cities5000.zip',
  airports: 'https://davidmegginson.github.io/ourairports-data/airports.csv',
  ports: 'https://msi.nga.mil/api/publications/download?key=16920959/SFH00000/UpdatedPub150.csv&type=download',
};

function die(msg) {
  console.error(`gen-map-data: ${msg}`);
  process.exit(1);
}

/** Download to the cache unless it is already there. ★ Fails loudly — see the header note. */
async function fetchCached(key, url, { binary = false } = {}) {
  const file = path.join(cacheDir, key);
  if (!REFRESH) {
    const have = await stat(file).catch(() => null);
    if (have && have.size > 0) return binary ? readFile(file) : readFile(file, 'utf8');
  }
  process.stderr.write(`  fetching ${key} …`);
  let res;
  try {
    res = await fetch(url, { redirect: 'follow' });
  } catch (e) {
    die(`${key}: ${url} is unreachable (${e.message}). No output written.`);
  }
  if (!res.ok) die(`${key}: ${url} returned HTTP ${res.status} ${res.statusText}. No output written.`);
  const buf = Buffer.from(await res.arrayBuffer());
  if (buf.length < 1024) die(`${key}: ${url} returned only ${buf.length} bytes — the source has changed shape.`);
  await writeFile(file, buf);
  process.stderr.write(` ${(buf.length / 1048576).toFixed(1)} MB\n`);
  return binary ? buf : buf.toString('utf8');
}

async function fetchGeoJson(key) {
  const text = await fetchCached(`${key}.geojson`, SRC[key]);
  let gj;
  try {
    gj = JSON.parse(text);
  } catch (e) {
    die(`${key}: not valid JSON (${e.message}) — delete .mapdata-cache/${key}.geojson and re-run with --refresh.`);
  }
  if (!gj || !Array.isArray(gj.features) || gj.features.length === 0) {
    die(`${key}: no features — the Natural Earth layer has changed shape.`);
  }
  return gj;
}

/* ───────────────────────── packing ───────────────────────── */

// ★ 2 dp ≈ 1 km, 3 dp ≈ 110 m. Coarse tiers never render at a zoom where 1 km is visible, so the
//   extra digit is pure weight — and weight here is POINTS TO LAY OUT, not just bytes.
const round = (v, dp) => Number(v.toFixed(dp));

/**
 * Round a ring/line and drop the consecutive duplicates that rounding creates — two points 200 m
 * apart become the same point at 2 dp, and a run of them is a stall in the renderer for no pixels.
 * Returns null for a ring left too short to enclose anything (< 4 points, i.e. no closed triangle).
 */
function packRing(coords, dp, { closed }) {
  const out = [];
  for (const c of coords) {
    const x = round(c[0], dp);
    const y = round(c[1], dp);
    if (!Number.isFinite(x) || !Number.isFinite(y)) continue;
    const last = out[out.length - 1];
    if (last && last[0] === x && last[1] === y) continue;
    out.push([x, y]);
  }
  if (closed) {
    if (out.length < 4) return null;
  } else if (out.length < 2) return null;
  return out;
}

/** Every outer+inner ring of a Polygon/MultiPolygon, packed. Holes are kept: lakes ARE holes. */
function packPolygons(geom, dp) {
  if (!geom) return [];
  const polys = geom.type === 'Polygon' ? [geom.coordinates]
    : geom.type === 'MultiPolygon' ? geom.coordinates
      : null;
  if (!polys) return [];
  const rings = [];
  for (const poly of polys) {
    for (const ring of poly) {
      const r = packRing(ring, dp, { closed: true });
      if (r) rings.push(r);
    }
  }
  return rings;
}

function packLines(geom, dp) {
  if (!geom) return [];
  const lines = geom.type === 'LineString' ? [geom.coordinates]
    : geom.type === 'MultiLineString' ? geom.coordinates
      : null;
  if (!lines) return [];
  const out = [];
  for (const l of lines) {
    const p = packRing(l, dp, { closed: false });
    if (p) out.push(p);
  }
  return out;
}

/* ───────────────────────── layers ───────────────────────── */

/**
 * Landcover: the polygons that let the basemap stop being one flat green.
 *
 * ★★★ THIS IS A LOOK, NOT A DATASET, so it is deliberately COARSE. We are not classifying land
 *   use -- we are giving the eye the three cues that make a map read as a map: a town is grey, a
 *   desert is sand, ice is white. Natural Earth's own `FEATURECLA` is the whole classifier.
 *
 * ★★ ONLY THE CLASSES THAT CHANGE THE COLOUR ARE KEPT. `ne_10m_geography_regions_polys` also
 *   carries Island, Range/mtn, Plateau, Coast, Pen/cape and 1,047 features in total -- painting
 *   those would colour half the planet for no information. Desert, Tundra and Wetlands are the
 *   three that mean "the ground here does not look like grass", and Desert is the one he asked
 *   for by name.
 *
 * ✗ Do not reach for a raster landcover set (MODIS, Copernicus). It would be a hundred times the
 *   size, need a projection, and could not be restyled to the app's palette -- which is the entire
 *   reason we left raster tiles behind.
 */
/* ★★★ THIS SET IS NOW EMPTY, AND THAT IS THE POINT. It once held Desert, Tundra and Wetlands --
 *  58, 4 and 3 Natural Earth polygons picked because they looked like the right idea. RESOLVE
 *  Ecoregions covers all land in 14 peer-reviewed biomes and is rasterised into the relief image,
 *  so these would now sit ON TOP of a better answer and contradict it.
 *  ★★ 'Range/mtn' was here too, briefly, and was the lesson: NE's mountain polygons are envelopes
 *  drawn around a range so a LABEL can be placed on it, not the extent of high ground -- one of
 *  them covers Belgium. ✗ Do not restore any of these from this file.
 *  ★ Glaciers stay as VECTORS below, because an ice sheet has a hard edge worth keeping crisp at
 *  every zoom, and it must still be drawn when the relief image is hidden. */
const COVER_CLASSES = new Set();

/**
 * Shoelace area in square degrees. ★ Not a real area -- it is stretched by latitude and means
 * nothing near the poles. It does not have to be right: it only has to rank a polygon against
 * the pixel it would occupy, and for "is this speck worth drawing at z2" that is enough.
 */
function ringArea(ring) {
  let a = 0;
  for (let i = 0, j = ring.length - 1; i < ring.length; j = i++) {
    a += ring[j][0] * ring[i][1] - ring[i][0] * ring[j][1];
  }
  return Math.abs(a) / 2;
}

/* ★★★ THE COARSE TIERS DROP THE SPECKS, and without this the landcover is the HEAVIEST thing in
 *  the whole basemap: 10m glaciated areas is every named snowfield on Earth, 215,263 points, 3.1 MB
 *  -- at z2, where Greenland is 40 pixels wide. tier0 keeps only what a world view can resolve. */
function bigEnough(rings, minArea) {
  return minArea > 0 ? rings.filter((r) => ringArea(r) >= minArea) : rings;
}

function buildCover(regionsGj, glaciersGj, dp, minArea = 0, shelvesGj = null) {
  const out = { ice: [] };
  const bucket = {};
  for (const f of regionsGj.features) {
    const cla = (f.properties || {}).FEATURECLA;
    if (!COVER_CLASSES.has(cla)) continue;
    const rings = bigEnough(packPolygons(f.geometry, dp), minArea);
    if (rings.length) out[bucket[cla]].push(...rings);
  }
  for (const f of glaciersGj.features) {
    const rings = bigEnough(packPolygons(f.geometry, dp), minArea);
    if (rings.length) out.ice.push(...rings);
  }
  /* ★ Antarctic ice shelves go in the SAME bucket as glaciers: to a reader they are the same
   *  thing -- permanent ice -- and separating them would only mean two identical draw calls. */
  for (const f of (shelvesGj?.features || [])) {
    const rings = bigEnough(packPolygons(f.geometry, dp), minArea);
    if (rings.length) out.ice.push(...rings);
  }
  // ★ Fail loudly: an empty `ice` means the glacier layer moved and Greenland ships green.
  if (!out.ice.length) die('cover/ice: no glacier polygons — the Natural Earth layer has changed.');
  return out;
}

/**
 * Roads -> flat polyline list, thinned by Natural Earth's own `scalerank`.
 *
 * ★★★ ROADS ARE DECORATION HERE AND ARE BUDGETED AS SUCH. An airport on this map is INFORMATION --
 *   somebody picks a receiver by it. A motorway is not: nobody tunes a radio by the M1. It earns
 *   its place only by making the map read as a map, which is precisely what Stuart asked for
 *   ("fake the experience of a real map that little bit more"), so it gets the cheapest slice of
 *   the source that achieves that and nothing more.
 *
 * ★★ WHICH IS WHY THE FILTER IS `scalerank`, NOT `type`. 25,766 of the 56,600 features are typed
 *   "Unknown" -- almost all of them in Asia, and most of them real trunk roads. Filtering by type
 *   would quietly delete the road network of the largest continent while looking like a sensible
 *   rule. scalerank is Natural Earth's own judgement of what belongs at what zoom and it covers
 *   every feature.
 *
 * ✗ Ferry routes are dropped at every tier: a line across open sea reads as a coastline error.
 */
/** Rivers -> polylines, thinned by scalerank exactly as the roads are. */
function buildRivers(gj, dp, maxScalerank) {
  const out = [];
  for (const f of gj.features) {
    const r = (f.properties || {}).scalerank ?? (f.properties || {}).SCALERANK;
    if (typeof r !== 'number' || r > maxScalerank) continue;
    out.push(...packLines(f.geometry, dp));
  }
  if (!out.length) die(`rivers: nothing survived scalerank <= ${maxScalerank}.`);
  return out;
}

function buildRoads(gj, dp, maxScalerank) {
  const out = [];
  for (const f of gj.features) {
    const p = f.properties || {};
    const rank = p.scalerank ?? p.SCALERANK;
    if (typeof rank !== 'number' || rank > maxScalerank) continue;
    if ((p.type || p.TYPE) === 'Ferry Route') continue;
    out.push(...packLines(f.geometry, dp));
  }
  if (!out.length) die(`roads: nothing survived scalerank <= ${maxScalerank}.`);
  return out;
}

/**
 * HydroLAKES -> flat ring list, for the ONE tier that can carry it.
 *
 * ★★★ THE RAW POLYGONS ARE UNUSABLE AS THEY STAND. They are traced from a 15-arcsec raster, so
 *   every shoreline is a staircase: lakes >= 1 km² come to 26.7 MILLION points and 430 MB of JSON.
 *   Rounding does not touch it -- the staircase steps are 0.00417 deg apart, which survives 3 dp.
 *   Douglas-Peucker at 0.004 deg (~400 m) takes the same lakes to 2.6 M points and 38.5 MB raw,
 *   9.5 MB gzipped, with no visible change at any zoom this map reaches.
 *
 * ★★ ONLY THE LARGEST RING PER LAKE. HydroLAKES encodes islands within a lake as further rings;
 *   at these zooms an island in a reservoir is sub-pixel, and keeping them buys nothing.
 *
 * ★ `shouldRead` is passed to the .shp reader so the 96 % of lakes below the threshold are never
 *   decoded at all -- the attribute table is read first precisely so the geometry pass can skip.
 */
function buildHydroLakes(dbfPath, shpPath, dp, minAreaKm2, tol) {
  const rows = readDbf(dbfPath, ['Lake_area']);
  if (rows.length < 1e6) die(`hydrolakes: only ${rows.length} rows — the dataset has changed shape.`);
  const want = rows.map((r) => r.Lake_area >= minAreaKm2);
  const out = [];
  eachPolygon(shpPath, (i, rings) => {
    /* ★★★ THE TOLERANCE SCALES WITH THE LAKE. A flat 400 m tolerance is right for Lake Superior
     *  and wrong for a 2.5 km² reservoir: it reduced Pitsford to a TEN-POINT BLOB. The whole
     *  reason small water is in this dataset is that somebody recognises the shape of the one
     *  near their house, so a small lake gets a proportionally finer tolerance.
     *  ★ It costs almost nothing: a small lake has few vertices to begin with, and the big lakes
     *  that dominate the byte count keep the coarse tolerance. */
    const area = rows[i].Lake_area;
    // ★ Clamped to [0.3, 1] x tol: a small lake gets ~120 m, a big one keeps ~400 m. Unclamped
    //   sqrt scaling took the layer to 125 MB -- the floor is what keeps this affordable.
    const t = tol * Math.min(1, Math.max(0.3, Math.sqrt(area / 300)));
    let best = null;
    for (const ring of rings) {
      const packed = packRing(simplifyRing(ring, t), dp, { closed: true });
      if (packed && (!best || packed.length > best.length)) best = packed;
    }
    if (best) out.push(best);
  }, (i) => want[i]);
  if (out.length < 1000) die(`hydrolakes: only ${out.length} lakes survived >= ${minAreaKm2} km².`);
  return out;
}

/**
 * Douglas-Peucker, ITERATIVE. ★ The textbook recursion blows the stack on a 20,000-point shoreline,
 * and it does so as a RangeError halfway through a 1.1 GB read — hours in, with nothing written.
 */
function simplifyRing(pts, tol) {
  if (pts.length < 4 || !tol) return pts;
  const keep = new Uint8Array(pts.length);
  keep[0] = keep[pts.length - 1] = 1;
  const stack = [[0, pts.length - 1]];
  const t2 = tol * tol;
  while (stack.length) {
    const [a, b] = stack.pop();
    if (b - a < 2) continue;
    const [ax, ay] = pts[a]; const [bx, by] = pts[b];
    const dx = bx - ax; const dy = by - ay; const den = dx * dx + dy * dy;
    let far = -1; let fd = 0;
    for (let i = a + 1; i < b; i++) {
      const [px, py] = pts[i];
      let d;
      if (den === 0) { const ex = px - ax; const ey = py - ay; d = ex * ex + ey * ey; } else {
        let t = ((px - ax) * dx + (py - ay) * dy) / den;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        const ex = px - (ax + t * dx); const ey = py - (ay + t * dy);
        d = ex * ex + ey * ey;
      }
      if (d > fd) { fd = d; far = i; }
    }
    if (fd > t2 && far > 0) { keep[far] = 1; stack.push([a, far], [far, b]); }
  }
  const out = [];
  for (let i = 0; i < pts.length; i++) if (keep[i]) out.push(pts[i]);
  return out;
}

/** Urban extents -> flat ring list. `scalerank` thins them: 0 is a metropolis, 8 a small town. */
function buildUrban(gj, dp, maxScalerank, minArea = 0) {
  const out = [];
  for (const f of gj.features) {
    const p = f.properties || {};
    const rank = p.scalerank ?? p.SCALERANK ?? 0;
    if (typeof rank === 'number' && rank > maxScalerank) continue;
    const rings = bigEnough(packPolygons(f.geometry, dp), minArea);
    if (rings.length) out.push(...rings);
  }
  if (!out.length) die(`urban: nothing survived scalerank <= ${maxScalerank}.`);
  return out;
}

/**
 * Countries, keyed by ISO_A2.
 * ★★ ISO_A2_EH, not ISO_A2. Natural Earth carries -99 in ISO_A2 for France, Norway and Kosovo
 *    (a sovereignty encoding, not a gap in the standard), so keying on it silently drops three
 *    countries — including one of the biggest FM-DX targets in Europe. src/services/countryBounds.ts
 *    already documents this; same trap, same fix, so the two datasets agree.
 */
function buildCountries(gj, dp, label) {
  const out = {};
  let skipped = 0;
  for (const f of gj.features) {
    const p = f.properties || {};
    const iso = p.ISO_A2_EH ?? p.iso_a2_eh;
    if (iso === undefined) die(`${label}: no ISO_A2_EH property — Natural Earth has renamed its fields.`);
    if (typeof iso !== 'string' || iso.length !== 2 || iso === '-9') { skipped++; continue; }
    const rings = packPolygons(f.geometry, dp);
    if (!rings.length) { skipped++; continue; }
    // ★ A few sovereignties appear as more than one feature; merge rather than overwrite, or the
    //   second feature erases the first and a country loses half its coastline.
    (out[iso] ||= []).push(...rings);
  }
  const n = Object.keys(out).length;
  if (n < 150) die(`${label}: only ${n} countries keyed (expected 200+) — source shape changed.`);
  if (skipped > gj.features.length / 2) die(`${label}: skipped ${skipped}/${gj.features.length} features.`);
  return out;
}

/** Natural Earth populated places -> [name, lon, lat, rank]; rank is NE's scalerank (0 = biggest). */
function buildNePlaces(gj, dp, maxScalerank) {
  const out = [];
  for (const f of gj.features) {
    const p = f.properties || {};
    const name = p.name ?? p.NAME;
    const rank = p.scalerank ?? p.SCALERANK;
    if (name === undefined || rank === undefined) {
      die('places: no name/scalerank property — ne_10m_populated_places_simple has changed shape.');
    }
    if (typeof rank !== 'number' || rank > maxScalerank) continue;
    const g = f.geometry;
    if (!g || g.type !== 'Point') continue;
    out.push([String(name), round(g.coordinates[0], dp), round(g.coordinates[1], dp), rank]);
  }
  if (!out.length) die(`places: nothing survived scalerank <= ${maxScalerank}.`);
  return out;
}

/* ── CSV ──
 * ★ A hand-rolled reader rather than a dependency: two files, both RFC-4180-plain apart from quoted
 *   commas, and this script must run on a clean checkout with no install step. */
function parseCsv(text) {
  const rows = [];
  let row = [];
  let field = '';
  let quoted = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (quoted) {
      if (c === '"') {
        if (text[i + 1] === '"') { field += '"'; i++; } else quoted = false;
      } else field += c;
      continue;
    }
    if (c === '"') { quoted = true; continue; }
    if (c === ',') { row.push(field); field = ''; continue; }
    if (c === '\r') continue;
    if (c === '\n') { row.push(field); field = ''; rows.push(row); row = []; continue; }
    field += c;
  }
  if (field.length || row.length) { row.push(field); rows.push(row); }
  return rows;
}

/** Find a header column by any of several spellings; ★ dies rather than guess — see header note. */
function col(header, names, label) {
  const lower = header.map((h) => h.trim().toLowerCase());
  for (const n of names) {
    const i = lower.indexOf(n.toLowerCase());
    if (i >= 0) return i;
  }
  die(`${label}: no column matching ${names.join(' / ')} — source has changed shape. Header was: ${header.join(',')}`);
}

/**
 * OurAirports -> [icao, lon, lat, tier, iata, name].
 * tier: 0 large · 1 medium · 2 small · 3 heliport · 4 seaplane base.
 * ★ gps_code first, ident second: `ident` is OurAirports' own key and for small strips is often a
 *   local code that no radio log will ever carry, whereas gps_code is the four-letter code an HFDL
 *   or ACARS message actually names. A row with neither, or with unparseable coordinates, is
 *   dropped — a marker at (0,0) in the Gulf of Guinea is worse than a missing one.
 */
const AIRPORT_TIER = { large_airport: 0, medium_airport: 1, small_airport: 2, heliport: 3, seaplane_base: 4 };
function buildAirports(csv, dp, allowedTiers) {
  const rows = parseCsv(csv);
  if (rows.length < 1000) die(`airports: only ${rows.length} rows — source has changed shape.`);
  const h = rows[0];
  const iType = col(h, ['type'], 'airports');
  const iName = col(h, ['name'], 'airports');
  const iLat = col(h, ['latitude_deg'], 'airports');
  const iLon = col(h, ['longitude_deg'], 'airports');
  const iGps = col(h, ['gps_code'], 'airports');
  const iIdent = col(h, ['ident'], 'airports');
  const iIata = col(h, ['iata_code'], 'airports');
  const out = [];
  for (let r = 1; r < rows.length; r++) {
    const row = rows[r];
    if (row.length <= iLon) continue;
    const tier = AIRPORT_TIER[row[iType].trim()];
    if (tier === undefined || !allowedTiers.has(tier)) continue;
    const code = (row[iGps] || '').trim() || (row[iIdent] || '').trim();
    if (!code) continue;
    const lat = Number(row[iLat]);
    const lon = Number(row[iLon]);
    if (!Number.isFinite(lat) || !Number.isFinite(lon)) continue;
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) continue;
    out.push([code, round(lon, dp), round(lat, dp), tier, (row[iIata] || '').trim(), (row[iName] || '').trim()]);
  }
  if (!out.length) die(`airports: nothing survived the tier filter ${[...allowedTiers].join(',')}.`);
  return out;
}

/** NGA World Port Index -> [name, lon, lat, countryCode]. */
function buildPorts(csv, dp) {
  const rows = parseCsv(csv);
  if (rows.length < 500) die(`ports: only ${rows.length} rows — source has changed shape.`);
  const h = rows[0];
  const iName = col(h, ['Main Port Name', 'portName', 'Port Name', 'Main Port Name '], 'ports');
  const iLat = col(h, ['Latitude', 'latitude'], 'ports');
  const iLon = col(h, ['Longitude', 'longitude'], 'ports');
  const iCc = col(h, ['Country Code', 'countryCode', 'Country'], 'ports');
  const out = [];
  for (let r = 1; r < rows.length; r++) {
    const row = rows[r];
    if (row.length <= Math.max(iName, iLat, iLon, iCc)) continue;
    const lat = Number(row[iLat]);
    const lon = Number(row[iLon]);
    const name = (row[iName] || '').trim();
    if (!name || !Number.isFinite(lat) || !Number.isFinite(lon)) continue;
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) continue;
    out.push([name, round(lon, dp), round(lat, dp), (row[iCc] || '').trim().slice(0, 2)]);
  }
  if (!out.length) die('ports: no usable rows.');
  return out;
}

/**
 * GeoNames cities5000 -> [name, lon, lat, rank].
 * ★ `unzip` via child_process rather than an npm zip library: one archive, one member, and the tool
 *   is present on every machine that builds this repo (macOS and Debian both ship it). Adding a
 *   dependency so a build script can open one zip is not a trade worth making.
 * ★ rank is SYNTHESISED from population so it can be compared with Natural Earth's scalerank in the
 *   same renderer: 0 = >5M, 1 = >1M, 2 = >500k, 3 = >100k, … 8 = the rest. NE's scalerank runs the
 *   same direction (small = important), so one label-priority rule serves both layers.
 */
function geonamesRank(pop) {
  if (pop > 5e6) return 0;
  if (pop > 1e6) return 1;
  if (pop > 5e5) return 2;
  if (pop > 1e5) return 3;
  if (pop > 5e4) return 4;
  if (pop > 2e4) return 5;
  if (pop > 1e4) return 6;
  if (pop > 5e3) return 7;
  return 8;
}

async function buildGeonames(dp) {
  const zip = path.join(cacheDir, 'cities5000.zip');
  await fetchCached('cities5000.zip', SRC.cities5000, { binary: true });
  let text;
  try {
    // -p writes the member to stdout, so nothing is unpacked onto disk. 64 MB is ample headroom.
    text = execFileSync('unzip', ['-p', zip, 'cities5000.txt'], { maxBuffer: 64 * 1024 * 1024 }).toString('utf8');
  } catch (e) {
    die(`cities5000: unzip failed (${e.message}). Is \`unzip\` on PATH, and is the cached zip intact? Try --refresh.`);
  }
  const out = [];
  let bad = 0;
  for (const line of text.split('\n')) {
    if (!line) continue;
    // GeoNames dump is TAB separated: 1 name, 4 lat, 5 lon, 14 population (1-based, per readme.txt).
    const f = line.split('\t');
    if (f.length < 15) { bad++; continue; }
    const name = f[1].trim();
    const lat = Number(f[4]);
    const lon = Number(f[5]);
    const pop = Number(f[14]) || 0;
    if (!name || !Number.isFinite(lat) || !Number.isFinite(lon)) { bad++; continue; }
    out.push([name, round(lon, dp), round(lat, dp), geonamesRank(pop)]);
  }
  if (out.length < 10000) die(`cities5000: only ${out.length} towns parsed (expected ~50k) — the dump has changed shape.`);
  if (bad > out.length / 10) die(`cities5000: ${bad} unparseable lines — field order has changed.`);
  return out;
}

/* ───────────────────────── counting, for the summary ───────────────────────── */

function countPoints(v) {
  if (Array.isArray(v)) {
    // A packed point is [x, y] (or [name, x, y, rank]) — a leaf, worth 1.
    if (typeof v[0] === 'number' && typeof v[1] === 'number' && v.every((e) => !Array.isArray(e))) return 1;
    if (typeof v[0] === 'string') return 1;
    let n = 0;
    for (const e of v) n += countPoints(e);
    return n;
  }
  if (v && typeof v === 'object') {
    let n = 0;
    for (const e of Object.values(v)) n += countPoints(e);
    return n;
  }
  return 0;
}

const featureCount = (v) => (Array.isArray(v) ? v.length : Object.keys(v).length);

/* ───────────────────────── main ───────────────────────── */

await mkdir(cacheDir, { recursive: true });
process.stderr.write('gen-map-data: sources\n');

const [c110, c50, c10, admin1, lakes, places, urban50, urban10, regions, glaciers, roads,
       rivers50, rivers10, lakes50, admin150, shelf,
       iceShelves, minorIslands, geoLines, reefs, playas] = await Promise.all([
  fetchGeoJson('countries110'),
  fetchGeoJson('countries50'),
  fetchGeoJson('countries10'),
  fetchGeoJson('admin1'),
  fetchGeoJson('lakes'),
  fetchGeoJson('places'),
  fetchGeoJson('urban50'),
  fetchGeoJson('urban10'),
  fetchGeoJson('regions'),
  fetchGeoJson('glaciers'),
  fetchGeoJson('roads'),
  fetchGeoJson('rivers50'),
  fetchGeoJson('rivers10'),
  fetchGeoJson('lakes50'),
  fetchGeoJson('admin150'),
  fetchGeoJson('shelf'),
  fetchGeoJson('iceShelves'),
  fetchGeoJson('minorIslands'),
  fetchGeoJson('geoLines'),
  fetchGeoJson('reefs'),
  fetchGeoJson('playas'),
]);
const airportsCsv = await fetchCached('airports.csv', SRC.airports);
const portsCsv = await fetchCached('ports.csv', SRC.ports);
const towns = await buildGeonames(3);

// ★ Unpacked once into the cache; ~1.5 GB of shapefile that must never reach the repo or a build.
const hydroDir = path.join(cacheDir, 'hydrolakes');
const hydroShp = path.join(hydroDir, 'HydroLAKES_polys_v10.shp');
if (!(await stat(hydroShp).catch(() => null))) {
  await fetchCached('HydroLAKES_polys_v10_shp.zip', SRC.hydrolakes, { binary: true });
  process.stderr.write('  unpacking HydroLAKES …');
  await mkdir(hydroDir, { recursive: true });
  try {
    execFileSync('unzip', ['-o', '-j', path.join(cacheDir, 'HydroLAKES_polys_v10_shp.zip'),
      '*/HydroLAKES_polys_v10.dbf', '*/HydroLAKES_polys_v10.shp', '-d', hydroDir], { stdio: 'pipe' });
  } catch (e) {
    die(`hydrolakes: unzip failed (${e.message}).`);
  }
  process.stderr.write(' done\n');
}
const hydroDbf = path.join(hydroDir, 'HydroLAKES_polys_v10.dbf');

/* ★★★ SHADED RELIEF -- the one raster in a vector basemap, and deliberately so. Stuart, 2026-09-26:
 *  "I would like the map to look like a realistic representation of the world just at a more coarse
 *  detail level for performance and space saving." Terrain IS a continuous field; the attempt to
 *  express it as polygons is what produced the mountain blobs that were pulled the same afternoon.
 *  ★ It is BUNDLED, not fetched, so it carries none of the failure modes that made us drop tiles. */
const etopoDir = path.join(cacheDir, 'etopo');
const etopoBin = path.join(etopoDir, 'ETOPO2v2c_i2_LSB.bin');
if (!(await stat(etopoBin).catch(() => null))) {
  await fetchCached('ETOPO2v2c_i2_LSB.zip', SRC.etopo, { binary: true });
  process.stderr.write('  unpacking ETOPO2 …');
  await mkdir(etopoDir, { recursive: true });
  try {
    execFileSync('unzip', ['-o', path.join(cacheDir, 'ETOPO2v2c_i2_LSB.zip'), '-d', etopoDir], { stdio: 'pipe' });
  } catch (e) { die(`etopo: unzip failed (${e.message}).`); }
  process.stderr.write(' done\n');
}
const etopo = readEtopo(etopoBin);

/* ★★★ BIOMES ARE PAINTED INTO THE RELIEF, NOT SHIPPED AS POLYGONS. Ecoregions2017 is a 243 MB
 *  shapefile covering every acre of land -- as vectors it would dwarf the whole basemap. Like
 *  elevation it is a FIELD, and a field belongs in the grid we are already writing. */
const ecoDir = path.join(cacheDir, 'ecoregions');
const ecoShp = path.join(ecoDir, 'Ecoregions2017.shp');
if (!(await stat(ecoShp).catch(() => null))) {
  await fetchCached('Ecoregions2017.zip', SRC.ecoregions, { binary: true });
  process.stderr.write('  unpacking Ecoregions2017 …');
  await mkdir(ecoDir, { recursive: true });
  try {
    execFileSync('unzip', ['-o', '-j', path.join(cacheDir, 'Ecoregions2017.zip'), '-d', ecoDir], { stdio: 'pipe' });
  } catch (e) { die(`ecoregions: unzip failed (${e.message}).`); }
  process.stderr.write(' done\n');
}
const ecoRows = readDbf(path.join(ecoDir, 'Ecoregions2017.dbf'), ['BIOME_NUM']);
if (ecoRows.length < 500) die(`ecoregions: only ${ecoRows.length} records — the dataset has changed shape.`);
const ecoShapes = [];
eachPolygon(ecoShp, (i, rings) => { ecoShapes.push({ biome: ecoRows[i].BIOME_NUM, rings }); });
if (ecoShapes.length < 500) die(`ecoregions: only ${ecoShapes.length} polygons read.`);

process.stderr.write(`  rasterising ${ecoShapes.length} ecoregions …`);
const reliefImages = [
  ['relief.png', 'basic', buildRelief(etopo, { width: 2700, biomes: rasteriseBiomes(ecoShapes, 2700) })],
  ['relief-hi.png', 'detail', buildRelief(etopo, { width: 4096, biomes: rasteriseBiomes(ecoShapes, 4096) })],
];
process.stderr.write(' done\n');

// tier0 world z0–4 · tier1 regional z5–7 · tier2 local z8+
const layers = [
  // tier0 — only what is legible at z0–4. A small_airport dot at z2 is a pixel of noise.
  ['tier0', 'countries', buildCountries(c110, 2, 'countries110')],
  ['tier0', 'places', buildNePlaces(places, 2, 4)],
  ['tier0', 'airports', buildAirports(airportsCsv, 2, new Set([0]))],
  ['tier0', 'cover', buildCover(regions, glaciers, 1, 1.0, iceShelves)],
  ['tier0', 'urban', buildUrban(urban50, 2, 2, 0.05)],
  ['tier0', 'rivers', buildRivers(rivers50, 2, 3)],
  /* ★ Equator, tropics and polar circles. One tier only: they are the same lines at every zoom,
   *  and arithmetic-exact, so a second copy would be a second thing to keep in step. */
  ['tier0', 'geolines', geoLines.features.map((f) => ({
    name: String(f.properties?.name || f.properties?.NAME || ''),
    lines: packLines(f.geometry, 2),
  })).filter((g) => g.lines.length)],
  ['tier0', 'islands', minorIslands.features.flatMap((f) => packPolygons(f.geometry, 2))],
  ['tier0', 'lakes', bigEnough(lakes50.features.flatMap((f) => packPolygons(f.geometry, 2)), 0.5)],
  // tier1
  ['tier1', 'countries', buildCountries(c50, 2, 'countries50')],
  ['tier1', 'places', buildNePlaces(places, 2, Infinity)],
  ['tier1', 'airports', buildAirports(airportsCsv, 2, new Set([0, 1]))],
  ['tier1', 'ports', buildPorts(portsCsv, 2)],
  ['tier1', 'cover', buildCover(regions, glaciers, 2, 0.05, iceShelves)],
  ['tier1', 'urban', buildUrban(urban50, 2, Infinity, 0.002)],
  ['tier1', 'roads', buildRoads(roads, 2, 4)],
  ['tier1', 'rivers', buildRivers(rivers50, 2, Infinity)],
  ['tier1', 'lakes', lakes50.features.flatMap((f) => packPolygons(f.geometry, 2))],
  ['tier1', 'admin1', admin150.features.flatMap((f) => packLines(f.geometry, 2))],
  ['tier1', 'shelf', shelf.features.flatMap((f) => packPolygons(f.geometry, 2))],
  ['tier1', 'islands', minorIslands.features.flatMap((f) => packPolygons(f.geometry, 2))],
  ['tier1', 'reefs', reefs.features.flatMap((f) => packLines(f.geometry, 2))],
  ['tier1', 'playas', playas.features.flatMap((f) => packPolygons(f.geometry, 2))],
  // tier2 — the heavy tier, and the reason for tiering: it is only ever loaded at z8+, where the
  // viewport is a few hundred km across and the renderer culls almost all of it.
  ['tier2', 'countries', buildCountries(c10, 3, 'countries10')],
  ['tier2', 'admin1', admin1.features.flatMap((f) => packLines(f.geometry, 3))],
  // ★ HydroLAKES REPLACES Natural Earth at tier2: it is a strict superset (1.43 M vs 1,590) and
  //   carrying both would draw every large lake twice.
  ['tier2', 'lakes', buildHydroLakes(hydroDbf, hydroShp, 3, 1, 0.004)],
  ['tier2', 'places', towns],
  ['tier2', 'airports', buildAirports(airportsCsv, 3, new Set([0, 1, 2, 3, 4]))],
  ['tier2', 'cover', buildCover(regions, glaciers, 3, 0, iceShelves)],
  ['tier2', 'urban', buildUrban(urban10, 3, Infinity)],
  ['tier2', 'roads', buildRoads(roads, 3, 8)],
  ['tier2', 'rivers', buildRivers(rivers10, 3, Infinity)],
  ['tier2', 'shelf', shelf.features.flatMap((f) => packPolygons(f.geometry, 3))],
  ['tier2', 'islands', minorIslands.features.flatMap((f) => packPolygons(f.geometry, 3))],
  ['tier2', 'reefs', reefs.features.flatMap((f) => packLines(f.geometry, 3))],
  ['tier2', 'playas', playas.features.flatMap((f) => packPolygons(f.geometry, 3))],
];

for (const [tier, name, data] of layers) {
  if (!data || featureCount(data) === 0) die(`${tier}/${name} is EMPTY — refusing to write a blank map.`);
}

const index = {
  version: 1,
  generated: new Date().toISOString().slice(0, 10),
  // ★ The licences ride WITH the data. GeoNames is CC BY 4.0 and the credit is not optional; any
  //   renderer that loads tier2 places must show it, and it cannot show what it was never told.
  licences: {
    resolve: { layers: ['relief (biome colouring)'], licence: 'CC BY 4.0 — ATTRIBUTION REQUIRED', attribution: '© RESOLVE Ecoregions 2017', url: 'https://ecoregions.appspot.com/' },
    'noaa-etopo': { layers: ['relief'], licence: 'Public domain (US Government)', url: 'https://www.ncei.noaa.gov/products/etopo-global-relief-model' },
    'natural-earth': { layers: ['countries', 'admin1', 'lakes', 'places(tier0,tier1)', 'urban', 'cover', 'roads', 'rivers', 'shelf', 'islands', 'reefs', 'playas', 'geolines'], licence: 'Public domain', url: 'https://www.naturalearthdata.com/' },
    geonames: { layers: ['places(tier2)'], licence: 'CC BY 4.0 — ATTRIBUTION REQUIRED', attribution: '© GeoNames', url: 'https://www.geonames.org/' },
    ourairports: { layers: ['airports'], licence: 'Public domain', url: 'https://ourairports.com/data/' },
    hydrolakes: { layers: ['lakes(tier2)'], licence: 'CC BY 4.0 — ATTRIBUTION REQUIRED', attribution: '© HydroLAKES / HydroSHEDS', url: 'https://www.hydrosheds.org/products/hydrolakes' },
    'nga-wpi': { layers: ['ports'], licence: 'Public domain (US Government)', url: 'https://msi.nga.mil/Publications/WPI' },
  },
  zoom: { tier0: [0, 4], tier1: [5, 7], tier2: [8, 22] },
  /* ★ The renderer needs the CUT-OFF LATITUDE to place the image, and it must come from the same
   *  constant that generated it -- a renderer that hardcodes 85 instead of 85.0511 slides the
   *  relief a few kilometres off the coastline at high latitude. */
  relief: { mercatorLat: MERC_LAT, basic: 'relief.png', detail: 'relief-hi.png' },
  /* ★★★ TWO PACKS, AND THE RENDERER MUST WORK WITH ONLY THE FIRST. Stuart, 2026-09-26: "ship the
   *  VibeServer with basic maps and give the server owner the option of a one time download of
   *  the more detailed level 2 maps. same with the app too."
   *
   *  basic  = tier0 + tier1. Bundled in EVERYTHING, always present, never fetched. The whole world
   *           is drawn correctly from it -- coastlines, countries, towns, airports, ports, roads,
   *           landcover. It is what a Pi Zero in captive-hotspot mode has, and it is complete.
   *  detail = tier2. Fine 10m coastlines, admin-1 lines, lakes, 69,753 GeoNames towns, every
   *           airstrip and heliport, and the dense road network. Optional, one download, kept
   *           on disk beside the basic pack.
   *
   *  ★★★ `detail` IS AN ENHANCEMENT, NEVER A REQUIREMENT. Any renderer that reads this index must
   *   treat a missing tier2 as normal and fall back to tier1 SILENTLY -- no error, no empty layer,
   *   no black map. A map that looks broken without an optional download is not an optional
   *   download. [[never_limit_permanently]] -- and the inverse: never let an absent extra READ as
   *   a fault. The renderer asks the index what it has; it does not assume.
   */
  packs: { basic: { tiers: ['tier0', 'tier1'], files: [], bytes: 0 },
           detail: { tiers: ['tier2'], files: [], bytes: 0, optional: true } },
  files: {},
};

/* ───────────────────────── sharding ─────────────────────────
 * ★★★ A HEAVY LAYER IS SPLIT INTO GEOGRAPHIC SHARDS, and this is not only about Cloudflare's
 *   25 MiB asset ceiling (which tier2-lakes broke at 38.5 MiB). It is the same principle as the
 *   tiers: DO NOT MAKE SOMEBODY DOWNLOAD THE WORLD TO LOOK AT THEIR OWN TOWN. Sharded, a user
 *   zooming to Northampton fetches the one shard containing Britain, not 184,869 lakes.
 *
 * ★★ EACH SHARD RECORDS ITS OWN DATA BBOX, not the grid cell it came from. A ring is assigned by
 *   its centre, so it can overhang the cell; the renderer intersects against the RECORDED bbox and
 *   is therefore exact. Assigning by cell and testing against the cell would clip features at
 *   every shard seam — a class of bug that looks like missing data and is very hard to see.
 *
 * ✗ Shard only ARRAYS. The country layer is keyed by ISO code and splitting it would mean a
 *   country could exist in two shards under one key.
 */
const SHARD_LIMIT = 6 * 1024 * 1024;
const SHARD_MAX_DEPTH = 6;

/** bbox of a ring, a polyline, or a single [name, lon, lat, …] record. */
function itemBox(item) {
  if (typeof item[0] === 'string') return [item[1], item[2], item[1], item[2]];
  let w = 180; let s = 90; let e = -180; let n = -90;
  for (const [lon, lat] of item) {
    if (lon < w) w = lon; if (lon > e) e = lon;
    if (lat < s) s = lat; if (lat > n) n = lat;
  }
  return [w, s, e, n];
}

/**
 * ★★★ SHARDING IS RECURSIVE, BECAUSE THE WORLD IS NOT EVENLY FULL. A fixed 6x3 grid put 89,390
 *   lakes into the one cell covering Europe and western Asia and produced a 54.6 MB shard —
 *   over Cloudflare's 25 MiB asset limit and, worse, a download somebody in Northampton would
 *   make to see one reservoir. A cell that is still too big is SPLIT AGAIN, so shard size follows
 *   data density rather than geography.
 *
 * ★★ Each shard records its OWN data bbox, not the cell it came from: an item is assigned by its
 *   centre and may overhang, and the renderer intersects against the recorded box. Testing against
 *   the cell instead would clip features at every seam — missing data with no error.
 *
 * ★ The depth cap exists so a pathological layer cannot recurse forever; it is a guard, not a
 *   target, and a shard that is still oversize at the cap is reported rather than shipped.
 */
function shardRecursive(items, box, depth, out) {
  const size = JSON.stringify(items).length;
  if (items.length <= 1 || (size <= SHARD_LIMIT || depth >= SHARD_MAX_DEPTH)) {
    if (!items.length) return;
    let b = [180, 90, -180, -90];
    for (const it of items) {
      const ib = itemBox(it);
      if (ib[0] < b[0]) b[0] = ib[0];
      if (ib[1] < b[1]) b[1] = ib[1];
      if (ib[2] > b[2]) b[2] = ib[2];
      if (ib[3] > b[3]) b[3] = ib[3];
    }
    out.push({ items, box: b });
    return;
  }
  const [w, s, e, n] = box;
  const mx = (w + e) / 2; const my = (s + n) / 2;
  const quads = [[], [], [], []];
  for (const it of items) {
    const ib = itemBox(it);
    const cx = (ib[0] + ib[2]) / 2; const cy = (ib[1] + ib[3]) / 2;
    quads[(cy >= my ? 2 : 0) + (cx >= mx ? 1 : 0)].push(it);
  }
  // ★ If every item lands in one quadrant the split achieved nothing; emit rather than spin.
  if (quads.some((q) => q.length === items.length)) {
    let b = [180, 90, -180, -90];
    for (const it of items) {
      const ib = itemBox(it);
      if (ib[0] < b[0]) b[0] = ib[0]; if (ib[1] < b[1]) b[1] = ib[1];
      if (ib[2] > b[2]) b[2] = ib[2]; if (ib[3] > b[3]) b[3] = ib[3];
    }
    out.push({ items, box: b });
    return;
  }
  const boxes = [[w, s, mx, my], [mx, s, e, my], [w, my, mx, n], [mx, my, e, n]];
  for (let i = 0; i < 4; i++) shardRecursive(quads[i], boxes[i], depth + 1, out);
}

function shardArray(items) {
  const out = [];
  shardRecursive(items, [-180, -90, 180, 90], 0, out);
  return out;
}

const rows = [];
const written = [];
let totalBytes = 0;
let totalPoints = 0;

for (const [tier, name, data] of layers) {
  const json = JSON.stringify(data);
  const bytes = Buffer.byteLength(json);
  const pts = countPoints(data);
  const pack = tier === 'tier2' ? 'detail' : 'basic';
  const base = `${tier}-${name}`;
  if (bytes > SHARD_LIMIT && Array.isArray(data)) {
    const cells = shardArray(data);
    const shards = [];
    let worst = 0;
    cells.forEach((cell, k) => {
      const file = `${base}.s${k}.json`;
      const sJson = JSON.stringify(cell.items);
      const sBytes = Buffer.byteLength(sJson);
      if (sBytes > worst) worst = sBytes;
      shards.push({ file, box: cell.box.map((v) => Number(v.toFixed(3))) });
      index.files[file] = { tier, layer: name, bytes: sBytes,
                            features: cell.items.length, box: shards.at(-1).box };
      index.packs[pack].files.push(file);
      index.packs[pack].bytes += sBytes;
      written.push([file, sJson]);
    });
    // ★★ Cloudflare Workers refuse an asset over 25 MiB, and a deploy that fails on it fails AFTER
    //    everything else has been uploaded. Catch it here, where the fix is a parameter.
    if (worst > 25 * 1024 * 1024) {
      die(`${base}: largest shard is ${(worst / 1048576).toFixed(1)} MiB, over the 25 MiB asset limit.`);
    }
    // ★ The renderer looks HERE first: shards are opt-in, so a layer without this key is one file.
    index.sharded ||= {};
    index.sharded[base] = shards;
    rows.push({ layer: name, tier, features: featureCount(data), points: pts,
                kb: bytes / 1024, note: `${shards.length} shards` });
    totalBytes += bytes;
    totalPoints += pts;
    continue;
  }
  const file = `${base}.json`;
  index.files[file] = { tier, layer: name, bytes, features: featureCount(data), points: pts };
  index.packs[pack].files.push(file);
  index.packs[pack].bytes += bytes;
  written.push([file, json]);
  rows.push({ layer: name, tier, features: featureCount(data), points: pts, kb: bytes / 1024 });
  totalBytes += bytes;
  totalPoints += pts;
}
const indexJson = JSON.stringify(index);

if (CHECK) {
  // ★ index.json carries a generated date, so it is compared field-by-field minus that date: a
  //   re-run on a later day must not read as drift, but a changed BYTE COUNT must.
  let stale = null;
  for (const [file, json] of written) {
    const have = await readFile(path.join(outDir, file), 'utf8').catch(() => null);
    if (have !== json) { stale = file; break; }
  }
  if (!stale) {
    const have = await readFile(path.join(outDir, 'index.json'), 'utf8').catch(() => null);
    let old = null;
    try { old = JSON.parse(have); } catch { /* missing or corrupt counts as stale */ }
    if (!old || JSON.stringify(old.files) !== JSON.stringify(index.files) || old.version !== index.version) {
      stale = 'index.json';
    }
  }
  if (stale) die(`assets/mapdata/v1/${stale} is STALE — run: node scripts/gen-map-data.mjs`);
  console.log(`assets/mapdata/v1 matches its sources (${written.length} layers, ${(totalBytes / 1048576).toFixed(2)} MB)`);
  process.exit(0);
}

await mkdir(outDir, { recursive: true });
/* ★★★ PRUNE WHAT THIS RUN DID NOT WRITE. When tier2-lakes was split into 18 shards the unsharded
 *  38.5 MB file simply STAYED, got rsynced to the directory, and broke the deploy on Cloudflare's
 *  25 MiB asset limit -- a file nothing referenced any more. A generator that only ever adds leaves
 *  the previous shape of the data lying next to the current one, and the stale copy always wins
 *  somewhere. The output directory is owned by this script, so it says what belongs in it. */
const keep = new Set([...written.map(([f]) => f), 'index.json', 'country-labels.json',
                      ...reliefImages.map(([f]) => f)]);
for (const f of await readdir(outDir).catch(() => [])) {
  if ((f.endsWith('.json') || f.endsWith('.png')) && !keep.has(f)) {
    await unlink(path.join(outDir, f));
    console.error(`  pruned stale ${f}`);
  }
}
for (const [file, json] of written) await writeFile(path.join(outDir, file), json);
for (const [file, pack, img] of reliefImages) {
  const png = encodePng(img);
  await writeFile(path.join(outDir, file), png);
  index.files[file] = { layer: 'relief', bytes: png.length, width: img.width, height: img.height };
  index.packs[pack].files.push(file);
  index.packs[pack].bytes += png.length;
  console.error(`  relief ${file}: ${img.width}x${img.height}, ${(png.length / 1048576).toFixed(2)} MB`);
}
await writeFile(path.join(outDir, 'index.json'), JSON.stringify(index));

/* ★★★ THE DETAIL PACK IS SHIPPED AS ONE TARBALL, NOT EIGHT FILES. A server owner or a phone on
 *  cellular clicking "download detailed maps" must get ONE request that either succeeds or fails
 *  -- eight parallel fetches have a partial-failure state, and a half-installed pack is a map with
 *  the coastline of one tier and the towns of another. One archive, one checksum, one atomic move
 *  into place by whatever installs it.
 *  ★ It goes on a GitHub release, beside the APKs: free bandwidth, versioned, and it keeps the
 *  Cloudflare directory out of the business of serving 40 MB to every VibeServer on the estate. */
const distDir = path.join(root, 'dist/mapdata');
await mkdir(distDir, { recursive: true });
const tarball = path.join(distDir, `mapdata-v${index.version}-detail.tar.gz`);
try {
  execFileSync('tar', ['-czf', tarball, '-C', outDir, ...index.packs.detail.files], { stdio: 'pipe' });
} catch (e) {
  die(`detail pack: tar failed (${e.message}).`);
}
const tarBytes = (await stat(tarball)).size;
// ★ Whatever installs this MUST verify the checksum before unpacking. A truncated 40 MB download
//   over a phone tether is not a rare event, and it unpacks into a map that is subtly wrong.
const sha = execFileSync('shasum', ['-a', '256', tarball]).toString().split(/\s+/)[0];
await writeFile(tarball + '.sha256', `${sha}  ${path.basename(tarball)}\n`);

const pad = (s, n) => String(s).padEnd(n);
const padl = (s, n) => String(s).padStart(n);
console.log(`\nwrote ${outDir}\n`);
console.log(`${pad('layer', 11)}${pad('tier', 7)}${padl('features', 10)}${padl('points', 11)}${padl('KB', 10)}`);
console.log('-'.repeat(49));
for (const r of rows) {
  console.log(`${pad(r.layer, 11)}${pad(r.tier, 7)}${padl(r.features.toLocaleString(), 10)}${padl(r.points.toLocaleString(), 11)}${padl(r.kb.toFixed(1), 10)}  ${r.note || ''}`);
}
console.log('-'.repeat(49));
console.log(`${pad('TOTAL', 18)}${padl('', 10)}${padl(totalPoints.toLocaleString(), 11)}${padl((totalBytes / 1024).toFixed(1), 10)}`);

// ★ The split is the number that decides what ships in an APK, so it is printed every run.
const mb = (b) => (b / 1048576).toFixed(1) + ' MB';
console.log(`\npacks:  basic (bundled everywhere) ${mb(index.packs.basic.bytes)}`
          + ` in ${index.packs.basic.files.length} files`);
console.log(`        detail (optional download)  ${mb(index.packs.detail.bytes)}`
          + ` in ${index.packs.detail.files.length} files`);
console.log(`\ndetail tarball: ${path.relative(root, tarball)}  ${mb(tarBytes)} compressed`);
console.log(`        sha256: ${sha}`);
console.log(`\n${(totalBytes / 1048576).toFixed(2)} MB across ${written.length} layer files + index.json`);
