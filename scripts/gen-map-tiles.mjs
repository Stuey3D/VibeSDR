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
import { inflateSync } from 'node:zlib';
import { DatabaseSync } from 'node:sqlite';
import { encodePng } from './lib/relief.mjs';

/** ★ --only=relief rebuilds just the relief (seconds) without the vector packs (minutes). */
const ONLY = (process.argv.find((a) => a.startsWith('--only=')) || '').slice(7);

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

if (ONLY !== 'relief') {   // (--only=basic still writes both NDJSONs; only the basic pack is tiled)
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
/* ★★ PORTS BY HARBOUR SIZE. The pack's ports carry [name, lon, lat, country] — no importance at all, so
 *  today's map drew the first 80 in the FILE that fell in view (Normandy's, never Britain's). The World
 *  Port Index does record Harbor Size (L / M / S / V); the pack dropped it. Read here from the same cached
 *  source with the same row filter as gen-map-data.mjs buildPorts, so the SET is identical and only the
 *  size is added. Large from Leaflet z5, medium z6, small z7, very small z9 — a NEW rule, since today's
 *  map had nothing to base one on. */
{
  const csvPath = path.join(root, '.mapdata-cache/ports.csv');
  const parseCsv = (t) => { const rows = []; let row = [], f = '', q = false;
    for (let i = 0; i < t.length; i++) { const c = t[i];
      if (q) { if (c === '"') { if (t[i + 1] === '"') { f += '"'; i++; } else q = false; } else f += c; }
      else if (c === '"') q = true; else if (c === ',') { row.push(f); f = ''; }
      else if (c === '\n' || c === '\r') { if (c === '\r' && t[i + 1] === '\n') i++; row.push(f); rows.push(row); row = []; f = ''; }
      else f += c; }
    if (f || row.length) { row.push(f); rows.push(row); } return rows; };
  const rows = parseCsv(readFileSync(csvPath, 'utf8')), h = rows[0];
  const ix = (n) => h.findIndex((x) => x.trim() === n);
  const iName = ix('Main Port Name'), iLat = ix('Latitude'), iLon = ix('Longitude'), iSize = ix('Harbor Size');
  const minZ = { L: 5, M: 6, S: 7, V: 9 };
  for (const r of rows.slice(1)) {
    const name = (r[iName] || '').trim(), lat = Number(r[iLat]), lon = Number(r[iLon]);
    if (!name || !Number.isFinite(lat) || !Number.isFinite(lon) || Math.abs(lat) > 90 || Math.abs(lon) > 180) continue;
    const size = ((r[iSize] || '').trim()[0] || 'V').toUpperCase();
    emit('vibemap-basic', 'ports', [minZ[size] ?? 9, MAXZ], { type: 'Point', coordinates: [Math.round(lon * 1e3) / 1e3, Math.round(lat * 1e3) / 1e3] },
         { name, size: 'LMSV'.indexOf(size) >= 0 ? 'LMSV'.indexOf(size) : 3 });
  }
}
/* ★★ THE MAIDENHEAD GRID, as vibemap.js drawGrid draws it: fields (20 x 10 deg) from Leaflet z3 to z5,
 *  squares (2 x 1 deg) from z6. Lines carry `level`; labels sit at each cell's centre. Pure arithmetic,
 *  so it is exact. ★ 6-character sub-squares (z12+) are NOT pre-built: 18.6 M labels worldwide — the
 *  host computes those for the view in hand. */
{
  const mh = (lon, lat, chars) => {
    const L = 'ABCDEFGHIJKLMNOPQR', x = lon + 180, y = lat + 90;
    let s = L[Math.floor(x / 20)] + L[Math.floor(y / 10)];
    if (chars >= 4) s += Math.floor((x % 20) / 2) + '' + Math.floor(y % 10);
    return s;
  };
  for (const [level, dLon, dLat, zr, chars] of [[0, 20, 10, [3, 5], 2], [1, 2, 1, [6, MAXZ], 4]]) {
    for (let lon = -180; lon <= 180; lon += dLon) emit('vibemap-basic', 'grid', zr, { type: 'LineString', coordinates: [[lon, -85], [lon, 85]] }, { level });
    for (let lat = -80; lat <= 80; lat += dLat) emit('vibemap-basic', 'grid', zr, { type: 'LineString', coordinates: [[-180, lat], [180, lat]] }, { level });
    for (let lon = -180; lon < 180; lon += dLon) for (let lat = -90; lat < 90; lat += dLat) {
      if (Math.abs(lat + dLat / 2) > 85) continue;
      emit('vibemap-basic', 'gridlabels', [Math.max(zr[0], 4), zr[1]], { type: 'Point', coordinates: [lon + dLon / 2, lat + dLat / 2] },
           { code: mh(lon + dLon / 2, lat + dLat / 2, chars), level });
    }
  }
}
// ★ Runways as the real centreline between both thresholds, each end labelled with its own
//   designator — exactly what drawRunways does.
/* ★★ AND IN THE BASIC PACK TOO (Stuart, 2026-09-27: "this detail level is fine, add the runways and we
 *  are golden"). The basic source is read no deeper than MapLibre z6 (the style's maxzoom) and
 *  over-zoomed from there, so its copy lives in the z6 tiles only (Leaflet [7, 7]) and the style's
 *  runways-coarse layer shows it from the same zoom as the detail one. A z6 tile quantises to ~150 m
 *  cells, so an end sits within ~60 m at UK latitudes — about a pixel at z10, far inside the coarse
 *  coastline's own error. With the detail pack in, runways-coarse is dropped (vibesdr:basicOnlyLayers). */
for (const [id, lon1, lat1, lon2, lat2, le, he, ft] of load('tier2', 'runways')) {
  const g = { type: 'LineString', coordinates: [[lon1, lat1], [lon2, lat2]] }, props = { id, le: le || '', he: he || '', ft: ft || 0 };
  emit('vibemap-detail', 'runways', [9, MAXZ], g, props);
  emit('vibemap-basic', 'runways', [7, 7], g, props);
}

await Promise.all(Object.values(sinks).map((s) => new Promise((r) => s.end(r))));
console.log('features:', count);

/* ── Tiles ───────────────────────────────────────────────────────────────────────────────── */
for (const pack of ['vibemap-basic', 'vibemap-detail']) {
  if (ONLY === 'basic' && pack !== 'vibemap-basic') continue;   // ★ --only=basic keeps the existing detail pack
  const out = path.join(OUT, `${pack}.pmtiles`);
  // ★ No tile-size or feature limits: tippecanoe's defaults DROP features to fit 500 kB tiles,
  //   which would be a silent change of map. Our tiers are already thinned by design.
  execFileSync('tippecanoe', ['-o', out, '--force', '-Z0', `-z${pack === 'vibemap-basic' ? 7 : MAXZ}`,
    '--no-feature-limit', '--no-tile-size-limit', '--quiet',
    '-P', path.join(OUT, `${pack}.ndjson`)], { stdio: 'inherit' });
  console.log(`${pack}.pmtiles  ${(statSync(out).size / 1048576).toFixed(1)} MB`);
}
}   // end vector packs

/* ══ RELIEF — today's shaded relief, EXACTLY, as GPU raster tiles ══════════════════════════════════
 * ★★★ NOT RE-RENDERED: the 64 relief-t{x}-{y}.png images gen-map-data.mjs already built (buildRelief:
 *     colour ramp, biomes, Horn hillshade from the NW, alpha fading in over the first 120 m) are the
 *     look Stuart signed off. They are Web Mercator — an 8192 px square world — which is EXACTLY a
 *     standard tile pyramid: at MapLibre's 512 px tiles, z4 is the full-resolution image cut 16x16 and
 *     z0-3 are halvings of it. So the GPU map draws the same relief, pixel for pixel, as a texture.
 * ★ Fully transparent tiles (open sea) are not written — the sea colour shows through anyway. */
function decodePng(buf) {
  let o = 8, w = 0, h = 0, type = 0; const idat = [];
  while (o < buf.length) {
    const len = buf.readUInt32BE(o), t = buf.toString('ascii', o + 4, o + 8), d = buf.subarray(o + 8, o + 8 + len);
    if (t === 'IHDR') { w = d.readUInt32BE(0); h = d.readUInt32BE(4); type = d[9];
      if (d[8] !== 8 || (type !== 6 && type !== 2)) throw new Error('relief PNG: only 8-bit RGB/RGBA is supported'); }
    else if (t === 'IDAT') idat.push(d);
    else if (t === 'IEND') break;
    o += 12 + len;
  }
  const bpp = type === 6 ? 4 : 3, raw = inflateSync(Buffer.concat(idat)), stride = w * bpp;
  const cur = Buffer.alloc(stride), prev = Buffer.alloc(stride), out = Buffer.alloc(w * h * 4);
  for (let y = 0, p = 0; y < h; y++) {
    const f = raw[p++]; raw.copy(cur, 0, p, p + stride); p += stride;
    for (let i = 0; i < stride; i++) {
      const a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
      let pr = 0;
      if (f === 1) pr = a; else if (f === 2) pr = b; else if (f === 3) pr = (a + b) >> 1;
      else if (f === 4) { const q = a + b - c, pa = Math.abs(q - a), pb = Math.abs(q - b), pc = Math.abs(q - c);
        pr = pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
      cur[i] = (cur[i] + pr) & 255;
    }
    for (let x = 0; x < w; x++) {
      const si = x * bpp, di = (y * w + x) * 4;
      out[di] = cur[si]; out[di + 1] = cur[si + 1]; out[di + 2] = cur[si + 2]; out[di + 3] = bpp === 4 ? cur[si + 3] : 255;
    }
    cur.copy(prev);
  }
  return { width: w, height: h, data: out };
}
{
  const N = 8, parts = [];
  for (let y = 0; y < N; y++) for (let x = 0; x < N; x++) parts.push({ x, y, img: decodePng(readFileSync(path.join(SRC, `relief-t${x}-${y}.png`))) });
  const side = parts[0].img.width, W = side * N;
  let world = Buffer.alloc(W * W * 4);
  for (const { x, y, img } of parts)
    for (let r = 0; r < side; r++) img.data.copy(world, ((y * side + r) * W + x * side) * 4, r * side * 4, (r + 1) * side * 4);
  const TS = 512, zTop = Math.round(Math.log2(W / TS));        // 8192 / 512 -> z4
  const mb = path.join(OUT, 'vibemap-relief.mbtiles');
  try { execFileSync('rm', ['-f', mb]); } catch {}
  const db = new DatabaseSync(mb);
  db.exec(`CREATE TABLE metadata (name TEXT, value TEXT); CREATE TABLE tiles (zoom_level INTEGER, tile_column INTEGER, tile_row INTEGER, tile_data BLOB);`);
  const meta = db.prepare('INSERT INTO metadata VALUES (?, ?)');
  /* ★★ WebP q90 WITH LOSSLESS ALPHA (cwebp; brew install webp). As PNG the pyramid was 54 MB —
   *  shaded terrain is photographic and compresses poorly losslessly. q90 is 12 % of that. MEASURED on
   *  the largest z4 tile: colour PSNR 37.1 dB, alpha error 0 (the lowland fade is exact), and
   *  indistinguishable side by side at 2x. JPEG was not an option: 16 % of the relief is partially
   *  transparent — the fade that lets the flat land colour show through. MapLibre reads WebP tiles on
   *  iOS and Android. */
  const tmpDir = path.join(OUT, '.relief-tmp'); mkdirSync(tmpDir, { recursive: true });
  const toWebp = (png, id) => {
    const a = path.join(tmpDir, `${id}.png`), b = path.join(tmpDir, `${id}.webp`);
    writeFileSync(a, png);
    execFileSync('cwebp', ['-quiet', '-q', '90', '-alpha_q', '100', a, '-o', b]);
    return readFileSync(b);
  };
  for (const [k, v] of [['name', 'vibemap-relief'], ['format', 'webp'], ['minzoom', '0'], ['maxzoom', String(zTop)],
                        ['bounds', '-180,-85.0511,180,85.0511'], ['type', 'overlay']]) meta.run(k, v);
  const put = db.prepare('INSERT INTO tiles VALUES (?, ?, ?, ?)');
  let ww = W, written = 0, skipped = 0;
  for (let z = zTop; z >= 0; z--) {
    const n = ww / TS;
    for (let ty = 0; ty < n; ty++) for (let tx = 0; tx < n; tx++) {
      const t = Buffer.alloc(TS * TS * 4); let any = false;
      for (let r = 0; r < TS; r++) world.copy(t, r * TS * 4, ((ty * TS + r) * ww + tx * TS) * 4, ((ty * TS + r) * ww + (tx + 1) * TS) * 4);
      for (let i = 3; i < t.length; i += 4) if (t[i]) { any = true; break; }
      if (!any) { skipped++; continue; }
      put.run(z, tx, n - 1 - ty, toWebp(encodePng({ width: TS, height: TS, data: t }), `${z}-${tx}-${ty}`));   // ★ MBTiles rows are TMS: flipped
      written++;
    }
    if (z > 0) {   // halve for the next zoom out: a 2x2 average, alpha-weighted so edges do not darken
      const h2 = ww / 2, nw = Buffer.alloc(h2 * h2 * 4);
      for (let y = 0; y < h2; y++) for (let x = 0; x < h2; x++) {
        let r = 0, g = 0, b = 0, a = 0;
        for (const [dx, dy] of [[0, 0], [1, 0], [0, 1], [1, 1]]) {
          const i = ((2 * y + dy) * ww + 2 * x + dx) * 4, al = world[i + 3];
          r += world[i] * al; g += world[i + 1] * al; b += world[i + 2] * al; a += al;
        }
        const o2 = (y * h2 + x) * 4;
        if (a) { nw[o2] = Math.round(r / a); nw[o2 + 1] = Math.round(g / a); nw[o2 + 2] = Math.round(b / a); nw[o2 + 3] = Math.round(a / 4); }
      }
      world = nw; ww = h2;
    }
  }
  db.close();
  execFileSync('rm', ['-rf', tmpDir]);
  const out = path.join(OUT, 'vibemap-relief.pmtiles');
  execFileSync('pmtiles', ['convert', mb, out], { stdio: 'ignore' });
  console.log(`vibemap-relief.pmtiles  ${(statSync(out).size / 1048576).toFixed(1)} MB  (${written} tiles, ${skipped} empty sea skipped)`);
}
