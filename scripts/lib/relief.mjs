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

function ramp(e) {
  if (e <= RAMP[0][0]) return RAMP[0];
  for (let i = 1; i < RAMP.length; i++) {
    if (e <= RAMP[i][0]) {
      const [e0, r0, g0, b0] = RAMP[i - 1];
      const [e1, r1, g1, b1] = RAMP[i];
      const t = (e - e0) / (e1 - e0);
      return [0, r0 + (r1 - r0) * t, g0 + (g1 - g0) * t, b0 + (b1 - b0) * t];
    }
  }
  return RAMP[RAMP.length - 1];
}

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

export function buildRelief(grid, { width = 2700, zFactor = 6 } = {}) {
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
      if (e <= 0) { px[o + 3] = 0; continue; }

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
      const k = 0.72 + shade * 0.62;

      const [, r, g, b] = ramp(e);
      px[o] = Math.max(0, Math.min(255, Math.round(r * k)));
      px[o + 1] = Math.max(0, Math.min(255, Math.round(g * k)));
      px[o + 2] = Math.max(0, Math.min(255, Math.round(b * k)));
      px[o + 3] = Math.round(255 * Math.min(1, e / 120));
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
