/**
 * vfoAB.ts — V12 VFO A/B on the web page (GitHub discussion #57; docs/v12/VFO-AB.md). The pure parts: the server's watch
 * state, read defensively, and which EAR each VFO is in.
 *
 * ★★★ THE EARS FOLLOW FREQUENCY, NOT THE LETTER (Stuart, 2026-10-27): "at a glance the lower is the left vfo and the
 *     higher is the right and it will always be that way". The server does the mixing (vibe_watch.h PanMixer); this file
 *     only labels the needles to match it — lower L, higher R, within 5 Hz both ("LR"), and no tags at all with STEREO off
 *     or a mono output (both in both ears). The 100 Hz swap hysteresis the server applies is mirrored here so a needle
 *     wobbling over the other does not flicker its tag either.
 * ★ Pure: no DOM, so scripts/test-web-vfo-ab.mjs can run it under plain node.
 */

// ★★★ The A ⟷ B balance is the two-server rule — imported, one copy (src/services/dualMix.ts; the server mirrors it in vibe_watch.h).
import { clampBalance } from '../../../src/services/dualMix';

export interface WatchChan {
  id: number; freq: number; mode: string; bw: number;
  sqlOn: boolean; sql: number; open: boolean; since: number; lvl: number; parked: boolean;
  /** ★ Round 3: muted in this listener's mix (absent from an older server = false). */
  mute: boolean;
}
export interface WatchState {
  max: number; lo: number; hi: number; stereo: boolean; monoOut: boolean;
  /** ★ Round 3: VFO A muted in this listener's mix. */
  muteA: boolean;
  /** ★ Round 3: the radio moves to hold both VFOs (a personal receiver) — lo/hi are where it is now, not a wall. */
  follow: boolean;
  /** ★★ Decoders run on either VFO ("vfo":"B" on the decoder socket). Absent (a round-3 server) = A only. */
  decB: boolean;
  /** ★★★ The listener's A ⟷ B balance as the server holds it (−1 all A … +1 all B); null from an older server. */
  bal: number | null;
  chans: WatchChan[]; refused?: string; note?: string;
}

const num = (v: unknown, d = 0): number => (typeof v === 'number' && Number.isFinite(v) ? v : d);

/** The server's `watch` message, or null if it is not one. Every field defaulted — an odd value never throws. */
export function watchSanitise(m: any): WatchState | null {
  if (!m || m.type !== 'watch') return null;
  const chans: WatchChan[] = Array.isArray(m.chans) ? m.chans.slice(0, 8).map((c: any) => ({
    id: num(c?.id, 1), freq: num(c?.freq), mode: typeof c?.mode === 'string' ? c.mode : 'am', bw: num(c?.bw, 10000),
    sqlOn: c?.sqlOn === true, sql: num(c?.sql, -100), open: c?.open !== false, since: num(c?.since),
    lvl: num(c?.lvl, -200), parked: c?.parked === true, mute: c?.mute === true,
  })).filter((c: WatchChan) => c.freq > 0) : [];
  return {
    max: num(m.max, -1), lo: num(m.lo), hi: num(m.hi), stereo: m.stereo !== false, monoOut: m.monoOut === true,
    muteA: m.muteA === true, follow: m.follow === true, decB: m.decB === true,
    bal: typeof m.bal === 'number' && Number.isFinite(m.bal) ? clampBalance(m.bal) : null, chans,
    refused: typeof m.refused === 'string' ? m.refused : undefined,
    note: typeof m.note === 'string' && m.note ? m.note.slice(0, 200) : undefined,
  };
}

/* ★★★ DECODE ON EITHER VFO (Stuart, 2026-10-27: "That seems like a weird limitations surely it doesnt matter which VFO is
 *  listen and which is decode?"). A decoder — or digital spots, Single or Band — runs on the VFO SELECTED when it is
 *  started; what the decoder box tunes moves THAT VFO; selecting the other VFO later moves nothing. A server that cannot
 *  decode B (round 3: no `decB` in its watch state) keeps the old rule — the controls go to A first. */
/** Which VFO a decoder started now runs on, and whether the controls must go to A first (an older server). */
export function decStartVfo(abOn: boolean, sel: 'A' | 'B', decB: boolean): { vfo: 'A' | 'B'; selectA: boolean } {
  if (!abOn || sel === 'A') return { vfo: 'A', selectA: false };
  return decB ? { vfo: 'B', selectA: false } : { vfo: 'A', selectA: true };
}
/** The 'DEC' tag on a VFO's needle label and squelch row: that VFO is decoding (a decoder or spots run on it). Only with
 *  two VFOs — with one there is nothing to tell apart. */
export function decTagFor(v: 'A' | 'B', abOn: boolean, decoderOn: 'A' | 'B' | null, spotsOn: 'A' | 'B' | null): string {
  return abOn && (decoderOn === v || spotsOn === v) ? 'DEC' : '';
}

export const SAME_HZ = 5;      // vibe_watch.h kSameHz
export const SWAP_HZ = 100;    // vibe_watch.h kSwapHz

/** Which ear each of A and B is in. `prevAFirst` = A was the LEFT one last time (the hysteresis). Returns tags for the
 *  needles ('L' | 'R' | 'LR' | '') and the new order. */
export function earTags(aHz: number, bHz: number, stereo: boolean, prevAFirst: boolean | null):
  { a: string; b: string; aFirst: boolean } {
  let aFirst = prevAFirst ?? aHz <= bHz;
  if (aFirst && aHz > bHz + SWAP_HZ) aFirst = false;
  else if (!aFirst && bHz > aHz + SWAP_HZ) aFirst = true;
  if (!stereo) return { a: '', b: '', aFirst };
  if (Math.abs(aHz - bHz) <= SAME_HZ) return { a: 'LR', b: 'LR', aFirst };
  return aFirst ? { a: 'L', b: 'R', aFirst } : { a: 'R', b: 'L', aFirst };
}

/** The brightness of the ACTIVE and the OTHER needle, cross-faded over `ms` since the last switch. Stuart: the active one
 *  full, the other "dims only SLIGHTLY … ~70–80 %". */
export const DIM = 0.75;
export function needleAlpha(sinceSwitchMs: number, ms = 200): { active: number; other: number } {
  const t = Math.max(0, Math.min(1, sinceSwitchMs / ms));
  const e = 0.5 - 0.5 * Math.cos(Math.PI * t);
  return { active: DIM + (1 - DIM) * e, other: 1 - (1 - DIM) * e };
}
