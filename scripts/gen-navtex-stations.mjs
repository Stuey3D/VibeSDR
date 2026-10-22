// gen-navtex-stations.mjs — WHICH NAVTEX STATION SENT THIS, FROM ONE TABLE (V12 beta 4, 2026-10-22).
//
// ★★★ WHY. A NAVTEX header carries one letter for its transmitter (B1) — "ZCZC BA10" is station B. The same letter is
//     used by a dozen stations round the world; which one you heard depends on the FREQUENCY and on WHERE YOU ARE. The
//     gallery and the listener's NAVTEX box both know the dial and the receiving server's position, so both can say
//     "B · Oostende, Belgium 🇧🇪" instead of "Station B".
// ★★ ONE SOURCE OF TRUTH. scripts/navtex-stations/navtex-stations.json — parsed from NGA Pub. 117 (public domain; the
//    Admiralty List of Radio Signals is copyright and is NOT used) by scripts/navtex-stations/parse-pub117.py. This
//    writes the two readers, table AND resolver, side by side so the rule cannot drift:
//      src/utils/navtexStations.generated.ts                  — the app AND the browser (web/client imports src/utils)
//      android/app/src/main/cpp/vibe_navtex_stations.h        — the server's gallery titles
// ★★ THE RULE (resolve / navtexStation):
//      1. The band: the table frequency (424 / 490 / 518 / 4209.5 kHz) within ±3 kHz of the dial. None → no station.
//      2. The TEXT first: a candidate (same letter, same band) whose name the message spells ("OOSTENDERADIO",
//         "NCC-HAMBURG") is the station, wherever it is — it said so.
//      3. Otherwise the NEAREST candidate to the receiving server, if it is within reach: 1500 km on MF, 2500 km on HF.
//         ★ 1500, not 700: the Lenovo in Northamptonshire logged Norwegian warnings from Rogaland (813 km) and Orlandet
//           (1400 km), Swedish from Stockholm (1053 km) and Danish from Varberg (1004 km) on one evening — night-time
//           518 kHz goes that far, and every other station sharing those letters is 2000 km+ further.
//      4. ★★ NEVER GUESS: two candidates within reach and the second less than 1.5× as far as the first → AMBIGUOUS, the
//         letter only. (518 W from the Midlands: Corsen 520 km, Valentia 648 km — and the evening's W was an Irish gale
//         warning. The nearest would have named the wrong country.)
//      Nothing resolved → the letter only, as before ("Station B").
//
// usage: node scripts/gen-navtex-stations.mjs           write the generated files
//        node scripts/gen-navtex-stations.mjs --check   exit 1 if either is stale (run-tests.sh)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const src = JSON.parse(fs.readFileSync(path.join(root, 'scripts/navtex-stations/navtex-stations.json'), 'utf8'));

// ★ Pub. 117's country column → ISO 3166 code (for the flag) and the name we show. A territory keeps its own name and
//   takes its sovereign's flag where it has none of its own. A country missing here FAILS the generator — never a blank.
const COUNTRY = {
  'Algeria': ['DZ'], 'Argentina': ['AR'], 'Azores': ['PT'], 'Bahrain': ['BH'], 'Belgium': ['BE'], 'Bermuda': ['BM'],
  'Bulgaria': ['BG'], 'Canada': ['CA'], 'Canary Islands': ['ES'], 'Cape Verde': ['CV'], 'Chile': ['CL'], 'China': ['CN'],
  'Columbia': ['CO', 'Colombia'], 'Croatia': ['HR'], 'Cyprus': ['CY'], 'Ecuador': ['EC'], 'Egypt': ['EG'],
  'Estonia': ['EE'], 'Faroe Island': ['FO', 'Faroe Islands'], 'France': ['FR'], 'Germany': ['DE'], 'Greece': ['GR'],
  'Greenland': ['GL'], 'Guam': ['GU'], 'Iceland': ['IS'], 'India': ['IN'], 'Indonesia': ['ID'],
  'Iran lamic Republic of)': ['IR', 'Iran'],   // ★ the PDF's own wrap of "Iran (Islamic Republic of)"
  'Ireland': ['IE'], 'Israel': ['IL'], 'Italy': ['IT'], 'Japan': ['JP'], 'Madeira': ['PT'], 'Malaysia': ['MY'],
  'Malta': ['MT'], 'Mauritius': ['MU'], 'Morocco': ['MA'], 'Namibia': ['NA'], 'Netherlands': ['NL'],
  'Netherlands Antilles': ['CW', 'Curaçao'],   // ★ the one station listed there is Curaçao (PJC)
  'Nigeria': ['NG'], 'North Korea': ['KP'], 'Norway': ['NO'], 'Oman': ['OM'], 'Pakistan': ['PK'], 'Peru': ['PE'],
  'Philippines': ['PH'], 'Portugal': ['PT'], 'Romania': ['RO'], 'Russian Federation': ['RU', 'Russia'],
  'Saudi Arabia': ['SA'], 'Senegal': ['SN'], 'Singapore': ['SG'], 'South Africa': ['ZA'], 'South Korea': ['KR'],
  'Spain': ['ES'], 'Svalbard': ['SJ'], 'Sweden': ['SE'], 'Taiwan': ['TW'], 'Thailand': ['TH'], 'Tunisia': ['TN'],
  'Turkey': ['TR'], 'Ukraine': ['UA'], 'United Kingdom': ['GB'], 'United States': ['US'], 'Uruguay': ['UY'],
  'Vietnam': ['VN'],
};
// ★ A typo in the source that would hide the station from its own text ("NCC-HAMBURG"): corrected here, in the open.
const NAME_FIX = { 'Haburg': 'Hamburg' };

/** "Oostende (OST)" → "Oostende"; "Portsmouth/ CAMSLANT (NMN)" → "Portsmouth"; "Point Reyes RCF (San Francisco)
 *  CAMSPAC (NMC)" → "Point Reyes". The call sign and the controlling station are not what a listener calls it. */
function shortName(n) {
  let s = n.replace(/\([^)]*\)/g, ' ').split('/')[0];
  s = s.replace(/\b(CAMSLANT|CAMSPAC|RCF)\b/g, ' ').replace(/\s+/g, ' ').trim();
  for (const [from, to] of Object.entries(NAME_FIX)) if (s === from) s = to;
  return s;
}

const rows = [];
const problems = [];
for (const s of src.stations) {
  const c = COUNTRY[s.country];
  if (!c) { problems.push(`no ISO code for Pub. 117 country "${s.country}"`); continue; }
  const name = shortName(s.name);
  if (!name) { problems.push(`a station at ${s.lat}, ${s.lon} has no name`); continue; }
  for (const t of s.tx) rows.push({ b1: t.b1, khz: t.khz, name, iso: c[0], country: c[1] ?? s.country, lat: s.lat, lon: s.lon });
}
if (problems.length || rows.length < 150) {
  for (const p of problems) console.error('  ✗ ' + p);
  console.error(`  ✗ ${rows.length} transmitters — the station table's shape changed`);
  process.exit(1);
}
rows.sort((a, b) => a.b1.localeCompare(b.b1) || a.khz - b.khz || a.name.localeCompare(b.name));

const HEAD = `GENERATED by scripts/gen-navtex-stations.mjs from scripts/navtex-stations/navtex-stations.json. Do not edit.
// Source: ${src.source}
// Edition: ${src.edition}`;

const ts = `// navtexStations.generated.ts — ${HEAD}
// ★ The rule is explained in the generator's header; vibe_navtex_stations.h is the server's copy of the same rule.

export interface NavtexStation { b1: string; khz: number; name: string; iso: string; country: string; lat: number; lon: number }

export const NAVTEX_STATIONS: readonly NavtexStation[] = [
${rows.map((r) => `  { b1: '${r.b1}', khz: ${r.khz}, name: ${JSON.stringify(r.name)}, iso: '${r.iso}', country: ${JSON.stringify(r.country)}, lat: ${r.lat}, lon: ${r.lon} },`).join('\n')}
];

/** The NAVTEX channels: 424 (Japan), 490 (national), 518 (international), 4209.5 kHz (HF). */
export const NAVTEX_BANDS_KHZ: readonly number[] = [424, 490, 518, 4209.5];
export const NAVTEX_REACH_MF_KM = 1500;
export const NAVTEX_REACH_HF_KM = 2500;
export const NAVTEX_AMBIGUOUS_RATIO = 1.5;

function km(la1: number, lo1: number, la2: number, lo2: number): number {
  const r = Math.PI / 180;
  const a = Math.sin(((la2 - la1) * r) / 2) ** 2 + Math.cos(la1 * r) * Math.cos(la2 * r) * Math.sin(((lo2 - lo1) * r) / 2) ** 2;
  return 2 * 6371 * Math.asin(Math.min(1, Math.sqrt(a)));
}
// ★ ASCII letters only, exactly as the server's letters(): "Curaçao" is CURAAO on both sides.
const letters = (s: string) => s.toUpperCase().replace(/[^A-Z]/g, '');

/** The band a dial is on (±3 kHz — a USB dial sits half a kHz below the carrier), or null. */
export function navtexBandKhz(dialHz: number): number | null {
  if (!Number.isFinite(dialHz)) return null;
  for (const b of NAVTEX_BANDS_KHZ) if (Math.abs(dialHz / 1000 - b) <= 3) return b;
  return null;
}

/** Which station sent letter \`b1\`, heard on \`dialHz\` at a server at \`lat\`/\`lon\` (NaN = unknown), with the message
 *  \`text\` to corroborate. null = the letter only (unknown band, nothing within reach, or ambiguous). */
export function navtexStation(b1: string | null | undefined, dialHz: number, lat: number, lon: number, text = ''): NavtexStation | null {
  if (!b1 || !/^[A-Z]$/.test(b1)) return null;
  const band = navtexBandKhz(dialHz);
  if (band == null) return null;
  const cands = NAVTEX_STATIONS.filter((s) => s.b1 === b1 && s.khz === band);
  if (!cands.length) return null;
  if (text) {
    const t = letters(text);
    const named = cands.filter((s) => letters(s.name).length >= 5 && t.includes(letters(s.name)));
    if (named.length === 1) return named[0];
  }
  if (!Number.isFinite(lat) || !Number.isFinite(lon)) return null;
  const reach = band > 1000 ? NAVTEX_REACH_HF_KM : NAVTEX_REACH_MF_KM;
  const near = cands.map((s) => ({ s, d: km(lat, lon, s.lat, s.lon) })).filter((x) => x.d <= reach).sort((a, b) => a.d - b.d);
  if (!near.length) return null;
  if (near.length > 1 && near[1].d < NAVTEX_AMBIGUOUS_RATIO * Math.max(near[0].d, 1)) return null;
  return near[0].s;
}

/** "🇧🇪" from "BE". */
export function navtexFlag(iso: string): string {
  return /^[A-Z]{2}$/.test(iso) ? String.fromCodePoint(0x1f1e6 + iso.charCodeAt(0) - 65, 0x1f1e6 + iso.charCodeAt(1) - 65) : '';
}

/** "B · Oostende, Belgium 🇧🇪", or "Station B" when the station cannot be named. */
export function navtexStationLabel(b1: string | null | undefined, dialHz: number, lat: number, lon: number, text = ''): string {
  const s = navtexStation(b1, dialHz, lat, lon, text);
  if (!s) return b1 ? \`Station \${b1}\` : 'Station ?';
  return \`\${s.b1} \\u00b7 \${s.name}, \${s.country} \${navtexFlag(s.iso)}\`;
}
`;

const cstr = (s) => JSON.stringify(s).replace(/[\u0080-￿]/g, (ch) =>
  [...Buffer.from(ch, 'utf8')].map((b) => '\\x' + b.toString(16)).join('') + '""');
const cpp = `// vibe_navtex_stations.h — ${HEAD}
// ★ The rule is explained in the generator's header; src/utils/navtexStations.generated.ts is the clients' copy.
#pragma once
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace vibe {
namespace navtex {

struct Station { char b1; double khz; const char* name; const char* iso; const char* country; double lat, lon; };

inline const Station* stations(size_t& n) {
    static const Station t[] = {
${rows.map((r) => `        { '${r.b1}', ${r.khz.toFixed(1)}, ${cstr(r.name)}, "${r.iso}", ${cstr(r.country)}, ${r.lat}, ${r.lon} },`).join('\n')}
    };
    n = sizeof(t) / sizeof(t[0]);
    return t;
}

constexpr double kReachMfKm = 1500, kReachHfKm = 2500, kAmbiguousRatio = 1.5;

inline double bandKhz(double dialHz) {
    static const double bands[] = { 424, 490, 518, 4209.5 };
    if (!std::isfinite(dialHz)) return 0;
    for (double b : bands) if (std::fabs(dialHz / 1000.0 - b) <= 3.0) return b;
    return 0;
}

inline double km(double la1, double lo1, double la2, double lo2) {
    const double r = 3.14159265358979323846 / 180.0;
    const double a = std::pow(std::sin((la2 - la1) * r / 2), 2) + std::cos(la1 * r) * std::cos(la2 * r) * std::pow(std::sin((lo2 - lo1) * r / 2), 2);
    return 2 * 6371.0 * std::asin(std::fmin(1.0, std::sqrt(a)));
}

/** Upper-case A–Z only: "Oostende" → "OOSTENDE". Any non-ASCII byte is dropped — the clients' letters() does the
 *  same, so "Curaçao" is CURAAO on both sides. */
inline std::string letters(const std::string& s) {
    std::string o;
    for (unsigned char c : s) if (std::isalpha(c) && c < 0x80) o += (char)std::toupper(c);
    return o;
}

/** navtexStation() — nullptr = the letter only. */
inline const Station* resolve(char b1, double dialHz, double lat, double lon, const std::string& text = "") {
    if (b1 < 'A' || b1 > 'Z') return nullptr;
    const double band = bandKhz(dialHz);
    if (band == 0) return nullptr;
    size_t n = 0; const Station* t = stations(n);
    std::vector<const Station*> c;
    for (size_t i = 0; i < n; i++) if (t[i].b1 == b1 && t[i].khz == band) c.push_back(&t[i]);
    if (c.empty()) return nullptr;
    if (!text.empty()) {
        const std::string tl = letters(text);
        const Station* named = nullptr; int hits = 0;
        for (auto* s : c) { const std::string nl = letters(s->name); if (nl.size() >= 5 && tl.find(nl) != std::string::npos) { named = s; ++hits; } }
        if (hits == 1) return named;
    }
    if (!std::isfinite(lat) || !std::isfinite(lon)) return nullptr;
    const double reach = band > 1000 ? kReachHfKm : kReachMfKm;
    const Station* best = nullptr; double d1 = 1e18, d2 = 1e18;
    for (auto* s : c) {
        const double d = km(lat, lon, s->lat, s->lon);
        if (d > reach) continue;
        if (d < d1) { d2 = d1; d1 = d; best = s; } else if (d < d2) d2 = d;
    }
    if (!best) return nullptr;
    if (d2 < 1e17 && d2 < kAmbiguousRatio * std::fmax(d1, 1.0)) return nullptr;
    return best;
}

/** "🇧🇪" from "BE". */
inline std::string flag(const char* iso) {
    std::string o;
    if (!iso || !std::isupper((unsigned char)iso[0]) || !std::isupper((unsigned char)iso[1])) return o;
    for (int i = 0; i < 2; i++) {
        const unsigned cp = 0x1F1E6 + (unsigned)(iso[i] - 'A');
        o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

/** navtexStationLabel(): "B · Oostende, Belgium 🇧🇪", or "Station B". */
inline std::string label(char b1, double dialHz, double lat, double lon, const std::string& text = "") {
    const Station* s = resolve(b1, dialHz, lat, lon, text);
    if (!s) return std::string("Station ") + (b1 >= 'A' && b1 <= 'Z' ? std::string(1, b1) : "?");
    return std::string(1, s->b1) + " \\xc2\\xb7 " + s->name + ", " + s->country + " " + flag(s->iso);
}

} // namespace navtex
} // namespace vibe
`;

const outs = [
  ['src/utils/navtexStations.generated.ts', ts],
  ['android/app/src/main/cpp/vibe_navtex_stations.h', cpp],
];
const check = process.argv.includes('--check');
let stale = 0;
for (const [f, body] of outs) {
  const p = path.join(root, f);
  const cur = fs.existsSync(p) ? fs.readFileSync(p, 'utf8') : '';
  if (cur === body) continue;
  stale++;
  if (check) console.error(`  ✗ ${f} is stale — run node scripts/gen-navtex-stations.mjs`);
  else fs.writeFileSync(p, body);
}
console.log(`navtex stations: ${rows.length} transmitters from ${src.stations.length} stations${check ? (stale ? `, ${stale} stale` : ', up to date') : ''}`);
process.exit(check && stale ? 1 : 0);
