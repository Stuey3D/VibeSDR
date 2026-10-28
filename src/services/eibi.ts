/**
 * eibi.ts — local EiBi shortwave-schedule bookmarks (parity with the network
 * servers, which augment /api/bookmarks with the *currently active* EiBi set).
 *
 * Local SDR has no server doing this, so we fetch the EiBi season schedule
 * (eibispace.de, plain CSV) once a day, cache it, and time-filter client-side:
 * a station only appears while it's scheduled to broadcast (current UTC time +
 * weekday inside its entry's window) and drops off when it's done — exactly how
 * the server feed behaves.
 *
 * CSV columns (semicolon-separated):
 *   kHz ; Time(UTC) ; Days ; ITU ; Station ; Lng ; Target ; Remarks ; P ; Start ; Stop
 */

import AsyncStorage from '@react-native-async-storage/async-storage';
import type { ServerBookmark } from './stations';
import {
  type EibiState, type FetchOutcome, seasonFor, previousSeason, seasonUrl, shouldFetch,
  refreshWithFallback, noteFor, utcDate,
} from './eibiSeason';

interface EibiEntry {
  freqHz: number;
  time:   string;   // "HHMM-HHMM"
  days:   string;   // EiBi day code ("", "12345", "1-5", "Mo-Fr", "Sa", …)
  station: string;
  lang:   string;
  itu:    string;   // transmitter-country code (EiBi ITU code, col 4)
}

// EiBi CSV files are Windows-1252 (Latin-1 superset) — decoding them as UTF-8
// turns accented station names into the "�" replacement char. Map each byte:
// 0x00–0x7F and 0xA0–0xFF straight to that code point (Latin-1); the 0x80–0x9F
// band uses the cp1252 punctuation table. (Matches UberSDR's eibi.go fix.)
const CP1252_HI: Record<number, number> = {
  0x80: 0x20AC, 0x82: 0x201A, 0x83: 0x0192, 0x84: 0x201E, 0x85: 0x2026,
  0x86: 0x2020, 0x87: 0x2021, 0x88: 0x02C6, 0x89: 0x2030, 0x8A: 0x0160,
  0x8B: 0x2039, 0x8C: 0x0152, 0x8E: 0x017D, 0x91: 0x2018, 0x92: 0x2019,
  0x93: 0x201C, 0x94: 0x201D, 0x95: 0x2022, 0x96: 0x2013, 0x97: 0x2014,
  0x98: 0x02DC, 0x99: 0x2122, 0x9A: 0x0161, 0x9B: 0x203A, 0x9C: 0x0153,
  0x9E: 0x017E, 0x9F: 0x0178,
};
function decodeWin1252(bytes: Uint8Array): string {
  let out = '';
  for (let i = 0; i < bytes.length; i++) {
    const b = bytes[i];
    out += String.fromCharCode((b >= 0x80 && b <= 0x9F) ? (CP1252_HI[b] ?? b) : b);
  }
  return out;
}

let _entries: EibiEntry[] | null = null;
// ★ What is held and what EiBi last said "not yet" to — see eibiSeason.ts. The season choice and
//   its fallback live there (pure, tested); this file does the fetching and the storage.
const _state: EibiState = { season: '', date: '', triedSeason: '', triedAt: 0 };
let _loadedFromStore = false;
let _error = '';

function parse(csv: string): EibiEntry[] {
  const out: EibiEntry[] = [];
  for (const line of csv.split(/\r?\n/)) {
    if (!line) continue;
    const f = line.split(';');
    const khz = parseFloat(f[0]);
    if (!isFinite(khz) || khz <= 0) continue;   // skips the header row too
    const station = (f[4] || '').trim();
    if (!station) continue;
    out.push({
      freqHz: Math.round(khz * 1000),
      time:  (f[1] || '').trim(),
      days:  (f[2] || '').trim(),
      station,
      lang:  (f[5] || '').trim(),
      itu:   (f[3] || '').trim(),
    });
  }
  return out;
}

function timeActive(t: string, nowMin: number): boolean {
  const m = t.match(/^(\d{2})(\d{2})-(\d{2})(\d{2})$/);
  if (!m) return true;                       // unknown format → don't hide
  const s = (+m[1]) * 60 + (+m[2]);
  const e = (+m[3]) * 60 + (+m[4]);          // 2400 → 1440
  if (s === e) return true;                  // 0000-0000 = continuous
  return s < e ? (nowMin >= s && nowMin < e) // normal window
               : (nowMin >= s || nowMin < e); // wraps past midnight
}

const DAYCODE = ['', 'Mo', 'Tu', 'We', 'Th', 'Fr', 'Sa', 'Su'];
const DAYNUM: Record<string, number> = { Mo: 1, Tu: 2, We: 3, Th: 4, Fr: 5, Sa: 6, Su: 7 };

function dayActive(days: string, eibiDay: number): boolean {
  const d = days.trim();
  if (!d) return true;                                   // daily
  if (/^[1-7]+$/.test(d)) return d.includes(String(eibiDay));   // "12345"
  let m = d.match(/^([1-7])-([1-7])$/);                  // "1-5"
  if (m) { const a = +m[1], b = +m[2]; return a <= b ? (eibiDay >= a && eibiDay <= b) : (eibiDay >= a || eibiDay <= b); }
  const code = DAYCODE[eibiDay];
  m = d.match(/^(Mo|Tu|We|Th|Fr|Sa|Su)-(Mo|Tu|We|Th|Fr|Sa|Su)$/);  // "Mo-Fr"
  if (m) { const a = DAYNUM[m[1]], b = DAYNUM[m[2]]; return a <= b ? (eibiDay >= a && eibiDay <= b) : (eibiDay >= a || eibiDay <= b); }
  if (/^(Mo|Tu|We|Th|Fr|Sa|Su)(,(Mo|Tu|We|Th|Fr|Sa|Su))*$/.test(d)) return d.includes(code);
  return true;   // dates / "irr" / "alt" / Ramadan etc. → don't hide
}

// EiBi transmitter-country (ITU) code → ISO 3166-1 alpha-2, for a flag. EiBi uses
// its own ITU-style codes (col 4); the common broadcasters are covered, the rest
// fall through to no flag.
const EIBI_ITU2ISO: Record<string, string> = {
  AFG:'AF', AFS:'ZA', AGL:'AO', ALB:'AL', ALG:'DZ', AND:'AD', ARG:'AR', ARM:'AM',
  ARS:'SA', ASC:'SH', AUS:'AU', AUT:'AT', AZE:'AZ', B:'BR', BAH:'BS', BEL:'BE',
  BEN:'BJ', BFA:'BF', BGD:'BD', BHR:'BH', BLR:'BY', BOL:'BO', BOT:'BW', BRU:'BN',
  BUL:'BG', BUR:'MM', CAN:'CA', CBG:'KH', CHL:'CL', CHN:'CN', CKH:'CK', CLM:'CO',
  CLN:'LK', CME:'CM', COD:'CD', COG:'CG', CTI:'CI', CTR:'CR', CUB:'CU', CVA:'VA',
  CYP:'CY', CZE:'CZ', D:'DE', DJI:'DJ', DNK:'DK', DOM:'DO', E:'ES', EGY:'EG',
  EQA:'EC', ERI:'ER', EST:'EE', ETH:'ET', F:'FR', FIN:'FI', FJI:'FJ', G:'GB',
  GAB:'GA', GEO:'GE', GHA:'GH', GNB:'GW', GRC:'GR', GRL:'GL', GTM:'GT', GUF:'GF',
  GUI:'GN', GUM:'GU', GUY:'GY', HKG:'HK', HND:'HN', HNG:'HU', HOL:'NL', HRV:'HR',
  HWA:'US', I:'IT', ICE:'IS', IND:'IN', INS:'ID', IRL:'IE', IRN:'IR', IRQ:'IQ',
  ISR:'IL', J:'JP', JOR:'JO', KAZ:'KZ', KEN:'KE', KGZ:'KG', KOR:'KR', KRE:'KP',
  KWT:'KW', LAO:'LA', LBN:'LB', LBR:'LR', LBY:'LY', LTU:'LT', LUX:'LU', LVA:'LV',
  MCO:'MC', MDA:'MD', MDG:'MG', MEX:'MX', MLA:'MY', MLD:'MV', MLI:'ML', MLT:'MT',
  MNG:'MN', MOZ:'MZ', MRC:'MA', MRA:'MP', MTN:'MR', MWI:'MW', MYA:'MM', NCG:'NI',
  NGR:'NG', NIG:'NE', NMB:'NA', NOR:'NO', NPL:'NP', NZL:'NZ', OMA:'OM', PAK:'PK',
  PHL:'PH', PNG:'PG', PNR:'PA', POL:'PL', POR:'PT', PRG:'PY', PRU:'PE', QAT:'QA',
  ROU:'RO', RRW:'RW', RUS:'RU', SDN:'SD', SEN:'SN', SEY:'SC', SNG:'SG', SOM:'SO',
  SRB:'RS', SRL:'SL', SUI:'CH', SVK:'SK', SVN:'SI', SWZ:'SZ', SYR:'SY', TCD:'TD',
  THA:'TH', TJK:'TJ', TKM:'TM', TUN:'TN', TUR:'TR', TWN:'TW', TZA:'TZ', UAE:'AE',
  UGA:'UG', UKR:'UA', URG:'UY', USA:'US', UZB:'UZ', VEN:'VE', VTN:'VN', YEM:'YE',
  ZMB:'ZM', ZWE:'ZW', S:'SE', SVK_:'SK',
};
function ituToFlag(itu: string): string | undefined {
  const iso = EIBI_ITU2ISO[itu];
  if (!iso || !/^[A-Z]{2}$/.test(iso)) return undefined;
  return String.fromCodePoint(127397 + iso.charCodeAt(0), 127397 + iso.charCodeAt(1));
}

function activeNow(entries: EibiEntry[]): ServerBookmark[] {
  const now = new Date();
  const nowMin = now.getUTCHours() * 60 + now.getUTCMinutes();
  const eibiDay = now.getUTCDay() === 0 ? 7 : now.getUTCDay();
  const out: ServerBookmark[] = [];
  for (const e of entries) {
    if (!timeActive(e.time, nowMin)) continue;
    if (!dayActive(e.days, eibiDay)) continue;
    out.push({ name: e.station, frequency: e.freqHz, mode: 'am', group: 'EiBi', comment: e.lang || undefined, flag: ituToFlag(e.itu), itu: e.itu || undefined, source: 'eibi' });
  }
  return out;
}

/** One season's file. ★ Plain http on purpose for now: https://www.eibispace.de serves an EXPIRED
 *  certificate (checked 2026-10-03) — same note as vibeserver/eibi.cpp. */
async function fetchSeason(code: string): Promise<FetchOutcome> {
  let res: Response;
  try {
    res = await fetch(seasonUrl(code));
  } catch (e: any) {
    return { kind: 'failed', message: e?.message ?? 'no connection' };
  }
  if (res.status === 404 || res.status === 410) return { kind: 'unpublished' };
  if (!res.ok) return { kind: 'failed', message: `HTTP ${res.status}` };
  const csv = decodeWin1252(new Uint8Array(await res.arrayBuffer()));
  const count = parse(csv).length;
  // ★ A page with no entries is not a schedule — treat it as not published, never as the list.
  return count > 0 ? { kind: 'ok', csv, count } : { kind: 'unpublished' };
}

// ★ One load at a time: the radio screen, the server screen and the server feed all ask, and a
//   season fallback is two requests — three callers at once would be six.
let _inFlight: Promise<void> | null = null;
function ensureLoaded(): Promise<void> {
  if (!_inFlight) _inFlight = ensureLoadedOnce().finally(() => { _inFlight = null; });
  return _inFlight;
}

async function ensureLoadedOnce(): Promise<void> {
  const now = new Date();
  const cur = seasonFor(now);

  // Warm from cache first (instant, even if stale) so stations show offline.
  // ★★ This season's file OR the previous one: the previous IS the schedule while EiBi has not
  //    published the new season yet (B26 was a 404 on 2026-10-27). Accepting only the current
  //    season left the app with no schedule at all on every season change.
  if (!_loadedFromStore) {
    _loadedFromStore = true;
    try {
      const [cs, csv, cd, tried] = await Promise.all([
        AsyncStorage.getItem('lsv_eibi_season2'),
        AsyncStorage.getItem('lsv_eibi_csv2'),
        AsyncStorage.getItem('lsv_eibi_date2'),
        AsyncStorage.getItem('lsv_eibi_tried2'),
      ]);
      if (csv && cs && (cs === cur || cs === previousSeason(cur))) {
        _entries = parse(csv); _state.season = cs; _state.date = cd || '';
      }
      if (tried) {
        const t = JSON.parse(tried);
        if (typeof t?.season === 'string' && typeof t?.at === 'number') {
          _state.triedSeason = t.season; _state.triedAt = t.at;
        }
      }
    } catch {}
  }

  // Once a day; on a season change; and a season EiBi has not published every few hours.
  if (_entries && !shouldFetch(_state, now)) return;
  const r = await refreshWithFallback(_state, now, fetchSeason);
  if (r.kind === 'failed') { _error = r.error; return; }   // keep whatever cache we have
  _error = '';
  if (r.kind === 'ok') {
    _entries = parse(r.csv); _state.season = r.season; _state.date = utcDate(now);
    AsyncStorage.setItem('lsv_eibi_csv2', r.csv).catch(() => {});
    AsyncStorage.setItem('lsv_eibi_season2', r.season).catch(() => {});
    AsyncStorage.setItem('lsv_eibi_date2', _state.date).catch(() => {});
  }
  AsyncStorage.setItem('lsv_eibi_tried2',
    JSON.stringify({ season: _state.triedSeason, at: _state.triedAt })).catch(() => {});
}

/** A plain sentence about the schedule in use, for the server screen: the note when EiBi has not
 *  published this season yet ("EiBi hasn't published the B26 (winter) schedule yet — using A26
 *  until it appears."), or the last real failure. '' when all is well. */
export function eibiStatusNote(): string {
  return noteFor(_state, new Date()) || _error;
}

/** Currently-active EiBi entries as bookmarks (time + weekday filtered). */
export async function loadActiveEibi(): Promise<ServerBookmark[]> {
  await ensureLoaded();
  return _entries ? activeNow(_entries) : [];
}

/* ★★ EiBi language codes → the ISO codes the on-device transcriber uses (EiBi README §language codes; checked
 *  2026-10-15). Only languages the phones can transcribe are listed — the rest are no help to TRANSCRIBE. */
const EIBI_LANG_ISO: Record<string, string> = {
  A: 'ar', C: 'zh', CA: 'yue', D: 'de', E: 'en', F: 'fr', HI: 'hi', I: 'it', IN: 'id', J: 'ja', K: 'ko',
  M: 'zh', NL: 'nl', P: 'pt', PO: 'pl', R: 'ru', S: 'es', T: 'th', TU: 'tr', UK: 'uk', VN: 'vi',
  // ★ Added 2026-10-15 — CRI's Czech hour on 7415 (CZ) gave AUTO no hint at all. A code the phone cannot transcribe
  //   is dropped by the caller (`supported`), so listing it costs nothing.
  CZ: 'cs', SK: 'sk', HU: 'hu', RO: 'ro', GR: 'el', SWE: 'sv', DA: 'da', NO: 'nb', FI: 'fi', HB: 'he', HR: 'hr',
  SR: 'sr', BU: 'bg', CT: 'ca', FS: 'fa', ML: 'ms', UR: 'ur', BE: 'bn', TAM: 'ta', SWA: 'sw', LT: 'lt', LV: 'lv',
  ES: 'et', AL: 'sq', BY: 'be', KZ: 'kk', AZ: 'az', GE: 'ka', IS: 'is', MA: 'mt',
};

/** ★ TRANSCRIBE's AUTO hint (Stuart, 2026-10-15: "if i tune to china radio chinese needs to be a language to test").
 *  What EiBi says is on THIS frequency NOW goes first in AUTO's order — on iOS it orders the shortlist on a slow phone
 *  and breaks ties, never decides (EiBi was wrong for 3 of the 2026-10-17 bake-off's frequencies). Within 2.5 kHz
 *  (half a SW channel), on air now; nearest first. Returns ISO codes (no repeats) and the station EiBi names. */
export async function eibiLanguagesAt(freqHz: number): Promise<{ langs: string[]; station: string }> {
  await ensureLoaded();
  if (!_entries || !(freqHz > 0)) return { langs: [], station: '' };
  const near = activeNow(_entries)
    .filter(b => Math.abs(b.frequency - freqHz) <= 2500)
    .sort((a, b) => Math.abs(a.frequency - freqHz) - Math.abs(b.frequency - freqHz));
  const langs: string[] = [];
  for (const b of near) {
    for (const c of (b.comment ?? '').toUpperCase().split(/[,/ ]+/)) {
      const iso = EIBI_LANG_ISO[c.trim()];
      if (iso && !langs.includes(iso)) langs.push(iso);
    }
  }
  return { langs, station: near[0]?.name ?? '' };
}
