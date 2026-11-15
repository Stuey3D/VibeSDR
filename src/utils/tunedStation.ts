/**
 * tunedStation.ts — THE TUNED STATION'S LABEL AT THE VFO. ONE pure module, shared by the app (SDRScreen → WaterfallView)
 * and the web client (web/client/src/main.ts → waterfall.ts); the R36S's C++ twin is handheld-ui/src/tuned_label.h, line
 * for line. Tested by scripts/test_tuned_station.ts (and the handheld's golden, handheld-ui/test/run_tuned_label.sh).
 *
 * ★★★ STUART (2026-11-15): "Right now the VTS shows the station name then disappears unless it is DAB/DRM/RDS. When the
 *     VTS disappears can we have a label on the VFO with the current station name. We don't show EiBi bookmarks on the
 *     spectrum as they would swamp it, but we could have the current tuned one shown."
 *         [EiBi] China Radio Int     [Server] Radio Caroline     [User] Flex FM
 *  ★★ SO: ONE label, at the VFO, for the station the dial is ON — never a list across the spectrum (knownSignals.ts keeps
 *     EiBi's broadcast rows off the spectrum for exactly that reason, and this does not change it).
 *  ★ WHEN (tunedLabel): only once the VTS has stopped showing the name, only when there is no LIVE name (RDS PS, DAB
 *    service label, DRM label — the VTS holds those on screen itself), and never while the dial moves: the caller says
 *    `settled` (TUNED_SETTLE_MS without a change of frequency) so a sweep never flickers a name per channel.
 *  ★ WHICH (tunedStation): the listener's own bookmarks first, then the server's, then the EiBi schedule — within each,
 *    the nearest — and only within TUNED_TOL_HZ, the VTS's own "on tune" (stations.ts VTS_ON_HZ, the web's VTS_ON_HZ).
 *    An EiBi row counts only while it is ON AIR NOW (knownSignals.ts eibiOnAir — the server's rows carry "HHMM-HHMM" in
 *    their comment; the app's own list is already filtered to the hour and carries none, which reads as on air).
 *  ★ WHERE (tunedLabelX): beside the needle, flipped to its other side near the right edge and kept on screen at both.
 *    The vertical slot is each client's: just below its LISTEN readout, never over it.
 */
import { eibiOnAir } from './knownSignals.ts';

export type TunedSource = 'user' | 'server' | 'eibi';

/** One candidate: a bookmark or a schedule row, in the shape every client's lists already have. */
export interface TunedRow {
  frequency: number;
  name: string;
  source?: string;
  /** A DAB service ("dab") is skipped: its frequency is its multiplex's, shared with every other service in the block. */
  mode?: string;
  /** EiBi: the server's "HHMM-HHMM · E" (time + language) — the time decides whether it is on air now. */
  comment?: string;
}

export interface TunedStation { source: TunedSource; name: string; hz: number }

/** ±Hz of the dial that counts as ON a station — the VTS's own (stations.ts VTS_ON_HZ = 99; the test checks they agree). */
export const TUNED_TOL_HZ = 99;
/** How long the dial must rest before the label is worked out again (ms). Longer than the VTS's 250 ms check, so a
 *  bookmark landed on pops in the VTS first and the label never flashes up ahead of it. */
export const TUNED_SETTLE_MS = 600;
/** The most characters of a name drawn (the tag not counted) — a long EiBi name ends in "…". */
export const TUNED_MAX_CHARS = 28;

/** The tags, exactly as Stuart wrote them. */
export const TUNED_TAG: Readonly<Record<TunedSource, string>> = { user: '[User]', server: '[Server]', eibi: '[EiBi]' };
const RANK: Readonly<Record<TunedSource, number>> = { user: 0, server: 1, eibi: 2 };

function sourceOf(s: string | undefined): TunedSource | null {
  return s === 'user' || s === 'server' || s === 'eibi' ? s : null;
}

/**
 * The station the dial is ON, or null: the best-ranked source within `tolHz` (user → server → EiBi), the nearest within
 * a source, the first listed on an exact tie. An EiBi row off air at `nowMin` (minutes past 00:00 UTC) is not a candidate;
 * a row with no name, no usable frequency or an unknown source is skipped, and so is a DAB service (mode "dab" — the
 * block's frequency names none of its services; in DAB the playing service is the live name anyway).
 */
export function tunedStation(rows: readonly TunedRow[], dialHz: number, nowMin: number,
                             tolHz: number = TUNED_TOL_HZ): TunedStation | null {
  if (!(dialHz > 0)) return null;
  let best: TunedStation | null = null;
  let bestRank = 9, bestOff = Infinity;
  for (const r of rows) {
    const src = sourceOf(r.source);
    if (!src || !(r.frequency > 0) || r.mode === 'dab') continue;
    const off = Math.abs(r.frequency - dialHz);
    if (!(off <= tolHz)) continue;
    const name = String(r.name ?? '').trim();
    if (!name) continue;
    const rank = RANK[src];
    if (rank > bestRank || (rank === bestRank && off >= bestOff)) continue;
    if (src === 'eibi' && !eibiOnAir(r.comment, nowMin)) continue;
    best = { source: src, name, hz: r.frequency };
    bestRank = rank; bestOff = off;
  }
  return best;
}

/** "[EiBi] China Radio Int" — the name cut to TUNED_MAX_CHARS code points with "…". */
export function tunedLabelText(st: TunedStation): string {
  const cps = Array.from(st.name);
  const name = cps.length > TUNED_MAX_CHARS ? cps.slice(0, TUNED_MAX_CHARS - 1).join('').trimEnd() + '…' : st.name;
  return `${TUNED_TAG[st.source]} ${name}`;
}

export interface TunedLabelState {
  /** The live station name the VTS is holding — RDS PS, DAB service, DRM label. Non-empty = no label. */
  liveName?: string;
  /** The VTS is showing a station name right now (its station line, before it fades). True = no label. */
  vtsShowing: boolean;
  /** The dial has rested TUNED_SETTLE_MS (no tuning, no drag). False = no label. */
  settled: boolean;
}

/** The label to draw at the VFO, or null for none. */
export function tunedLabel(rows: readonly TunedRow[], dialHz: number, nowMin: number, st: TunedLabelState,
                           tolHz: number = TUNED_TOL_HZ): string | null {
  if (!st.settled || st.vtsShowing || String(st.liveName ?? '').trim()) return null;
  const t = tunedStation(rows, dialHz, nowMin, tolHz);
  return t ? tunedLabelText(t) : null;
}

/**
 * Where the label's box starts (its left edge, px): `gap` right of the needle when the box fits, else `gap` left of it;
 * then kept within [margin, width − boxW − margin] (a box wider than the view starts at the margin). `boxW` is the whole
 * box, padding included.
 */
export function tunedLabelX(needleX: number, boxW: number, width: number, gap = 6, margin = 2): number {
  let x = needleX + gap;
  if (x + boxW > width - margin) x = needleX - gap - boxW;
  return Math.max(margin, Math.min(width - margin - boxW, x));
}

/** Minutes past 00:00 UTC — the clock EiBi's times are in. */
export function utcMinutes(d: Date = new Date()): number {
  return d.getUTCHours() * 60 + d.getUTCMinutes();
}
