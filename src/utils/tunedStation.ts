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
 *    the nearest to the dial — and only a station the listener is RECEIVING: inside the receive passband (dial +
 *    bandwidthLow … dial + bandwidthHigh, the demodulator's own filter), or ON the dial (±TUNED_TOL_HZ, the VTS's
 *    "on tune" — an SSB bookmark is its dial, outside its own audio sideband). Stuart, 2026-11-15: a listener tuned
 *    slightly off who still hears the station must still get its name — the VTS's ±99 Hz is too tight for this label
 *    (the VTS keeps it). With no passband known, a mode default (tunedPassband); never wider than ±TUNED_MAX_HALF_HZ.
 *    An EiBi row counts only while it is ON AIR NOW (knownSignals.ts eibiOnAir — the server's rows carry "HHMM-HHMM" in
 *    their comment; the app's own list is already filtered to the hour and carries none, which reads as on air).
 *  ★★ A SUPPLEMENT, NEVER A REPEAT (Stuart, 2026-11-15, on the R36S render that showed "Flex FM" on the spectrum and
 *     "[User] Flex FM" under it): the spectrum's own labels (knownSignals.ts — your bookmarks, the server's, learnt FM
 *     names, EiBi's utility rows) already name what they mark. So a candidate whose own marker is DRAWN at the needle
 *     right now (`shown` — the placed labels, straight from the code that draws them: off, decluttered or off screen
 *     = not drawn) gets NO tuned label — and nothing else is offered in its place (Stuart: no "[EiBi] …" under a drawn
 *     bookmark). Instead that one spectrum label is HIGHLIGHTED (tunedPick.highlight): drawn in the tuned label's colour
 *     (stampColour), so the station you are on stands out the same way whichever names it. Same rules for when: the
 *     dial at rest, the VTS done, no live name — so the highlight never flickers through labels while tuning. EiBi's
 *     broadcast rows are never drawn there, which is what the tuned label is chiefly for.
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

/** A label the spectrum is DRAWING now (knownSignals.ts PlacedLabel.sig): its marker, its dial, and where it came from. */
export interface ShownMarker { hz: number; dialHz?: number; src: string }

/** Which drawn spectrum label names this station, or −1: a bookmark (yours or the server's) by a personal / server /
 *  learnt label, an EiBi row by an EiBi label — at its frequency, by the marker or its dial, within `tolHz`; the nearest
 *  when several do, the first on a tie. */
export function markerIndex(row: { frequency: number; source?: string }, shown: readonly ShownMarker[] | undefined,
                            tolHz: number = TUNED_TOL_HZ): number {
  if (!shown || !shown.length) return -1;
  const eibi = row.source === 'eibi';
  let best = -1, bestD = Infinity;
  for (let i = 0; i < shown.length; i++) {
    const m = shown[i];
    const mine = eibi ? m.src === 'eibi' : m.src === 'personal' || m.src === 'server' || m.src === 'learnt';
    if (!mine) continue;
    const d = Math.min(Math.abs(m.hz - row.frequency), m.dialHz != null ? Math.abs(m.dialHz - row.frequency) : Infinity);
    if (d <= tolHz && d < bestD) { best = i; bestD = d; }
  }
  return best;
}
/** Is this station already named on the spectrum? (markerIndex ≥ 0) */
export function markerCovers(row: { frequency: number; source?: string }, shown: readonly ShownMarker[] | undefined,
                             tolHz: number = TUNED_TOL_HZ): boolean {
  return markerIndex(row, shown, tolHz) >= 0;
}

/** ±Hz of the dial that counts as ON a station — the VTS's own (stations.ts VTS_ON_HZ = 99; the test checks they agree).
 *  Always a match for this label (whatever the passband), and how close a spectrum label must be to count as naming it. */
export const TUNED_TOL_HZ = 99;
/** The widest the label's passband may be either side of the dial. */
export const TUNED_MAX_HALF_HZ = 100_000;

/** The receive passband, as offsets from the dial (Hz): lo ≤ 0 ≤ hi for AM/FM, 0..+3k for USB. */
export interface Passband { lo: number; hi: number }

/**
 * The passband the label matches in: the demodulator's own (bandwidthLow / bandwidthHigh) when known and sane, else the
 * mode's default — AM family ±5 kHz, WFM ±75 kHz, NFM / FM ±6 kHz, USB-side 0…+3 kHz, LSB-side −3…0 kHz, CW ±300 Hz,
 * anything else (data) ±1.5 kHz. Clamped to ±TUNED_MAX_HALF_HZ.
 */
export function tunedPassband(mode: string | undefined, bwLow?: number | null, bwHigh?: number | null): Passband {
  const clamp = (v: number) => Math.max(-TUNED_MAX_HALF_HZ, Math.min(TUNED_MAX_HALF_HZ, v));
  if (typeof bwLow === 'number' && typeof bwHigh === 'number' && Number.isFinite(bwLow) && Number.isFinite(bwHigh) && bwHigh > bwLow)
    return { lo: clamp(bwLow), hi: clamp(bwHigh) };
  const m = String(mode || '').toLowerCase();
  let pb: Passband;
  if (m === 'wfm') pb = { lo: -75_000, hi: 75_000 };
  else if (m === 'nfm' || m === 'fm') pb = { lo: -6_000, hi: 6_000 };
  else if (m === 'usb' || m === 'ecssu') pb = { lo: 0, hi: 3_000 };
  else if (m === 'lsb' || m === 'ecssl') pb = { lo: -3_000, hi: 0 };
  else if (m === 'cw' || m === 'cwu' || m === 'cwl') pb = { lo: -300, hi: 300 };
  else if (m === 'am' || m === 'sam' || m === 'sau' || m === 'sal' || m === 'amst' || m === 'drm') pb = { lo: -5_000, hi: 5_000 };
  // ★ V12 FreeDV without edges: the side is the server's (freedv.ts), so either side of the dial
  else if (m === 'freedv') pb = { lo: -3_000, hi: 3_000 };
  else pb = { lo: -1_500, hi: 1_500 };
  return { lo: clamp(pb.lo), hi: clamp(pb.hi) };
}
/** The passband used when a caller gives none: the dial's own ±TUNED_TOL_HZ. */
const ON_TUNE: Passband = { lo: -TUNED_TOL_HZ, hi: TUNED_TOL_HZ };
/** Is a station at `hz` received by a dial at `dialHz` with passband `pb`? (Inside it, or on the dial.) */
export function inPassband(hz: number, dialHz: number, pb: Passband): boolean {
  const off = hz - dialHz;
  return Math.abs(off) <= TUNED_TOL_HZ || (off >= pb.lo && off <= pb.hi);
}
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
 * The station being received, or null: the best-ranked source inside the passband `pb` (inPassband — user → server →
 * EiBi), the nearest to the dial within
 * a source, the first listed on an exact tie. An EiBi row off air at `nowMin` (minutes past 00:00 UTC) is not a candidate;
 * a row with no name, no usable frequency or an unknown source is skipped, and so is a DAB service (mode "dab" — the
 * block's frequency names none of its services; in DAB the playing service is the live name anyway).
 */
export function tunedStation(rows: readonly TunedRow[], dialHz: number, nowMin: number,
                             pb: Passband = ON_TUNE): TunedStation | null {
  if (!(dialHz > 0)) return null;
  let best: TunedStation | null = null;
  let bestRank = 9, bestOff = Infinity;
  for (const r of rows) {
    const src = sourceOf(r.source);
    if (!src || !(r.frequency > 0) || r.mode === 'dab') continue;
    const off = Math.abs(r.frequency - dialHz);
    if (!inPassband(r.frequency, dialHz, pb)) continue;
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
  /** The receive passband (tunedPassband); none = on the dial only (±TUNED_TOL_HZ). */
  passband?: Passband;
  /** The spectrum's labels drawn right now (none / undefined = the labels are off). */
  shown?: readonly ShownMarker[];
}

/** What to draw for the station being received: the tuned label's text, OR the index into `shown` of the spectrum label
 *  that already names it (to draw in the tuned colour) — never both; neither = nothing. */
export interface TunedPick { label: string | null; highlight: number }
export function tunedPick(rows: readonly TunedRow[], dialHz: number, nowMin: number, st: TunedLabelState): TunedPick {
  if (!st.settled || st.vtsShowing || String(st.liveName ?? '').trim()) return { label: null, highlight: -1 };
  const t = tunedStation(rows, dialHz, nowMin, st.passband);
  if (!t) return { label: null, highlight: -1 };
  const i = markerIndex({ frequency: t.hz, source: t.source }, st.shown);
  return i >= 0 ? { label: null, highlight: i } : { label: tunedLabelText(t), highlight: -1 };
}
/** The label to draw at the VFO, or null for none (tunedPick's text). */
export function tunedLabel(rows: readonly TunedRow[], dialHz: number, nowMin: number, st: TunedLabelState): string | null {
  return tunedPick(rows, dialHz, nowMin, st).label;
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
