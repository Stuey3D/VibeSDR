// onlyMode.ts — V12 THE ONLY MODE, any whole-radio mode (Stuart, 2026-11-18: "the same thing we did for DAB earlier where it
// is the only allowed mode, we need to do for all modes as the Almere server has an ADSB only radio and I've accidentally
// closed the ADSB a couple of times meaning that it has to train the gain again"). ONE RULE, TWO READERS: the web page's
// web/client/src/onlyMode.ts is the same file; scripts/test-only-mode-clients.ts holds both to the same answers. (DAB's own
// EXIT keeps dabOnly.ts's rule, unchanged — DAB is one case of this one, and a new server says "dab" here too.)

/** The whole-radio modes with an EXIT key that moves the radio (the server's onlyModeNow list). */
export const ONLY_MODES = ['dab', 'adsb', 'ism', 'ndb', 'ais', 'avdata', 'sondes'];
/** Each one's name as its EXIT key says it. */
const KEY: Record<string, string> = { dab: 'DAB', adsb: 'ADS-B', ism: 'ISM', ndb: 'NDB', ais: 'AIS', avdata: 'DATALINK', sondes: 'SONDES' };
/** …and in a sentence (the server's onlyModeWhy). */
const WORDS: Record<string, string> = { dab: 'DAB', adsb: 'ADS-B', ism: 'ISM', ndb: 'NDB', ais: 'AIS', avdata: 'aircraft datalink', sondes: 'radiosonde' };

/** The receiver's only mode for THIS listener, or ''. `said`: the server's hwinfo `onlyMode` ('' none, '' for the
 *  signed-in admin), null when this server never sent one. An OLDER server never does, so its blocked modes decide, the
 *  way dabOnly.ts reads DAB: every demodulator an older server has (WFM, NFM, AM, USB, LSB) blocked, and exactly one of
 *  the whole-radio modes it OFFERS (`offered`) left — never for the signed-in admin. */
export function onlyModeFrom(said: string | null, blocked: Set<string>, admin: boolean, offered: Set<string>): string {
  if (said !== null) return ONLY_MODES.includes(said) ? said : '';
  if (admin) return '';
  if (!['wfm', 'nfm', 'am', 'usb', 'lsb'].every((m) => blocked.has(m))) return '';
  const left = ONLY_MODES.filter((m) => offered.has(m) && !blocked.has(m));
  return left.length === 1 ? left[0] : '';
}
/** The box's note in place of the EXIT key: "ADS-B only on this receiver". */
export function onlyModeNote(m: string): string { return `${KEY[m] ?? m.toUpperCase()} only on this receiver`; }
/** The reason, when a way out is tried anyway (Back, Escape) — the server's words. */
export function onlyModeWhy(m: string): string {
  const w = WORDS[m] ?? m;
  return `This receiver is ${w} only — its owner has set it up for ${w} listeners.`;
}
