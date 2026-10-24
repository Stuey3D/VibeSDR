/**
 * adsb.ts — the web client's ADS-B map view: the full-screen map and its live list (V12, docs/v12/ADSB.md).
 *
 * ★★★ THE MAP IS THE MODE (Stuart, 2026-10-24: "when ADS-B is selected the waterfall drops and the full-screen
 *     map is on, an Exit ADS-B button is at the top of the screen, same as our HFDL map exit", with "a live list
 *     in the bottom corner"). It replaced the decoder box + MAP button + popup window: nothing at 1090 MHz is
 *     worth a waterfall (the server slows it to 1 fps), and a map in a second window was one more thing to find.
 * ★★ THE PAGE'S HALF ONLY. Entering and leaving the mode and the locked controls are main.ts's (beside DAB,
 *    whose rules they follow); the aircraft on the map are web/mapkit/adsblayer.js, the one layer the app draws
 *    with too. This file turns a report into the list's rows, and makes and unmakes the map itself.
 * ★ Everything in a report came off the air or over the network: every string is escaped where it meets
 *   HTML, and the callsign is cut to the characters a callsign has.
 */
import type { AdsbAircraft, AdsbReport } from './spectrum';
import { probeMapGL, loadMapGLScripts, mapglLoad } from './mapgl';
import { loadLeafletInto } from './leafletAsset';
import { loadVibemapSource } from './chunkCache';
import { flagEmoji, adsbInfoLine } from '../../../src/services/adsbInfo';

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
/** ★ Feet, always — the map's labels say the same (adsblayer.js altText). */
export function altText(alt: AdsbAircraft['alt_baro']): string {
  if (alt === 'ground') return 'GND';
  const ft = num(alt);
  if (ft == null) return '—';
  return `${(Math.round(ft / 100) * 100).toLocaleString('en-GB')} ft`;
}
/** ★★ THE ALTITUDE COLOUR — the map's (adsblayer.js ALT_HUES / altColour, dark): orange-red at the ground fading through
 *  the spectrum to violet at 40 000 ft. ONE RULE, THREE READERS (the layer, this, src/services/adsbList.ts):
 *  scripts/test_adsb_layer.mjs fails if they part. */
export const ALT_HUES: [number, number][] = [[0, 15], [4000, 55], [10000, 120], [20000, 180], [30000, 235], [40000, 285]];
export function altColour(ft: number): string {
  let h = ALT_HUES[ALT_HUES.length - 1][1];
  const f = ft >= 0 ? ft : 0;
  for (let i = 1; i < ALT_HUES.length; i++) {
    const [a0, h0] = ALT_HUES[i - 1], [a1, h1] = ALT_HUES[i];
    if (f <= a1) { h = h0 + (h1 - h0) * (f - a0) / (a1 - a0); break; }
  }
  return `hsl(${h.toFixed(0)},90%,62%)`;
}
function emergency(a: AdsbAircraft): boolean {
  const sq = String(a.squawk ?? '');
  return sq === '7500' || sq === '7600' || sq === '7700' || (!!a.emergency && a.emergency !== 'none' && a.emergency !== 0 && a.emergency !== '0');
}

/* ★★ MILITARY — the map's rule (adsblayer.js MIL_BLOCKS / MIL_CALLS): an ICAO address in a block a state keeps for its
 *  military (UK 43C000–43CFFF, US AE0000–AFFFFF) or a military operator's callsign. ONE RULE, THREE READERS (the layer,
 *  web/client/src/adsb.ts, src/services/adsbList.ts): scripts/test_adsb_layer.mjs fails if they part. */
export const MIL_BLOCKS: [number, number][] = [[0x43C000, 0x43CFFF], [0xAE0000, 0xAFFFFF]];
export const MIL_CALLS = /^(RRR|ASCOT|TARTN|RCH|NATO|GAF|CTM|IAM|BAF)\d/;
export function isMilitaryHex(hex: string, flight: string | undefined): boolean {
  const h = String(hex || '');
  const n = parseInt(h.replace(/^~/, ''), 16);
  if (h[0] !== '~' && Number.isFinite(n)) for (const [lo, hi] of MIL_BLOCKS) if (n >= lo && n <= hi) return true;
  return MIL_CALLS.test(String(flight || '').trim().toUpperCase());
}
/** ★ What the emergency is, for its badge: the squawk first (7500 / 7600 / 7700), else the declared state; '' when none. */
export function emergencyBadge(squawk: unknown, state: unknown): string {
  const sq = String(squawk ?? '');
  if (sq === '7500') return '7500 HIJACK';
  if (sq === '7600') return '7600 RADIO FAIL';
  if (sq === '7700') return '7700 EMERGENCY';
  const st = String(state ?? '');
  return st && st !== 'none' && st !== '0' ? 'EMERGENCY' : '';
}

/** The map's top bar: how many have a position (the count, on the map) and, beside it, how many are heard
 *  at all and the message rate — "14 heard · 212 msg/s". */
export function adsbOnMap(r: AdsbReport | null): number {
  return r ? r.aircraft.filter((a) => num(a.lat) != null && num(a.lon) != null).length : 0;
}
export function adsbStatus(r: AdsbReport | null): string {
  if (!r) return 'listening on 1090 MHz…';
  const rate = num(r.msgRate);
  const r1 = rate == null ? '' : rate >= 1000 ? ` · ${(rate / 1000).toFixed(1)}k msg/s` : ` · ${Math.round(rate)} msg/s`;
  return `${r.aircraft.length} heard${r1}`;
}

/** ★★ THE FURTHEST AIRCRAFT (Stuart, 2026-10-24: "We could put a Furthest aircraft here") — the located aircraft
 *  farthest from the receiver, for the top bar's key (tap = show it on the map). null without a receiver position or
 *  a located aircraft. ONE RULE, TWO READERS: the app's MapOverlay page (__adsbPush) picks it the same way. */
export function adsbFurthest(r: AdsbReport | null, rx: LatLon | null): { hex: string; nm: number; name: string } | null {
  if (!r || !rx) return null;
  let best: { hex: string; nm: number; name: string } | null = null;
  for (const a of r.aircraft) {
    const lat = num(a.lat), lon = num(a.lon), hex = cleanHex(a.hex);
    if (lat == null || lon == null || !hex) continue;
    const nm = rangeBearing(rx, lat, lon).nm;
    if (!best || nm > best.nm) best = { hex, nm, name: String(a.flight || '').trim() || hex.toUpperCase() };
  }
  return best;
}
/** "FURTHEST 182 NM · RYR12K" — the name goes on a phone (CSS), the distance never does. */
export function adsbFurthestHtml(f: { nm: number; name: string } | null): string {
  if (!f) return '';
  const esc = (s: string) => s.replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
  return `<span class="k">FURTHEST</span> ${Math.round(f.nm)} NM<span class="n"> · ${esc(f.name)}</span>`;
}
/** ★★★ THE CAPTURE WARNING (Stuart, 2026-10-24) — the server counts the samples its ADS-B decoder RECEIVES against
 *  what the rate promises (vibe_adsb_capture.h CaptureMeter: the dongle/USB and the server's own IQ queue, not the
 *  decoder's own lag). Under 97 % the receiver is dropping samples, and replies are cut in half — the Pi 2 at 3.2 MS/s
 *  heard a third of what 2.4 did. `short` stays on a phone; `rest` says what it means. Null when nothing is wrong or unknown.
 *  ★★ ONE RULE, TWO READERS: src/services/adsbTypes.ts adsbCaptureWarning is the same — scripts/test_capture_and_rate_guard.ts. */
export const ADSB_CAPTURE_WARN_PCT = 97;
export function adsbCaptureWarning(r: AdsbReport | null): { short: string; rest: string } | null {
  const c = r ? num(r.capture) : null;
  if (c == null || c < 0 || c >= ADSB_CAPTURE_WARN_PCT) return null;
  return { short: `capture ${Math.round(c)} %`, rest: ' — the receiver is dropping samples (its USB bus or this server cannot keep up)' };
}

/** ★ The list, nearest first when the receiver's position is known (the app's AircraftPanel order),
 *  else the most recently heard first. Aircraft without a position sort after the located ones. */
export function adsbRows(r: AdsbReport, rx: LatLon | null) {
  const rows = r.aircraft.map((a) => {
    const lat = num(a.lat), lon = num(a.lon);
    const rb = rx && lat != null && lon != null ? rangeBearing(rx, lat, lon) : null;
    // ★★ military: the SERVER SAYS (mil:1 — its Annex 10 + VRS block table, the Mictronics flag), else the old rule
    const em = emergencyBadge(a.squawk, a.emergency), mil = a.mil === 1 || isMilitaryHex(String(a.hex || ''), a.flight);
    return { a, hex: cleanHex(a.hex), rb, located: lat != null && lon != null, em, mil, rank: em ? 0 : mil ? 1 : 2 };
  }).filter((x) => x.hex);
  /** ★★ THE ORDER (Stuart, 2026-10-24): emergencies first, then military, then the nearest to the receiver. */
  rows.sort((p, q) => {
    if (p.rank !== q.rank) return p.rank - q.rank;
    if (p.rb && q.rb) return p.rb.nm - q.rb.nm;
    if (p.rb) return -1;
    if (q.rb) return 1;
    if (p.located !== q.located) return p.located ? -1 : 1;
    return (num(p.a.seen) ?? 99) - (num(q.a.seen) ?? 99);
  });
  return rows;
}


/** ★ The live list's rows. Each is a button to the highlight (role) and to Tab (tabindex): tapped, clicked
 *  or entered, it shows that aircraft on the map (AdsbMap.focus). */
/** ★ SIGNAL BARS — 3 from −12 dBFS, 2 from −22, else 1; 0 = no level. ONE RULE, TWO READERS: src/services/adsbList.ts. */
export function adsbBars(rssi: unknown): number {
  const v = num(rssi);
  if (v == null) return 0;
  return v >= -12 ? 3 : v >= -22 ? 2 : 1;
}
function barsHtml(n: number): string {
  if (!n) return '';
  return `<span class="sig l${n}" title="Signal ${n} of 3">${[1, 2, 3].map((k) => `<i class="${k <= n ? 'on' : ''}"></i>`).join('')}</span>`;
}

/** Where this server serves the owner's logos ("…/adsb/"), '' where the page has no http(s) origin. Machine-wide, like
 *  /mapgl/: the front door answers it too. */
export function adsbImageBase(): string {
  if (typeof location === 'undefined' || !/^https?:$/.test(location.protocol)) return '';
  return location.origin + '/adsb/';
}
/** ★ Windows draws regional-indicator pairs as two letters, not a flag: there the code is shown as a small chip. */
function flagHtml(iso: unknown): string {
  const c = String(iso ?? '').toUpperCase();
  if (!/^[A-Z]{2}$/.test(c)) return '';
  const win = typeof navigator !== 'undefined' && /Windows/.test(navigator.userAgent || '');
  return win ? `<span class="cc" title="${esc(c)}">${esc(c)}</span>` : `<span class="flag" title="${esc(c)}">${flagEmoji(c)}</span>`;
}
/** ★ The row's second line: what the aircraft IS (operator logo or badge · type · registration · route), from the
 *  server's aircraft data; the hex and squawk when there is none (an older server, an unknown airframe). */
export function adsbSecondLine(a: AdsbAircraft, hex: string, sq: string, imgBase: string): string {
  const info = adsbInfoLine(a);
  if (!info) return `${esc(hex.toUpperCase())}${sq ? ` · ${esc(sq)}` : ''}`;
  const logo = imgBase && a.logo ? `<img class="oplogo" src="${esc(imgBase + 'logo/' + a.logo)}" alt="${esc(a.op || '')}">` : '';
  // the operator code becomes a badge (or the logo replaces it); the rest is plain text
  const parts = info.split(' \u00b7 ');
  const head = a.op && parts[0] === a.op ? (logo || `<span class="op">${esc(a.op)}</span>`) : '';
  const rest = (head ? parts.slice(1) : parts).map(esc).join(' · ');
  return `${head}${head && rest ? ' ' : ''}${rest}${sq ? ` · ${esc(sq)}` : ''}`;
}

export function adsbListHtml(r: AdsbReport | null, rx: LatLon | null, imgBase = ''): string {
  if (!r) return '<div class="adsbEmpty">Listening on 1090 MHz — aircraft appear here as they are heard.</div>';
  const rows = adsbRows(r, rx);
  if (!rows.length) return '<div class="adsbEmpty">No aircraft heard yet. Each one appears as its transponder is decoded.</div>';
  return rows.map(({ a, hex, rb, located, em: badge, mil }) => {
    const call = cleanFlight(a.flight);
    const vs = num(a.baro_rate);
    const altFt = num(a.alt_baro);
    const trend = a.alt_baro === 'ground' || altFt == null ? ''
      : vs != null && vs > 300 ? '<i class="up" title="Climbing">▲</i>' : vs != null && vs < -300 ? '<i class="dn" title="Descending">▼</i>'
      : '<i class="lv" title="Level">●</i>';
    const altStyle = altFt != null && a.alt_baro !== 'ground' ? ` style="color:${altColour(altFt)}"` : '';
    const gs = num(a.gs);
    const sq = String(a.squawk ?? '').replace(/[^0-7]/g, '').slice(0, 4);
    const where = rb ? `${rb.nm < 10 ? rb.nm.toFixed(1) : Math.round(rb.nm)} NM <span class="brg">${COMPASS[Math.round(rb.brg / 45) % 8]}</span>`
      : located ? '<span class="dim">located</span>' : '<span class="dim">no position</span>';
    // ★ TRACKED FOR xx NM (Stuart, 2026-10-24): the path the server has followed it along, under its range
    const trk = num(a.tracked_nm);
    const tracked = trk != null && trk >= 1 ? `<small class="trk">tracked ${Math.round(trk)} NM</small>` : '';
    const em = emergency(a);
    const tag = badge ? `<span class="tag em">${esc(badge)}</span>` : mil ? '<span class="tag mil">MIL</span>' : '';
    return `<div class="adsbRow${em ? ' em' : ''}${mil && !em ? ' mil' : ''}${located ? '' : ' nopos'}" role="button" tabindex="0" data-hex="${esc(hex)}"`
      + ` title="${located ? 'Show on the map' : 'No position yet — not on the map'}">`
      + `<span class="id"><b>${flagHtml(a.country)}${esc(call || a.r || hex.toUpperCase())}${barsHtml(adsbBars(a.rssi))}${tag}</b><small>${adsbSecondLine(a, hex, sq, imgBase)}</small></span>`
      + `<span class="alt"${altStyle}>${esc(altText(a.alt_baro))}${trend}</span>`
      + `<span class="spd">${gs != null ? `${Math.round(gs)} kt` : ''}</span>`
      + `<span class="rng">${where}${tracked}</span>`
      + '</div>';
  }).join('');
}

/* ══ THE MAP ══════════════════════════════════════════════════════════════════════════════════════
 * ★★ IN THE PAGE, NOT A WINDOW. The GPU map (MapLibre + our packs, every file from THIS server's /mapgl/ —
 *    mapgl.ts) in the 'aero' profile, the airports and runways of the HFDL map; the Leaflet map (from the
 *    receiver's own /vs/, SRI-checked) where there is no WebGL 2 or no /mapgl/. The aircraft are the shared
 *    layer (adsblayer.js), carried as a STRING in the bundle so the map never depends on a file an older
 *    install might lack; CSP allows the inline script it becomes ('unsafe-inline', no eval).
 * ★★ MADE ON ENTRY, DESTROYED ON EXIT. A MapLibre map holds a WebGL context and tens of MB of tiles; a
 *    listener who leaves ADS-B for the rest of the evening must not carry it (the 1 GB phones — memory
 *    android_potato_phone_users). Every async step checks `gen`, so an exit while it is still loading
 *    leaves nothing behind.
 * ★ The toggles' prefs live in this page's localStorage (vibesdr.adsbMapPrefs — the popup's key, kept so a
 *   listener's AUTO / FIT / RINGS carry over). */
const PREFS_KEY = 'vibesdr.adsbMapPrefs';
function loadPrefs(): Record<string, boolean> | null {
  try { const v = JSON.parse(localStorage.getItem(PREFS_KEY) || 'null'); return v && typeof v === 'object' ? v : null; } catch { return null; }
}
function savePrefs(p: unknown) {
  try { localStorage.setItem(PREFS_KEY, JSON.stringify(p)); } catch { /* private window: the toggles still work for this visit */ }
}
function runSource(src: string) {
  const el = document.createElement('script');
  el.textContent = src;          // ★ runs synchronously on append — the global exists on the next line
  document.head.appendChild(el);
}

export class AdsbMap {
  private gen = 0;
  private vm: any = null;
  private lmap: any = null;
  private layer: any = null;
  private pending: AdsbAircraft[] | null = null;
  private rx: LatLon | null = null;
  private booting = false;

  constructor(private host: HTMLElement, private note: (text: string) => void) {}

  /** Make the map, if it is not made or being made. The host must be on screen (MapLibre sizes from it). */
  open(rx: LatLon | null) {
    this.rx = rx;
    if (this.layer || this.booting) return;
    this.booting = true;
    const my = ++this.gen;
    void this.boot(my).catch((e) => {
      console.error('[adsb] the map could not load', e);
      if (my === this.gen) this.note('The map did not finish loading — the list still updates.');
    }).finally(() => { if (my === this.gen) this.booting = false; });
  }

  private async boot(my: number) {
    const w = window as any;
    const kit = await probeMapGL();
    if (my !== this.gen) return;
    const { ADSB_LAYER_JS } = await import('./generated/adsbLayerSource');
    if (my !== this.gen) return;
    if (!w.VibeAdsbLayer) runSource(ADSB_LAYER_JS);
    if (!w.VibeAdsbLayer) { this.note('The aircraft layer did not load.'); return; }
    const rx = this.rx;
    if (kit && await loadMapGLScripts(kit.base)) {
      if (my !== this.gen) return;
      try {
        const vm = w.VibeMapGL.createNow(this.host, {
          style: kit.style, load: mapglLoad, base: kit.base, rangeBase: kit.base, detail: kit.detail, profile: 'aero',
          center: rx ? [rx.lon, rx.lat] : [0, 30], zoom: rx ? 6 : 1.5,
        });
        if (vm) {
          this.vm = vm;
          vm.ready.catch((e: unknown) => console.error('ADS-B GPU map: the style did not load', e));
          vm.setNight(true);          // ★ the greyline on every map, as on the HFDL map
          vm.map.addControl(new w.maplibregl.NavigationControl({ showCompass: false }), 'top-left');
          this.attach(vm.map);
          return;
        }
      } catch (e) {
        console.error('ADS-B GPU map failed — using the Leaflet map', e);
        try { this.vm?.destroy(); } catch (e2) { console.error('ADS-B GPU map: tidy-up failed', e2); }
        this.vm = null;
        this.host.innerHTML = '';
      }
    }
    if (!await loadLeafletInto(document)) { if (my === this.gen) this.note('The map could not be loaded on this server — the list still updates.'); return; }
    if (my !== this.gen) return;
    if (!w.VibeMap) { const { VIBEMAP_JS } = await loadVibemapSource(); if (my !== this.gen) return; if (!w.VibeMap) runSource(VIBEMAP_JS); }
    const L = w.L;
    this.lmap = L.map(this.host, { worldCopyJump: true, preferCanvas: true });
    this.lmap.setView(rx ? [rx.lat, rx.lon] : [30, 0], rx ? 7 : 2);
    if (w.VibeMap) w.VibeMap.attach(this.lmap, { dataBase: '/mapdata/v1/', profile: 'aero' });
    this.attach(this.lmap);
  }

  private attach(map: any) {
    const w = window as any;
    // ★ imageBase: the owner's logos/silhouettes (named by the server only when they exist). photos: OFF until the
    //   Planespotters terms are read at first hand — see the TODO in adsblayer.js.
    this.layer = w.VibeAdsbLayer.create(map, { receiver: this.rx, prefs: loadPrefs(), onPrefs: savePrefs, theme: 'dark',
                                                imageBase: adsbImageBase() || null });
    if (this.pending) { this.layer.update(this.pending); this.pending = null; }
    if (this.obstacle) { try { this.layer.setObstacle?.(this.obstacle); } catch (e) { console.error('[adsb] setObstacle', e); } }
  }

  /** Every report: the whole aircraft list, and the receiver if it has arrived or moved since. */
  update(list: AdsbAircraft[], rx: LatLon | null) {
    if (rx && (!this.rx || rx.lat !== this.rx.lat || rx.lon !== this.rx.lon)) {
      this.rx = rx;
      try { this.layer?.setReceiver(rx.lat, rx.lon); } catch (e) { console.error('[adsb] setReceiver', e); }
    }
    if (this.layer) {
      try { this.layer.update(list); } catch (e) { console.error('[adsb] layer.update', e); }
    } else this.pending = list;
  }

  /** The live list's corner of the map, { corner, w, h } in CSS px or null: the layer keeps aircraft clear of it. */
  setObstacle(o: { corner: string; w: number; h: number } | null) {
    this.obstacle = o;
    try { this.layer?.setObstacle?.(o); } catch (e) { console.error('[adsb] setObstacle', e); }
  }
  private obstacle: { corner: string; w: number; h: number } | null = null;

  /** The colour key (the list's (i) button). Returns whether it is now shown. */
  toggleLegend(): boolean {
    try { return !!this.layer?.toggleLegend?.(); } catch (e) { console.error('[adsb] toggleLegend', e); return false; }
  }

  /** A list row: select it and open its card. False = not on the map (no position, or no map yet). */
  focus(hex: string): boolean {
    try { return !!this.layer?.focus(cleanHex(hex)); } catch (e) { console.error('[adsb] focus', e); return false; }
  }

  /** The MapLibre map (for its container's controls), or null. */
  glMap(): any { return this.vm?.map ?? null; }

  /** Unmake everything — the layer, the map, its WebGL context, its tiles. */
  close() {
    this.gen++;
    this.booting = false;
    this.pending = null;
    try { this.layer?.destroy(); } catch (e) { console.error('[adsb] layer.destroy', e); }
    this.layer = null;
    try { this.vm?.destroy(); } catch (e) { console.error('[adsb] map destroy', e); }
    this.vm = null;
    try { this.lmap?.remove(); } catch (e) { console.error('[adsb] Leaflet remove', e); }
    this.lmap = null;
    this.host.innerHTML = '';
    this.host.className = '';        // ★ MapLibre / Leaflet leave their container classes behind
  }
}
