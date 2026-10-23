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
// ★★ THE RULE (resolve / navtexResolve) — a LIKELIHOOD, not a cut-off (owner-approved, 2026-10-22: NickB heard a
//    Caribbean NAVTEX in the UK; a hard 1500 km reach showed it as a bare letter). Every candidate with the letter on
//    the band (the table frequency within ±3 kHz of the dial) is scored from the MESSAGE:
//      1. POSITIONS IN THE TEXT — the strongest evidence. "51-14.13N 002-54.22E", "5114N 00254E", "51 14.1N 002 54.2E",
//         "54-19,2N 005-44,2E"; anything with a lost '_' or out of range is ignored. The candidate's MEDIAN distance to
//         them: × (0.01 + 100 / (1 + (med/200 km)²)). If no candidate is within 1000 km of its warning's positions,
//         none of them sent it (a station not in Pub. 117): the letter only.
//      2. A STATION NAMED IN THE TEXT (same letter and band — "OOSTENDERADIO", "NCC-HAMBURG"): × 1000, decisive.
//      3. LANGUAGE: a stopword count (en nl de fr es pt it no da sv pl); the candidate's country speaks it → × 4 on 490
//         (the national-language service), × 1.5 elsewhere (518 is mostly English).
//      4. DISTANCE FROM THE RECEIVING SERVER, a SOFT prior: 1 / (1 + (d/800 km)²); 1 for all when the server's position
//         is unknown. A far station with positions or its name in the text still wins.
//    Then: the best ≥ 3× the next → named ("· DX 6,800 km" past 1500 km on MF, 2500 km on HF); else the best two,
//    "W · Corsen 🇫🇷 or Valentia 🇮🇪?". With NO evidence (no fitting positions, no name, no language) the best must be
//    within 1500 km (MF) / 2500 km (HF) and the server's position known — else the letter only, as before ("Station B").
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

// ★★ POSITIONS PUB. 117 (AS PARSED) GETS WRONG — corrected here, in the open, from the Wikipedia list below.
//    Saudanes: Pub. 117 gives 66-11N 001-18W, a point in the Norwegian Sea; the station is on Iceland's north-east coast,
//    66°11′N 18°57′W (Wikipedia). Limnos: Pub. 117 gives 34-52N, south of Crete; Limnos is at 39°54′N 25°11′E.
const POSITION_FIX = { 'Saudanes': [66.186, -18.952], 'Limnos': [39.906, 25.181] };

// ★★★ THE SUPPLEMENTARY LIST — transmitters MISSING from Pub. 117 (2021), each with its evidence. Added BESIDE the
//     table, never instead of it: where a source disagrees with Pub. 117 the Pub. 117 entry stays a candidate (the
//     likelihood rule picks between them from the message), and the disagreement is written down below.
//   Sources:
//     [WP] Wikipedia, "List of Navtex stations", https://en.wikipedia.org/wiki/List_of_Navtex_stations, revision
//          1371431375 (2026-08-26). FACTS ONLY (name, letter, kHz, country, position) — the article's text is
//          CC BY-SA and none of it is copied. Positions are its {{coord}} values, rounded to 0.001°.
//     [RX] Received at Stuart's server (IO92nh) on 2026-10-23, 489.5 kHz USB: "ZCZC EA17 / 230440 UTC OCT 26 /
//          MONDOLFO RADIO / COSTAVURNAV NR. 576/26 …" — an Italian warning for platforms off Ravenna, the station naming
//          itself in its header. Pub. 117's only E on 490 near Europe is Corsen, so the gallery said "Corsen, France".
//     [IN] Marina Militare (Istituto Idrografico), Avvisi ai Naviganti notice listing Mondolfo NAVTEX as U on 518 kHz
//          and E on 490 kHz — seen 2026-10-23 only as a search engine's summary of the PDF (marina.difesa.it, "Riepilogativo
//          RDS"); the PDF itself returned 404, so it is corroboration, not the source. [WP] is the source we read.
//   Disagreements with Pub. 117, where Pub. 117 is KEPT (Wikipedia's entry is not shown to be newer):
//     518: Vardø C [WP] / V; Tallinn F [WP] / U; Kerch B [WP] / G; İzmir I [WP] / D; Malta O [WP] / T; Murmansk C [WP] /
//          K; Arkhangelsk F [WP] / L; Talcahuano C [WP] / J, E. 490: Jukbyeon J [WP] / V; Byeonsan K [WP] / W; Easter
//          Island G on 490 [WP] (no position) / G on 518. Not added: what [WP] marks inactive (Kelibia T 518, Constanța
//          L 490, Bahía Blanca D 490, Kaohsiung P 518 — Casablanca M 518, also inactive there, stays from Pub. 117);
//          Guayaquil M 518 (no position).
//   Where [WP] names a transmitter site for a Pub. 117 station (Pinneberg for Hamburg, Punta Arenas for Magallanes,
//   Serapeum for Ismailia) the Pub. 117 name is kept, so a message naming its station ("NCC-HAMBURG") still matches.
const SUPPLEMENT = [
  // Italy: [WP] lists a newer network than Pub. 117 2021 (Rome R, Cagliari T, Trieste U, Augusta V on 518).
  { name: 'Mondolfo', country: 'Italy', lat: 43.748, lon: 13.142, tx: [['E', 490], ['U', 518]],
    evidence: 'E 490: [RX] + [WP]; U 518: [WP] + [IN] (Pub. 117 has Trieste on U 518 — kept)' },
  { name: 'La Maddalena', country: 'Italy', lat: 41.223, lon: 9.399, tx: [['R', 518], ['I', 490]],
    evidence: '[WP] (Pub. 117 has Rome on R 518 — kept)' },
  { name: 'Sellia Marina', country: 'Italy', lat: 38.873, lon: 16.720, tx: [['V', 518], ['W', 490]],
    evidence: '[WP] (Pub. 117 has Augusta on V 518 — kept)' },
  { name: 'Piombino', country: 'Italy', lat: 42.917, lon: 10.533, tx: [['N', 490]],
    evidence: '[WP]; it gives no position, so this is the TOWN\'s (Wikipedia "Piombino", revision 1375293261)' },
  // France
  { name: 'Corsen', country: 'France', lat: 48.467, lon: -5.05, tx: [['A', 518]],
    evidence: '[WP], and Pub. 117\'s own France section; its 300C table says W (kept)' },
  { name: 'La Garde', country: 'France', lat: 43.104, lon: 5.991, tx: [['W', 518]], evidence: '[WP] (Pub. 117: S 490 only)' },
  // Germany — Pub. 117 lists Hamburg's S on 518 with no frequency (left out of its table); [WP] gives 518, at Pinneberg.
  { name: 'Hamburg', country: 'Germany', lat: 53.673, lon: 9.809, tx: [['S', 518]], evidence: '[WP] (transmitter at Pinneberg)' },
  // Norway — the Oslofjord/Skagerrak station; explains the Lenovo's "tonight's M" (a Skagerrak warning on M 518).
  { name: 'Jeløy', country: 'Norway', lat: 59.436, lon: 10.589, tx: [['M', 518]], evidence: '[WP]' },
  // Greece — a national (490) service Pub. 117 does not list.
  { name: 'Kerkyra', country: 'Greece', lat: 39.607, lon: 19.891, tx: [['P', 490]], evidence: '[WP]' },
  { name: 'Irakleio', country: 'Greece', lat: 35.323, lon: 25.749, tx: [['Q', 490], ['S', 4209.5]], evidence: '[WP]' },
  { name: 'Limnos', country: 'Greece', lat: 39.906, lon: 25.181, tx: [['R', 490]], evidence: '[WP]' },
  // Chile — the Spanish 490 service, on the same letters as Pub. 117's 518 entries.
  { name: 'Antofagasta', country: 'Chile', lat: -23.491, lon: -70.425, tx: [['H', 490]], evidence: '[WP]' },
  { name: 'Valparaiso', country: 'Chile', lat: -32.802, lon: -71.485, tx: [['I', 490]], evidence: '[WP]' },
  { name: 'Talcahuano', country: 'Chile', lat: -36.715, lon: -73.108, tx: [['J', 490]], evidence: '[WP]' },
  { name: 'Puerto Montt', country: 'Chile', lat: -41.490, lon: -72.958, tx: [['K', 490]], evidence: '[WP]' },
  { name: 'Magallanes', country: 'Chile', lat: -52.948, lon: -71.057, tx: [['L', 490]], evidence: '[WP] (Punta Arenas)' },
  // HF 4209.5 kHz — Pub. 117 lists no HF letters at all; these are [WP]'s.
  { name: 'Istanbul', country: 'Turkey', lat: 41.067, lon: 28.95, tx: [['M', 4209.5]], evidence: '[WP]' },
  { name: 'Ismailia', country: 'Egypt', lat: 30.470, lon: 32.367, tx: [['X', 4209.5]], evidence: '[WP] (Serapeum)' },
  { name: 'New Orleans', country: 'United States', lat: 29.885, lon: -89.946, tx: [['G', 4209.5]], evidence: '[WP]' },
  { name: 'Shanghai', country: 'China', lat: 31.109, lon: 121.544, tx: [['Q', 4209.5]], evidence: '[WP]' },
  { name: 'Hai Phong', country: 'Vietnam', lat: 20.733, lon: 106.733, tx: [['W', 4209.5]], evidence: '[WP]' },
];

const rows = [];
const problems = [];
for (const s of src.stations) {
  const c = COUNTRY[s.country];
  if (!c) { problems.push(`no ISO code for Pub. 117 country "${s.country}"`); continue; }
  const name = shortName(s.name);
  if (!name) { problems.push(`a station at ${s.lat}, ${s.lon} has no name`); continue; }
  const [lat, lon] = POSITION_FIX[name] ?? [s.lat, s.lon];
  for (const t of s.tx) rows.push({ b1: t.b1, khz: t.khz, name, iso: c[0], country: c[1] ?? s.country, lat, lon });
}
for (const s of SUPPLEMENT) {
  const c = COUNTRY[s.country];
  if (!c || !s.evidence) { problems.push(`supplement "${s.name}": no ISO code or no evidence`); continue; }
  for (const [b1, khz] of s.tx) {
    if (rows.some((r) => r.b1 === b1 && r.khz === khz && r.name === s.name)) { problems.push(`supplement "${s.name}" ${b1} ${khz}: already in Pub. 117`); continue; }
    rows.push({ b1, khz, name: s.name, iso: c[0], country: c[1] ?? s.country, lat: s.lat, lon: s.lon });
  }
}
if (problems.length || rows.length < 150) {
  for (const p of problems) console.error('  ✗ ' + p);
  console.error(`  ✗ ${rows.length} transmitters — the station table's shape changed`);
  process.exit(1);
}
rows.sort((a, b) => a.b1.localeCompare(b.b1) || a.khz - b.khz || a.name.localeCompare(b.name));

// ★★ THE WEIGHTS AND WORD LISTS — written into BOTH readers from here, so they cannot differ.
const W = { priorKm: 800, named: 1000, posFloor: 0.01, posGain: 100, posKm: 200, posFitKm: 1000, lang490: 4, langOther: 1.5,
            clear: 3, reachMf: 1500, reachHf: 2500, langMinHits: 3, langLead: 1.5, nameLines: 4, posCountryKm: 400 };
// ★ Short, common, distinctive function words, upper-case ASCII as NAVTEX prints them (no diacritics: FRAN, PAA).
const STOP = {
  en: ['THE', 'AND', 'OF', 'IS', 'ARE', 'TO', 'IN', 'AT', 'WITH', 'FROM', 'WILL', 'BE', 'BETWEEN', 'FOR'],
  nl: ['HET', 'EEN', 'VAN', 'NIET', 'BIJ', 'NAAR', 'TUSSEN', 'WORDT', 'ZAL', 'WORDEN', 'OOK', 'DIT', 'EN'],
  de: ['DER', 'DIE', 'DAS', 'UND', 'NICHT', 'AUF', 'BEI', 'VON', 'ZWISCHEN', 'WIRD', 'IST', 'MIT', 'NACH', 'DEN', 'DEM'],
  fr: ['LE', 'LES', 'ET', 'DES', 'DU', 'EST', 'UNE', 'ENTRE', 'SUR', 'DANS', 'AU', 'AUX'],
  es: ['EL', 'LOS', 'LAS', 'Y', 'DEL', 'CON', 'POR', 'PARA', 'UNA', 'ENTRE'],
  pt: ['OS', 'DO', 'DA', 'DOS', 'DAS', 'EM', 'COM', 'PARA', 'UMA', 'NAO', 'NO', 'NA'],
  // ★ + DI SULLE ALLA SONO NELLA DEI SUL NELLE DEGLI (2026-10-23): Mondolfo's warning read as no language with the
  //   first ten. Never ALLE (German "all").
  it: ['IL', 'GLI', 'DELLA', 'DELLE', 'CON', 'PER', 'UNA', 'NON', 'TRA', 'DEL', 'DI', 'SULLE', 'ALLA', 'SONO', 'NELLA', 'DEI',
       'SUL', 'NELLE', 'DEGLI'],
  no: ['OG', 'IKKE', 'ER', 'TIL', 'FRA', 'MED', 'AV', 'PAA', 'VED', 'MELLOM', 'FYR'],
  da: ['OG', 'IKKE', 'ER', 'TIL', 'FRA', 'MED', 'AF', 'PAA', 'VED', 'MELLEM', 'FYR'],
  sv: ['OCH', 'INTE', 'AR', 'TILL', 'FRAN', 'MED', 'AV', 'PA', 'VID', 'MELLAN', 'FYR'],
  pl: ['NIE', 'JEST', 'ORAZ', 'MIEDZY', 'PRZEZ', 'NA', 'DO'],
};
// ★ The languages a station's country broadcasts in (Norwegian and Danish read alike in capitals — both count for each).
const LANGS = {
  GB: ['en'], IE: ['en'], US: ['en'], CA: ['en', 'fr'], BM: ['en'], GU: ['en'], MT: ['en'], ZA: ['en'], NA: ['en'],
  NL: ['nl'], BE: ['nl', 'fr'], CW: ['nl'], DE: ['de'], FR: ['fr'], ES: ['es'], AR: ['es'], CL: ['es'], CO: ['es'],
  EC: ['es'], PE: ['es'], UY: ['es'], PT: ['pt'], CV: ['pt'], IT: ['it'], NO: ['no', 'da'], SJ: ['no', 'da'],
  DK: ['da', 'no'], FO: ['da', 'no'], SE: ['sv'], PL: ['pl'],
};
// ★ A language that names ONE country among the NAVTEX nations — used only to say where a station the table does not
//   know is (b). English, French, Spanish, Portuguese, Dutch, Norwegian/Danish are spoken in several, so they say nothing.
const LANG_COUNTRY = { it: ['IT', 'Italy'], de: ['DE', 'Germany'], sv: ['SE', 'Sweden'], pl: ['PL', 'Poland'] };
// ★ "<NAME> RADIO" lines that are not a station's name.
const GENERIC = ['COAST', 'COASTAL', 'COAST GUARD', 'MARITIME', 'NAVTEX', 'THIS IS', 'PORT', 'MRCC', 'MRSC', 'CROSS', 'THE', 'NAVAREA'];

const HEAD = `GENERATED by scripts/gen-navtex-stations.mjs from scripts/navtex-stations/navtex-stations.json. Do not edit.
// Source: ${src.source}
// Edition: ${src.edition}
// Supplement: ${SUPPLEMENT.length} stations missing from that edition (evidence in the generator), and ${Object.keys(POSITION_FIX).length} positions corrected.`;

const ts = `// navtexStations.generated.ts — ${HEAD}
// ★ The rule is explained in the generator's header; vibe_navtex_stations.h is the server's copy of the same rule.

export interface NavtexStation { b1: string; khz: number; name: string; iso: string; country: string; lat: number; lon: number }

export const NAVTEX_STATIONS: readonly NavtexStation[] = [
${rows.map((r) => `  { b1: '${r.b1}', khz: ${r.khz}, name: ${JSON.stringify(r.name)}, iso: '${r.iso}', country: ${JSON.stringify(r.country)}, lat: ${r.lat}, lon: ${r.lon} },`).join('\n')}
];

/** The NAVTEX channels: 424 (Japan), 490 (national), 518 (international), 4209.5 kHz (HF). */
export const NAVTEX_BANDS_KHZ: readonly number[] = [424, 490, 518, 4209.5];
export const NAVTEX_REACH_MF_KM = ${W.reachMf};
export const NAVTEX_REACH_HF_KM = ${W.reachHf};
/** ★ The weights — the SAME numbers as vibe_navtex_stations.h (both written from the generator's W). */
export const NAVTEX_WEIGHTS = ${JSON.stringify(W)} as const;
const STOP: Record<string, readonly string[]> = ${JSON.stringify(STOP)};
const LANGS: Record<string, readonly string[]> = ${JSON.stringify(LANGS)};
const LANG_COUNTRY: Record<string, readonly [string, string]> = ${JSON.stringify(LANG_COUNTRY)};
const GENERIC: readonly string[] = ${JSON.stringify(GENERIC)};

function km(la1: number, lo1: number, la2: number, lo2: number): number {
  const r = Math.PI / 180;
  const a = Math.sin(((la2 - la1) * r) / 2) ** 2 + Math.cos(la1 * r) * Math.cos(la2 * r) * Math.sin(((lo2 - lo1) * r) / 2) ** 2;
  return 2 * 6371 * Math.asin(Math.min(1, Math.sqrt(a)));
}
// ★ ASCII letters only, exactly as the server's letters(): "Curaçao" is CURAAO on both sides.
const letters = (s: string) => s.toUpperCase().replace(/[^A-Z]/g, '');
const isD = (c: string | undefined) => c !== undefined && c >= '0' && c <= '9';
const isAlnum = (c: string | undefined) => c !== undefined && ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'));

/** The band a dial is on (±3 kHz — a USB dial sits half a kHz below the carrier), or null. */
export function navtexBandKhz(dialHz: number): number | null {
  if (!Number.isFinite(dialHz)) return null;
  for (const b of NAVTEX_BANDS_KHZ) if (Math.abs(dialHz / 1000 - b) <= 3) return b;
  return null;
}

/** One half of a position at i: nd degree digits, an optional '-' or ' ', two minute digits, an optional [.,] and
 *  1–3 decimals, an optional space, a hemisphere letter. [value, end] or null. ★ The server's coordHalf() is this. */
function coordHalf(s: string, i: number, nd: number, hem: string): [number, number] | null {
  let j = i, deg = 0;
  for (let k = 0; k < nd; k++, j++) { if (!isD(s[j])) return null; deg = deg * 10 + (s.charCodeAt(j) - 48); }
  if (s[j] === '-' || s[j] === ' ') j++;
  if (!isD(s[j]) || !isD(s[j + 1])) return null;
  let min = (s.charCodeAt(j) - 48) * 10 + (s.charCodeAt(j + 1) - 48); j += 2;
  if ((s[j] === '.' || s[j] === ',') && isD(s[j + 1])) {
    j++; let f = 0.1;
    for (let k = 0; k < 3 && isD(s[j]); k++, j++, f /= 10) min += (s.charCodeAt(j) - 48) * f;
    if (isD(s[j])) return null;
  }
  if (s[j] === ' ') j++;
  const h = s[j];
  if (h === undefined || hem.indexOf(h) < 0) return null;
  if (min >= 60 || deg > (nd === 2 ? 90 : 180)) return null;
  const v = deg + min / 60;
  return [h === 'S' || h === 'W' ? -v : v, j + 1];
}
/** Every position the message gives, [lat, lon]. */
export function navtexPositions(text: string): Array<[number, number]> {
  const s = text.toUpperCase(), out: Array<[number, number]> = [];
  for (let i = 0; i < s.length && out.length < 64; i++) {
    if (!isD(s[i]) || isD(s[i - 1])) continue;
    const la = coordHalf(s, i, 2, 'NS');
    if (!la) continue;
    let j = la[1], gap = 0;
    while ((s[j] === ' ' || s[j] === ',' || s[j] === '\\t') && gap < 3) { j++; gap++; }
    // ★ ITA2: a figures-shift '-' printed in letters shift is an 'A' ("44 31.4N A   012 30.6E", Mondolfo 2026-10-23).
    if ((s[j] === '-' || s[j] === 'A') && s[j + 1] === ' ') { j++; for (let k = 0; k < 3 && s[j] === ' '; k++) j++; }
    const lo = coordHalf(s, j, 3, 'EW');
    if (!lo || isAlnum(s[lo[1]])) continue;
    out.push([la[0], lo[0]]);
    i = lo[1] - 1;
  }
  return out;
}
/** The languages the text reads as (stopwords), [] when it cannot say. */
export function navtexLanguages(text: string): string[] {
  const words = text.toUpperCase().split(/[^A-Z]+/).filter(Boolean);
  const score: Record<string, number> = {};
  for (const [l, list] of Object.entries(STOP)) { let n = 0; for (const w of words) if (list.includes(w)) n++; score[l] = n; }
  const top = Math.max(0, ...Object.values(score));
  if (top < NAVTEX_WEIGHTS.langMinHits) return [];
  const got = Object.keys(STOP).filter((l) => score[l] >= Math.max(NAVTEX_WEIGHTS.langMinHits, top / NAVTEX_WEIGHTS.langLead));
  return got.length <= 3 ? got : [];
}

/** The station a message names ITSELF in its first lines — "MONDOLFO RADIO", "OOSTENDERADIO", "ISTANBUL NAVTEX" —
 *  as [name, suffix], or null. ★ The server's textName() is this, character for character. */
export function navtexTextName(text: string): [string, string] | null {
  const lines = text.toUpperCase().split('\\n').map((l) => l.replace(/[\\r\\t]/g, ' ').trim()).filter((l) => l.length > 0);
  const from = lines.length && lines[0].startsWith('ZCZC') ? 1 : 0;
  for (let i = from; i < lines.length && i < from + NAVTEX_WEIGHTS.nameLines; i++) {
    let l = lines[i];
    while (l.length && '.,:;'.includes(l[l.length - 1])) l = l.slice(0, -1);
    if (l.length > 32) continue;
    const suffix = l.endsWith('RADIO') ? 'RADIO' : l.endsWith('NAVTEX') ? 'NAVTEX' : '';
    if (!suffix) continue;
    const n = l.slice(0, l.length - suffix.length).trim().split(' ').filter((w) => w.length > 0).join(' ');
    if (!n.length || n[0] < 'A' || n[0] > 'Z') continue;
    let clean = true;
    for (const c of n) if (!((c >= 'A' && c <= 'Z') || c === ' ' || c === '-' || c === "'")) clean = false;
    if (!clean || letters(n).length < 4 || GENERIC.includes(n)) continue;
    return [n, suffix];
  }
  return null;
}
/** "MONDOLFO" + "RADIO" → "Mondolfo Radio". */
function titleCase(s: string): string {
  let o = '';
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    o += i === 0 || s[i - 1] === ' ' || s[i - 1] === '-' || s[i - 1] === "'" ? c : c.toLowerCase();
  }
  return o;
}

/** A station the TABLE does not have, named by its own message. */
export interface NavtexTextStation { name: string; iso: string; country: string }

export interface NavtexResolution {
  /** One station (named), or the best two (too close to call). Empty when \`text\` is set. */
  stations: NavtexStation[];
  /** ★ (b) The message names a station that is none of the candidates, and its positions or language rule them all
   *  out: that name, with the country where it can soundly be said ("E · Mondolfo Radio, Italy 🇮🇹?"). */
  text?: NavtexTextStation;
  /** Each one's distance from the receiving server, km (NaN when the server's position is unknown). */
  km: number[];
  /** Beyond ordinary reach of the receiving server — the title says "DX". */
  dx: boolean;
}

/** Where a station the table does not have is: a station of that name elsewhere in the table (another letter or
 *  band) gives its own name and country; else the country of the table station nearest the message's positions
 *  (median, within posCountryKm), if the text's language — when there is one — is spoken there; else a language spoken
 *  in only one NAVTEX country; else no country. */
function textStation(tn: [string, string], pos: Array<[number, number]>, langs: string[]): NavtexTextStation {
  const own = NAVTEX_STATIONS.find((s) => letters(s.name) === letters(tn[0]));
  if (own) return { name: own.name, iso: own.iso, country: own.country };
  const name = titleCase(tn[0] + ' ' + tn[1]);
  if (pos.length) {
    let best: NavtexStation | null = null, bestMed = Infinity;
    for (const s of NAVTEX_STATIONS) {
      const ds = pos.map(([pa, po]) => km(s.lat, s.lon, pa, po)).sort((x, y) => x - y);
      const med = ds.length % 2 ? ds[(ds.length - 1) / 2] : (ds[ds.length / 2 - 1] + ds[ds.length / 2]) / 2;
      if (med < bestMed) { bestMed = med; best = s; }
    }
    if (best && bestMed <= NAVTEX_WEIGHTS.posCountryKm && (!langs.length || (LANGS[best.iso] ?? []).some((l) => langs.includes(l))))
      return { name, iso: best.iso, country: best.country };
  }
  const only = langs.length === 1 ? LANG_COUNTRY[langs[0]] : undefined;
  if (only) return { name, iso: only[0], country: only[1] };
  return { name, iso: '', country: '' };
}

/** Which station sent letter \`b1\`, heard on \`dialHz\` at a server at \`lat\`/\`lon\` (NaN = unknown), judged from the
 *  message \`text\`. null = the letter only. */
export function navtexResolve(b1: string | null | undefined, dialHz: number, lat: number, lon: number, text = ''): NavtexResolution | null {
  if (!b1 || !/^[A-Z]$/.test(b1)) return null;
  const band = navtexBandKhz(dialHz);
  if (band == null) return null;
  const cands = NAVTEX_STATIONS.filter((s) => s.b1 === b1 && s.khz === band);
  if (!cands.length) return null;
  const W = NAVTEX_WEIGHTS;
  const here = Number.isFinite(lat) && Number.isFinite(lon);
  const reach = band > 1000 ? W.reachHf : W.reachMf;
  const t = letters(text);
  const pos = navtexPositions(text);
  const langs = navtexLanguages(text);
  const scored = cands.map((s) => {
    const d = here ? km(lat, lon, s.lat, s.lon) : NaN;
    let sc = here ? 1 / (1 + (d / W.priorKm) ** 2) : 1;
    const named = letters(s.name).length >= 5 && t.includes(letters(s.name));
    if (named) sc *= W.named;
    let med = Infinity;
    if (pos.length) {
      const ds = pos.map(([pa, po]) => km(s.lat, s.lon, pa, po)).sort((x, y) => x - y);
      med = ds.length % 2 ? ds[(ds.length - 1) / 2] : (ds[ds.length / 2 - 1] + ds[ds.length / 2]) / 2;
      sc *= W.posFloor + W.posGain / (1 + (med / W.posKm) ** 2);
    }
    const speaks = langs.length > 0 && (LANGS[s.iso] ?? []).some((l) => langs.includes(l));
    if (speaks) sc *= band === 490 ? W.lang490 : W.langOther;
    return { s, d, sc, named, med, speaks };
  }).sort((x, y) => y.sc - x.sc || x.s.name.localeCompare(y.s.name));
  const named = scored.some((x) => x.named);
  const posFit = pos.length > 0 && scored.some((x) => x.med <= W.posFitKm);
  // ★★ (b) THE STATION NAMES ITSELF AND IT IS NOT A CANDIDATE (Mondolfo, 2026-10-23: an Italian warning off Ravenna was
  //    "Corsen, France"). Only when the message ALSO rules every candidate out — positions that fit none of them, or,
  //    with no positions, a language none of them speaks. Positions that fit a candidate always keep it: a station
  //    may send another language (Niton's T on 490 is French), and language alone never outweighs the table.
  const tn = navtexTextName(text);
  if (tn && !named && !scored.some((x) => letters(x.s.name) === letters(tn[0]))) {
    const contradicted = !posFit && (pos.length > 0 || (langs.length > 0 && !scored.some((x) => x.speaks)));
    if (contradicted) return { stations: [], km: [], dx: false, text: textStation(tn, pos, langs) };
  }
  if (pos.length && !posFit && !named) return null;            // ★ positions nowhere near any candidate: not one of them
  const evidence = named || posFit || scored.some((x) => x.speaks);
  const best = scored[0], next = scored[1];
  if (!evidence && (!here || !(best.d <= reach))) return null;   // ★ no evidence and nothing near: the letter only
  if (!next || best.sc >= W.clear * next.sc) return { stations: [best.s], km: [best.d], dx: here && best.d > reach };
  return { stations: [best.s, next.s], km: [best.d, next.d], dx: false };
}

/** The station, when ONE can be named — or null. */
export function navtexStation(b1: string | null | undefined, dialHz: number, lat: number, lon: number, text = ''): NavtexStation | null {
  const r = navtexResolve(b1, dialHz, lat, lon, text);
  return r && r.stations.length === 1 ? r.stations[0] : null;
}

/** "7,500" — to the nearest 100 km. */
function kmText(d: number): string {
  const t = String(Math.round(d / 100) * 100);
  let o = '';
  for (let i = 0; i < t.length; i++) { if (i && (t.length - i) % 3 === 0) o += ','; o += t[i]; }
  return o;
}

/** "🇧🇪" from "BE". */
export function navtexFlag(iso: string): string {
  return /^[A-Z]{2}$/.test(iso) ? String.fromCodePoint(0x1f1e6 + iso.charCodeAt(0) - 65, 0x1f1e6 + iso.charCodeAt(1) - 65) : '';
}

/** "B · Oostende, Belgium 🇧🇪", "R · Isabella, United States 🇺🇸 · DX 6,700 km", "W · Corsen 🇫🇷 or Valentia 🇮🇪?", or
 *  "Station B" when the station cannot be named. */
export function navtexStationLabel(b1: string | null | undefined, dialHz: number, lat: number, lon: number, text = ''): string {
  const r = navtexResolve(b1, dialHz, lat, lon, text);
  if (!r) return b1 ? \`Station \${b1}\` : 'Station ?';
  if (r.text) return \`\${b1} \\u00b7 \${r.text.name}\${r.text.country ? \`, \${r.text.country} \${navtexFlag(r.text.iso)}\` : ''}?\`;
  const [a, b] = r.stations;
  if (b) return \`\${a.b1} \\u00b7 \${a.name} \${navtexFlag(a.iso)} or \${b.name} \${navtexFlag(b.iso)}?\`;
  return \`\${a.b1} \\u00b7 \${a.name}, \${a.country} \${navtexFlag(a.iso)}\${r.dx ? \` \\u00b7 DX \${kmText(r.km[0])} km\` : ''}\`;
}
`;

const cstr = (s) => JSON.stringify(s).replace(/[\u0080-￿]/g, (ch) =>
  [...Buffer.from(ch, 'utf8')].map((b) => '\\x' + b.toString(16)).join('') + '""');
const cpp = `// vibe_navtex_stations.h — ${HEAD}
// ★ The rule is explained in the generator's header; src/utils/navtexStations.generated.ts is the clients' copy.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
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

// ★ The weights — the SAME numbers as navtexStations.generated.ts (both written from the generator's W).
constexpr double kPriorKm = ${W.priorKm}, kNamed = ${W.named}, kPosFloor = ${W.posFloor}, kPosGain = ${W.posGain}, kPosKm = ${W.posKm},
                 kPosFitKm = ${W.posFitKm}, kLang490 = ${W.lang490}, kLangOther = ${W.langOther}, kClear = ${W.clear},
                 kReachMfKm = ${W.reachMf}, kReachHfKm = ${W.reachHf}, kLangLead = ${W.langLead};
constexpr int kLangMinHits = ${W.langMinHits};

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
inline std::string upperAscii(const std::string& s) { std::string o = s; for (auto& c : o) if (c >= 'a' && c <= 'z') c = (char)(c - 32); return o; }
inline bool isD(const std::string& s, size_t i) { return i < s.size() && s[i] >= '0' && s[i] <= '9'; }
inline bool isAlnumAt(const std::string& s, size_t i) { return i < s.size() && ((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'Z')); }

/** navtex coordHalf(): nd degree digits, optional '-'/' ', two minute digits, optional [.,] + 1–3 decimals, optional
 *  space, a hemisphere letter. false = not a clean position. */
inline bool coordHalf(const std::string& s, size_t i, int nd, const char* hem, double& v, size_t& end) {
    size_t j = i; int deg = 0;
    for (int k = 0; k < nd; k++, j++) { if (!isD(s, j)) return false; deg = deg * 10 + (s[j] - '0'); }
    if (j < s.size() && (s[j] == '-' || s[j] == ' ')) j++;
    if (!isD(s, j) || !isD(s, j + 1)) return false;
    double min = (s[j] - '0') * 10 + (s[j + 1] - '0'); j += 2;
    if (j < s.size() && (s[j] == '.' || s[j] == ',') && isD(s, j + 1)) {
        j++; double f = 0.1;
        for (int k = 0; k < 3 && isD(s, j); k++, j++, f /= 10) min += (s[j] - '0') * f;
        if (isD(s, j)) return false;
    }
    if (j < s.size() && s[j] == ' ') j++;
    if (j >= s.size() || !std::strchr(hem, s[j])) return false;
    if (min >= 60 || deg > (nd == 2 ? 90 : 180)) return false;
    const double x = deg + min / 60;
    v = (s[j] == 'S' || s[j] == 'W') ? -x : x; end = j + 1;
    return true;
}
/** navtexPositions(): every position the message gives. */
inline std::vector<std::pair<double, double>> positions(const std::string& text) {
    const std::string s = upperAscii(text);
    std::vector<std::pair<double, double>> out;
    for (size_t i = 0; i < s.size() && out.size() < 64; i++) {
        if (!isD(s, i) || (i > 0 && isD(s, i - 1))) continue;
        double la, lo; size_t e1, e2;
        if (!coordHalf(s, i, 2, "NS", la, e1)) continue;
        size_t j = e1; int gap = 0;
        while (j < s.size() && (s[j] == ' ' || s[j] == ',' || s[j] == '\\t') && gap < 3) { j++; gap++; }
        // ★ ITA2: a figures-shift '-' printed in letters shift is an 'A' (navtexPositions, the same rule).
        if (j + 1 < s.size() && (s[j] == '-' || s[j] == 'A') && s[j + 1] == ' ') { j++; for (int k = 0; k < 3 && j < s.size() && s[j] == ' '; k++) j++; }
        if (!coordHalf(s, j, 3, "EW", lo, e2) || isAlnumAt(s, e2)) continue;
        out.push_back({ la, lo });
        i = e2 - 1;
    }
    return out;
}
struct LangList { const char* lang; std::vector<const char*> words; };
inline const std::vector<LangList>& stopwords() {
    static const std::vector<LangList> t = {
${Object.entries(STOP).map(([l, ws]) => `        { "${l}", { ${ws.map((w) => `"${w}"`).join(', ')} } },`).join('\n')}
    };
    return t;
}
inline std::vector<std::string> isoLangs(const char* iso) {
    static const std::vector<std::pair<const char*, std::vector<const char*>>> t = {
${Object.entries(LANGS).map(([c, ls]) => `        { "${c}", { ${ls.map((w) => `"${w}"`).join(', ')} } },`).join('\n')}
    };
    for (auto& e : t) if (std::strcmp(e.first, iso) == 0) return std::vector<std::string>(e.second.begin(), e.second.end());
    return {};
}
/** navtexLanguages(). */
inline std::vector<std::string> languages(const std::string& text) {
    const std::string s = upperAscii(text);
    std::vector<std::string> words; std::string w;
    for (char c : s) { if (c >= 'A' && c <= 'Z') w += c; else if (!w.empty()) { words.push_back(w); w.clear(); } }
    if (!w.empty()) words.push_back(w);
    const auto& L = stopwords();
    std::vector<int> score(L.size(), 0); int top = 0;
    for (size_t i = 0; i < L.size(); i++) {
        for (auto& x : words) for (auto* sw : L[i].words) if (x == sw) { score[i]++; break; }
        top = std::max(top, score[i]);
    }
    std::vector<std::string> got;
    if (top < kLangMinHits) return got;
    for (size_t i = 0; i < L.size(); i++) if (score[i] >= std::max((double)kLangMinHits, top / kLangLead)) got.push_back(L[i].lang);
    if (got.size() > 3) got.clear();
    return got;
}

inline const std::vector<const char*>& genericNames() {
    static const std::vector<const char*> t = { ${GENERIC.map((g) => `"${g}"`).join(', ')} };
    return t;
}
/** navtexTextName(): the station a message names ITSELF in its first lines ("MONDOLFO RADIO", "OOSTENDERADIO"),
 *  {name, suffix}, or two empty strings. */
inline std::pair<std::string, std::string> textName(const std::string& text) {
    std::vector<std::string> lines; std::string cur;
    const std::string s = upperAscii(text);
    auto trim = [](std::string x) {
        size_t a = 0, b = x.size();
        while (a < b && x[a] == ' ') a++;
        while (b > a && x[b - 1] == ' ') b--;
        return x.substr(a, b - a);
    };
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == '\\n') { const std::string t = trim(cur); if (!t.empty()) lines.push_back(t); cur.clear(); }
        else cur += (s[i] == '\\r' || s[i] == '\\t') ? ' ' : s[i];
    }
    const size_t from = !lines.empty() && lines[0].compare(0, 4, "ZCZC") == 0 ? 1 : 0;
    for (size_t i = from; i < lines.size() && i < from + ${W.nameLines}; i++) {
        std::string l = lines[i];
        while (!l.empty() && std::strchr(".,:;", l.back()) && l.back() != 0) l.pop_back();
        if (l.size() > 32) continue;
        auto ends = [&](const char* x) { const size_t n = std::strlen(x); return l.size() >= n && l.compare(l.size() - n, n, x) == 0; };
        const std::string suffix = ends("RADIO") ? "RADIO" : ends("NAVTEX") ? "NAVTEX" : "";
        if (suffix.empty()) continue;
        std::string n, w;
        const std::string body = l.substr(0, l.size() - suffix.size()) + " ";
        for (char c : body) { if (c == ' ') { if (!w.empty()) { if (!n.empty()) n += ' '; n += w; w.clear(); } } else w += c; }
        if (n.empty() || n[0] < 'A' || n[0] > 'Z') continue;
        bool clean = true;
        for (char c : n) if (!((c >= 'A' && c <= 'Z') || c == ' ' || c == '-' || c == '\\'')) clean = false;
        if (!clean || letters(n).size() < 4) continue;
        bool generic = false; for (auto* g : genericNames()) if (n == g) generic = true;
        if (generic) continue;
        return { n, suffix };
    }
    return { "", "" };
}
inline std::string titleCase(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        o += (i == 0 || s[i - 1] == ' ' || s[i - 1] == '-' || s[i - 1] == '\\'') ? c : (char)std::tolower((unsigned char)c);
    }
    return o;
}
inline double medianKm(double la, double lo, const std::vector<std::pair<double, double>>& pos) {
    std::vector<double> ds; for (auto& p : pos) ds.push_back(km(la, lo, p.first, p.second));
    std::sort(ds.begin(), ds.end());
    return ds.size() % 2 ? ds[(ds.size() - 1) / 2] : (ds[ds.size() / 2 - 1] + ds[ds.size() / 2]) / 2;
}

/** navtexResolve(): 0 stations = the letter only (or, with textName set, a station the table does not have, named by its
 *  own message); 1 = named; 2 = too close to call. */
struct Resolution { std::vector<const Station*> stations; std::vector<double> km; bool dx = false;
                    std::string textName, textIso, textCountry; };
/** navtexResolve's textStation(): where a station the table does not have is. */
inline void textStation(Resolution& r, const std::pair<std::string, std::string>& tn,
                        const std::vector<std::pair<double, double>>& pos, const std::vector<std::string>& langs) {
    size_t n = 0; const Station* t = stations(n);
    const std::string want = letters(tn.first);
    for (size_t i = 0; i < n; i++) if (letters(t[i].name) == want) { r.textName = t[i].name; r.textIso = t[i].iso; r.textCountry = t[i].country; return; }
    r.textName = titleCase(tn.first + " " + tn.second);
    if (!pos.empty()) {
        const Station* best = nullptr; double bestMed = INFINITY;
        for (size_t i = 0; i < n; i++) { const double m = medianKm(t[i].lat, t[i].lon, pos); if (m < bestMed) { bestMed = m; best = &t[i]; } }
        bool spoken = langs.empty();
        if (best) for (auto& l : isoLangs(best->iso)) if (std::find(langs.begin(), langs.end(), l) != langs.end()) spoken = true;
        if (best && bestMed <= ${W.posCountryKm} && spoken) { r.textIso = best->iso; r.textCountry = best->country; return; }
    }
    if (langs.size() == 1) {
${Object.entries(LANG_COUNTRY).map(([l, [iso, c]]) => `        if (langs[0] == "${l}") { r.textIso = "${iso}"; r.textCountry = "${c}"; return; }`).join('\n')}
    }
}
inline Resolution resolveAll(char b1, double dialHz, double lat, double lon, const std::string& text = "") {
    Resolution r;
    if (b1 < 'A' || b1 > 'Z') return r;
    const double band = bandKhz(dialHz);
    if (band == 0) return r;
    size_t n = 0; const Station* t = stations(n);
    struct Sc { const Station* s; double d, sc, med; bool named, speaks; };
    std::vector<Sc> c;
    const bool here = std::isfinite(lat) && std::isfinite(lon);
    const double reach = band > 1000 ? kReachHfKm : kReachMfKm;
    const std::string tl = letters(text);
    const auto pos = positions(text);
    const auto langs = languages(text);
    for (size_t i = 0; i < n; i++) {
        if (t[i].b1 != b1 || t[i].khz != band) continue;
        Sc x{ &t[i], here ? km(lat, lon, t[i].lat, t[i].lon) : NAN, 1, INFINITY, false, false };
        x.sc = here ? 1.0 / (1.0 + std::pow(x.d / kPriorKm, 2)) : 1.0;
        const std::string nl = letters(t[i].name);
        x.named = nl.size() >= 5 && tl.find(nl) != std::string::npos;
        if (x.named) x.sc *= kNamed;
        if (!pos.empty()) {
            std::vector<double> ds; for (auto& p : pos) ds.push_back(km(t[i].lat, t[i].lon, p.first, p.second));
            std::sort(ds.begin(), ds.end());
            x.med = ds.size() % 2 ? ds[(ds.size() - 1) / 2] : (ds[ds.size() / 2 - 1] + ds[ds.size() / 2]) / 2;
            x.sc *= kPosFloor + kPosGain / (1.0 + std::pow(x.med / kPosKm, 2));
        }
        if (!langs.empty()) for (auto& l : isoLangs(t[i].iso)) if (std::find(langs.begin(), langs.end(), l) != langs.end()) { x.speaks = true; break; }
        if (x.speaks) x.sc *= band == 490 ? kLang490 : kLangOther;
        c.push_back(x);
    }
    if (c.empty()) return r;
    std::stable_sort(c.begin(), c.end(), [](const Sc& a, const Sc& b) {
        if (a.sc != b.sc) return a.sc > b.sc;
        return std::strcmp(a.s->name, b.s->name) < 0;
    });
    bool named = false, posFit = false, speaks = false;
    for (auto& x : c) { named |= x.named; posFit |= !pos.empty() && x.med <= kPosFitKm; speaks |= x.speaks; }
    // ★★ (b) the message names a station that is none of the candidates and rules them all out (navtexResolve).
    const auto tn = textName(text);
    if (!tn.first.empty() && !named) {
        bool isCand = false; const std::string want = letters(tn.first);
        for (auto& x : c) if (letters(x.s->name) == want) isCand = true;
        const bool contradicted = !posFit && (!pos.empty() || (!langs.empty() && !speaks));
        if (!isCand && contradicted) { textStation(r, tn, pos, langs); return r; }
    }
    if (!pos.empty() && !posFit && !named) return r;
    const bool evidence = named || posFit || speaks;
    const Sc& best = c[0];
    if (!evidence && (!here || !(best.d <= reach))) return r;
    if (c.size() == 1 || best.sc >= kClear * c[1].sc) { r.stations = { best.s }; r.km = { best.d }; r.dx = here && best.d > reach; return r; }
    r.stations = { best.s, c[1].s }; r.km = { best.d, c[1].d };
    return r;
}
/** The gallery's test: one station named, or a station named by its own message (never the "or" pair). */
inline bool nameable(char b1, double dialHz, double lat, double lon, const std::string& text = "") {
    const Resolution r = resolveAll(b1, dialHz, lat, lon, text);
    return r.stations.size() == 1 || !r.textName.empty();
}
/** navtexStation(): the station when ONE can be named, else nullptr. */
inline const Station* resolve(char b1, double dialHz, double lat, double lon, const std::string& text = "") {
    const Resolution r = resolveAll(b1, dialHz, lat, lon, text);
    return r.stations.size() == 1 ? r.stations[0] : nullptr;
}
inline std::string kmText(double d) {
    std::string s = std::to_string((long long)(std::llround(d / 100.0) * 100));
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return s;
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

/** navtexStationLabel(): "B · Oostende, Belgium 🇧🇪", "… · DX 6,700 km", "W · Corsen 🇫🇷 or Valentia 🇮🇪?", or "Station B". */
inline std::string label(char b1, double dialHz, double lat, double lon, const std::string& text = "") {
    const Resolution r = resolveAll(b1, dialHz, lat, lon, text);
    if (!r.textName.empty())
        return std::string(1, b1) + " \\xc2\\xb7 " + r.textName + (r.textCountry.empty() ? std::string() : ", " + r.textCountry + " " + flag(r.textIso.c_str())) + "?";
    if (r.stations.empty()) return std::string("Station ") + (b1 >= 'A' && b1 <= 'Z' ? std::string(1, b1) : "?");
    const Station* a = r.stations[0];
    if (r.stations.size() == 2) {
        const Station* b = r.stations[1];
        return std::string(1, a->b1) + " \\xc2\\xb7 " + a->name + " " + flag(a->iso) + " or " + b->name + " " + flag(b->iso) + "?";
    }
    return std::string(1, a->b1) + " \\xc2\\xb7 " + a->name + ", " + a->country + " " + flag(a->iso)
         + (r.dx ? " \\xc2\\xb7 DX " + kmText(r.km[0]) + " km" : std::string());
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
