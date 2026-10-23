// gen-synop-stations.mjs — WHICH WEATHER STATION A SYNOP REPORT CAME FROM, FROM ONE PUBLIC-DOMAIN TABLE (V12, 2026-10-23).
//
// ★★★ WHY. A SYNOP report names its station by a five-figure WMO number ("03772"); a listener wants "London Heathrow,
//     United Kingdom". The server's RTTY decoder translates the report (vibe_synop.h) and names the station from this
//     table — the clients only show what the server sends ("a much wider range of devices can listen than can host").
// ★★★ NEVER INVENTED. The names come from ONE public-domain list: NOAA NCEI's Integrated Surface Database station history
//     (isd-history.csv + country-list.txt, https://www.ncei.noaa.gov/pub/data/noaa/ — a US Government work, public
//     domain). A number that is not in it is shown as the number alone. Nothing is typed in by hand.
// ★★ WHAT IS KEPT (the filter, in the open):
//      • a USAF id of six digits ending in 0 — the ISD's spelling of a WMO block + station number (USAF = WMO × 10);
//        ids beginning 99 (ISD's buoys and ships) and 00 (no WMO number) are not WMO stations and are dropped;
//      • whose record ENDS in 2010 or later — a station silent for fifteen years is not on the air, and an old number
//        since given to another place must not wear the old name;
//      • the LATEST record when a number appears more than once (a station moved or renamed: today's name).
//    Names are title-cased from the ISD's capitals ("LONDON HEATHROW" → "London Heathrow") — case only, never spelling.
// ★★ SIZE (the Pi 2 is the target): ids + offsets are 8 bytes a station, names ~16 — see the header's own count. Compiled
//    in as read-only data: the kernel pages it from the binary only when a lookup touches it (a binary search touches
//    ~14 ids and one name), so it costs no RAM until a SYNOP is heard. No client carries it.
//
// usage: node scripts/gen-synop-stations.mjs                         write vibe_synop_stations.h from the committed TSV
//        node scripts/gen-synop-stations.mjs --check                 exit 1 if the header is stale (run-tests.sh)
//        node scripts/gen-synop-stations.mjs --from <isd-history.csv> <country-list.txt> <fetched YYYY-MM-DD>
//                                                                    refresh the TSV from a new NCEI download, then write
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const TSV = path.join(root, 'scripts/synop-stations/wmo-stations.tsv');
const OUT = path.join(root, 'android/app/src/main/cpp/vibe_synop_stations.h');
const SOURCE = 'NOAA NCEI Integrated Surface Database station history (isd-history.csv, country-list.txt), '
             + 'https://www.ncei.noaa.gov/pub/data/noaa/ — a US Government work, public domain';

// ★ Words the ISD spells as initials, kept in capitals by the title-casing (case only — never a spelling change).
const KEEP_CAPS = new Set(['AFB', 'RAF', 'RAAF', 'AWS', 'AMOS', 'AP', 'LV', 'NAS', 'MCAS', 'USA', 'UK', 'II', 'III', 'IV',
  'CG', 'DOE', 'AB', 'AAF', 'AGL', 'NWS', 'WSO', 'RCS', 'CO', 'BC', 'NWT', 'SAR', 'DMI', 'TV', 'USAF', 'ANG', 'GTS']);
function titleCase(s) {
  return s.toLowerCase().replace(/[a-z0-9']+/g, (w, at) => {
    if (KEEP_CAPS.has(w.toUpperCase())) return w.toUpperCase();
    if (at > 0 && (w === 'and' || w === 'of' || w === 'the')) return w;
    return w.charAt(0).toUpperCase() + w.slice(1);
  }).replace(/\s+/g, ' ').trim();
}

/** A minimal CSV line reader — the ISD file quotes every field and none holds a quote. */
function csvLine(l) {
  const out = []; let cur = '', q = false;
  for (const ch of l) {
    if (ch === '"') q = !q;
    else if (ch === ',' && !q) { out.push(cur); cur = ''; }
    else cur += ch;
  }
  out.push(cur);
  return out;
}

function refresh(csvPath, countryPath, fetched) {
  if (!/^\d{4}-\d{2}-\d{2}$/.test(fetched || '')) { console.error('--from needs the download date, YYYY-MM-DD'); process.exit(2); }
  const countries = new Map();
  for (const l of fs.readFileSync(countryPath, 'latin1').split(/\r?\n/)) {
    const m = /^([A-Z0-9]{2})\s+(\S.*?)\s*$/.exec(l);
    if (m) countries.set(m[1], titleCase(m[2]));   // ★ the column heading ("FIPS ID …") does not match: 'FIPS' is 4 letters
  }
  const lines = fs.readFileSync(csvPath, 'latin1').split(/\r?\n/);
  const head = csvLine(lines[0]);
  const col = (n) => { const i = head.indexOf(n); if (i < 0) throw new Error('isd-history.csv: no column ' + n); return i; };
  const cU = col('USAF'), cN = col('STATION NAME'), cC = col('CTRY'), cE = col('END');
  const best = new Map();
  /* ★★★ THE SOURCE MIXES TWO COUNTRY ALPHABETS (found 2026-10-23). isd-history.csv's CTRY is FIPS 10-4 ("UK", "GM")
   *  in most rows, but some recent rows carry ISO 3166 ("GB", "DE"): LONDON HEATHROW is "GB", which country-list.txt —
   *  FIPS — reads as GABON. So a code is only believed where it BELONGS: each code's home WMO block is the block most
   *  of its rows are in, and a code used in another block, rarely (under 10 % of that block's commonest code), is the
   *  other alphabet — that station gets NO country rather than a wrong one (Heathrow: "London Heathrow", not "…,
   *  Gabon"). A code country-list.txt does not know ("DE") gets none either. Nothing is guessed in its place. */
  const blockCode = new Map();   // "03|UK" → rows
  for (const l of lines.slice(1)) {
    const f = csvLine(l);
    if (!/^\d{5}0$/.test(f[cU] || '')) continue;
    const k = f[cU].slice(0, 2) + '|' + (f[cC] || '').trim();
    blockCode.set(k, (blockCode.get(k) || 0) + 1);
  }
  const homeBlock = new Map(), blockTop = new Map();
  { const byCode = new Map();
    for (const [k, n] of blockCode) {
      const [b, c] = k.split('|');
      if (!c) continue;
      if (!byCode.has(c) || byCode.get(c).n < n) byCode.set(c, { b, n });
      blockTop.set(b, Math.max(blockTop.get(b) || 0, n));
    }
    for (const [c, v] of byCode) homeBlock.set(c, v.b); }
  const believe = (block, code) => {
    if (!code || !countries.has(code)) return false;
    if (homeBlock.get(code) === block) return true;
    return (blockCode.get(block + '|' + code) || 0) >= 0.1 * (blockTop.get(block) || 0);
  };
  let disbelieved = 0;
  for (const l of lines.slice(1)) {
    if (!l.trim()) continue;
    const f = csvLine(l);
    const usaf = f[cU], name = (f[cN] || '').trim(), end = f[cE] || '';
    if (!/^\d{5}0$/.test(usaf) || usaf.startsWith('99') || usaf.startsWith('00') || !name) continue;
    if (end.slice(0, 4) < '2010') continue;
    const wmo = usaf.slice(0, 5);
    const prev = best.get(wmo);
    if (!prev || end > prev.end) best.set(wmo, { end, name, ctry: (f[cC] || '').trim() });
  }
  const ids = [...best.keys()].sort();
  const rows = [];
  for (const id of ids) {
    const r = best.get(id);
    const ok = believe(id.slice(0, 2), r.ctry);
    if (r.ctry && !ok) disbelieved++;
    rows.push(`${id}\t${titleCase(r.name).replace(/\t/g, ' ')}\t${ok ? countries.get(r.ctry) : ''}`);
  }
  console.log(`synop stations: ${disbelieved} country codes not believed (the other alphabet, or unknown) — shown without a country`);
  const hdr = [
    `# wmo-stations.tsv — WMO block+station number, name, country. GENERATED by scripts/gen-synop-stations.mjs --from.`,
    `# Source: ${SOURCE}.`,
    `# Fetched: ${fetched}. Filter: USAF ids NNNNN0 (not 99/00), record ending 2010 or later, the latest record per number.`,
    `# Names title-cased from the source's capitals; nothing else changed. ${rows.length} stations.`,
  ];
  fs.mkdirSync(path.dirname(TSV), { recursive: true });
  fs.writeFileSync(TSV, hdr.join('\n') + '\n' + rows.join('\n') + '\n');
  console.log(`synop stations: ${rows.length} written to ${path.relative(root, TSV)}`);
}

function cstr(s) {
  // ★ Plain ASCII escapes only — the header must compile identically everywhere (UTF-8 kept as \x bytes).
  let o = '';
  for (const b of Buffer.from(s, 'utf8')) {
    if (b === 0x22 || b === 0x5c) o += '\\' + String.fromCharCode(b);
    else if (b >= 0x20 && b < 0x7f && b !== 0x3f) o += String.fromCharCode(b);   // ★ no '?' — trigraphs
    else o += '\\' + b.toString(8).padStart(3, '0');
  }
  return o;
}

function header() {
  const txt = fs.readFileSync(TSV, 'utf8').split('\n');
  const meta = txt.filter((l) => l.startsWith('#'));
  const fetched = (/Fetched: (\d{4}-\d{2}-\d{2})/.exec(meta.join('\n')) || [])[1] || '?';
  const rows = txt.filter((l) => l && !l.startsWith('#')).map((l) => l.split('\t'));
  const countries = [...new Set(rows.map((r) => r[2] || ''))].sort();
  if (countries.length > 65535) throw new Error('too many countries');
  const cIdx = new Map(countries.map((c, i) => [c, i]));
  let names = '', nameBytes = 0;
  const ids = [], offs = [], ctry = [];
  let last = -1;
  for (const [id, name, c] of rows) {
    const n = Number(id);
    if (!/^\d{5}$/.test(id) || n <= last) throw new Error('wmo-stations.tsv: ids must be 5 digits, sorted, unique: ' + id);
    last = n;
    ids.push(n); offs.push(nameBytes); ctry.push(cIdx.get(c || ''));
    names += cstr(name) + '\\0';
    nameBytes += Buffer.byteLength(name, 'utf8') + 1;
  }
  let cBytes = 0; const cOffs = [];
  for (const c of countries) { cOffs.push(cBytes); cBytes += Buffer.byteLength(c, 'utf8') + 1; }
  const wrap = (arr, per) => { const o = []; for (let i = 0; i < arr.length; i += per) o.push('    ' + arr.slice(i, i + per).join(',') + ','); return o.join('\n'); };
  // ★ The names as one string literal, cut into ~110-character source lines (adjacent literals concatenate).
  const nameLines = []; { let cur = ''; for (const piece of names.split(/(?<=\\0)/)) { if (cur.length + piece.length > 110) { nameLines.push(cur); cur = ''; } cur += piece; } if (cur) nameLines.push(cur); }
  const bytes = rows.length * 4 * 2 + rows.length * 2 + nameBytes + cBytes + countries.length * 4;
  return `// vibe_synop_stations.h — GENERATED by scripts/gen-synop-stations.mjs from scripts/synop-stations/wmo-stations.tsv. Do not edit.
// Source: ${SOURCE}.
// Fetched ${fetched}. ${rows.length} WMO stations, ${countries.length} countries; ~${Math.round(bytes / 1024)} KB of read-only data.
// ★ Read by vibe_synop.h (the RTTY SYNOP/SHIP translator). A number not in the table is shown as the number only.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace vibe {
namespace synop_stations {

static constexpr int kCount = ${rows.length};
static constexpr int kCountries = ${countries.length};
/** WMO block + station numbers, ascending. */
static const uint32_t kIds[kCount] = {
${wrap(ids, 14)}
};
/** Byte offset of each station's name in kNames. */
static const uint32_t kNameAt[kCount] = {
${wrap(offs, 14)}
};
/** Index of each station's country in kCountryAt / kCountryNames ("" = the source gives none). */
static const uint16_t kCountryOf[kCount] = {
${wrap(ctry, 24)}
};
static const uint32_t kCountryAt[kCountries] = {
${wrap(cOffs, 14)}
};
static const char kCountryNames[] =
${countries.map((c) => '    "' + cstr(c) + '\\0"').join('\n')};
static const char kNames[] =
${nameLines.map((l) => '    "' + l + '"').join('\n')};

/** The station's name and country, or false when the number is not in the public list. */
inline bool lookup(uint32_t wmo, std::string& name, std::string& country) {
    const uint32_t* e = kIds + kCount;
    const uint32_t* p = std::lower_bound(kIds, e, wmo);
    if (p == e || *p != wmo) return false;
    const size_t i = (size_t)(p - kIds);
    name = kNames + kNameAt[i];
    country = kCountryNames + kCountryAt[kCountryOf[i]];
    return true;
}

}  // namespace synop_stations
}  // namespace vibe
`;
}

const argv = process.argv.slice(2);
if (argv[0] === '--from') refresh(argv[1], argv[2], argv[3]);
const body = header();
const cur = fs.existsSync(OUT) ? fs.readFileSync(OUT, 'utf8') : '';
const check = argv.includes('--check');
const stale = cur !== body;
if (check) {
  if (stale) console.error(`  ✗ ${path.relative(root, OUT)} is stale — run node scripts/gen-synop-stations.mjs`);
} else if (stale) fs.writeFileSync(OUT, body);
console.log(`synop stations: header ${stale ? (check ? 'STALE' : 'written') : 'up to date'} (${body.length} bytes of source)`);
process.exit(check && stale ? 1 : 0);
