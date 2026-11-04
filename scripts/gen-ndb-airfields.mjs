/**
 * gen-ndb-airfields.mjs — the airfields an UNLISTED NDB may belong to, compiled into every server for the
 * "Suspected …" clue (android/app/src/main/cpp/vibe_ndb_airfields.h, read by vibe_ndb_suspect.h).
 *
 *   node scripts/gen-ndb-airfields.mjs            -> android/app/src/main/cpp/vibe_ndb_airfields.h
 *   node scripts/gen-ndb-airfields.mjs --check    -> fail if the checked-in table has drifted from the cached source
 *   node scripts/gen-ndb-airfields.mjs --refresh  -> re-download airports.csv (otherwise .mapdata-cache/ is used)
 *
 * ★★ SOURCE: OurAirports airports.csv — PUBLIC DOMAIN (dedicated by David Megginson), the same file
 *    gen-ndb-navaids.mjs's heard() step reads for the positions of our own receptions (one cache file, .mapdata-cache/).
 *
 * ★★ THE FILTER (2026-11-04, measured): large_airport + medium_airport + small_airport that carry a 4-letter ICAO
 *    location indicator (icao_code — gps_code would add 9 000 US strips with FAA-derived K-codes). ≈ 10 000 of the 72 000 rows. Why:
 *    – An NDB is an approach / en-route aid published in an AIP; an airfield that has one has an ICAO indicator. The
 *      42 000 small airstrips without one (farm strips, private fields) do not host NDBs — and including them makes
 *      the name-letters clue find a "match" for any three letters (the absurd-match risk, not just size).
 *    – Heliports are left out (23 000 rows, ≈ 400 with an ICAO code): offshore-platform NDBs are in the navaid lists
 *      already, and a helipad named after its hospital is never what a locator is named after.
 *    – Closed airfields, seaplane bases and balloon ports: out.
 *    Each record: ICAO, IATA, name (≤ 40 bytes), country, position, size class. 10 091 records on 2026-11-04: 709 KB of source, ≈ 0.43 MB .rodata (20 B a record + 227 KB of names)
 *    — paged in only when an unlisted ident is heard.
 */
import { readFile, writeFile, mkdir, stat } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const cacheDir = path.join(root, '.mapdata-cache');
const out = path.join(root, 'android/app/src/main/cpp/vibe_ndb_airfields.h');
const CHECK = process.argv.includes('--check');
const REFRESH = process.argv.includes('--refresh');
const UA = 'VibeSDR-gen-ndb-airfields/1.0 (NDB suspect clue build; https://vibesdr.net)';
const SRC = 'https://davidmegginson.github.io/ourairports-data/airports.csv';

function die(m) { console.error(`gen-ndb-airfields: ${m}`); process.exit(1); }
async function cached(name, fetcher) {
  const file = path.join(cacheDir, name);
  if (!REFRESH) {
    const have = await stat(file).catch(() => null);
    if (have && have.size > 0) return { file, date: have.mtime.toISOString().slice(0, 10) };
  }
  await mkdir(cacheDir, { recursive: true });
  await fetcher(file);
  return { file, date: new Date().toISOString().slice(0, 10) };
}
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
function cstr(s) {
  let o = '';
  for (const b of Buffer.from(s, 'utf8')) {
    if (b >= 0x20 && b < 0x7f && b !== 0x22 && b !== 0x5c && b !== 0x3f) o += String.fromCharCode(b);
    else o += '\\' + b.toString(8).padStart(3, '0');
  }
  return o;
}
/** Trim to ≤ n UTF-8 bytes without cutting a character. */
function clip(s, n) { while (Buffer.byteLength(s, 'utf8') > n) s = s.slice(0, -1); return s.trim(); }

const { file, date } = await cached('airports.csv', async (f) => {
  const res = await fetch(SRC, { headers: { 'User-Agent': UA } }).catch((e) => die(`${SRC} unreachable (${e.message})`));
  if (!res.ok) die(`${SRC} returned HTTP ${res.status}`);
  const text = await res.text();
  if (text.length < 1000000) die(`airports.csv is only ${text.length} bytes — the source has changed shape`);
  await writeFile(f, text);
});
const rows = parseCsv(await readFile(file, 'utf8'));
const h = rows.shift();
const need = ['ident', 'type', 'name', 'latitude_deg', 'longitude_deg', 'iso_country', 'icao_code', 'iata_code', 'gps_code'];
for (const n of need) if (!h.includes(n)) die(`airports.csv: no column "${n}" — the source has changed shape`);
const col = Object.fromEntries(h.map((k, i) => [k, i]));
const SIZE = { large_airport: 2, medium_airport: 1, small_airport: 0 };
const ICAO = /^[A-Z]{4}$/;
const recs = [];
for (const r of rows) {
  if (r.length < h.length) continue;
  const size = SIZE[r[col.type]];
  if (size === undefined) continue;
  const icao = ICAO.test(r[col.icao_code]) ? r[col.icao_code] : '';   // ★ icao_code only: gps_code adds 9 000 US FAA K-codes for strips
  if (!icao) continue;
  const lat = Number(r[col.latitude_deg]), lon = Number(r[col.longitude_deg]);
  if (!Number.isFinite(lat) || !Number.isFinite(lon)) continue;
  const iata = /^[A-Z]{3}$/.test(r[col.iata_code]) ? r[col.iata_code] : '';
  recs.push({ icao, iata, name: clip(r[col.name] || '', 40), cc: (r[col.iso_country] || '--').slice(0, 2).toUpperCase(), lat, lon, size });
}
if (recs.length < 8000) die(`only ${recs.length} airfields survived — the source has changed shape`);
recs.sort((a, b) => a.lat - b.lat || a.icao.localeCompare(b.icao));   // ★ by latitude: the lookup is a band search

const names = new Map(); let blob = '\\000'; let off = 1;
const nameOff = (n) => {
  if (!n) return 0;
  if (names.has(n)) return names.get(n);
  const o = off; names.set(n, o);
  blob += cstr(n) + '\\000'; off += Buffer.byteLength(n, 'utf8') + 1;
  return o;
};
const lines = recs.map((r) => `{${r.lat.toFixed(3)}f,${r.lon.toFixed(3)}f,${nameOff(r.name)},"${r.icao}","${r.iata}","${r.cc}",${r.size}}`);
let src = `// vibe_ndb_airfields.h — GENERATED by scripts/gen-ndb-airfields.mjs. Do not edit.
//
// ★★ ${recs.length} airfields with an ICAO indicator (large / medium / small airports), sorted by latitude — the
//    "Suspected …" clue for an NDB heard but not in the published list (vibe_ndb_suspect.h).
//    Source: OurAirports airports.csv (public domain), retrieved ${date}.
// ★ .rodata: paged in only when an unlisted ident is heard.
#pragma once
#include <cstddef>
#include <cstdint>
namespace vibendb {
struct AirfieldRec { float lat, lon; uint32_t name; char icao[5]; char iata[4]; char cc[3]; uint8_t size; };
constexpr size_t kAirfieldCount = ${recs.length};
constexpr const char* kAirfieldSource = "OurAirports airports.csv (public domain) ${date}";
inline const AirfieldRec kAirfields[kAirfieldCount] = {
`;
for (let i = 0; i < lines.length; i += 4) src += lines.slice(i, i + 4).join(',') + ',\n';
src += '};\ninline const char kAirfieldNames[] =\n';
for (const p of blob.match(/(?:\\[0-7]{3}|[^\\]){1,200}/g) || []) src += `"${p}"\n`;
src += ';\n} // namespace vibendb\n';

if (CHECK) {
  const have = await readFile(out, 'utf8').catch(() => '');
  if (have !== src) die('vibe_ndb_airfields.h has drifted from the cached source — run node scripts/gen-ndb-airfields.mjs');
  console.log(`vibe_ndb_airfields.h is current (${recs.length} airfields)`);
} else {
  await writeFile(out, src);
  console.log(`wrote ${path.relative(root, out)}: ${recs.length} airfields, ${(src.length / 1024).toFixed(0)} KB source, names ${(off / 1024).toFixed(0)} KB`);
}
