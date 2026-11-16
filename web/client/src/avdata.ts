/**
 * avdata.ts — ★★ V12 THE AIRCRAFT DATALINK on the page (docs/v12/AVIATION.md): ACARS and VDL2, a whole-radio mode in
 * AIS's shape — the page covers itself with a TrackView (the map, the list, EXIT). The two BANDS are the top bar's
 * segmented control (the server owns the band: a pick sends it, the next report says where the radio is). Aircraft with a
 * position are on the map; most ACARS / VDL2 traffic carries none, so the list is the heart of it.
 * ★ The report is parsed by the app's own code (src/services/aviation.ts — one rule, two readers). ★ MESSAGE TEXT shows
 *   only where the server sent it: the owner's opt-in, on the owner's own session. Everything is escaped here.
 */
import { avAge, avLine, type AvBand, type AvReport, type AvAircraft } from '../../../src/services/aviation';

function esc(s: unknown): string {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string));
}
export const avKey = (a: AvAircraft, i: number): string => a.icao || a.reg || a.flight || `row${i}`;

export function avStatus(r: AvReport | null): string {
  if (!r) return 'taking the receiver to the aircraft datalink…';
  return `${r.bandName} ${(r.hz / 1e6).toFixed(4).replace(/0+$/, '')} MHz · ${r.aircraft.length} aircraft · ${r.msgs} message${r.msgs === 1 ? '' : 's'}`
    + (r.gain != null ? ` · gain ${r.gain.toFixed(1)} dB${r.gainAuto ? ' auto' : ''}` : '') + (r.dropped ? ' · fell behind' : '');
}
export function avRows(r: AvReport | null): string {
  if (!r) return '<div class="trkEmpty">Taking the receiver to the aircraft datalink…</div>';
  if (!r.aircraft.length) return `<div class="trkEmpty">Listening on every ${esc(r.bandName)} channel. Aircraft appear here as they are heard — ACARS and VDL2 are bursts, so a quiet minute is normal; a receiver near an airport or under a flight path hears the most.</div>`;
  return r.aircraft.map((a, i) => {
    const second = [a.via, a.n > 1 ? `${a.n} messages` : ''].filter(Boolean).map(esc).join(' · ');
    const body = a.text ? `<small class="avText">${esc(a.text)}</small>` : '';
    return `<div class="trkRow${a.lat == null ? ' nopos' : ''}" role="button" tabindex="0" data-key="${esc(avKey(a, i))}">`
      + `<span class="id"><b>${esc(avLine(a))}</b><small>${second}</small>${body}</span>`
      + `<span class="sp">${esc(avAge(a.age))}</span><span class="rg">${a.lat == null ? '<span class="dim">no position</span>' : ''}</span></div>`;
  }).join('');
}
/** The map's items: the aircraft that reported a position. */
export function avItems(r: AvReport | null): Array<AvAircraft & { key: string }> {
  if (!r) return [];
  return r.aircraft.map((a, i) => ({ ...a, key: avKey(a, i) })).filter((a) => a.lat != null && a.lon != null);
}
export function avIcon(): { html: string; w: number; h: number } {
  return { html: '<svg width="20" height="20" viewBox="0 0 24 24" aria-hidden="true"><path fill="#ffd34d" stroke="#000" stroke-width="0.8" d="M12 2l1.6 7.2 7.4 3.3v1.9l-7.4-1.6-.4 5.2 2.3 1.7V21L12 20l-3.5 1v-1.3l2.3-1.7-.4-5.2L3 14.4v-1.9l7.4-3.3z"/></svg>', w: 20, h: 20 };
}
export function avCard(a: AvAircraft): string {
  return `<b>${esc(avLine(a))}</b><br>${esc(a.via)} · ${esc(avAge(a.age))}${a.text ? `<br><small>${esc(a.text)}</small>` : ''}`;
}
/** The top bar's band control (TrackView ranges). */
export function avRanges(bands: AvBand[]): Array<{ id: string; label: string; words: string }> {
  return bands.map((b) => ({ id: b.id, label: b.name, words: `${b.name}: ${b.region}` }));
}
