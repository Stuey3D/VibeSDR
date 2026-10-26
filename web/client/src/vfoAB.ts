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

export interface WatchChan {
  id: number; freq: number; mode: string; bw: number;
  sqlOn: boolean; sql: number; open: boolean; since: number; lvl: number; parked: boolean;
}
export interface WatchState {
  max: number; lo: number; hi: number; stereo: boolean; monoOut: boolean;
  chans: WatchChan[]; refused?: string; note?: string;
}

const num = (v: unknown, d = 0): number => (typeof v === 'number' && Number.isFinite(v) ? v : d);

/** The server's `watch` message, or null if it is not one. Every field defaulted — an odd value never throws. */
export function watchSanitise(m: any): WatchState | null {
  if (!m || m.type !== 'watch') return null;
  const chans: WatchChan[] = Array.isArray(m.chans) ? m.chans.slice(0, 8).map((c: any) => ({
    id: num(c?.id, 1), freq: num(c?.freq), mode: typeof c?.mode === 'string' ? c.mode : 'am', bw: num(c?.bw, 10000),
    sqlOn: c?.sqlOn === true, sql: num(c?.sql, -100), open: c?.open !== false, since: num(c?.since),
    lvl: num(c?.lvl, -200), parked: c?.parked === true,
  })).filter((c: WatchChan) => c.freq > 0) : [];
  return {
    max: num(m.max, -1), lo: num(m.lo), hi: num(m.hi), stereo: m.stereo !== false, monoOut: m.monoOut === true, chans,
    refused: typeof m.refused === 'string' ? m.refused : undefined,
    note: typeof m.note === 'string' && m.note ? m.note.slice(0, 200) : undefined,
  };
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
