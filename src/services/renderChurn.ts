/* ★★★ "IS THIS ACTUALLY NEWS?" — the equality tests that keep a repeated message from re-rendering
 *  the whole radio screen (power audit, 2026-10-01: the owner's iPhone 17 Pro Max ran warm while
 *  listening, builds 357/358).
 *
 *  SDRScreen is one component with ~300 useState hooks; a fresh object handed to ANY of them
 *  re-renders all of it, plus every child whose props are not stable. Several feeds deliver the
 *  SAME content again and again as a NEW object:
 *   · RDS arrives at ~1 Hz on a VibeServer — the server change-detects on BER and signal level
 *     too, neither of which this screen draws — so every second built an identical liveStation.
 *   · the learned-bookmark poll (30 s) and the UberSDR bookmark poll (10 min) answer with a new
 *     array whose contents almost never change.
 *  React only skips a render when the value is the SAME object (Object.is), so the cure is to hand
 *  it back the previous object when nothing it draws has changed. Pure and dependency-free so the
 *  rules are tested (scripts/test_renderChurn.ts).
 *
 *  ✗ These are NOT for anything a listener is waiting on: they only ever answer "unchanged" when
 *    every drawn field is identical, so a real change is never held back. */

/** The live station label — RDS, DAB or a DMR caller — exactly the fields SDRScreen stores. */
export interface LiveStationLike {
  name?: string; psRaw?: string; text?: string; badge?: string;
  countryIso?: string; pi?: string; ecc?: number;
  /** DAB service ID as hex ("C6D6") — DAB's identity in the VTS line, never an RDS PI. */
  sid?: string;
}

/** True when two live-station labels would draw identically. Every field is compared, so adding
 *  a field to the label without adding it here would make a change to it invisible — hence the
 *  explicit list rather than a generic shallow compare that a new field could slip past. */
export function sameLiveStation(a: LiveStationLike, b: LiveStationLike): boolean {
  return a.name === b.name && a.psRaw === b.psRaw && a.text === b.text && a.badge === b.badge
    && a.countryIso === b.countryIso && a.pi === b.pi && a.ecc === b.ecc && a.sid === b.sid;
}

/** Hand back `prev` when `next` says nothing new — for a functional setState. */
export function keepIfSameStation<T extends LiveStationLike>(prev: T, next: T): T {
  return sameLiveStation(prev, next) ? prev : next;
}

/** True when two lists hold the same entries in the same order, compared field by field (one level
 *  deep — a bookmark is a flat record). Order matters: the lists are drawn and searched in order. */
export function sameFlatList<T extends object>(a: readonly T[], b: readonly T[]): boolean {
  if (a === b) return true;
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    const x = a[i] as Record<string, unknown>, y = b[i] as Record<string, unknown>;
    if (x === y) continue;
    const kx = Object.keys(x), ky = Object.keys(y);
    if (kx.length !== ky.length) return false;
    for (const k of kx) if (x[k] !== y[k]) return false;
  }
  return true;
}

/** The lightning badge (`lx`, sent with EVERY spectrum frame): the same when it would draw the same.
 *  What is drawn is "⚡ STORMS" and a spoken label with the rate and the seconds since the last
 *  strike, both rounded to whole numbers — so that is the resolution compared, and no finer.
 *  ★ Keep in step with ControlsBar's accessibilityLabel if it ever shows more. */
export function sameStorms(a: { rate: number; ago: number } | null,
                           b: { rate: number; ago: number } | null): boolean {
  if (a === b) return true;
  if (!a || !b) return false;
  return Math.round(a.rate) === Math.round(b.rate) && Math.round(a.ago) === Math.round(b.ago);
}

/** The minute a clock showing HH:MM is on. A 1 Hz timer only needs to re-render when this moves:
 *  the seconds are not drawn, so the other 59 renders a minute drew the same pixels. */
export function minuteKey(ms: number): number {
  return Math.floor(ms / 60_000);
}
