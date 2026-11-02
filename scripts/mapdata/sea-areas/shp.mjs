// shp.mjs — a minimal ESRI shapefile + dBASE reader for gen-sea-areas.mjs (polygons only, geographic coordinates).
// Our own code (the shapefile format is ESRI's published technical description, 1998); no dependency.
import { readFileSync } from 'node:fs';

/** [{ props, rings: [[[lon, lat], …], …] }] — every part of a polygon record as a ring (outer rings and holes alike;
 *  the caller tells them apart by winding when it needs to). */
export function readShapefile(base, enc = 'utf8') {   // enc: the .dbf's text encoding ('latin1' for ISO-8859-1/15 files such as AEMET's)
  const shp = readFileSync(base + '.shp'), dbf = readFileSync(base + '.dbf');
  const props = readDbf(dbf, enc);
  const out = [];
  let o = 100, i = 0;
  while (o + 8 <= shp.length) {
    const len = shp.readInt32BE(o + 4) * 2;
    const rec = o + 8;
    const type = shp.readInt32LE(rec);
    const rings = [];
    if (type === 5 || type === 15 || type === 25) {
      const nParts = shp.readInt32LE(rec + 36), nPts = shp.readInt32LE(rec + 40);
      const parts = []; for (let p = 0; p < nParts; p++) parts.push(shp.readInt32LE(rec + 44 + 4 * p));
      const pts0 = rec + 44 + 4 * nParts;
      for (let p = 0; p < nParts; p++) {
        const a = parts[p], b = p + 1 < nParts ? parts[p + 1] : nPts;
        const r = [];
        for (let k = a; k < b; k++) r.push([shp.readDoubleLE(pts0 + 16 * k), shp.readDoubleLE(pts0 + 16 * k + 8)]);
        rings.push(r);
      }
    }
    out.push({ props: props[i] || {}, rings });
    o = rec + len; i++;
  }
  return out;
}
export function readDbf(b, enc = 'utf8') {
  const n = b.readUInt32LE(4), hlen = b.readUInt16LE(8), rlen = b.readUInt16LE(10);
  const fields = [];
  for (let o = 32; o < hlen - 1 && b[o] !== 0x0d; o += 32) {
    fields.push({ name: b.toString('latin1', o, o + 11).replace(/\0.*$/, ''), type: String.fromCharCode(b[o + 11]), len: b[o + 16] });
  }
  const rows = [];
  for (let r = 0; r < n; r++) {
    let o = hlen + r * rlen + 1;
    const row = {};
    for (const f of fields) { row[f.name] = b.toString(enc, o, o + f.len).trim(); o += f.len; }
    rows.push(row);
  }
  return rows;
}
