/**
 * knownSignals.ts — KNOWN-SIGNAL LABELS on the spectrum: which DECODER a signal in view wants. ONE pure module, shared by
 * the app (WaterfallView.tsx — the JS path and the Android engine alike, as the labels are an overlay above both) and the
 * web client (web/client/src/waterfall.ts). Tested by scripts/test_known_signals.ts.
 *
 * ★★★ STUART'S BRIEF (2026-10-30): "we dont try any smart identification for now, just use EiBi and what we know about
 *     known signals so 4582 for DDK 4608.1 for Northwood labelled as RTTY WEFAX etc, this is more for a user to know
 *     what decoder to use, the VTS shows the bookmarks". So: a STATIC list — never a guess from the signal itself.
 *      • OURS (KNOWN_SIGNALS): well-known utility signals and the decoder each one wants, typed in with its source.
 *      • EiBi's (eibiKnownSignals): the schedule's utility entries ON AIR NOW, classed by EiBi's own codes — the
 *        language column's "-HF" (HFDL), "-TY" (teletype), "-CW" (Morse) — or a mode the station's name says
 *        ("Meteo Fax", "RTTY", "Volmet", "NAVTEX"). Broadcast entries are never labels.
 *  ★★ EiBi's BROADCAST rows are never labels — they would crowd the spectrum, and the VTS station strip shows them.
 *     But the user's own bookmarks, the server owner's saved ones and FM names this receiver LEARNT from RDS are
 *     (bookmarkSignals — Stuart's follow-ups the same day).
 *
 * ★★★ THE MARKER SITS ON THE SIGNAL, THE DIAL IS WHERE YOU TUNE. A published utility frequency is the ASSIGNED (carrier
 *     / centre) frequency — where the signal is SEEN on the spectrum — so that is where the line goes (`hz`). The DIAL
 *     a USB receiver uses is below it by the decoder's audio centre (`dialHz`, DecoderClient's params): RTTY 1000 Hz
 *     (DWD 4583 → 4582 USB, Stuart's number), WEFAX 1900 Hz (Northwood 4610 → 4608.1, Stuart's number), NAVTEX 500 Hz,
 *     DSC 1700 Hz (dscChannels.ts dscTuneFor). The amateur digital modes are the other way round: what is published is
 *     the DIAL, and the signals sit 0–3 kHz ABOVE it — the marker is at the dial with the span above drawn as a bar.
 *
 * ★ PLACEMENT (labelLayout): only labels inside the view; when two would overlap the higher PRIORITY wins, then the
 *   one nearer the dial; a loser drops to the second row, and a loser there is not drawn. At most LABEL_MAX on screen.
 *   The rows are the two the boundary labels use (LISTEN, RF CENTRE): the caller passes their boxes as `blocked`.
 * ★ COST: the visible set and the layout are recomputed only when the view, the dial or the list changes (the callers
 *   key a cache on those); nothing at all is done while the toggle is off or the view holds none.
 */
import { DSC_ALL, dscTuneFor } from './dscChannels.ts';
import { NAVTEX_BANDS_KHZ } from './navtexStations.generated.ts';
import { RTTY_WEATHER } from './rttySpec.ts';

/** Decoder ids, as the decoder panel names them (decoderBands.ts DECODER_PLACES). '' = no decoder (a voice service). */
export type KnownDecoder = 'rtty' | 'wefax' | 'navtex' | 'dsc' | 'spots' | 'time' | 'hfdl' | 'cw' | '';

/** ★★ A DECODER PRESET (Stuart, 2026-10-30: "can they be tapped to tune and chose the decoder?"). The settings the signal
 *  itself was PUBLISHED with, applied for this session when its label is tapped — never written over the user's own saved
 *  choice (the callers' preset path, see SDRScreen onKnownTap / main.ts knownTap). Only where the signal fixes them:
 *  NAVTEX and DSC have nothing to choose (DSC's tones come from the dial — dscParams), and a bookmark or an EiBi row
 *  carries none (it keeps whatever the user has set). */
export interface KnownPreset {
  /** RTTY: shift, baud, alphabet, polarity — DWD's are rttySpec.ts RTTY_WEATHER (the menu's WEATHER key). */
  rtty?: { shift: number; baud: number; encoding: 'ITA2' | 'ASCII' | 'CCIR476'; inverted: boolean };
  /** WEFAX: lines a minute and the index of co-operation (IOC 576 = the decoders' 1809-pixel line, the only one built). */
  wefaxLpm?: 60 | 120 | 240; wefaxIoc?: 576;
  /** TIME: the stations that share this frequency (60 kHz is MSF AND WWVB — where the receiver is decides). */
  time?: readonly string[];
  /** Digital spots: the mode published on this dial. */
  spots?: 'FT8' | 'FT4' | 'WSPR' | 'JS8';
}

export interface KnownSignal {
  /** Where the marker goes: the signal's centre / carrier as seen on the spectrum (Hz). */
  hz: number;
  /** Optional occupied span drawn as a bar (Hz, absolute) — the digital modes' 0–3 kHz above the dial. */
  lo?: number; hi?: number;
  /** The label: "RTTY · DWD", "WEFAX · Northwood", "FT8", "NAVTEX", "DSC". */
  label: string;
  /** The decoder it wants ('' = none — VOLMET is speech, CHU has no decoder here). */
  dec: KnownDecoder;
  /** Where to tune for it, and in which mode. */
  dialHz: number; mode: string;
  /** Higher wins a collision (PRIO_*). */
  prio: number;
  src: 'table' | 'eibi' | 'personal' | 'server' | 'learnt';
  /** The decoder's settings for this signal, where they are published (curated entries only). */
  preset?: KnownPreset;
}

/** ★★ Collision priority (Stuart, 2026-10-30): safety > curated decoder labels > personal bookmarks > bookmarks saved to
 *  the server > learnt FM RDS names > EiBi's data rows. DSC's routine calling channels are curated, but a dozen sit in
 *  30 kHz, so they come last among the curated — still above every bookmark. */
export const PRIO_SAFETY = 7;   // DSC distress, NAVTEX
export const PRIO_TABLE = 6;    // the curated table
export const PRIO_ROUTINE = 5;  // DSC routine calling
export const PRIO_PERSONAL = 4; // the user's own bookmarks
export const PRIO_SERVER = 3;   // saved to the server by its owner
export const PRIO_LEARNT = 2;   // an FM station's name this receiver learnt from RDS
export const PRIO_EIBI = 1;

/** The most labels drawn at once — a hard cap, whatever is in view (Pi-2-class browsers, 1 GB phones). */
export const LABEL_MAX = 12;
/** Rows of labels (the two boundary rows). */
export const LABEL_ROWS = 2;
/** Clear space between two labels on one row (px, caller's units). */
export const LABEL_GAP = 6;

/** Audio centres the decoders listen at in USB (DecoderClient's extension params). */
const RTTY_AF = 1000, WEFAX_AF = 1900, NAVTEX_AF = 500;
/** HFDL: a ground station's listed frequency is its USB dial; the signal is centred 1440 Hz above it. */
const HFDL_AF = 1440;
/** A USB voice channel's speech sits 300–2700 Hz above the dial; its marker goes at the middle. */
const VOICE_AF = 1500;

const kHz = (k: number) => Math.round(k * 1000);

function rtty(khz: number, who: string): KnownSignal {
  const hz = kHz(khz);
  return { hz, label: `RTTY · ${who}`, dec: 'rtty', dialHz: hz - RTTY_AF, mode: 'usb', prio: PRIO_TABLE, src: 'table',
           preset: { rtty: { ...RTTY_WEATHER } } };
}
function wefax(khz: number, who: string): KnownSignal {
  const hz = kHz(khz);
  return { hz, label: `WEFAX · ${who}`, dec: 'wefax', dialHz: hz - WEFAX_AF, mode: 'usb', prio: PRIO_TABLE, src: 'table',
           preset: { wefaxLpm: 120, wefaxIoc: 576 } };
}
function volmet(khz: number, who: string): KnownSignal {
  const d = kHz(khz);
  return { hz: d + VOICE_AF, lo: d + 300, hi: d + 2700, label: `VOLMET · ${who}`, dec: '', dialHz: d, mode: 'usb',
           prio: PRIO_TABLE, src: 'table' };
}
function time(hz: number, who: string, mode: KnownSignal['mode'] = 'am', dec: KnownDecoder = 'time', stations: readonly string[] = []): KnownSignal {
  const s: KnownSignal = { hz, label: dec ? `TIME · ${who}` : `${who} · time`, dec, dialHz: hz, mode, prio: PRIO_TABLE, src: 'table' };
  if (dec && stations.length) s.preset = { time: stations };
  return s;
}
function digi(mode: 'FT8' | 'FT4' | 'WSPR' | 'JS8', dial: number, top = 3000, bottom = 0): KnownSignal {
  return { hz: dial, lo: dial + bottom, hi: dial + top, label: mode, dec: 'spots', dialHz: dial, mode: 'usb',
           prio: PRIO_TABLE, src: 'table', preset: { spots: mode } };
}

/**
 * ★★ THE CURATED TABLE. Every entry with its source:
 *  • DWD (Deutscher Wetterdienst, Pinneberg) RTTY, 50 Bd / 450 Hz shift: DDH47 147.3 · DDK2 4583 · DDH7 7646 ·
 *    DDK9 10100.8 · DDH9 11039 · DDH8 14467.3 kHz — DWD's published broadcast schedule ("Sendeplan"), the same six in
 *    every compilation checked 2026-10-30 (hfunderground wiki "RTTY maritime weather transmissions", littlecamels.com
 *    bandplan); EiBi A26 lists 147.3 as "DWD 2 Pinneberg RTTY". Stuart decodes 4582 USB = 4583 − 1 kHz.
 *  • DWD WEFAX (DDH3 3855 · DDK3 7880 · DDK6 13882.5 kHz) and the Met Office's Northwood WEFAX (GYA 2618.5 · 4610 ·
 *    8040 · 11086.5 kHz) — the carriers decoderBands.ts WEFAX_KHZ and wefaxAlign.ts already use; EiBi A26 lists
 *    2618.5 as "GYA Northwood Meteo Fax". Stuart tunes 4608.1 USB = 4610 − 1.9 kHz.
 *  • NAVTEX 424 / 490 / 518 / 4209.5 kHz — NGA Pub. 117 via navtexStations.generated.ts.
 *  • DSC — dscChannels.ts (ITU RR Appendix 15 distress, ITU-R M.541 routine), its own tune rule.
 *  • FT8 / FT4 / WSPR / JS8 dials — the published dials the server's Band mode uses (android/app/src/main/cpp/
 *    decoders/digi_bands.h publishedDials, which records where each came from); scripts/test_known_signals.ts reads
 *    that header and fails if the two disagree. WSPR's signals sit 1400–1600 Hz above its dial.
 *  • Shannon VOLMET 3413 · 5505 · 8957 · 13264 kHz USB (Irish Aviation Authority; EiBi A26 "Shannon Volmet" 3413).
 *    Speech — no decoder; the label says USB.
 *  • Time signals: MSF / WWVB 60 kHz, DCF77 77.5 kHz, RBU 66.666 kHz, RWM 4996 / 9996 / 14996 kHz, WWV(H) 2.5 / 5 /
 *    10 / 15 MHz, WWV 20 / 25 MHz — the stations DecoderClient's timeStationFor decodes (decoderBands.ts
 *    TIME_SIGNAL_HZ). CHU (Ottawa) 3330 / 7850 / 14670 kHz is labelled as a time station WITHOUT a decoder — the
 *    TIME decoder does not read CHU's format, and a "TIME" label there would send people to a decoder that stays silent.
 */
const DWD_RTTY = [147.3, 4583, 7646, 10100.8, 11039, 14467.3];
const DWD_FAX = [3855, 7880, 13882.5];
const NORTHWOOD_FAX = [2618.5, 4610, 8040, 11086.5];
const SHANNON_VOLMET = [3413, 5505, 8957, 13264];
/** digi_bands.h publishedDials — FT8, FT4, WSPR, JS8 (Hz). Kept in step by scripts/test_known_signals.ts. */
export const DIGI_DIALS: Readonly<Record<'FT8' | 'FT4' | 'WSPR' | 'JS8', readonly number[]>> = {
  FT8: [1840000, 3573000, 5357000, 7074000, 10136000, 14074000, 18100000, 21074000, 24915000, 28074000, 50313000,
        70154000, 144174000, 432174000],
  FT4: [3575000, 7047500, 10140000, 14080000, 18104000, 21140000, 24919000, 28180000, 50318000, 144170000],
  WSPR: [136000, 474200, 1836600, 3568600, 5364700, 7038600, 10138700, 14095600, 18104600, 21094600, 24924600,
         28124600, 70091000, 144490500],
  JS8: [1842000, 3578000, 7078000, 10130000, 14078000, 18104000, 21078000, 24922000, 28078000, 50318000],
};

function buildTable(): KnownSignal[] {
  const t: KnownSignal[] = [];
  for (const k of DWD_RTTY) t.push(rtty(k, 'DWD'));
  for (const k of DWD_FAX) t.push(wefax(k, 'DWD'));
  for (const k of NORTHWOOD_FAX) t.push(wefax(k, 'Northwood'));
  for (const k of NAVTEX_BANDS_KHZ) {
    const hz = kHz(k);
    t.push({ hz, label: 'NAVTEX', dec: 'navtex', dialHz: hz - NAVTEX_AF, mode: 'usb', prio: PRIO_SAFETY, src: 'table' });
  }
  for (const c of DSC_ALL) {
    const tune = dscTuneFor(c);
    t.push({ hz: c.hz, label: c.kind === 'distress' ? 'DSC · distress' : 'DSC', dec: 'dsc', dialHz: tune.hz, mode: tune.mode,
             prio: c.kind === 'distress' ? PRIO_SAFETY : PRIO_ROUTINE, src: 'table' });
  }
  for (const d of DIGI_DIALS.FT8) t.push(digi('FT8', d));
  for (const d of DIGI_DIALS.FT4) t.push(digi('FT4', d));
  for (const d of DIGI_DIALS.WSPR) { const s = digi('WSPR', d, 1600, 1400); s.hz = d + 1500; t.push(s); }
  for (const d of DIGI_DIALS.JS8) t.push(digi('JS8', d));
  for (const k of SHANNON_VOLMET) t.push(volmet(k, 'Shannon'));
  // ★ the stations each one is decoded as — DecoderClient timeStationFor's own map (RBU's 66.666 kHz reads as 'rwm')
  // ★ CW = 'cwu' on the wire (sdrTypes.ts SDRMode has no plain 'cw'): the carrier keyed in amplitude becomes a beat note
  t.push(time(40_000, 'JJY', 'cwu', 'time', ['jjy']),
         time(60_000, 'MSF/WWVB/JJY', 'cwu', 'time', ['msf', 'wwvb', 'jjy']), time(77_500, 'DCF77', 'cwu', 'time', ['dcf77']),
         time(66_666, 'RBU', 'cwu', 'time', ['rwm']));
  for (const f of [4_996_000, 9_996_000, 14_996_000]) t.push(time(f, 'RWM', 'cwu', 'time', ['rwm']));
  for (const f of [2_500_000, 5_000_000, 10_000_000, 15_000_000]) t.push(time(f, 'WWV/WWVH', 'am', 'time', ['wwv']));
  for (const f of [20_000_000, 25_000_000]) t.push(time(f, 'WWV', 'am', 'time', ['wwv']));
  for (const k of [3330, 7850, 14670]) t.push(time(kHz(k), 'CHU', 'am', ''));
  return t.sort((a, b) => a.hz - b.hz);
}

/** The curated table, lowest first. */
export const KNOWN_SIGNALS: readonly KnownSignal[] = buildTable();

// ── EiBi ────────────────────────────────────────────────────────────────────────────────────────────────────────────

/** An EiBi row as either client holds it: the app's local schedule (comment = the language column) or a server's
 *  GET /stations (comment = "HHMM-HHMM · language"). */
export interface EibiRow { name: string; frequency: number; comment?: string; source?: string }

/** True when "HHMM-HHMM" covers `nowMin` (minutes past 00:00 UTC). No time = on air (the app's list is already
 *  filtered). Days are not in a server's comment, so a weekday-only entry shows every day — a hint, not a schedule. */
export function eibiOnAir(comment: string | undefined, nowMin: number): boolean {
  const m = /(\d{2})(\d{2})-(\d{2})(\d{2})/.exec(comment || '');
  if (!m) return true;
  const s = +m[1] * 60 + +m[2], e = +m[3] * 60 + +m[4];
  if (s === e) return true;
  return s < e ? nowMin >= s && nowMin < e : nowMin >= s || nowMin < e;
}

/** A short place for a label from an EiBi station name: "GYA Northwood Meteo Fax" → "Northwood", "Shannon HFDL (ID7)"
 *  → "Shannon", "NOJ Fax Kodiak" → "Kodiak", "Tokyo Volmet x10,x40" → "Tokyo". At most 14 characters. */
export function eibiPlace(name: string): string {
  let s = String(name || '')
    .replace(/\([^)]*\)/g, ' ')
    .replace(/\b(meteo|met|wx)?\s*fax\b|\bhfdl\b|\bvolmet\b|\brtty\b|\bnavtex\b|\bradio\b|\btime signal\b/gi, ' ')
    .replace(/\bx\d{2}\b,?\s*(x?\d{2}\b)?/gi, ' ')
    .replace(/[,;]/g, ' ');
  const words = s.split(/\s+/).filter(Boolean);
  // ★ a leading call sign (GYA, NOJ, DWD, VFF) and lone numbers say nothing a listener needs
  if (words.length > 1 && /^[A-Z0-9]{3,6}$/.test(words[0]) && /[A-Z]/.test(words[0])) words.shift();
  s = words.filter(w => !/^\d+$/.test(w)).join(' ');
  return s.length > 14 ? s.slice(0, 13).trimEnd() + '…' : s;
}

/** Class an EiBi row, or null for a broadcast (no label). */
export function eibiClass(row: EibiRow): { dec: KnownDecoder; kind: string; af: number; voice?: boolean } | null {
  const codes = (row.comment || '').toUpperCase().split(/[\s,·/;]+/);
  const name = row.name || '';
  if (codes.includes('-HF') || /\bHFDL\b/i.test(name)) return { dec: 'hfdl', kind: 'HFDL', af: HFDL_AF };
  if (/\b(meteo|met|wx)?\s*fax\b/i.test(name)) return { dec: 'wefax', kind: 'WEFAX', af: WEFAX_AF };
  if (/\bnavtex\b/i.test(name)) return { dec: 'navtex', kind: 'NAVTEX', af: NAVTEX_AF };
  if (codes.includes('-TY') || /\bRTTY\b/i.test(name)) return { dec: 'rtty', kind: 'RTTY', af: RTTY_AF };
  if (codes.includes('-CW')) return { dec: 'cw', kind: 'CW', af: 0 };
  if (/\bvolmet\b/i.test(name)) return { dec: '', kind: 'VOLMET', af: VOICE_AF, voice: true };
  return null;
}

/** EiBi's utility entries on air at `nowMin` (minutes past 00:00 UTC), as labels. Listed frequencies are carriers (fax,
 *  teletype — as the curated table) or USB dials (HFDL, VOLMET — the marker goes on the signal above). An entry within
 *  1 kHz of a curated one, or of an earlier EiBi one of the same kind, is dropped: one label a signal. */
export function eibiKnownSignals(rows: readonly EibiRow[], nowMin: number, curated: readonly KnownSignal[] = KNOWN_SIGNALS): KnownSignal[] {
  const out: KnownSignal[] = [];
  for (const r of rows) {
    if (!(r.frequency > 0) || r.source !== 'eibi') continue;   // ★ the schedule only — bookmarks are bookmarkSignals's
    const c = eibiClass(r);
    if (!c || !eibiOnAir(r.comment, nowMin)) continue;
    const f = r.frequency;
    const dialIsListed = c.dec === 'hfdl' || c.voice;
    const hz = dialIsListed ? f + c.af : f;
    const dialHz = dialIsListed ? f : f - c.af;
    if (curated.some(k => Math.abs(k.hz - hz) <= 1000) || out.some(k => k.label.startsWith(c.kind) && Math.abs(k.hz - hz) <= 1000)) continue;
    const place = eibiPlace(r.name);
    const sig: KnownSignal = { hz, label: place ? `${c.kind} · ${place}` : c.kind, dec: c.dec, dialHz,
                               mode: c.dec === 'cw' ? 'cwu' : 'usb', prio: PRIO_EIBI, src: 'eibi' };
    if (c.voice) { sig.lo = f + 300; sig.hi = f + 2700; }
    out.push(sig);
  }
  return out.sort((a, b) => a.hz - b.hz);
}

// ── Bookmarks: personal, saved to the server, and learnt FM names ─────────────────────────────────────────────────
/* ★★★ STUART (2026-10-30): "the reason I dont want normal bookmarks is because EiBi ones will crowd the spectrum, but
 *     actual learnt ones and maybe ones saved to the server and personal ones could apply" — and "one other place the
 *     labels could apply and that is Broadcast FM with a learnt RDS name". So three more sources, and EiBi's broadcast
 *     rows stay OUT:
 *      • PERSONAL — the user's own bookmarks (this device / their account, for this server);
 *      • SAVED TO THE SERVER — the owner's list: a VibeServer row with "manual": true, or an OpenWebRX / Kiwi server's
 *        bookmarks (they say nothing either way, and are the owner's). An UberSDR's list mixes the schedule in and
 *        cannot be told apart, so its unmarked rows are NOT labels (the caller says which: `unmarkedIsSaved`);
 *      • LEARNT — a VibeServer row with "manual": false, in the FM broadcast band (76–108 MHz), mode WFM, not a DAB
 *        service, whose name was HEARD and settled (nameSrc 2, or 3 = the broadcaster's own RadioDNS name) — never a
 *        provisional "PI4322 93.7MHz" or an unverified guess. The receiver learnt it, so it is per receiver by nature:
 *        a name learnt at one server's location is never shown on another's.
 *  ★ The label is the name, shortened (bookmarkLabel). The marker is the bookmark's own frequency (where you tune).
 *  ★ One label a frequency: rows within 1 kHz of each other keep the highest priority (a personal bookmark of a station
 *    the server also learnt shows the personal name). */
export interface BookmarkRow {
  name: string; frequency: number; mode?: string; source?: string; manual?: boolean;
  pi?: number; nameSrc?: number; sid?: number; repeater?: boolean;
}
/** The longest label a bookmark gets (characters, an ellipsis included). */
export const BOOKMARK_LABEL_MAX = 16;
export function bookmarkLabel(name: string): string {
  const s = String(name || '').replace(/\s+/g, ' ').trim();
  return s.length > BOOKMARK_LABEL_MAX ? s.slice(0, BOOKMARK_LABEL_MAX - 1).trimEnd() + '…' : s;
}
const FM_LO = 76e6, FM_HI = 108e6;
/** True when a server row is a name this receiver LEARNT for an FM broadcast channel (see above). */
export function isLearntFm(b: BookmarkRow): boolean {
  if (b.manual !== false || b.source !== 'server') return false;
  if (!(b.frequency >= FM_LO && b.frequency <= FM_HI)) return false;
  if (b.mode && b.mode.toLowerCase() !== 'wfm') return false;
  if (typeof b.sid === 'number' && b.sid >= 0) return false;
  const src = typeof b.nameSrc === 'number' ? b.nameSrc : 2;
  return src >= 2 && !/^PI[0-9A-F]{4}\b/i.test(b.name);
}
const bmMode = (m?: string): string => (m || 'am').toLowerCase();
export function bookmarkSignals(personal: readonly BookmarkRow[], server: readonly BookmarkRow[], unmarkedIsSaved: boolean): KnownSignal[] {
  const cand: KnownSignal[] = [];
  const add = (b: BookmarkRow, prio: number, src: KnownSignal['src']) => {
    if (!(b.frequency > 0) || !Number.isFinite(b.frequency)) return;
    const label = bookmarkLabel(b.name);
    if (!label) return;
    cand.push({ hz: Math.round(b.frequency), label, dec: '', dialHz: Math.round(b.frequency), mode: bmMode(b.mode), prio, src });
  };
  for (const b of personal) add(b, PRIO_PERSONAL, 'personal');
  for (const b of server) {
    if (b.source === 'eibi' || b.repeater) continue;                     // ★ the schedule never; nor a repeater database
    if (b.manual === true || (b.manual === undefined && unmarkedIsSaved)) add(b, PRIO_SERVER, 'server');
    else if (isLearntFm(b)) add(b, PRIO_LEARNT, 'learnt');
  }
  cand.sort((a, b) => (b.prio - a.prio) || (a.hz - b.hz));
  const out: KnownSignal[] = [];
  for (const c of cand) if (!out.some(o => Math.abs(o.hz - c.hz) <= 1000)) out.push(c);
  return out.sort((a, b) => a.hz - b.hz);
}

/** The curated table, EiBi's on-air utility entries and the bookmark labels, one sorted list. */
export function knownSignalList(eibi: readonly KnownSignal[], bookmarks: readonly KnownSignal[] = []): KnownSignal[] {
  if (!eibi.length && !bookmarks.length) return KNOWN_SIGNALS as KnownSignal[];
  return [...KNOWN_SIGNALS, ...eibi, ...bookmarks].sort((a, b) => a.hz - b.hz);
}

// ── In view, and laid out ──────────────────────────────────────────────────────────────────────────────────────────

/** First index with hz ≥ lo in a sorted list (binary search — the visible set is a slice, never a scan). */
function lowerBound(list: readonly KnownSignal[], lo: number): number {
  let a = 0, b = list.length;
  while (a < b) { const m = (a + b) >> 1; if (list[m].hz < lo) a = m + 1; else b = m; }
  return a;
}

/** The signals whose marker is inside [lo, hi] (Hz), from a list sorted by hz. */
export function signalsInView(list: readonly KnownSignal[], lo: number, hi: number): KnownSignal[] {
  const out: KnownSignal[] = [];
  if (!(hi > lo)) return out;
  for (let i = lowerBound(list, lo); i < list.length && list[i].hz <= hi; i++) out.push(list[i]);
  return out;
}

export interface PlacedLabel {
  sig: KnownSignal;
  /** The marker's x, and the label box's left and width (px). */
  x: number; lx: number; w: number;
  /** 0 = the row by the boundary, 1 = the next. */
  row: number;
}

/** A box another label already owns on a row (LISTEN, RF CENTRE…), px. */
export interface BlockedBox { row: number; x0: number; x1: number }

/**
 * Lay out the labels in view: `lo`..`lo + span` Hz across `width` px; `measure(label)` = the label's box width (px);
 * `dialHz` breaks priority ties (nearer wins). Each label sits to the RIGHT of its marker (`pad` px), or to its left
 * near the right edge (as RF CENTRE's does). A label that overlaps one already placed (or a blocked box) tries the next
 * row; if every row is taken it is dropped. Never more than LABEL_MAX. Returned in the order placed.
 */
export function labelLayout(list: readonly KnownSignal[], lo: number, span: number, width: number,
                            measure: (label: string) => number, dialHz: number,
                            blocked: readonly BlockedBox[] = [], pad = 3, rows = LABEL_ROWS, gap = LABEL_GAP): PlacedLabel[] {
  const out: PlacedLabel[] = [];
  if (!(span > 0 && width > 0)) return out;
  const vis = signalsInView(list, lo, lo + span);
  if (!vis.length) return out;
  const d = Number.isFinite(dialHz) ? dialHz : lo + span / 2;
  vis.sort((a, b) => (b.prio - a.prio) || (Math.abs(a.hz - d) - Math.abs(b.hz - d)) || (a.hz - b.hz));
  const taken: Array<Array<[number, number]>> = Array.from({ length: rows }, () => []);
  for (const b of blocked) if (b.row >= 0 && b.row < rows) taken[b.row].push([b.x0, b.x1]);
  const free = (row: number, x0: number, x1: number) => taken[row].every(([a, b]) => x1 + gap <= a || x0 >= b + gap);
  for (const sig of vis) {
    if (out.length >= LABEL_MAX) break;
    const x = (sig.hz - lo) / span * width;
    const w = measure(sig.label);
    const lx = x + pad + w > width ? Math.max(0, x - pad - w) : x + pad;
    for (let r = 0; r < rows; r++) {
      if (!free(r, lx, lx + w)) continue;
      taken[r].push([lx, lx + w]);
      out.push({ sig, x, lx, w, row: r });
      break;
    }
  }
  return out;
}

/** A label's width estimate where text cannot be measured synchronously (React Native): `chars × fontSize × k` + the
 *  box's padding. k = 0.6 covers the app's display faces at 9 pt (wide letters and the "·" included). */
export function estimateLabelWidth(label: string, fontSize: number, padX = 3, k = 0.6): number {
  return Math.ceil(label.length * fontSize * k + 2 * padX);
}

// ── Tapped: tune, and open the right decoder ──────────────────────────────────────────────────────────────────────
/* ★★★ STUART (2026-10-30): "can they be tapped to tune and chose the decoder?" — ONE TAP on a label's box tunes to the
 *     signal's DIAL in its MODE and opens its decoder with the settings the signal was published with (`preset`). A tap is
 *     a listener's own tune — the callers send it down the same road as tap-to-tune (ask-before-tuning on a shared dial,
 *     the VFO lock, a whole-radio mode refusing it). A DRAG that starts on a label still pans: only a tap ever hits one.
 *  ★ A decoder this receiver does not run (the owner's block list, an older server, a backend without it) is not opened —
 *    the tap still tunes (AGENTS.md: never a dead control; the label stays useful as a bookmark). */
/** Which decoders this receiver can open now (absent = no). */
export type KnownAvail = Partial<Record<Exclude<KnownDecoder, ''>, boolean>>;
export interface KnownTapPlan {
  /** Where to tune (the dial, not the marker) and in which mode. */
  hz: number; mode: string;
  /** The decoder to open ('' = just tune: a voice service, a bookmark, or one this receiver does not run). */
  dec: KnownDecoder;
  /** Its settings for this session (only with a decoder). */
  preset?: KnownPreset;
}
export function knownTapPlan(sig: KnownSignal, avail: KnownAvail): KnownTapPlan {
  const dec: KnownDecoder = sig.dec && avail[sig.dec] ? sig.dec : '';
  // ★ a bookmark saved as plain "cw" (OpenWebRX's name) is CW upper here — sdrTypes.ts SDRMode has no 'cw'
  const mode = String(sig.mode || '').toLowerCase();
  const plan: KnownTapPlan = { hz: sig.dialHz, mode: mode === 'cw' ? 'cwu' : mode, dec };
  if (dec && sig.preset) plan.preset = sig.preset;
  return plan;
}

/** The smallest hit area a label's box gets (pt / CSS px): a finger is bigger than a 13-pt box. */
export const LABEL_HIT_MIN_H = 30;
/** Extra hit width either side of a box (pt / CSS px). */
export const LABEL_HIT_PAD_X = 4;
/**
 * ★★ THE LABEL UNDER A TAP. Each placed box (`lx`..`lx + w` across, `rowTop(row)`..`+ boxH` down) is hit over a larger
 * area — `padX` wider each side and at least `minH` tall, centred on the box — and where two such areas OVERLAP (the
 * second row sits 14 above the first) the point goes to the NEARER box, so a neighbour's enlarged area never steals a tap
 * aimed at its neighbour. Null = no label there (the caller tunes to the point instead). Units are the caller's.
 */
export function knownLabelHit(placed: readonly PlacedLabel[] | null | undefined, x: number, y: number,
                              rowTop: (row: number) => number, boxH: number,
                              minH = LABEL_HIT_MIN_H, padX = LABEL_HIT_PAD_X): PlacedLabel | null {
  if (!placed || !placed.length || !Number.isFinite(x) || !Number.isFinite(y)) return null;
  let best: PlacedLabel | null = null, bestD = Infinity;
  const half = Math.max(boxH, minH) / 2;
  for (const p of placed) {
    const t = rowTop(p.row), cy = t + boxH / 2;
    if (x < p.lx - padX || x > p.lx + p.w + padX || y < cy - half || y > cy + half) continue;
    // distance from the point to the box itself (0 inside it)
    const dx = x < p.lx ? p.lx - x : x > p.lx + p.w ? x - p.lx - p.w : 0;
    const dy = y < t ? t - y : y > t + boxH ? y - t - boxH : 0;
    const d = dx * dx + dy * dy;
    if (d < bestD) { bestD = d; best = p; }
  }
  return best;
}

/** ★★ TAP, NOT DRAG — the rule both clients' gestures keep: a press that moved more than `slop` (pt / CSS px) or was
 *  held longer than `maxMs` is a pan or a hold, never a label tap. The app's Tap gesture (maxDuration 300, maxDist) and
 *  the web's pointer handler (its 2-px pan threshold) are this rule; the test proves it in one place. */
export const LABEL_TAP_SLOP = 10, LABEL_TAP_MAX_MS = 300;
export function isLabelTap(dx: number, dy: number, ms: number, slop = LABEL_TAP_SLOP, maxMs = LABEL_TAP_MAX_MS): boolean {
  return Math.hypot(dx, dy) <= slop && ms <= maxMs;
}

/** ★ THE LABELS' COLOUR — fixed, not the VFO's: they share the boundary rows with LISTEN (drawn in the VFO colour), so a
 *  VFO-coloured label would read as a second needle. Not the scale's green or amber, RF CENTRE's cyan or the DC spike's
 *  red — each already means something there. A pale lilac on a dark box (RF CENTRE's own backing) reads on every
 *  faceplate and palette. */
export const KNOWN_LABEL_COLOUR = '#d9b8ff';
export const KNOWN_LABEL_BOX = 'rgba(0,0,0,0.62)';
