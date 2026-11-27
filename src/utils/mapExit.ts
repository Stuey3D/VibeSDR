/**
 * ★★★ THE WAY OFF A MAP — beta96 (Stuart, 2026-11-27, on Almere's ADS-B radio from the Mac app: "right now i think if i were
 * to go to almere's adsb map on the app i'd be stuck and need to force quit the app to exit the server" — and then, recorded:
 * the map had CLOSE MAP and no Servers chip; CLOSE MAP dropped him on a 1090 MHz WATERFALL "which may have caused the server
 * to exit a mode its not meant to exit"). The pure part, tested by scripts/test_map_exit.ts.
 *
 * Two rules, for every full-screen map in the app (MapOverlay):
 *  1. THE SERVERS CHIP IS ALWAYS ON THE MAP — the same component as over the waterfall, tucked to its edge tab — so the
 *     listener can always leave the server (back to the server list) and reach the server's other radios, whatever the
 *     map's own EXIT says and whatever the ask-before-tuning lock says. Leaving is not tuning: no lock ever holds it.
 *  2. A RADIO LOCKED TO A MAP MODE (the server's onlyMode — Single mode, or every other mode blocked — is one of the AIR &
 *     SEA map's modes) HAS NO RADIO SCREEN. The map is up from the moment the app knows (the radio's /vibeserver.json, read
 *     before the connect; else hwinfo), its EXIT is LEAVE SERVER, and Back leaves the server. CLOSE MAP is never offered
 *     there: it only ever led to a waterfall the listener could do nothing with.
 */

/** The whole-radio modes whose screen IS the AIR & SEA map (liveMaps.ts) — a radio locked to one of these has no other. */
export const MAP_LOCK_MODES: readonly string[] = ['adsb', 'ais', 'ndb', 'sondes', 'vormap', 'avdata'];

/** The radio's map-locked mode, or ''. `only` is the listener's only mode (onlyMode.ts onlyModeFrom; '' for the admin). */
export function mapLockedMode(only: string | null | undefined): string {
  return only && MAP_LOCK_MODES.includes(only) ? only : '';
}

/** The only mode to act on BEFORE hwinfo: the radio's /vibeserver.json said it (`pre`) — hwinfo, once it has spoken
 *  (`said` not null), is the word (the signed-in admin is told '' there and gets the radio screen back). */
export function onlyModeBeforeHwinfo(said: string | null, fromHwinfo: string, pre: string | null | undefined): string {
  return said !== null ? fromHwinfo : (pre || fromHwinfo);
}

/** What the map's EXIT key reads, and whether it leaves the server. */
export function mapExitKey(locked: boolean): { label: string; words: string; leavesServer: boolean } {
  return locked
    ? { label: 'LEAVE SERVER', words: 'Leave this server — back to the server list. The map is this receiver\'s only screen', leavesServer: true }
    : { label: '', words: '', leavesServer: false };
}

/**
 * ★★ WHAT BACK DOES ON A MAP (Android's hardware Back, a pad's or a keyboard's Back) — in this order:
 *   'collapse'   the Servers menu is open over the map: Back closes it (as over the waterfall);
 *   'leave'      a map-locked radio: Back leaves the server (there is nothing behind the map to go back to);
 *   'offer'      the map's EXIT would leave a whole-radio mode and the dial is locked (ask before tuning, an owner's scan):
 *                Back cannot leave the MODE, so it offers to leave the SERVER — never nothing (that was a force quit);
 *   'close'      otherwise: the map's own EXIT / CLOSE MAP, as before.
 */
export type MapBack = 'collapse' | 'leave' | 'offer' | 'close';
export function mapBackAction(s: { chipOpen: boolean; locked: boolean; exitLocked: boolean; canLeave: boolean }): MapBack {
  if (s.chipOpen) return 'collapse';
  if (s.locked && s.canLeave) return 'leave';
  if (s.exitLocked && s.canLeave) return 'offer';
  return 'close';
}

/** The words of the 'offer' (Back on a map whose EXIT the dial lock refuses). */
export const MAP_LEAVE_OFFER = {
  title: 'Leave this server?',
  body: (why: string) => `${why ? why.replace(/[.\s]+$/, '') + '.\n\n' : ''}Leaving the server does not change the radio — it stays as it is for everybody listening.`,
  leave: 'Leave this server',
  stay: 'Stay on the map',
} as const;

/**
 * ★ Where the Servers chip's tucked tab sits on a map: the LEFT EDGE (as over the waterfall), clear of the page's top bar and
 * the shared-tuner strip under it, about two-fifths down — the top-left corner is the legend's key (and the DIGI MODES map's
 * keys), the bottom-left the live list. Never so low that its menu (≈ 330 pt) runs off a landscape phone.
 *   h: the window height; safeTop: the top inset; mapTop: the page's top bar's bottom (0 = not reported yet); strip: the
 *   shared-tuner strip is drawn.
 */
export const MAP_CHIP_MENU_H = 330;
export function mapChipTop(h: number, safeTop: number, mapTop: number, strip: boolean): number {
  const floor = (mapTop > 0 ? mapTop : safeTop + 48) + (strip ? 40 : 0) + 10;
  const want = Math.round(h * 0.4) - 26;
  return Math.max(floor, Math.min(want, h - MAP_CHIP_MENU_H));
}

/**
 * ★★ "OTHER RADIOS ON THIS SERVER" — the Servers menu's row, only where there IS another radio (no dead row, AGENTS.md):
 * the server's door lists two or more radios. `doorBase` is the address whose list to show: the door this radio was chosen
 * at, or, for an address that names its radio (`…/r/<id>`, the server list's per-radio rows), that address without the
 * radio. Null = no row.
 */
export function otherRadiosDoor(o: { baseUrl: string; doorRadios: number; siblingRadios: number }): { doorBase: string; radioId: string } | null {
  const m = /^(.*)\/r\/([^/]+)\/?$/.exec(o.baseUrl);
  if (m) return o.siblingRadios > 1 ? { doorBase: m[1], radioId: decodeURIComponent(m[2]) } : null;
  return o.doorRadios > 1 ? { doorBase: o.baseUrl.replace(/\/+$/, ''), radioId: '' } : null;
}
/** The address part an address names its radio by, or '' (`…/r/<id>` → the server's own address). */
export function doorOf(baseUrl: string): string {
  const m = /^(.*)\/r\/[^/]+\/?$/.exec(baseUrl);
  return m ? m[1] : '';
}
