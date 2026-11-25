/**
 * vormap.ts — ★★ V12 THE VOR / ILS MAP on the page (docs/v12/VOR-MAP.md): the whole-radio VOR map mode in NDB's place and the
 * sondes' shape — the page covers itself with a TrackView (the map, the live list, EXIT VOR MAP). Each VOR or localiser heard
 * is at its published position with its ident, a VOR's radial drawn as a line from the station towards the receiver, and the
 * receiver's FIX where radials from different stations cross, with its circle of uncertainty and plain words. Heard this sweep
 * = bright; older = faded. The status says which window is being searched, how far round the sweep is, how many are heard.
 * ★ The report is parsed, and the rows, items and fix are worked out, by the app's own code (src/services/vorMap.ts — one
 *   rule, two readers). Everything escaped here.
 */
import { esc, mhz, vorAgo, vorFixLine, vorIdentWhy, vorMapRowsOrdered, vorMapStatus, vorName, vorRange, vorReading, vorStationWords, VORMAP_EMPTY,
         type VorMapReport } from '../../../src/services/vorMap';

export { vorMapItems, vorMapStatus, vorMapSanitise, vorFixLine } from '../../../src/services/vorMap';

/** ★ The live list's rows (the track view's own markup): lowest frequency first, a name earned on air, the reading, the
 *  station, the level and when last heard — faded when not heard this sweep. A row with no place on the map says so. */
export function vorMapRows(r: VorMapReport | null): string {
  const rows = vorMapRowsOrdered(r);
  const fix = vorFixLine(r);
  const head = fix ? `<div class="trkEmpty vorFixLine">${esc(fix)}</div>` : '';
  if (!rows.length) return `${head}<div class="trkEmpty">${esc(VORMAP_EMPTY)}</div>`;
  const nowS = r?.now || Date.now() / 1000;
  return head + rows.map((b) => `<div class="trkRow${b.station ? '' : ' nopos'}${b.fresh ? '' : ' vorOld'}" role="button" tabindex="0" data-key="${b.hz}"`
    + ` title="${esc([vorIdentWhy(b), b.station ? 'Show on the map' : b.ident ? 'Not in the published list, so not on the map' : 'No ident read yet, so not on the map'].filter(Boolean).join(' — '))}">`
    + `<span class="id"><b>${esc(vorName(b))}</b><small>${esc([`${mhz(b.hz)} ${b.kind === 'loc' ? 'LOC' : 'VOR'}`, vorStationWords(b), vorReading(b)].filter(Boolean).join(' · '))}</small></span>`
    + `<span class="sp">${esc(`${b.level.toFixed(0)} dB`)}</span>`
    + `<span class="rg">${b.station ? esc(vorRange(b)) : '<span class="dim">not on the map</span>'}<small>${esc(vorAgo(nowS, b.last))}</small></span></div>`).join('');
}
/** The key (TrackView legend): what the marks mean. */
export const VORMAP_LEGEND = '<div><svg width="18" height="16" viewBox="0 0 22 20"><path d="M6 1h10l5 9-5 9H6L1 10Z" fill="rgba(0,0,0,0.55)" stroke="#ffb000" stroke-width="2"/></svg> VOR, its radial towards you</div>'
  + '<div><svg width="14" height="16" viewBox="0 0 18 20"><path d="M9 1 16 18 9 14 2 18Z" fill="#ffb000" stroke="#000"/></svg> ILS localiser</div>'
  + '<div><svg width="16" height="16" viewBox="0 0 24 24"><circle cx="12" cy="12" r="7" fill="none" stroke="#7cff8a" stroke-width="2"/></svg> FIX — where radials cross, and how sure</div>'
  + '<div>Faded: not heard this sweep</div>';
export { vorMapStatus as vorStatus };
export const VORMAP_ROW_CSS = '.trkRow.vorOld{opacity:0.55}.vorFixLine{color:#7cff8a;font-weight:600}';
