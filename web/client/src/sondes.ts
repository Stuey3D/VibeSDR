/**
 * sondes.ts — ★★ V12 RADIOSONDES on the page (docs/v12/RS41.md): the SONDES whole-radio mode in the aircraft
 * datalink's shape — the page covers itself with a TrackView (the map, the list, EXIT). The BANDS (400–402 / 402–404 /
 * 404–406 MHz, and the whole 400–406 where the radio runs that wide) are the top bar's segmented control (the server owns
 * the band: a pick sends it, the next report says where the radio is). Each sonde is on the map at its last position; a
 * chosen one draws its track (the report carries it — no extra request).
 * ★ The report is parsed by the app's own code (src/services/sondes.ts — one rule, two readers). Everything escaped here.
 */
import { sondeAge, sondeDetail, sondeEmpty, sondeLine, sondeStatus, type Sonde, type SondeBand, type SondeReport } from '../../../src/services/sondes';

function esc(s: unknown): string {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string));
}
export { sondeStatus };
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
/** A balloon: falling ones point down (the climb rate's sign is the one thing a glance must get). */
export function sondeIcon(s?: Sonde): { html: string; w: number; h: number } {
  const down = !!s && s.climb != null && s.climb < -2;
  const fill = down ? '#ff8a4d' : '#7fd4ff';
  return { html: `<svg width="20" height="22" viewBox="0 0 20 22" aria-hidden="true"><ellipse cx="10" cy="8" rx="6.5" ry="7.2" fill="${fill}" stroke="#000" stroke-width="0.9"/>`
    + `<path d="M10 15.2v4.3" stroke="#000" stroke-width="1"/><rect x="7.6" y="19" width="4.8" height="2.6" fill="#fff" stroke="#000" stroke-width="0.7"/>`
    + (down ? '<path d="M10 4.5v6M7.4 8.3 10 11l2.6-2.7" stroke="#000" stroke-width="1.2" fill="none"/>' : '') + '</svg>', w: 20, h: 22 };
}
export function sondeCard(s: Sonde): string {
  const pos = s.lat != null ? `<br>${s.lat.toFixed(5)}, ${s.lon!.toFixed(5)}${s.speed != null ? ` · ${s.speed.toFixed(1)} m/s towards ${Math.round(s.heading ?? 0)}°` : ''}` : '';
  return `<b>${esc(sondeLine(s))}</b><br>${esc(sondeDetail(s))}${pos}<br><small>${s.n} frame${s.n === 1 ? '' : 's'} · last ${esc(sondeAge(s.age))}${s.sats != null ? ` · ${s.sats} satellites` : ''}</small>`;
}
/** The chosen sonde's track for the map (TrackView's trail): from the newest report, never a request. */
export function sondeTrail(r: SondeReport | null, key: string): [number, number][] {
  const s = r?.sondes.find((x) => x.serial === key);
  return s ? s.track.map((p) => [p[0], p[1]] as [number, number]) : [];
}
/** The top bar's band control. */
export function sondeRanges(bands: SondeBand[]): Array<{ id: string; label: string; words: string }> {
  return bands.map((b) => ({ id: b.id, label: b.name.replace(' MHz', ''), words: `Search ${b.name}` }));
}
