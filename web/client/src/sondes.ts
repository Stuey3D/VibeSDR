/**
 * sondes.ts — ★★ V12 RADIOSONDES on the page (docs/v12/RS41.md): the SONDES whole-radio mode in the aircraft
 * datalink's shape — the page covers itself with a TrackView (the map, the list, EXIT). The BANDS (400–402 / 402–404 /
 * 404–406 MHz, and the whole 400–406 where the radio runs that wide) are the top bar's segmented control (the server owns
 * the band: a pick sends it, the next report says where the radio is). Each sonde is on the map at its last position; a
 * chosen one draws its track (the report carries it — no extra request).
 * ★ The report is parsed by the app's own code (src/services/sondes.ts — one rule, two readers). Everything escaped here.
 */
import { sondeAge, sondeCard, sondeDetail, sondeEmpty, sondeIcon, sondeLine, sondeStatus, sondeTrail, type Sonde, type SondeBand, type SondeReport } from '../../../src/services/sondes';

function esc(s: unknown): string {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string));
}
export { sondeStatus, sondeIcon, sondeCard, sondeTrail };
export function sondeRows(r: SondeReport | null): string {
  if (!r || !r.sondes.length) return `<div class="trkEmpty">${esc(sondeEmpty(r))}</div>`;
  return r.sondes.map((s) => `<div class="trkRow${s.lat == null ? ' nopos' : ''}" role="button" tabindex="0" data-key="${esc(s.serial)}">`
    + `<span class="id"><b>${esc(sondeLine(s))}</b><small>${esc(sondeDetail(s))}</small></span>`
    + `<span class="sp">${esc(sondeAge(s.age))}</span><span class="rg">${s.lat == null ? '<span class="dim">no position</span>' : ''}</span></div>`).join('');
}
/** The map's items: the sondes with a position. */
export function sondeItems(r: SondeReport | null): Array<Sonde & { key: string }> {
  if (!r) return [];
  return r.sondes.filter((s) => s.lat != null && s.lon != null).map((s) => ({ ...s, key: s.serial }));
}
/** The top bar's band control. */
export function sondeRanges(bands: SondeBand[]): Array<{ id: string; label: string; words: string }> {
  return bands.map((b) => ({ id: b.id, label: b.name.replace(' MHz', ''), words: `Search ${b.name}` }));
}
