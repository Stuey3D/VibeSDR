/**
 * ★★★ beta92 THE SPOTS LIST BY PAD, KEYBOARD AND REMOTE (Stuart, 2026-11-23: "make the spots navigatable by controller
 * but make it so that you dont have to scroll a list of hundreds to get to the header buttons or to the control deck").
 *
 * The decoder box's spots list (DecoderPanel: SpotRowView + the band skimmer's "waiting for callsign" SkimWaitRow) as
 * a list the highlight walks, with every way OUT one press away however deep you are:
 *
 *   ↑ / ↓      move one row; ↑ on the FIRST row goes to the header keys (never wraps to the bottom)
 *   ENTER      tunes the highlighted row, exactly as a tap does (the callsign's QRZ link stays a tap)
 *   BACK       from ANY row: to the header keys, in one press. BACK again (from the header) lets go of the box —
 *              the screen's highlight and the control deck are the next press away
 *   from the header, ↓ enters the list on its TOP row (the newest) — never a remembered deep row
 *
 * ★ THE HIGHLIGHT FOLLOWS ITS SPOT, BY KEY. Spots are newest-first, so every burst PREPENDS rows and moves every index;
 *   the highlight is held on the row's KEY (the FlatList's own keyExtractor, never an index) and its index re-found on
 *   each new list. A highlighted spot that leaves the list (a filter, the age limit) pins the highlight to the SAME
 *   PLACE, clamped to the list's end.
 * ★ No page jump: no list zone in the app or the web has one (Page Up / Down are zoom on the web). The ways out above
 *   are what keep a list of hundreds from being a trap.
 * The web page's #spotList does the same (web/client/src/gamepad.ts [data-navlist], main.ts renderSpotsRows).
 */

/** The spot's identity in the list — the FlatList keyExtractor's own (no index: a burst prepends). */
export function spotRowKey(sp: { time: number; call: string; freqHz?: number }): string {
  return `${sp.time}-${sp.call}-${sp.freqHz}`;
}
/** A band skimmer "waiting for callsign" row's identity (its React key). */
export function skimWaitKey(sg: { mode: string; id: number | string }): string {
  return `skw-${sg.mode}${sg.id}`;
}

export type SpotsZone = 'header' | 'list';
export interface SpotsNav { zone: SpotsZone; idx: number; key: string | null }
export type SpotsNavKey = 'ArrowUp' | 'ArrowDown' | 'Back' | 'Enter';
export interface SpotsStep {
  /** Where the highlight is now; null = BACK from the header: the box lets go. */
  nav: SpotsNav | null;
  /** ENTER on a row: the row to tune (its index in `keys`). */
  tune?: number;
}

const clamp = (i: number, n: number) => Math.max(0, Math.min(n - 1, i));

/** Into the list: always its top row, the newest. */
export function spotsEnterList(keys: readonly string[]): SpotsNav {
  return keys.length ? { zone: 'list', idx: 0, key: keys[0] } : { zone: 'header', idx: 0, key: null };
}

/** One key in the spots list. Left / right are the header's own (the caller's), as everywhere in the box. */
export function spotsNavKey(nav: SpotsNav, k: SpotsNavKey, keys: readonly string[]): SpotsStep {
  const n = keys.length;
  if (nav.zone === 'header') {
    if (k === 'Back') return { nav: null };
    if (k === 'ArrowDown') return { nav: spotsEnterList(keys) };
    return { nav };   // ↑ in the header: nowhere further up; ENTER presses the header key (the caller's)
  }
  if (n === 0 || k === 'Back') return { nav: { zone: 'header', idx: nav.idx, key: nav.key } };
  const at = spotsNavFollow(nav, keys);
  if (k === 'Enter') return { nav: at, tune: at.idx };
  if (k === 'ArrowUp') {
    if (at.idx <= 0) return { nav: { zone: 'header', idx: 0, key: at.key } };
    return { nav: { zone: 'list', idx: at.idx - 1, key: keys[at.idx - 1] } };
  }
  const i = clamp(at.idx + 1, n);
  return { nav: { zone: 'list', idx: i, key: keys[i] } };
}

/** A new list (spots arrived, a filter changed): the highlight stays on its spot (by key); a spot that has gone pins
 *  it to the same place, clamped. Returns `nav` itself when nothing moved. */
export function spotsNavFollow(nav: SpotsNav, keys: readonly string[]): SpotsNav {
  if (nav.zone !== 'list' || !keys.length) return nav;
  const found = nav.key != null ? keys.indexOf(nav.key) : -1;
  if (found >= 0) return found === nav.idx ? nav : { ...nav, idx: found };
  const i = clamp(nav.idx, keys.length);
  return { zone: 'list', idx: i, key: keys[i] };
}
