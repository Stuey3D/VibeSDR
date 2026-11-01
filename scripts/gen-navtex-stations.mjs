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
// ★★ 2026-11-01 (Stuart): the German station is shown as PINNEBERG — the transmitter site, the name the international
//    lists use (Pub. 117's own entry reads "Haburg (DDH-49)", the NCC Hamburg service). "HAMBURG" stays an ALIAS (NAME_ALIAS
//    below), so "NCC-HAMBURG", "NAVTEX HAMBURG (NCC)" still name it in a message's text and self-ID.
const NAME_FIX = { 'Haburg': 'Pinneberg' };
const NAME_ALIAS = { Pinneberg: ['HAMBURG'] };

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
//   Where [WP] names a transmitter site for a Pub. 117 station (Punta Arenas for Magallanes, Serapeum for Ismailia) the
//   Pub. 117 name is kept. Germany is shown as Pinneberg (2026-11-01), with "HAMBURG" an alias (NAME_ALIAS).
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
  { name: 'Pinneberg', country: 'Germany', lat: 53.673, lon: 9.809, tx: [['S', 518]], evidence: '[WP] (Pinneberg, the NCC Hamburg service)' },
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
  // ★ 2026-11-01 (the NAVTEX map): Tallinn on F 518. Pub. 117 2021 lists Tallinn on U only; [WP] says F, and the
  //   Lenovo (IO92nh) received 118 "Station F" messages on 518 kHz between 2026-10-05 and 11-01 that are Estonian —
  //   "ZCZC FA73 / ESTONIAN NAV WARN 173/26 / GULF OF FINLAND / NARVA DGNSS STATION IN POSITION 5928.02N, 02802.36E";
  //   "ZCZC FE40 / BALTIC SEA WEATHERFORECAST" — no F candidate in the table was within 1 500 km of those positions.
  //   Stuart's UberSDR (same site) names them Tallinn too. Position: Pub. 117's own Tallinn entry (59.5N 24.5E).
  //   Pub. 117's U 518 stays a candidate.
  { name: 'Tallinn', country: 'Estonia', lat: 59.5, lon: 24.5, tx: [['F', 518]], evidence: '[RX] + [WP] (Pub. 117: U 518 — kept)' },
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

// ★★★ 2026-11-01 — ADDITIVE CLUES (the rule above is PROVEN and stays as it is; these only tighten its weak spots):
//   SELF-ID, FUZZY, KNOWN NAMES ONLY. "<NAME> RADIO" (RADIO itself one letter off: "MTNDOOFO AADIO"), "STATHMOS <NAME>"
//     (Greek: "STATHMOS IRAKLEIOY"), "NAVTEX<NAME>" ("NAVTEXJAHAMBURG (NCC)") is matched against the TABLE's names only,
//     within an edit distance scaled to the name's length: ⌊len/4⌋ on its own ("MTNDOOFO" → MONDOLFO, 2), ⌊len×0.4⌋ only
//     when the content backs the station's country — its issuer series or its language ("CONDOGNO" → MONDOLFO, 3, with
//     COSTAVURNAV and Italian). The best name must be strictly closer than any other. A name that matches nothing is
//     IGNORED — the title never shows a station the table does not have ("Condogno Radio, Italy?" was one).
//   ISSUER SERIES — a CHOICE among the label's own candidates, never a name in its own right (stations relay each other's:
//     Oostende sends WZ, Niton AVURNAV). Matched at a word start, one letter wrong in a phrase of 10+ letters. `strong` =
//     a warning SERVICE (may correct a header letter, below); a sea area or forecast heading only chooses.
//   GARBLED POSITIONS CARRY NO WEIGHT — a position further than a NAVTEX service area (serviceMf / serviceHf km) from every
//     transmitter using the letter on the band is damage ("02-44.67N" for 62-44.67N), dropped before the score sees it.
//   DISTANCE SANITY — a candidate past farKm on MF is named only on its name in the text or positions that fit it.
//   A CORRUPTED HEADER LETTER — when the self-ID or a strong series points to ONE station whose transmitter on this band
//     uses another letter, and the message started OUTSIDE the header letter's slots (each letter: 10 minutes every 4
//     hours, A at 00:00, B at 00:10 … X at 03:50) but INSIDE that transmitter's: "Suspected <station>", on its letter.
//     A two-letter header ("ZCZC VL") whose 2nd letter is that station's is said too (the letters shifted).
//   CONFIDENCE — confident (the plain name), likely ("Suspected <name>"), ambiguous ("<a> or <b>?"); the WHY is plain
//     English, per message (a language line only when THIS message is in that language), never a score.
const SERIES = [
  // [phrase, iso, station name ('' = any in that country), strong, what the why line calls it]
  ['COSTAVURNAV', 'IT', '', true, 'Italian warning series COSTAVURNAV'],
  ['AVURNAV BREST', 'FR', 'Corsen', true, 'French warning series AVURNAV BREST'],
  ['AVURNAV CHERBOURG', 'FR', '', true, 'French warning series AVURNAV CHERBOURG'],
  ['ESTONIAN NAV', 'EE', 'Tallinn', true, 'Estonian navigational warning series'],
  ['SWEDISH NAV', 'SE', '', true, 'Swedish navigational warning series'],
  ['NORWEGIAN NAV', 'NO', '', true, 'Norwegian navigational warning series'],
  ['DANISH NAV', 'DK', '', true, 'Danish navigational warning series'],
  ['NCC HAMBURG', 'DE', '', true, 'German warning service NCC Hamburg'],
  ['HAMBURG NCC', 'DE', '', true, 'German warning service NCC Hamburg'],
  ['DEUTSCHE BUCHT', 'DE', '', false, 'German sea area Deutsche Bucht'],
  ['GERMAN BIGHT', 'DE', '', false, 'German sea area German Bight'],
  ['WESTLICHE OSTSEE', 'DE', '', false, 'German sea area Westliche Ostsee'],
  ['SEEWETTERVORHERSAGE', 'DE', '', false, 'German sea forecast (Seewetterbericht)'],
];
const LANG_NAME = { en: 'English', nl: 'Dutch', de: 'German', fr: 'French', es: 'Spanish', pt: 'Portuguese', it: 'Italian',
                    no: 'Norwegian', da: 'Danish', sv: 'Swedish', pl: 'Polish' };
const W2 = { serviceMf: 3000, serviceHf: 5000, farKm: 8000, slotEarly: 120, slotLate: 720, nearEarly: 300, nearLate: 1500 };

const TS_EXTRA = String.raw`
// ── ★★★ 2026-11-01: THE ADDITIVE CLUES (the generator's header explains each) — baseResolve above is the proven rule ──
type SeriesRow = readonly [string, string, string, boolean, string];
const SERIES: readonly SeriesRow[] = @@SERIES@@;
const LANG_NAME: Record<string, string> = @@LANG_NAME@@;
const W2 = @@W2@@ as const;
/** ★ Other names a station goes by in its own messages ("HAMBURG" for Pinneberg) — matched like its name, shown as its name. */
const NAME_ALIAS: Record<string, readonly string[]> = @@NAME_ALIAS@@;
/** The letters a station is matched by: its name, then its aliases. */
function nameKeys(s: NavtexStation): string[] { return [letters(s.name), ...(NAME_ALIAS[s.name] ?? [])]; }
/** The station's own name for a name or an alias ("Hamburg" → "Pinneberg"); the name itself when it is neither. */
export function navtexCanonicalName(name: string): string {
  const L = letters(name);
  for (const s of NAVTEX_STATIONS) if (nameKeys(s).includes(L)) return s.name;
  return name;
}

/** Levenshtein distance. ★ The server's lev() is this. */
function lev(a: string, b: string): number {
  let prev: number[] = [];
  for (let j = 0; j <= b.length; j++) prev[j] = j;
  for (let i = 1; i <= a.length; i++) {
    const cur: number[] = [i];
    for (let j = 1; j <= b.length; j++) cur[j] = Math.min(prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1), prev[j] + 1, cur[j - 1] + 1);
    prev = cur;
  }
  return prev[b.length];
}
/** The fewest edits that make 'w' out of some stretch of 'hay' — starting anywhere, or (wordStart) only at a word's start. */
function levSub(hay: string, w: string, wordStart: boolean): number {
  const BIG = 1000000;
  let prev: number[] = [];
  for (let j = 0; j <= hay.length; j++) prev[j] = !wordStart || j === 0 || hay[j - 1] === ' ' ? 0 : BIG;
  for (let i = 1; i <= w.length; i++) {
    const cur: number[] = [prev[0] + 1];
    for (let j = 1; j <= hay.length; j++) cur[j] = Math.min(prev[j - 1] + (w[i - 1] === hay[j - 1] ? 0 : 1), prev[j] + 1, cur[j - 1] + 1);
    prev = cur;
  }
  let best = BIG;
  for (let j = 0; j <= hay.length; j++) best = Math.min(best, prev[j]);
  return best;
}
function median(xs: number[]): number {
  const ds = [...xs].sort((x, y) => x - y);
  return ds.length % 2 ? ds[(ds.length - 1) / 2] : (ds[ds.length / 2 - 1] + ds[ds.length / 2]) / 2;
}

/** ★ GARBLED POSITIONS CARRY NO WEIGHT: a position further than a service area from every transmitter using the letter
 *  on the band is damage ("02-44.67N" for 62-44.67N) — dropped, never counted against anyone. */
function sanePositions(pos: Array<[number, number]>, b1: string, band: number, sid: NavtexSelfId | null): Array<[number, number]> {
  const lim = band > 1000 ? W2.serviceHf : W2.serviceMf;
  // ★ …or from the station the message names itself: its own positions are not damage
  const tx = NAVTEX_STATIONS.filter((s) => (s.b1 === b1 && s.khz === band) || (sid !== null && letters(s.name) === sid.L));
  return pos.filter(([pa, po]) => tx.some((s) => km(s.lat, s.lon, pa, po) <= lim));
}

/** The issuer series and sea areas the text carries (SERIES order; one wrong letter in a 10+ letter phrase), and the UK's
 *  "WZ 649/26" at a line's start. */
export function navtexSeries(text: string): SeriesRow[] {
  const U = text.toUpperCase();
  const hay = U.replace(/[^A-Z]+/g, ' ');
  const out: SeriesRow[] = [];
  for (const sr of SERIES) if (levSub(hay, sr[0], true) <= (letters(sr[0]).length >= 10 ? 1 : 0)) out.push(sr);
  for (const l of U.split('\n')) if (/^\s*WZ [0-9]/.test(l)) { out.push(['WZ', 'GB', '', false, 'UK warning series WZ']); break; }
  return out;
}

/** The station a message names ITSELF, matched to a name the TABLE has (fuzzy), or null. */
export interface NavtexSelfId { L: string; row: NavtexStation; raw: string; exact: boolean; d: number; close: boolean }
let NAMES: Array<[string, NavtexStation]> | null = null;
function tableNames(): Array<[string, NavtexStation]> {
  if (!NAMES) {
    const seen = new Map<string, NavtexStation>();
    for (const s of NAVTEX_STATIONS) for (const L of nameKeys(s)) if (L.length >= 4 && !seen.has(L)) seen.set(L, s);
    NAMES = [...seen];
  }
  return NAMES;
}
/** The self-ID lines in the first lines: [the name's letters, the line, match inside it]. */
function selfIdTokens(text: string): Array<[string, string, boolean]> {
  const lines = text.toUpperCase().split('\n').map((l) => l.replace(/[\r\t]/g, ' ').trim()).filter((l) => l.length > 0);
  const from = lines.length && lines[0].startsWith('ZCZC') ? 1 : 0;
  const out: Array<[string, string, boolean]> = [];
  for (let i = from; i < lines.length && i < from + NAVTEX_WEIGHTS.nameLines; i++) {
    let l = lines[i];
    while (l.length && '.,:;'.includes(l[l.length - 1])) l = l.slice(0, -1);
    if (l.length > 40) continue;
    const ws = l.split(' ').filter((w) => w.length > 0);
    const last = ws.length ? ws[ws.length - 1] : '';
    let name = '';
    if (l.endsWith('RADIO')) name = l.slice(0, l.length - 5);
    else if (l.endsWith('NAVTEX')) name = l.slice(0, l.length - 6);
    else if (ws.length >= 2 && last.length === 5 && lev(last, 'RADIO') <= 1) name = ws.slice(0, -1).join(' ');
    if (name) {
      const n = name.split(' ').filter((w) => w.length > 0).join(' ').replace(/^[^A-Z]+/, '');
      if (!GENERIC.includes(n) && letters(n).length >= 4) out.push([letters(n), lines[i], false]);
    }
    if (ws.length >= 2 && lev(letters(ws[0]), 'STATHMOS') <= 1 && letters(ws[1]).length >= 4) out.push([letters(ws[1]), ws[0] + ' ' + ws[1], false]);
    const k = l.indexOf('NAVTEX');
    if (k >= 0) { const m = /^[A-Z]+/.exec(l.slice(k + 6)); if (m && m[0].length >= 4) out.push([m[0], lines[i], true]); }
  }
  return out;
}
export function navtexSelfId(text: string, langs: string[] = navtexLanguages(text), ser: SeriesRow[] = navtexSeries(text)): NavtexSelfId | null {
  for (const [tok, raw, sub] of selfIdTokens(text)) {
    let best = 1000000, second = 1000000, bestL = '', row: NavtexStation | null = null;
    for (const [L, s] of tableNames()) {
      const d = sub ? levSub(tok, L, false) : lev(tok, L);
      // ★ an alias and the name are ONE station: only another station is a rival
      if (d < best) { if (!row || letters(row.name) !== letters(s.name)) second = best; best = d; bestL = L; row = s; }
      else if (d < second && (!row || letters(row.name) !== letters(s.name))) second = d;
    }
    if (!row || second <= best) continue;
    const close = bestL.length >= 5 ? Math.floor(bestL.length / 4) : 0;
    const loose = bestL.length >= 6 ? Math.floor(bestL.length * 0.4) : close;
    const iso = row.iso;
    const backed = ser.some((h) => h[1] === iso) || (langs.length > 0 && (LANGS[iso] ?? []).some((l) => langs.includes(l)));
    if (best <= close || (best <= loose && backed)) return { L: letters(row.name), row, raw, exact: best === 0, d: best, close: best <= close };
  }
  return null;
}

/** Inside letter b1's 10-minute slots (every 4 hours from A 00:00), 'early' s before to 'late' s after the slot's start. */
function slotIn(b1: string, utc: number, early: number, late: number): boolean {
  if (!(b1 >= 'A' && b1 <= 'X') || !Number.isFinite(utc)) return false;
  const start = (b1.charCodeAt(0) - 65) * 600;
  const d = ((((Math.floor(utc) % 14400) - start) % 14400) + 14400) % 14400;
  return d <= late || d >= 14400 - early;
}
function langLine(iso: string, langs: string[]): string[] {
  const l = (LANGS[iso] ?? []).find((x) => langs.includes(x));
  return l ? ['Written in ' + LANG_NAME[l]] : [];
}
function cleanRaw(s: string): string { return s.replace(/\s+/g, ' ').trim(); }

interface Clues { band: number; langs: string[]; ser: SeriesRow[]; sid: NavtexSelfId | null; pos: Array<[number, number]>; t: string }
function clues(b1: string, band: number, text: string): Clues {
  const langs = navtexLanguages(text);
  const ser = navtexSeries(text);
  const sid = navtexSelfId(text, langs, ser);
  return { band, langs, ser, sid, pos: sanePositions(navtexPositions(text), b1, band, sid), t: letters(text) };
}
/** The plain-English evidence for station s from THIS message. */
function whyFor(s: NavtexStation, c: Clues): string[] {
  const w: string[] = [];
  if (c.sid && c.sid.L === letters(s.name)) w.push(c.sid.exact ? 'Identifies itself: “' + cleanRaw(c.sid.raw) + '”'
                                                               : 'Identifies itself as ' + s.name + ' (“' + cleanRaw(c.sid.raw) + '”)');
  else {
    const k = nameKeys(s).find((L) => L.length >= 5 && c.t.includes(L));
    if (k) w.push('Names ' + (k === letters(s.name) ? s.name : k[0] + k.slice(1).toLowerCase()) + ' in the text');
  }
  const sh = c.ser.find((h) => h[1] === s.iso && (!h[2] || h[2] === s.name));
  if (sh) w.push(sh[4]);
  if (c.pos.length && median(c.pos.map(([pa, po]) => km(s.lat, s.lon, pa, po))) <= NAVTEX_WEIGHTS.posFitKm) w.push('Positions in the text lie near ' + s.name);
  w.push(...langLine(s.iso, c.langs));
  return w;
}
/** ★ A CORRUPTED HEADER LETTER: the self-ID or a strong series points to ONE station whose transmitter on this band uses
 *  another letter, and the message started outside b1's slots but inside that transmitter's. */
function headerFix(b1: string, utc: number, hdr: string, c: Clues): { row: NavtexStation; why: string[] } | null {
  if (!Number.isFinite(utc) || slotIn(b1, utc, W2.nearEarly, W2.nearLate)) return null;
  const cands = NAVTEX_STATIONS.filter((s) => s.b1 === b1 && s.khz === c.band);
  let rows: NavtexStation[] = [], because = '';
  if (c.sid) {
    const L = c.sid.L;
    if (cands.some((x) => letters(x.name) === L)) return null;
    rows = NAVTEX_STATIONS.filter((s) => s.khz === c.band && letters(s.name) === L);
    because = c.sid.row.name + ' identifies itself';
  } else {
    const h = c.ser.find((x) => x[3] && !cands.some((y) => y.iso === x[1]));
    if (!h) return null;
    rows = NAVTEX_STATIONS.filter((s) => s.khz === c.band && s.iso === h[1] && (!h[2] || h[2] === s.name));
    because = 'the ' + h[4] + ' points to ' + (rows.length ? rows[0].country : 'another country');
  }
  rows = rows.filter((s) => s.b1 !== b1 && slotIn(s.b1, utc, W2.slotEarly, W2.slotLate));
  if (rows.length !== 1) return null;
  const row = rows[0];
  const why = ['Header letter looks corrupted: started outside ' + b1 + '’s slots; ' + because];
  if (hdr.length >= 2 && hdr[1] === row.b1) why.push('The header’s letters look shifted: “' + hdr.slice(0, 2) + '” — ' + row.b1 + ' is ' + row.name + '’s letter');
  return { row, why: why.concat(whyFor(row, c).filter((w) => !why.some((x) => x.includes(w)))) };
}
function one(row: NavtexStation, lat: number, lon: number, conf: 'confident' | 'likely', why: string[]): NavtexResolution {
  const here = Number.isFinite(lat) && Number.isFinite(lon);
  const d = here ? km(lat, lon, row.lat, row.lon) : NaN;
  return { stations: [row], km: [d], dx: here && d > (row.khz > 1000 ? NAVTEX_WEIGHTS.reachHf : NAVTEX_WEIGHTS.reachMf), conf, why };
}

/** Which station sent letter 'b1', heard on 'dialHz' at a server at 'lat'/'lon' (NaN = unknown), judged from the message
 *  'text', started at 'utc' (UTC seconds; NaN = unknown) with header 'hdr' ("JA12", or the two letters left of "ZCZC VL").
 *  null = the letter only. */
export function navtexResolve(b1: string | null | undefined, dialHz: number, lat: number, lon: number, text = '', utc = NaN, hdr = ''): NavtexResolution | null {
  const band = navtexBandKhz(dialHz);
  if (!b1 || !/^[A-Z]$/.test(b1) || band == null) return null;
  const c = clues(b1, band, text);
  const base = baseResolve(b1, dialHz, lat, lon, text, c.sid);
  const here = Number.isFinite(lat) && Number.isFinite(lon);
  // 1. one station, as the proven rule names it — unless it is past farKm on MF without its name or positions
  if (base && base.stations.length === 1) {
    const s = base.stations[0];
    const far = here && band < 1000 && base.km[0] > W2.farKm;
    if (!far || base.named || base.posWin) {
      const w = whyFor(s, c);
      if (base.priorOnly || !w.length) w.push('Nearest station using ' + s.b1 + ' on ' + band + ' kHz' + (here ? ' (' + kmText(base.km[0]) + ' km)' : ''));
      // ★ a loose self-ID (content-backed) that DECIDED it is likely; one the rule reaches without it stays confident
      let conf: 'confident' | 'likely' = 'confident';
      if (base.fuzzy && c.sid && !c.sid.close) {
        const b0 = baseResolve(b1, dialHz, lat, lon, text, null);
        if (!(b0 && b0.stations.length === 1 && b0.stations[0] === s)) conf = 'likely';
      }
      return { stations: base.stations, km: base.km, dx: base.dx, conf, why: w };
    }
  }
  // 2. a corrupted header letter
  const fix = headerFix(b1, utc, hdr, c);
  if (fix) return one(fix.row, lat, lon, 'likely', fix.why);
  // 3. a known station names itself and is none of the candidates
  if (base && base.text && c.sid) {
    return { stations: [], km: [], dx: false, text: base.text, conf: 'likely',
             why: whyFor(c.sid.row, c).concat([c.sid.row.name + ' is not listed on ' + b1 + ' at ' + band + ' kHz']) };
  }
  // 4. undecided: the issuer series, a choice among the candidates
  const cands = NAVTEX_STATIONS.filter((s) => s.b1 === b1 && s.khz === band);
  for (const h of c.ser) {
    const hit = cands.filter((s) => s.iso === h[1] && (!h[2] || h[2] === s.name));
    if (hit.length === 1) return one(hit[0], lat, lon, 'likely', whyFor(hit[0], c));
  }
  // 5. too close to call
  if (base && base.stations.length === 2) {
    const [a, b] = base.stations;
    return { stations: base.stations, km: base.km, dx: false, conf: 'ambiguous',
             why: ['Two stations use ' + b1 + ' on ' + band + ' kHz: ' + a.name + (here ? ' (' + kmText(base.km[0]) + ' km)' : '') + ' and ' + b.name + (here ? ' (' + kmText(base.km[1]) + ' km)' : '')] };
  }
  return null;
}

/** ★ Diagnostic for the Lenovo replay: the station a corrupted-header reading WOULD give, whatever the rule decided. */
export function navtexHeaderFixProbe(b1: string, dialHz: number, text: string, utc: number, hdr = ''): string | null {
  const band = navtexBandKhz(dialHz);
  if (band == null || !/^[A-Z]$/.test(b1)) return null;
  const f = headerFix(b1, utc, hdr, clues(b1, band, text));
  return f ? f.row.name : null;
}

/** ★ A message whose start was lost: its station only from its own text — a surviving "ZCZC VL" fragment through the rule
 *  (never on the distance alone), else a self-ID or strong series whose station is the ONE on this band in its slot.
 *  Always likely. null = say nothing. */
export function navtexStartLostResolve(dialHz: number, lat: number, lon: number, text: string, utc = NaN): NavtexResolution | null {
  const band = navtexBandKhz(dialHz);
  if (band == null) return null;
  const first = (text.split('\n').find((l) => l.trim().length > 0) ?? '').toUpperCase();
  const m = /(?:ZCZC|(?<!Z)CZC|ZCZ(?!C)) ?([A-Z])([A-Z])?(?![A-Z])/.exec(first);
  if (m) {
    const r = navtexResolve(m[1], dialHz, lat, lon, text, utc, m[1] + (m[2] ?? ''));
    // ★ only on the message's own text — a self-ID, a series, a name, positions — never distance or language alone
    const textual = (r?.why ?? []).some((w) => !w.startsWith('Nearest station') && !w.startsWith('Written in') && !w.startsWith('Two stations'));
    if (r && (r.stations.length === 1 || r.text) && textual)
      return { ...r, conf: 'likely', why: ['Start of the message lost'].concat(r.why ?? []) };
    return null;
  }
  const c = clues('A', band, text);
  let rows: NavtexStation[] = [];
  if (c.sid) rows = NAVTEX_STATIONS.filter((s) => s.khz === band && letters(s.name) === c.sid!.L);
  else { const h = c.ser.find((x) => x[3]); if (h) rows = NAVTEX_STATIONS.filter((s) => s.khz === band && s.iso === h[1] && (!h[2] || h[2] === s.name)); }
  rows = rows.filter((s) => slotIn(s.b1, utc, W2.slotEarly, W2.slotLate));
  if (rows.length !== 1) return null;
  return one(rows[0], lat, lon, 'likely', ['Start of the message lost; it started in ' + rows[0].b1 + '’s slot'].concat(whyFor(rows[0], c)));
}
/** "E · Suspected Mondolfo, Italy 🇮🇹" for a start-lost message, or null. */
export function navtexStartLostLabel(dialHz: number, lat: number, lon: number, text: string, utc = NaN): string | null {
  const r = navtexStartLostResolve(dialHz, lat, lon, text, utc);
  if (!r) return null;
  const m = /(?:ZCZC|(?<!Z)CZC|ZCZ(?!C)) ?([A-Z])([A-Z])?(?![A-Z])/.exec((text.split('\n').find((l) => l.trim().length > 0) ?? '').toUpperCase());
  return navtexLabelOf(r.stations.length ? r.stations[0].b1 : m ? m[1] : '?', r);
}
`
  .replace('@@SERIES@@', JSON.stringify(SERIES)).replace('@@LANG_NAME@@', JSON.stringify(LANG_NAME)).replace('@@W2@@', JSON.stringify(W2)).replace('@@NAME_ALIAS@@', JSON.stringify(NAME_ALIAS));

const CPP_PRE = String.raw`// ── ★★★ 2026-11-01: THE ADDITIVE CLUES — the same as navtexStations.generated.ts (the generator's header explains each) ──
struct SeriesRow { const char* phrase; const char* iso; const char* name; bool strong; const char* what; };
inline const std::vector<SeriesRow>& seriesRows() {
    static const std::vector<SeriesRow> t = {
@@SERIES@@
    };
    return t;
}
inline const char* langName(const std::string& l) {
@@LANG_NAME@@
    return "";
}
/** ★ Other names a station goes by in its own messages ("HAMBURG" for Pinneberg) — matched like its name. */
inline std::vector<std::string> nameKeys(const Station* s) {
    std::vector<std::string> k = { letters(s->name) };
@@NAME_ALIAS@@
    return k;
}
/** navtexCanonicalName(): the station's own name for a name or an alias ("Hamburg" → "Pinneberg"). */
inline std::string canonicalName(const std::string& name) {
    const std::string L = letters(name);
    size_t n = 0; const Station* t = stations(n);
    for (size_t i = 0; i < n; i++) for (auto& k : nameKeys(&t[i])) if (k == L) return t[i].name;
    return name;
}
constexpr double kServiceMfKm = @@serviceMf@@, kServiceHfKm = @@serviceHf@@, kFarKm = @@farKm@@;
constexpr int kSlotEarly = @@slotEarly@@, kSlotLate = @@slotLate@@, kNearEarly = @@nearEarly@@, kNearLate = @@nearLate@@;

/** lev(): Levenshtein distance. */
inline int lev(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++) cur[j] = std::min({ prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1), prev[j] + 1, cur[j - 1] + 1 });
        std::swap(prev, cur);
    }
    return prev[b.size()];
}
/** levSub(): the fewest edits that make w out of some stretch of hay — anywhere, or (wordStart) only at a word's start. */
inline int levSub(const std::string& hay, const std::string& w, bool wordStart) {
    const int BIG = 1000000;
    std::vector<int> prev(hay.size() + 1), cur(hay.size() + 1);
    for (size_t j = 0; j <= hay.size(); j++) prev[j] = !wordStart || j == 0 || hay[j - 1] == ' ' ? 0 : BIG;
    for (size_t i = 1; i <= w.size(); i++) {
        cur[0] = prev[0] + 1;
        for (size_t j = 1; j <= hay.size(); j++) cur[j] = std::min({ prev[j - 1] + (w[i - 1] == hay[j - 1] ? 0 : 1), prev[j] + 1, cur[j - 1] + 1 });
        std::swap(prev, cur);
    }
    int best = BIG; for (int v : prev) best = std::min(best, v);
    return best;
}
/** navtexSeries(): the series and sea areas the text carries, and "WZ 649/26" at a line's start. */
inline std::vector<SeriesRow> seriesHits(const std::string& text) {
    const std::string U = upperAscii(text);
    std::string hay; bool sp = false;
    for (char c : U) { if (c >= 'A' && c <= 'Z') { hay += c; sp = false; } else if (!sp) { hay += ' '; sp = true; } }
    while (!hay.empty() && hay[0] == ' ') hay.erase(0, 1);
    while (!hay.empty() && hay.back() == ' ') hay.pop_back();
    std::vector<SeriesRow> out;
    for (const auto& sr : seriesRows()) if (levSub(hay, sr.phrase, true) <= (letters(sr.phrase).size() >= 10 ? 1 : 0)) out.push_back(sr);
    size_t p = 0;
    while (p <= U.size()) {
        size_t e = U.find('\n', p); if (e == std::string::npos) e = U.size();
        size_t q = p; while (q < e && (U[q] == ' ' || U[q] == '\t' || U[q] == '\r')) q++;
        if (q + 3 < e && U.compare(q, 3, "WZ ") == 0 && U[q + 3] >= '0' && U[q + 3] <= '9') { out.push_back({ "WZ", "GB", "", false, "UK warning series WZ" }); break; }
        p = e + 1;
    }
    return out;
}
/** navtexSelfId(): the station a message names ITSELF, matched to a name the TABLE has. */
struct SelfId { std::string L; const Station* row = nullptr; std::string raw; bool exact = false, close = false; int d = 0; };
inline const std::vector<std::pair<std::string, const Station*>>& tableNames() {
    static const std::vector<std::pair<std::string, const Station*>> t = [] {
        std::vector<std::pair<std::string, const Station*>> o;
        size_t n = 0; const Station* s = stations(n);
        for (size_t i = 0; i < n; i++) for (const auto& L : nameKeys(&s[i])) {
            if (L.size() < 4) continue;
            bool seen = false; for (auto& e : o) if (e.first == L) seen = true;
            if (!seen) o.push_back({ L, &s[i] });
        }
        return o;
    }();
    return t;
}
struct SidTok { std::string tok, raw; bool sub; };
inline std::vector<SidTok> selfIdTokens(const std::string& text) {
    std::vector<std::string> lines; std::string cur;
    const std::string s = upperAscii(text);
    auto trim = [](std::string x) { size_t a = 0, b = x.size(); while (a < b && x[a] == ' ') a++; while (b > a && x[b - 1] == ' ') b--; return x.substr(a, b - a); };
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == '\n') { const std::string t = trim(cur); if (!t.empty()) lines.push_back(t); cur.clear(); }
        else cur += (s[i] == '\r' || s[i] == '\t') ? ' ' : s[i];
    }
    std::vector<SidTok> out;
    const size_t from = !lines.empty() && lines[0].compare(0, 4, "ZCZC") == 0 ? 1 : 0;
    for (size_t i = from; i < lines.size() && i < from + @@nameLines@@; i++) {
        std::string l = lines[i];
        while (!l.empty() && std::strchr(".,:;", l.back()) && l.back() != 0) l.pop_back();
        if (l.size() > 40) continue;
        std::vector<std::string> ws; { std::string w; for (char c : l + " ") { if (c == ' ') { if (!w.empty()) ws.push_back(w); w.clear(); } else w += c; } }
        const std::string last = ws.empty() ? std::string() : ws.back();
        auto ends = [&](const char* x) { const size_t n = std::strlen(x); return l.size() >= n && l.compare(l.size() - n, n, x) == 0; };
        std::string name; bool have = false;
        if (ends("RADIO")) { name = l.substr(0, l.size() - 5); have = true; }
        else if (ends("NAVTEX")) { name = l.substr(0, l.size() - 6); have = true; }
        else if (ws.size() >= 2 && last.size() == 5 && lev(last, "RADIO") <= 1) { for (size_t k = 0; k + 1 < ws.size(); k++) { if (k) name += ' '; name += ws[k]; } have = true; }
        if (have) {
            std::string n, w;
            for (char c : name + " ") { if (c == ' ') { if (!w.empty()) { if (!n.empty()) n += ' '; n += w; w.clear(); } } else w += c; }
            size_t a = 0; while (a < n.size() && !(n[a] >= 'A' && n[a] <= 'Z')) a++;
            n = n.substr(a);
            bool generic = false; for (auto* g : genericNames()) if (n == g) generic = true;
            if (!generic && letters(n).size() >= 4) out.push_back({ letters(n), lines[i], false });
        }
        if (ws.size() >= 2 && lev(letters(ws[0]), "STATHMOS") <= 1 && letters(ws[1]).size() >= 4) out.push_back({ letters(ws[1]), ws[0] + " " + ws[1], false });
        const size_t k = l.find("NAVTEX");
        if (k != std::string::npos) {
            std::string m; for (size_t j = k + 6; j < l.size() && l[j] >= 'A' && l[j] <= 'Z'; j++) m += l[j];
            if (m.size() >= 4) out.push_back({ m, lines[i], true });
        }
    }
    return out;
}
inline SelfId selfId(const std::string& text, const std::vector<std::string>& langs, const std::vector<SeriesRow>& ser) {
    for (const auto& tk : selfIdTokens(text)) {
        int best = 1000000, second = 1000000; std::string bestL; const Station* row = nullptr;
        for (const auto& e : tableNames()) {
            const int d = tk.sub ? levSub(tk.tok, e.first, false) : lev(tk.tok, e.first);
            // ★ an alias and the name are ONE station: only another station is a rival
            const bool same = row && letters(row->name) == letters(e.second->name);
            if (d < best) { if (!same) second = best; best = d; bestL = e.first; row = e.second; }
            else if (d < second && !same) second = d;
        }
        if (!row || second <= best) continue;
        const int close = bestL.size() >= 5 ? (int)(bestL.size() / 4) : 0;
        const int loose = bestL.size() >= 6 ? (int)std::floor(bestL.size() * 0.4) : close;
        bool backed = false;
        for (auto& h : ser) if (std::strcmp(h.iso, row->iso) == 0) backed = true;
        if (!langs.empty()) for (auto& l : isoLangs(row->iso)) if (std::find(langs.begin(), langs.end(), l) != langs.end()) backed = true;
        if (best <= close || (best <= loose && backed)) { SelfId r; r.L = letters(row->name); r.row = row; r.raw = tk.raw; r.exact = best == 0; r.d = best; r.close = best <= close; return r; }
    }
    return SelfId();
}
/** ★ Garbled positions carry no weight: further than a service area from every transmitter using the letter on the band
 *  (or from the station naming itself) = damage, dropped. */
inline std::vector<std::pair<double, double>> sanePositions(const std::vector<std::pair<double, double>>& pos, char b1, double band, const SelfId* sid) {
    const double lim = band > 1000 ? kServiceHfKm : kServiceMfKm;
    size_t n = 0; const Station* t = stations(n);
    std::vector<std::pair<double, double>> out;
    for (auto& p : pos) {
        bool ok = false;
        for (size_t i = 0; i < n && !ok; i++)
            if ((t[i].b1 == b1 && t[i].khz == band) || (sid && !sid->L.empty() && letters(t[i].name) == sid->L)) ok = km(t[i].lat, t[i].lon, p.first, p.second) <= lim;
        if (ok) out.push_back(p);
    }
    return out;
}
inline bool slotIn(char b1, double utc, int early, int late) {
    if (b1 < 'A' || b1 > 'X' || !std::isfinite(utc)) return false;
    const long long start = (long long)(b1 - 'A') * 600;
    const long long d = ((((long long)std::floor(utc) % 14400) - start) % 14400 + 14400) % 14400;
    return d <= late || d >= 14400 - early;
}

`
  .replace('@@SERIES@@', SERIES.map(([p, iso, n, st, w]) => `        { ${JSON.stringify(p)}, "${iso}", ${JSON.stringify(n)}, ${st}, ${JSON.stringify(w)} },`).join('\n'))
  .replace('@@NAME_ALIAS@@', Object.entries(NAME_ALIAS).map(([n, al]) => `    if (std::strcmp(s->name, ${JSON.stringify(n)}) == 0) { ${al.map((a) => `k.push_back(${JSON.stringify(a)});`).join(' ')} }`).join('\n'))
  .replace('@@LANG_NAME@@', Object.entries(LANG_NAME).map(([l, n]) => `    if (l == "${l}") return "${n}";`).join('\n'))
  .replace(/@@(serviceMf|serviceHf|farKm|slotEarly|slotLate|nearEarly|nearLate)@@/g, (_, k) => String(W2[k])).replace('@@nameLines@@', String(W.nameLines));
const CPP_POST = String.raw`
// ── ★★★ 2026-11-01: the wrapper — confidence, the why, a corrupted header letter (navtexResolve, the same steps) ──
struct Clues { double band = 0; std::vector<std::string> langs; std::vector<SeriesRow> ser; SelfId sid; std::vector<std::pair<double, double>> pos; std::string t; };
inline Clues clues(char b1, double band, const std::string& text) {
    Clues c; c.band = band; c.langs = languages(text); c.ser = seriesHits(text); c.sid = selfId(text, c.langs, c.ser);
    c.pos = sanePositions(positions(text), b1, band, &c.sid); c.t = letters(text);
    return c;
}
inline std::string cleanRaw(const std::string& s) {
    std::string o; bool sp = false;
    for (char c : s) { if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { sp = !o.empty(); continue; } if (sp) o += ' '; sp = false; o += c; }
    return o;
}
/** whyFor(): the plain-English evidence for station s from THIS message. */
inline std::vector<std::string> whyFor(const Station* s, const Clues& c) {
    std::vector<std::string> w;
    const std::string L = letters(s->name);
    if (!c.sid.L.empty() && c.sid.L == L)
        w.push_back(c.sid.exact ? "Identifies itself: \xe2\x80\x9c" + cleanRaw(c.sid.raw) + "\xe2\x80\x9d"
                                : std::string("Identifies itself as ") + s->name + " (\xe2\x80\x9c" + cleanRaw(c.sid.raw) + "\xe2\x80\x9d)");
    else for (const auto& k : nameKeys(s)) if (k.size() >= 5 && c.t.find(k) != std::string::npos) {
        std::string shown = k == L ? std::string(s->name) : k.substr(0, 1);
        if (k != L) for (size_t j = 1; j < k.size(); j++) shown += (char)std::tolower((unsigned char)k[j]);
        w.push_back("Names " + shown + " in the text"); break;
    }
    for (auto& h : c.ser) if (std::strcmp(h.iso, s->iso) == 0 && (!*h.name || std::strcmp(h.name, s->name) == 0)) { w.push_back(h.what); break; }
    if (!c.pos.empty() && medianKm(s->lat, s->lon, c.pos) <= kPosFitKm) w.push_back(std::string("Positions in the text lie near ") + s->name);
    for (auto& l : isoLangs(s->iso)) if (std::find(c.langs.begin(), c.langs.end(), l) != c.langs.end()) { w.push_back(std::string("Written in ") + langName(l)); break; }
    return w;
}
/** headerFix(): a corrupted header letter — see navtexResolve. nullptr = none. */
inline const Station* headerFix(char b1, double utc, const std::string& hdr, const Clues& c, std::vector<std::string>& why) {
    if (!std::isfinite(utc) || slotIn(b1, utc, kNearEarly, kNearLate)) return nullptr;
    size_t n = 0; const Station* t = stations(n);
    auto isCand = [&](const Station& s) { return s.b1 == b1 && s.khz == c.band; };
    std::vector<const Station*> rows; std::string because;
    if (!c.sid.L.empty()) {
        for (size_t i = 0; i < n; i++) if (isCand(t[i]) && letters(t[i].name) == c.sid.L) return nullptr;
        for (size_t i = 0; i < n; i++) if (t[i].khz == c.band && letters(t[i].name) == c.sid.L) rows.push_back(&t[i]);
        because = std::string(c.sid.row->name) + " identifies itself";
    } else {
        const SeriesRow* h = nullptr;
        for (auto& x : c.ser) {
            if (!x.strong) continue;
            bool covered = false; for (size_t i = 0; i < n; i++) if (isCand(t[i]) && std::strcmp(t[i].iso, x.iso) == 0) covered = true;
            if (!covered) { h = &x; break; }
        }
        if (!h) return nullptr;
        for (size_t i = 0; i < n; i++) if (t[i].khz == c.band && std::strcmp(t[i].iso, h->iso) == 0 && (!*h->name || std::strcmp(h->name, t[i].name) == 0)) rows.push_back(&t[i]);
        because = std::string("the ") + h->what + " points to " + (rows.empty() ? std::string("another country") : std::string(rows[0]->country));
    }
    std::vector<const Station*> in;
    for (auto r : rows) if (r->b1 != b1 && slotIn(r->b1, utc, kSlotEarly, kSlotLate)) in.push_back(r);
    if (in.size() != 1) return nullptr;
    const Station* row = in[0];
    why.push_back(std::string("Header letter looks corrupted: started outside ") + b1 + "\xe2\x80\x99" "s slots; " + because);
    if (hdr.size() >= 2 && hdr[1] == row->b1)
        why.push_back("The header\xe2\x80\x99" "s letters look shifted: \xe2\x80\x9c" + hdr.substr(0, 2) + "\xe2\x80\x9d \xe2\x80\x94 " + std::string(1, row->b1) + " is " + row->name + "\xe2\x80\x99" "s letter");
    const size_t keep = why.size();
    for (auto& w : whyFor(row, c)) { bool dup = false; for (size_t k = 0; k < keep; k++) if (why[k].find(w) != std::string::npos) dup = true; if (!dup) why.push_back(w); }
    return row;
}
inline Resolution one(const Station* row, double lat, double lon, const char* conf, std::vector<std::string> why) {
    Resolution r;
    const bool here = std::isfinite(lat) && std::isfinite(lon);
    const double d = here ? km(lat, lon, row->lat, row->lon) : NAN;
    r.stations = { row }; r.km = { d }; r.dx = here && d > (row->khz > 1000 ? kReachHfKm : kReachMfKm); r.conf = conf; r.why = std::move(why);
    return r;
}
/** navtexResolve(): the proven rule (baseResolve), then the additive clues. utc = when the message started (UTC s, NaN =
 *  unknown); hdr = its header ("JA12", or "VL" from a short "ZCZC VL"). */
inline Resolution resolveAll(char b1, double dialHz, double lat, double lon, const std::string& text = "", double utc = NAN, const std::string& hdr = "") {
    Resolution none;
    const double band = bandKhz(dialHz);
    if (b1 < 'A' || b1 > 'Z' || band == 0) return none;
    const Clues c = clues(b1, band, text);
    Base f;
    Resolution base = baseResolve(b1, dialHz, lat, lon, text, c.sid.L.empty() ? nullptr : &c.sid, f);
    const bool here = std::isfinite(lat) && std::isfinite(lon);
    char bb[32]; std::snprintf(bb, sizeof bb, "%g", band);
    // 1. one station, as the proven rule names it — unless past kFarKm on MF without its name or positions
    if (base.stations.size() == 1) {
        const Station* s = base.stations[0];
        const bool far = here && band < 1000 && base.km[0] > kFarKm;
        if (!far || f.named || f.posWin) {
            std::vector<std::string> w = whyFor(s, c);
            if (f.priorOnly || w.empty()) w.push_back(std::string("Nearest station using ") + s->b1 + " on " + bb + " kHz" + (here ? " (" + kmText(base.km[0]) + " km)" : std::string()));
            std::string conf = "confident";
            if (f.fuzzy && !c.sid.L.empty() && !c.sid.close) {
                Base f0; const Resolution b0 = baseResolve(b1, dialHz, lat, lon, text, nullptr, f0);
                if (!(b0.stations.size() == 1 && b0.stations[0] == s)) conf = "likely";
            }
            base.conf = conf; base.why = w;
            return base;
        }
    }
    // 2. a corrupted header letter
    { std::vector<std::string> w; if (const Station* fx = headerFix(b1, utc, hdr, c, w)) return one(fx, lat, lon, "likely", w); }
    // 3. a known station names itself and is none of the candidates
    if (!base.textName.empty() && !c.sid.L.empty()) {
        base.conf = "likely";
        base.why = whyFor(c.sid.row, c);
        base.why.push_back(std::string(c.sid.row->name) + " is not listed on " + b1 + " at " + bb + " kHz");
        return base;
    }
    // 4. undecided: the issuer series, a choice among the candidates
    size_t n = 0; const Station* t = stations(n);
    for (auto& h : c.ser) {
        const Station* hit = nullptr; int k = 0;
        for (size_t i = 0; i < n; i++)
            if (t[i].b1 == b1 && t[i].khz == band && std::strcmp(t[i].iso, h.iso) == 0 && (!*h.name || std::strcmp(h.name, t[i].name) == 0)) { hit = &t[i]; k++; }
        if (k == 1) return one(hit, lat, lon, "likely", whyFor(hit, c));
    }
    // 5. too close to call
    if (base.stations.size() == 2) {
        base.conf = "ambiguous";
        base.why = { std::string("Two stations use ") + b1 + " on " + bb + " kHz: " + base.stations[0]->name + (here ? " (" + kmText(base.km[0]) + " km)" : std::string())
                     + " and " + base.stations[1]->name + (here ? " (" + kmText(base.km[1]) + " km)" : std::string()) };
        return base;
    }
    return none;
}
/** navtexHeaderFixProbe(): the station a corrupted-header reading WOULD give, whatever the rule decided (diagnostics). */
inline const Station* headerFixProbe(char b1, double dialHz, const std::string& text, double utc, const std::string& hdr = "") {
    const double band = bandKhz(dialHz);
    if (b1 < 'A' || b1 > 'Z' || band == 0) return nullptr;
    std::vector<std::string> w;
    return headerFix(b1, utc, hdr, clues(b1, band, text), w);
}
/** The first non-blank line's "ZCZC VL" fragment: its letters, or "" (navtexStartLostResolve's regex). */
inline std::string headerFragment(const std::string& text) {
    const std::string U = upperAscii(text);
    size_t p = 0; std::string first;
    while (p <= U.size()) { size_t e = U.find('\n', p); if (e == std::string::npos) e = U.size(); std::string l = U.substr(p, e - p);
        bool blank = true; for (char c : l) if (c != ' ' && c != '\t' && c != '\r') blank = false;
        if (!blank) { first = l; break; } p = e + 1; }
    for (size_t i = 0; i < first.size(); i++) {
        size_t j = std::string::npos;
        if (first.compare(i, 4, "ZCZC") == 0) j = i + 4;
        else if ((first.compare(i, 3, "CZC") == 0 && (i == 0 || first[i - 1] != 'Z')) || (first.compare(i, 3, "ZCZ") == 0 && (i + 3 >= first.size() || first[i + 3] != 'C'))) j = i + 3;
        if (j == std::string::npos) continue;
        if (j < first.size() && first[j] == ' ') j++;
        if (j >= first.size() || first[j] < 'A' || first[j] > 'Z') continue;
        std::string h(1, first[j]);
        if (j + 1 < first.size() && first[j + 1] >= 'A' && first[j + 1] <= 'Z') {
            if (j + 2 < first.size() && first[j + 2] >= 'A' && first[j + 2] <= 'Z') continue;   // a word, not a header
            h += first[j + 1];
        }
        return h;
    }
    return std::string();
}
/** navtexStartLostResolve(): a message whose start was lost — its station only from its own text. Always likely. */
inline Resolution startLostResolve(double dialHz, double lat, double lon, const std::string& text, double utc = NAN) {
    Resolution none;
    const double band = bandKhz(dialHz);
    if (band == 0) return none;
    const std::string h = headerFragment(text);
    if (!h.empty()) {
        Resolution r = resolveAll(h[0], dialHz, lat, lon, text, utc, h);
        bool textual = false;
        for (auto& w : r.why) if (w.compare(0, 15, "Nearest station") != 0 && w.compare(0, 10, "Written in") != 0 && w.compare(0, 12, "Two stations") != 0) textual = true;
        if ((r.stations.size() == 1 || !r.textName.empty()) && textual) { r.conf = "likely"; r.why.insert(r.why.begin(), "Start of the message lost"); return r; }
        return none;
    }
    const Clues c = clues('A', band, text);
    size_t n = 0; const Station* t = stations(n);
    std::vector<const Station*> rows;
    if (!c.sid.L.empty()) { for (size_t i = 0; i < n; i++) if (t[i].khz == band && letters(t[i].name) == c.sid.L) rows.push_back(&t[i]); }
    else {
        const SeriesRow* h2 = nullptr; for (auto& x : c.ser) if (x.strong) { h2 = &x; break; }
        if (h2) for (size_t i = 0; i < n; i++) if (t[i].khz == band && std::strcmp(t[i].iso, h2->iso) == 0 && (!*h2->name || std::strcmp(h2->name, t[i].name) == 0)) rows.push_back(&t[i]);
    }
    std::vector<const Station*> in; for (auto r : rows) if (slotIn(r->b1, utc, kSlotEarly, kSlotLate)) in.push_back(r);
    if (in.size() != 1) return none;
    std::vector<std::string> w = { std::string("Start of the message lost; it started in ") + in[0]->b1 + "\xe2\x80\x99" "s slot" };
    for (auto& x : whyFor(in[0], c)) w.push_back(x);
    return one(in[0], lat, lon, "likely", w);
}
/** The gallery's test: one station named, or a station named by its own message (never the "or" pair). */
inline bool nameable(char b1, double dialHz, double lat, double lon, const std::string& text = "", double utc = NAN, const std::string& hdr = "") {
    const Resolution r = resolveAll(b1, dialHz, lat, lon, text, utc, hdr);
    return r.stations.size() == 1 || !r.textName.empty();
}
/** navtexStation(): the station when ONE can be named, else nullptr. */
inline const Station* resolve(char b1, double dialHz, double lat, double lon, const std::string& text = "", double utc = NAN, const std::string& hdr = "") {
    const Resolution r = resolveAll(b1, dialHz, lat, lon, text, utc, hdr);
    return r.stations.size() == 1 ? r.stations[0] : nullptr;
}
`;

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
  /** ★ 2026-11-01: confident (the plain name), likely ("Suspected <name>"), ambiguous (the pair). */
  conf?: 'confident' | 'likely' | 'ambiguous';
  /** ★ Why, in plain English, for THIS message (strongest first) — never a score. */
  why?: string[];
}
interface BaseResolution extends NavtexResolution { named: boolean; fuzzy: boolean; posWin: boolean; priorOnly: boolean }

/** ★ (b) The station a message names itself, when it is none of the candidates — always a station the TABLE has (the
 *  fuzzy self-ID matches known names only; an unknown name is ignored, 2026-11-01). */
function textStation(sid: NavtexSelfId): NavtexTextStation {
  return { name: sid.row.name, iso: sid.row.iso, country: sid.row.country };
}

/** Which station sent letter \`b1\`, heard on \`dialHz\` at a server at \`lat\`/\`lon\` (NaN = unknown), judged from the
 *  message \`text\`. null = the letter only. */
function baseResolve(b1: string | null | undefined, dialHz: number, lat: number, lon: number, text: string, sid: NavtexSelfId | null): BaseResolution | null {
  if (!b1 || !/^[A-Z]$/.test(b1)) return null;
  const band = navtexBandKhz(dialHz);
  if (band == null) return null;
  const cands = NAVTEX_STATIONS.filter((s) => s.b1 === b1 && s.khz === band);
  if (!cands.length) return null;
  const W = NAVTEX_WEIGHTS;
  const here = Number.isFinite(lat) && Number.isFinite(lon);
  const reach = band > 1000 ? W.reachHf : W.reachMf;
  const t = letters(text);
  const pos = sanePositions(navtexPositions(text), b1, band, sid);
  const langs = navtexLanguages(text);
  const scored = cands.map((s) => {
    const d = here ? km(lat, lon, s.lat, s.lon) : NaN;
    let sc = here ? 1 / (1 + (d / W.priorKm) ** 2) : 1;
    const exact = nameKeys(s).some((L) => L.length >= 5 && t.includes(L));   // ★ + its aliases ("HAMBURG" for Pinneberg)
    const named = exact || (sid !== null && letters(s.name) === sid.L);   // ★ + the fuzzy self-ID (known names only)
    if (named) sc *= W.named;
    let med = Infinity;
    if (pos.length) {
      const ds = pos.map(([pa, po]) => km(s.lat, s.lon, pa, po)).sort((x, y) => x - y);
      med = ds.length % 2 ? ds[(ds.length - 1) / 2] : (ds[ds.length / 2 - 1] + ds[ds.length / 2]) / 2;
      sc *= W.posFloor + W.posGain / (1 + (med / W.posKm) ** 2);
    }
    const speaks = langs.length > 0 && (LANGS[s.iso] ?? []).some((l) => langs.includes(l));
    if (speaks) sc *= band === 490 ? W.lang490 : W.langOther;
    return { s, d, sc, named, exact, med, speaks };
  }).sort((x, y) => y.sc - x.sc || x.s.name.localeCompare(y.s.name));
  const named = scored.some((x) => x.named);
  const posFit = pos.length > 0 && scored.some((x) => x.med <= W.posFitKm);
  // ★★ (b) THE STATION NAMES ITSELF AND IT IS NOT A CANDIDATE (Mondolfo, 2026-10-23: an Italian warning off Ravenna was
  //    "Corsen, France"). Only when the message ALSO rules every candidate out — positions that fit none of them, or,
  //    with no positions, a language none of them speaks. Positions that fit a candidate always keep it: a station
  //    may send another language (Niton's T on 490 is French), and language alone never outweighs the table.
  const contradicted = !posFit && (pos.length > 0 || (langs.length > 0 && !scored.some((x) => x.speaks)));
  if (sid && !named && !scored.some((x) => letters(x.s.name) === sid.L)) {
    if (contradicted) return { stations: [], km: [], dx: false, text: textStation(sid), named: false, fuzzy: false, posWin: false, priorOnly: false };
  }
  // ★ a "<NAME> RADIO" the table does not know, and the text rules every candidate out: the letter only — the name is
  //   NEVER shown (2026-11-01: "Condogno Radio, Italy?" was a station that does not exist)
  if (!sid && !named && contradicted && navtexTextName(text)) return null;
  if (pos.length && !posFit && !named) return null;            // ★ positions nowhere near any candidate: not one of them
  const evidence = named || posFit || scored.some((x) => x.speaks);
  const best = scored[0], next = scored[1];
  if (!evidence && (!here || !(best.d <= reach))) return null;   // ★ no evidence and nothing near: the letter only
  const flags = { named: best.named, fuzzy: best.named && !best.exact && !(sid && sid.exact), posWin: best.med <= W.posFitKm, priorOnly: !evidence };
  if (!next || best.sc >= W.clear * next.sc) return { stations: [best.s], km: [best.d], dx: here && best.d > reach, ...flags };
  return { stations: [best.s, next.s], km: [best.d, next.d], dx: false, ...flags };
}
${TS_EXTRA}

/** The station, when ONE can be named — or null. */
export function navtexStation(b1: string | null | undefined, dialHz: number, lat: number, lon: number, text = '', utc = NaN, hdr = ''): NavtexStation | null {
  const r = navtexResolve(b1, dialHz, lat, lon, text, utc, hdr);
  return r && r.stations.length === 1 ? r.stations[0] : null;
}

/** "7,500" — to the nearest 100 km. */
function kmText(d: number): string {
  const t = String(Math.round(d / 100) * 100);
  let o = '';
  for (let i = 0; i < t.length; i++) { if (i && (t.length - i) % 3 === 0) o += ','; o += t[i]; }
  return o;
}

/** The languages a station's country broadcasts in ('IT' → ['it']) — the gallery's TRANSLATE takes its hint here. */
export function navtexIsoLanguages(iso: string): readonly string[] { return LANGS[iso] ?? []; }

/** "🇧🇪" from "BE". */
export function navtexFlag(iso: string): string {
  return /^[A-Z]{2}$/.test(iso) ? String.fromCodePoint(0x1f1e6 + iso.charCodeAt(0) - 65, 0x1f1e6 + iso.charCodeAt(1) - 65) : '';
}

/** "B · Oostende, Belgium 🇧🇪", "R · Isabella, United States 🇺🇸 · DX 6,700 km", "W · Corsen 🇫🇷 or Valentia 🇮🇪?", or
 *  "Station B" when the station cannot be named. */
export function navtexStationLabel(b1: string | null | undefined, dialHz: number, lat: number, lon: number, text = '', utc = NaN, hdr = ''): string {
  return navtexLabelOf(b1, navtexResolve(b1, dialHz, lat, lon, text, utc, hdr));
}
/** The label of a resolution: "E · Mondolfo, Italy 🇮🇹" (confident), "E · Suspected Mondolfo, Italy 🇮🇹" (likely — on the
 *  STATION's letter when the header's looked corrupted), "W · Corsen 🇫🇷 or Valentia 🇮🇪?", or "Station B". */
export function navtexLabelOf(b1: string | null | undefined, r: NavtexResolution | null): string {
  if (!r) return b1 ? \`Station \${b1}\` : 'Station ?';
  if (r.text) return \`\${b1} \\u00b7 Suspected \${r.text.name}\${r.text.country ? \`, \${r.text.country} \${navtexFlag(r.text.iso)}\` : ''}\`;
  const [a, b] = r.stations;
  if (b) return \`\${a.b1} \\u00b7 \${a.name} \${navtexFlag(a.iso)} or \${b.name} \${navtexFlag(b.iso)}?\`;
  return \`\${a.b1} \\u00b7 \${r.conf === 'likely' ? 'Suspected ' : ''}\${a.name}, \${a.country} \${navtexFlag(a.iso)}\${r.dx ? \` \\u00b7 DX \${kmText(r.km[0])} km\` : ''}\`;
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

${CPP_PRE}/** baseResolve() — the PROVEN rule (navtexResolve's baseResolve): 0 stations = the letter only (or, with textName set, a
 *  station the table has on another letter or band, naming itself); 1 = named; 2 = too close to call. */
struct Resolution { std::vector<const Station*> stations; std::vector<double> km; bool dx = false;
                    std::string textName, textIso, textCountry;
                    std::string conf;                 ///< ★ "confident" | "likely" ("Suspected …") | "ambiguous" | "" (none)
                    std::vector<std::string> why; };  ///< ★ plain English, THIS message's evidence, strongest first
struct Base { bool named = false, fuzzy = false, posWin = false, priorOnly = false; };
inline Resolution baseResolve(char b1, double dialHz, double lat, double lon, const std::string& text, const SelfId* sid, Base& f) {
    Resolution r;
    if (b1 < 'A' || b1 > 'Z') return r;
    const double band = bandKhz(dialHz);
    if (band == 0) return r;
    size_t n = 0; const Station* t = stations(n);
    struct Sc { const Station* s; double d, sc, med; bool named, speaks, exact; };
    std::vector<Sc> c;
    const bool here = std::isfinite(lat) && std::isfinite(lon);
    const double reach = band > 1000 ? kReachHfKm : kReachMfKm;
    const std::string tl = letters(text);
    const auto pos = sanePositions(positions(text), b1, band, sid);
    const auto langs = languages(text);
    for (size_t i = 0; i < n; i++) {
        if (t[i].b1 != b1 || t[i].khz != band) continue;
        Sc x{ &t[i], here ? km(lat, lon, t[i].lat, t[i].lon) : NAN, 1, INFINITY, false, false, false };
        x.sc = here ? 1.0 / (1.0 + std::pow(x.d / kPriorKm, 2)) : 1.0;
        const std::string nl = letters(t[i].name);
        x.exact = false; for (const auto& k : nameKeys(&t[i])) if (k.size() >= 5 && tl.find(k) != std::string::npos) x.exact = true;   // ★ + aliases
        x.named = x.exact || (sid && nl == sid->L);   // ★ + the fuzzy self-ID (known names only)
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
    const bool contradicted = !posFit && (!pos.empty() || (!langs.empty() && !speaks));
    if (sid && !named) {
        bool isCand = false;
        for (auto& x : c) if (letters(x.s->name) == sid->L) isCand = true;
        if (!isCand && contradicted) { r.textName = sid->row->name; r.textIso = sid->row->iso; r.textCountry = sid->row->country; return r; }
    }
    // ★ an unknown "<NAME> RADIO" and the text rules every candidate out: the letter only — the name is never shown
    if (!sid && !named && contradicted && !textName(text).first.empty()) return r;
    if (!pos.empty() && !posFit && !named) return r;
    const bool evidence = named || posFit || speaks;
    const Sc& best = c[0];
    if (!evidence && (!here || !(best.d <= reach))) return r;
    f.named = best.named; f.fuzzy = best.named && !best.exact && !(sid && sid->exact); f.posWin = best.med <= kPosFitKm; f.priorOnly = !evidence;
    if (c.size() == 1 || best.sc >= kClear * c[1].sc) { r.stations = { best.s }; r.km = { best.d }; r.dx = here && best.d > reach; return r; }
    r.stations = { best.s, c[1].s }; r.km = { best.d, c[1].d };
    return r;
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

${CPP_POST}
/** navtexLabelOf(): "E · Mondolfo, Italy 🇮🇹" (confident), "E · Suspected Mondolfo, Italy 🇮🇹" (likely — on the STATION's
 *  letter when the header's looked corrupted), "W · Corsen 🇫🇷 or Valentia 🇮🇪?", or "Station B". */
inline std::string labelOf(char b1, const Resolution& r) {
    if (!r.textName.empty())
        return std::string(1, b1) + " \\xc2\\xb7 Suspected " + r.textName + (r.textCountry.empty() ? std::string() : ", " + r.textCountry + " " + flag(r.textIso.c_str()));
    if (r.stations.empty()) return std::string("Station ") + (b1 >= 'A' && b1 <= 'Z' ? std::string(1, b1) : "?");
    const Station* a = r.stations[0];
    if (r.stations.size() == 2) {
        const Station* b = r.stations[1];
        return std::string(1, a->b1) + " \\xc2\\xb7 " + a->name + " " + flag(a->iso) + " or " + b->name + " " + flag(b->iso) + "?";
    }
    return std::string(1, a->b1) + " \\xc2\\xb7 " + (r.conf == "likely" ? "Suspected " : "") + a->name + ", " + a->country + " " + flag(a->iso)
         + (r.dx ? " \\xc2\\xb7 DX " + kmText(r.km[0]) + " km" : std::string());
}
/** navtexStationLabel(). utc = when the message started (UTC s); hdr = its header ("JA12"). */
inline std::string label(char b1, double dialHz, double lat, double lon, const std::string& text = "", double utc = NAN, const std::string& hdr = "") {
    return labelOf(b1, resolveAll(b1, dialHz, lat, lon, text, utc, hdr));
}
/** navtexStartLostLabel(): "N · Suspected Piombino, Italy 🇮🇹" for a start-lost message, or "" (say nothing). */
inline std::string startLostLabel(double dialHz, double lat, double lon, const std::string& text, double utc = NAN) {
    const Resolution r = startLostResolve(dialHz, lat, lon, text, utc);
    if (r.stations.empty() && r.textName.empty()) return std::string();
    const std::string h = headerFragment(text);
    return labelOf(!r.stations.empty() ? r.stations[0]->b1 : !h.empty() ? h[0] : '?', r);
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
