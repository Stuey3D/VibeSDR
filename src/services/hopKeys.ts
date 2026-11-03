/* hopKeys.ts — V12 (2026-11-03): THE TUNE KEYS IN EVERY MODE WHERE THE DECODER MOVES THE RADIO ITSELF.
 *
 * ★★★ Stuart: "any modes that switch frequencies like DAB blocks do so ISM and All band digi spots need to have the drum
 *     replaced with the buttons like we do for DAB in the app." DAB has done this since 2026-10-05 (SDRScreen `vfoKeys ||
 *     dabOn`, services/dabStepper.ts): the drum is swapped for the keys, ONE press = ONE block through ONE stepper (keys,
 *     hardware wheel / trackpad, D-pad / pad / keyboard, lock-screen skip), the block is lit above the keys, and the
 *     user's own `vfoKeys` preference is never written — leaving the mode hands it back untouched.
 *  ★ ISM and DIGI BAND now do the same. Only modes where the DECODER moves the frequency (Stuart, via the coordinator,
 *    2026-11-03): DAB (blocks), ISM (bands), DIGI BAND (amateur bands). A mode the listener tunes — NAVTEX, DSC, SAME,
 *    SELCAL, single-dial digi spots and the rest — keeps the drum. ADS-B, AIS and NDB have ONE place each and nothing
 *    to step, so they keep the existing lock (noTuneNow).
 *
 * ★★ THE LABELS (Stuart, 2026-11-03, final — he had been misreading the keys and stopping one block short): ONE design
 *    for DAB blocks, ISM bands and DIGI BAND, on the app AND the web. Each had half of it: the app lit the CURRENT block
 *    over the pair ("MUX 9A"), the web named the NEIGHBOURS on the keys ("11B" over ‹, "11D" over ›). Now both do both:
 *        MUX 11C                 ISM 868                 BAND 40m          ← the current one, bold and lit
 *     [11B ‹]  ⌁  [11D ›]     [433 ‹]  ⌁  [915 ›]     [60m ‹]  ⌁  [20m ›]  ← where each key goes, small and dim
 *    Names only — DIGI BAND runs every digital decoder at once, so never "20 m FT8". The current one moves ON THE PRESS
 *    (the stepper's `show`), not when the server answers, so a quick double press can be counted; a refused step snaps
 *    back when the server's word wins (DabAskGuard). Screen reader: "Block 11C. Previous 11B, next 11D".
 * ★★ THE ENDS WRAP in the app, as DAB's always have there (dabStepper `edges` 'wrap': "the app's drum and the crown WRAP
 *    (a ring)"), so both keys always name somewhere once a radio reaches two bands or more. The web client's arrows clamp
 *    — DAB, ISM and DIGI BAND alike — and leave the key at an end blank (its syncDialTips rule, unchanged).
 * ★ The band lists are the SERVER's for THIS radio (vibeserver.json ismBands / spotBands — only the bands it reaches,
 *   the decoder-reach work). Nothing here is a fixed list.
 *
 * Pure, so scripts/test_hop_keys.ts drives it.
 */
import type { IsmBand, IsmCap, IsmReport } from './ism';
import type { SpotBand } from '../utils/digiModes';

/** Which hopping mode owns the tune keys now, or null (the user's own drum / keys). DAB first: it is entered and left
 *  by the server's word as the others are, but only one whole-radio mode is ever on at once. DIGI BAND counts only while
 *  the controls tune the VFO Band runs on (VFO A/B: the other VFO's controls tune it as ever — bandSpotsStepRef's rule). */
export type HopMode = 'dab' | 'ism' | 'spots';
export function hopKeysMode(o: { dab: boolean; ism: boolean; spotsBand: boolean; spotsOnActiveVfo: boolean }): HopMode | null {
  if (o.dab) return 'dab';
  if (o.ism) return 'ism';
  if (o.spotsBand && o.spotsOnActiveVfo) return 'spots';
  return null;
}
/** ★★ The keys are drawn when the user chose them OR a hopping mode is on. The preference is only ever READ here. */
export function hopVfoKeys(userVfoKeys: boolean, mode: HopMode | null): boolean {
  return userVfoKeys || mode !== null;
}

/** A held key walks the strip at DAB's pace (SDRScreen DAB_SWEEP_BLOCKS_PER_SEC) — slow enough to read each band. */
export const HOP_SWEEP_PER_SEC = 4;

/** Wrap an index into 0..n-1 (dabWrap's rule). */
export function hopWrap(i: number, n: number): number {
  return n > 0 ? ((i % n) + n) % n : -1;
}

/** What the keys show: the current one (lit over the pair, after its `legend`), where each key goes (`prev` on ‹, `next`
 *  on ›; absent with fewer than two to step between), and the screen reader's line: "Block 11C. Previous 11B, next 11D". */
export interface HopStrip { legend: string; prev?: string; cur: string; next?: string; spoken: string }
export function hopStrip(names: readonly string[], i: number, legend: string, what: 'Block' | 'Band',
                         curText?: string): HopStrip | undefined {
  const n = names.length;
  if (!n) return undefined;
  if (i < 0 || i >= n) {
    // Off every band (an exact frequency typed between them): the frequency, and no neighbour it could honestly name.
    const cur = curText ?? '—';
    return { legend, cur, spoken: `${what} ${cur}` };
  }
  const cur = curText ?? names[i];
  if (n < 2) return { legend, cur, spoken: `${what} ${cur}` };
  const prev = names[hopWrap(i - 1, n)], next = names[hopWrap(i + 1, n)];
  return { legend, prev, cur, next, spoken: `${what} ${cur}. Previous ${prev}, next ${next}` };
}

/* ── DAB ─────────────────────────────────────────────────────────────────────────────────────────────────────────── */
/** DAB over Band III's blocks (DAB_BLOCKS names, "5A" … "13F"); `block` = the block the keys are on (dabBlock — the
 *  stepper moves it on the press). Not known yet: "—", no neighbours. */
export function dabHopStrip(names: readonly string[], block: number): HopStrip | undefined {
  return hopStrip(names, block, 'MUX', 'Block');
}

/* ── ISM ─────────────────────────────────────────────────────────────────────────────────────────────────────────── */
/** The short name: the server's band id ("433", "868", "315", "915"). */
export function ismShortName(b: IsmBand): string { return b.id; }
/** Where the report says the radio is, as an index into the server's band list (-1 = not known yet). */
export function ismHopIndex(cap: IsmCap | undefined | null, r: IsmReport | null): number {
  if (!cap || !r?.band) return -1;
  return cap.bands.findIndex((b) => b.id === r.band);
}
/** ISM. `pending` = the band a burst of presses (or an ask the server has not answered) is on, else -1. An exact
 *  frequency put on the capture (`custom`) is shown as itself ("433.420"), with the band it sits in's neighbours. */
export function ismHopStrip(cap: IsmCap | undefined | null, r: IsmReport | null, pending: number): HopStrip | undefined {
  if (!cap || !cap.bands.length) return undefined;
  const names = cap.bands.map(ismShortName);
  if (pending >= 0 && pending < names.length) return hopStrip(names, pending, 'ISM', 'Band');
  if (!r) return { legend: 'ISM', cur: '—', spoken: 'Band not known yet' };
  return hopStrip(names, ismHopIndex(cap, r), 'ISM', 'Band', r.custom ? (r.centre / 1e6).toFixed(3) : undefined);
}

/* ── DIGI BAND ───────────────────────────────────────────────────────────────────────────────────────────────────── */
/** "20 m" → "20m" — it sits on a key, over the chevron. */
export function spotsShortName(b: SpotBand): string { return b.name.replace(/\s+/g, ''); }
/** The band the dial is on (spotBandAt's rule), as an index; -1 = between bands. */
export function spotsHopIndex(bands: readonly SpotBand[], dialHz: number): number {
  return bands.findIndex((b) => dialHz >= b.bandLo && dialHz <= b.bandHi);
}
/** ★ The band `dir` from where the keys are: `from` (an index — a burst in flight) when ≥ 0, else the dial's band. WRAPS
 *  (the app's DAB rule). Between bands: the nearest that way (stepSpotBand's rule, which the web and the server keep
 *  clamped). -1 = no bands. */
export function spotsHopNext(bands: readonly SpotBand[], dialHz: number, dir: number, from = -1): number {
  const n = bands.length;
  if (!n) return -1;
  const i = from >= 0 && from < n ? from : spotsHopIndex(bands, dialHz);
  if (i >= 0) return hopWrap(i + (dir < 0 ? -1 : 1), n);
  if (dir >= 0) { for (let k = 0; k < n; k++) if (bands[k].hz > dialHz) return k; return 0; }
  for (let k = n - 1; k >= 0; k--) if (bands[k].hz < dialHz) return k;
  return n - 1;
}
/** DIGI BAND, from the burst's band if one is in flight, else the dial's. Between bands: the dial in MHz. */
export function spotsHopStrip(bands: readonly SpotBand[], dialHz: number, pending: number): HopStrip | undefined {
  if (!bands.length) return undefined;
  const names = bands.map(spotsShortName);
  const i = pending >= 0 && pending < bands.length ? pending : spotsHopIndex(bands, dialHz);
  return hopStrip(names, i, 'BAND', 'Band', i < 0 && dialHz > 0 ? (dialHz / 1e6).toFixed(3) : undefined);
}
