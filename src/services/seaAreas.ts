// seaAreas.ts — V12 2026-11-02 THE SHIPPING MAP's SEA AREAS: each forecast area OUTLINED (thin, chart-like), its NAME in
// small capitals at its label point, and — while a forecast for it is current — tinted AMBER at force 7 and RED at gale 8+.
// ONE RULE, TWO READERS: the web client (web/client/src/main.ts shipCompose) and the app (SDRScreen → MapOverlay), both
// through tracklayer.js (an item per area: its name is the icon, its outline the shape). docs/v12/SHIPPING-MAP.md.
//
// ★★ Stuart (2026-11-02): "can we add the shipping lanes and shipping zones to the map too?" … "worldwide if possible" …
//    "the things like Dogger, Wight Plymouth etc".
//  · The data: src/utils/seaAreas.generated.ts (scripts/gen-sea-areas.mjs — the Met Office's corner points for the UK
//    areas, NOAA NWS / ECCC / DWD zones beyond; the sources and licences are in the generator's header and on the map's
//    attribution line, seaAreaAttribution()).
//  · ★ ITS OWN TOGGLE, "SEA AREAS", beside WEATHER (not inside it): the outlines are geography — useful with no forecast
//    at all (which area is that ship in?) — and the forecasts are weather. Switching the arrows off must not take Dogger
//    away, and the reverse. The TINT is the weather's (it says what the forecast says), so it follows WEATHER.
//  · ABSENT WHERE THERE IS NO DATA: the toggle is drawn only when an area lies within reach of this map (the receiver,
//    or a forecast's area) — a server in Japan gets no SEA AREAS toggle that would draw nothing (seaAreasNear).
//  · DECLUTTER: names drop before outlines — a UK / offshore area's name from zoom 4 / 5, a US coastal zone's outline
//    from zoom 6 and its name from 8; a name yields to every other icon and label (tracklayer _y), and an area with a
//    current forecast shows no name of its own (the forecast's arrow label carries it).
import { SEA_AREA_ROWS, SEA_AREA_SOURCES } from '../utils/seaAreas.generated.ts';

export interface SeaArea {
  id: string; kind: 's' | 'o' | 'c'; src: string; name: string; lat: number; lon: number;
  rings: Array<Array<[number, number]>>;   // [lat, lon] — Leaflet's order
  bbox: [number, number, number, number];  // minLat, minLon, maxLat, maxLon
}
/** Google's polyline decoding at 0.001° (the generator's encodeRing). */
export function decodeRing(s: string): Array<[number, number]> {
  const out: Array<[number, number]> = []; let i = 0, lat = 0, lon = 0;
  const one = () => { let r = 0, sh = 0, b; do { b = s.charCodeAt(i++) - 63; r |= (b & 0x1f) << sh; sh += 5; } while (b >= 0x20 && i < s.length + 1); return r & 1 ? ~(r >> 1) : r >> 1; };
  while (i < s.length) { lat += one(); lon += one(); out.push([lat / 1000, lon / 1000]); }
  return out;
}
let cache: SeaArea[] | null = null;
/** ★ Every area, decoded once (lazily — a map that never shows the layer never pays for it). */
export function seaAreas(): SeaArea[] {
  if (cache) return cache;
  cache = SEA_AREA_ROWS.map(([id, kind, src, name, lat, lon, enc]) => {
    const rings = enc.split(' ').filter(Boolean).map(decodeRing).filter((r) => r.length >= 3);
    let a = 90, b = 180, c = -90, d = -180;
    for (const r of rings) for (const [y, x] of r) { if (y < a) a = y; if (x < b) b = x; if (y > c) c = y; if (x > d) d = x; }
    return { id, kind: kind as SeaArea['kind'], src, name, lat, lon, rings, bbox: [a, b, c, d] as [number, number, number, number] };
  });
  return cache;
}
const byId = () => { const m = new Map<string, SeaArea>(); for (const a of seaAreas()) m.set(a.id, a); return m; };
let idx: Map<string, SeaArea> | null = null;
export function seaAreaById(id: string): SeaArea | undefined { return (idx ??= byId()).get(id); }

function km(aLat: number, aLon: number, bLat: number, bLon: number): number {
  const r = Math.PI / 180, dLat = (bLat - aLat) * r, dLon = (bLon - aLon) * r;
  const s = Math.sin(dLat / 2) ** 2 + Math.cos(aLat * r) * Math.cos(bLat * r) * Math.sin(dLon / 2) ** 2;
  return 12742 * Math.asin(Math.min(1, Math.sqrt(s)));
}
/** ★ How far from a point an area is still "this map's" — an offshore / shipping area well beyond the horizon (a NAVTEX
 *  forecast reaches ~400 NM), a coastal zone only near. Measured to the label point and to the box's nearest corner. */
export const SEA_REACH_KM = { s: 2600, o: 2000, c: 700 } as const;
function near(a: SeaArea, lat: number, lon: number): boolean {
  const lim = SEA_REACH_KM[a.kind];
  const [y0, x0, y1, x1] = a.bbox;
  const cy = Math.max(y0, Math.min(y1, lat)), cx = Math.max(x0, Math.min(x1, lon));
  return km(lat, lon, cy, cx) <= lim || km(lat, lon, a.lat, a.lon) <= lim;
}
/** ★ The areas this map draws: those within reach of the receiver (or of any point given — a forecast's area, a ship),
 *  plus every area a forecast names. With nothing to go on (no position, no forecast) the UK's and the Germans'
 *  (the NAVTEX map's home waters) — never all ~750 at once. */
export function seaAreasNear(points: Array<{ lat: number; lon: number }>, forecastIds: Iterable<string> = []): SeaArea[] {
  const want = new Set(forecastIds);
  const pts = points.filter((p) => isFinite(p.lat) && isFinite(p.lon)).slice(0, 8);
  return seaAreas().filter((a) => want.has(a.id) || (pts.length ? pts.some((p) => near(a, p.lat, p.lon)) : a.src === 'uk' || a.src === 'de'));
}

// ── the weather's tint ───────────────────────────────────────────────────────────────────────────────────────────
/** The bits of a current forecast the tint reads (navtexWarn.ts NavFc, structurally). */
export interface SeaFc { gale: number; fmax: number; wind?: { sev: number; sev2: number } | null }
/** ★ 2 = RED (a gale, 8+, forecast or warned — the forecast's gale words, its force, or its arrows), 1 = AMBER (force 7),
 *  0 = no tint. */
export function seaSeverity(f: SeaFc | null | undefined): 0 | 1 | 2 {
  if (!f) return 0;
  if (f.gale >= 8 || f.fmax >= 8 || (f.wind && (f.wind.sev === 2 || f.wind.sev2 === 2))) return 2;
  if (f.fmax === 7 || (f.wind && (f.wind.sev === 1 || f.wind.sev2 === 1))) return 1;
  return 0;
}
export const SEA_COLOURS = { line: '#8fb4d4', amber: '#ffb74d', red: '#ff5a4a', name: '#a9c7e2' } as const;

// ── the items (tracklayer.js) ───────────────────────────────────────────────────────────────────────────────────
const esc = (s: string) => s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string));
/** ★ The name on the map: the area's own (UK, ECCC, DWD); a US zone's long description gives way to its zone code. */
export function seaLabel(a: SeaArea): string {
  if (a.src === 'us') return a.kind === 'o' && a.name.length <= 26 ? a.name : a.id.slice(3).toUpperCase();
  return a.name;
}
const SRC_WORDS: Record<string, string> = { uk: 'Met Office shipping forecast area', us: 'NOAA NWS marine zone', ca: 'Environment Canada marine forecast area', de: 'DWD coastal area' };
export const SEA_NAME_ZOOM = { s: 4, o: 5, c: 8 } as const;
export const SEA_LINE_ZOOM = { s: 0, o: 3, c: 6 } as const;
export function seaNameIcon(a: SeaArea): { html: string; w: number; h: number } {
  const t = seaLabel(a).toUpperCase();
  const w = Math.round(t.length * 6.1 + 6), h = 12;
  return { w, h, html: `<div style="width:${w}px;height:${h}px;font:600 8.5px/12px ui-monospace,Menlo,Consolas,monospace;letter-spacing:.09em;color:${SEA_COLOURS.name};`
    + `text-align:center;white-space:nowrap;text-shadow:0 0 3px #04101c,0 0 2px #04101c;opacity:.9">${esc(t)}</div>` };
}
export function seaCard(a: SeaArea, sev: 0 | 1 | 2, hasFc: boolean, wrap: 'web' | 'app' = 'web'): string {
  const rows = [`<div class="t"><b>${esc(a.name.toUpperCase())}</b></div>`, `<div class="r"><span class="k">Area</span><span class="v">${esc(SRC_WORDS[a.src] || 'sea area')}</span></div>`];
  if (a.src === 'us') rows.push(`<div class="r"><span class="k">Zone</span><span class="v">${esc(a.id.slice(3).toUpperCase())}</span></div>`);
  rows.push(`<div class="r"><span class="k">Forecast</span><span class="v">${esc(hasFc ? (sev === 2 ? 'GALE in force — the forecast is on the map' : sev === 1 ? 'near gale (7) — the forecast is on the map' : 'current — the forecast is on the map') : 'none heard for this area')}</span></div>`);
  const body = rows.join('');
  return wrap === 'app' ? `<div class="trkCard"><div style="white-space:normal;max-width:320px">${body}</div></div>` : `<div class="trkCard">${body}</div>`;
}
/** ★ The items for the SEA AREAS layer. `fcBy`: the current (not stale) forecast per area id — the WEATHER layer's;
 *  pass an empty map when WEATHER is off (no tint, every name shown). */
export function seaAreaItems(areas: SeaArea[], fcBy: Map<string, SeaFc>, wrap: 'web' | 'app' = 'web'): any[] {
  return areas.map((a) => {
    const f = fcBy.get(a.id) ?? null, sev = seaSeverity(f);
    const col = sev === 2 ? SEA_COLOURS.red : sev === 1 ? SEA_COLOURS.amber : SEA_COLOURS.line;
    const ic = f ? { html: '', w: 1, h: 1 } : seaNameIcon(a);   // ★ a forecast's own label names the area
    return {
      key: `a:${a.id}`, lat: a.lat, lon: a.lon, _ic: ic, _lb: '', _cd: seaCard(a, sev, !!f, wrap),
      _sh: { kind: 'area', rings: a.rings, sig: a.id, colour: col, weight: sev ? 1.6 : 0.8, opacity: sev ? 0.9 : 0.5, fillOpacity: sev === 2 ? 0.2 : sev === 1 ? 0.15 : 0,
             minZoom: sev ? 0 : SEA_LINE_ZOOM[a.kind] },
      // ★ an area with a forecast: its own marker never shows (the forecast's is the button), only its outline
      _mz: f ? 99 : SEA_NAME_ZOOM[a.kind], _y: true, _nofit: true, _rk: -1e6 + (a.kind === 's' ? 2 : a.kind === 'o' ? 1 : 0),
      // ★ the NAME first ("Dogger sea area"): a forecast's marker says "Sea area Dogger: …" — the two never read alike
      _aria: `${a.name} sea area${sev === 2 ? ': gale in force' : sev === 1 ? ': near gale' : ''}. Press Enter for the card.`,
    };
  });
}
/** ★ The tinted areas take the forecast's arrow to their own label point (inside the outline, as drawn) — the met table's
 *  hand-placed point for the UK sits where its outline did not exist yet. */
export function seaLabelPointFor(id: string): { lat: number; lon: number } | null {
  const a = seaAreaById(id);
  return a ? { lat: a.lat, lon: a.lon } : null;
}

// ── the KEY and the attribution ─────────────────────────────────────────────────────────────────────────────────
export function seaLegendRows(weather: boolean): Array<[string, string]> {
  const box = (stroke: string, fill: string, sw: number) => `<svg width="22" height="16" viewBox="0 0 22 16"><path d="M2 3 L19 2 L20 13 L3 14 Z" fill="${fill}" stroke="${stroke}" stroke-width="${sw}"/></svg>`;
  const rows: Array<[string, string]> = [[box(SEA_COLOURS.line, 'none', 1), 'Sea area (the shipping forecast’s, or the national marine forecast’s): its outline and name']];
  if (weather) {
    rows.push([box(SEA_COLOURS.amber, 'rgba(255,183,77,.25)', 1.6), 'Sea area tinted amber: force 7 (near gale) forecast']);
    rows.push([box(SEA_COLOURS.red, 'rgba(255,90,74,.3)', 1.6), 'Sea area tinted red: a gale (force 8 or more) forecast or warned']);
  }
  return rows;
}
// ── ONE COMPOSITION, TWO READERS ──────────────────────────────────────────────────────────────────────────────────
/** The met report's areas, as both readers hold them (navtexWarn.ts NavArea, structurally). */
interface MetAreaLike { id: string; lat: number; lon: number; stale: boolean; fc: SeaFc[] }
/** ★ The areas this map has (for the toggle's presence): near the receiver, plus the forecasts' own. */
export function seaAreasFor(rx: { lat: number; lon: number } | null, met: { areas: MetAreaLike[] } | null): SeaArea[] {
  const ids = met ? met.areas.map((a) => a.id) : [];
  return seaAreasNear(rx ? [rx] : [], ids);
}
/** ★ The SEA AREAS layer's items. `weatherOn`: the WEATHER layer is drawn — the current forecasts tint their areas and
 *  carry the names; with WEATHER off the areas are plain outlines with their names. */
export function seaCompose(rx: { lat: number; lon: number } | null, met: { areas: MetAreaLike[] } | null, weatherOn: boolean, wrap: 'web' | 'app' = 'web'): { items: any[]; attribution: string; count: number } {
  const areas = seaAreasFor(rx, met);
  const fcBy = new Map<string, SeaFc>();
  if (weatherOn && met) for (const a of met.areas) if (!a.stale && a.fc.length) fcBy.set(a.id, a.fc[0]);
  return { items: seaAreaItems(areas, fcBy, wrap), attribution: seaAttribution(areas), count: areas.length };
}
/** ★ A forecast's arrow at its area's label point (inside the outline) when the area has one. */
export function seaPlaceMet<T extends { id: string; lat: number; lon: number }>(areas: T[]): T[] {
  return areas.map((a) => { const p = seaLabelPointFor(a.id); return p ? { ...a, lat: p.lat, lon: p.lon } : a; });
}
export function seaLegendHtml(weather: boolean, attribution: string): string {
  return seaLegendRows(weather).map(([g, w]) => `<div class="kr">${g}<span>${esc(w)}</span></div>`).join('')
    + (attribution ? `<div class="kr" style="opacity:.7;font-size:10px"><span>${esc(attribution)}</span></div>` : '');
}
/** The words for the map's attribution line: the sources whose areas are drawn. */
export function seaAttribution(areas: SeaArea[]): string {
  const srcs = [...new Set(areas.map((a) => a.src))].filter((s) => SEA_AREA_SOURCES[s]);
  return srcs.length ? 'Sea areas: ' + srcs.map((s) => SEA_AREA_SOURCES[s].attribution).join(' · ') : '';
}
