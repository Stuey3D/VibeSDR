/**
 * gen-ndb-navaids.mjs — the published NDBs, compiled into every server for the NDB skimmer's matching
 * (android/app/src/main/cpp/vibe_ndb_navaids.h).
 *
 *   node scripts/gen-ndb-navaids.mjs             -> android/app/src/main/cpp/vibe_ndb_navaids.h
 *   node scripts/gen-ndb-navaids.mjs --check     -> fail if the checked-in table has drifted from the cached sources
 *   node scripts/gen-ndb-navaids.mjs --refresh   -> re-download every source (otherwise .mapdata-cache/ is used)
 *   node scripts/gen-ndb-navaids.mjs --stats     -> also print what each source added (and the 5–30 km merges)
 *
 * ★★★ SOURCES — ONLY DATA WE MAY BUNDLE WITHOUT ASKING ANYONE (Stuart, 2026-10-29: "any public sources please add them
 *     with the credits but any hobbyist sources that need me to write off and ask leave out"). Each is credited beside the
 *     beacon list; docs/v12/NDB-SKIMMER.md § "Beacon list sources" records every source considered, its licence and why
 *     it is in or out.
 *   0 OurAirports navaids.csv — PUBLIC DOMAIN (dedicated by David Megginson). The world list; the base.
 *   1 FAA NASR 28-day subscription, NAV_BASE.csv — US GOVERNMENT WORK, public domain (17 U.S.C. §105). The current
 *     cycle's US (and US-administered) NDBs: the most authoritative position for them; SHUTDOWN ones dropped.
 *   2 OpenStreetMap (Overpass: airmark=beacon + beacon:type=ndb) — ODbL 1.0, © OpenStreetMap
 *     contributors. ★★ ODbL: the table this script writes is a DERIVATIVE DATABASE, so it is offered under ODbL 1.0
 *     (the generated header says so; the other sources are public domain, which ODbL may carry). The app/server that
 *     reads it is a "Produced Work": any licence, with the credit. ★ An OSM element whose tags cite a hobbyist beacon
 *     list as ITS source is left out (the rule above, applied to what an OSM mapper copied in).
 *   Country of an OSM beacon: Natural Earth countries (public domain), point in polygon — the map pack's own cache.
 *
 * ★★ THE MERGE. One beacon is one record: the same ident, frequencies within 0.5 kHz and positions within 30 km are the
 *    same beacon (an ident is never reused that close on the same frequency — the 5 km the brief started from kept a
 *    mispositioned copy as a second beacon; --stats lists every 5–30 km merge). The position kept is the most
 *    authoritative: FAA (current cycle) > OurAirports > OSM. The name: OurAirports' (mixed case) when it has one.
 *    Every record carries its SOURCE (NavaidRec::src) — the credit can name where each beacon came from.
 *
 * ★★ WHY COMPILED IN, NOT A MAP-DATA FILE. The matching runs on the SERVER (decode on the server; clients display), on
 *    every server including the Android ones that have no map pack on disk. ~7 000 beacons pack to ≈ 250 KB of .rodata
 *    — paged in only when the NDB mode first looks one up (the ADS-B starter table's reasoning, vibe_adsb_starter.h).
 * ★ Only NDB and NDB-DME, 150–1750 kHz, with an ident and a position. Sorted by frequency (the lookup is a range search).
 * ★ Fails LOUDLY on a source that has changed shape (gen-map-data's rule): a silently empty table is a decoder that
 *   never names anything.
 */
import { readFile, writeFile, mkdir, stat } from 'node:fs/promises';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const cacheDir = path.join(root, '.mapdata-cache');
const out = path.join(root, 'android/app/src/main/cpp/vibe_ndb_navaids.h');
const CHECK = process.argv.includes('--check');
const REFRESH = process.argv.includes('--refresh');
const STATS = process.argv.includes('--stats');
const UA = 'VibeSDR-gen-ndb-navaids/1.0 (NDB beacon list build; https://vibesdr.net)';

const SRC_OA = 'https://davidmegginson.github.io/ourairports-data/navaids.csv';
const SRC_FAA_INDEX = 'https://www.faa.gov/air_traffic/flight_info/aeronav/aero_data/NASR_Subscription/';
const SRC_OVERPASS = 'https://overpass-api.de/api/interpreter';
// ★ ONE tag query, ONE request — never a scan of every aeroway=navigationaid (that timed out the public server, and
//   navigationaid=ndb had no elements on 2026-10-29: OSM maps NDBs as airmark=beacon + beacon:type=ndb).
const OVERPASS_Q = '[out:json][timeout:300];nwr["airmark"="beacon"]["beacon:type"~"ndb",i];out center tags;';
const SRC_NE = 'https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_50m_admin_0_countries.geojson';

/** The source tags, in NavaidRec::src order. The credit text the clients and the gallery show is built from these. */
const SOURCES = [
  { key: 'ourairports', credit: 'OurAirports (public domain)' },
  { key: 'faa', credit: 'FAA NASR (US Government, public domain)' },
  { key: 'osm', credit: '© OpenStreetMap contributors (ODbL)' },
];
const S_OA = 0, S_FAA = 1, S_OSM = 2;
/** Position precedence: lower wins. */
const RANK = { [S_FAA]: 0, [S_OA]: 1, [S_OSM]: 2 };

/** OSM elements whose own tags say they were copied from a hobbyist beacon list: left out (see the header). */
const HOBBYIST = /jdg\.it|classaxe|ndblist|rwwfinder|dxinfocentre|beaconworld|mil-airfields|ndb\.?list|radiobeacon/i;

function die(m) { console.error(`gen-ndb-navaids: ${m}`); process.exit(1); }

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

async function get(url, init = {}) {
  let res;
  try { res = await fetch(url, { redirect: 'follow', ...init, headers: { 'User-Agent': UA, ...(init.headers || {}) } }); }
  catch (e) { die(`${url} unreachable (${e.message})`); }
  if (!res.ok) die(`${url} returned HTTP ${res.status}`);
  return res;
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
function csvObjects(text, need, label) {
  const rows = parseCsv(text);
  const h = rows.shift();
  for (const n of need) if (!h.includes(n)) die(`${label}: no column "${n}" — the source has changed shape`);
  return rows.filter((r) => r.length >= h.length).map((r) => Object.fromEntries(h.map((k, i) => [k, r[i]])));
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

const IDENT = /^[A-Z0-9]{1,5}$/;
const okKhz = (k) => k >= 150 && k <= 1750;
const titleCase = (s) => s.toLowerCase().replace(/\b([a-z])/g, (m) => m.toUpperCase());

// ── 0 OurAirports ────────────────────────────────────────────────────────────────────────────────────────────────────
async function ourAirports() {
  const { file, date } = await cached('navaids.csv', async (f) => {
    const text = await (await get(SRC_OA)).text();
    if (text.length < 100000) die(`navaids.csv is only ${text.length} bytes — the source has changed shape`);
    await writeFile(f, text);
  });
  const rows = csvObjects(await readFile(file, 'utf8'), ['ident', 'name', 'type', 'frequency_khz', 'latitude_deg', 'longitude_deg', 'iso_country'], 'navaids.csv');
  const recs = [];
  for (const r of rows) {
    if (r.type !== 'NDB' && r.type !== 'NDB-DME') continue;
    const khz = Number(r.frequency_khz), lat = Number(r.latitude_deg), lon = Number(r.longitude_deg);
    const ident = (r.ident || '').toUpperCase().trim();
    if (!okKhz(khz) || !Number.isFinite(lat) || !Number.isFinite(lon) || !IDENT.test(ident)) continue;
    recs.push({ src: S_OA, hz: Math.round(khz * 1000), ident, name: (r.name || '').trim().slice(0, 48), cc: (r.iso_country || '').slice(0, 2).toUpperCase(), lat, lon });
  }
  if (recs.length < 4000) die(`OurAirports: only ${recs.length} NDBs survived — the source has changed shape`);
  return { recs, date };
}

// ── 1 FAA NASR ───────────────────────────────────────────────────────────────────────────────────────────────────────
async function faa() {
  const { file, date } = await cached('faa-NAV_BASE.csv', async (f) => {
    // The index lists the current and the next 28-day cycle (YYYY-MM-DD); the current is the latest not in the future.
    const idx = await (await get(SRC_FAA_INDEX)).text();
    const today = new Date().toISOString().slice(0, 10);
    const cycles = [...new Set([...idx.matchAll(/NASR_Subscription\/(\d{4}-\d{2}-\d{2})/g)].map((m) => m[1]))].filter((c) => c <= today).sort();
    if (!cycles.length) die('FAA NASR: no current cycle on the subscription page — the page has changed shape');
    const cyc = cycles.at(-1);
    const page = await (await get(SRC_FAA_INDEX + cyc)).text();
    const zipUrl = (page.match(/https:\/\/nfdc\.faa\.gov\/webContent\/28DaySub\/extra\/[^"]*_NAV_CSV\.zip/) || [])[0];
    if (!zipUrl) die(`FAA NASR ${cyc}: no NAV_CSV.zip link — the page has changed shape`);
    const zip = Buffer.from(await (await get(zipUrl)).arrayBuffer());
    const zf = f + '.zip';
    await writeFile(zf, zip);
    const csv = execFileSync('unzip', ['-p', zf, 'NAV_BASE.csv'], { maxBuffer: 64 << 20 });
    if (csv.length < 100000) die(`FAA NAV_BASE.csv is only ${csv.length} bytes — the source has changed shape`);
    await writeFile(f, csv);
  });
  const rows = csvObjects(await readFile(file, 'latin1'), ['EFF_DATE', 'NAV_ID', 'NAV_TYPE', 'NAV_STATUS', 'NAME', 'COUNTRY_CODE', 'LAT_DECIMAL', 'LONG_DECIMAL', 'FREQ'], 'FAA NAV_BASE.csv');
  const recs = []; let eff = '';
  for (const r of rows) {
    if (!/NDB/.test(r.NAV_TYPE) || /SHUTDOWN|DECOMMISSIONED/.test(r.NAV_STATUS)) continue;
    const khz = Number(r.FREQ), lat = Number(r.LAT_DECIMAL), lon = Number(r.LONG_DECIMAL);
    const ident = (r.NAV_ID || '').toUpperCase().trim();
    if (!okKhz(khz) || !Number.isFinite(lat) || !Number.isFinite(lon) || !IDENT.test(ident)) continue;
    eff = r.EFF_DATE.replace(/\//g, '-');
    recs.push({ src: S_FAA, hz: Math.round(khz * 1000), ident, name: titleCase((r.NAME || '').trim()).slice(0, 48), cc: (r.COUNTRY_CODE || '').slice(0, 2).toUpperCase(), lat, lon });
  }
  if (recs.length < 200) die(`FAA: only ${recs.length} NDBs survived — the source has changed shape`);
  return { recs, date: eff || date };
}

// ── 2 OpenStreetMap ──────────────────────────────────────────────────────────────────────────────────────────────────
/** "339 kHz", "331", "373.00 kHz", "0.336" (MHz), "426500 Hz" → kHz, or NaN. */
function osmKhz(v) {
  if (!v) return NaN;
  const m = String(v).replace(',', '.').match(/([0-9]+(?:\.[0-9]+)?)\s*(khz|mhz|hz)?/i);
  if (!m) return NaN;
  let x = Number(m[1]); const u = (m[2] || '').toLowerCase();
  if (u === 'mhz' || (!u && x < 2)) x *= 1000;
  else if (u === 'hz' || (!u && x > 100000)) x /= 1000;
  return x;
}
/** Natural Earth countries: [{iso, rings: [[[lon,lat]...]...], bbox}] for point-in-polygon. */
async function countries() {
  const { file } = await cached('countries50.geojson', async (f) => {
    const text = await (await get(SRC_NE)).text();
    await writeFile(f, text);
  });
  const gj = JSON.parse(await readFile(file, 'utf8'));
  const cs = [];
  for (const ft of gj.features) {
    const iso = String(ft.properties.ISO_A2_EH ?? '').trim();
    if (!/^[A-Z]{2}$/.test(iso)) continue;
    const g = ft.geometry; const polys = g.type === 'Polygon' ? [g.coordinates] : g.type === 'MultiPolygon' ? g.coordinates : [];
    for (const p of polys) {
      let x0 = 180, x1 = -180, y0 = 90, y1 = -90;
      for (const [x, y] of p[0]) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); y0 = Math.min(y0, y); y1 = Math.max(y1, y); }
      cs.push({ iso, rings: p, bbox: [x0, y0, x1, y1] });
    }
  }
  if (cs.length < 200) die('countries50.geojson: too few countries — the source has changed shape');
  return cs;
}
function inRing(x, y, ring) {
  let c = false;
  for (let i = 0, j = ring.length - 1; i < ring.length; j = i++) {
    const [xi, yi] = ring[i], [xj, yj] = ring[j];
    if ((yi > y) !== (yj > y) && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) c = !c;
  }
  return c;
}
function countryAt(cs, lat, lon) {
  for (const c of cs) {
    const [x0, y0, x1, y1] = c.bbox;
    if (lon < x0 || lon > x1 || lat < y0 || lat > y1) continue;
    if (inRing(lon, lat, c.rings[0]) && !c.rings.slice(1).some((h) => inRing(lon, lat, h))) return c.iso;
  }
  return '';
}
async function osm() {
  const { file, date } = await cached('osm-ndb.json', async (f) => {
    const res = await get(SRC_OVERPASS, { method: 'POST', body: new URLSearchParams({ data: OVERPASS_Q }) });
    const text = await res.text();
    let j; try { j = JSON.parse(text); } catch { die('Overpass: not JSON (busy? try later — ONE request, never a loop)'); }
    if (!Array.isArray(j.elements) || j.elements.length < 300) die(`Overpass: only ${j.elements?.length} elements — the query or the server has changed`);
    await writeFile(f, text);
  });
  const j = JSON.parse(await readFile(file, 'utf8'));
  const cs = await countries();
  const recs = []; let hobby = 0, dead = 0, seen = new Set();
  for (const e of j.elements) {
    if (seen.has(e.type + e.id)) continue; seen.add(e.type + e.id);
    const t = e.tags || {};
    if (Object.entries(t).some(([k, v]) => /^(disused|abandoned|decom+is+ioned|removed|demolished|was)(:|$)/i.test(k) && v !== 'no')) { dead++; continue; }
    if (/^(closed|decommissioned|disused)$/i.test(t.operational_status || '')) { dead++; continue; }
    if (Object.values(t).some((v) => HOBBYIST.test(v))) { hobby++; continue; }
    const lat = e.lat ?? e.center?.lat, lon = e.lon ?? e.center?.lon;
    const khz = osmKhz(t['beacon:frequency'] || t.frequency || t['navigationaid:frequency']);
    const ident = String(t['beacon:code'] || t['navigationaid:ident'] || t.ref || '').toUpperCase().trim();
    if (!okKhz(khz) || !Number.isFinite(lat) || !Number.isFinite(lon) || !IDENT.test(ident)) continue;
    const name = String(t.name || t['beacon:name'] || '').trim().slice(0, 48);
    recs.push({ src: S_OSM, hz: Math.round(khz * 1000), ident, name, cc: countryAt(cs, lat, lon), lat, lon });
  }
  if (recs.length < 300) die(`OSM: only ${recs.length} NDBs survived — the query or the tags have changed`);
  return { recs, date, hobby, dead };
}

// ── the merge ────────────────────────────────────────────────────────────────────────────────────────────────────────
function km(a, b) {
  const r = Math.PI / 180, dp = (b.lat - a.lat) * r, dl = (b.lon - a.lon) * r;
  const s = Math.sin(dp / 2) ** 2 + Math.cos(a.lat * r) * Math.cos(b.lat * r) * Math.sin(dl / 2) ** 2;
  return 2 * 6371 * Math.asin(Math.min(1, Math.sqrt(s)));
}
const SAME_KHZ = 500, SAME_KM = 30;
function merge(lists) {
  const byIdent = new Map(); const all = []; const far = []; const refined = [];
  // ★ Highest precedence first, so a later (lower-precedence) duplicate never moves a better position.
  const flat = lists.flat().sort((a, b) => RANK[a.src] - RANK[b.src]);
  for (const r of flat) {
    const cands = byIdent.get(r.ident) || [];
    let best = null, bestKm = Infinity;
    for (const c of cands) {
      if (Math.abs(c.hz - r.hz) > SAME_KHZ) continue;
      const d = km(c, r); if (d < bestKm) { bestKm = d; best = c; }
    }
    if (best && bestKm <= SAME_KM) {
      best.srcs |= 1 << r.src;
      if (bestKm > 5) far.push(`${r.ident} ${(r.hz / 1000).toFixed(1)} ${SOURCES[best.src].key}↔${SOURCES[r.src].key} ${bestKm.toFixed(1)} km`);
      if (r.src === S_OA && r.name) best.name = r.name;         // OurAirports' mixed-case name over FAA's capitals
      if (!best.cc && r.cc) best.cc = r.cc;
      if (!best.name && r.name) best.name = r.name;
      // ★★ OurAirports gives NDB frequencies in WHOLE kHz only (not one of its 6 746 has a fraction, 2026-10-29): a
      //    half-kHz beacon sits there 500 Hz off. A finer frequency from another source wins — MIQ is 427 in OurAirports,
      //    426.5 in OSM, and was heard on 426.0 (the matcher's ±600 Hz reaches 426.5, not 427).
      if (best.src === S_OA && best.hz % 1000 === 0 && r.hz % 1000 !== 0) { refined.push(`${r.ident} ${best.hz / 1000}→${r.hz / 1000}`); best.hz = r.hz; }
      continue;
    }
    const n = { ...r, srcs: 1 << r.src };
    cands.push(n); byIdent.set(r.ident, cands); all.push(n);
  }
  return { all, far, refined };
}

// ── build ────────────────────────────────────────────────────────────────────────────────────────────────────────────
const oa = await ourAirports();
const fa = await faa();
const os = await osm();
const { all: recs, far, refined } = merge([oa.recs, fa.recs, os.recs]);
recs.sort((a, b) => a.hz - b.hz || a.ident.localeCompare(b.ident) || a.lat - b.lat);
const dates = [oa.date, fa.date, os.date];
const counts = [0, 0, 0], only = [0, 0, 0];
for (const r of recs) { counts[r.src]++; for (let i = 0; i < 3; i++) if (r.srcs === 1 << i) only[i]++; }

// one name blob, de-duplicated
const names = new Map(); let blob = '\\000'; let off = 1;   // offset 0 = "" (★ NUL as \000: a digit after it stays a digit)
const nameOff = (n) => {
  if (!n) return 0;
  if (names.has(n)) return names.get(n);
  const o = off; names.set(n, o);
  blob += cstr(n) + '\\000'; off += Buffer.byteLength(n, 'utf8') + 1;
  return o;
};
const lines = recs.map((r) => `{${r.hz},${r.lat.toFixed(5)}f,${r.lon.toFixed(5)}f,${nameOff(r.name)},"${r.ident}","${r.cc || '--'}",${r.src}}`);
const srcLines = SOURCES.map((s, i) => `//    ${i} ${s.credit}, retrieved ${dates[i]}: ${counts[i]} beacons`).join('\n');
let src = `// vibe_ndb_navaids.h — GENERATED by scripts/gen-ndb-navaids.mjs. Do not edit.
//
// ★★ ${recs.length} NDB / NDB-DME, 150–1750 kHz, sorted by frequency. The NDB skimmer's matching (vibe_ndb_db.h).
//    Merged from (NavaidRec::src — the source whose POSITION each record carries):
${srcLines}
// ★★ DATABASE LICENCE: this table contains OpenStreetMap data, so the table itself is made available under the Open
//    Database License 1.0 (https://opendatacommons.org/licenses/odbl/1-0/); the OurAirports and FAA records in it are
//    public domain. Credit: ${SOURCES.map((s) => s.credit).join('; ')}.
// ★ .rodata: paged in only when read, so it costs no RAM on a server that never runs the NDB mode.
#pragma once
#include <cstddef>
#include <cstdint>
namespace vibendb {
struct NavaidRec { uint32_t hz; float lat, lon; uint32_t name; char ident[6]; char cc[3]; uint8_t src; };
constexpr size_t kNavaidCount = ${recs.length};
/** NavaidRec::src → the credit for that source. */
inline const char* const kNavaidSourceNames[${SOURCES.length}] = { ${SOURCES.map((s) => `"${cstr(s.credit)}"`).join(', ')} };
constexpr const char* kNavaidSource = "${cstr(SOURCES.map((s, i) => `${s.credit} ${dates[i]}`).join('; '))}";
/** The one-line credit the clients and the gallery show beside the beacon list. */
constexpr const char* kNavaidCredit = "${cstr(SOURCES.map((s) => s.credit).join('; '))}";
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

const summary = `${recs.length} NDBs — ${SOURCES.map((s, i) => `${s.key} ${counts[i]} positions (${only[i]} only there)`).join(', ')}; OSM left out: ${os.hobby} cite a hobbyist list, ${os.dead} disused`;
if (STATS) { console.log(summary); console.log(`${far.length} merges at 5–30 km:\n  ` + far.join('\n  ')); console.log(`${refined.length} whole-kHz frequencies refined: ${refined.join(', ')}`); }
if (CHECK) {
  const have = await readFile(out, 'utf8').catch(() => '');
  if (have !== src) die('vibe_ndb_navaids.h has drifted from the cached sources — run node scripts/gen-ndb-navaids.mjs');
  console.log(`vibe_ndb_navaids.h is current (${recs.length} NDBs)`);
} else {
  await writeFile(out, src);
  console.log(`wrote ${path.relative(root, out)}: ${summary}; ${(src.length / 1024).toFixed(0)} KB source, names ${(off / 1024).toFixed(0)} KB`);
}
