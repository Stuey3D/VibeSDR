// mapScope.ts — beta96 THE MAPS' SCOPE (docs/v12/MAP-SHARED.md): "OTHER RADIOS ON THIS SERVER" and "THIS SESSION".
// One rule for the web client (web/client/src/main.ts: the AIR & SEA map, the ISM mesh map, the AMATEUR window) and the
// app (SDRScreen → MapOverlay). Pure — scripts/test_map_scope.ts.
//
// ★★★ Stuart (11-26, via the coordinator):
//  · OTHER RADIOS ON THIS SERVER — a switch, ON by default, each viewer's own (remembered in this browser / app, never sent
//    to the server). OFF: a map on a radio shows only THIS radio's own decodes — the other SDRs' and the feeds' data are
//    hidden, and the SHOW counts follow. Not drawn when the server has only one source. Never on the standalone viewer.
//  · THIS SESSION — beside every time window on a map on a radio: only what was FIRST HEARD since this viewer opened that
//    decoder / mode in this visit (a client-side start time, reset on re-entry). Never on the standalone viewer (a live
//    history viewer with no control of a radio).
//
// ── Which radio heard it ──────────────────────────────────────────────────────────────────────────────────────────
//   The server tags what each radio writes to the shared stores with the radio's id (vibe_log_radio.h): the aircraft table
//   (aircraft.json "radios", beta96), the ship store (ais-ships.json "radios"), the NAVTEX map (navtex-stations.json
//   "radios"), the station store (stations.json "radios"), the gallery's items ("ri" — DSC alerts, the ISM node cards,
//   the remembered sondes / VOR cards). A feed writes with its own id ("feed"), so OFF hides it too. vibeserver.json says
//   this radio's id ("radioId") and how many radios the machine has ("radioCount").
//   ★ An entry with NO tag is UNKNOWN (written by an older build, or a store that keeps none yet — the NAVTEX warnings and
//     weather) and is shown either way: the logs' rule (logRadio.ts) — a filter never hides what it cannot judge.
//   ★ What this radio's own live report carries (ADS-B, AIS, the datalink, sondes, VOR, NDB, ISM, the NCDXF beacons) is
//     this radio's by definition.
//
// ── First heard ───────────────────────────────────────────────────────────────────────────────────────────────────
//   From the server where it says (aircraft.json "first", a ship's "age", a NAVTEX station's / warning's "first", a
//   station's "first", a DSC call's time, a card's time); else when THIS viewer first saw the entry in a report
//   (FirstSeen) — so an entry with no server time that was already there when the session began counts as new. Said in
//   the KEY's words, never hidden.
import { entryRadios } from '../utils/logRadio';

export const OTHER_RADIOS_LABEL = 'OTHER RADIOS ON THIS SERVER';
export const OTHER_RADIOS_WORDS = 'Show what the other radios on this server (and its feeds) heard. Off: only this radio’s own decodes. '
  + 'Your own view — nothing changes on the radio.';
/** Web localStorage / app AsyncStorage keys ("0" = off; absent = on). */
export const OTHER_RADIOS_KEY = 'vibesdr.mapOtherRadios';
export const OTHER_RADIOS_KEY_APP = 'lsv_map_other_radios';
export function otherRadiosOf(saved: string | null | undefined): boolean { return saved !== '0'; }

export const SESSION_ID = 'session';
export const SESSION_LABEL = 'THIS SESSION';
export const SESSION_WORDS = 'Only what was first heard since you opened this mode in this visit — your own view; it starts again each time you come back';
/** The ALL key a map with no time window of its own gets beside THIS SESSION. */
export const ALL_ID = 'all';
export const SCOPE_ALL_RANGE = { id: ALL_ID, label: 'ALL', words: 'Everything this map holds' };
export const SESSION_RANGE = { id: SESSION_ID, label: SESSION_LABEL, words: SESSION_WORDS };
/** A first-heard time this many seconds before the session's start still counts (the server's and our clocks, a poll). */
export const SESSION_SLACK_SEC = 2;

/** The scope a map draws under. `mine` '' = no id here (no switch); `t0` 0 = no session filter (unix seconds). */
export interface MapScope { mine: string; others: boolean; t0: number }
export const SCOPE_ALL: MapScope = { mine: '', others: true, t0: 0 };

/** ★ Is the switch drawn? Only on a map on a radio (never the viewer), only with an id, and only when the server has
 *  more than one source: 2+ radios on the machine, or an entry heard by a source that is not this radio (a feed). */
export function otherRadiosOffered(o: { mine: string; radioCount: number; viewer: boolean; seen?: Iterable<string> }): boolean {
  if (o.viewer || !o.mine) return false;
  if (o.radioCount >= 2) return true;
  for (const id of o.seen ?? []) if (id && id !== o.mine) return true;
  return false;
}
/** ★ Is THIS SESSION drawn? A map on a radio only (never the standalone viewer). */
export function sessionOffered(o: { viewer: boolean; onRadio: boolean }): boolean { return !o.viewer && o.onRadio; }

/** The radios an entry carries (`radios` array, or a gallery line's `ri` text) — [] = unknown. */
export function radiosOf(v: unknown): string[] { return entryRadios(v); }
/** ★ OTHER RADIOS: does this entry pass? Unknown (no tag) always passes; else it must list this radio. */
export function scopeRadioOk(radios: string[] | undefined | null, sc: MapScope): boolean {
  if (sc.others || !sc.mine || !radios || !radios.length) return true;
  return radios.includes(sc.mine);
}
/** ★ THIS SESSION: does an entry first heard at `first` (unix s; null = not known) pass? */
export function scopeSessionOk(first: number | null | undefined, sc: MapScope): boolean {
  if (!sc.t0) return true;
  return first != null && Number.isFinite(first) && first >= sc.t0 - SESSION_SLACK_SEC;
}
/** Both. */
export function scopeOk(radios: string[] | undefined | null, first: number | null | undefined, sc: MapScope): boolean {
  return scopeRadioOk(radios, sc) && scopeSessionOk(first, sc);
}
/** A list through the scope (the same list back, untouched, when nothing filters). */
export function scopeList<T>(list: T[], sc: MapScope, radios: (x: T) => string[] | undefined | null, first: (x: T) => number | null | undefined): T[] {
  if ((sc.others || !sc.mine) && !sc.t0) return list;
  return list.filter((x) => scopeOk(radios(x), first(x), sc));
}

/** ★ When THIS viewer first saw each entry (unix s) — the session's fallback where the server says nothing. Bounded. */
export class FirstSeen {
  private m = new Map<string, number>();
  constructor(private cap = 4096) {}
  /** The entry's first-heard time: the server's when given, else when this viewer first saw it (remembered from now). */
  first(key: string, now: number, server?: number | null): number {
    if (server != null && Number.isFinite(server)) return server;
    const had = this.m.get(key);
    if (had != null) return had;
    if (this.m.size >= this.cap) { const k = this.m.keys().next().value; if (k !== undefined) this.m.delete(k); }
    this.m.set(key, now);
    return now;
  }
  clear() { this.m.clear(); }
  get size() { return this.m.size; }
}

/** ★ THE SESSION'S START: reset when the map is (re)opened or its live mode changes (re-entry); else kept. */
export function sessionStart(prev: { mode: string; t0: number } | null, mode: string, now: number, reopened: boolean): { mode: string; t0: number } {
  if (!prev || reopened || prev.mode !== mode) return { mode, t0: now };
  return prev;
}
/** The narrowest server window that covers the session (a map's polls ask for it while THIS SESSION is picked). */
export function sessionWindow(t0: number, now: number): '1h' | '24h' | '7d' | '30d' | 'all' {
  const s = Math.max(0, now - t0) + 60;
  return s <= 3600 ? '1h' : s <= 86400 ? '24h' : s <= 7 * 86400 ? '7d' : s <= 30 * 86400 ? '30d' : 'all';
}
/** The time window's keys with THIS SESSION beside them (`ranges` [] = the map has none: ALL | THIS SESSION). */
export function withSession<R extends { id: string; label: string; words: string }>(ranges: R[], offered: boolean): Array<R | typeof SESSION_RANGE | typeof SCOPE_ALL_RANGE> {
  if (!offered) return ranges;
  return [...(ranges.length ? ranges : [SCOPE_ALL_RANGE]), SESSION_RANGE];
}

/** Every radio id the entries name (the switch's "seen", and the test's). */
export function radiosSeen(lists: Array<Array<{ radios?: string[] }> | undefined | null>): Set<string> {
  const s = new Set<string>();
  for (const l of lists) for (const x of l ?? []) for (const id of x.radios ?? []) s.add(id);
  return s;
}
/** ★ The switch's chip (a SHOW chip like the layers'): id, words, pressed. */
export const OTHER_RADIOS_CHIP = 'otherRadios';
