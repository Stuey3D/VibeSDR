/**
 * gen-vor-navaids.mjs — the published VORs (and the few ILS localisers open data lists), compiled into every server for the
 * VOR / ILS decoder's station names, positions and declination (android/app/src/main/cpp/vibe_vor_navaids.h).
 *
 *   node scripts/gen-vor-navaids.mjs             -> android/app/src/main/cpp/vibe_vor_navaids.h
 *   node scripts/gen-vor-navaids.mjs --check     -> fail if the checked-in table has drifted from the cached sources
 *   node scripts/gen-vor-navaids.mjs --refresh   -> re-download (otherwise .mapdata-cache/ is used)
 *
 * ★★★ SOURCES — ONLY DATA WE MAY BUNDLE WITHOUT ASKING ANYONE (the NDB list's rule, docs/v12/NDB-SKIMMER.md):
 *   OurAirports (PUBLIC DOMAIN, dedicated by David Megginson):
 *     navaids.csv             — every VOR, VOR-DME and VORTAC with an ident, frequency and position; the DME channel; the
 *                               station's SLAVED variation (the declination its radials are aligned to) where given, else
 *                               the magnetic variation at the site. ~3 650 stations worldwide.
 *     airport-frequencies.csv — rows typed ILS / LOC with a frequency on the localiser raster (108.10–111.95, odd tenths):
 *     airports.csv              the airport's position and name for them. ★ Only ~20 worldwide: national AIPs (the UK's is
 *                               Crown copyright) are NOT bundled, so most localisers are shown without a runway (said so).
 * ★ Sorted by frequency (the lookup is a range search). Fails LOUDLY on a source that changed shape.
 */
import { readFile, writeFile, mkdir, stat } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const cacheDir = path.join(root, '.mapdata-cache');
const out = path.join(root, 'android/app/src/main/cpp/vibe_vor_navaids.h');
const CHECK = process.argv.includes('--check');
const REFRESH = process.argv.includes('--refresh');
const UA = 'VibeSDR-gen-vor-navaids/1.0 (VOR station list build; https://vibesdr.net)';
const OA = 'https://davidmegginson.github.io/ourairports-data/';

function die(m) { console.error(`gen-vor-navaids: ${m}`); process.exit(1); }
async function cached(name, minBytes) {
  const file = path.join(cacheDir, name);
  if (!REFRESH) {
    const have = await stat(file).catch(() => null);
    if (have && have.size > 0) return { file, date: have.mtime.toISOString().slice(0, 10) };
  }
  if (CHECK) die(`${name} is not cached — run without --check first`);
  await mkdir(cacheDir, { recursive: true });
  let res;
  try { res = await fetch(OA + name, { redirect: 'follow', headers: { 'User-Agent': UA } }); } catch (e) { die(`${name} unreachable (${e.message})`); }
  if (!res.ok) die(`${name} returned HTTP ${res.status}`);
  const text = await res.text();
  if (text.length < minBytes) die(`${name} is only ${text.length} bytes — the source has changed shape`);
  await writeFile(file, text);
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
function csvObjects(text, need, label) {
  const rows = parseCsv(text);
  const h = rows.shift();
  for (const n of need) if (!h.includes(n)) die(`${label}: no column "${n}" — the source has changed shape`);
  return rows.filter((r) => r.length >= h.length).map((r) => Object.fromEntries(h.map((k, i) => [k, r[i]])));
}
function cstr(s) {
  let o = '';
  for (const b of Buffer.from(s, 'utf8')) {
    if (b >= 0x20 && b < 0x7f && b !== 0x22 && b !== 0x5c && b !== 0x3f) o += String.fromCharCode(b);
    else o += '\\' + b.toString(8).padStart(3, '0');
  }
  return o;
}
const IDENT = /^[A-Z0-9]{1,4}$/;
const fmt = (v, d) => (Math.round(v * 10 ** d) / 10 ** d).toFixed(d);

const nav = await cached('navaids.csv', 100000);
const freqs = await cached('airport-frequencies.csv', 100000);
const apts = await cached('airports.csv', 1000000);
const rows = csvObjects(await readFile(nav.file, 'utf8'),
  ['ident', 'name', 'type', 'frequency_khz', 'latitude_deg', 'longitude_deg', 'iso_country', 'dme_channel', 'slaved_variation_deg', 'magnetic_variation_deg'], 'navaids.csv');
const recs = [];
for (const r of rows) {
  if (!['VOR', 'VOR-DME', 'VORTAC'].includes(r.type)) continue;
  const khz = Number(r.frequency_khz), lat = Number(r.latitude_deg), lon = Number(r.longitude_deg);
  const ident = (r.ident || '').trim().toUpperCase();
  if (!(khz >= 108000 && khz <= 117975) || !IDENT.test(ident) || !Number.isFinite(lat) || !Number.isFinite(lon)) continue;
  const sl = r.slaved_variation_deg !== '' ? Number(r.slaved_variation_deg) : NaN;
  const mv = r.magnetic_variation_deg !== '' ? Number(r.magnetic_variation_deg) : NaN;
  const vari = Number.isFinite(sl) ? sl : Number.isFinite(mv) ? mv : 0;
  recs.push({ khz, lat, lon, ident, cc: (r.iso_country || '').slice(0, 2), name: (r.name || ident).trim(), dme: (r.dme_channel || '').trim().slice(0, 4),
              vari, slaved: Number.isFinite(sl), kind: 0, type: r.type });
}
if (recs.length < 2000) die(`only ${recs.length} VORs — the source has changed shape`);
// ILS localisers named in airport-frequencies.csv
const airport = new Map();
for (const a of csvObjects(await readFile(apts.file, 'utf8'), ['ident', 'name', 'latitude_deg', 'longitude_deg', 'iso_country'], 'airports.csv'))
  airport.set(a.ident, a);
let ils = 0;
for (const f of csvObjects(await readFile(freqs.file, 'utf8'), ['airport_ident', 'type', 'description', 'frequency_mhz'], 'airport-frequencies.csv')) {
  const t = `${f.type} ${f.description}`;
  if (!/\b(ILS|LOC|LLZ)\b/i.test(t)) continue;
  const khz = Math.round(Number(f.frequency_mhz) * 1000);
  if (!(khz >= 108100 && khz <= 111950)) continue;
  if ((Math.floor(khz / 100) % 10) % 2 !== 1) continue;   // the localiser raster: odd tenths
  const a = airport.get(f.airport_ident);
  if (!a) continue;
  const rwy = (t.match(/(?:RWY|RW|Rw|pista|RUNWAY)\s*([0-9]{2}[LRC]?(?:\/[0-9]{2}[LRC]?)?)/i) || t.match(/\bILS\s+([0-9]{2}[LRC]?)\b/i) || [])[1] || '';
  recs.push({ khz, lat: Number(a.latitude_deg), lon: Number(a.longitude_deg), ident: f.airport_ident.slice(0, 4), cc: (a.iso_country || '').slice(0, 2),
              name: `${a.name}${rwy ? ' runway ' + rwy.toUpperCase() : ''}`, dme: '', vari: 0, slaved: false, kind: 1 });
  ils++;
}
recs.sort((a, b) => a.khz - b.khz || a.ident.localeCompare(b.ident) || a.lat - b.lat);
// name pool
const names = []; const nameAt = new Map(); let off = 0;
for (const r of recs) { if (!nameAt.has(r.name)) { nameAt.set(r.name, off); names.push(r.name); off += Buffer.byteLength(r.name, 'utf8') + 1; } }
const vors = recs.filter((r) => r.kind === 0).length;
let h = `// vibe_vor_navaids.h — GENERATED by scripts/gen-vor-navaids.mjs. Do not edit.
//
// ★★ ${vors} VOR / VOR-DME / VORTAC (108–117.975 MHz) and ${ils} ILS localisers, sorted by frequency — the VOR / ILS decoder's
//    station list (docs/v12/VOR-ILS.md). Source: OurAirports (public domain), navaids.csv ${nav.date},
//    airport-frequencies.csv ${freqs.date}, airports.csv ${apts.date}.
// ★ var = the station's SLAVED variation (deg, east +) — the declination its radials are aligned to — where OurAirports gives
//   it (slaved = 1), else the magnetic variation at the site. A radial's TRUE bearing = magnetic radial + var.
// ★ kind 0 = VOR, 1 = ILS localiser (position = the airport's; name says the runway when the source does).
// ★ .rodata: paged in only when the decoder first looks a station up.
#pragma once
#include <cstddef>
#include <cstdint>
namespace vibevor {
struct StationRec { uint32_t khz; float lat, lon, var; uint32_t name; char ident[5]; char cc[3]; char dme[5]; uint8_t kind, slaved; };
constexpr size_t kStationCount = ${recs.length};
constexpr const char* kStationCredit = "OurAirports (public domain)";
inline const StationRec kStations[kStationCount] = {
`;
const lines = [];
for (const r of recs)
  lines.push(`{${r.khz},${fmt(r.lat, 5)}f,${fmt(r.lon, 5)}f,${fmt(r.vari, 3)}f,${nameAt.get(r.name)},"${cstr(r.ident)}","${cstr(r.cc)}","${cstr(r.dme)}",${r.kind},${r.slaved ? 1 : 0}}`);
for (let i = 0; i < lines.length; i += 4) h += lines.slice(i, i + 4).join(',') + (i + 4 < lines.length ? ',\n' : '\n');
h += `};
inline const char kStationNames[] =
`;
for (let i = 0; i < names.length; i += 8) h += '  "' + names.slice(i, i + 8).map((n) => cstr(n) + '\\0').join('') + '"\n';
h += `;
} // namespace vibevor
`;
if (CHECK) {
  const cur = await readFile(out, 'utf8').catch(() => '');
  if (cur !== h) die('vibe_vor_navaids.h has drifted from the cached sources — regenerate it');
  console.log(`gen-vor-navaids: up to date (${recs.length} stations)`);
} else {
  await writeFile(out, h);
  console.log(`gen-vor-navaids: ${vors} VORs + ${ils} localisers -> ${path.relative(root, out)} (${(h.length / 1024).toFixed(0)} KB source)`);
}
