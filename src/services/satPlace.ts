/**
 * ★★ beta94 SATELLITE IN LOCAL HARDWARE (tester Mac Ha, Stuart 2026-11-25) — the pure half: when the SATELLITE key is
 * drawn, whether it opens on the position prompt, and the words. No React Native here (scripts/test_sat_place.ts runs it).
 *
 * Stuart: "allow it to be selected if no location and give the user the ability to enable location or add their own …
 * location, state satellites need location to calculate doppler and will not work without your location. This doesn't
 * need to be street level, nearest town will work." · "in local hardware mode the location goes nowhere anyway".
 *
 *  - The app's OWN engine (Local Hardware: a radio on this phone, or rtl_tcp / SpyServer through this phone's shim) with
 *    hwinfo's `dopplerNeed: "location"`: the key is drawn and SELECTABLE; the box opens on the prompt (ENABLE LOCATION /
 *    a town or locator). The position goes to this phone's satellite engine only (setSatObserver) — never to a server.
 *  - Anywhere else (a real server with no location): the menu's rule, unchanged — a refused key is not drawn unless on
 *    (Stuart 10-27: the reason is the owner's to fix, not the listener's).
 */
import type { SatCap } from './satellites';

/** Where Local Hardware's position came from: the user typed a town / locator (kept on this phone; it wins), or the
 *  phone's own location. */
export type LocalPlaceSource = 'entered' | 'gps';
export type LocalPlace = { lat: number; lon: number; label?: string; src: LocalPlaceSource };

/** Does this session run on the app's OWN engine (the shim in this phone's app process)? A VibeServer reached over the
 *  local path carries `localHost`; Local Hardware (USB, rtl_tcp, SpyServer) does not. Android only: iOS has no orbit
 *  data supply in its engine (vsSatSupply), so a position there would only swap "no location" for "no orbit data". */
export function satOwnEngine(o: { os: string; isLocal: boolean; localHost?: string | null; hasNative: boolean }): boolean {
  return o.os === 'android' && o.isLocal && !o.localHost && o.hasNative;
}

/** The engine says it has no position, and THIS app can give it one. */
export function satNeedsPlace(cap: SatCap | null | undefined, ownEngine: boolean): boolean {
  return ownEngine && !!cap && !cap.ok && cap.need === 'location';
}

/** The SATELLITE key: drawn? / greyed with a reason? / opens on the position prompt?
 *  `avail` false = not drawn. `why` set = drawn dimmed (a press says it). `prompt` = selectable, opens on the prompt. */
export function satOffer(o: { cap: SatCap | null | undefined; on: boolean; isVibe: boolean; ownEngine: boolean }):
    { avail: boolean; why?: string; prompt: boolean } {
  const { cap, on } = o;
  if (!cap || !o.isVibe) return { avail: false, prompt: false };   // ★ no `doppler` = no key at all (no dead controls)
  const prompt = satNeedsPlace(cap, o.ownEngine);
  if (cap.ok || prompt) return { avail: true, prompt };            // ★ the prompt is the way in — not dimmed
  // ★ the menu's rule (Stuart 10-27): refused = not drawn, unless on (so it can always be left)
  return on ? { avail: true, why: undefined, prompt: false } : { avail: false, prompt: false };
}

/** The prompt's words (Stuart's, in plain English). */
export const SAT_PLACE_TITLE = 'SATELLITES NEED YOUR LOCATION';
export const SAT_PLACE_TEXT =
  'Satellites need your location to work out when each one passes over and to correct its Doppler shift — ' +
  'without it, satellite mode cannot work. It does not need to be exact: the nearest town is enough.';
export const SAT_PLACE_PRIVATE =
  'It stays on this phone: it is used for satellites, spot distances and the maps in Local Hardware, and is never ' +
  'sent to a server or published.';
export const SAT_PLACE_ENTRY_HINT = 'Town or locator (e.g. Northampton or IO92nh)';
export const SAT_PLACE_DENIED =
  'VibeSDR is not allowed to use this phone’s location. Allow it in the phone’s Settings (Apps › VibeSDR › ' +
  'Permissions › Location), or type the nearest town or your locator instead.';
export const SAT_PLACE_NOT_FOUND = 'That place could not be found. Try the nearest town, or a locator such as IO92nh (a locator works offline).';

/** "Position: Northampton · IO92nh (entered)" / "Position: this phone's location · IO92nh". */
export function satPlaceLine(p: LocalPlace | null | undefined, grid: string): string {
  if (!p) return '';
  return p.src === 'entered'
    ? `Position: ${p.label && p.label.toUpperCase() !== grid.toUpperCase() ? `${p.label} · ` : ''}${grid} (entered)`
    : `Position: this phone’s location · ${grid}`;
}

/** Which position Local Hardware uses: one the user ENTERED wins (their choice); else the phone's location. */
export function localPlaceChoose(entered: { lat: number; lon: number; label?: string } | null | undefined,
                                 gps: { lat: number; lon: number } | null | undefined): LocalPlace | null {
  if (entered && Number.isFinite(entered.lat) && Number.isFinite(entered.lon)) return { ...entered, src: 'entered' };
  if (gps && Number.isFinite(gps.lat) && Number.isFinite(gps.lon)) return { lat: gps.lat, lon: gps.lon, src: 'gps' };
  return null;
}
