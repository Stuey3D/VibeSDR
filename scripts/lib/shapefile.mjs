/**
 * shapefile.mjs — a minimal, streaming ESRI Shapefile reader. Polygons and their attributes only.
 *
 * ★★★ WHY THIS EXISTS RATHER THAN `ogr2ogr`. The map generator already runs on a bare machine with
 *   nothing but Node, `unzip`, `tar` and `shasum`. Requiring GDAL to rebuild the basemap would mean
 *   a 200 MB toolchain install standing between anyone and a map refresh, for a format whose
 *   polygon subset is ~150 lines. The shapefile spec has not changed since 1998 and it is not going
 *   to: this reader cannot rot.
 *
 * ★★ IT IS DELIBERATELY NOT A GENERAL SHAPEFILE LIBRARY. It reads shape type 5 (Polygon) and 15/25
 *   (PolygonZ/M, whose X/Y prefix is identical) and it THROWS on anything else rather than guessing
 *   -- a reader that silently skips the shapes it does not understand produces a map with holes in
 *   it and no error to explain them. ✗ Do not add a "just ignore unknown types" path.
 *
 * ★ Chunked, not record-by-record: HydroLAKES is a 1.1 GB .shp with 1.4 M records, and one
 *   readSync per record is millions of syscalls. It reads in 8 MB blocks and carries the partial
 *   record across the boundary.
 *
 * Sources of truth: ESRI Shapefile Technical Description (July 1998) for the .shp geometry, and
 * the dBASE III+ layout for the .dbf attributes.
 */
import { openSync, readSync, closeSync, statSync } from 'node:fs';

const CHUNK = 8 * 1024 * 1024;

/**
 * Read a .dbf into an array of plain objects, keeping only `wanted` fields.
 * ★ Only the requested columns are decoded. HydroLAKES' .dbf is 377 MB across 20+ columns and we
 *   need two of them; decoding the rest is pure waste and a lot of garbage for the collector.
 */
export function readDbf(file, wanted) {
  const fd = openSync(file, 'r');
  try {
    const head = Buffer.alloc(32);
    readSync(fd, head, 0, 32, 0);
    const numRecords = head.readUInt32LE(4);
    const headerLen = head.readUInt16LE(8);
    const recordLen = head.readUInt16LE(10);
    if (!numRecords || !recordLen) throw new Error(`${file}: empty or unreadable dBASE header`);

    // Field descriptors: 32 bytes each, terminated by 0x0D.
    const fieldsBuf = Buffer.alloc(headerLen - 32);
    readSync(fd, fieldsBuf, 0, fieldsBuf.length, 32);
    const fields = [];
    let offset = 1;                                   // byte 0 of a record is the deletion flag
    for (let i = 0; i + 32 <= fieldsBuf.length; i += 32) {
      if (fieldsBuf[i] === 0x0d) break;
      const name = fieldsBuf.toString('latin1', i, i + 11).replace(/\0.*$/, '').trim();
      const type = String.fromCharCode(fieldsBuf[i + 11]);
      const len = fieldsBuf[i + 16];
      fields.push({ name, type, len, offset });
      offset += len;
    }
    const keep = fields.filter((f) => !wanted || wanted.includes(f.name));
    // ★ Fail loudly on a column that is not there. A missing `Lake_area` silently becomes
    //   `undefined`, every area comparison is false, and the layer ships EMPTY.
    for (const w of wanted || []) {
      if (!keep.some((f) => f.name === w)) {
        throw new Error(`${file}: no column "${w}" (have: ${fields.map((f) => f.name).join(', ')})`);
      }
    }

    const out = new Array(numRecords);
    const buf = Buffer.alloc(CHUNK - (CHUNK % recordLen));
    let rec = 0;
    let pos = headerLen;
    while (rec < numRecords) {
      const got = readSync(fd, buf, 0, buf.length, pos);
      if (got <= 0) break;
      pos += got;
      for (let o = 0; o + recordLen <= got && rec < numRecords; o += recordLen, rec++) {
        const row = {};
        for (const f of keep) {
          const raw = buf.toString('latin1', o + f.offset, o + f.offset + f.len).trim();
          row[f.name] = (f.type === 'N' || f.type === 'F')
            ? (raw === '' ? null : Number(raw))
            : raw;
        }
        out[rec] = row;
      }
    }
    if (rec !== numRecords) throw new Error(`${file}: read ${rec} of ${numRecords} records`);
    return out;
  } finally {
    closeSync(fd);
  }
}

/**
 * Stream a .shp, calling `onShape(index, rings)` for each polygon. `rings` is an array of rings,
 * each an array of [lon, lat]. Returns the number of shapes read.
 *
 * `shouldRead(index)` is consulted BEFORE the geometry is decoded, so a caller that already knows
 * from the .dbf that it does not want record N pays only for skipping it.
 * ★★ That ordering is the whole performance story here: HydroLAKES is 1.4 M lakes and we keep a
 *    few per cent of them. Decoding first and filtering after would decode 1.1 GB for nothing.
 */
export function eachPolygon(file, onShape, shouldRead = () => true) {
  const fd = openSync(file, 'r');
  try {
    const size = statSync(file).size;
    const head = Buffer.alloc(100);
    readSync(fd, head, 0, 100, 0);
    if (head.readInt32BE(0) !== 9994) throw new Error(`${file}: not a shapefile (bad magic)`);

    let pos = 100;
    let carry = Buffer.alloc(0);
    let index = 0;
    while (pos < size || carry.length >= 8) {
      if (carry.length < 8 && pos < size) {
        const buf = Buffer.alloc(Math.min(CHUNK, size - pos));
        const got = readSync(fd, buf, 0, buf.length, pos);
        pos += got;
        carry = carry.length ? Buffer.concat([carry, buf.subarray(0, got)]) : buf.subarray(0, got);
        continue;
      }
      if (carry.length < 8) break;
      const contentLen = carry.readInt32BE(4) * 2;          // stored in 16-bit words
      const total = 8 + contentLen;
      if (carry.length < total) {
        if (pos >= size) break;                             // truncated tail: stop, do not guess
        const buf = Buffer.alloc(Math.min(CHUNK, size - pos));
        const got = readSync(fd, buf, 0, buf.length, pos);
        pos += got;
        carry = Buffer.concat([carry, buf.subarray(0, got)]);
        continue;
      }
      const body = carry.subarray(8, total);
      carry = carry.subarray(total);
      const i = index++;
      if (!shouldRead(i)) continue;

      const shapeType = body.readInt32LE(0);
      if (shapeType === 0) continue;                        // null shape: legal, and empty
      if (shapeType !== 5 && shapeType !== 15 && shapeType !== 25) {
        throw new Error(`${file}: shape ${i} is type ${shapeType}, not a polygon. `
          + 'This reader handles polygons only, and refuses to silently skip what it cannot draw.');
      }
      // 4 type + 32 bbox, then numParts, numPoints, parts[], points[]
      const numParts = body.readInt32LE(36);
      const numPoints = body.readInt32LE(40);
      const partsAt = 44;
      const pointsAt = partsAt + numParts * 4;
      const rings = [];
      for (let p = 0; p < numParts; p++) {
        const start = body.readInt32LE(partsAt + p * 4);
        const end = p + 1 < numParts ? body.readInt32LE(partsAt + (p + 1) * 4) : numPoints;
        const ring = new Array(end - start);
        for (let k = start; k < end; k++) {
          const at = pointsAt + k * 16;
          ring[k - start] = [body.readDoubleLE(at), body.readDoubleLE(at + 8)];
        }
        rings.push(ring);
      }
      onShape(i, rings);
    }
    return index;
  } finally {
    closeSync(fd);
  }
}
