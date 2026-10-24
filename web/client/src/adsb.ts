/**
 * adsb.ts — the web client's ADS-B box list and its map window (V12, docs/v12/ADSB.md).
 *
 * ★★ THE PAGE'S HALF ONLY. Entering and leaving the mode, the locked controls and the box itself are
 *    main.ts's (beside DAB, whose rules they follow); the aircraft on the map are web/mapkit/adsblayer.js,
 *    the one layer the app draws with too. This file turns a report into the list the box shows, and
 *    writes the map window that hosts the layer.
 * ★ Everything in a report came off the air or over the network: every string is escaped where it meets
 *   HTML, and the callsign is cut to the characters a callsign has.
 */
import type { AdsbAircraft, AdsbReport } from './spectrum';
import type { MapGLKit } from './mapgl';

export type LatLon = { lat: number; lon: number };

const D2R = Math.PI / 180;
/** Great-circle distance in nautical miles and initial bearing — the same formulas as adsblayer.js. */
export function rangeBearing(rx: LatLon, lat: number, lon: number): { nm: number; brg: number } {
  const p1 = rx.lat * D2R, p2 = lat * D2R, dp = (lat - rx.lat) * D2R, dl = (lon - rx.lon) * D2R;
  const h = Math.sin(dp / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2;
  const nm = 2 * 6371008.8 * Math.asin(Math.min(1, Math.sqrt(h))) / 1852;
  const y = Math.sin(dl) * Math.cos(p2), x = Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl);
  return { nm, brg: (Math.atan2(y, x) / D2R + 360) % 360 };
}
const COMPASS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];

function esc(v: unknown): string {
  return String(v ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!));
}
function num(v: unknown): number | null { const n = Number(v); return v == null || v === '' || !isFinite(n) ? null : n; }
export function cleanFlight(v: unknown): string { return String(v ?? '').replace(/[^A-Za-z0-9 -]/g, '').trim().slice(0, 10); }
export function cleanHex(v: unknown): string { return String(v ?? '').replace(/[^~0-9A-Fa-f]/g, '').slice(0, 7).toLowerCase(); }

/** Flight levels from the transition altitude, feet below — AircraftPanel.tsx's rule (18 000 ft). */
export function altText(alt: AdsbAircraft['alt_baro']): string {
  if (alt === 'ground') return 'GND';
  const ft = num(alt);
  if (ft == null) return '—';
  return ft >= 18000 ? `FL${String(Math.round(ft / 100)).padStart(3, '0')}` : `${(Math.round(ft / 100) * 100).toLocaleString('en-GB')} ft`;
}
function emergency(a: AdsbAircraft): boolean {
  const sq = String(a.squawk ?? '');
  return sq === '7500' || sq === '7600' || sq === '7700' || (!!a.emergency && a.emergency !== 'none' && a.emergency !== 0 && a.emergency !== '0');
}

/** "14 aircraft · 9 on map · 212 msg/s" — the box header's status. */
export function adsbStatus(r: AdsbReport | null): string {
  if (!r) return 'listening on 1090 MHz…';
  const n = r.aircraft.length;
  const pos = r.aircraft.filter((a) => num(a.lat) != null && num(a.lon) != null).length;
  const rate = num(r.msgRate);
  // ★ Short enough for one line beside MAP and EXIT on a wide box ("on the map · 1458 msg/s" wrapped).
  const r1 = rate == null ? '' : rate >= 1000 ? ` · ${(rate / 1000).toFixed(1)}k msg/s` : ` · ${Math.round(rate)} msg/s`;
  return `${n} aircraft${n ? ` · ${pos} on map` : ''}${r1}`;
}

/** ★ The list, nearest first when the receiver's position is known (the app's AircraftPanel order),
 *  else the most recently heard first. Aircraft without a position sort after the located ones. */
export function adsbRows(r: AdsbReport, rx: LatLon | null) {
  const rows = r.aircraft.map((a) => {
    const lat = num(a.lat), lon = num(a.lon);
    const rb = rx && lat != null && lon != null ? rangeBearing(rx, lat, lon) : null;
    return { a, hex: cleanHex(a.hex), rb, located: lat != null && lon != null };
  }).filter((x) => x.hex);
  rows.sort((p, q) => {
    if (p.rb && q.rb) return p.rb.nm - q.rb.nm;
    if (p.rb) return -1;
    if (q.rb) return 1;
    if (p.located !== q.located) return p.located ? -1 : 1;
    return (num(p.a.seen) ?? 99) - (num(q.a.seen) ?? 99);
  });
  return rows;
}

export function adsbListHtml(r: AdsbReport | null, rx: LatLon | null, mapOpen: boolean): string {
  if (!r) return '<div class="adsbEmpty">Listening on 1090 MHz — aircraft appear here as they are heard.</div>';
  const rows = adsbRows(r, rx);
  if (!rows.length) return '<div class="adsbEmpty">No aircraft heard yet. Each one appears as its transponder is decoded.</div>';
  return rows.map(({ a, hex, rb, located }) => {
    const call = cleanFlight(a.flight);
    const vs = num(a.baro_rate);
    const trend = vs == null || Math.abs(vs) < 300 ? '' : vs > 0 ? '<i class="up" title="Climbing">▲</i>' : '<i class="dn" title="Descending">▼</i>';
    const gs = num(a.gs);
    const sq = String(a.squawk ?? '').replace(/[^0-7]/g, '').slice(0, 4);
    const where = rb ? `${rb.nm < 10 ? rb.nm.toFixed(1) : Math.round(rb.nm)} NM <span class="brg">${COMPASS[Math.round(rb.brg / 45) % 8]}</span>`
      : located ? '<span class="dim">located</span>' : '<span class="dim">no position</span>';
    const em = emergency(a);
    return `<div class="adsbRow${em ? ' em' : ''}${located ? '' : ' nopos'}" data-hex="${esc(hex)}"${mapOpen && located ? ' title="Show on the map"' : ''}>`
      + `<span class="id"><b>${esc(call || hex.toUpperCase())}</b><small>${esc(hex.toUpperCase())}${sq ? ` · ${esc(sq)}` : ''}</small></span>`
      + `<span class="alt">${esc(altText(a.alt_baro))}${trend}</span>`
      + `<span class="spd">${gs != null ? `${Math.round(gs)} kt` : ''}</span>`
      + `<span class="rng">${where}</span>`
      + '</div>';
  }).join('');
}

/* ══ THE MAP WINDOW ═══════════════════════════════════════════════════════════════════════════════
 * ★★ The spots map's mechanism (openSpotsMap in main.ts): a same-origin about:blank window, opened inside
 *    the click, written once we know whether the GPU map is on offer, fed by postMessage. The GPU map's
 *    files come from THIS server's /mapgl/ (absolute — the window's own location says nothing); the layer
 *    and the Leaflet fallback's renderer arrive as STRINGS, so the window never depends on a file an
 *    older install might lack.
 * ★ The basemap is the 'aero' profile — airports and runways, as on the HFDL map. */
export interface AdsbMapOpts {
  kit: MapGLKit | null;
  rx: LatLon | null;
  layerJs: string;
  vibemapJs: string;
  leaflet: { js: string; css: string; jsSri: string; cssSri: string } | null;
  dataBase: string;
  openerOrigin: string;
  title: string;
}

export function adsbMapHtml(o: AdsbMapOpts): string {
  // Closing tags assembled at runtime — a literal one would end the page's OWN inline script.
  const ES = '<' + '/script>';
  const ST = '<' + '/style>';
  const safe = (v: unknown) => JSON.stringify(v).replace(/</g, '\\u003c');
  const k = o.kit;
  const glHead = k ? `<link rel="stylesheet" href="${esc(k.base)}vendor/maplibre-gl.css">
<script src="${esc(k.base)}vendor/maplibre-gl.js">${ES}
<script src="${esc(k.base)}vendor/pmtiles.js">${ES}
<script src="${esc(k.base)}vendor/vibemapgl.js">${ES}` : '';
  return `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${esc(o.title)}</title>
${glHead}
<style>
  :root{--amber:#ffb833;--dim:rgba(255,184,51,0.55);--bg:rgba(9,6,2,0.94);--bdr:rgba(255,160,0,0.30);--hi:#ffe566}
  html,body{height:100%;margin:0;background:#090602;font:12px ui-monospace,"SF Mono",Menlo,Consolas,monospace;color:var(--amber)}
  #wrap{display:flex;flex-direction:column;height:100%}
  #top{display:flex;align-items:center;gap:10px;padding:7px 12px;background:var(--bg);border-bottom:1px solid var(--bdr);flex:0 0 auto;min-width:0}
  #ttl{letter-spacing:2px;color:rgba(255,184,51,0.8);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;min-width:0}
  #top .sp{flex:1 1 auto}
  #cnt{color:var(--hi);font-size:14px}
  #cntl{color:var(--dim);white-space:nowrap}
  #main{position:relative;flex:1 1 auto;min-height:0}
  #m{position:absolute;inset:0}
  #note{position:absolute;left:50%;bottom:22px;transform:translateX(-50%);z-index:20;background:var(--bg);border:1px solid var(--bdr);
    border-radius:8px;padding:8px 14px;color:var(--amber);letter-spacing:1px;max-width:calc(100% - 40px);text-align:center;display:none}
  .maplibregl-ctrl-group{background:var(--bg)!important;border:1px solid var(--bdr)}
  .maplibregl-ctrl-group button+button{border-top:1px solid rgba(255,160,0,0.20)}
  .maplibregl-ctrl-group button .maplibregl-ctrl-icon{filter:invert(0.7) sepia(1) saturate(3) hue-rotate(-10deg);opacity:.7}
  .maplibregl-ctrl-attrib,.maplibregl-ctrl-attrib.maplibregl-compact{background:rgba(9,6,2,0.7);color:var(--dim);font-size:10px}
  .maplibregl-ctrl-attrib a{color:var(--dim)}
  .maplibregl-ctrl-attrib-button{filter:invert(0.7) sepia(1) saturate(3) hue-rotate(-10deg);opacity:.55}
  .leaflet-container{background:#123049}
  .leaflet-bar a{background:var(--bg);color:var(--amber);border-color:var(--bdr)}
  .leaflet-control-attribution{background:rgba(9,6,2,0.7)!important;color:var(--dim)}
  .leaflet-control-attribution a{color:var(--dim)}
  @media (max-width:420px){#ttl{letter-spacing:1px}#cntl{display:none}}
${ST}
<div id="wrap">
  <div id="top"><span id="ttl">ADS-B AIRCRAFT MAP</span><span class="sp"></span><span id="cnt">0</span><span id="cntl">on the map</span></div>
  <div id="main"><div id="m"></div><div id="note"></div></div>
</div>
<script>
const GL = ${k ? safe(k) : 'null'};
let RX = ${safe(o.rx)};
const LAYER_SRC = ${safe(o.layerJs)};
const VIBEMAP_SRC = ${safe(o.vibemapJs)};
const LEAFLET = ${safe(o.leaflet)};
const GL_DATA_BASE = ${safe(o.dataBase)};
const OPENER_ORIGIN = ${safe(o.openerOrigin)};
const PREFS_KEY = 'vibesdr.adsbMapPrefs';
let layer = null, pending = null;
function note(t) { const n = document.getElementById('note'); n.textContent = t || ''; n.style.display = t ? 'block' : 'none'; }
function loadPrefs() { try { const v = JSON.parse(localStorage.getItem(PREFS_KEY) || 'null'); return v && typeof v === 'object' ? v : null; } catch (e) { return null; } }
function savePrefs(p) { try { localStorage.setItem(PREFS_KEY, JSON.stringify(p)); } catch (e) { /* private window: the toggles still work for this visit */ } }
function evalSrc(src) { const el = document.createElement('script'); el.textContent = src; document.head.appendChild(el); }
function count(list) {
  let n = 0;
  for (const a of list || []) if (a && a.lat != null && a.lon != null) n++;
  document.getElementById('cnt').textContent = String(n);
}
function attach(map) {
  evalSrc(LAYER_SRC);
  if (!window.VibeAdsbLayer) { note('The aircraft layer did not load.'); return; }
  layer = VibeAdsbLayer.create(map, { receiver: RX, prefs: loadPrefs(), onPrefs: savePrefs, theme: 'dark' });
  if (pending) { layer.update(pending); pending = null; }
}
function bootGPU() {
  if (!GL || !window.VibeMapGL || !window.maplibregl) return false;
  try {
    const load = (path, kind) => fetch(path).then((r) => {
      if (!r.ok) { console.error('GPU map: ' + path + ' answered HTTP ' + r.status); return null; }
      return kind === 'text' ? r.text() : r.arrayBuffer();
    }).catch((e) => { console.error('GPU map: ' + path + ' could not be fetched', e); return null; });
    const vm = VibeMapGL.createNow(document.getElementById('m'), {
      style: GL.style, load, base: GL.base, rangeBase: GL.base, detail: GL.detail, profile: 'aero',
      center: RX ? [RX.lon, RX.lat] : [0, 30], zoom: RX ? 6 : 1.5,
    });
    if (!vm) return false;
    vm.ready.catch((e) => console.error('GPU map: the style did not load', e));
    vm.setNight(true);          // ★ the greyline on every map, as on the HFDL map
    vm.map.addControl(new maplibregl.NavigationControl({ showCompass: false }), 'top-left');
    attach(vm.map);
    return true;
  } catch (e) { console.error('GPU map unavailable — using the Leaflet map', e); return false; }
}
function bootLeaflet() {
  if (!LEAFLET) { note('The map could not be loaded on this server.'); return; }
  const css = document.createElement('link'); css.rel = 'stylesheet'; css.href = LEAFLET.css; css.integrity = LEAFLET.cssSri;
  document.head.appendChild(css);
  const js = document.createElement('script'); js.src = LEAFLET.js; js.integrity = LEAFLET.jsSri;
  js.onload = () => {
    evalSrc(VIBEMAP_SRC);
    const map = L.map('m', { worldCopyJump: true, preferCanvas: true });
    map.setView(RX ? [RX.lat, RX.lon] : [30, 0], RX ? 7 : 2);
    if (window.VibeMap) window.VibeMap.attach(map, { dataBase: GL_DATA_BASE, profile: 'aero' });
    attach(map);
  };
  js.onerror = () => note('The map could not be loaded on this server.');
  document.head.appendChild(js);
}
if (!bootGPU()) bootLeaflet();
/* ★★ ONLY THE PAGE THAT OPENED THIS WINDOW may feed it, and only from its origin. */
window.addEventListener('message', (ev) => {
  if (ev.source !== window.opener || ev.origin !== OPENER_ORIGIN) return;
  const d = ev.data;
  if (!d || typeof d !== 'object') return;
  if (d.type === 'vibesdr-adsb') {
    note('');
    const list = Array.isArray(d.aircraft) ? d.aircraft : [];
    count(list);
    if (d.rx && (!RX || d.rx.lat !== RX.lat || d.rx.lon !== RX.lon)) { RX = d.rx; if (layer) layer.setReceiver(RX.lat, RX.lon); }
    if (layer) layer.update(list); else pending = list;
  } else if (d.type === 'vibesdr-adsb-focus' && layer) {
    layer.focus(String(d.hex || ''));
  } else if (d.type === 'vibesdr-adsb-off') {
    note('ADS-B has ended on this receiver — the last aircraft heard stay until you close this window.');
  }
});
${ES}`;
}
