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
import { DAB_BLOCKS } from '../services/dabBlocks.ts';

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
  spots?: 'FT8' | 'FT4' | 'WSPR' | 'JS8' | 'Q65';
}

export interface KnownSignal {
  /** Where the marker goes: the signal's centre / carrier as seen on the spectrum (Hz). */
  hz: number;
  /** Optional occupied span drawn as a bar (Hz, absolute) — the digital modes' 0–3 kHz above the dial. */
  lo?: number; hi?: number;
  /** The label: "RTTY · DWD", "WEFAX · Northwood", "FT8", "NAVTEX", "DSC". */
  label: string;
  /** The decoder it wants ('' = none — VOLMET is speech). */
  dec: KnownDecoder;
  /** Where to tune for it, and in which mode. */
  dialHz: number; mode: string;
  /** Higher wins a collision (PRIO_*). */
  prio: number;
  src: 'table' | 'eibi' | 'personal' | 'server' | 'learnt' | 'dab' | 'dx' | 'tag' | 'sat';   // 'sat' = a satellite in range (beta91 satLabelSignals)   // 'dab' = a Band III block (dabBlockView.ts); 'tag' = the server identified its shape (sigTagSignals)
  /** The decoder's settings for this signal, where they are published (curated entries only). */
  preset?: KnownPreset;
  /** ★★ V12 a DX CATCH from the server's DX Log (dxDialSignals) — drawn ITALIC in DX_LABEL_COLOUR. On a wide DAB view a
   *  block named only by a DX catch carries it too (dabBlockView.ts). */
  dx?: DxDialMark;
  /** ★★ V12 a SIGNAL TAG — the server identified the signal from its spectrum shape alone (vibe_varatag.h): its kind and
   *  the server's confidence. Display and tune only; never a decoder (VARA is proprietary and never decoded). */
  tag?: { kind: 'vara'; conf: number };
  /** ★★ beta91 a SATELLITE IN RANGE (satLabelSignals): the birds it names (one, or a label-only constellation's several
   *  merged), the highest one's elevation, and whether it is a name only (ORBCOMM — never decoded). */
  sat?: { names: string[]; tx: string; el: number; labelOnly: boolean };
}
/** What a DX label carries beyond its text: when it was last heard (unix s), its name, and its DAB block (DAB only). */
export interface DxDialMark {
  heard: number; name: string;
  /** DAB only: the block ("12C") and the ensemble's name alone ("Far Away", or "EId E0F1") — a wide DAB view's block box. */
  block?: string; ensemble?: string;
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
/** ★★ V12 a DX catch (dxDialSignals) — below every bookmark and learnt name (Stuart, 2026-11-16): the first to be shortened
 *  or dropped when the rows run out. (EiBi's utility rows are HF; a DX catch is FM or Band III — they never meet.) */
export const PRIO_DX = 0;

/** The most labels drawn at once — a hard cap, whatever is in view (Pi-2-class browsers, 1 GB phones). */
export const LABEL_MAX = 12;
/** Rows of labels always kept for them (the two boundary rows) — the VFO's own box sits above at least these. */
export const LABEL_ROWS = 2;
/** ★★ The most rows the signal labels may STACK on (Stuart, 2026-11-16: closely spaced signals "must still stack, never
 *  overlap or drop a label") — as few as the labels in view need; a name is shortened ("…") only once these are used up. */
export const LABEL_ROWS_MAX = 4;
/** The fewest characters a shortened label keeps (before its "…"). */
export const LABEL_MIN_CHARS = 3;
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
function digi(mode: 'FT8' | 'FT4' | 'WSPR' | 'JS8' | 'Q65', dial: number, top = 3000, bottom = 0): KnownSignal {
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
 *  • FT8 / FT4 / WSPR / JS8 / Q65 dials — the published dials the server's Band mode uses (android/app/src/main/cpp/
 *    decoders/digi_bands.h publishedDials, which records where each came from); scripts/test_known_signals.ts reads
 *    that header and fails if the two disagree. WSPR's signals sit 1400–1600 Hz above its dial.
 *  • Shannon VOLMET 3413 · 5505 · 8957 · 13264 kHz USB (Irish Aviation Authority; EiBi A26 "Shannon Volmet" 3413).
 *    Speech — no decoder; the label says USB.
 *  • Time signals: MSF / WWVB 60 kHz, DCF77 77.5 kHz, RBU 66.666 kHz, RWM 4996 / 9996 / 14996 kHz, WWV(H) 2.5 / 5 /
 *    10 / 15 MHz, WWV 20 / 25 MHz — the stations DecoderClient's timeStationFor decodes (decoderBands.ts
 *    TIME_SIGNAL_HZ).
 *  • ✗ NOT CHU (Ottawa, 3330 / 7850 / 14670 kHz): NRC ended the shortwave broadcast on 2026-06-22, so a label there
 *    names a silent channel (Stuart, 2026-11-15: "no point supporting a time station that doesn't exist").
 */
const DWD_RTTY = [147.3, 4583, 7646, 10100.8, 11039, 14467.3];
const DWD_FAX = [3855, 7880, 13882.5];
const NORTHWOOD_FAX = [2618.5, 4610, 8040, 11086.5];
const SHANNON_VOLMET = [3413, 5505, 8957, 13264];
/** digi_bands.h publishedDials — FT8, FT4, WSPR, JS8, Q65 (Hz). Kept in step by scripts/test_known_signals.ts. */
export const DIGI_DIALS: Readonly<Record<'FT8' | 'FT4' | 'WSPR' | 'JS8' | 'Q65', readonly number[]>> = {
  FT8: [1840000, 3573000, 5357000, 7074000, 10136000, 14074000, 18100000, 21074000, 24915000, 28074000, 50313000,
        70154000, 144174000, 432174000],
  FT4: [3575000, 7047500, 10140000, 14080000, 18104000, 21140000, 24919000, 28180000, 50318000, 144170000],
  WSPR: [136000, 474200, 1836600, 3568600, 5364700, 7038600, 10138700, 14095600, 18104600, 21094600, 24924600,
         28124600, 70091000, 144490500],
  JS8: [1842000, 3578000, 7078000, 10130000, 14078000, 18104000, 21078000, 24922000, 28078000, 50318000],
  // ★ V12: Q65's EME segments are TILED (a window every 4–4.5 kHz) — one label per run of joined windows (q65Runs)
  Q65: [50275000, 50310000, 144115000, 144119500, 144124000, 144128500, 144133000, 144137500, 144180000,
        432074000, 432078500, 432083000, 432087000, 432091500, 432096000,
        1296050000, 1296054500, 1296059000, 1296063500, 1296068000, 1296072500,
        1296077000, 1296081500, 1296086000, 1296090500, 1296095000, 1296099500,
        1296104000, 1296108500, 1296113000, 1296117500, 1296122000],
};
/** Q65's window: 200–4800 Hz above each dial (digimodes/q65.h). */
const Q65_LO = 200, Q65_HI = 4800;
/** ★ Q65's tiled dials as RUNS: dials whose windows join (or overlap) make one label — 17 labels on 23 cm would crowd
 *  out everything else. The label's dial is the run's first (a tap tunes there and opens DIGI SPOTS, which runs every
 *  Q65 dial the listener's window reaches); its bar covers the whole run. */
export function q65Runs(dials: readonly number[]): { dial: number; lo: number; hi: number }[] {
  const out: { dial: number; lo: number; hi: number }[] = [];
  for (const d of [...dials].sort((a, b) => a - b)) {
    const last = out[out.length - 1];
    if (last && d + Q65_LO <= last.hi + 1) last.hi = Math.max(last.hi, d + Q65_HI);
    else out.push({ dial: d, lo: d + Q65_LO, hi: d + Q65_HI });
  }
  return out;
}

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
  for (const r of q65Runs(DIGI_DIALS.Q65)) t.push(digi('Q65', r.dial, r.hi - r.dial, r.lo - r.dial));
  for (const k of SHANNON_VOLMET) t.push(volmet(k, 'Shannon'));
  // ★ the stations each one is decoded as — DecoderClient timeStationFor's own map (RBU's 66.666 kHz reads as 'rwm')
  // ★ CW = 'cwu' on the wire (sdrTypes.ts SDRMode has no plain 'cw'): the carrier keyed in amplitude becomes a beat note
  t.push(time(40_000, 'JJY', 'cwu', 'time', ['jjy']),
         time(60_000, 'MSF/WWVB/JJY', 'cwu', 'time', ['msf', 'wwvb', 'jjy']), time(77_500, 'DCF77', 'cwu', 'time', ['dcf77']),
         time(66_666, 'RBU', 'cwu', 'time', ['rwm']), time(162_000, 'TDF', 'cwu', 'time', ['tdf']));
  for (const f of [4_996_000, 9_996_000, 14_996_000]) t.push(time(f, 'RWM', 'cwu', 'time', ['rwm']));
  for (const f of [2_500_000, 5_000_000, 10_000_000, 15_000_000]) t.push(time(f, 'WWV/WWVH', 'am', 'time', ['wwv']));
  for (const f of [20_000_000, 25_000_000]) t.push(time(f, 'WWV', 'am', 'time', ['wwv']));
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

/** The curated table, EiBi's on-air utility entries, the bookmark labels and the DX catches (dxDialSignals — already
 *  cleared of every channel the others hold), one sorted list. */
export function knownSignalList(eibi: readonly KnownSignal[], bookmarks: readonly KnownSignal[] = [],
                                dx: readonly KnownSignal[] = []): KnownSignal[] {
  if (!eibi.length && !bookmarks.length && !dx.length) return KNOWN_SIGNALS as KnownSignal[];
  return [...KNOWN_SIGNALS, ...eibi, ...bookmarks, ...dx].sort((a, b) => a.hz - b.hz);
}

// ── DX catches on the dial ────────────────────────────────────────────────────────────────────────────────────────
/* ★★★ STUART (2026-11-16): the server's DX Log (vibe_dxscan.h) puts its LOCALS in the discovered bookmarks; the other
 *     entries — the DX catches: Sporadic-E, tropo, meteor scatter — stay in the log and NEVER become bookmarks. They are
 *     shown on the dial as signal labels too, MARKED AS SPORADIC:
 *      • the look: "RNE 1 · DX · 2 h ago" — the name, a DX tag and when it was last heard; ITALIC; its own colour
 *        (DX_LABEL_COLOUR), never the bookmarks' lilac, the VFO's or DAB's;
 *      • for a WEEK from when it was last heard (DX_DIAL_MAX_AGE_S), then only in the DX Log;
 *      • a local, a bookmark or a known-signal label on the same channel WINS: no DX label is drawn over or instead of it
 *        (the server already leaves out a channel its log holds a Local on; `others` and `bookmarkHz` do the rest here);
 *      • only real catches: class "dx" — never a Suspect (a spill, an image: no station there at all) or a Provisional
 *        (the first day's baseline, not yet judged — mostly the locals themselves);
 *      • FM in every range the scanner sweeps — OIRT 65.81–74 (30 kHz, named to two decimals: "69.80"), 76–87.45 and
 *        87.5–108 — and Band III for DAB catches (a wide DAB view folds them into its block labels: dabBlockView.ts).
 *  ★ The server's GET /vibeserver/dxlog.json?dial=1 sends exactly these (vibe_dxscan.h dialJson); an older server sends
 *    its ordinary page, which this filters by the same rules. Both are gated as the DX Log itself is (the read PIN, and
 *    the owner's "admin lock on viewing": a listener who cannot open the log gets a 403 and no labels).
 *  ★ The text says how long ago, so the callers rebuild it each minute while any are held (cheap: ≤ DX_DIAL_MAX). */
/** The DX labels' colour: a lemon yellow — not the bookmarks' lilac (KNOWN_LABEL_COLOUR), the VFO's red, DAB's pink,
 *  RF CENTRE's cyan, the scale's green or amber, or the DAB grade marks. */
export const DX_LABEL_COLOUR = '#ffe45c';
/** How long a catch stays on the dial after it was last heard (s) — vibe_dxscan.h Log::kDialSec. */
export const DX_DIAL_MAX_AGE_S = 7 * 86400;
/** The most DX labels held (vibe_dxscan.h Log::kDialMax). */
export const DX_DIAL_MAX = 100;
/** How often the callers fetch the list while the labels are on (ms) — the scanner sweeps only while nobody listens. */
export const DX_DIAL_FETCH_MS = 10 * 60_000;
/** Two labels on one FM channel: within this (Hz) — under half OIRT's 30 kHz raster. */
const DX_SAME_FM_HZ = 12_000;
/** …on one DAB block (10A and 10N sit 160 kHz apart). */
const DX_SAME_DAB_HZ = 50_000;
/** The DX Log's own extremes: OIRT's foot to the top of Band II (vibe_dxscan.h kFmRanges). */
const DX_FM_LO_KHZ = 65_800, DX_FM_HI_KHZ = 108_000;

/** A DX Log entry as GET /vibeserver/dxlog.json sends it (dxLog.ts DxEntry; ?dial=1 sends only these fields). */
export interface DxDialEntry {
  band: string; class: string; lastHeard: number;
  freqKhz?: number; range?: string; pi?: string; ps?: string; rds?: boolean;
  block?: string; eid?: string; ensemble?: string;
}

/** "just now", "12 min ago", "2 h ago", "1 day ago", "3 days ago". */
export function dxDialAgo(heard: number, nowS: number): string {
  const s = Math.max(0, nowS - heard);
  if (s < 120) return 'just now';
  if (s < 3600) return `${Math.floor(s / 60)} min ago`;
  if (s < 86400) return `${Math.floor(s / 3600)} h ago`;
  const d = Math.floor(s / 86400);
  return `${d} day${d === 1 ? '' : 's'} ago`;
}
/** An FM channel as the DX Log writes it: "98.8"; OIRT (30 kHz) and any channel off the 100 kHz raster to two decimals —
 *  "69.80", "86.65" (dxLog.ts dxWhere's rule). */
export function dxFmChannel(khz: number, range?: string): string {
  const oirt = range === 'oirt' || (!range && khz > 0 && khz < 75_000);
  return (khz / 1000).toFixed(oirt || khz % 100 ? 2 : 1);
}
/** The name a DX label shows: FM — the PS, else the PI, else (no RDS) the channel; DAB — the block and the ensemble. */
export function dxDialName(e: DxDialEntry): string {
  if (e.band === 'DAB') {
    const ens = String(e.ensemble ?? '').replace(/\s+/g, ' ').trim();
    return [e.block ?? '', ens || (e.eid ? `EId ${e.eid}` : '')].filter(Boolean).join(' ');
  }
  const ps = String(e.ps ?? '').replace(/\s+/g, ' ').trim();
  if (ps) return ps;
  if (e.rds !== false && e.pi && e.pi !== '0000') return `PI ${e.pi}`;
  return dxFmChannel(e.freqKhz ?? 0, e.range);
}
/** "RNE 1 · DX · 2 h ago" — the name (bookmarkLabel's length), the tag, when. Shortened from the END when the rows run
 *  out, so the name goes last of all; the italic and the colour still say DX. */
export function dxDialLabel(e: DxDialEntry, nowS: number): string {
  return `${bookmarkLabel(dxDialName(e))} · DX · ${dxDialAgo(e.lastHeard, nowS)}`;
}
/** Where a DX catch sits (Hz): the FM channel, or the DAB block's centre (the server's freqKhz, else Band III's table). */
function dxDialHz(e: DxDialEntry): number {
  if (e.band === 'DAB') {
    if (e.freqKhz && e.freqKhz > 0) return e.freqKhz * 1000;
    return DAB_BLOCKS.find((b) => b.name === e.block)?.hz ?? 0;
  }
  return (e.freqKhz ?? 0) * 1000;
}

/**
 * ★★ THE DX CATCHES AS LABELS (see above): `entries` from GET /vibeserver/dxlog.json (?dial=1, or an older server's page),
 * `nowS` the SERVER's clock (its page's `now` plus the time since — a phone's clock an hour out must not age a catch),
 * `others` the labels already in force (table, EiBi, bookmarks), `bookmarkHz` every bookmark's frequency (a local in the
 * discovered bookmarks with no settled name draws no label, and still wins its channel). One label a channel — the most
 * recently heard; newest first up to DX_DIAL_MAX; returned sorted by frequency.
 */
export function dxDialSignals(entries: readonly DxDialEntry[] | null | undefined, nowS: number,
                              others: readonly KnownSignal[] = [], bookmarkHz: readonly number[] = []): KnownSignal[] {
  if (!entries || !entries.length || !Number.isFinite(nowS)) return [];
  const cand = entries.filter((e) => !!e && e.class === 'dx' && e.lastHeard > 0 && nowS - e.lastHeard <= DX_DIAL_MAX_AGE_S
                                    && e.lastHeard - nowS <= 3600)
                      .sort((a, b) => b.lastHeard - a.lastHeard);
  const taken = [...others.filter((o) => o.src !== 'dx').map((o) => o.hz), ...bookmarkHz.filter((h) => h > 0)];
  const out: KnownSignal[] = [];
  for (const e of cand) {
    if (out.length >= DX_DIAL_MAX) break;
    const dab = e.band === 'DAB';
    if (!dab && e.band !== 'FM') continue;
    if (!dab && !((e.freqKhz ?? 0) >= DX_FM_LO_KHZ && (e.freqKhz ?? 0) <= DX_FM_HI_KHZ)) continue;
    const hz = dxDialHz(e);
    if (!(hz > 0)) continue;
    const tol = dab ? DX_SAME_DAB_HZ : DX_SAME_FM_HZ;
    if (taken.some((h) => Math.abs(h - hz) <= tol) || out.some((o) => Math.abs(o.hz - hz) <= tol)) continue;
    const name = dxDialName(e);
    out.push({ hz, label: dxDialLabel(e, nowS), dec: '', dialHz: hz, mode: dab ? 'dab' : 'wfm', prio: PRIO_DX, src: 'dx',
               dx: { heard: e.lastHeard, name, ...(dab && e.block ? { block: e.block, ensemble: dxDialName({ ...e, block: '' }) } : {}) } });
  }
  return out.sort((a, b) => a.hz - b.hz);
}
/** True for a DX catch's label (drawn italic in DX_LABEL_COLOUR) — a dxDialSignals row, or a DAB block named by one. */
export function isDxLabel(s: KnownSignal | null | undefined): boolean {
  return !!s && !!s.dx;
}
/** The words a screen reader or a hover gives a DX label: "DX catch 98.8 MHz, RNE 1, last heard 2 h ago". */
export function dxDialHint(s: KnownSignal, nowS: number): string {
  if (!s.dx) return s.label;
  const where = s.dx.block ? `DAB ${s.dx.block}` : `${dxFmChannel(Math.round(s.hz / 1000))} MHz`;
  return `DX catch ${where}, ${s.dx.name}, last heard ${dxDialAgo(s.dx.heard, nowS)}`;
}
/** The server's clock now (s), from a page's `now` and when it arrived (Date.now() ms then) — the age the labels show. */
export function dxServerNow(pageNow: number | undefined, fetchedAtMs: number, nowMs = Date.now()): number {
  const local = nowMs / 1000;
  return typeof pageNow === 'number' && pageNow > 0 ? pageNow + (nowMs - fetchedAtMs) / 1000 : local;
}

// ── Signal tags: what the SERVER identified from a signal's shape (V12) ───────────────────────────────────────────
/* ★★★ THE VARA HF TAG (vibe_varatag.h, docs/v12/VARA-TAG.md — Stuart, 2026-11-19). A VibeServer marks a signal whose
 *     OUTWARD shape is VARA HF's: a flat block of one of its published widths (2750 / 2300 / 500 Hz), a regular comb of
 *     carriers ~47 Hz apart in the power spectrum, coming and going in ARQ bursts. IDENTIFY ONLY:
 *     ★★★ VARA is a proprietary modem whose author enforces it — VibeSDR NEVER decodes it, in any form. The label says so
 *         in so many words, and never anything more (VARA_TAG_LABEL). There is no decoder to open: a tap only tunes there.
 *  ★ The server sends `{"type":"sigtags","v":1,"tags":[{"id","hz","bw","k":"vara","c"}]}` on the spectrum socket when the
 *    set changes (and every 10 s while any are held); an older server sends nothing and nothing is drawn. The client
 *    forgets them SIGTAG_STALE_MS after the last message, so a server that stops sending cannot leave one behind.
 *  ★ A curated label, an EiBi utility row or a bookmark on the same signal WINS (they name it better); a tag is drawn in
 *    the labels' own look, with the occupied span as the digital modes' bar. */
/** The tag's words — ONE TEXT, every reader (web, app, R36S known_labels.h): never more than this. */
export const VARA_TAG_LABEL = 'VARA HF (proprietary — not decoded)';
/** What a hover or a screen reader says. */
export const VARA_TAG_HINT = 'VARA HF — a proprietary data modem (Winlink). VibeSDR recognises its shape on the spectrum; it does not decode it.';
/** The audio centre a VARA HF signal sits at above a USB dial (Hz) — where a tap tunes the dial. */
export const VARA_AUDIO_CENTRE_HZ = 1500;
/** Tags not refreshed for this long are dropped (ms). */
export const SIGTAG_STALE_MS = 30_000;
/** One tag as the server sends it. */
export interface SigTagEntry { id: number; hz: number; bw: number; k: string; c: number }
/** A `sigtags` message's tags, checked: only kinds this client knows, finite, HF, at most 16. */
export function parseSigTags(msg: unknown): SigTagEntry[] {
  const tags = (msg && typeof msg === 'object' ? (msg as { tags?: unknown }).tags : null);
  if (!Array.isArray(tags)) return [];
  const out: SigTagEntry[] = [];
  for (const t of tags.slice(0, 16)) {
    if (!t || typeof t !== 'object') continue;
    const o = t as Record<string, unknown>;
    const hz = Number(o.hz), bw = Number(o.bw), c = Number(o.c);
    if (o.k !== 'vara' || !(hz > 0 && hz < 30e6) || !(bw >= 300 && bw <= 3200)) continue;
    out.push({ id: Number(o.id) || 0, hz: Math.round(hz), bw: Math.round(bw), k: 'vara', c: Number.isFinite(c) ? c : 0 });
  }
  return out;
}
/** The tags as labels, clear of every label already in force within half the tag's width (`others`). Sorted by hz. */
export function sigTagSignals(tags: readonly SigTagEntry[] | null | undefined, others: readonly KnownSignal[] = []): KnownSignal[] {
  if (!tags || !tags.length) return [];
  const out: KnownSignal[] = [];
  for (const t of tags) {
    if (t.k !== 'vara') continue;
    const half = t.bw / 2;
    if (others.some((o) => o.src !== 'tag' && Math.abs(o.hz - t.hz) <= half)) continue;
    out.push({ hz: t.hz, lo: t.hz - half, hi: t.hz + half, label: VARA_TAG_LABEL, dec: '', dialHz: t.hz - VARA_AUDIO_CENTRE_HZ,
               mode: 'usb', prio: PRIO_EIBI, src: 'tag', tag: { kind: 'vara', conf: t.c } });
  }
  return out.sort((a, b) => a.hz - b.hz);
}
/** True for a server signal tag's label. */
export function isTagLabel(s: KnownSignal | null | undefined): boolean {
  return !!s && s.src === 'tag';
}
// ── ★★ beta91 SATELLITE LABELS (vibe_sat_engine.h skyLabels, docs/v12/SAT-LABELS.md) ─────────────────────────────────
/* ★★★ STUART (2026-11-22): every satellite in range whose downlink falls in the waterfall is named there — "Meteor-M2-4
 *     LRPT", "ISS FM", "ORBCOMM FM114" — at its DOPPLER-SHIFTED frequency, following the drift, gone at LOS. NOT only in
 *     SATELLITE mode: "i could simply be listening to 2m band and if it appears we can label it".
 *  ★ The server works it out (it has the orbits, the location and a checked clock) and sends, on the spectrum socket,
 *    `{"type":"sat_labels","v":1,"t":unix,"sats":[{"id","n","norad","tx","hz","lo"?,"hi"?,"r","el","m","b"?}]}` every 4 s
 *    while a bird is up in the radio's capture, and an empty list once when the last one sets. The client keeps those in
 *    its own view (signalsInView) and forgets them SATLABEL_STALE_MS after the last message.
 *  ★★ IDENTIFY ONLY for a label-only bird (`b`: ORBCOMM — commercial data messaging): a name, never a decoder. Several
 *     ORBCOMM birds on the band at once are ONE label ("ORBCOMM FM114 +2") over the band. A tap only tunes there.
 *  ★ Switched with the other signal labels (Display settings' SIGNAL LABELS); the owner's "satlabels" stops the server. */
/** Satellite labels' colour: a pale sky blue — not the bookmarks' lilac, the DX lemon, the VFO's red or DAB's pink. */
export const SAT_LABEL_COLOUR = '#a8d8ff';
/** A satellite downlink's priority: below the curated table, above every bookmark; a label-only bird's, EiBi's. */
export const PRIO_SAT = 5;
/** Satellite labels not refreshed for this long are dropped (ms) — the server sends every 4 s while one is up. */
export const SATLABEL_STALE_MS = 15_000;
/** The most a message may carry (the server's kSkyLabelMax). */
export const SATLABEL_MAX = 24;
/** One downlink as the server sends it. */
export interface SatLabelEntry { id: string; n: string; norad: number; tx: string; hz: number; lo?: number; hi?: number; r: number; el: number; m: string; b: boolean }
const SAT_MODES = new Set(['nfm', 'usb', 'lsb', 'cw', 'am']);
/** A `sat_labels` message, checked: finite, 1 MHz – 10 GHz, a band ≤ 2 MHz wide, names trimmed, at most SATLABEL_MAX. */
export function parseSatLabels(msg: unknown): SatLabelEntry[] {
  const sats = (msg && typeof msg === 'object' ? (msg as { sats?: unknown }).sats : null);
  if (!Array.isArray(sats)) return [];
  const out: SatLabelEntry[] = [];
  for (const x of sats.slice(0, SATLABEL_MAX)) {
    if (!x || typeof x !== 'object') continue;
    const o = x as Record<string, unknown>;
    const hz = Number(o.hz), r = Number(o.r), el = Number(o.el), norad = Number(o.norad);
    const n = String(o.n ?? '').trim().slice(0, 48), tx = String(o.tx ?? '').trim().slice(0, 32);
    if (!(hz >= 1e6 && hz <= 1e10) || !n) continue;
    const e: SatLabelEntry = { id: String(o.id ?? '').slice(0, 32), n, norad: Number.isFinite(norad) ? norad : 0, tx, hz: Math.round(hz),
                               r: Number.isFinite(r) ? r : 0, el: Number.isFinite(el) ? el : 0,
                               m: SAT_MODES.has(String(o.m)) ? String(o.m) : 'nfm', b: o.b === true };
    const lo = Number(o.lo), hi = Number(o.hi);
    if (Number.isFinite(lo) && Number.isFinite(hi) && hi > lo && hi - lo <= 2.1e6 && hz >= lo - 1 && hz <= hi + 1) { e.lo = Math.round(lo); e.hi = Math.round(hi); }
    out.push(e);
  }
  return out;
}
/** ★ The name a label uses: the catalogue's name without its long form ("SO-50 (SaudiSat-1C)" → "SO-50"). */
export function satShortName(name: string): string {
  return name.replace(/\s*\(.*\)\s*$/, '').trim() || name.trim();
}
/** ★ The downlink in a word or two: "FM repeater" → "FM", a transponder → "linear", a beacon → "beacon",
 *  "SSTV events" → "SSTV"; everything else as the catalogue says it ("LRPT", "APRS", "Telemetry"). */
export function satTxWord(tx: string): string {
  const t = tx.trim();
  if (/repeater/i.test(t)) return 'FM';
  if (/transponder/i.test(t)) return 'linear';
  if (/beacon/i.test(t)) return 'beacon';
  if (/^sstv events?$/i.test(t)) return 'SSTV';
  if (/^voice \/ sstv$/i.test(t)) return 'Voice/SSTV';
  return t;
}
/** ★ ONE TEXT, every reader: "ISS FM", "Meteor-M2-4 LRPT", "ORBCOMM FM114" (the downlink's word left out when the name
 *  already says it — a label-only bird's downlink is its constellation's name). */
export function satLabelText(name: string, tx: string): string {
  const n = satShortName(name), w = satTxWord(tx);
  if (!w || n.toLowerCase().includes(w.toLowerCase())) return n;
  return `${n} ${w}`;
}
/** The constellation a label-only bird belongs to — its name's first word ("ORBCOMM FM114" → "ORBCOMM"). */
const satFamily = (name: string) => name.trim().split(/\s+/)[0].toUpperCase();
/**
 * The server's entries as labels. `ageS` = seconds since the message (each mark carried along its Doppler rate, at most
 * 8 s of it). Sorted by hz. Label-only birds over a BAND (no channel of their own) are merged per constellation into one
 * label over the band, named after the highest ("ORBCOMM FM114 +2").
 */
export function satLabelSignals(entries: readonly SatLabelEntry[] | null | undefined, ageS = 0): KnownSignal[] {
  if (!entries || !entries.length) return [];
  const dt = Math.max(0, Math.min(8, ageS));
  const out: KnownSignal[] = [];
  const bands = new Map<string, SatLabelEntry[]>();
  for (const e of entries) {
    if (e.b && e.lo != null && e.hi != null) {
      const k = satFamily(e.n);
      const g = bands.get(k); if (g) g.push(e); else bands.set(k, [e]);
      continue;
    }
    const move = e.r * dt;
    const hz = Math.round(e.hz + move);
    const s: KnownSignal = { hz, label: satLabelText(e.n, e.tx), dec: '', dialHz: hz, mode: e.m,
                             prio: e.b ? PRIO_EIBI : PRIO_SAT, src: 'sat', sat: { names: [e.n], tx: e.tx, el: e.el, labelOnly: e.b } };
    if (e.lo != null && e.hi != null) { s.lo = Math.round(e.lo + move); s.hi = Math.round(e.hi + move); }
    out.push(s);
  }
  for (const g of bands.values()) {
    g.sort((a, b) => b.el - a.el);
    const top = g[0], move = top.r * dt;
    const hz = Math.round(top.hz + move);
    out.push({ hz, lo: Math.round(top.lo! + move), hi: Math.round(top.hi! + move),
               label: satLabelText(top.n, top.tx) + (g.length > 1 ? ` +${g.length - 1}` : ''), dec: '', dialHz: hz, mode: 'nfm',
               prio: PRIO_EIBI, src: 'sat', sat: { names: g.map((x) => x.n), tx: top.tx, el: top.el, labelOnly: true } });
  }
  return out.sort((a, b) => a.hz - b.hz);
}
/** True for a satellite label. */
export function isSatLabel(s: KnownSignal | null | undefined): boolean {
  return !!s && s.src === 'sat';
}
/** What a hover or a screen reader says: who, which downlink, how high, and — for a label-only bird — that it is named only. */
export function satLabelHint(s: KnownSignal): string {
  const m = s.sat;
  if (!m) return s.label;
  const up = `${Math.round(m.el)}° up`;
  if (m.labelOnly) return `${m.names.join(', ')} — commercial data messaging, named only (never decoded); ${up}; Doppler-shifted`;
  return `${m.names[0]} — ${m.tx}, ${up}; at its Doppler-shifted frequency now`;
}
/** Labels plus satellites, one sorted list. */
export function withSatLabels(list: readonly KnownSignal[], sats: readonly KnownSignal[]): KnownSignal[] {
  if (!sats.length) return list as KnownSignal[];
  return [...list, ...sats].sort((a, b) => a.hz - b.hz);
}
/** ★ A label's colour, line and text alike: a DX catch's lemon, a satellite's sky blue, else the labels' lilac. */
export function knownLabelColour(s: KnownSignal | null | undefined): string {
  return s && s.dx ? DX_LABEL_COLOUR : s && s.src === 'sat' ? SAT_LABEL_COLOUR : KNOWN_LABEL_COLOUR;
}

/** Labels plus tags, one sorted list (tags already cleared by sigTagSignals). */
export function withSigTags(list: readonly KnownSignal[], tags: readonly KnownSignal[]): KnownSignal[] {
  if (!tags.length) return list as KnownSignal[];
  return [...list, ...tags].sort((a, b) => a.hz - b.hz);
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
  /** 0 = the row by the boundary, 1 = the next. A taller box's LOWEST row. */
  row: number;
  /** ★★ Rows the box is tall (default 1) — a wide DAB view's named block is two (dabBlockView.ts: the block over its name). */
  span?: number;
  /** The text drawn, one entry per line, top first, CENTRED in the box (absent = `sig.label` on one line, left-aligned). */
  lines?: string[];
}

/** A box another label already owns on a row (LISTEN, RF CENTRE…), px. */
export interface BlockedBox { row: number; x0: number; x1: number }

/**
 * Lay out the labels in view: `lo`..`lo + span` Hz across `width` px; `measure(label)` = the label's box width (px);
 * `dialHz` breaks priority ties (nearer wins). Placed highest priority first, never more than `max` (LABEL_MAX unless
 * a caller — the wide DAB view — says otherwise); returned in the order placed.
 * ★★★ THE VINTAGE DIAL'S WAY (Stuart, 2026-11-16: "centre all signal labels on their carrier frequency", "make it look
 *     like the vintage tuning dial if needed" — FmdxDial.tsx: each name centred on its frequency, cascading onto the
 *     next row where the one before is taken):
 *   • each label's box is CENTRED on its marker (labelBoxX), shifted inward at an edge;
 *   • it STACKS: the lowest row (0 = by the boundary, up to `rows`, LABEL_ROWS_MAX) where it overlaps no box already there
 *     (nor a `blocked` one) — and where its MARKER LINE is clear: every label has a line rising from its top edge to its
 *     carrier (labelStems), so a box may not cover the line of a label on a row below it, and a label may not sit under a
 *     box on a row above it. Where centred does not fit on a row, the box slides along it (nearest to centred first) as
 *     far as it still sits over its own carrier;
 *   • only when every row is used up is the name shortened with "…" (to LABEL_MIN_CHARS) to fit — `lines` then carries
 *     the shortened text; a label that fits nowhere even so is dropped.
 * `pad` is no longer used (kept so callers need not change).
 */
export function labelLayout(list: readonly KnownSignal[], lo: number, span: number, width: number,
                            measure: (label: string) => number, dialHz: number,
                            blocked: readonly BlockedBox[] = [], pad = 3, rows = LABEL_ROWS_MAX, gap = LABEL_GAP,
                            max = LABEL_MAX): PlacedLabel[] {
  void pad;
  const out: PlacedLabel[] = [];
  if (!(span > 0 && width > 0)) return out;
  const vis = signalsInView(list, lo, lo + span);
  if (!vis.length) return out;
  const d = Number.isFinite(dialHz) ? dialHz : lo + span / 2;
  vis.sort((a, b) => (b.prio - a.prio) || (Math.abs(a.hz - d) - Math.abs(b.hz - d)) || (a.hz - b.hz));
  const taken: Array<Array<[number, number]>> = Array.from({ length: rows }, () => []);
  for (const b of blocked) if (b.row >= 0 && b.row < rows) taken[b.row].push([b.x0, b.x1]);
  const LINE = 1;   // a marker line's half-clearance (px)
  /** Where a `w`-wide box for a marker at `x` can go on row `r` (its left edge), nearest to centred; NaN = nowhere. */
  const slot = (r: number, x: number, w: number): number => {
    // its own line must not run up through a box on a row above
    if (out.some((p) => p.row > r && x >= p.lx - LINE && x <= p.lx + p.w + LINE)) return NaN;
    const c = labelBoxX(x, w, width);
    const ok = (lx: number) => lx >= 0 && lx + w <= width && lx - LINE <= x && x <= lx + w + LINE
      && taken[r].every(([a, b]) => lx + w + gap <= a || lx >= b + gap)
      && out.every((p) => p.row >= r || p.x < lx - LINE || p.x > lx + w + LINE);   // no line from a row below under it
    if (ok(c)) return c;
    const cands: number[] = [];
    for (const [a, b] of taken[r]) cands.push(a - gap - w, b + gap);
    for (const p of out) if (p.row < r) cands.push(p.x - LINE - 0.5 - w, p.x + LINE + 0.5);
    cands.sort((u, v) => Math.abs(u - c) - Math.abs(v - c));
    for (const lx of cands) if (ok(lx)) return lx;
    return NaN;
  };
  const tryRows = (sig: KnownSignal, x: number, text: string, w: number): boolean => {
    for (let r = 0; r < rows; r++) {
      const lx = slot(r, x, w);
      if (Number.isNaN(lx)) continue;
      taken[r].push([lx, lx + w]);
      const p: PlacedLabel = { sig, x, lx, w, row: r };
      if (text !== sig.label) p.lines = [text];
      out.push(p);
      return true;
    }
    return false;
  };
  for (const sig of vis) {
    if (out.length >= max) break;
    const x = (sig.hz - lo) / span * width;
    if (tryRows(sig, x, sig.label, measure(sig.label))) continue;
    // every row used up: shorten with "…", longest first
    const cps = Array.from(sig.label.replace(/…$/, ''));
    for (let n = cps.length - 1; n >= LABEL_MIN_CHARS; n--) {
      const t = cps.slice(0, n).join('').trimEnd() + '…';
      if (tryRows(sig, x, t, measure(t))) break;
    }
  }
  return out;
}

/** ★★ A label's box: CENTRED on its marker `x`, moved inward only as far as it must to stay within 0..width (a box wider
 *  than the view starts at 0). The same rule as DAB's blocks (dabBlockView.ts dabBlockBoxX). */
export function labelBoxX(x: number, w: number, width: number): number {
  return Math.max(0, Math.min(Math.max(0, width - w), x - w / 2));
}

/** ★★ A LABEL'S MARKER LINE (Stuart, 2026-11-16 — "a short vertical line ABOVE the label, rising from the centre of the
 *  label's top edge up toward the signal"): one per placed box, at the TRUE carrier `x` (off the box's centre only where
 *  the box was shifted inward at an edge), from the box's top edge (`y1`) up to `rise` above the highest box in the stack
 *  (`y0`) — so a label on a lower row still points past the rows above it to its carrier. Where another box (a placed
 *  label, or one of `extra` — the VFO's) sits in the way, the line stops `gap` short of that box's bottom edge: it never
 *  crosses another label's text, drawn in the gap instead. A line shorter than `minLen` is still drawn at `minLen`
 *  (inside the gap rows leave). `rowTop(row)` = a box's top on that row and `boxH` its height, in the caller's units
 *  (y grows downward); a taller box (span) is topped by its highest row. Skipped (null) for none placed. */
export interface LabelStem { x: number; y0: number; y1: number }
export function labelStems(placed: readonly PlacedLabel[], rowTop: (row: number) => number, boxH: number, rise: number,
                           extra: readonly { lx: number; w: number; row: number; span?: number }[] = [], gap = 1,
                           minLen = 2): LabelStem[] {
  const top = (p: { row: number; span?: number }) => rowTop(p.row + Math.max(1, p.span ?? 1) - 1);
  const bot = (p: { row: number }) => rowTop(p.row) + boxH;
  if (!placed.length) return [];
  const stackTop = Math.min(...placed.map(top));
  const boxes = [...placed, ...extra];
  return placed.map((p) => {
    const x = Math.round(p.x), y1 = top(p);
    let y0 = stackTop - rise;
    for (const q of boxes) {
      if (q === p || x < q.lx || x > q.lx + q.w) continue;
      const qb = bot(q);
      if (qb <= y1 && qb + gap > y0) y0 = qb + gap;   // a box above it, in the way: stop under it
    }
    if (y1 - y0 < minLen) y0 = y1 - minLen;
    return { x, y0, y1 };
  });
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
  for (const p of placed) {
    // ★ a taller box (span > 1) runs from its top row's top down to its lowest row's box bottom
    const t = rowTop(p.row + Math.max(1, p.span ?? 1) - 1), h = rowTop(p.row) - t + boxH, cy = t + h / 2;
    const halfP = Math.max(h, minH) / 2;
    if (x < p.lx - padX || x > p.lx + p.w + padX || y < cy - halfP || y > cy + halfP) continue;
    // distance from the point to the box itself (0 inside it)
    const dx = x < p.lx ? p.lx - x : x > p.lx + p.w ? x - p.lx - p.w : 0;
    const dy = y < t ? t - y : y > t + h ? y - t - h : 0;
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
