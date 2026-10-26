/**
 * gen-ndb-navaids.mjs — the published NDBs, compiled into every server for the NDB skimmer's matching
 * (android/app/src/main/cpp/vibe_ndb_navaids.h).
 *
 *   node scripts/gen-ndb-navaids.mjs             -> android/app/src/main/cpp/vibe_ndb_navaids.h
 *   node scripts/gen-ndb-navaids.mjs --check     -> fail if the checked-in table has drifted from the cached source
 *   node scripts/gen-ndb-navaids.mjs --refresh   -> re-download the source (otherwise .mapdata-cache/ is used)
 *
 * SOURCE AND LICENCE: OurAirports navaids.csv — PUBLIC DOMAIN (dedicated by David Megginson), the same source and the
 * same cache (.mapdata-cache/, gitignored) as the airports and runways in scripts/gen-map-data.mjs. The clients credit
 * it beside the beacon list ("Beacon data: OurAirports, public domain").
 *
 * ★★ WHY COMPILED IN, NOT A MAP-DATA FILE. The matching runs on the SERVER (decode on the server; clients display), on
 *    every server including the Android ones that have no map pack on disk. ~6 700 beacons pack to ≈ 250 KB of .rodata
 *    — paged in only when the NDB mode first looks one up (the ADS-B starter table's reasoning, vibe_adsb_starter.h).
 * ★ Only NDB and NDB-DME, 150–1750 kHz, with an ident and a position. Sorted by frequency (the lookup is a range search).
 * ★ Fails LOUDLY on a source that has changed shape (gen-map-data's rule): a silently empty table is a decoder that
 *   never names anything.
 */
import { readFile, writeFile, mkdir, stat } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const cacheDir = path.join(root, '.mapdata-cache');
const out = path.join(root, 'android/app/src/main/cpp/vibe_ndb_navaids.h');
const SRC = 'https://davidmegginson.github.io/ourairports-data/navaids.csv';
const CHECK = process.argv.includes('--check');
const REFRESH = process.argv.includes('--refresh');

function die(m) { console.error(`gen-ndb-navaids: ${m}`); process.exit(1); }

async function source() {
  const file = path.join(cacheDir, 'navaids.csv');
  if (!REFRESH) {
    const have = await stat(file).catch(() => null);
    if (have && have.size > 0) return { text: await readFile(file, 'utf8'), date: have.mtime.toISOString().slice(0, 10) };
  }
  await mkdir(cacheDir, { recursive: true });
  let res;
  try { res = await fetch(SRC, { redirect: 'follow' }); } catch (e) { die(`${SRC} unreachable (${e.message})`); }
  if (!res.ok) die(`${SRC} returned HTTP ${res.status}`);
  const text = await res.text();
  if (text.length < 100000) die(`navaids.csv is only ${text.length} bytes — the source has changed shape`);
  await writeFile(file, text);
  return { text, date: new Date().toISOString().slice(0, 10) };
}

/** RFC 4180-ish CSV (quoted fields, doubled quotes). */
function parseCsv(text) {
  const rows = []; let row = [], f = '', q = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (q) { if (c === '"') { if (text[i + 1] === '"') { f += '"'; i++; } else q = false; } else f += c; continue; }
    if (c === '"') q = true;
    else if (c === ',') { row.push(f); f = ''; }
    else if (c === '\n') { row.push(f); rows.push(row); row = []; f = ''; }
    else if (c !== '\r') f += c;
  }
  if (f || row.length) { row.push(f); rows.push(row); }
  return rows;
}

/** A C string literal: printable ASCII as is, everything else (UTF-8 bytes, quotes, backslashes) as octal escapes. */
function cstr(s) {
  let o = '';
  for (const b of Buffer.from(s, 'utf8')) {
    if (b >= 0x20 && b < 0x7f && b !== 0x22 && b !== 0x5c && b !== 0x3f) o += String.fromCharCode(b);
    else o += '\\' + b.toString(8).padStart(3, '0');
  }
  return o;
}

const { text, date } = await source();
const rows = parseCsv(text);
const h = rows.shift();
const col = (n) => { const i = h.indexOf(n); if (i < 0) die(`no column "${n}" — the source has changed shape`); return i; };
const iIdent = col('ident'), iName = col('name'), iType = col('type'), iFreq = col('frequency_khz'),
  iLat = col('latitude_deg'), iLon = col('longitude_deg'), iCc = col('iso_country');
const recs = [];
for (const r of rows) {
  if (r.length < h.length) continue;
  if (r[iType] !== 'NDB' && r[iType] !== 'NDB-DME') continue;
  const khz = Number(r[iFreq]), lat = Number(r[iLat]), lon = Number(r[iLon]);
  const ident = (r[iIdent] || '').toUpperCase().trim();
  if (!(khz >= 150 && khz <= 1750) || !Number.isFinite(lat) || !Number.isFinite(lon) || !/^[A-Z0-9]{1,5}$/.test(ident)) continue;
  recs.push({ hz: Math.round(khz * 1000), ident, name: (r[iName] || '').trim().slice(0, 48), cc: (r[iCc] || '').slice(0, 2).toUpperCase(), lat, lon });
}
if (recs.length < 4000) die(`only ${recs.length} NDBs survived — the source has changed shape`);
recs.sort((a, b) => a.hz - b.hz || a.ident.localeCompare(b.ident) || a.lat - b.lat);

// one name blob, de-duplicated
const names = new Map(); let blob = '\\000'; let off = 1;   // offset 0 = "" (★ NUL as \000: a digit after it stays a digit)
const nameOff = (n) => {
  if (!n) return 0;
  if (names.has(n)) return names.get(n);
  const o = off; names.set(n, o);
  blob += cstr(n) + '\\000'; off += Buffer.byteLength(n, 'utf8') + 1;
  return o;
};
const lines = recs.map((r) => `{${r.hz},${r.lat.toFixed(5)}f,${r.lon.toFixed(5)}f,${nameOff(r.name)},"${r.ident}","${r.cc || '--'}"}`);
let src = `// vibe_ndb_navaids.h — GENERATED by scripts/gen-ndb-navaids.mjs. Do not edit.
//
// ★★ OurAirports navaids.csv — PUBLIC DOMAIN (David Megginson; https://ourairports.com/data/), retrieved ${date}:
//    ${recs.length} NDB / NDB-DME, 150–1750 kHz, sorted by frequency. The NDB skimmer's matching (vibe_ndb_db.h).
// ★ .rodata: paged in only when read, so it costs no RAM on a server that never runs the NDB mode.
#pragma once
#include <cstddef>
#include <cstdint>
namespace vibendb {
struct NavaidRec { uint32_t hz; float lat, lon; uint32_t name; char ident[6]; char cc[3]; };
constexpr size_t kNavaidCount = ${recs.length};
constexpr const char* kNavaidSource = "OurAirports navaids.csv (public domain), ${date}";
inline const NavaidRec kNavaids[kNavaidCount] = {
`;
for (let i = 0; i < lines.length; i += 4) src += lines.slice(i, i + 4).join(',') + ',\n';
src += `};
`;
// the blob, in pieces the compiler is happy with
src += 'inline const char kNavaidNames[] =\n';
const parts = blob.match(/(?:\\[0-7]{3}|[^\\]){1,200}/g) || [];
for (const p of parts) src += `"${p}"\n`;
src += ';\n} // namespace vibendb\n';

if (CHECK) {
  const have = await readFile(out, 'utf8').catch(() => '');
  if (have !== src) die('vibe_ndb_navaids.h has drifted from the cached source — run node scripts/gen-ndb-navaids.mjs');
  console.log(`vibe_ndb_navaids.h is current (${recs.length} NDBs)`);
} else {
  await writeFile(out, src);
  console.log(`wrote ${path.relative(root, out)}: ${recs.length} NDBs, ${(src.length / 1024).toFixed(0)} KB source, names ${(off / 1024).toFixed(0)} KB`);
}
