// orbcommView.ts — ★★ V12 2026-11-24 WHAT AN ORBCOMM PICK LOOKS LIKE ON THE WATERFALL (both clients: the app's WaterfallView,
// the web page's waterfall.ts). Stuart: an ORBCOMM pick parked the dial on 137.500 with a narrow NFM passband — nothing showed
// the IQ slice the decoder actually searches, so it looked like it listened to an empty spot and he tuned by hand. "Its
// different to every other satellite": the decoder takes the WHOLE slice (one channel ± Doppler, or the band) and decodes
// every carrier in it at once. So while the pick runs the view shows:
//   • the slice as a shaded band, lo…hi from the server's orbcomm_status (it follows the dial on a one-pipeline radio — the
//     clients just draw what the status says), labelled "ORBCOMM · every carrier decoded here";
//   • each carrier the decoder follows at its own RF — LOCKED solid, still acquiring hollow — named once the bird is known.
// ★ Pure — scripts/test_orbcomm_app.ts. Colours: the satellite labels' sky blue (knownSignals.ts SAT_LABEL_COLOUR).
import type { OrbStatus } from '../services/orbcomm';
import { SAT_GUESS_COLOUR, type SatLabelEntry } from './knownSignals';

export const ORB_SLICE_LABEL = 'ORBCOMM · every carrier decoded here';
/** The SATELLITE box's plain line for an ORBCOMM pick. */
export const ORB_NO_TUNE = 'The whole ORBCOMM band is decoded at once — no need to tune.';
/** The overlay's ink — the satellite labels' sky blue. */
export const ORB_OVERLAY_COLOUR = '#a8d8ff';
/** ★★ b97 a carrier mark whose name is only a guess — the guessed satellite labels' amber (knownSignals.ts) */
export const ORB_GUESS_COLOUR = SAT_GUESS_COLOUR;

// ── ★★ b97 CONFIRMED vs GUESSED — ONE source of truth for the waterfall's satellite labels, the slice's carrier marks and the
//    SATELLITE card (Stuart 11-27, XCover beta96: the spectrum named TWO carriers "ORBCOMM FM116" while the card named one
//    and listed the other "not yet named"). The server's sat_labels put an ORBCOMM bird at its CATALOGUE channel + Doppler —
//    a prediction (ORBCOMM birds change channel), drawn as if it were a fact, so it landed on whatever carrier sat there.
//    Now:
//      • CONFIRMED = the decoder named THAT carrier (orbcomm_status carriers[].sat — its own position report, its learnt
//        spacecraft ID, or its channel + Doppler: what the card says). The bird's label sits on that carrier, sky blue,
//        exactly as before; on every such carrier if the decoder names several (the card lists them too).
//      • GUESSED = only predicted: amber with "?" ("ORBCOMM FM116?") — the label, the carrier mark under it, and the card's
//        unnamed row ("probably FM116?") alike.
//      • A prediction landing on a carrier the decoder names as ANOTHER bird is dropped, and a bird confirmed elsewhere
//        drops its prediction — one name is never on two carriers unless the decoder says both are that bird.
//    Other satellites' labels (ISS, Meteor …) and ORBCOMM's merged band label are untouched.
/** A prediction this close to a carrier is "on" it: half an ORBCOMM channel step (2.5 kHz) — a dongle's ppm and old elements fit. */
export const ORB_MATCH_HZ = 1250;
/** One carrier's name as everything shows it ('' = none). */
export interface OrbCarrierName { id: number; rf: number; name: string; guess: boolean }
export interface OrbLabelPlan { entries: SatLabelEntry[]; carriers: OrbCarrierName[] }
const isOrbEntry = (e: SatLabelEntry) => /^ORBCOMM\b/i.test(e.n) && !(e.b && e.lo != null && e.hi != null);
/** The plan: the sat_labels entries with ORBCOMM's reconciled against the decoder (null = no decoder running: every
 *  ORBCOMM channel label is a guess), and each of the decoder's carriers' names. Pure. */
export function orbLabelPlan(entries: readonly SatLabelEntry[] | null | undefined, st: OrbStatus | null | undefined): OrbLabelPlan {
  const carriersIn = (st?.carriers ?? []).filter((c) => c.rf > 0);
  const confirmed = carriersIn.filter((c) => !!c.sat);
  const same = (c: { sat: string; norad: number }, e: { n: string; norad: number }) => (c.norad > 0 && e.norad > 0 ? c.norad === e.norad : c.sat === e.n);
  const out: SatLabelEntry[] = [];
  const done = new Set<string>();
  for (const e of entries ?? []) {
    if (!isOrbEntry(e)) { out.push(e); continue; }
    const key = e.norad > 0 ? String(e.norad) : e.n;
    const mine = confirmed.filter((c) => same(c, e));
    if (mine.length) {   // confirmed: on the decoder's carrier(s), the ordinary label (the catalogue's channel was a guess)
      if (done.has(key)) continue;
      done.add(key);
      for (const c of mine) out.push({ ...e, hz: Math.round(c.rf), lo: undefined, hi: undefined, r: 0, g: false });
      continue;
    }
    if (confirmed.some((c) => Math.abs(c.rf - e.hz) <= ORB_MATCH_HZ)) continue;   // that carrier is another bird
    out.push({ ...e, g: true });
  }
  // a carrier the decoder named whose bird had no channel label (over the band, or not sent): named where it is
  for (const c of confirmed) {
    const key = c.norad > 0 ? String(c.norad) : c.sat;
    if (done.has(key)) continue;
    if (out.some((e) => !e.g && isOrbEntry(e) && Math.abs(e.hz - c.rf) <= ORB_MATCH_HZ && same(c, e))) continue;
    const row = st?.sats.find((b) => (c.norad > 0 && b.norad === c.norad) || b.name === c.sat);
    out.push({ id: key, n: c.sat, norad: c.norad, tx: 'ORBCOMM', hz: Math.round(c.rf), r: 0, el: row && row.el > -90 ? row.el : 0, m: 'nfm', b: false, g: false });
  }
  const guesses = out.filter((e) => e.g);
  const carriers: OrbCarrierName[] = carriersIn.map((c) => {
    if (c.sat) return { id: c.id, rf: c.rf, name: c.sat, guess: false };
    let best: SatLabelEntry | null = null;
    for (const g of guesses) if (Math.abs(g.hz - c.rf) <= ORB_MATCH_HZ && (!best || Math.abs(g.hz - c.rf) < Math.abs(best.hz - c.rf))) best = g;
    return { id: c.id, rf: c.rf, name: best ? best.n : '', guess: !!best };
  });
  return { entries: out, carriers };
}
/** The card's guess for an unnamed row's carrier ('' = none) — the same plan the waterfall draws. */
export function orbRowGuess(plan: OrbLabelPlan | null | undefined, carrierId: number): string {
  const c = plan?.carriers.find((x) => x.id === carrierId);
  return c && c.guess ? c.name : '';
}

export interface OrbOverlay {
  lo: number; hi: number;
  /** ★★ b97 `guess`: the name is only predicted — drawn amber, "FM116?" */
  carriers: { rf: number; locked: boolean; label: string; guess?: boolean }[];
  /** ★★★ beta94 the band's label when it is not ORBCOMM's — the transponder finder's (src/services/xpdr.ts) */
  title?: string;
}
/** The overlay for a status — null when there is no slice to draw (an older server, before the first status).
 *  ★★ b97 `sat` = the sat_labels entries (the guesses come from them, through orbLabelPlan — the same as the labels'). */
export function orbOverlayFrom(st: OrbStatus | null | undefined, sat?: readonly SatLabelEntry[] | null): OrbOverlay | null {
  if (!st || !st.slice) return null;
  const { lo, hi } = st.slice;
  const names = new Map(orbLabelPlan(sat, st).carriers.map((c) => [c.id, c]));
  const carriers = st.carriers
    .filter((c) => c.rf > 0)
    .map((c) => {
      const w = names.get(c.id);
      const label = w && w.name ? w.name.replace(/^ORBCOMM\s+/i, '') + (w.guess ? '?' : '') : '';
      return w && w.guess ? { rf: c.rf, locked: c.lock, label, guess: true } : { rf: c.rf, locked: c.lock, label };
    })
    .slice(0, 12);
  return { lo, hi, carriers };
}
/** A cheap identity for redraw decisions (changes when anything drawn changes, to 100 Hz). */
export function orbOverlayKey(ov: OrbOverlay | null): string {
  if (!ov) return '';
  return `${ov.title ?? ''}|${Math.round(ov.lo / 100)}|${Math.round(ov.hi / 100)}|${ov.carriers.map((c) => `${Math.round(c.rf / 100)}${c.locked ? 'L' : 'h'}${c.label}${c.guess ? 'g' : ''}`).join(',')}`;
}

export interface OrbOverlayLayout {
  /** the band's on-screen edges, clamped to the view; null = the slice is off screen */
  band: { x0: number; x1: number; loIn: boolean; hiIn: boolean } | null;
  /** where the band's label starts (left-aligned in the band, kept on screen), null = no room */
  label: { x: number; w: number } | null;
  /** each carrier on screen: x, solid (locked) or hollow, its name ('' = not yet) */
  marks: { x: number; locked: boolean; label: string; guess?: boolean }[];
}
/** Lay the overlay out for a view: `hzToX` maps RF to x (either client's), `W` is the view's width, `labelW` the label's
 *  width (measured or estimated). A label that does not fit the visible band sits at the band's left, clamped on screen. */
export function orbOverlayLayout(ov: OrbOverlay | null, hzToX: (hz: number) => number, W: number, labelW = 0): OrbOverlayLayout {
  const out: OrbOverlayLayout = { band: null, label: null, marks: [] };
  if (!ov || !(W > 0)) return out;
  const a = hzToX(ov.lo), b = hzToX(ov.hi);
  if (!Number.isFinite(a) || !Number.isFinite(b)) return out;
  const x0 = Math.max(0, Math.min(a, b)), x1 = Math.min(W, Math.max(a, b));
  if (x1 - x0 >= 1) {
    out.band = { x0, x1, loIn: a >= 0 && a <= W, hiIn: b >= 0 && b <= W };
    const w = Math.max(0, labelW);
    out.label = { x: Math.max(0, Math.min(W - w, x0 + 4)), w };
  }
  for (const c of ov.carriers) {
    const x = hzToX(c.rf);
    if (Number.isFinite(x) && x >= 0 && x <= W) out.marks.push(c.guess ? { x, locked: c.locked, label: c.label, guess: true } : { x, locked: c.locked, label: c.label });
  }
  return out;
}
