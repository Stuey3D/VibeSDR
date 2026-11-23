/* bandSkim.ts — V12 beta92 THE BAND SKIMMER's readable CW and RTTY (Stuart, 2026-11-23, watching the web client on the
 * Lenovo RSP1A). ONE RULE, TWO READERS: the web client (web/client/src/main.ts renderSpots / applyKnownSignals / the
 * render loop's VFO) and the app (SDRScreen + DecoderPanel) both draw from these functions; scripts/test_band_skim.ts
 * holds them.
 *
 * ★★ What Stuart asked for:
 *   1. "digi band should be called Band Skimmer as it covers cw which is analogue" — BAND_SKIMMER_NAME / BAND_SKIM_KEY
 *      (the single-mode decoder stays DIGITAL SPOTS). Only words a person reads changed: the wire's ids ("spots",
 *      scope "band", digital_spot, data-dec "spotsband") are as they were, so older servers and clients still talk.
 *   2. In ALL: a CW row shows its FREQUENCY and "waiting for callsign" until a call is read, then it is a normal spot row;
 *      EXPANDED, the CW and RTTY rows show the decoded message "like we do in the cw skimmer, same for rtty".
 *   3. The waterfall marks where CW (and RTTY) has been detected, while the box is open (bandSkimLabels — the signal-label
 *      layer every reader already draws, in the mode's own colour).
 *   4. "the vfo needs to show the width of the skimmer too": the Band VFO is drawn over the band's span AND the CW / RTTY
 *      windows being skimmed (bandSkimPassband) — drawn only, never sent, as Band's span always was.
 *
 * Wire (vibe_decoder_host.h skimJsonLocked_, on the decoder socket while Band runs, ≤ once a second):
 *   {"type":"band_skim","data":{"windows":[{mode,lo,hi,state}],
 *                               "signals":[{mode,id,hz,snr,wpm,wpmLo,wpmHi,q,active,set,last,call,country,settings,text}]}}
 * ★ A server without it (before beta92) sends none: no pending rows, no marks, the VFO as before — every function here
 *   takes null and returns what the box drew before.
 */
import { cwWpmRangeOf } from '../utils/cwFormat.ts';
import { DIGI_MODE_STYLE } from '../utils/digiModes.ts';
import type { KnownSignal } from '../utils/knownSignals.ts';

/** The decoder's name wherever there is room for it (the box title, the panel's own button, the tour, About). */
export const BAND_SKIMMER_NAME = 'BAND SKIMMER';
/** ★ The compact key in a decoder group (DIGI BAND's place — every key there is the widest legend's size, and phones fit
 *  three a row: scripts/test_decoder_key_grid.ts). */
export const BAND_SKIM_KEY = 'BAND SKIM';
/** A CW / RTTY row whose callsign has not been read yet. */
export const BAND_SKIM_WAITING = 'waiting for callsign';
/** An RTTY row whose speed, shift and polarity are still being worked out. */
export const BAND_SKIM_RTTY_WAITING = 'RTTY detected — waiting for its settings';

/** The box's title: "BAND SKIMMER · 40 M" (or just the name off any band). */
export function bandSkimTitle(bandName: string | null | undefined): string {
  return bandName ? `${BAND_SKIMMER_NAME} · ${bandName.toUpperCase()}` : BAND_SKIMMER_NAME;
}

export type SkimMode = 'CW' | 'RTTY';
export type SkimQ = 'high' | 'medium' | 'low' | 'poor';
export interface BandSkimWindow { mode: SkimMode; lo: number; hi: number; state: string }
export interface BandSkimSignal {
  mode: SkimMode; id: number; hz: number; snr: number; wpm: number; wpmLo?: number; wpmHi?: number; q: SkimQ;
  active: boolean; set: boolean; last: number; call: string; country: string; settings: string; text: string;
}
export interface BandSkim { windows: BandSkimWindow[]; signals: BandSkimSignal[] }

/** The most rows kept (the server sends ≤ 80), and each text's length. */
export const BAND_SKIM_MAX = 80;
export const BAND_SKIM_TEXT_MAX = 160;

/** A band_skim frame's data, checked: CW / RTTY only, finite RF, strings cut short, at most BAND_SKIM_MAX rows. null = not
 *  a band_skim frame's data. */
export function parseBandSkim(d: unknown): BandSkim | null {
  if (!d || typeof d !== 'object') return null;
  const o = d as { windows?: unknown; signals?: unknown };
  const mode = (v: unknown): SkimMode | null => (v === 'CW' || v === 'RTTY' ? v : null);
  const num = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : 0);
  const str = (v: unknown, n: number) => (typeof v === 'string' ? v.slice(0, n) : '');
  const windows: BandSkimWindow[] = [];
  for (const w of Array.isArray(o.windows) ? o.windows.slice(0, 16) : []) {
    const m = mode((w as any)?.mode), lo = num((w as any)?.lo), hi = num((w as any)?.hi);
    if (!m || !(lo > 0) || !(hi > lo) || hi - lo > 1e6) continue;
    windows.push({ mode: m, lo, hi, state: str((w as any)?.state, 12) });
  }
  const signals: BandSkimSignal[] = [];
  for (const x of Array.isArray(o.signals) ? o.signals.slice(0, BAND_SKIM_MAX) : []) {
    const m = mode((x as any)?.mode), hz = num((x as any)?.hz);
    if (!m || !(hz > 0)) continue;
    const q = (x as any)?.q;
    signals.push({
      mode: m, id: num((x as any)?.id), hz, snr: num((x as any)?.snr), wpm: num((x as any)?.wpm), ...cwWpmRangeOf(x),
      q: q === 'high' || q === 'medium' || q === 'low' ? q : 'poor',
      active: (x as any)?.active === true, set: (x as any)?.set !== false, last: num((x as any)?.last),
      call: str((x as any)?.call, 16), country: str((x as any)?.country, 40), settings: str((x as any)?.settings, 40),
      text: str((x as any)?.text, BAND_SKIM_TEXT_MAX),
    });
  }
  return { windows, signals };
}

/** ★★ THE VFO SHOWS THE SKIMMER'S WIDTH (Stuart: "the vfo needs to show the width of the skimmer too"): Band's drawn span
 *  (`band` — offsets from the dial, digiModes.ts bandPassband) widened to hold every CW / RTTY window being skimmed. A
 *  window more than 300 kHz from the dial is not this band's (a stale frame) and is left out. */
export function bandSkimPassband(band: [number, number], skim: BandSkim | null | undefined, dialHz: number): [number, number] {
  let [lo, hi] = band;
  for (const w of skim?.windows ?? []) {
    const a = w.lo - dialHz, b = w.hi - dialHz;
    if (Math.abs(a) > 300000 || Math.abs(b) > 300000) continue;
    lo = Math.min(lo, a); hi = Math.max(hi, b);
  }
  return [lo, hi];
}

/** A mode's colour on the waterfall and in the list (the spots' own table — teal CW, olive RTTY). */
export function skimColour(mode: string): string {
  return (DIGI_MODE_STYLE[mode] ?? DIGI_MODE_STYLE.CW).colour;
}

/** ★★ THE WATERFALL MARKS (Stuart: "show where CW has been detected on the waterfall, like the real CW skimmer"): every
 *  signal still heard, labelled with its callsign — or its mode until one is read — in its mode's colour; a tap tunes it.
 *  Sorted by hz (the label layer's contract). Gone signals are not marked (the waterfall shows what is there now). */
export function bandSkimLabels(skim: BandSkim | null | undefined): KnownSignal[] {
  if (!skim) return [];
  const out: KnownSignal[] = [];
  for (const s of skim.signals) {
    if (!s.active) continue;
    out.push({ hz: Math.round(s.hz), label: s.call || s.mode, dec: '', dialHz: Math.round(s.hz), mode: 'usb',
               prio: 3, src: 'skim', skim: { mode: s.mode, call: s.call } });
  }
  return out.sort((a, b) => a.hz - b.hz);
}
/** True for a band skimmer mark. */
export function isSkimLabel(s: KnownSignal | null | undefined): boolean {
  return !!s && s.src === 'skim';
}
/** What a hover or a screen reader says for a mark. */
export function skimLabelHint(s: KnownSignal): string {
  const m = s.skim;
  if (!m) return s.label;
  return `${m.mode} ${m.call ? m.call : BAND_SKIM_WAITING} · ${(s.hz / 1e3).toFixed(2)} kHz — heard by the band skimmer`;
}
/** Labels plus marks, one sorted list. */
export function withSkimLabels(list: readonly KnownSignal[] | null, marks: readonly KnownSignal[]): KnownSignal[] | null {
  if (!marks.length) return list as KnownSignal[] | null;
  return [...(list ?? []), ...marks].sort((a, b) => a.hz - b.hz);
}

/** How near (Hz) a spot and a followed signal must be to be the same station when the signal carries no call (a signal
 *  picked up afresh — the station paused, the signal was lost a moment — reads its call again before it is spotted again). */
export const BAND_SKIM_NEAR_HZ = 150;
type SpotLike = { mode: string; callsign: string; frequency: number; timestamp?: number };
/** A spot older than this (ms) no longer stands for the signal on its frequency (the skimmer's respot interval, 10 min). */
export const BAND_SKIM_SPOT_FRESH_MS = 600_000;

/** ★★ "waiting for callsign" ROWS (Stuart: "in the ALL-modes view a CW row shows its FREQUENCY and 'waiting for callsign'
 *  when no call has been detected yet"): the signals heard with no call yet, lowest frequency first. A signal with a call
 *  is a spot (the server spots it once heard twice) and is listed as one — and so is a signal with no call of its own
 *  that sits on a station already spotted (`spots`, within BAND_SKIM_NEAR_HZ): never two rows for one station.
 *  `mode`: the list's MODE filter ('ALL', 'CW' …). */
export function bandSkimPending(skim: BandSkim | null | undefined, mode = 'ALL', spots: readonly SpotLike[] = [], now = Date.now()): BandSkimSignal[] {
  if (!skim) return [];
  const f = mode.toUpperCase();
  const fresh = spots.filter((p) => (p.mode === 'CW' || p.mode === 'RTTY') && !((p.timestamp ?? now) < now - BAND_SKIM_SPOT_FRESH_MS));
  return skim.signals.filter((s) => !s.call && s.active && (f === 'ALL' || f === s.mode)
      && !fresh.some((p) => p.mode === s.mode && Math.abs(p.frequency - s.hz) <= BAND_SKIM_NEAR_HZ))
    .sort((a, b) => a.hz - b.hz || a.id - b.id);
}

/** ★★ THE MESSAGE UNDER A SPOT ROW (EXPANDED): the CW / RTTY signal this spot came from — the same mode and call, the
 *  nearest in frequency (within 500 Hz); else a signal of that mode with no call yet within BAND_SKIM_NEAR_HZ of the spot
 *  (the same station, picked up afresh). null = none (an FT8 spot, or a signal no longer followed). */
export function bandSkimSignalFor(skim: BandSkim | null | undefined, spot: SpotLike): BandSkimSignal | null {
  if (!skim || (spot.mode !== 'CW' && spot.mode !== 'RTTY')) return null;
  let best: BandSkimSignal | null = null, bestD = 500;
  for (const s of skim.signals) {
    if (s.mode !== spot.mode || s.call !== spot.callsign) continue;
    const d = Math.abs(s.hz - spot.frequency);
    if (d <= bestD) { best = s; bestD = d; }
  }
  if (best) return best;
  bestD = BAND_SKIM_NEAR_HZ;
  for (const s of skim.signals) {
    if (s.mode !== spot.mode || s.call) continue;
    const d = Math.abs(s.hz - spot.frequency);
    if (d <= bestD) { best = s; bestD = d; }
  }
  return best;
}

/** The text an expanded row reads: RTTY's "waiting for its settings" until they are found; else the message (trimmed). */
export function bandSkimText(s: BandSkimSignal): string {
  if (s.mode === 'RTTY' && !s.set) return BAND_SKIM_RTTY_WAITING;
  return s.text.trim();
}
