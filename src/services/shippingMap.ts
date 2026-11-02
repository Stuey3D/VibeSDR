// shippingMap.ts — V12 2026-11-02 THE SHIPPING MAP: ships (AIS), DSC positions, NAVTEX stations, warnings and the weather
// on ONE map. ONE RULE, TWO READERS: the web client (web/client/src/main.ts) and the app (SDRScreen → MapOverlay).
// docs/v12/SHIPPING-MAP.md.
//
// ★★★ Stuart (2026-11-02): "rather than separate Navtex maps why dont we integrate Navtex with DSC and AIS and have a
//     complete shipping overview. In theory a user with a multi SDR server could have one set for Navtex and another for
//     AIS and or DSC and then you will see all ships and forecasts etc on one map."
//  · The layers: SHIPS (the live AIS report while this listener's radio is in AIS, and the gallery's SHIP STORE —
//    vibe_ais_store.h — which every radio on the server writes), DSC (the calls this listener's DSC box decoded, and the
//    gallery's pinned DSC alerts from any radio), STATIONS · WARNINGS · WEATHER (the NAVTEX map's, unchanged).
//  · A layer with no data on this server is ABSENT — no toggle at all (AGENTS.md: no dead controls). The layer the map
//    was opened for (NAVTEX's MAP → the NAVTEX layers, DSC's MAP → DSC, AIS mode → SHIPS) is always there, and first.
//  · ★★ THE TIME FILTER (1 h / 24 h / 7 d / 30 d / all) IS NAVTEX'S (Stuart, 2026-11-02: "the time selections 1hr 24hr
//    etc refer to Navtex and not AIS/DSC"). It chooses the NAVTEX stations / warnings / forecasts only. Ships and DSC have
//    their own fixed rules, below: a ship is drawn while it is recent (SHIP_KEEP_SEC; faded past an hour), a DSC call for
//    a day (DSC_KEEP_SEC) — a distress or urgency call for a week (DSC_ALERT_KEEP_SEC). Latest-message-wins stays the
//    NAVTEX layers' own rule; a ship is drawn as its latest report says (live wins over the store, per MMSI).
import { esc, rangeBearing, compass8, ago, aisIcon, aisName, aisCard, aisListRows, aisSanitise,
         type AisVessel, type AisReport, type LatLon, type TrackItem, type TrackRow } from './tracks';
import { NAV_LAYERS } from './navtexWarn';
import { DSC_PIN_COLOURS, type DscCall } from './dsc';

export const SHIPPING_TITLE = 'SHIPPING MAP';

// ── the layers ─────────────────────────────────────────────────────────────────────────────────────────────────────
export type ShipLayerId = 'ships' | 'dsc' | 'stations' | 'warnings' | 'weather';
export type ShipLayers = Record<ShipLayerId, boolean>;
export type ShipFocus = 'ships' | 'dsc' | 'navtex';
export const SHIP_LAYERS: Array<{ id: ShipLayerId; label: string; words: string }> = [
  { id: 'ships', label: 'SHIPS', words: 'Show the ships (AIS)' },
  { id: 'dsc', label: 'DSC', words: 'Show the positions sent in DSC calls' },
  ...(NAV_LAYERS as Array<{ id: ShipLayerId; label: string; words: string }>),
];
/** Saved toggles (every layer on by default). The NAVTEX map's own saved toggles are read too, so a listener who switched
 *  WEATHER off there finds it off here. */
export function shipLayersOf(saved: string | null | undefined, navSaved?: string | null): ShipLayers {
  const d: ShipLayers = { ships: true, dsc: true, stations: true, warnings: true, weather: true };
  for (const s of [navSaved, saved]) {
    try { const j = s ? JSON.parse(s) : null; if (j && typeof j === 'object') for (const k of Object.keys(d) as ShipLayerId[]) if (typeof j[k] === 'boolean') d[k] = j[k]; } catch { /* default */ }
  }
  return d;
}
/** ★ What each layer has on this server now (counts, not the data). */
export interface ShipHave { ships: number; dsc: number; stations: number; warnings: number; weather: number; aisMode: boolean }
const FOCUS_LAYERS: Record<ShipFocus, ShipLayerId[]> = { ships: ['ships'], dsc: ['dsc'], navtex: ['stations', 'warnings', 'weather'] };
/** ★★ LAYERS WITH NO DATA ARE ABSENT: the layers drawn as toggles, in order — the opened layer first. The opened layer is
 *  always there (it is what the listener asked for; its empty words say why it is empty), and SHIPS always is in AIS mode. */
export function shipLayersPresent(have: ShipHave, focus: ShipFocus): ShipLayerId[] {
  const out: ShipLayerId[] = [];
  const add = (id: ShipLayerId) => { if (!out.includes(id)) out.push(id); };
  if (focus === 'navtex') { add('stations'); if (have.warnings > 0) add('warnings'); if (have.weather > 0) add('weather'); }
  else FOCUS_LAYERS[focus].forEach(add);
  if (have.ships > 0 || have.aisMode) add('ships');
  if (have.dsc > 0) add('dsc');
  if (focus !== 'navtex') {
    if (have.stations > 0) add('stations');
    if (have.warnings > 0) add('warnings');
    if (have.weather > 0) add('weather');
  }
  return out;
}
/** The toggles for the view (the map's TOGGLES row), in the order present. One layer alone draws no toggle row at all. */
export function shipToggles(present: ShipLayerId[]): Array<{ id: string; label: string; words: string }> {
  if (present.length < 2) return [];
  return present.map((id) => SHIP_LAYERS.find((l) => l.id === id)!).filter(Boolean);
}
/** Is the layer drawn? Present AND toggled on. */
export function shipOn(id: ShipLayerId, present: ShipLayerId[], layers: ShipLayers): boolean {
  return present.includes(id) && (present.length < 2 || layers[id]);
}

// ── SHIPS: the live report and the store ───────────────────────────────────────────────────────────────────────────
export interface StoredShips { now: number; count: number; total: number; off: boolean; vessels: AisVessel[]; pts: Map<number, [number, number][]> }
/** ★ /vibeserver/gallery/ais-ships.json — the live report's vessel shape, sanitised the same way. */
export function storeSanitise(j: any): StoredShips | null {
  if (!j || typeof j !== 'object' || !Array.isArray(j.vessels)) return null;
  const r = aisSanitise({ type: 'ais', vessels: j.vessels.slice(0, 2048) });
  const pts = new Map<number, [number, number][]>();
  for (const v of j.vessels.slice(0, 2048)) {
    const m = Number(v?.mmsi);
    if (!(m > 0) || !Array.isArray(v.pts)) continue;
    const p: [number, number][] = [];
    for (const q of v.pts.slice(0, 64)) if (Array.isArray(q) && isFinite(q[0]) && isFinite(q[1]) && Math.abs(q[0]) <= 90 && Math.abs(q[1]) <= 180) p.push([Number(q[0]), Number(q[1])]);
    if (p.length) pts.set(m, p);
  }
  const n = (x: unknown) => (isFinite(Number(x)) ? Number(x) : 0);
  return { now: n(j.now), count: n(j.count), total: n(j.total), off: !!j.off, vessels: r ? r.vessels : [], pts };
}
/** ★ Ships and DSC keep their OWN time rules — never the NAVTEX time filter (see the header). */
export const SHIP_KEEP_SEC = 6 * 3600;
export const DSC_KEEP_SEC = 24 * 3600;
export const DSC_ALERT_KEEP_SEC = 7 * 24 * 3600;
/** ★ The ships to draw: the live report's (every one — it is now) and the store's heard within SHIP_KEEP_SEC, not
 *  already live. Live first, then the store newest first. */
export function shipShips(live: AisReport | null, store: StoredShips | null): AisVessel[] {
  const out: AisVessel[] = live ? live.vessels.slice() : [];
  const have = new Set(out.map((v) => v.mmsi));
  if (store) for (const v of store.vessels) if (!have.has(v.mmsi) && v.seen <= SHIP_KEEP_SEC) { out.push(v); have.add(v.mmsi); }
  return out;
}
/** A ship heard over an hour ago is drawn faded (it may have moved on); the card says when. */
export function shipFaded(v: AisVessel): boolean { return v.seen > 3600; }
export function shipItems(vessels: AisVessel[], rx: LatLon | null): TrackItem[] {
  return vessels.filter((v) => v.lat != null && v.lon != null).map((v) => {
    const ic = aisIcon(v);
    const faded = shipFaded(v);
    return {
      key: `s:${v.mmsi}`, lat: v.lat as number, lon: v.lon as number,
      _ic: faded ? { ...ic, html: `<div style="opacity:.45">${ic.html}</div>` } : ic,
      _lb: aisName(v), _cd: aisCard(v, rx),
      // ★ the declutter: a ship's label above the weather's, below a distress
      _rk: (v.kind === 'sart' ? 2e13 : 5e11) - Math.min(v.seen, 1e6), _aria: `${aisName(v)}: ${v.line || v.kind}. Press Enter for the card.`,
    } as TrackItem;
  });
}
export function shipRows(vessels: AisVessel[], rx: LatLon | null): TrackRow[] {
  return aisListRows({ vessels } as AisReport, rx).map((r) => ({ ...r, key: `s:${r.key}` }));
}

// ── DSC: the box's calls and the gallery's pinned alerts ──────────────────────────────────────────────────────────
export interface DscPin { key: string; lat: number; lon: number; level: number; line: string; t: number; who: string; from: 'live' | 'gallery'; f?: string }
/** The calls this listener's DSC box decoded that sent a position (utc is milliseconds). */
export function dscPinsFromCalls(calls: readonly DscCall[]): DscPin[] {
  return calls.filter((c) => c.lat !== null && c.lon !== null).slice(0, 300).map((c) => ({
    key: `d:${c.id}:${c.rf}`, lat: c.lat as number, lon: c.lon as number, level: c.level, line: c.line, t: Math.floor(c.utc / 1000),
    who: c.distressId || c.self || '', from: 'live' as const,
  }));
}
/** ★ /vibeserver/gallery.json?kind=dsc — the pinned items ("plat"/"plon", vibe_dsc_gallery.h): distress and urgency alerts
 *  from ANY radio on the server (background decoding included). */
export function dscPinsFromGallery(j: any): DscPin[] {
  if (!j || !Array.isArray(j.items)) return [];
  const out: DscPin[] = [];
  for (const it of j.items.slice(0, 1000)) {
    if (!it || it.k !== 'dsc') continue;
    const lat = Number(it.plat), lon = Number(it.plon), t = Number(it.t);
    if (!isFinite(lat) || !isFinite(lon) || Math.abs(lat) > 90 || Math.abs(lon) > 180 || !(t > 0)) continue;
    const f = typeof it.f === 'string' && /^[A-Za-z0-9._-]{1,128}$/.test(it.f) ? it.f : '';
    const title = String(it.title ?? '').slice(0, 200);
    const lv = Number(it.lv);
    const m = /\b(\d{9})\b/.exec(title);
    out.push({ key: `d:g:${f || t}`, lat, lon, level: isFinite(lv) && lv >= 0 ? Math.min(3, lv) : 0, line: title, t, who: m ? m[1] : '', from: 'gallery', f });
  }
  return out;
}
/** ★ The two together, the same call heard both ways (the box and the gallery's alert) once: same position, within 2 min. */
export function dscPinsMerge(live: DscPin[], gal: DscPin[]): DscPin[] {
  const out = live.slice();
  for (const g of gal) if (!out.some((p) => Math.abs(p.t - g.t) <= 120 && Math.abs(p.lat - g.lat) < 1e-3 && Math.abs(p.lon - g.lon) < 1e-3)) out.push(g);
  return out.sort((a, b) => b.t - a.t);
}
/** ★ DSC's own rule: a call for a day, a distress or urgency call (level ≥ 2) for a week. `now` and `p.t` in seconds. */
export function dscInRange(pins: DscPin[], now: number): DscPin[] {
  return pins.filter((p) => now - p.t <= (p.level >= 2 ? DSC_ALERT_KEEP_SEC : DSC_KEEP_SEC));
}
const DSC_WORDS = ['call', 'safety', 'URGENCY', 'DISTRESS'];
/** ★ The DSC glyph — a map pin (not a hull, not a dot), its level's colour; distress bigger, with a ring. */
export function dscIcon(level: number): { html: string; w: number; h: number } {
  const col = DSC_PIN_COLOURS[Math.max(0, Math.min(3, level))];
  if (level >= 3) return { w: 30, h: 30, html: `<svg width="30" height="30" viewBox="0 0 30 30"><circle cx="15" cy="15" r="13" fill="none" stroke="${col}" stroke-width="2" opacity=".7"/>`
    + `<path d="M15 4 C10 4 7.5 7.5 7.5 11.5 C7.5 17 15 25 15 25 C15 25 22.5 17 22.5 11.5 C22.5 7.5 20 4 15 4 Z" fill="${col}" stroke="#000000aa" stroke-width="1.4"/>`
    + `<text x="15" y="14.5" text-anchor="middle" font-family="ui-monospace,Menlo,monospace" font-size="7" font-weight="800" fill="#fff">SOS</text></svg>` };
  return { w: 22, h: 22, html: `<svg width="22" height="22" viewBox="0 0 22 22"><path d="M11 2 C7 2 5 5 5 8.5 C5 13 11 20 11 20 C11 20 17 13 17 8.5 C17 5 15 2 11 2 Z" fill="${col}" stroke="#000000aa" stroke-width="1.3"/>`
    + `<circle cx="11" cy="8.5" r="2.4" fill="#0b1420"/></svg>` };
}
function utcText(t: number): string { return new Date(t * 1000).toISOString().slice(0, 16).replace('T', ' '); }
export function dscCard(p: DscPin, rx: LatLon | null, now: number, wrap: 'web' | 'app' = 'web'): string {
  const rows: string[] = [];
  const add = (k: string, v: string) => { if (v) rows.push(`<div class="r"><span class="k">${esc(k)}</span><span class="v">${esc(v)}</span></div>`); };
  add('Level', DSC_WORDS[Math.max(0, Math.min(3, p.level))]);
  if (p.who) add('MMSI', p.who);
  if (rx) { const rb = rangeBearing(rx, p.lat, p.lon); add('From here', `${rb.nm < 10 ? rb.nm.toFixed(1) : Math.round(rb.nm)} NM ${compass8(rb.brg)}`); }
  add('Heard', `${utcText(p.t)} UTC (${ago(Math.max(0, now - p.t))} ago)`);
  add('From', p.from === 'gallery' ? 'the gallery (any radio on this server)' : 'your DSC decoder');
  const body = `<div class="t"><b>DSC ${esc(DSC_WORDS[Math.max(0, Math.min(3, p.level))].toUpperCase())}</b></div>${rows.join('')}<div class="raw" style="white-space:pre-wrap">${esc(p.line)}</div>`;
  return wrap === 'app' ? `<div class="trkCard"><div style="white-space:normal;max-width:320px">${body}</div></div>` : `<div class="trkCard">${body}</div>`;
}
export function dscPinItems(pins: DscPin[], rx: LatLon | null, now: number, wrap: 'web' | 'app' = 'web'): TrackItem[] {
  return pins.slice(0, 300).map((p) => ({
    key: p.key, lat: p.lat, lon: p.lon, _ic: dscIcon(p.level), _lb: p.who ? `MMSI ${p.who}` : `DSC ${DSC_WORDS[Math.max(0, Math.min(3, p.level))]}`,
    _cd: dscCard(p, rx, now, wrap), _f: p.f,
    // ★ a distress or urgency call is never hidden by the declutter
    _rk: p.level >= 3 ? 3e13 : p.level === 2 ? 2.5e13 : 4e11, _aria: `DSC ${DSC_WORDS[Math.max(0, Math.min(3, p.level))]}${p.who ? ' from MMSI ' + p.who : ''}. Press Enter for the call.`,
  } as TrackItem));
}
export function dscPinRows(pins: DscPin[], rx: LatLon | null, now: number): TrackRow[] {
  return pins.slice(0, 300).map((p) => {
    const rb = rx ? rangeBearing(rx, p.lat, p.lon) : null;
    return {
      key: p.key, colour: DSC_PIN_COLOURS[Math.max(0, Math.min(3, p.level))], flag: '', name: p.who ? `MMSI ${p.who}` : 'DSC call',
      second: [DSC_WORDS[Math.max(0, Math.min(3, p.level))].toUpperCase(), p.from === 'gallery' ? 'gallery' : ''].filter(Boolean).join(' · '),
      sp: '', trend: 0, range: rb ? `${rb.nm < 10 ? rb.nm.toFixed(1) : Math.round(rb.nm)} NM` : '', brg: rb ? compass8(rb.brg) : '',
      noPos: false, age: ago(Math.max(0, now - p.t)), em: p.level >= 2,
    } as TrackRow;
  });
}
export function shipStatus(vessels: AisVessel[], store: StoredShips | null, live: AisReport | null): string {
  if (!vessels.length) return '';
  const pos = vessels.filter((v) => v.lat != null).length;
  const stored = vessels.length - (live ? live.vessels.length : 0);
  return `${vessels.length} ${vessels.length === 1 ? 'ship' : 'ships'} (${pos} on the map${live && stored > 0 ? `, ${stored} from the store` : !live && store ? ', from the store' : ''})`;
}
export function dscStatus(pins: DscPin[]): string {
  if (!pins.length) return '';
  const d = pins.filter((p) => p.level >= 3).length, u = pins.filter((p) => p.level === 2).length;
  return `${pins.length} DSC ${pins.length === 1 ? 'position' : 'positions'}${d ? ` · ${d} DISTRESS` : ''}${u ? ` · ${u} URGENCY` : ''}`;
}

// ── the KEY: the ship and DSC glyphs beside the NAVTEX hazards ─────────────────────────────────────────────────────
/** ★ Rows of the legend [glyph html, words] — the web's KEY and the app's legend draw the same list. */
export function shipLegendRows(present: ShipLayerId[]): Array<[string, string]> {
  const rows: Array<[string, string]> = [];
  const v = (o: Partial<AisVessel>) => aisIcon({ mmsi: 1, kind: 'A', seen: 0, age: 0, posAge: 0, count: 1, sog: 10, cog: 45, hdg: 45, ...o } as AisVessel).html;
  const small = (h: string) => `<span style="display:inline-block;width:20px;height:20px;overflow:hidden;vertical-align:middle;transform:scale(.7);transform-origin:center">${h}</span>`;
  if (present.includes('ships')) {
    rows.push([small(v({ family: 'cargo' })), 'Ship under way, pointing along its heading (green cargo, red tanker, blue passenger, orange fishing, cyan tug, violet leisure)']);
    rows.push([small(v({ sog: 0 })), 'Ship still (moored or at anchor)']);
    rows.push([small(v({ kind: 'aton' })), 'Aid to navigation (dashed = virtual, red = off position)']);
    rows.push([small(v({ kind: 'sart' })), 'AIS distress beacon (SART / MOB / EPIRB)']);
    rows.push([`<span style="opacity:.45">${small(v({ family: 'cargo' }))}</span>`, 'Faded: last heard over an hour ago (from the ship store)']);
  }
  if (present.includes('dsc')) {
    rows.push([small(dscIcon(3).html), 'DSC DISTRESS position']);
    rows.push([small(dscIcon(2).html), 'DSC URGENCY position']);
    rows.push([small(dscIcon(0).html), 'DSC call that sent a position']);
  }
  return rows;
}
export function shipLegendHtml(present: ShipLayerId[]): string {
  return shipLegendRows(present).map(([g, w]) => `<div class="kr">${g}<span>${esc(w)}</span></div>`).join('');
}
/** The weather arrows' KEY rows (the WEATHER layer). */
export function windLegendRows(): Array<[string, string]> {
  const arrow = (col: string, len: number, sw: number) => `<svg width="22" height="22" viewBox="0 0 22 22"><g transform="rotate(45 11 11)"><line x1="11" y1="${11 + len / 2}" x2="11" y2="${11 - len / 2 + 4}" stroke="${col}" stroke-width="${sw}" stroke-linecap="round"/><path d="M11 ${11 - len / 2} L7.5 ${11 - len / 2 + 6} L14.5 ${11 - len / 2 + 6} Z" fill="${col}"/></g></svg>`;
  return [
    [arrow('#cfe6ff', 12, 2), 'Wind arrow: points DOWNWIND (SW 6–8 points north-east); longer and heavier for a stronger wind'],
    [arrow('#ffb74d', 16, 2.6), 'Force 7 (near gale): amber'],
    [arrow('#ff5a4a', 20, 3.2), 'Gale force 8 or more: red'],
    [`<svg width="22" height="22" viewBox="0 0 22 22"><circle cx="11" cy="11" r="8" fill="none" stroke="#cfe6ff" stroke-width="2" stroke-dasharray="3 3"/></svg>`, 'Variable wind (no one direction)'],
    [`<svg width="22" height="22" viewBox="0 0 22 22"><path d="M19 11 A8 8 0 1 0 11 19" fill="none" stroke="#cfe6ff" stroke-width="2"/><path d="M11 16 L15 19 L11 22 Z" fill="#cfe6ff"/></svg>`, 'Cyclonic wind'],
    [`<svg width="22" height="22" viewBox="0 0 22 22" opacity=".5"><line x1="4" y1="18" x2="16" y2="6" stroke="#cfe6ff" stroke-width="2" stroke-dasharray="3 3"/></svg>`, 'Faint dashed arrow: the wind later (veering, backing or becoming)'],
  ];
}
