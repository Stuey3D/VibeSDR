/**
 * tracks.ts — the web client's AIS and APRS map views (V12, docs/v12/AIS-APRS.md): a full-screen map with a live list
 * in the corner, ADS-B's shape (adsb.ts), for SHIPS (AIS mode: the whole radio on 162 MHz) and APRS STATIONS (the
 * APRS decoder box's MAP button).
 *
 * ★★ THE PAGE'S HALF ONLY. The markers, the glide, a tapped item's track and its card are web/mapkit/tracklayer.js (one
 *    layer, carried as a string — the app's map will load the same file). This file makes and unmakes the map (MapLibre
 *    with our own packs from this server's /mapgl/, else Leaflet from /vs/ — the same rule as AdsbMap), the overlay, the
 *    list, and what a ship or a station LOOKS like: its icon, its label, its card, its row.
 * ★★ MADE ON ENTRY, DESTROYED ON EXIT — a MapLibre map holds a WebGL context and tens of MB of tiles; a listener who
 *    leaves for the evening must not carry it (the 1 GB phones).
 * ★ Everything in a report came off the air: every string is escaped where it meets HTML.
 */
import { probeMapGL, loadMapGLScripts, mapglLoad } from './mapgl';
import { loadLeafletInto } from './leafletAsset';
import { loadVibemapSource } from './chunkCache';
import { flagEmoji } from '../../../src/services/adsbInfo';

export type LatLon = { lat: number; lon: number };

// ── the report shapes (vibe_ais_service.h / vibe_aprs_run.h) ──────────────────────────────────────────────────────
export interface AisVessel {
  mmsi: number; kind: string; name?: string; call?: string; dest?: string; imo?: number; flag?: string; country?: string;
  stype?: number; stypeName?: string; family?: string; nav?: number; navName?: string; aton?: number; atonName?: string;
  virtual?: number; offpos?: number; lat?: number; lon?: number; sog?: number; cog?: number; hdg?: number; rot?: number; turn?: number;
  draught?: number; len?: number; beam?: number; eta?: string; alt?: number; trend?: number; line?: string;
  ch?: string; level?: number; snr?: number; seen: number; age: number; posAge: number; count: number; trail?: number;
}
export interface AisReport {
  type: 'ais'; rate: number; now: number; msgs: number; msgRate: number; chA: number; chB: number; repaired: number;
  dropped: number; capture: number; noise: number; vessels: AisVessel[]; safety: { age: number; mmsi: number; name: string; text: string; to?: number }[];
  gain?: number; gainAuto?: number;
}
export interface AprsStation {
  key: string; from: string; kind: string; name?: string; sym: string; symName?: string; lat?: number; lon?: number;
  course?: number; speed?: number; alt?: number; comment?: string; status?: string; micE?: string; path?: string; via?: string;
  raw?: string; line?: string; wx?: Record<string, number>; killed?: number; msg?: number; seen: number; age: number; count: number;
  posAge: number; trail?: [number, number][];
}
export interface AprsList {
  baud: number; frames: number; repaired: number; dupes: number; stations: AprsStation[];
  messages: { age: number; from: string; to: string; text: string; kind: string; id?: string }[];
}

const num = (v: unknown): number | null => { const n = Number(v); return v == null || v === '' || !isFinite(n) ? null : n; };
const str = (v: unknown, n: number) => String(v ?? '').slice(0, n);
export function esc(v: unknown): string {
  return String(v ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!));
}
/** ★ What a report may carry, cut to its fields (a hostile or broken server must not put an object where a number goes). */
export function aisSanitise(m: any): AisReport | null {
  if (!m || m.type !== 'ais' || !Array.isArray(m.vessels)) return null;
  const vs: AisVessel[] = [];
  for (const v of m.vessels.slice(0, 1500)) {
    const mmsi = num(v?.mmsi);
    if (mmsi == null || mmsi <= 0) continue;
    vs.push({
      mmsi, kind: str(v.kind, 8), name: str(v.name, 40), call: str(v.call, 10), dest: str(v.dest, 40), imo: num(v.imo) ?? undefined,
      flag: /^[A-Z]{2}$/.test(String(v.flag ?? '')) ? v.flag : undefined, country: str(v.country, 60),
      stype: num(v.stype) ?? undefined, stypeName: str(v.stypeName, 40), family: str(v.family, 12), nav: num(v.nav) ?? undefined,
      navName: str(v.navName, 60), aton: num(v.aton) ?? undefined, atonName: str(v.atonName, 60), virtual: num(v.virtual) ?? undefined,
      offpos: num(v.offpos) ?? undefined, lat: num(v.lat) ?? undefined, lon: num(v.lon) ?? undefined, sog: num(v.sog) ?? undefined,
      cog: num(v.cog) ?? undefined, hdg: num(v.hdg) ?? undefined, rot: num(v.rot) ?? undefined, turn: num(v.turn) ?? undefined,
      draught: num(v.draught) ?? undefined, len: num(v.len) ?? undefined, beam: num(v.beam) ?? undefined, eta: str(v.eta, 16),
      alt: num(v.alt) ?? undefined, trend: num(v.trend) ?? undefined, line: str(v.line, 200), ch: str(v.ch, 2),
      level: num(v.level) ?? undefined, snr: num(v.snr) ?? undefined, seen: num(v.seen) ?? 0, age: num(v.age) ?? 0,
      posAge: num(v.posAge) ?? -1, count: num(v.count) ?? 0, trail: num(v.trail) ?? undefined,
    });
  }
  const safety = Array.isArray(m.safety) ? m.safety.slice(0, 20).map((s: any) => ({ age: num(s?.age) ?? 0, mmsi: num(s?.mmsi) ?? 0, name: str(s?.name, 40), text: str(s?.text, 200), to: num(s?.to) ?? undefined })) : [];
  return { type: 'ais', rate: num(m.rate) ?? 0, now: num(m.now) ?? 0, msgs: num(m.msgs) ?? 0, msgRate: num(m.msgRate) ?? 0,
           chA: num(m.chA) ?? 0, chB: num(m.chB) ?? 0, repaired: num(m.repaired) ?? 0, dropped: num(m.dropped) ?? 0,
           capture: num(m.capture) ?? -1, noise: num(m.noise) ?? 0, vessels: vs, safety,
           gain: num(m.gain) ?? undefined, gainAuto: num(m.gainAuto) ?? undefined };
}
export function aprsSanitise(d: any): AprsList | null {
  if (!d || !Array.isArray(d.stations)) return null;
  const st: AprsStation[] = [];
  for (const s of d.stations.slice(0, 600)) {
    const key = str(s?.key, 40);
    if (!key) continue;
    const wx: Record<string, number> = {};
    if (s.wx && typeof s.wx === 'object') for (const k of ['tempC', 'hum', 'windDir', 'windKmh', 'gustKmh', 'rain1hMm', 'rainTodayMm', 'hPa']) { const n = num(s.wx[k]); if (n != null) wx[k] = n; }
    const trail = Array.isArray(s.trail) ? s.trail.slice(-200).filter((p: any) => Array.isArray(p) && num(p[0]) != null && num(p[1]) != null).map((p: any) => [Number(p[0]), Number(p[1])] as [number, number]) : undefined;
    st.push({ key, from: str(s.from, 16), kind: str(s.kind, 14), name: str(s.name, 12), sym: str(s.sym, 2).padEnd(2, ' '), symName: str(s.symName, 40),
              lat: num(s.lat) ?? undefined, lon: num(s.lon) ?? undefined, course: num(s.course) ?? undefined, speed: num(s.speed) ?? undefined,
              alt: num(s.alt) ?? undefined, comment: str(s.comment, 200), status: str(s.status, 200), micE: str(s.micE, 20), path: str(s.path, 80),
              via: str(s.via, 16), raw: str(s.raw, 400), line: str(s.line, 300), wx: Object.keys(wx).length ? wx : undefined,
              killed: num(s.killed) ?? undefined, msg: num(s.msg) ?? undefined, seen: num(s.seen) ?? 0, age: num(s.age) ?? 0,
              count: num(s.count) ?? 0, posAge: num(s.posAge) ?? -1, trail });
  }
  const messages = Array.isArray(d.messages) ? d.messages.slice(-60).map((m: any) => ({ age: num(m?.age) ?? 0, from: str(m?.from, 16), to: str(m?.to, 12), text: str(m?.text, 200), kind: str(m?.kind, 10), id: str(m?.id, 8) || undefined })) : [];
  return { baud: num(d.baud) ?? 1200, frames: num(d.frames) ?? 0, repaired: num(d.repaired) ?? 0, dupes: num(d.dupes) ?? 0, stations: st, messages };
}

// ── shared formatting ──────────────────────────────────────────────────────────────────────────────────────────────
const D2R = Math.PI / 180;
export function rangeBearing(rx: LatLon, lat: number, lon: number): { km: number; nm: number; brg: number } {
  const p1 = rx.lat * D2R, p2 = lat * D2R, dp = (lat - rx.lat) * D2R, dl = (lon - rx.lon) * D2R;
  const h = Math.sin(dp / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2;
  const km = 2 * 6371.0088 * Math.asin(Math.min(1, Math.sqrt(h)));
  const y = Math.sin(dl) * Math.cos(p2), x = Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl);
  return { km, nm: km / 1.852, brg: (Math.atan2(y, x) / D2R + 360) % 360 };
}
const C8 = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
export const compass8 = (d: number) => C8[Math.round(((d % 360) + 360) % 360 / 45) % 8];
export function ago(s: number): string {
  const v = Math.max(0, Math.round(s));
  return v < 5 ? 'now' : v < 60 ? `${v} s` : v < 3600 ? `${Math.round(v / 60)} min` : `${Math.floor(v / 3600)} h ${Math.round((v % 3600) / 60)} min`;
}
/** "just now" / "12 s ago" — the card's wording (ago() alone is the list's short form). */
export function heardAgo(s: number): string { const a = ago(s); return a === 'now' ? 'just now' : `${a} ago`; }
function flagHtml(iso: string | undefined): string {
  if (!iso || !/^[A-Z]{2}$/.test(iso)) return '';
  const win = typeof navigator !== 'undefined' && /Windows/.test(navigator.userAgent || '');
  return win ? `<span class="cc">${esc(iso)}</span> ` : `<span class="flag">${flagEmoji(iso)}</span> `;
}
function fmtPos(lat?: number, lon?: number): string {
  if (lat == null || lon == null) return '';
  const f = (v: number, p: string, n: string, w: number) => {
    const a = Math.abs(v), d = Math.floor(a), m = (a - d) * 60;
    return `${String(d).padStart(w, '0')}°${m.toFixed(3).padStart(6, '0')}′${v >= 0 ? p : n}`;
  };
  return `${f(lat, 'N', 'S', 2)} ${f(lon, 'E', 'W', 3)}`;
}

// ── AIS: icons, rows, cards ───────────────────────────────────────────────────────────────────────────────────────
/** ★ The colour of a ship by what it is (shipFamily on the server) — the conventions chart plotters share. */
export const AIS_COLOURS: Record<string, string> = {
  cargo: '#6dd36d', tanker: '#ff6b5e', passenger: '#4fa3ff', fishing: '#ffad42', tug: '#39d0d8', special: '#ffe14d',
  leisure: '#c58cff', hsc: '#f7f06a', other: '#b8c2cc',
};
function aisMoving(v: AisVessel) { return v.sog != null && v.sog >= 0.5 && v.nav !== 1 && v.nav !== 5 && v.nav !== 6; }
export function aisIcon(v: AisVessel): { html: string; w: number; h: number } {
  const col = v.kind === 'sart' ? '#ff3b3b' : AIS_COLOURS[v.family || 'other'] || AIS_COLOURS.other;
  if (v.kind === 'aton') {
    const dash = v.virtual ? ' stroke-dasharray="2 2"' : '';
    return { w: 18, h: 18, html: `<svg width="18" height="18" viewBox="0 0 18 18"><path d="M9 1.5 L16.5 9 L9 16.5 L1.5 9 Z" fill="${v.offpos ? '#ff6b5e' : '#ffe14d'}" fill-opacity=".85" stroke="#1b1b00" stroke-width="1.2"${dash}/><path d="M9 5 V13 M5 9 H13" stroke="#1b1b00" stroke-width="1.4"/></svg>` };
  }
  if (v.kind === 'base') return { w: 18, h: 18, html: '<svg width="18" height="18" viewBox="0 0 18 18"><path d="M9 2 L13 16 H5 Z" fill="#9fb0c2" stroke="#10161d" stroke-width="1.2"/><circle cx="9" cy="3" r="2" fill="#ffcf5a"/></svg>' };
  if (v.kind === 'sar') {
    const r = v.cog ?? 0;
    return { w: 26, h: 26, html: `<svg width="26" height="26" viewBox="0 0 26 26" style="transform:rotate(${r}deg)"><path d="M13 2 L15 10 L24 14 L24 16 L15 14 L14.5 20 L17 22 L17 23.5 L13 22.5 L9 23.5 L9 22 L11.5 20 L11 14 L2 16 L2 14 L11 10 Z" fill="#ffe14d" stroke="#1b1b00" stroke-width="1"/></svg>` };
  }
  if (v.kind === 'sart') return { w: 22, h: 22, html: '<svg width="22" height="22" viewBox="0 0 22 22"><circle cx="11" cy="11" r="9" fill="#ff3b3b" stroke="#fff" stroke-width="1.5"/><path d="M6.5 6.5 L15.5 15.5 M15.5 6.5 L6.5 15.5" stroke="#fff" stroke-width="2"/></svg>' };
  if (!aisMoving(v) || (v.hdg == null && v.cog == null)) {
    return { w: 14, h: 14, html: `<svg width="14" height="14" viewBox="0 0 14 14"><circle cx="7" cy="7" r="5" fill="${col}" stroke="#0b1014" stroke-width="1.4"/></svg>` };
  }
  const rot = v.hdg ?? v.cog ?? 0;
  // ★ the hull, pointing up: a pointed bow, a square stern; bigger for a ship that says it is long
  const big = (v.len ?? 0) >= 120 ? 1.25 : (v.len ?? 0) > 0 && (v.len ?? 0) < 25 ? 0.85 : 1;
  const w = Math.round(14 * big), h = Math.round(26 * big);
  return { w: h, h, html: `<svg width="${w}" height="${h}" viewBox="0 0 14 26" style="transform:rotate(${rot}deg)"><path d="M7 1 C10 5 12 9 12 13 L12 24 L2 24 L2 13 C2 9 4 5 7 1 Z" fill="${col}" stroke="#0b1014" stroke-width="1.3"/><path d="M4.5 17 H9.5" stroke="#0b1014" stroke-width="1" opacity=".7"/></svg>` };
}
export function aisName(v: AisVessel): string { return v.name || v.call || `MMSI ${v.mmsi}`; }
function aisKindWord(v: AisVessel): string {
  if (v.kind === 'aton') return v.atonName || 'aid to navigation';
  if (v.kind === 'base') return 'base station';
  if (v.kind === 'sar') return 'SAR aircraft';
  if (v.kind === 'sart') return 'DISTRESS BEACON';
  return v.stypeName || (v.kind === 'B' ? 'class B' : 'vessel');
}
function trendMark(t?: number): string {
  return t === 1 ? ' <i class="up" title="Speeding up">▲</i>' : t === -1 ? ' <i class="dn" title="Slowing down">▼</i>' : '';
}
export function aisCard(v: AisVessel, rx: LatLon | null): string {
  const rows: string[] = [];
  const add = (k: string, val: string) => { if (val) rows.push(`<div class="r"><span class="k">${k}</span><span class="v">${val}</span></div>`); };
  add('MMSI', esc(v.mmsi));
  add('Flag', esc(v.country || ''));
  add('Call', esc(v.call || ''));
  if (v.imo) add('IMO', esc(v.imo));
  add('Type', esc(aisKindWord(v)) + (v.kind === 'B' ? ' · class B' : ''));
  if (v.navName && v.nav !== 15) add('Status', esc(v.navName));
  if (v.sog != null) add('Speed', `${v.sog.toFixed(1)} kn${trendMark(v.trend)}`);
  if (v.cog != null) add('Course', `${Math.round(v.cog)}° ${compass8(v.cog)}${v.hdg != null ? ` · heading ${v.hdg}°` : ''}`);
  if (v.rot != null && Math.abs(v.rot) >= 1) add('Turning', `${Math.abs(v.rot).toFixed(0)}°/min ${v.rot > 0 ? 'to starboard ▶' : '◀ to port'}`);
  else if (v.turn) add('Turning', v.turn > 0 ? 'to starboard ▶' : '◀ to port');
  if (v.alt != null) add('Altitude', `${v.alt} m`);
  if (v.len) add('Size', `${v.len} × ${v.beam ?? '?'} m`);
  if (v.draught != null) add('Draught', `${v.draught.toFixed(1)} m`);
  if (v.dest) add('Destination', esc(v.dest) + (v.eta ? ` · ETA ${esc(v.eta)} UTC` : ''));
  if (v.lat != null && v.lon != null) {
    add('Position', esc(fmtPos(v.lat, v.lon)));
    if (rx) { const rb = rangeBearing(rx, v.lat, v.lon); add('From here', `${rb.nm < 10 ? rb.nm.toFixed(1) : Math.round(rb.nm)} NM ${compass8(rb.brg)}`); }
  }
  add('Heard', `${heardAgo(v.seen)} · ${v.count} message${v.count === 1 ? '' : 's'} · AIS ${v.ch === 'B' ? '2' : '1'}${v.snr != null ? ` · ${Math.round(v.snr)} dB` : ''}`);
  return `<div class="trkCard"><div class="t">${flagHtml(v.flag)}<b>${esc(aisName(v))}</b></div>${rows.join('')}</div>`;
}
export function aisStatus(r: AisReport | null): string {
  if (!r) return 'listening on 161.975 and 162.025 MHz…';
  const pos = r.vessels.filter((v) => v.lat != null).length;
  const cap = r.capture >= 0 && r.capture < 97 ? ` · capture ${Math.round(r.capture)} %` : '';
  return `${r.vessels.length} heard · ${pos} on the map · ${r.msgRate.toFixed(r.msgRate < 10 ? 1 : 0)} msg/min${cap}`;
}
export function aisRows(r: AisReport, rx: LatLon | null): string {
  if (!r.vessels.length) return '<div class="trkEmpty">No ships heard yet. Each one appears as its transponder is decoded — a class A ship reports every few seconds under way, every three minutes at anchor.</div>';
  const rows = r.vessels.map((v) => ({ v, rb: rx && v.lat != null && v.lon != null ? rangeBearing(rx, v.lat, v.lon) : null }));
  rows.sort((a, b) => (a.rb && b.rb ? a.rb.km - b.rb.km : a.rb ? -1 : b.rb ? 1 : a.v.seen - b.v.seen));
  return rows.map(({ v, rb }) => {
    const col = v.kind === 'sart' ? '#ff3b3b' : AIS_COLOURS[v.family || 'other'] || AIS_COLOURS.other;
    const sp = v.sog != null && v.sog >= 0.1 ? `${v.sog.toFixed(v.sog < 10 ? 1 : 0)} kn${v.cog != null && v.sog >= 0.5 ? ' ' + compass8(v.cog) : ''}` : v.lat != null ? 'still' : '';
    const where = rb ? `${rb.nm < 10 ? rb.nm.toFixed(1) : Math.round(rb.nm)} NM <span class="brg">${compass8(rb.brg)}</span>` : v.lat != null ? '' : '<span class="dim">no position</span>';
    const second = [aisKindWord(v), v.navName && v.nav !== 0 && v.nav !== 15 ? v.navName : '', v.dest ? '→ ' + v.dest : ''].filter(Boolean).map(esc).join(' · ');
    return `<div class="trkRow${v.lat == null ? ' nopos' : ''}${v.kind === 'sart' ? ' em' : ''}" role="button" tabindex="0" data-key="${esc(v.mmsi)}">`
      + `<span class="id"><b><i class="sw" style="background:${col}"></i>${flagHtml(v.flag)}${esc(aisName(v))}</b><small>${second || 'MMSI ' + esc(v.mmsi)}</small></span>`
      + `<span class="sp">${esc(sp)}${trendMark(v.trend)}</span><span class="rg">${where}</span></div>`;
  }).join('');
}

// ── APRS: icons, rows, cards ──────────────────────────────────────────────────────────────────────────────────────
/** ★ OUR OWN SYMBOL SET (drawn here — no third-party artwork): the APRS 1.0.1 symbol CODE picks a glyph, the table and
 *  any overlay character are shown as the spec says (the alternate table's overlay letter or digit on the badge). A code
 *  we have no glyph for is the symbol character itself on the badge, so nothing is ever blank. */
const APRS_GLYPH: Record<string, string> = {
  car: 'M2.5 9.5 L4.2 5.5 H11.8 L13.5 9.5 V12 H2.5 Z M4.5 12.5 a1.4 1.4 0 1 0 0.01 0 M11.5 12.5 a1.4 1.4 0 1 0 0.01 0',
  truck: 'M1.5 5 H9.5 V11.5 H1.5 Z M9.5 7 H12.5 L14.5 9.5 V11.5 H9.5 Z M4 12.3 a1.3 1.3 0 1 0 0.01 0 M12 12.3 a1.3 1.3 0 1 0 0.01 0',
  house: 'M8 2 L14.5 8 H12.5 V14 H3.5 V8 H1.5 Z',
  star: 'M8 1.5 L9.9 6 L14.7 6.3 L11 9.4 L12.2 14 L8 11.4 L3.8 14 L5 9.4 L1.3 6.3 L6.1 6 Z',
  diamond: 'M8 1.5 L14.5 8 L8 14.5 L1.5 8 Z',
  cloud: 'M4.2 12.5 A2.8 2.8 0 0 1 4.6 6.9 A3.8 3.8 0 0 1 11.6 6.4 A3 3 0 0 1 12 12.5 Z',
  balloon: 'M8 1.5 a4.6 4.6 0 1 1 -0.01 0 Z M8 10.7 L7 13 H9 Z',
  plane: 'M8 1 L9.2 6 L14.5 8.7 V10 L9.2 8.6 L8.9 12.3 L10.4 13.6 V14.6 L8 14 L5.6 14.6 V13.6 L7.1 12.3 L6.8 8.6 L1.5 10 V8.7 L6.8 6 Z',
  heli: 'M2 3.5 H14 M8 3.5 V5.5 M4.5 6.5 H10 a2.8 2.8 0 0 1 0 5.6 H6 Z M10.5 9 H15 M14.5 7.5 V10.5',
  boat: 'M1.5 9.5 H14.5 L12.5 13.5 H3.5 Z M8 2 V9.5 M8 2.5 L12.5 8.5 H8',
  person: 'M8 1.8 a1.9 1.9 0 1 1 -0.01 0 Z M5.2 6.2 H10.8 L10 10.2 H9.2 V14.2 H6.8 V10.2 H6 Z',
  bike: 'M4 11 a2.6 2.6 0 1 0 0.01 0 M12 11 a2.6 2.6 0 1 0 0.01 0 M4 11 L7 6 H10 L12 11 M7 6 L8.5 11 L10 6',
  antenna: 'M8 4 V14.5 M4.5 14.5 H11.5 M4.8 7.2 L8 4 L11.2 7.2 M2.8 4.5 a6 6 0 0 1 2 -2.5 M13.2 4.5 a6 6 0 0 0 -2 -2.5',
};
const APRS_CODE_GLYPH: Record<string, string> = {
  '>': 'car', 'j': 'car', 'k': 'truck', 'u': 'truck', 'v': 'truck', 'R': 'truck', 'U': 'truck', 'a': 'truck', 'f': 'truck',
  '-': 'house', 'K': 'house', 'H': 'house', '#': 'star', '&': 'diamond', '_': 'cloud', 'O': 'balloon', '^': 'plane',
  "'": 'plane', 'X': 'heli', 's': 'boat', 'Y': 'boat', 'C': 'boat', '[': 'person', 'b': 'bike', '<': 'bike', 'r': 'antenna',
  'y': 'antenna', 'n': 'star',
};
export function aprsColour(s: AprsStation): string {
  if (s.kind === 'weather' || s.sym[1] === '_') return '#39d0d8';
  if (s.kind === 'object' || s.kind === 'item') return '#ffad42';
  if (s.sym[1] === '#' || s.sym[1] === '&' || s.sym[1] === 'r') return '#c58cff';
  if (s.speed != null && s.speed >= 2) return '#4fa3ff';
  return '#59c26b';
}
export function aprsIcon(s: AprsStation): { html: string; w: number; h: number } {
  const col = aprsColour(s);
  const code = s.sym[1] || '/', table = s.sym[0] || '/';
  const g = APRS_GLYPH[APRS_CODE_GLYPH[code] || ''];
  const overlay = table !== '/' && table !== '\\' && /[0-9A-Z]/.test(table) ? table : '';
  const line = (APRS_CODE_GLYPH[code] === 'heli' || APRS_CODE_GLYPH[code] === 'bike' || APRS_CODE_GLYPH[code] === 'antenna');
  const inner = g
    ? `<path d="${g}" transform="translate(3 3)" fill="${line ? 'none' : '#0b1014'}" stroke="#0b1014" stroke-width="${line ? 1.5 : 0.4}" stroke-linejoin="round" stroke-linecap="round"/>`
    : `<text x="11" y="15" text-anchor="middle" font-family="ui-monospace,Menlo,monospace" font-size="11" font-weight="700" fill="#0b1014">${esc(code)}</text>`;
  const ov = overlay ? `<text x="17.5" y="21" text-anchor="middle" font-family="ui-monospace,monospace" font-size="8" font-weight="800" fill="#fff" stroke="#0b1014" stroke-width="2" paint-order="stroke">${esc(overlay)}</text>` : '';
  const ring = table === '\\' ? ' stroke-dasharray="3 1.6"' : '';
  return { w: 22, h: 22, html: `<svg width="22" height="22" viewBox="0 0 22 22"><circle cx="11" cy="11" r="10" fill="${col}" stroke="#0b1014" stroke-width="1.4"${ring}/>${inner}${ov}</svg>` };
}
export function aprsName(s: AprsStation): string { return s.name || s.from || s.key; }
const altTrend = new Map<string, { alt: number; t: number; d: number }>();
/** ★ ▲/▼ for a station that reports its altitude (a balloon climbing, a hill-walker coming down): against the last report. */
function aprsAltCue(s: AprsStation): string {
  if (s.alt == null) return '';
  const p = altTrend.get(s.key);
  let d = p?.d ?? 0;
  if (!p || p.alt !== s.alt) {
    d = p ? (s.alt - p.alt > 5 ? 1 : s.alt - p.alt < -5 ? -1 : 0) : 0;
    altTrend.set(s.key, { alt: s.alt, t: Date.now(), d });
    if (altTrend.size > 2000) altTrend.clear();
  }
  return d > 0 ? ' <i class="up" title="Climbing">▲</i>' : d < 0 ? ' <i class="dn" title="Descending">▼</i>' : '';
}
export function aprsCard(s: AprsStation, rx: LatLon | null): string {
  const rows: string[] = [];
  const add = (k: string, val: string) => { if (val) rows.push(`<div class="r"><span class="k">${k}</span><span class="v">${val}</span></div>`); };
  if (s.name) add('Object of', esc(s.from));
  add('Symbol', esc(s.symName || s.sym.trim()));
  if (s.killed) add('State', 'removed by its owner');
  if (s.micE) add('Mic-E', esc(s.micE));
  if (s.speed != null && s.speed >= 1) add('Speed', `${Math.round(s.speed)} km/h${s.course != null ? ` ${compass8(s.course)} (${Math.round(s.course)}°)` : ''}`);
  if (s.alt != null) add('Altitude', `${Math.round(s.alt)} m${aprsAltCue(s)}`);
  const w = s.wx;
  if (w) {
    const parts: string[] = [];
    if (w.tempC != null) parts.push(`${w.tempC.toFixed(1)} °C`);
    if (w.hum != null) parts.push(`${Math.round(w.hum)} % RH`);
    if (w.windKmh != null) parts.push(`wind ${Math.round(w.windKmh)} km/h${w.windDir != null && w.windKmh > 0 ? ' ' + compass8(w.windDir) : ''}`);
    if (w.gustKmh != null) parts.push(`gust ${Math.round(w.gustKmh)}`);
    if (w.rain1hMm != null && w.rain1hMm > 0) parts.push(`rain ${w.rain1hMm.toFixed(1)} mm/h`);
    if (w.hPa != null) parts.push(`${w.hPa.toFixed(1)} hPa`);
    add('Weather', esc(parts.join(' · ')));
  }
  if (s.comment) add('Comment', esc(s.comment));
  if (s.status) add('Status', esc(s.status));
  if (s.lat != null && s.lon != null) {
    add('Position', esc(fmtPos(s.lat, s.lon)));
    if (rx) { const rb = rangeBearing(rx, s.lat, s.lon); add('From here', `${rb.km < 10 ? rb.km.toFixed(1) : Math.round(rb.km)} km ${compass8(rb.brg)}`); }
  }
  add('Path', esc(s.path || 'direct') + (s.via ? ` · via <b>${esc(s.via)}</b>` : ''));
  add('Heard', `${heardAgo(s.seen)} · ${s.count} packet${s.count === 1 ? '' : 's'}`);
  if (s.raw) rows.push(`<div class="raw" title="The packet as received (TNC2)">${esc(s.raw)}</div>`);
  return `<div class="trkCard"><div class="t"><b>${esc(aprsName(s))}</b></div>${rows.join('')}</div>`;
}
export function aprsStatus(l: AprsList | null, hz: number): string {
  if (!l) return `listening on ${(hz / 1e6).toFixed(3)} MHz…`;
  const pos = l.stations.filter((s) => s.lat != null).length;
  return `${l.stations.length} station${l.stations.length === 1 ? '' : 's'} · ${pos} on the map · ${l.frames} packets${l.dupes ? ` (${l.dupes} digipeated copies)` : ''}`;
}
export function aprsRows(l: AprsList, rx: LatLon | null): string {
  if (!l.stations.length) return '<div class="trkEmpty">No stations heard yet. Each appears with its first packet — most beacon every 10 to 30 minutes.</div>';
  return l.stations.map((s) => {
    const rb = rx && s.lat != null && s.lon != null ? rangeBearing(rx, s.lat, s.lon) : null;
    const second = [s.symName, s.comment || s.status].filter(Boolean).map((x) => esc(x)).join(' · ');
    const where = rb ? `${rb.km < 10 ? rb.km.toFixed(1) : Math.round(rb.km)} km <span class="brg">${compass8(rb.brg)}</span>` : s.lat != null ? '' : '<span class="dim">no position</span>';
    const sp = s.speed != null && s.speed >= 2 ? `${Math.round(s.speed)} km/h` : s.wx?.tempC != null ? `${s.wx.tempC.toFixed(1)} °C` : '';
    return `<div class="trkRow${s.lat == null ? ' nopos' : ''}" role="button" tabindex="0" data-key="${esc(s.key)}">`
      + `<span class="id"><b><i class="sw" style="background:${aprsColour(s)}"></i>${esc(aprsName(s))}</b><small>${second || esc(s.kind)}</small></span>`
      + `<span class="sp">${esc(sp)}</span><span class="rg">${where}</span><span class="ag">${esc(ago(s.seen))}</span></div>`;
  }).join('');
}

// ── the overlay ───────────────────────────────────────────────────────────────────────────────────────────────────
const CSS = `
.trkView{position:fixed;inset:0;z-index:600;background:#0b1014;color:#dfe7ef;display:flex;flex-direction:column;
  font:13px/1.35 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
.trkView[hidden]{display:none}
.trkTop{flex:0 0 auto;display:flex;align-items:center;gap:8px;padding:8px 10px;padding-top:max(8px,env(safe-area-inset-top));
  background:#121a22;border-bottom:1px solid #22303d;flex-wrap:wrap}
.trkTop .btn{background:#1e2a36;color:#e8eef5;border:1px solid #33475a;border-radius:6px;padding:6px 10px;font:600 12px/1 system-ui,sans-serif;cursor:pointer}
.trkTop .btn:hover,.trkTop .btn:focus-visible{background:#2a3a4a;outline:none}
.trkTop .btn.exit{background:#5a1f1f;border-color:#8a3434}
.trkTop .ttl{font:700 13px/1 ui-monospace,Menlo,monospace;letter-spacing:.06em}
.trkTop .st{color:#9fb0c2;font-size:12px;flex:1 1 200px;min-width:0}
.trkMapHost{position:relative;flex:1 1 auto;min-height:0}
.trkMapHost .trkNote{position:absolute;left:50%;top:12px;transform:translateX(-50%);background:rgba(10,14,20,.9);padding:6px 10px;border-radius:6px;z-index:800;font-size:12px}
.trkList{position:absolute;left:10px;bottom:calc(10px + env(safe-area-inset-bottom));z-index:700;width:min(430px,calc(100% - 20px));
  max-height:min(46%,420px);display:flex;flex-direction:column;background:rgba(10,14,20,.93);border:1px solid #2c3a4a;border-radius:8px;overflow:hidden}
.trkList .hd{display:flex;align-items:center;gap:8px;padding:6px 10px;font:700 11px/1.2 ui-monospace,Menlo,monospace;letter-spacing:.05em;
  color:#b9c7d5;cursor:pointer;user-select:none;border-bottom:1px solid #22303d}
.trkList .hd .n{color:#ffd34d}
.trkList .hd .mn{margin-left:auto;background:none;border:0;color:#9fb0c2;font-size:15px;cursor:pointer;padding:0 4px}
.trkList.min .rows,.trkList.min .safety{display:none}
.trkList.min .hd{border-bottom:0}
.trkList .rows{overflow:auto;flex:1 1 auto;min-height:0}
.trkList .safety{padding:6px 10px;background:#3a2a10;color:#ffe2a8;font-size:12px;border-bottom:1px solid #5a4316}
.trkRow{display:grid;grid-template-columns:minmax(0,1fr) auto auto auto;gap:2px 10px;align-items:center;padding:5px 10px;cursor:pointer;border-bottom:1px solid #151f29}
.trkRow:hover,.trkRow:focus-visible,.trkRow.sel{background:#1a2632;outline:none}
.trkRow.sel{box-shadow:inset 3px 0 0 #ffd34d}
.trkRow.nopos{opacity:.65}
.trkRow.em{background:#4a1515}
.trkRow .id{min-width:0}
.trkRow .id b{display:block;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-weight:600}
.trkRow .id small{display:block;color:#93a3b4;font-size:11px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.trkRow .sw{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:1px}
.trkRow .sp,.trkRow .rg,.trkRow .ag{font:12px/1.2 ui-monospace,Menlo,monospace;color:#cfd9e3;white-space:nowrap;text-align:right}
.trkRow .ag{color:#8596a7}
.trkRow .brg,.trkRow .dim{color:#8596a7}
.trkView i.up{color:#6dd36d;font-style:normal}
.trkView i.dn{color:#ff8a6b;font-style:normal}
.trkEmpty{padding:12px;color:#9fb0c2;font-size:12px}
.trkCard .t{font-size:13px;margin:0 18px 6px 0;color:#fff}
.trkCard .r{display:grid;grid-template-columns:7.2em minmax(0,1fr);gap:8px;padding:1px 0}
.trkCard .k{color:#8596a7}
.trkCard .v{color:#e3eaf1;word-break:break-word}
.trkCard .raw{margin-top:6px;padding:5px 6px;background:#0d151c;border-radius:4px;color:#9fb0c2;font-size:11px;word-break:break-all}
.trkView .flag{font-size:14px}
.trkView .cc{font:700 10px/1 ui-monospace,monospace;background:#2a3a4a;border-radius:3px;padding:1px 3px}
@media (max-width:520px){
  .trkList{left:6px;right:6px;width:auto;max-height:34%}
  .trkRow{grid-template-columns:minmax(0,1fr) auto auto}
  .trkRow .ag{display:none}
  .vtl-card{top:6px !important;right:6px !important;left:52px;max-width:none !important;max-height:30% !important;font-size:11px !important}
}
`;
function addCss() {
  if (document.getElementById('trk-style')) return;
  const st = document.createElement('style'); st.id = 'trk-style'; st.textContent = CSS; document.head.appendChild(st);
}
function runSource(src: string) { const el = document.createElement('script'); el.textContent = src; document.head.appendChild(el); }
const MIN_KEY = 'vibesdr.trkListMin';

export interface TrackViewOpts {
  title: string;                 // "AIS" / "APRS"
  exitLabel: string;             // "EXIT AIS" / "CLOSE MAP"
  listTitle: string;             // "SHIPS" / "STATIONS"
  onExit: () => void;
  icon: (it: any) => { html: string; w: number; h: number };
  label: (it: any) => string;
  card: (it: any) => string;
  trail?: (key: string, done: (pts: [number, number][]) => void) => void;
}
export class TrackView {
  readonly root: HTMLElement;
  private mapHost: HTMLElement;
  private list: HTMLElement;
  private rowsEl: HTMLElement;
  private safetyEl: HTMLElement;
  private statEl: HTMLElement;
  private countEl: HTMLElement;
  private gen = 0;
  private vm: any = null;
  private lmap: any = null;
  private layer: any = null;
  private pending: any[] | null = null;
  private rx: LatLon | null = null;
  private lastRows = '';
  private sel = '';
  private inerted: HTMLElement[] = [];

  constructor(private o: TrackViewOpts) {
    addCss();
    const r = document.createElement('div');
    r.className = 'trkView'; r.hidden = true;
    r.setAttribute('role', 'dialog'); r.setAttribute('aria-label', o.title + ' map');
    r.innerHTML = `<div class="trkTop"><button class="btn exit" type="button">${esc(o.exitLabel)}</button><span class="ttl">${esc(o.title)}</span>`
      + `<span class="st"></span><button class="btn fit" type="button" title="Fit every ${esc(o.listTitle.toLowerCase())} on the map">FIT</button></div>`
      + `<div class="trkMapHost"><div class="trkList"><div class="hd" role="button" tabindex="0"><span>${esc(o.listTitle)}</span> <span class="n">0</span>`
      + `<button class="mn" type="button" aria-label="Minimise the list">▾</button></div><div class="safety" hidden></div><div class="rows"></div></div></div>`;
    this.root = r;
    this.mapHost = r.querySelector('.trkMapHost') as HTMLElement;
    this.list = r.querySelector('.trkList') as HTMLElement;
    this.rowsEl = r.querySelector('.rows') as HTMLElement;
    this.safetyEl = r.querySelector('.safety') as HTMLElement;
    this.statEl = r.querySelector('.st') as HTMLElement;
    this.countEl = r.querySelector('.hd .n') as HTMLElement;
    (r.querySelector('.exit') as HTMLButtonElement).onclick = () => o.onExit();
    (r.querySelector('.fit') as HTMLButtonElement).onclick = () => { try { this.layer?.fit(); } catch (e) { console.error('[tracks] fit', e); } };
    const hd = r.querySelector('.hd') as HTMLElement;
    const toggle = () => this.setMin(!this.list.classList.contains('min'));
    hd.onclick = toggle;
    hd.onkeydown = (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggle(); } };
    this.rowsEl.addEventListener('click', (e) => this.rowPick(e));
    this.rowsEl.addEventListener('keydown', (e) => { if ((e as KeyboardEvent).key === 'Enter') this.rowPick(e); });
    for (const ev of ['mousedown', 'wheel', 'touchstart', 'dblclick', 'pointerdown']) this.list.addEventListener(ev, (e) => e.stopPropagation(), { passive: true });
    r.addEventListener('keydown', (e) => { if (e.key === 'Escape') { e.stopPropagation(); if (this.sel) this.select(''); else o.onExit(); } });
    document.body.appendChild(r);
    let min = false; try { min = localStorage.getItem(MIN_KEY) === '1'; } catch { /* private window */ }
    this.setMin(min, false);
  }
  get open() { return !this.root.hidden; }
  private setMin(min: boolean, save = true) {
    this.list.classList.toggle('min', min);
    (this.list.querySelector('.mn') as HTMLElement).textContent = min ? '▴' : '▾';
    if (save) try { localStorage.setItem(MIN_KEY, min ? '1' : '0'); } catch { /* private window */ }
  }
  private rowPick(e: Event) {
    const row = (e.target as HTMLElement).closest('.trkRow') as HTMLElement | null;
    if (!row) return;
    const k = row.dataset.key || '';
    if (!this.layer?.focus(k)) this.note('Not on the map — no position heard yet.', 2500);
    this.markSel(k);
  }
  private markSel(k: string) {
    this.sel = k;
    for (const el of Array.from(this.rowsEl.querySelectorAll('.trkRow.sel'))) el.classList.remove('sel');
    const cssEsc = (globalThis as any).CSS?.escape as ((v: string) => string) | undefined;
    if (k) this.rowsEl.querySelector(`.trkRow[data-key="${cssEsc ? cssEsc(k) : k.replace(/["\\]/g, '')}"]`)?.classList.add('sel');
  }
  private select(k: string) { this.layer?.select(k || null); this.markSel(k); }
  private wantFocus = '';
  /** Select an item and fly to it (a row tapped outside the map, e.g. the APRS box). Before the map is up: when it is. */
  focus(key: string): boolean {
    if (!this.layer) { this.wantFocus = key; return false; }
    const ok = !!this.layer.focus(key);
    if (ok) this.markSel(key);
    return ok;
  }
  private noteT: ReturnType<typeof setTimeout> | null = null;
  note(text: string, ms = 0) {
    let n = this.mapHost.querySelector('.trkNote') as HTMLElement | null;
    if (!text) { n?.remove(); return; }
    if (!n) { n = document.createElement('div'); n.className = 'trkNote'; this.mapHost.appendChild(n); }
    n.textContent = text;
    if (this.noteT) clearTimeout(this.noteT);
    if (ms) this.noteT = setTimeout(() => n?.remove(), ms);
  }

  show(rx: LatLon | null) {
    this.rx = rx;
    if (this.open) return;
    this.root.hidden = false;
    // ★ the page under the map is not reachable while the map covers it (keyboard, a screen reader)
    this.inerted = [];
    for (const el of Array.from(document.body.children) as HTMLElement[]) {
      if (el === this.root || el.tagName === 'SCRIPT' || el.tagName === 'STYLE' || el.inert) continue;
      if (el.classList.contains('trkView') || el.id === 'pill' || el.id === 'toast') continue;
      el.inert = true; this.inerted.push(el);
    }
    (this.root.querySelector('.exit') as HTMLElement).focus();
    this.boot(++this.gen).catch((e) => { console.error('[tracks] the map could not load', e); this.note('The map did not finish loading — the list still updates.'); });
  }
  hide() {
    if (!this.open) return;
    this.root.hidden = true;
    this.attribObs?.disconnect(); this.attribObs = null; this.list.style.bottom = '';
    for (const el of this.inerted) el.inert = false;
    this.inerted = [];
    this.gen++;
    try { this.layer?.destroy(); } catch (e) { console.error('[tracks] layer.destroy', e); }
    this.layer = null;
    try { this.vm?.destroy(); } catch (e) { console.error('[tracks] map destroy', e); }
    this.vm = null;
    try { this.lmap?.remove(); } catch (e) { console.error('[tracks] Leaflet remove', e); }
    this.lmap = null;
    for (const el of Array.from(this.mapHost.children)) if (el !== this.list) el.remove();
    this.mapHost.className = 'trkMapHost';
    this.lastRows = ''; this.sel = ''; this.pending = null;
  }
  destroy() { this.hide(); this.root.remove(); }

  private async boot(my: number) {
    const w = window as any;
    const { TRACK_LAYER_JS } = await import('./generated/trackLayerSource');
    if (my !== this.gen) return;
    if (!w.VibeTrackLayer) runSource(TRACK_LAYER_JS);
    const holder = document.createElement('div');
    holder.style.cssText = 'position:absolute;inset:0';
    this.mapHost.insertBefore(holder, this.list);
    const rx = this.rx;
    const kit = await probeMapGL();
    if (my !== this.gen) return;
    if (kit && await loadMapGLScripts(kit.base)) {
      if (my !== this.gen) return;
      try {
        const vm = w.VibeMapGL.createNow(holder, { style: kit.style, load: mapglLoad, base: kit.base, rangeBase: kit.base, detail: kit.detail,
                                                   center: rx ? [rx.lon, rx.lat] : [0, 30], zoom: rx ? 7 : 1.5 });
        if (vm) {
          this.vm = vm;
          vm.ready.catch((e: unknown) => console.error('[tracks] GPU map: the style did not load', e));
          vm.map.addControl(new w.maplibregl.NavigationControl({ showCompass: false }), 'top-left');
          const L = w.VibeMapGLCompat.install(vm, { leafletZoomOffset: 1 });
          this.attach(L.map(), L);
          return;
        }
      } catch (e) {
        console.error('[tracks] GPU map failed — using the Leaflet map', e);
        try { this.vm?.destroy(); } catch { /* tidy */ }
        this.vm = null; holder.innerHTML = '';
      }
    }
    if (!await loadLeafletInto(document)) { if (my === this.gen) this.note('The map could not be loaded on this server — the list still updates.'); return; }
    if (my !== this.gen) return;
    const L = w.L;
    this.lmap = L.map(holder, { worldCopyJump: true, preferCanvas: true });
    this.lmap.setView(rx ? [rx.lat, rx.lon] : [30, 0], rx ? 8 : 2);
    if (my !== this.gen) return;
    // ★ the shared map data (coastlines, places) — as AdsbMap draws it under Leaflet
    if (!w.VibeMap) { try { const { VIBEMAP_JS } = await loadVibemapSource(); if (my !== this.gen) return; if (!w.VibeMap) runSource(VIBEMAP_JS); } catch (e) { console.error('[tracks] vibemap', e); } }
    if (w.VibeMap) w.VibeMap.attach(this.lmap, { dataBase: '/mapdata/v1/' });
    this.attach(this.lmap, L);
  }
  private attach(map: any, L: any) {
    const w = window as any;
    this.layer = w.VibeTrackLayer.create(map, L, {
      receiver: this.rx, icon: this.o.icon, label: this.o.label, card: this.o.card, trail: this.o.trail,
      onSelect: (k: string | null) => this.markSel(k || ''),
    });
    if (this.pending) { this.layer.update(this.pending); this.pending = null; }
    this.watchAttrib();
    if (this.wantFocus) { const k = this.wantFocus; this.wantFocus = ''; setTimeout(() => this.focus(k), 50); }
  }

  /** ★ The map's attribution (a licence line we may not hide) sits at the bottom; on a phone it wraps to three lines and
   *  the list's last row went under it. The list stands just above it, however tall it grows. */
  private attribObs: ResizeObserver | null = null;
  private watchAttrib() {
    this.attribObs?.disconnect(); this.attribObs = null;
    const at = this.mapHost.querySelector('.leaflet-control-attribution, .maplibregl-ctrl-attrib') as HTMLElement | null;
    if (!at || typeof ResizeObserver === 'undefined') return;
    const sync = () => {
      const h = at.getBoundingClientRect().height;
      const wide = this.mapHost.clientWidth > 520;
      // ★ on a wide map the attribution is to the right of the list; only where they share the width does it push up
      this.list.style.bottom = !wide && h > 0 ? `calc(${Math.round(h) + 6}px + env(safe-area-inset-bottom))` : '';
    };
    this.attribObs = new ResizeObserver(sync);
    this.attribObs.observe(at); this.attribObs.observe(this.mapHost);
    sync();
  }

  /** Every report: the whole list (items with key/lat/lon), the rows' HTML, the status line, the count. */
  update(items: any[], rowsHtml: string, status: string, count: number, rx: LatLon | null, safety = '') {
    if (rx && (!this.rx || rx.lat !== this.rx.lat || rx.lon !== this.rx.lon)) { this.rx = rx; try { this.layer?.setReceiver(rx.lat, rx.lon); } catch (e) { console.error('[tracks] rx', e); } }
    if (this.layer) { try { this.layer.update(items); } catch (e) { console.error('[tracks] layer.update', e); } }
    else this.pending = items;
    if (rowsHtml !== this.lastRows) {
      const top = this.rowsEl.scrollTop;
      this.rowsEl.innerHTML = rowsHtml; this.lastRows = rowsHtml;
      this.rowsEl.scrollTop = top;
      if (this.sel) this.markSel(this.sel);
    }
    if (this.statEl.textContent !== status) this.statEl.textContent = status;
    this.countEl.textContent = String(count);
    this.safetyEl.hidden = !safety;
    if (safety && this.safetyEl.innerHTML !== safety) this.safetyEl.innerHTML = safety;
  }
}

/** ★ The AIS view's safety strip: the newest safety-related broadcasts (types 12/14) — a SART's "SART ACTIVE" first. */
export function aisSafetyHtml(r: AisReport): string {
  if (!r.safety.length) return '';
  return r.safety.slice(-3).reverse().map((s) => `<div>⚠ <b>${esc(s.name || 'MMSI ' + s.mmsi)}</b>: ${esc(s.text)} <span style="opacity:.7">· ${esc(ago(s.age))} ago</span></div>`).join('');
}
