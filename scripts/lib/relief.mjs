/**
 * relief.mjs — a shaded-relief image built from a raw elevation grid, and a tiny PNG encoder.
 *
 * ★★★ WHY A RASTER, IN A BASEMAP THAT DELETED RASTER TILES. The thing we threw away was a
 *   DEPENDENCY: tiles fetched on demand from somebody else's server, which went black when they
 *   were slow and vanished when they blocked us. A single small image BUNDLED with the app is the
 *   opposite of that — it is offline, it is ours, it is one file, and nothing can fail to arrive.
 *
 * ★★ AND TERRAIN GENUINELY IS A RASTER. Coastlines, roads and lakes are shapes and belong in
 *   vectors. Elevation is a continuous field: expressing it as polygons means contour bands, which
 *   is both bigger and worse-looking than the grid it came from. Natural Earth's mountain polygons
 *   are what happens when you try — envelopes round a range, one of which covers Belgium, which is
 *   why they were pulled on 2026-09-26.
 *
 * ★ Source: ETOPO2v2c, 2 arc-minute, 10800x5400 int16 little-endian, cell-registered, row 0 at
 *   90N/180W running east then south. PUBLIC DOMAIN (US Government, NOAA/NGDC). No header to
 *   parse — it is exactly width*height*2 bytes, which is asserted before a pixel is read.
 */
import { readFileSync } from 'node:fs';
import { deflateSync } from 'node:zlib';

export const ETOPO_W = 10800;
export const ETOPO_H = 5400;

export function readEtopo(file) {
  const buf = readFileSync(file);
  if (buf.length !== ETOPO_W * ETOPO_H * 2) {
    throw new Error(`${file}: ${buf.length} bytes, expected ${ETOPO_W * ETOPO_H * 2} `
      + `(${ETOPO_W}x${ETOPO_H} int16) — the grid has changed shape.`);
  }
  return new Int16Array(buf.buffer, buf.byteOffset, ETOPO_W * ETOPO_H);
}

/**
 * ★★ THE COLOUR RAMP IS THE MAP'S PALETTE, NOT AN ATLAS'S. It starts at exactly MAP_LAND so low
 *   ground is indistinguishable from the flat vector fill underneath it, and climbs through olive
 *   and brown to a pale snow only at genuine altitude. An atlas ramp (bright green lowlands,
 *   orange uplands) would be legible and completely wrong under an amber UI.
 */
/* ★★★ THE SEA FLOOR WAS BEING THROWN AWAY. Half of ETOPO is bathymetry and the first version
 *  discarded every cell at or below sea level -- so the map had the Himalaya and a flat blue
 *  nothing where the Mid-Atlantic Ridge, the Marianas Trench and the continental shelves are. It
 *  is the same grid, already downloaded and already parsed: the ocean costs NOTHING to draw.
 *  ★★ And it is not decoration for a radio map. The shelf edge is where the HF ground-wave path
 *  changes, and for anyone watching AIS the shelf IS where the shipping is.
 *  ★ Darker with depth, and much lower contrast than the land ramp: the sea must stay a backdrop
 *  that labels and receiver markers sit on top of, not a second subject competing with the land. */
const SEA_RAMP = [
  [-11000, 0x08, 0x1b, 0x30],
  [-5000, 0x0d, 0x26, 0x40],
  [-3000, 0x11, 0x2d, 0x4a],
  [-1000, 0x15, 0x36, 0x56],
  [-200, 0x1a, 0x3f, 0x62],
  [0, 0x1e, 0x47, 0x6d],
];

const RAMP = [
  [-50, 0x3c, 0x5a, 0x3f],
  [200, 0x3e, 0x5a, 0x3d],
  [600, 0x49, 0x59, 0x3a],
  [1200, 0x5c, 0x57, 0x3c],
  [2000, 0x6d, 0x5d, 0x46],
  [3000, 0x7c, 0x73, 0x64],
  [4200, 0x9a, 0x9b, 0x98],
  [6000, 0xc9, 0xcd, 0xd1],
];

function rampOn(table, e) {
  if (e <= table[0][0]) return table[0];
  for (let i = 1; i < table.length; i++) {
    if (e <= table[i][0]) {
      const [e0, r0, g0, b0] = table[i - 1];
      const [e1, r1, g1, b1] = table[i];
      const t = (e - e0) / (e1 - e0);
      return [0, r0 + (r1 - r0) * t, g0 + (g1 - g0) * t, b0 + (b1 - b0) * t];
    }
  }
  return table[table.length - 1];
}
const ramp = (e) => rampOn(RAMP, e);
const seaRamp = (e) => rampOn(SEA_RAMP, e);

/**
 * Build an RGBA shaded-relief image in Web Mercator (see the note below on why).
 *
 * ★★★ ALPHA FADES IN OVER THE FIRST 120 m OF ALTITUDE, and that is not a style choice — it is what
 *   stops the relief contradicting the coastline. The vector coast is Natural Earth's; the grid is
 *   NOAA's at ~3.7 km per cell. They disagree by a cell or two everywhere, and an abrupt land/sea
 *   edge in the image would scatter green specks into the vector sea and grey specks onto the
 *   vector land. Fading the image out as it approaches sea level makes the two sources agree by
 *   construction, because neither asserts anything where they differ.
 *
 * ★ Hillshade is the standard Horn method, sun from the north-west at 45 deg. `zFactor` exaggerates
 *   because at 2 arc-minutes real slopes are averaged almost flat — without it the Himalaya look
 *   like a gentle rise.
 */
/* ★★★ THE IMAGE IS WRITTEN IN WEB MERCATOR, NOT EQUIRECTANGULAR, AND THAT IS NOT OPTIONAL.
 *  Leaflet's L.imageOverlay stretches an image linearly between two corners IN PROJECTED SPACE.
 *  Hand it the raw ETOPO grid (which is linear in LATITUDE) with bounds of +/-85 deg and every
 *  row lands at the wrong place: Britain slides south, the tropics inflate, and the relief
 *  disagrees with the vector coastline by hundreds of kilometres at high latitude. It would look
 *  like a data error and it is a projection error.
 *  ★ So the reprojection happens HERE, once, at build time: each output row is a Mercator y, and
 *  the latitude it corresponds to selects the source row. The renderer then does nothing clever,
 *  which is where this belongs -- the same reason the tiers and shards are precomputed.
 *  ★ MERC_LAT is Web Mercator's own cut-off, the latitude at which the world becomes square. */
export const MERC_LAT = 85.0511287798066;

export function buildRelief(grid, { width = 2700, zFactor = 6, biomes = null } = {}) {
  const w = width;
  const h = width;                          // Web Mercator's world is square
  /** Mercator row -> source grid row. */
  const rowFor = (y) => {
    const n = Math.PI * (1 - (2 * (y + 0.5)) / h);
    const lat = (180 / Math.PI) * Math.atan(Math.sinh(n));
    return Math.min(ETOPO_H - 1, Math.max(0, Math.floor(((90 - lat) / 180) * ETOPO_H)));
  };
  const srcRow = new Int32Array(h);
  for (let y = 0; y < h; y++) srcRow[y] = rowFor(y);
  const px = Buffer.alloc(w * h * 4);
  const at = (x, y) => {
    const sx = Math.min(ETOPO_W - 1, Math.max(0, Math.round((x / w) * ETOPO_W)));
    const sy = srcRow[Math.min(h - 1, Math.max(0, y))];
    return grid[sy * ETOPO_W + sx];
  };
  // Sun from the north-west, 45 deg up.
  const az = (315 * Math.PI) / 180;
  const alt = (45 * Math.PI) / 180;
  const sinAlt = Math.sin(alt);
  const cosAlt = Math.cos(alt);

  for (let y = 0; y < h; y++) {
    // ★ Cell width shrinks with latitude; without this the shading skews badly towards the poles.
    const lat = 90 - (srcRow[y] / ETOPO_H) * 180;
    const lonScale = Math.max(0.15, Math.cos((lat * Math.PI) / 180));
    for (let x = 0; x < w; x++) {
      const e = at(x, y);
      const o = (y * w + x) * 4;
      const xm = x > 0 ? x - 1 : x; const xp = x < w - 1 ? x + 1 : x;
      const ym = y > 0 ? y - 1 : y; const yp = y < h - 1 ? y + 1 : y;
      const dzdx = ((at(xp, ym) + 2 * at(xp, y) + at(xp, yp))
                  - (at(xm, ym) + 2 * at(xm, y) + at(xm, yp))) / (8 * lonScale);
      const dzdy = ((at(xm, yp) + 2 * at(x, yp) + at(xp, yp))
                  - (at(xm, ym) + 2 * at(x, ym) + at(xp, ym))) / 8;
      const sx = (dzdx * zFactor) / 3700;      // 3700 m is roughly one cell at the equator
      const sy = (dzdy * zFactor) / 3700;
      const slope = Math.atan(Math.hypot(sx, sy));
      const aspect = Math.atan2(sy, -sx);
      let shade = sinAlt * Math.cos(slope) + cosAlt * Math.sin(slope) * Math.cos(az - aspect);
      shade = Math.max(0, Math.min(1, shade));
      // ★ Compressed around 1.0: this MODULATES the palette, it does not replace it. Full-range
      //   shading would turn every north-east slope black and read as a hole in the land.
      const sea = e <= 0;
      /* ★ The sea's shading is deliberately flatter (0.88..1.12 against the land's 0.72..1.34).
       *  Abyssal slopes are enormous, and at full contrast a trench reads as a black gash. */
      const k = sea ? 0.88 + shade * 0.24 : 0.72 + shade * 0.62;

      let [, r, g, b] = sea ? seaRamp(e) : ramp(e);
      if (!sea && biomes) {
        const bi = BIOME_COLOUR[biomes[y * w + x]];
        if (bi) {
          /* ★★ THE BIOME OWNS THE LOWLANDS; ELEVATION TAKES OVER WITH HEIGHT. Above ~1200 m the
           *  rock, scree and snow of the elevation ramp matter more than what grows there, and by
           *  4 km nothing grows at all -- so the two blend rather than one winning outright. A
           *  hard switch would draw a contour line across every mountain. */
          const t = Math.max(0, Math.min(1, (e - 1200) / 2800));
          r = bi[0] + (r - bi[0]) * t;
          g = bi[1] + (g - bi[1]) * t;
          b = bi[2] + (b - bi[2]) * t;
        }
      }
      px[o] = Math.max(0, Math.min(255, Math.round(r * k)));
      px[o + 1] = Math.max(0, Math.min(255, Math.round(g * k)));
      px[o + 2] = Math.max(0, Math.min(255, Math.round(b * k)));
      /* ★★ Both sides fade towards the shoreline, for the same reason: NOAA's grid and Natural
       *  Earth's coastline disagree by a cell or two everywhere, so neither may assert anything
       *  within ~120 m of sea level. Below it the vector sea colour shows through; above it the
       *  vector land does. */
      px[o + 3] = sea ? Math.round(255 * Math.min(1, -e / 120)) : Math.round(255 * Math.min(1, e / 120));
    }
  }
  return { width: w, height: h, data: px };
}

/* ───────────────────────── a minimal PNG encoder ─────────────────────────
 * ★ RGBA, filter type 0, one IDAT. Node ships the only hard part (deflate) in zlib, so this is
 *   chunk framing and a CRC — far less than a dependency is worth for one file per build.
 */
const CRC = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();
function crc32(buf) {
  let c = -1;
  for (let i = 0; i < buf.length; i++) c = CRC[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, 'latin1'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body));
  return Buffer.concat([len, body, crc]);
}

export function encodePng({ width, height, data }) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;      // bit depth
  ihdr[9] = 6;      // colour type: RGBA
  // 10,11,12 = deflate / adaptive filtering / no interlace, all zero.
  const stride = width * 4;
  const raw = Buffer.alloc((stride + 1) * height);
  for (let y = 0; y < height; y++) {
    raw[y * (stride + 1)] = 0;                       // filter: none
    data.copy(raw, y * (stride + 1) + 1, y * stride, y * stride + stride);
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

/* ───────────────────────── biomes ─────────────────────────
 * ★★★ BIOME IS RASTERISED INTO THE SAME IMAGE, NOT SHIPPED AS POLYGONS. RESOLVE Ecoregions 2017 is
 *   847 polygons and a 243 MB shapefile covering every acre of land; as vectors it would dwarf the
 *   entire rest of the basemap. But like elevation it is a FIELD -- every point on land has
 *   exactly one biome -- and a field belongs in a grid. Painted into the relief it costs almost
 *   nothing, because the pixels already exist.
 *
 * ★★ IT ALSO RETIRES THE HAND-PICKED NATURAL EARTH COVER CLASSES. 'Desert' from NE was 58 polygons
 *   chosen because they looked like the right idea; this is a peer-reviewed global classification
 *   with the Amazon, Borneo, the taiga and the Sahel all correctly bounded. ✗ Do not draw both:
 *   the NE desert polygons would sit ON TOP of the raster and contradict it.
 *
 * ★ Licence: CC BY 4.0 (Dinerstein et al. 2017) -- attribution required, like GeoNames and
 *   HydroLAKES. It rides in index.json.
 */

/**
 * ★★ THE PALETTE IS DARK AND CLOSE-TOGETHER ON PURPOSE. These are large filled areas under an
 *   amber UI, so they are separated by LIGHTNESS rather than hue -- rainforest darkest, desert
 *   lightest, everything else between. A true-colour biome map (bright green tropics, yellow
 *   savanna) is what a school atlas does and it would be unreadable here.
 */
export const BIOME_COLOUR = {
  1: [0x2b, 0x47, 0x2c],   // Tropical & Subtropical Moist Broadleaf Forests -- the Amazon, Borneo
  2: [0x3c, 0x52, 0x33],   // Tropical & Subtropical Dry Broadleaf Forests
  3: [0x32, 0x48, 0x2e],   // Tropical & Subtropical Coniferous Forests
  4: [0x3f, 0x5a, 0x3c],   // Temperate Broadleaf & Mixed Forests -- Britain, most of Europe
  5: [0x34, 0x4f, 0x3b],   // Temperate Conifer Forests
  6: [0x2e, 0x47, 0x39],   // Boreal Forests/Taiga
  7: [0x5b, 0x60, 0x37],   // Tropical & Subtropical Grasslands, Savannas & Shrublands
  8: [0x58, 0x60, 0x3b],   // Temperate Grasslands, Savannas & Shrublands
  9: [0x34, 0x54, 0x3e],   // Flooded Grasslands & Savannas
  10: [0x4d, 0x58, 0x43],  // Montane Grasslands & Shrublands
  11: [0x59, 0x60, 0x57],  // Tundra
  12: [0x54, 0x5f, 0x39],  // Mediterranean Forests, Woodlands & Scrub
  13: [0x6d, 0x61, 0x42],  // Deserts & Xeric Shrublands -- the Sahara, and MAP_DESERT's own colour
  14: [0x2e, 0x49, 0x39],  // Mangroves
};

/** Mercator row -> latitude, the inverse of the row mapping used to build the image. */
function latForRow(y, h) {
  const n = Math.PI * (1 - (2 * (y + 0.5)) / h);
  return (180 / Math.PI) * Math.atan(Math.sinh(n));
}

/**
 * Rasterise biome polygons into a Uint8Array of biome numbers, in the SAME Mercator grid as the
 * relief image.
 *
 * ★★★ SCANLINE FILL, IN LATITUDE, WITH ALL OF A POLYGON'S RINGS AT ONCE. Testing each pixel
 *   against each polygon would be 7 million x 847. Instead each polygon is drawn once: for every
 *   image row its latitude crosses, the crossings of that latitude with every edge are collected,
 *   sorted, and filled in pairs. Gathering all rings of one polygon TOGETHER is what makes holes
 *   work -- an island in a lake in a biome is an odd number of crossings, and splitting the rings
 *   would fill it.
 */
export function rasteriseBiomes(shapes, size) {
  const out = new Uint8Array(size * size);
  const rowLat = new Float64Array(size);
  for (let y = 0; y < size; y++) rowLat[y] = latForRow(y, size);
  // Mercator rows run north -> south, so latitude DECREASES with y: the search must respect that.
  const rowForLat = (lat) => {
    let lo = 0; let hi = size - 1;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (rowLat[mid] > lat) lo = mid + 1; else hi = mid;
    }
    return lo;
  };

  const xs = [];
  for (const { biome, rings } of shapes) {
    if (!BIOME_COLOUR[biome]) continue;
    let south = 90; let north = -90;
    for (const ring of rings) {
      for (const [, lat] of ring) { if (lat < south) south = lat; if (lat > north) north = lat; }
    }
    const y0 = Math.max(0, rowForLat(Math.min(85.05, north)));
    const y1 = Math.min(size - 1, rowForLat(Math.max(-85.05, south)));
    for (let y = y0; y <= y1; y++) {
      const lat = rowLat[y];
      xs.length = 0;
      for (const ring of rings) {
        for (let i = 0, j = ring.length - 1; i < ring.length; j = i++) {
          const [x1, y1r] = ring[j]; const [x2, y2r] = ring[i];
          if ((y1r > lat) === (y2r > lat)) continue;
          xs.push(x1 + ((lat - y1r) / (y2r - y1r)) * (x2 - x1));
        }
      }
      if (xs.length < 2) continue;
      xs.sort((a, b) => a - b);
      for (let i = 0; i + 1 < xs.length; i += 2) {
        let px0 = Math.round(((xs[i] + 180) / 360) * size);
        let px1 = Math.round(((xs[i + 1] + 180) / 360) * size);
        if (px1 < px0) { const t = px0; px0 = px1; px1 = t; }
        px0 = Math.max(0, px0); px1 = Math.min(size - 1, px1);
        const base = y * size;
        for (let x = px0; x <= px1; x++) out[base + x] = biome;
      }
    }
  }
  return out;
}
