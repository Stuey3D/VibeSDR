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
import { dataUrl } from './origin';
import { padRehighlight } from './gamepad';
import { loadVibemapSource } from './chunkCache';
import { MAP_AUTOFIT_JS } from '../../../src/utils/mapAutoFit';
import { MESH_ROUTE_JS } from '../../../src/utils/meshRoute';   // ★★ beta96 a mesh node's route, drawn hop by hop   // ★ beta92 ONE auto fit for every map (the ADS-B map's)
import { flagEmoji } from '../../../src/services/adsbInfo';
import { snrChipHtml } from '../../../src/utils/snrColour';   // ★ beta96 the colour-coded SNR per message
import { aprsColour, aprsIcon as aprsSymIcon, aprsAltPlausible } from '../../../src/utils/aprsSymbols';
import { CARD_IDS, CARD_LABEL, CARD_NARROW_W, CARD_W, CARD_W_SNUG, CARD_ROOMY_W, cardPlace, oneSide, MAP_HELP, MAP_HELP_NOTES, cardDefaults, cardLimit as cardLimitFor, cardToggle, trimOutliers, type CardId } from '../../../src/utils/mapCards';
import { modeIconSvg } from '../../../src/utils/modeIcons';
import { edgeDockSet, wireEdgeDock } from './healthTuck';
import { TUNE_ICON_PATH } from '../../../src/services/airSeaTune';
import { aisDxSanitise, aisDxState, aisDxWords, AIS_DX_COLOUR, AIS_DX_SHORT } from '../../../src/services/aisDx';

/** ★★ b96-rangefeed `approx`: words for a position that is not exact (the locator square's centre — rxPosition.ts) */
export type LatLon = { lat: number; lon: number; approx?: string };

// ── the report shapes (vibe_ais_service.h / vibe_aprs_run.h) ──────────────────────────────────────────────────────
export interface AisVessel {
  mmsi: number; kind: string; name?: string; call?: string; dest?: string; imo?: number; flag?: string; country?: string;
  stype?: number; stypeName?: string; family?: string; nav?: number; navName?: string; aton?: number; atonName?: string;
  virtual?: number; offpos?: number; lat?: number; lon?: number; sog?: number; cog?: number; hdg?: number; rot?: number; turn?: number;
  draught?: number; len?: number; beam?: number; eta?: string; alt?: number; trend?: number; line?: string;
  ch?: string; level?: number; snr?: number; seen: number; age: number; posAge: number; count: number; trail?: number;
  /** ★ V12 2026-11-16 type 21's status byte (IALA A-126): light 0 none · 1 on · 2 off · 3 fault; RACON 0 none · 1 not
   *  monitored · 2 working · 3 fault; alarm 1. */
  light?: number; racon?: number; alarm?: number;
  /** ★ V12 2026-11-16 the station's weather (binary message 8, Circ.289 met/hydro), newest site first. */
  met?: AisMet[];
  /** ★ beta95 believed or not (aisDx.ts — the server's rule): dx · unconf (uLat/uLon) · held · fakes */
  dx?: number; unconf?: number; uLat?: number; uLon?: number; held?: number; fakes?: number;
}
/** One met/hydro report (vibe_ais_asm.h metJson): every field optional — a station sends what its sensors have. */
export interface AisMet {
  fi: number; age: number; lat?: number; lon?: number; obs?: string;
  wind?: number; gust?: number; windDir?: number; gustDir?: number; airC?: number; hum?: number; dewC?: number;
  hPa?: number; pTrend?: number; vis?: number; visMore?: number; level?: number; levelTrend?: number; cur?: number; curDir?: number;
  wave?: number; waveS?: number; waveDir?: number; swell?: number; swellS?: number; swellDir?: number; sea?: number;
  waterC?: number; precip?: number; sal?: number; ice?: number;
}
export interface AisNotice { age: number; mmsi: number; name: string; text: string; fi: number; what?: number; lat?: number; lon?: number; to?: number }
const MET_KEYS = ['lat', 'lon', 'wind', 'gust', 'windDir', 'gustDir', 'airC', 'hum', 'dewC', 'hPa', 'pTrend', 'vis', 'visMore', 'level',
  'levelTrend', 'cur', 'curDir', 'wave', 'waveS', 'waveDir', 'swell', 'swellS', 'swellDir', 'sea', 'waterC', 'precip', 'sal', 'ice'] as const;
export function aisMetSanitise(a: unknown): AisMet[] | undefined {
  if (!Array.isArray(a)) return undefined;
  const out: AisMet[] = [];
  for (const m of a.slice(0, 4)) {
    if (!m || typeof m !== 'object') continue;
    const o: any = { fi: num((m as any).fi) ?? 0, age: num((m as any).age) ?? 0 };
    const obs = str((m as any).obs, 12); if (/^\d\d \d\d:\d\d$/.test(obs)) o.obs = obs;
    for (const k of MET_KEYS) { const n = num((m as any)[k]); if (n != null) o[k] = n; }
    out.push(o);
  }
  return out.length ? out : undefined;
}
export interface AisReport {
  type: 'ais'; rate: number; now: number; msgs: number; msgRate: number; chA: number; chB: number; repaired: number;
  dropped: number; capture: number; noise: number; vessels: AisVessel[]; safety: { age: number; mmsi: number; name: string; text: string; to?: number }[];
  gain?: number; gainAuto?: number;
  notices?: AisNotice[];
  /** ★★★ beta96 the maritime combination: "ais+dsc" while AIS runs with DSC channel 70 (vibe_maritime_combo.h); its calls so far. */
  combo?: 'ais+dsc'; dscCalls?: number;
  /** ★ beta96 2 = a vessel's "snr" is its last frame's (vibe_ais.h burstSnr_); older servers read the floor AFTER the
   *  frame (~3 dB whatever the ship) — no chip from them. */
  snrv?: number;
}
export interface AprsStation {
  key: string; from: string; kind: string; name?: string; sym: string; symName?: string; lat?: number; lon?: number;
  course?: number; speed?: number; alt?: number; altBad?: number; comment?: string; status?: string; micE?: string; path?: string; via?: string;
  raw?: string; line?: string; wx?: Record<string, number>; killed?: number; msg?: number; seen: number; age: number; count: number;
  posAge: number; trail?: [number, number][];
  /** ★ beta96 its last frame's SNR (dB, the carrier over the widest IQ filter's floor) — none from an audio-only frame */
  snr?: number;
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
      level: num(v.level) ?? undefined, snr: (num(m.snrv) ?? 0) >= 2 ? num(v.snr) ?? undefined : undefined, seen: num(v.seen) ?? 0, age: num(v.age) ?? 0,
      posAge: num(v.posAge) ?? -1, count: num(v.count) ?? 0, trail: num(v.trail) ?? undefined,
      light: num(v.light) ?? undefined, racon: num(v.racon) ?? undefined, alarm: num(v.alarm) ?? undefined, met: aisMetSanitise(v.met),
      ...aisDxSanitise(v),   // ★ beta95 DX / unconfirmed / held / implausible
    });
  }
  const safety = Array.isArray(m.safety) ? m.safety.slice(0, 20).map((s: any) => ({ age: num(s?.age) ?? 0, mmsi: num(s?.mmsi) ?? 0, name: str(s?.name, 40), text: str(s?.text, 200), to: num(s?.to) ?? undefined })) : [];
  const notices: AisNotice[] = Array.isArray(m.notices) ? m.notices.slice(-20).map((s: any) => ({ age: num(s?.age) ?? 0, mmsi: num(s?.mmsi) ?? 0, name: str(s?.name, 40),
    text: str(s?.text, 300), fi: num(s?.fi) ?? 0, what: num(s?.what) ?? undefined, lat: num(s?.lat) ?? undefined, lon: num(s?.lon) ?? undefined, to: num(s?.to) ?? undefined })) : [];
  return { notices, type: 'ais', rate: num(m.rate) ?? 0, now: num(m.now) ?? 0, msgs: num(m.msgs) ?? 0, msgRate: num(m.msgRate) ?? 0,
           chA: num(m.chA) ?? 0, chB: num(m.chB) ?? 0, repaired: num(m.repaired) ?? 0, dropped: num(m.dropped) ?? 0,
           capture: num(m.capture) ?? -1, noise: num(m.noise) ?? 0, vessels: vs, safety,
           gain: num(m.gain) ?? undefined, gainAuto: num(m.gainAuto) ?? undefined, snrv: num(m.snrv) ?? undefined,
           ...(m.combo === 'ais+dsc' ? { combo: 'ais+dsc' as const, dscCalls: num(m.dsc?.calls) ?? 0 } : {}) };
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
              alt: num(s.alt) ?? undefined, altBad: s.altBad === 1 ? 1 : undefined, comment: str(s.comment, 200), status: str(s.status, 200), micE: str(s.micE, 20), path: str(s.path, 80),
              via: str(s.via, 16), raw: str(s.raw, 400), line: str(s.line, 300), wx: Object.keys(wx).length ? wx : undefined,
              killed: num(s.killed) ?? undefined, msg: num(s.msg) ?? undefined, seen: num(s.seen) ?? 0, age: num(s.age) ?? 0,
              count: num(s.count) ?? 0, posAge: num(s.posAge) ?? -1, trail, ...(num(s.snr) != null ? { snr: num(s.snr)! } : {}) });
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
  const ic = aisIconBase(v);
  // ★ beta95 a DX ship stands out (Stuart: "they are the exciting ones"): a gold halo round whatever it is
  return v.dx ? { ...ic, html: `<span style="display:inline-block;filter:drop-shadow(0 0 2px ${AIS_DX_COLOUR}) drop-shadow(0 0 3px ${AIS_DX_COLOUR})">${ic.html}</span>` } : ic;
}
function aisIconBase(v: AisVessel): { html: string; w: number; h: number } {
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
/** ★ beta95 the map's label: the name, and DX when it is one. */
export function aisLabel(v: AisVessel): string { return v.dx ? `${aisName(v)} · DX` : aisName(v); }
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
  } else if (v.unconf && v.uLat != null && v.uLon != null) {
    add('Claims', esc(fmtPos(v.uLat, v.uLon)) + (rx ? ` · ${Math.round(rangeBearing(rx, v.uLat, v.uLon).nm)} NM ${compass8(rangeBearing(rx, v.uLat, v.uLon).brg)}` : ''));
  }
  { const w = aisDxWords(v); if (w) add(v.dx ? '<b style="color:' + AIS_DX_COLOUR + '">DX</b>' : 'Position?', esc(w)); }   // ★ beta95
  // ★ V12 2026-11-16 the AtoN's own status (type 21) and its weather (binary message 8)
  if (v.light) add('Light', v.light === 3 ? '<b style="color:#ff6b5e">FAULT</b>' : v.light === 1 ? 'on' : 'off');
  if (v.racon) add('RACON', v.racon === 3 ? '<b style="color:#ff6b5e">FAULT</b>' : v.racon === 2 ? 'working' : 'fitted');
  if (v.alarm) add('Status', '<b style="color:#ff6b5e">ALARM</b>');
  for (const m of v.met || []) {
    const far = m.lat != null && m.lon != null && v.lat != null && v.lon != null ? rangeBearing({ lat: v.lat, lon: v.lon }, m.lat, m.lon) : null;
    const where = far && far.nm >= 0.5 ? ` <span style="opacity:.7">at ${esc(fmtPos(m.lat, m.lon))}</span>` : '';
    add('Weather', `${esc(heardAgo(m.age))}${m.obs ? ` · obs ${esc(m.obs)} UTC` : ''}${where}`);
    for (const [k, val] of aisMetRows(m)) add(k, esc(val));
  }
  add('Heard', `${heardAgo(v.seen)} · ${v.count} message${v.count === 1 ? '' : 's'} · AIS ${v.ch === 'B' ? '2' : '1'}${v.snr != null ? ` · ${Math.round(v.snr)} dB` : ''}`);
  return `<div class="trkCard"><div class="t">${flagHtml(v.flag)}<b>${esc(aisName(v))}</b></div>${rows.join('')}</div>`;
}
const C16 = ['N', 'NNE', 'NE', 'ENE', 'E', 'ESE', 'SE', 'SSE', 'S', 'SSW', 'SW', 'WSW', 'W', 'WNW', 'NW', 'NNW'];
const compass16 = (d: number) => C16[Math.round(((d % 360) + 360) % 360 / 22.5) % 16];
const TREND = ['steady', 'falling', 'rising'];
const PRECIP = ['', 'rain', 'thunderstorm', 'freezing rain', 'mixed / ice', 'snow', ''];
/** ★ One met report as card rows, plain words, only what the station sent: [label, value]. Shared shape with the app. */
export function aisMetRows(m: AisMet): [string, string][] {
  const r: [string, string][] = [];
  if (m.wind != null) r.push(['Wind', `${m.windDir != null ? compass16(m.windDir) + ' ' : ''}${m.wind} kn${m.gust != null && m.gust > m.wind ? `, gusting ${m.gust}` : ''}`]);
  if (m.airC != null) r.push(['Air', `${m.airC.toFixed(1)} °C${m.hum != null ? ` · humidity ${m.hum} %` : ''}${m.dewC != null ? ` · dew point ${m.dewC.toFixed(1)} °C` : ''}`]);
  if (m.hPa != null) r.push(['Pressure', `${m.hPa} hPa${m.pTrend != null && TREND[m.pTrend] ? ' ' + TREND[m.pTrend] : ''}`]);
  if (m.vis != null) r.push(['Visibility', `${m.visMore ? 'over ' : ''}${m.vis.toFixed(1)} NM`]);
  if (m.level != null) r.push(['Tide', `${m.level.toFixed(2)} m${m.levelTrend != null && TREND[m.levelTrend] ? ' ' + TREND[m.levelTrend] : ''}`]);
  if (m.cur != null) r.push(['Current', `${m.cur.toFixed(1)} kn${m.curDir != null ? ' ' + compass16(m.curDir) : ''}`]);
  if (m.wave != null) r.push(['Waves', `${m.wave.toFixed(1)} m${m.waveS != null ? ` · ${m.waveS} s` : ''}${m.waveDir != null ? ' from ' + compass16(m.waveDir) : ''}`]);
  if (m.swell != null) r.push(['Swell', `${m.swell.toFixed(1)} m${m.swellS != null ? ` · ${m.swellS} s` : ''}${m.swellDir != null ? ' from ' + compass16(m.swellDir) : ''}`]);
  if (m.sea != null) r.push(['Sea state', `Beaufort ${m.sea}`]);
  if (m.waterC != null) r.push(['Sea temperature', `${m.waterC.toFixed(1)} °C`]);
  if (m.precip != null && PRECIP[m.precip]) r.push(['Precipitation', PRECIP[m.precip]]);
  if (m.sal != null) r.push(['Salinity', `${m.sal.toFixed(1)} ‰`]);
  if (m.ice != null) r.push(['Ice', m.ice ? 'yes' : 'none']);
  return r;
}
/** ★ The list's short weather word for a station: "SW 14 kn · tide 3.21 m". */
export function aisMetShort(m: AisMet | undefined): string {
  if (!m) return '';
  const p: string[] = [];
  if (m.wind != null) p.push(`${m.windDir != null ? compass16(m.windDir) + ' ' : ''}${m.wind} kn`);
  if (m.level != null) p.push(`tide ${m.level.toFixed(2)} m`);
  else if (m.vis != null) p.push(`vis ${m.vis.toFixed(1)} NM`);
  return p.join(' · ');
}
export function aisStatus(r: AisReport | null): string {
  if (!r) return 'listening on 161.975 and 162.025 MHz…';
  const pos = r.vessels.filter((v) => v.lat != null).length;
  const cap = r.capture >= 0 && r.capture < 97 ? ` · capture ${Math.round(r.capture)} %` : '';
  // ★★★ beta96 the maritime combination: channel 70's calls beside the ships (each call is on the map's DSC layer)
  const dsc = r.combo === 'ais+dsc' ? ` · DSC ch 70: ${r.dscCalls ?? 0} ${(r.dscCalls ?? 0) === 1 ? 'call' : 'calls'}` : '';
  return `${r.vessels.length} heard · ${pos} on the map · ${r.msgRate.toFixed(r.msgRate < 10 ? 1 : 0)} msg/min${cap}${dsc}`;
}
export function aisRows(r: AisReport, rx: LatLon | null): string {
  if (!r.vessels.length) return '<div class="trkEmpty">No ships heard yet. Each one appears as its transponder is decoded — a class A ship reports every few seconds under way, every three minutes at anchor.</div>';
  const rows = r.vessels.map((v) => ({ v, rb: rx && v.lat != null && v.lon != null ? rangeBearing(rx, v.lat, v.lon) : null }));
  rows.sort((a, b) => (a.rb && b.rb ? a.rb.km - b.rb.km : a.rb ? -1 : b.rb ? 1 : a.v.seen - b.v.seen));
  return rows.map(({ v, rb }) => {
    const col = v.kind === 'sart' ? '#ff3b3b' : AIS_COLOURS[v.family || 'other'] || AIS_COLOURS.other;
    const sp = v.sog != null && v.sog >= 0.1 ? `${v.sog.toFixed(v.sog < 10 ? 1 : 0)} kn${v.cog != null && v.sog >= 0.5 ? ' ' + compass8(v.cog) : ''}` : v.lat != null ? 'still' : '';
    // ★★ beta95 DX (aisDx.ts): the tag in the range column, which never truncates; unconfirmed / implausible say so there
    const dxs = aisDxState(v);
    const ub = dxs === 'unconf' && rx && v.uLat != null && v.uLon != null ? rangeBearing(rx, v.uLat, v.uLon) : null;
    const where = snrChipHtml('ais', v.snr) + (rb ? `${dxs === 'dx' ? '<span class="dxTag">DX</span>' : ''}${rb.nm < 10 ? rb.nm.toFixed(1) : Math.round(rb.nm)} NM <span class="brg">${compass8(rb.brg)}</span>`
      : v.lat != null ? (dxs === 'dx' ? '<span class="dxTag">DX</span>' : '')
      : dxs === 'unconf' ? `<span class="dim" title="${esc(aisDxWords(v))}">${AIS_DX_SHORT.unconf}${ub ? ` ${Math.round(ub.nm)} NM ${compass8(ub.brg)}` : ''}</span>`
      : dxs === 'implausible' ? `<span class="dim" title="${esc(aisDxWords(v))}">${AIS_DX_SHORT.implausible}</span>`
      : '<span class="dim">no position</span>');   // ★ beta96 the last frame's SNR chip first (the column never truncates)
    const second = [aisKindWord(v), dxs === 'unconf' ? 'unconfirmed position' : dxs === 'implausible' ? 'position implausible' : '',
      v.navName && v.nav !== 0 && v.nav !== 15 ? v.navName : '', v.dest ? '→ ' + v.dest : '', aisMetShort(v.met?.[0])].filter(Boolean).map(esc).join(' · ');
    return `<div class="trkRow${v.lat == null ? ' nopos' : ''}${v.kind === 'sart' ? ' em' : ''}${dxs === 'dx' ? ' dx' : ''}" role="button" tabindex="0" data-key="${esc(v.mmsi)}">`
      + `<span class="id"><b><i class="sw" style="background:${col}"></i>${flagHtml(v.flag)}${esc(aisName(v))}</b><small>${second || 'MMSI ' + esc(v.mmsi)}</small></span>`
      + `<span class="sp">${esc(sp)}${trendMark(v.trend)}</span><span class="rg">${where}</span></div>`;
  }).join('');
}

// ── APRS: icons, rows, cards ──────────────────────────────────────────────────────────────────────────────────────
/** ★★ beta92 THE SYMBOLS ARE ONE SET for the app and the web page — src/utils/aprsSymbols.ts (28 px, a square badge for
 *  digipeaters / gateways, solid glyphs with cut details, a light halo for the satellite map; our own drawings). */
export { aprsColour };
export function aprsIcon(s: AprsStation): { html: string; w: number; h: number } { return aprsSymIcon(s); }
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
  // ★★ beta95 an altitude beyond sense for the symbol (aprsAltPlausible): shown, marked, never as real — no ▲/▼ for it
  if (s.alt != null) add('Altitude', aprsAltPlausible(s) ? `${Math.round(s.alt)} m${aprsAltCue(s)}`
    : `<span style="opacity:.6;text-decoration:line-through">${Math.round(s.alt).toLocaleString('en-GB')} m</span> <b style="color:#ffad42">implausible${s.symName ? ' for a ' + esc(s.symName) : ''}</b>`);
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
    const where = snrChipHtml('aprs', s.snr)   // ★ beta96 its last frame's SNR (IQ only)
      + (rb ? `${rb.km < 10 ? rb.km.toFixed(1) : Math.round(rb.km)} km <span class="brg">${compass8(rb.brg)}</span>` : s.lat != null ? '' : '<span class="dim">no position</span>');
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
.trkTop .scopeNote{color:#ffd34d;font-size:12px;flex:1 0 100%}
.trkTop .scopeNote[hidden]{display:none}
.trkTop .scr{display:contents}
.trkTop .slot-share{display:inline-flex;align-items:center;gap:6px;flex:none}
.trkTop .slot-share:empty,.trkTop .slot-share>[hidden]{display:none}
.trkTop .mapShLine{background:rgba(255,190,80,.10);color:#ffcf7a;border-color:rgba(255,190,80,.45);font:700 10.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.1em;white-space:nowrap}
.trkTop .mapShLine.alone{color:#7bd88f;border-color:rgba(123,216,143,.45);background:rgba(123,216,143,.08)}
.trkTop .mapShLine.locked{color:#ff9d8a;border-color:rgba(255,120,100,.55);background:rgba(255,90,70,.10)}
.trkTop .btn.dialLocked{opacity:.4;filter:grayscale(.7);cursor:not-allowed}
.trkTop .tune[hidden]{display:none}
.trkTop .tune{display:inline-flex;flex:none}
.trkTop .tgl{align-items:center}
.trkTop .tgl .lbl{align-self:center;margin-right:6px;font:800 9.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.12em;padding:3px 5px;border-radius:4px;flex:none;background:#1d3347;color:#bfe3ff}
.trkTop .tgl .lbl+.grp{margin-left:0}
.trkTop .tuneChip{background:#3a1814;border-color:#a3473a;color:#ffd9d2;font:700 11px/1 ui-monospace,Menlo,monospace;letter-spacing:.06em;white-space:nowrap}
.trkTop .tuneChip:hover,.trkTop .tuneChip:focus-visible{background:#4d1f19}
.tunePop{position:fixed;z-index:950;max-height:min(70vh,520px);overflow:auto;background:#101820;border:1px solid #2c3e52;border-radius:10px;box-shadow:0 10px 30px rgba(0,0,0,.55);padding:6px}
.tunePop[hidden]{display:none}
.tunePop .tpG{font:700 9.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.12em;color:#7f93a7;padding:8px 8px 4px}
.tunePop .tpRow{display:flex;align-items:center;gap:9px;width:100%;text-align:left;background:none;border:0;border-radius:6px;color:#e8eef5;padding:8px;font:600 13px/1.2 system-ui,sans-serif;cursor:pointer}
.tunePop .tpRow:hover,.tunePop .tpRow:focus-visible{background:#1e2a36;outline:none}
.tunePop .tpRow svg{flex:none;color:#9fc6e6}
.tunePop .tpRow .nm{flex:none}
.tunePop .tpRow .fq{flex:1 1 auto;color:#9fb0c2;font:12px/1.2 ui-monospace,Menlo,monospace;text-align:right}
.tunePop .tpRow .lv{flex:none;font:800 9px/1 ui-monospace,Menlo,monospace;letter-spacing:.1em;color:#111;background:#ff6b5a;border-radius:3px;padding:3px 4px}
.tunePop .tpRow.live{background:rgba(255,107,90,.12)}
.tunePop .tpRow[aria-disabled="true"]{opacity:.42;cursor:not-allowed}
.trkTop .tgl .btn.chip{border-radius:999px!important;margin-left:3px!important;display:inline-flex;align-items:center;gap:5px;padding:5px 9px}
.trkTop .tgl .btn.chip .dot{width:8px;height:8px;border-radius:50%;display:inline-block}
.trkTop .tgl .btn.chip[aria-pressed="true"]{background:#1e2a36;color:#e8eef5;border-color:#7fc8ff}
.trkTop .tgl .btn.chip[aria-pressed="false"]{opacity:.45}
.trkTop .tgl .btn.chip[aria-pressed="false"] .dot{background:transparent!important;border:1px solid currentColor}
.trkTop .tgl .btn.chip .n{font-size:9.5px;opacity:.7;font-variant-numeric:tabular-nums}
.trkTop .tgl .btn.chip .n:empty{display:none}
.trkKey{position:absolute;right:10px;bottom:30px;z-index:900;background:rgba(11,20,32,.92);border:1px solid #2c3e52;border-radius:8px;color:#d7dee6;font:12px/1.3 inherit;max-height:60%;overflow:auto}
.trkKey>summary{cursor:pointer;padding:6px 10px;font-weight:600;letter-spacing:.08em;list-style:none}
.trkKey>summary::-webkit-details-marker{display:none}
.trkKey .kb{padding:2px 10px 8px}
.trkKey .kr{display:flex;align-items:center;gap:8px;padding:2px 0;white-space:nowrap}
.trkKey .kr svg{flex:none}
.trkTop .rng{display:inline-flex;gap:0}
.trkTop .rng .btn{border-radius:0;margin-left:-1px;padding:6px 8px}
.trkTop .rng .btn:first-child{border-radius:6px 0 0 6px;margin-left:0}
.trkTop .rng .btn:last-child{border-radius:0 6px 6px 0}
.trkTop .rng .btn[aria-pressed="true"]{background:#ffd34d;color:#111;border-color:#ffd34d}
.trkTop .tgl .btn[aria-pressed="true"]{background:#7fc8ff;border-color:#7fc8ff}
.trkTop .rng[hidden],.trkKey[hidden]{display:none}
.trkTop .tgl{max-width:100%;overflow-x:auto;scrollbar-width:none}
.trkTop .tgl::-webkit-scrollbar{display:none}
.trkTop .tgl .btn{flex:none}
.trkTop .tgl .grp{align-self:center;margin:0 4px 0 8px;font:700 9.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.08em;color:#7f93a7}
.trkTop .tgl .grp:first-child{margin-left:0}
.trkTop .tgl .btn.live{box-shadow:inset 0 -3px 0 #ff5a4d}
.trkTop .tgl .btn.live::before{content:"";display:inline-block;width:6px;height:6px;border-radius:50%;background:#ff3b2f;margin-right:5px;vertical-align:1px;animation:trkLive 1.6s ease-in-out infinite}
@keyframes trkLive{50%{opacity:.35}}
@media (prefers-reduced-motion:reduce){.trkTop .tgl .btn.live::before{animation:none}}
.trkList .hd .tabs{display:inline-flex;gap:2px;flex-wrap:wrap}
.trkList .hd .tab{background:none;border:0;border-bottom:2px solid transparent;color:#8fa2b5;font:700 11px/1.2 ui-monospace,Menlo,monospace;letter-spacing:.05em;padding:2px 5px;cursor:pointer}
.trkList .hd .tab[aria-selected="true"]{color:#e8eef5;border-bottom-color:#ffd34d}
.trkList .hd .tab .tn{color:#ffd34d;margin-left:4px}
.trkList .hd .tab:focus-visible,.trkList .hd .srt button:focus-visible{outline:2px solid #ffd34d}
.trkList .hd .srt{display:inline-flex;margin-left:4px}
.trkList .hd .srt button{background:#1e2a36;color:#cfd9e3;border:1px solid #33475a;font:700 9.5px/1 ui-monospace,Menlo,monospace;padding:3px 5px;cursor:pointer;margin-left:-1px}
.trkList .hd .srt button:first-child{border-radius:4px 0 0 4px;margin-left:0}.trkList .hd .srt button:last-child{border-radius:0 4px 4px 0}
.trkList .hd .srt button[aria-pressed="true"]{background:#ffd34d;color:#111;border-color:#ffd34d}
.trkList .hd .srt[hidden],.trkList .hd .tabs[hidden],.trkList .hd .lt[hidden],.trkList .filt[hidden]{display:none}
.trkList .filt{display:flex;align-items:center;gap:6px;padding:4px 10px;font-size:11px;color:#cfd9e3;background:#132030;border-bottom:1px solid #22303d}
.trkList .filt button{background:#1e2a36;color:#e8eef5;border:1px solid #33475a;border-radius:4px;font:600 10px/1 system-ui,sans-serif;padding:3px 6px;cursor:pointer}
.trkMsg .lbl{color:#7fc8ff}
.trkCard .nvTr{background:#1d2a36;color:#e3eaf1;border:1px solid #3c5266;border-radius:5px;font:600 11px ui-monospace,Menlo,monospace;padding:3px 8px;cursor:pointer}
.trkCard .nvTr.on{background:#ffd34d;color:#111;border-color:#ffd34d}
.trkCard .nvTr:focus-visible{outline:2px solid #ffd34d}
.trkList .sec{padding:4px 10px;font:700 10px/1.2 ui-monospace,Menlo,monospace;letter-spacing:.06em;color:#7f93a7;background:#0d151c;border-bottom:1px solid #151f29}
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
.trkRow.dx{box-shadow:inset 3px 0 0 #ffd34d}.trkRow.dx.sel{box-shadow:inset 3px 0 0 #ffd34d}
.trkRow .snrChip{margin:0 5px 0 0}
.trkRow .dxTag{display:inline-block;margin-right:5px;padding:0 4px;border-radius:3px;background:#ffd34d;color:#111;font:700 9.5px/14px ui-monospace,Menlo,monospace;letter-spacing:.6px;vertical-align:1px}
.trkView i.up{color:#6dd36d;font-style:normal}
.trkView i.dn{color:#ff8a6b;font-style:normal}
.trkEmpty{padding:12px;color:#9fb0c2;font-size:12px}
.trkCard .t{font-size:13px;margin:0 18px 6px 0;color:#fff}
.trkCard .r{display:grid;grid-template-columns:7.2em minmax(0,1fr);gap:8px;padding:1px 0}
.trkCard .k{color:#8596a7}
.trkCard .v{color:#e3eaf1;word-break:break-word}
.trkCard .raw{margin-top:6px;padding:5px 6px;background:#0d151c;border-radius:4px;color:#9fb0c2;font-size:11px;word-break:break-all}
.trkView .flag{font-size:14px}
.trkView .avCredit{padding:6px 10px 8px;font:10px/1.35 ui-monospace,Menlo,monospace;color:#8a9aaa;border-top:1px solid #151f29}
.trkView .avCredit a{color:#9ccfff}
.trkView .cc{font:700 10px/1 ui-monospace,monospace;background:#2a3a4a;border-radius:3px;padding:1px 3px}
@media (max-width:520px){
  .trkTop .scr{display:flex;align-items:center;gap:8px;flex:1 1 0;min-width:0;overflow-x:auto;overscroll-behavior-x:contain;scrollbar-width:none;
    -webkit-mask-image:linear-gradient(to right,#000 calc(100% - 14px),transparent 100%);mask-image:linear-gradient(to right,#000 calc(100% - 14px),transparent 100%)}
  .trkTop .scr::-webkit-scrollbar{display:none}
  .trkTop .scr>*{flex:0 0 auto}
  .trkTop .scr .ttl{white-space:nowrap}
  .trkTop .st{flex:1 0 100%}
  .trkList{left:6px;right:6px;width:auto;max-height:34%}
  .trkRow{grid-template-columns:minmax(0,1fr) auto auto}
  .trkRow .ag{display:none}
  .vtl-card{top:6px !important;right:6px !important;left:52px;max-width:none !important;max-height:30% !important;font-size:11px !important}
}
/* ★★★ beta96 THE MAP CONTROL CARDS (Stuart 11-27, docs/v12/MAP-CONTROL-CARDS.md): the map takes centre stage. ONE bar with
 *  only what must always be there — the way out, the shared tuner line + CHAT, the row of small card buttons, and the live
 *  message rate top right; everything else in CARDS that pop out of those buttons and shrink back (the health chips'
 *  and the app's SERVERS button's shape). How many may be open follows the WINDOW (cardLimit). */
.trkTop{flex-wrap:nowrap;gap:6px;min-height:40px;box-sizing:border-box}
.trkTop .st{position:absolute!important;width:1px;height:1px;overflow:hidden;clip:rect(0 0 0 0);white-space:nowrap;flex:none}
.trkTop .scopeNote{display:none}
.trkTop .ttl{display:none}
.trkMapHost .cardBtns{position:absolute;top:8px;right:8px;z-index:790;display:flex;flex-direction:column;align-items:stretch;gap:6px}
.trkMapHost .cbtn{justify-content:center;box-shadow:0 2px 8px rgba(0,0,0,.45);display:inline-flex;align-items:center;gap:5px;background:rgba(30,42,54,.9);color:#cfd9e3;border:1px solid #33475a;border-radius:999px;
  padding:5px 10px;font:700 10.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.08em;cursor:pointer;white-space:nowrap}
.trkMapHost .cbtn:hover,.trkMapHost .cbtn:focus-visible{background:#2a3a4a;outline:none;border-color:#7fc8ff}
.trkMapHost .cbtn[aria-expanded="true"]{background:#e8eef5;color:#0b1014;border-color:#e8eef5}
.trkMapHost .cbtn[hidden]{display:none}
.trkMapHost .cbtn .cn{font-weight:600;opacity:.75;font-variant-numeric:tabular-nums}
.trkMapHost .cbtn .cn:empty{display:none}
.trkMapHost .cbtn .lv{width:6px;height:6px;border-radius:50%;background:#ff3b2f;animation:trkLive 1.6s ease-in-out infinite}
.trkTop .spacer{flex:1 1 auto;min-width:0}
.trkTop .rate{flex:none;font:700 11.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.04em;color:#ffd34d;white-space:nowrap;font-variant-numeric:tabular-nums}
.trkTop .rate:empty{display:none}
.trkTop .slot-top{display:inline-flex;align-items:center;gap:6px;flex:none}
.trkTop .slot-top:empty{display:none}
.trkCards{position:absolute;right:var(--trk-rail-w,84px);top:8px;bottom:8px;z-index:760;display:flex;flex-direction:row-reverse;gap:8px;align-items:flex-start;pointer-events:none;max-width:calc(100% - var(--trk-rail-w,84px) - 8px)}
.trkCards .col{display:flex;flex-direction:column;gap:8px;max-height:100%;min-height:0;width:var(--trk-card-w,340px)}
.trkCard2{pointer-events:auto;display:flex;flex-direction:column;min-height:0;flex:0 1 auto;background:rgba(10,14,20,.94);border:1px solid #2c3a4a;
  border-radius:10px;box-shadow:0 8px 24px rgba(0,0,0,.45);overflow:hidden;transform-origin:top right;animation:trkCardIn .16s ease-out}
@keyframes trkCardIn{from{opacity:0;transform:scale(.92) translateX(8px)}to{opacity:1;transform:none}}
@media (prefers-reduced-motion:reduce){.trkCard2{animation:none}.trkMapHost .cbtn .lv{animation:none}}
.trkCard2[hidden]{display:none}
.trkCard2>.ch{display:flex;align-items:center;gap:8px;padding:6px 6px 6px 10px;border-bottom:1px solid #22303d;flex:none;
  font:800 10.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.12em;color:#9fb0c2}
.trkCard2>.ch .x{margin-left:auto;background:none;border:1px solid transparent;border-radius:6px;color:#9fb0c2;font:600 15px/1 system-ui,sans-serif;padding:3px 7px;cursor:pointer}
.trkCard2>.ch .x:hover,.trkCard2>.ch .x:focus-visible{border-color:#7fc8ff;color:#fff;outline:none}
.trkCard2>.cb{overflow:auto;min-height:0;flex:1 1 auto;overscroll-behavior:contain}
.trkCard2 .facts{padding:6px 10px;color:#9fb0c2;font-size:12px;border-bottom:1px solid #151f29}
.trkCard2 .facts:empty{display:none}
.trkCard2 .sect{padding:8px 10px;border-bottom:1px solid #151f29}
.trkCard2 .sect:last-child{border-bottom:0}
.trkCard2 .sect[hidden]{display:none}
.trkCard2 .sh{font:700 9.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.12em;color:#7f93a7;margin:0 0 6px}
.trkCard2 .row{display:flex;flex-wrap:wrap;gap:5px;align-items:center}
.trkCard2 .btn{background:#1e2a36;color:#e8eef5;border:1px solid #33475a;border-radius:6px;padding:6px 10px;font:600 12px/1 system-ui,sans-serif;cursor:pointer}
.trkCard2 .btn:hover,.trkCard2 .btn:focus-visible{background:#2a3a4a;outline:none;border-color:#7fc8ff}
.trkCard2 .btn[aria-pressed="true"]{background:#ffd34d;color:#111;border-color:#ffd34d}
.trkCard2 .btn.on{background:#7fc8ff;color:#0b1014;border-color:#7fc8ff}
.trkCard2 .btn.dialLocked{opacity:.4;filter:grayscale(.7);cursor:not-allowed}
.trkCard2 .rng{display:inline-flex;flex-wrap:wrap;gap:0}
.trkCard2 .rng[hidden],.trkCard2 .tgl[hidden],.trkCard2 .scope[hidden]{display:none}
.trkCard2 .rng:not(.tgl):not(.scope) .btn{border-radius:0;margin-left:-1px;padding:6px 8px}
.trkCard2 .rng:not(.tgl):not(.scope) .btn:first-child{border-radius:6px 0 0 6px;margin-left:0}
.trkCard2 .rng:not(.tgl):not(.scope) .btn:last-child{border-radius:0 6px 6px 0}
.trkCard2 .tgl,.trkCard2 .scope{display:flex;flex-wrap:wrap;gap:5px;align-items:center}
.trkCard2 .tgl .lbl{display:none}
.trkCard2 .tgl .grp{flex:1 0 100%;font:700 9.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.1em;color:#7f93a7;margin:4px 0 0}
.trkCard2 .tgl .grp:first-child{margin-top:0}
.trkCard2 .btn.chip{border-radius:999px;display:inline-flex;align-items:center;gap:5px;padding:5px 9px}
.trkCard2 .btn.chip .dot{width:8px;height:8px;border-radius:50%;display:inline-block}
.trkCard2 .btn.chip[aria-pressed="true"]{background:#1e2a36;color:#e8eef5;border-color:#7fc8ff}
.trkCard2 .btn.chip[aria-pressed="false"]{opacity:.45}
.trkCard2 .btn.chip[aria-pressed="false"] .dot{background:transparent!important;border:1px solid currentColor}
.trkCard2 .btn.chip .n{font-size:10px;opacity:.75;font-variant-numeric:tabular-nums}
.trkCard2 .btn.chip .n:empty{display:none}
.trkCard2 .btn.chip.live{box-shadow:inset 0 -3px 0 #ff5a4d}
.trkCard2 .btn.chip.live::before{content:"";display:inline-block;width:6px;height:6px;border-radius:50%;background:#ff3b2f;animation:trkLive 1.6s ease-in-out infinite}
.trkCard2 .tunePop{position:static;max-height:none;box-shadow:none;border:0;border-radius:0;background:none;padding:4px}
.trkCard2 .tunePop[hidden]{display:block}
.trkCard2 .slot-tune:empty{display:none}
.trkCard2 .slot-tune{display:flex;flex-wrap:wrap;gap:6px;align-items:center;padding:8px 10px;border-bottom:1px solid #151f29}
.trkCard2 .slot-ctl .vsadsb-ctl{position:static!important;display:flex!important;flex-wrap:wrap;gap:5px;margin:0!important;box-shadow:none!important;background:none!important;border:0!important;padding:0!important;inset:auto!important;transform:none!important}
.trkCard2 .slot-ctl .vsadsb-ctl button{background:#1e2a36!important;color:#e8eef5!important;border:1px solid #33475a!important;border-radius:6px!important;padding:6px 10px!important;font:600 12px/1 system-ui,sans-serif!important;margin:0!important;cursor:pointer;min-width:0!important;height:auto!important}
.trkCard2 .slot-ctl .vsadsb-ctl button.on{background:#7fc8ff!important;color:#0b1014!important;border-color:#7fc8ff!important}
.trkCard2 .slot-ctl .vsadsb-ctl button:focus-visible{outline:2px solid #ffd34d!important}
.trkCard2 .slot-ctl .vsadsb-ctl button[data-k="auto"],.trkCard2 .slot-ctl .vsadsb-ctl button[data-k="fit"]{display:none!important}
.trkCard2 .slot-ctl:empty{display:none}
.trkCard2 .fitSel{display:flex;flex-wrap:wrap;gap:5px}
.trkCard2 .note{color:#8596a7;font-size:11px;margin-top:6px}
.trkCard2 .note:empty{display:none}
.trkCard2 .kb{padding:8px 10px;color:#d7dee6;font-size:12px;line-height:1.35}
.trkCard2 .trkKey{position:static;max-height:none;overflow:visible;border:0;border-radius:0;background:none;z-index:auto}
.trkCard2 .trkKey .kr{white-space:normal}
.trkCard2 .kb .kr{display:flex;align-items:center;gap:8px;padding:2px 0}
.trkCard2 .srcRow{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:2px 10px;padding:6px 10px;border-bottom:1px solid #151f29;font-size:12px}
.trkCard2 .srcRow b{font-weight:600;color:#e8eef5;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.trkCard2 .srcRow small{grid-column:1/-1;color:#93a3b4;font-size:11px}
.trkCard2 .srcRow .sv{font:12px/1.2 ui-monospace,Menlo,monospace;color:#ffd34d;white-space:nowrap}
.trkCard2 .srcRow.down .sv{color:#ff8a6b}
.trkCard2[data-card="list"] .trkList{position:static;width:auto;max-height:none;border:0;border-radius:0;background:none;flex:1 1 auto;min-height:0}
.trkCard2[data-card="list"]{flex:1 1 auto}
.trkCard2[data-card="list"] .trkList .hd{cursor:default}
.trkCard2[data-card="list"] .trkList .hd .mn{display:none}
.trkTop .slot-share{min-width:0;flex:0 1 auto}
.trkTop .slot-share .mapShLine{min-width:0;overflow:hidden;text-overflow:ellipsis;flex:0 1 auto;text-transform:none;letter-spacing:.02em}
.trkTop .slot-share>span{min-width:0;max-width:100%}
.trkTop .rate{order:0}
/* ★ a phone: the bar may WRAP the shared line (never reorder it) — EXIT, the line, CHAT, the rate */
.trkView[data-narrow] .trkTop{flex-wrap:wrap;row-gap:6px}
.trkView[data-narrow] .trkTop .slot-share>span{flex-wrap:wrap}
.trkView[data-narrow] .trkTop .slot-share .mapShLine{white-space:normal;text-align:left;line-height:1.25}
.trkView[data-narrow] .trkTop .rate{font-size:10.5px}
.trkView[data-narrow] .cbtn{padding:5px 8px;font-size:10px;letter-spacing:.06em}
.trkView[data-narrow] .cardBtns{top:6px;right:6px}
.trkView[data-narrow] .trkCards{left:6px;right:var(--trk-rail-w,76px);top:6px;bottom:6px;max-width:none}
.trkView[data-narrow] .trkCards .col{width:100%}
.trkView[data-narrow] .trkCard2{max-height:58%}
/* ★★★ TWEAK 3+ (Stuart 11-27): the chips are EDGE DOCKS (index.html .edgeDock — the SERVER HEALTH pill's own look and
 *  slide): TUNE and LAYERS down the LEFT edge, MAP CONTROLS top RIGHT, LIST bottom RIGHT (the health pill's side). */
.mChips{position:absolute;z-index:790;display:flex;flex-direction:column;gap:8px;pointer-events:none}
/* ★ tweak 4: SERVERS top-left where the app has it, then TUNE, LAYERS; the zoom ± step over to the right, under MAP */
.mChipsL{left:0;top:8px}
.trkMapHost .maplibregl-ctrl-top-left,.trkMapHost .leaflet-top.leaflet-left{left:auto!important;right:0;top:var(--trk-zoom-top,72px)}
.trkView[data-oneside] .trkMapHost .maplibregl-ctrl-top-left,.trkView[data-oneside] .trkMapHost .leaflet-top.leaflet-left{top:0}
.mChipsR{right:0;top:8px}
.mChipsRB{right:0;bottom:calc(var(--trk-attrib-h,0px) + 8px)}
.mDock{position:relative}
.mDock[hidden]{display:none}
.mDock .mchip{all:unset;box-sizing:border-box;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:3px;cursor:pointer;padding:0 4px;color:#dfe7ef;border-radius:8px}
.mDock .mchip:focus-visible{outline:2px solid var(--hc);outline-offset:1px}
.mDock .mchip .cap{font:700 9px/1 system-ui,sans-serif;letter-spacing:.16em;color:#B9C0B4;text-align:center}
.mDock.open .mchip .cap{color:#fff}
.mDock.open .hCard{border-color:var(--hc)}
.mDock .mchip .ic{display:flex;align-items:center;gap:5px;color:var(--hc);font:700 12px/1 ui-monospace,Menlo,monospace}
.mDock .mchip .ic .cn{color:#e8eef5}
.mDock .mchip .ic .cn:empty{display:none}
.mDock .mIco{width:22px;height:22px;display:block}
.mDock .lsum{display:flex;align-items:center;gap:9px}
.mDock .lsum:empty::after{content:"—";color:#7f93a7}
.mDock .le{display:inline-flex;align-items:center;gap:3px;font:700 12px/1 ui-monospace,Menlo,monospace;color:#e8eef5;font-variant-numeric:tabular-nums}
.mDock .lg{display:inline-flex}
.mDock .lg svg{display:block}
/* ★ a phone-width window: the LAYERS chip is a slim column — each glyph over its count (the one-card breakpoint) */
.trkView[data-narrow] .mDock .lsum{flex-direction:column;gap:5px}
.trkView[data-narrow] .mDock .le{flex-direction:column;gap:2px;font-size:11px}
/* ★ the ask shield's look: the TUNE chip fades while this listener must ask first (a press nudges — setDialLock) */
.mDock.askFaded .hCard{opacity:.4;filter:grayscale(.7)}
.trkCard2 .srvPick{all:unset;box-sizing:border-box;display:flex;flex-direction:column;gap:2px;width:100%;padding:8px 10px;cursor:pointer;border-bottom:1px solid #151f29}
.trkCard2 .srvPick b{font:600 13px/1.25 system-ui,sans-serif;color:#e8eef5}
.trkCard2 .srvPick small{font-size:11px;color:#93a3b4}
.trkCard2 .srvPick:hover,.trkCard2 .srvPick:focus-visible{background:#1a2632;outline:none}
.trkView .trkCards{position:absolute;inset:0;left:0;right:0;top:0;bottom:0;max-width:none;display:block;z-index:795;pointer-events:none}   /* ★ an open card above the other chips */
.trkCards .cSide{position:absolute;display:flex;flex-direction:column;gap:8px;width:var(--trk-card-w,340px);pointer-events:none;min-height:0}
.trkCards .cSide>.trkCard2{pointer-events:auto}
.trkCards .cL{left:var(--trk-chip-l,70px);top:84px;bottom:calc(var(--trk-attrib-h,0px) + 8px)}
.trkCards .cR{right:var(--trk-chip-r,70px);top:8px;bottom:calc(var(--trk-attrib-h,0px) + 8px)}
.trkCards .cB{right:var(--trk-chip-rb,70px);top:40%;bottom:calc(var(--trk-attrib-h,0px) + 8px);justify-content:flex-end}
.trkCards.both .cR{bottom:62%}
.trkCards .cSide>.trkCard2{max-height:100%}
.trkCards .cB>.trkCard2{flex:0 1 auto}
.trkView[data-narrow] .trkCards .cL{left:var(--trk-chip-l,64px);right:var(--trk-chip-r,64px);width:auto;top:6px}
.trkView[data-narrow] .trkCards .cR,.trkView[data-narrow] .trkCards .cB{left:var(--trk-chip-l,64px);right:var(--trk-chip-r,64px);width:auto}
.trkView[data-narrow] .trkCards .cB{right:var(--trk-chip-rb,64px);top:30%}
.trkView[data-narrow] .trkCard2{max-height:100%}
/* ★★★ TWEAK 4 (Stuart 11-27): a press on a chip EXPANDS it in place into its card (flush to the same edge, the dock's
 *  rim); its header shrinks it back. The tuck › stays the separate, deliberate action — smaller, set off by a rule. */
.mDock.open{visibility:hidden}
/* ★★★ the ? HELP button and its card, bottom-right above the attribution */
.mHelp{all:unset;box-sizing:border-box;position:absolute;right:10px;bottom:calc(var(--trk-attrib-h,0px) + 10px);z-index:790;width:30px;height:30px;
  border-radius:50%;display:flex;align-items:center;justify-content:center;cursor:pointer;font:800 15px/1 system-ui,sans-serif;color:var(--hc,#5BE36B);
  background:rgba(8,12,8,.86);border:1.5px solid rgba(120,200,120,.45);box-shadow:0 2px 8px rgba(0,0,0,.45)}
.mHelp:hover,.mHelp:focus-visible,.mHelp.on{border-color:var(--hc,#5BE36B);outline:none;background:#16231a}
.trkCards .cH{right:10px;bottom:calc(var(--trk-attrib-h,0px) + 48px);top:20%;justify-content:flex-end;width:min(340px,calc(100% - 20px))}
.trkCards .cH>.trkCard2{border:1.5px solid var(--hc,#5BE36B);border-radius:12px;transform-origin:bottom right;background:rgba(8,12,8,.94)}
.helpBody{padding:6px 10px 10px}
.hRowH{display:flex;gap:10px;align-items:flex-start;padding:6px 0;font-size:12.5px;line-height:1.35;color:#dfe7ef;border-bottom:1px solid #151f29}
.hRowH b{font:700 10.5px/1 system-ui,sans-serif;letter-spacing:.12em;color:#fff}
.hRowH .hi{flex:none;color:var(--hc,#5BE36B);display:flex}
.hRowH .hi svg{width:18px;height:18px;display:block}
.hNoteH{padding:6px 0 0;font-size:11.5px;color:#9fb0c2;line-height:1.35}
/* ★★★ the LAYERS card is a LIST: one row per mode — its colour, its name, its count — a press shows / hides it */
.trkCard2[data-card="layers"] .sect{padding:0}
.trkCard2[data-card="layers"] .tgl{display:flex;flex-direction:column;align-items:stretch;gap:0}
.trkCard2[data-card="layers"] .tgl .grp{flex:none;margin:0;padding:8px 10px 4px;font:700 9.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.12em;color:#7f93a7}
.trkCard2[data-card="layers"] .tgl .btn.chip{display:flex;align-items:center;gap:10px;width:100%;box-sizing:border-box;border:0;border-bottom:1px solid #151f29;
  border-radius:0;background:none;padding:10px 12px;font:600 13px/1.2 system-ui,sans-serif;color:#e8eef5;opacity:1;text-align:left}
.trkCard2[data-card="layers"] .tgl .btn.chip:hover,.trkCard2[data-card="layers"] .tgl .btn.chip:focus-visible{background:#1a2632;outline:none}
.trkCard2[data-card="layers"] .tgl .btn.chip .dot{width:10px;height:10px;flex:none}
.trkCard2[data-card="layers"] .tgl .btn.chip .n{margin-left:auto;font:700 13px/1 ui-monospace,Menlo,monospace;opacity:1;color:#ffd34d}
.trkCard2[data-card="layers"] .tgl .btn.chip[aria-pressed="false"]{color:#7f93a7}
.trkCard2[data-card="layers"] .tgl .btn.chip[aria-pressed="false"] .n{color:#7f93a7}
.trkCard2[data-card="layers"] .tgl .btn.chip::after{content:"ON";flex:none;font:800 9px/1 ui-monospace,Menlo,monospace;letter-spacing:.1em;padding:3px 5px;border-radius:4px;background:#1e3a26;color:#7ee787;margin-left:8px}
.trkCard2[data-card="layers"] .tgl .btn.chip[aria-pressed="false"]::after{content:"OFF";background:#2a1d1d;color:#ff8a6b}
.trkCard2[data-card="layers"] .tgl .btn.chip.live::before{margin-right:-4px}
/* ★ tweak 4: a tucked tab wears its chip's own glyph (the health pill's + is its own) */
.mDock .hTab{gap:3px}
.mDock .hTabIco{display:flex;flex-direction:column;align-items:center;gap:1px;color:var(--hc)}
.mDock .hTabIco svg,.mDock .tIco{width:16px;height:16px;display:block}
.mDock .tCn{font:700 8.5px/1 ui-monospace,Menlo,monospace;color:#e8eef5;letter-spacing:0}
.mDock .tCn:empty{display:none}
.mDock .hTab:has(.tCn:not(:empty)){height:62px}
.trkCards .cL{left:0;top:var(--trk-cl-top,8px)}
.trkCards .cR{right:0;top:var(--trk-cr-top,8px)}
/* ★ tweak 4: the LIST grows UP out of its chip at the bottom-left, where the old list sheet sat */
.mChipsBL{left:12px;bottom:var(--trk-list-bottom,0px);overflow:hidden;padding-top:12px;margin-top:-12px}
.trkView[data-narrow] .mChipsBL{--trk-list-bottom:var(--trk-attrib-h,0px)}
.mChipsBL .mDock .mchip{flex-direction:row;gap:8px;min-height:34px}
.trkMapHost .maplibregl-ctrl-bottom-left,.trkMapHost .leaflet-bottom.leaflet-left{left:var(--trk-list-w,0px)}
.trkCards .cB{left:12px;right:auto;bottom:var(--trk-cb-bottom,0px);top:30%;justify-content:flex-end;width:min(var(--trk-card-w,340px),calc(100% - 24px))}
.trkCards.lAndB .cL{bottom:47%}
.trkCards.lAndB .cB{top:55%}
.trkCards .cB>.trkCard2{border:1.5px solid var(--hc,#5BE36B);border-bottom-width:0;border-radius:12px 12px 0 0;transform-origin:bottom left}
.trkCards .cSide>.trkCard2{border:1.5px solid var(--hc,#5BE36B);background:rgba(8,12,8,.92)}
.trkCards .cL>.trkCard2{border-left-width:0;border-radius:0 12px 12px 0;transform-origin:top left}
.trkCards .cR>.trkCard2{border-right-width:0;border-radius:12px 0 0 12px;transform-origin:top right}
@keyframes trkCardIn{from{opacity:.2;transform:scale(.35)}to{opacity:1;transform:none}}
.trkCard2>.ch{cursor:pointer;font:700 9px/1 system-ui,sans-serif;letter-spacing:.16em;color:#B9C0B4}
.trkCard2>.ch:hover{background:rgba(255,255,255,.04)}
.trkCard2>.ch .x{font:700 18px/1 system-ui,sans-serif;color:var(--hc,#5BE36B);border:0;padding:0 6px}
.trkCards .cL>.trkCard2>.ch{flex-direction:row}
.mDock .hTuck{width:15px!important;font-size:15px!important;opacity:.6}
.mDock:not(.left) .hTuck{border-right:1px solid rgba(255,255,255,.14);margin-right:5px!important}
.mDock.left .hTuck{border-left:1px solid rgba(255,255,255,.14);margin-left:5px!important}
.mDock .hTuck:hover{opacity:1}
.mDock .mchip{padding:2px 6px;min-height:40px}
.trkView[data-narrow] .trkCards .cL{left:0;right:var(--trk-chip-r,64px);top:var(--trk-cl-top,8px)}
.trkView[data-narrow] .trkCards .cR{left:var(--trk-chip-l,64px);right:0}
.trkView[data-narrow] .trkCards .cB{left:6px;right:6px;width:auto}
`;
function addCss() {
  if (document.getElementById('trk-style')) return;
  const st = document.createElement('style'); st.id = 'trk-style'; st.textContent = CSS; document.head.appendChild(st);
}
function runSource(src: string) { const el = document.createElement('script'); el.textContent = src; document.head.appendChild(el); }
const MIN_KEY = 'vibesdr.trkListMin';
/** ★★★ beta96 the map control cards (docs/v12/MAP-CONTROL-CARDS.md): their rules are shared with the app (mapCards.ts). */
const CARDS_KEY = 'vibesdr.mapCards';
/** ★ the chips' sides follow the window (mapCards.ts cardSide / oneSide) — MAP CONTROLS moves edge as it is resized. */
const CARD_SIDE_NOTE = 'mapCards.cardSide';
/** ★★★ tweak 3 the LAYERS chip's glyphs — the shapes the map draws: the aircraft and ship silhouettes (airSeaTune's,
 *  as the TUNE list draws them), the NAVTEX station's disc and mast (navtexMap.ts navIcon), the balloon, the beacon. */
export type LayerGlyph = 'aircraft' | 'ship' | 'navtex' | 'balloon' | 'beacon' | 'navaid' | 'dot';
const P16 = (d: string) => `<svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor" aria-hidden="true"><path d="${d}"/></svg>`;
const LAYER_GLYPH: Record<LayerGlyph, string> = {
  aircraft: P16(TUNE_ICON_PATH.aircraft), ship: P16(TUNE_ICON_PATH.ship), balloon: P16(TUNE_ICON_PATH.balloon),
  beacon: P16(TUNE_ICON_PATH.beacon), navaid: P16(TUNE_ICON_PATH.navaid),
  navtex: '<svg viewBox="0 0 26 26" width="16" height="16" aria-hidden="true"><circle cx="13" cy="13" r="11" fill="currentColor" stroke="#0b1420" stroke-width="1.4"/>'
    + '<path d="M13 7 L9.5 19 M13 7 L16.5 19 M10.6 15 H15.4" stroke="#0b1420" stroke-width="1.8" fill="none" stroke-linecap="round"/></svg>',
  dot: '<svg viewBox="0 0 16 16" width="12" height="12" aria-hidden="true"><circle cx="8" cy="8" r="6" fill="currentColor"/></svg>',
};
/** A SHOW toggle's layer id → its glyph (the maps whose host gives no summary: the ISM maps). */
function glyphOf(id: string): LayerGlyph {
  return /^(adsb|acars|vdl2|hfdl|adsc|aircraft)/.test(id) ? 'aircraft' : /^(ships|ais|dsc)/.test(id) ? 'ship' : /^(navst|navtex)/.test(id) ? 'navtex'
    : /sonde/.test(id) ? 'balloon' : /^(ndb|beacon)/.test(id) ? 'beacon' : /^vor/.test(id) ? 'navaid' : 'dot';
}
/** ★ the SERVERS chip: a rack of two servers (the house line style) */
const SERVERS_SVG = '<svg class="mIco" viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">'
  + '<rect x="3.5" y="4" width="17" height="6.5" rx="1.5"/><rect x="3.5" y="13.5" width="17" height="6.5" rx="1.5"/><path d="M7 7.25h.01M7 16.75h.01M11 7.25h6M11 16.75h6"/></svg>';
const LIST_SVG = '<svg class="mIco" viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round">'
  + '<path d="M9 6h11M9 12h11M9 18h11"/><circle cx="4.5" cy="6" r="1.2" fill="currentColor"/><circle cx="4.5" cy="12" r="1.2" fill="currentColor"/><circle cx="4.5" cy="18" r="1.2" fill="currentColor"/></svg>';
/** ★ the HELP card's glyphs — each chip's own */
const HELP_GLYPH: Record<string, string> = {
  servers: SERVERS_SVG.replace('mIco', 'tIco'), tune: modeIconSvg('radio', 'tIco'), layers: P16(TUNE_ICON_PATH.aircraft), map: modeIconSvg('map', 'tIco'),
  list: LIST_SVG.replace('mIco', 'tIco'),
};
const FIT_KEY = 'vibesdr.mapFitTo';
export { CARD_IDS, type CardId, trimOutliers };

export interface TrackViewOpts {
  /** ★★ AUTO FIT (Stuart 2026-11-21, ISM: "the map needs the auto fit"; 2026-11-23, every map: "an auto fit that gently
   *  glides the zoom in and out like the adsb maps"). ✗ No longer an option: since beta92 EVERY track view runs the shared
   *  auto fit (src/utils/mapAutoFit.ts — the ADS-B map's camera) and its FIT key is the AUTO key. Kept so callers compile. */
  fitOnOpen?: boolean;
  title: string;                 // "AIS" / "APRS"
  exitLabel: string;             // "EXIT AIS" / "CLOSE MAP"
  listTitle: string;             // "SHIPS" / "STATIONS"
  onExit: () => void;
  icon: (it: any) => { html: string; w: number; h: number };
  label: (it: any) => string;
  card: (it: any) => string;
  trail?: (key: string, done: (pts: [number, number][]) => void) => void;
  /** ★ V12 the ISM map: the Leaflet zoom from which names show (the layer's own default is 11), and how far a chosen
   *  item is zoomed in to (10). */
  labelZoom?: number;
  focusZoom?: number;
  /** ★ a count that grows when an item is heard again — the layer pulses it (live pushes). */
  heard?: (it: any) => number;
  /** ★ who keeps its label when two collide (the layer's declutter; higher wins). */
  labelRank?: (it: any) => number;
  /** ★ a faint great-circle line from the receiver to this item (the NAVTEX stations map). */
  arc?: (it: any) => boolean;
  /** ★★ V12 2026-11-01 THE TIME FILTER (Stuart: "filter the map based on last hour last day last week last month all
   *  time"): a segmented control in the top bar — the page filters; `rangeHint` its title (what "All" means here). */
  ranges?: Array<{ id: string; label: string; words: string }>;
  range?: string;
  onRange?: (id: string) => void;
  rangeHint?: string;
  /** ★ V12 what the segmented control IS, for a screen reader (default: the time filter's words) — the datalink's bands. */
  rangeLabel?: string;
  /** ★ V12 2026-11-01: a KEY — a folded panel, bottom-right of the map, listing what each icon means (rows of HTML). The
   *  labels give way when the map is busy or zoomed out (the declutter), so the icons alone must still be readable. */
  legend?: string;
  /** ★ the basemap PROFILE (vibemap-style.js "vibesdr:profiles"): 'marine' for sea maps — ports, no airports or runways
   *  (Stuart, 2026-11-01: "the airports are on the map layer but we need the ports instead"). Default: everything. */
  profile?: string;
  /** ★★ V12 2026-11-01 THE NAVTEX MAP's layers (Stations · Warnings · Weather): toggle keys in the top bar. */
  toggles?: Array<{ id: string; label: string; words: string }>;
  toggled?: Record<string, boolean>;
  onToggle?: (id: string, on: boolean) => void;
  /** ★ passed to the layer (tracklayer.js): an item's shape, its cluster group, the words a screen reader says, the card drawn. */
  shape?: (it: any) => any;
  cluster?: (it: any) => string;
  clusterZoom?: number;
  clusterAlways?: string[];
  /** ★ groups whose icons give way to every other item and name (tracklayer.js yieldGroups — the NAVTEX weather labels) */
  yieldGroups?: string[];
  aria?: (it: any) => string;
  onCard?: (key: string, el: HTMLElement) => void;
  /** ★★ beta96 (Stuart 11-26) a tap on an icon ZOOMS IN to it as a row does (the ISM mesh maps) — not only its card */
  zoomOnSelect?: boolean;
  /** ★★ V12 beta90 the SCALE BAR (every map has one — vibemapgl.js / vibemap.js): true = NM under the km (the marine
   *  and aviation maps). Default: NM for the 'marine' and 'aero' profiles, km alone otherwise. */
  nautical?: boolean;
  /** ★★★ V12 beta90 THE AVIATION MAP ON THE ADS-B MAP (Stuart, 2026-11-21): items carrying `_adsb` (aviationMap.ts
   *  avPlaneAdsb) are drawn by the ADS-B layer (web/mapkit/adsblayer.js — silhouettes, tones, labels, range rings,
   *  AUTO / FIT, the glide, its own cards); the rest (HFDL ground stations) by the track layer as before. */
  aircraft?: boolean;
  /** ★ beta91 (aircraft:true) where this server serves the owner's logos and silhouettes ("…/adsb/", adsb.ts adsbImageBase);
   *  null / absent = none. Every source's aircraft carries the ADS-B data's keys (vibe_aircraft_info.h). */
  imageBase?: string | null;
  /** ★★ beta96 THE AIR & SEA MAP: aircraft AND other layers on one map — both cameras made, setCamera picks one. */
  mixed?: boolean;
  /** ★★ beta96 elements (by id) the view leaves reachable while it covers the page, raised above it while it is shown —
   *  ADS-B mode's CHAT panel, its key being in this view's top bar now. */
  liveIds?: string[];
  /** ★★★ beta96 a TUNE row the server says this radio cannot run, pressed: its reason (the host's pill). */
  onTuneRefused?: (why: string) => void;
}
/** ★★ beta96 an AREA-class item (the click order's last tier): a sea area, an area-shaped NAVTEX warning, a forecast arrow. */
export function isAreaItem(it: any): boolean {
  return !!it && (!!it._nofit || it._cl === 'm' || (!!it._sh && it._sh.kind === 'area'));
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
  private vmL: any = null;   // ★ the Leaflet map's VibeMap (its setProfile)
  private layer: any = null;
  private pending: any[] | null = null;
  private rx: LatLon | null = null;
  private lastRows = '';
  private sel = '';
  private inerted: HTMLElement[] = [];
  private acLayer: any = null;                       // ★ beta90 the ADS-B layer (aircraft:true)
  private acKeys = new Map<string, string>();        // item key → its hex, and back
  private acHex = new Map<string, string>();

  constructor(private o: TrackViewOpts) {
    addCss();
    const r = document.createElement('div');
    r.className = 'trkView'; r.hidden = true;
    r.setAttribute('role', 'dialog'); r.setAttribute('aria-label', o.title + ' map');
    // ★ beta96 everything but EXIT and the status line sits in .scr: in the row as before on a wide window, ONE row that
    //   scrolls sideways on a phone (ADS-B's own bar did, Stuart 2026-10-25: "the only thing that must stay anchored is the
    //   Exit ADS-B button") — the AIR & SEA map's chips, ranges, GAIN and CHAT no longer stack four rows deep
    /* ★★★ beta96 THE MAP CONTROL CARDS (Stuart 11-27, docs/v12/MAP-CONTROL-CARDS.md): the bar holds only the way out, the
     *  title (wide windows), the SHARED TUNER line + CHAT ('share' slot), the card buttons and the live rate (top right).
     *  Everything else is in a card — LIST · TUNE · VIEW · MAP · KEY · SOURCES — that pops out of its button. */
    /* ★★★ tweak 3+ (Stuart 11-27): each chip is an EDGE DOCK exactly as the SERVER HEALTH pill's (healthTuck.ts, index.html
     *  .edgeDock) — out: the card with its caption and glyphs and a › (‹ on the left edge) that tucks it away; tucked: the
     *  + tab on the edge that brings it back; the same drag, flick, slide and colours. The chip itself opens its card. */
    // ★★★ tweak 4: a tucked chip's tab wears ITS OWN glyph (`tabIcon`) — never the health pill's + (Stuart: "the collapsed
    //     modes need to show the icons for what they control")
    const dock = (id: string, side: 'left' | 'right' | 'bottom', cap: string, inner: string, what: string, dial = false, tabIcon = '') =>
      `<div class="edgeDock mDock${side === 'right' ? '' : ' ' + side}" data-dock="${id}" data-side="${side}" hidden>`
      // ★★★ Stuart 11-27 (11:38): TWO states only — the collapsed chip (its own glyph, and the counts) and the open card.
      //     One press opens the card from the chip; one press on the card's header shrinks it back. No tuck, no + tab:
      //     the chip is already the small thing on the edge. (tabIcon is kept for the drivers' sake — unused.)
      + `<div class="hCard"><button type="button" class="hBody mchip" data-cbtn="${id}"${dial ? ' data-dial="1"' : ''} aria-expanded="false" aria-label="${esc(cap)} — open ${esc(what)}"><span class="cap">${esc(cap)}</span>${inner}</button></div></div>`;
    const card = (id: string, title: string, body: string) => `<section class="trkCard2" data-card="${id}" role="region" aria-label="${esc(title)}" hidden>`
      + `<div class="ch"><span class="ct">${esc(title)}</span><button class="x" type="button" aria-label="Close ${esc(title.toLowerCase())}">×</button></div><div class="cb">${body}</div></section>`;
    /* ★★★ beta96 TWEAK 2 (Stuart 11-27, live on the preview): the bar is EXACTLY "EXIT | Shared VFO: … 2/5 | CHAT | the
     *  incoming messages" — no title (kept, hidden, for the dialog's name and the drivers), no card buttons: they are a
     *  small RAIL down the map's right edge (.cardBtns in .trkMapHost), which costs the map nothing. */
    r.innerHTML = `<div class="trkTop"><button class="btn exit" type="button">${esc(o.exitLabel)}</button><span class="ttl">${esc(o.title)}</span>`
      + `<span class="rate" role="status" aria-live="off"></span><span class="spacer"></span><span class="st" aria-live="polite"></span></div>`
      /* ★★★ TWEAK 3 (Stuart 11-27, live): the HEALTH CARDS' shape — chips FLANKING the map that pop out a card and shrink
       *  back: TUNE and LAYERS down the left edge, MAP CONTROLS on the right, LIST at the bottom (its card pops UP). */
      + `<div class="trkMapHost"><div class="mChips mChipsL">${dock('servers', 'left', 'SERVERS', `<span class="ic">${SERVERS_SVG}</span>`, 'the servers card', false, SERVERS_SVG.replace('mIco', 'tIco'))}${dock('tune', 'left', 'TUNE', `<span class="ic">${modeIconSvg('radio', 'mIco')}</span>`, 'the tuning card', true, modeIconSvg('radio', 'tIco'))}`
      + `${dock('layers', 'left', 'LAYERS', '<span class="lsum"></span>', 'the layers card', false, LAYER_GLYPH.aircraft)}</div>`
      + `<div class="mChips mChipsR">${dock('map', 'right', 'MAP', `<span class="ic">${modeIconSvg('map', 'mIco')}</span>`, 'the map controls', false, modeIconSvg('map', 'tIco'))}</div>`
      + `<div class="mChips mChipsBL">${dock('list', 'bottom', 'LIST', `<span class="ic">${LIST_SVG}<span class="cn"></span></span>`, 'the list', false, `${LIST_SVG.replace('mIco', 'tIco')}<span class="tCn"></span>`)}</div>`
      // ★★★ the ? HELP button, bottom-right above the map's attribution (its (i) is the attribution's own — kept)
      + `<button type="button" class="mHelp" data-cbtn="help" aria-expanded="false" aria-label="Help — what the map's chips do" title="What the chips do">?</button>`
      + `<div class="trkCards"><div class="cSide cL"></div><div class="cSide cR"></div><div class="cSide cB"></div><div class="cSide cH"></div></div><div class="trkPark" hidden>`
      + card('help', 'HELP', `<div class="helpBody">${MAP_HELP.map((h) => `<div class="hRowH"><span class="hi">${HELP_GLYPH[h.id]}</span><span><b>${esc(h.name)}</b> \u2014 ${esc(h.words)}</span></div>`).join('')}`
        + MAP_HELP_NOTES.map((t) => `<div class="hNoteH">${esc(t)}</div>`).join('') + `</div>`)
      + card('list', 'LIST', `<div class="facts"></div><div class="trkList"><div class="hd"><span class="lt">${esc(o.listTitle)}</span><span class="tabs" role="tablist" aria-label="Lists" hidden></span> <span class="n">0</span>`
        + `<span class="srt" role="group" aria-label="Sort the aircraft" hidden></span>`
        + `<button class="mn" type="button" aria-label="Minimise the list" tabindex="-1">▾</button></div><div class="safety" hidden></div><div class="filt" hidden></div><div class="rows"></div></div>`)
      + card('servers', 'SERVERS', `<div class="srvBody" role="group" aria-label="Servers and radios"></div>`)
      + card('tune', 'TUNE', `<div class="facts"></div><div class="trkSlot slot-tune"></div><div class="tunePop" role="menu" aria-label="Tune the radio"></div>`)
      + card('layers', 'LAYERS', `<div class="facts"></div><div class="sect"><span class="rng tgl" role="group" aria-label="Layers"${o.toggles?.length ? '' : ' hidden'}></span></div>`
        + `<div class="sect scopeSect" hidden><span class="rng scope" role="group" aria-label="Sources"></span><div class="note scopeNote2"></div></div>`)
      + card('map', 'MAP CONTROLS', `<div class="sect rngSect"${o.ranges?.length ? '' : ' hidden'}><div class="sh">TIME</div>`
        + (o.ranges?.length ? `<span class="rng" role="group" aria-label="${esc(o.rangeLabel || 'Show what was heard in the last…')}" title="${esc(o.rangeHint || '')}">`
           + o.ranges.map((x) => `<button class="btn" type="button" data-rng="${esc(x.id)}" aria-pressed="${x.id === o.range}" aria-label="${esc(x.words)}">${esc(x.label)}</button>`).join('') + '</span>' : '')
        + `</div>`
        + `<div class="sect fitSect"><div class="sh">MAP</div><div class="row"><button class="btn fit" type="button" title="Fit every ${esc(o.listTitle.toLowerCase())} on the map">AUTO</button>`
        + `<button class="btn fitOnce" type="button">FIT</button></div><div class="sh fitToH" style="margin-top:8px" hidden>FIT TO</div><div class="fitSel" role="radiogroup" aria-label="Fit to" hidden></div><div class="note fitNote"></div>`
        + `<div class="ctlSect" hidden><div class="trkSlot slot-ctl" style="margin-top:8px"></div><div class="note farNote"></div></div></div>`
        + `<div class="sect srcSect" hidden><div class="sh">SOURCES — TAP TO SHOW OR HIDE</div><div class="srcBody" role="group" aria-label="Sources"></div></div>`
        + `<details class="sect keySect"${o.legend ? '' : ' hidden'}><summary>KEY</summary><div class="trkKey"><div class="kb">${o.legend || ''}</div></div></details>`)
      + `</div></div>`;
    this.root = r;
    this.mapHost = r.querySelector('.trkMapHost') as HTMLElement;
    this.list = r.querySelector('.trkList') as HTMLElement;
    this.rowsEl = r.querySelector('.rows') as HTMLElement;
    this.safetyEl = r.querySelector('.safety') as HTMLElement;
    this.statEl = r.querySelector('.st') as HTMLElement;
    this.countEl = r.querySelector('.hd .n') as HTMLElement;
    (r.querySelector('.exit') as HTMLButtonElement).onclick = () => o.onExit();
    /* ★★★ beta92 the .fit key is the shared AUTO FIT's (attach → VibeAutoFit.attach, button): on · paused by a touch ·
     *  off, as the ADS-B map's AUTO key. Its click is the auto fit's own. */
    this.fitBtn = r.querySelector('.fit') as HTMLButtonElement;
    for (const b of Array.from(r.querySelectorAll('[data-rng]')) as HTMLButtonElement[]) {
      b.onclick = () => {
        const id = b.dataset.rng || '';
        for (const o2 of Array.from(r.querySelectorAll('[data-rng]'))) o2.setAttribute('aria-pressed', String(o2 === b));
        o.onRange?.(id);
        this.refit = true;
      };
    }
    // ★★★ beta96 THE CARDS: each button opens / shuts its card; × shuts it; FIT (once) fits the chosen layer
    this.cardsEl = r.querySelector('.trkCards') as HTMLElement;
    this.parkEl = r.querySelector('.trkPark') as HTMLElement;
    for (const b of Array.from(r.querySelectorAll('[data-cbtn]')) as HTMLButtonElement[]) b.onclick = () => this.cardOpen(b.dataset.cbtn as CardId, !this.isCardOpen(b.dataset.cbtn as CardId), true);
    // ★★★ tweak 4 (Stuart 11-27): the open card IS the chip, expanded in place — a press on its header (or its ‹ / ›) shrinks
    //     it back to the chip; the tuck to the edge stays the separate, deliberate › / drag / flick on the chip
    for (const ch of Array.from(r.querySelectorAll('.trkCard2 > .ch')) as HTMLElement[]) {
      ch.onclick = () => { const id = (ch.closest('.trkCard2') as HTMLElement).dataset.card as CardId; this.cardOpen(id, false, true); (r.querySelector(`[data-cbtn="${id}"]`) as HTMLElement | null)?.focus(); };
    }
    for (const d of Array.from(r.querySelectorAll('.mDock')) as HTMLElement[]) {
      const id = d.dataset.dock as CardId;
      edgeDockSet(d, true);   // ★ always out: the chip IS the collapsed state (no tuck since 11:38)
      void id; void wireEdgeDock;
    }
    for (const c of [...Array.from(r.querySelectorAll('.trkCard2')), ...Array.from(r.querySelectorAll('.mChips'))] as HTMLElement[])
      for (const ev of ['mousedown', 'wheel', 'touchstart', 'dblclick', 'pointerdown']) c.addEventListener(ev, (e) => e.stopPropagation(), { passive: true });
    (r.querySelector('.fitOnce') as HTMLButtonElement).onclick = () => this.fitOnce();
    this.cardsLoad();
    this.setToggles(o.toggles ?? [], o.toggled ?? {});
    for (const id of CARD_IDS) this.cardsAvail(id, this.cardAvail.has(id));
    this.viewSync();
    this.cardsLayout();
    if (typeof ResizeObserver !== 'undefined') { this.cardRo = new ResizeObserver(() => this.cardsLayout()); this.cardRo.observe(r); }
    else window.addEventListener('resize', () => this.cardsLayout());
    this.rowsEl.addEventListener('pointerdown', (e) => {
      const row = (e.target as HTMLElement).closest('.trkRow') as HTMLElement | null;
      this.down = { key: row?.dataset.key || '', at: performance.now(), x: e.clientX, y: e.clientY };
    });
    this.rowsEl.addEventListener('click', (e) => this.rowPick(e));
    this.rowsEl.addEventListener('keydown', (e) => {
      const k = (e as KeyboardEvent).key;
      if (k !== 'Enter' && !(k === ' ' && (e.target as HTMLElement).closest('.trkRow'))) return;
      if (k === ' ') e.preventDefault();
      this.rowPick(e);
    });
    for (const ev of ['mousedown', 'wheel', 'touchstart', 'dblclick', 'pointerdown']) this.list.addEventListener(ev, (e) => e.stopPropagation(), { passive: true });
    /* ★★★ beta96 Escape (and the pad's BACK): inside a card — that card shuts, focus back on its button; else an item's
     *  own card on the map (the selection) first, as before; else the TOP card (the last opened); else the map. */
    r.addEventListener('keydown', (e) => {
      if (e.key !== 'Escape') return;
      e.stopPropagation();
      // ★ the page's one back (gamepad.ts → main.ts hooks.back) runs first and may have shut a card for this very press
      if (performance.now() - this.cardClosedAt < 80) return;
      if (this.closeTopCard(e.target)) return;
      if (this.sel) this.select('');
      else if (this.dialExit) this.onDialLocked?.(this.dialWhy);
      else o.onExit();
    });
    // ★★★ beta96 the dial lock (setDialLock): captured before EXIT's / a band key's own handler sees the press
    for (const ev of ['click', 'keydown'] as const) r.addEventListener(ev, (e) => {
      if (ev === 'keydown' && (e as KeyboardEvent).key !== 'Enter' && (e as KeyboardEvent).key !== ' ') return;
      if (this.dialRefuses(e.target)) { e.preventDefault(); e.stopImmediatePropagation(); }
    }, { capture: true });
    document.body.appendChild(r);
  }
  /* ── ★★★ beta96 THE CARDS (Stuart 11-27) ─────────────────────────────────────────────────────────────────────────────
   *  ONE card on a phone-width window (opening another shuts the first); wider windows take more, side by side in columns
   *  (two stacked per column where the window is tall), never more than covers ~70 % of the map's width. Which cards are
   *  open is remembered per viewer, separately for a narrow and a wide window: nothing on a phone, LIST on a wide one. */
  private cardsEl: HTMLElement;
  private parkEl: HTMLElement;
  private cardRo: ResizeObserver | null = null;
  private cardOrder: CardId[] = [];
  private cardAvail = new Set<CardId>(['list', 'map', 'help']);
  private narrow(): boolean { const w = this.root.clientWidth || window.innerWidth; return w < CARD_NARROW_W; }
  /** How many cards this window takes at once (the window's size, never the device). */
  cardLimit(): number { return cardLimitFor(this.root.clientWidth || window.innerWidth, this.root.clientHeight || window.innerHeight); }
  private cardClosedAt = -1e9;
  /** ★ BACK / Escape on the map: the card holding `from` (focus), else — unless an item's own card is open (the selection
   *  goes first) — the top card. True = one was shut. The pad's BACK reaches it through main.ts hooks.back. */
  closeTopCard(from: EventTarget | null = document.activeElement): boolean {
    const inCard = (from as HTMLElement | null)?.closest?.('.trkCard2') as HTMLElement | null;
    const open = this.openCards.filter((id) => this.cardAvail.has(id) && this.cardOrder.includes(id));
    const shown = Array.from(this.cardsEl.querySelectorAll('.trkCard2')).map((c) => (c as HTMLElement).dataset.card as CardId);
    const top = inCard && this.root.contains(inCard) ? inCard.dataset.card as CardId : this.sel ? undefined : [...open].reverse().find((id) => shown.includes(id));
    if (!top) return false;
    this.cardOpen(top, false, true);
    this.cardClosedAt = performance.now();
    if (inCard) (this.root.querySelector(`[data-cbtn="${top}"]`) as HTMLElement | null)?.focus();
    return true;
  }
  isCardOpen(id: CardId): boolean { return this.cardOrder.includes(id); }
  get openCards(): CardId[] { return this.cardOrder.slice(); }
  private cardsKey(): string { return this.narrow() ? CARDS_KEY + '.narrow' : CARDS_KEY + '.wide'; }
  private cardsNarrowWas: boolean | null = null;
  private cardsLoad() {
    let ids: CardId[] | null = null;
    try { const j = JSON.parse(localStorage.getItem(this.cardsKey()) || 'null'); if (Array.isArray(j)) ids = j.filter((x) => (CARD_IDS as readonly string[]).includes(x)); } catch { /* private window */ }
    this.cardOrder = ids ?? cardDefaults(this.root.clientWidth || window.innerWidth);
    this.cardsNarrowWas = this.narrow();
  }
  private cardsSave() { try { localStorage.setItem(this.cardsKey(), JSON.stringify(this.cardOrder)); } catch { /* private window */ } }
  /** Open / shut a card. `save` = a person's press (remembered); the host's own opening (TUNE's list from elsewhere) too. */
  cardOpen(id: CardId, on: boolean, save = false) {
    if (on && !this.cardAvail.has(id)) return;
    // ★ the oldest gives way past the window's limit (mapCards.ts cardToggle)
    this.cardOrder = cardToggle(this.cardOrder.filter((x) => this.cardAvail.has(x)), id, on, this.root.clientWidth || window.innerWidth, this.root.clientHeight || window.innerHeight, this.oneSideNow);
    if (save) this.cardsSave();
    this.cardsLayout();
    if (on && save) {
      // ★ the pad / keyboard: into the card (its first control), so Escape / BACK come straight back out
      const el = this.root.querySelector(`.trkCard2[data-card="${id}"]`) as HTMLElement | null;
      (el?.querySelector('.cb button:not([disabled]):not([hidden]), .cb [tabindex="0"]') as HTMLElement | null)?.focus({ preventScroll: true });
    }
  }
  /** Which cards may be offered (a card with nothing in it has no button — AGENTS.md: never a dead control). */
  private cardsAvail(id: CardId, on: boolean) {
    if (on) this.cardAvail.add(id); else this.cardAvail.delete(id);
    const d = this.root.querySelector(`.mDock[data-dock="${id}"]`) as HTMLElement | null;
    if (d) d.hidden = !on;
  }
  /** ★ A chip tucked into its edge (its card shuts with it) or brought back out (its card stays shut until pressed). */
  dockTuck(id: CardId, tucked: boolean) {
    const d = this.root.querySelector(`.mDock[data-dock="${id}"]`) as HTMLElement | null;
    if (!d) return;
    edgeDockSet(d, !tucked);
    if (tucked && this.isCardOpen(id)) this.cardOpen(id, false, true);
    if (!tucked) (d.querySelector('.mchip') as HTMLElement | null)?.focus({ preventScroll: true });
    this.layoutSig = ''; this.cardsLayout();
  }
  get tuckedDocks(): CardId[] { return (Array.from(this.root.querySelectorAll('.mDock.tucked')) as HTMLElement[]).map((d) => d.dataset.dock as CardId); }
  private oneSideNow = false;
  /** For the drivers: every chip down the left (a tall map) or flanking (MAP CONTROLS top-right). */
  get chipsOneSide(): boolean { return this.oneSideNow; }
  private layoutSig = '';
  /** Put the open cards beside their chips (and the rest in the park), the chips pressed, the obstacle told. */
  cardsLayout() {
    if (!this.cardsEl) return;   // ★ still being made
    { /* ★★★ Stuart 11-27: a TALL window puts every chip down the LEFT (SERVERS, TUNE, LAYERS, MAP CONTROLS; LIST bottom-left);
       *  a shorter one flanks — MAP CONTROLS top-right. By the window, live as it is resized. */
      // ★ the whole stack measured: every offered chip's own height (a tucked one counts as out — no flapping as it tucks)
      const docks = (Array.from(this.root.querySelectorAll('.mDock')) as HTMLElement[]).filter((d) => !d.hidden);
      const stackH = docks.reduce((t, d) => t + Math.max(40, (d.querySelector('.hCard') as HTMLElement).offsetHeight) + 8, 16);
      const one = oneSide(this.mapHost.clientHeight || 0, stackH);
      this.oneSideNow = one;
      const md = this.root.querySelector('.mDock[data-dock="map"]') as HTMLElement | null;
      const host = this.root.querySelector(one ? '.mChipsL' : '.mChipsR') as HTMLElement | null;
      if (md && host && md.parentElement !== host) {
        host.appendChild(md);
        md.dataset.side = one ? 'left' : 'right';
        md.classList.toggle('left', one);
        this.layoutSig = '';
      }
      this.root.toggleAttribute('data-oneside', one);
    }
    { // ★ the MAP card's layer keys: shown while a layer put keys in it (ADS-B, NDB)
      const sl = this.root.querySelector('.slot-ctl') as HTMLElement | null, sc = this.root.querySelector('.ctlSect') as HTMLElement | null;
      if (sl && sc) { const any = Array.from(sl.children).some((c) => !(c as HTMLElement).hidden); if (sc.hidden === any) sc.hidden = !any; }
    }
    if (this.cardsNarrowWas !== null && this.cardsNarrowWas !== this.narrow()) { this.cardsLoad(); }
    this.root.toggleAttribute('data-narrow', this.narrow());
    const W = this.root.clientWidth || window.innerWidth, H = this.root.clientHeight || window.innerHeight;
    // ★ the cards stand beside their chips: the chips' widths, the bottom chip's height
    const cw = (sel: string) => { const e = this.root.querySelector(sel) as HTMLElement | null; const b = e?.getBoundingClientRect(); return b && b.width > 0 ? b : null; };
    // ★ a tucked dock's card is off the edge: only its 26 px tab counts
    const dw = (sel: string) => Math.max(0, ...(Array.from(this.root.querySelectorAll(sel)) as HTMLElement[]).filter((d) => !d.hidden)
      .map((d) => (d.classList.contains('tucked') ? 26 : (d.querySelector('.hCard') as HTMLElement).getBoundingClientRect().width)));
    void CARD_SIDE_NOTE;
    void cw;
    const setv = (k: string, v: string) => { if (this.root.style.getPropertyValue(k) !== v) this.root.style.setProperty(k, v); };
    setv('--trk-chip-l', `${Math.ceil(dw('.mChipsL .mDock')) + 8}px`);
    setv('--trk-chip-r', `${Math.ceil(dw('.mChipsR .mDock')) + 8}px`);
    { const md = this.root.querySelector('.mChipsR') as HTMLElement | null; const b = md?.getBoundingClientRect(); setv('--trk-zoom-top', b && b.height ? `${Math.ceil(b.height) + 16}px` : '0px'); }
    { // ★ tweak 4: the LIST dock at the bottom-left — the scale bar steps right of it
      const ld = this.root.querySelector('.mChipsBL .mDock') as HTMLElement | null;
      const lb = ld && !ld.hidden ? (ld.classList.contains('tucked') ? (ld.querySelector('.hTab') as HTMLElement).getBoundingClientRect() : (ld.querySelector('.hCard') as HTMLElement).getBoundingClientRect()) : null;
      setv('--trk-list-w', `${Math.ceil(lb?.width ?? 0) + 8}px`);
      setv('--trk-list-h', `${Math.ceil(lb?.height ?? 0) + 8}px`);
    }
    setv('--trk-card-w', `${W >= CARD_ROOMY_W ? CARD_W : CARD_W_SNUG}px`);
    const place = cardPlace(this.cardOrder.filter((id) => this.cardAvail.has(id)), W, H, this.oneSideNow);
    const open = [...place.L, ...place.R, ...place.B, ...place.H];
    const sig = JSON.stringify([place, this.narrow()]);
    for (const b of Array.from(this.root.querySelectorAll('[data-cbtn]')) as HTMLElement[]) {
      const on = open.includes(b.dataset.cbtn as CardId);
      b.setAttribute('aria-expanded', String(on));
      b.closest('.mDock')?.classList.toggle('open', on);
      if (b.classList.contains('mHelp')) b.classList.toggle('on', on);
    }
    this.cardsEl.classList.toggle('both', false);
    this.cardsEl.classList.toggle('lAndB', place.L.length > 0 && place.B.length > 0);   // ★ tweak 4: the left column and the LIST share the left edge
    if (sig === this.layoutSig) return;
    this.layoutSig = sig;
    // ★ every card to the park first (moving an element keeps it and its state — never innerHTML over them)
    const focused = document.activeElement as HTMLElement | null;
    for (const c of Array.from(this.root.querySelectorAll('.trkCard2')) as HTMLElement[]) {
      if (!open.includes(c.dataset.card as CardId)) c.hidden = true;
      if (c.parentElement !== this.parkEl) this.parkEl.appendChild(c);
    }
    for (const side of ['L', 'R', 'B', 'H'] as const) {
      const host = this.cardsEl.querySelector(`.c${side}`) as HTMLElement;
      for (const id of place[side]) { const c = this.root.querySelector(`.trkCard2[data-card="${id}"]`) as HTMLElement; c.hidden = false; host.appendChild(c); }
    }
    for (const side of ['L', 'R', 'B', 'H'] as const) for (const id of place[side]) {
      const x = this.root.querySelector(`.trkCard2[data-card="${id}"] > .ch .x`) as HTMLElement | null;
      if (x) x.textContent = side === 'L' ? '‹' : '›';   // ★ points at the edge the chip lives on
    }
    if (focused && focused !== document.activeElement && this.root.contains(focused) && !focused.closest('[hidden]')) focused.focus({ preventScroll: true });
    this.cardsAnchor(place);
    setTimeout(() => { this.placeAcObstacle(); try { this.af?.poke?.(); } catch { /* none */ } }, 0);
  }
  /** ★★★ tweak 4: each side's cards open AT their chip — the left / right column's top is its first open chip's top, the
   *  bottom card's foot the LIST chip's foot — so the chip seems to grow into its card (the health pill's way). */
  private cardsAnchor(place: Record<'L' | 'R' | 'B', CardId[]>) {
    const m = this.mapHost.getBoundingClientRect();
    const dockTop = (id: CardId) => (this.root.querySelector(`.mDock[data-dock="${id}"]`) as HTMLElement | null)?.getBoundingClientRect() ?? null;
    const top = (ids: CardId[]) => Math.min(...ids.map((id) => { const b = dockTop(id); return b ? b.top - m.top : 1e9; }));
    const setv = (k: string, v: string) => { if (this.root.style.getPropertyValue(k) !== v) this.root.style.setProperty(k, v); };
    if (place.L.length) setv('--trk-cl-top', `${Math.max(6, Math.round(top(place.L)))}px`);
    if (place.R.length) setv('--trk-cr-top', `${Math.max(6, Math.round(top(place.R)))}px`);
    if (place.B.length) { const b = dockTop('list'); if (b) setv('--trk-cb-bottom', `${Math.max(0, Math.round(m.bottom - b.bottom))}px`); }
  }
  /** ★★★ tweak 3 the LAYERS chip: a glyph and a count per layer present ([plane] 157 [ship] 3 [NAVTEX] 40); a slim column
   *  on a phone-width window (each glyph over its count). Entries: the host's (setLayerSummary), else the SHOW toggles'. */
  private lsumSig = '';
  private lsumHost = false;
  setLayerSummary(entries: Array<{ glyph: LayerGlyph; n: number; colour: string; words: string }>) {
    this.lsumHost = true;
    this.drawLayerSummary(entries);
  }
  private drawLayerSummary(entries: Array<{ glyph: LayerGlyph; n: number; colour: string; words: string }>) {
    const sig = JSON.stringify(entries);
    if (sig === this.lsumSig) return;
    this.lsumSig = sig;
    const el = this.root.querySelector('.mDock[data-dock="layers"] .lsum') as HTMLElement;
    el.innerHTML = entries.map((e) => `<span class="le" title="${esc(e.words)}"><span class="lg" style="color:${esc(e.colour)}">${LAYER_GLYPH[e.glyph] ?? LAYER_GLYPH.dot}</span><span class="ln">${esc(e.n)}</span></span>`).join('');
    const b = this.root.querySelector('[data-cbtn="layers"]') as HTMLElement;
    b.setAttribute('aria-label', `Layers — ${entries.map((e) => e.words).join(', ') || 'what the map shows'}`);
    // ★ its tucked tab: the main family's glyph in its colour (the aircraft where there are aircraft)
    const ti = this.root.querySelector('.mDock[data-dock="layers"] .hTabIco') as HTMLElement | null;
    if (ti) { const f = entries[0]; ti.innerHTML = LAYER_GLYPH[f?.glyph ?? 'aircraft'] ?? LAYER_GLYPH.dot; ti.style.color = f?.colour ?? ''; }
    this.layoutSig = ''; this.cardsLayout();
  }
  /** The live rate, top right ("ADS-B 1,425 msg/s" · "VDL2 153 msg/min") — '' = none (a viewer, nothing live). */
  setRate(text: string, title = '') {
    const el = this.root.querySelector('.trkTop .rate') as HTMLElement;
    if (el.textContent !== text) el.textContent = text;
    el.title = title;
  }
  /** The facts the old status paragraph carried, each in the card it belongs to ('' = none). */
  setFacts(f: Partial<Record<'list' | 'tune' | 'layers', string>>) {
    this.hostFacts = true;
    for (const k of ['list', 'tune', 'layers'] as const) {
      const el = this.root.querySelector(`.trkCard2[data-card="${k}"] .facts`) as HTMLElement | null;
      const t = f[k] ?? '';
      if (el && el.textContent !== t) el.textContent = t;
    }
  }
  private hostFacts = false;
  /** The MAP card's aircraft note ("Furthest: RYR12K 213 NM SE") — '' = none. */
  setFarNote(t: string) { const el = this.root.querySelector('.farNote') as HTMLElement; if (el.textContent !== t) el.textContent = t; }
  private srcSig = '';
  /** ★★★ tweak 3 SOURCES, in MAP CONTROLS: each radio and feed and what it adds ("PiAware 127", "RTL-SDR 36"), each row a
   *  show / hide toggle for what ONLY it heard (this viewer's own view, never sent). null / one source = no section. */
  setSources(rows: Array<{ id: string; name: string; what: string; value: string; on: boolean; down?: boolean }> | null, onToggle?: (id: string, on: boolean) => void) {
    const sect = this.root.querySelector('.srcSect') as HTMLElement;
    sect.hidden = !rows || rows.length < 2;
    if (onToggle) this.onSource = onToggle;
    const sig = JSON.stringify(rows);
    if (sig === this.srcSig) return;
    this.srcSig = sig;
    const body = this.root.querySelector('.srcBody') as HTMLElement;
    body.innerHTML = (rows ?? []).map((x) => `<button type="button" class="srcRow${x.down ? ' down' : ''}" data-src="${esc(x.id)}" aria-pressed="${x.on}" title="${esc(x.on ? 'Shown — tap to hide what only this source heard' : 'Hidden — tap to show it again')}">`
      + `<b>${esc(x.name)}</b><span class="sv">${esc(x.value)}</span><small>${esc(x.what)}</small></button>`).join('');
    for (const b of Array.from(body.querySelectorAll('[data-src]')) as HTMLButtonElement[]) b.onclick = () => this.onSource?.(b.dataset.src || '', b.getAttribute('aria-pressed') !== 'true');
  }
  private onSource: ((id: string, on: boolean) => void) | null = null;
  private srvSig2 = '';
  /** ★★★ tweak 3 THE SERVERS chip (Stuart 11-27: "must ALWAYS be reachable on every map screen, even when EXIT is hidden or
   *  the mode can't be left"): back out to this server's radios / landing, or over to another radio on it. */
  setServers(rows: Array<{ id: string; label: string; sub: string }>, onPick: (id: string) => void) {
    this.onServer = onPick;
    this.cardsAvail('servers', rows.length > 0);
    const sig = JSON.stringify(rows);
    if (sig === this.srvSig2) return;
    this.srvSig2 = sig;
    const body = this.root.querySelector('.srvBody') as HTMLElement;
    body.innerHTML = rows.map((x) => `<button type="button" class="srvPick" data-srv="${esc(x.id)}"><b>${x.id === 'back' ? '‹ ' : ''}${esc(x.label)}</b><small>${esc(x.sub)}</small></button>`).join('');
    for (const b of Array.from(body.querySelectorAll('[data-srv]')) as HTMLButtonElement[]) b.onclick = () => this.onServer?.(b.dataset.srv || '');
    this.layoutSig = ''; this.cardsLayout();
  }
  private onServer: ((id: string) => void) | null = null;
  /* ── ★★★ beta96 FIT TO ONE LAYER (Stuart 11-27: "with this much data the fit button doesnt work, maybe the fit button
   *  fits to the adsb layer or user chosen layer") — the shared auto fit and FIT frame ONE layer (ADS-B by default where
   *  there is ADS-B, else the map's main layer; the viewer's choice remembered), and drop the furthest few per cent (one
   *  aircraft at Tallinn must not zoom the whole UK view out). Never greyed while the layer has anything. ── */
  private fitGroups: Array<{ id: string; label: string; n: number }> = [];
  private fitPick = '';
  private fitDefault = '';
  private fitSig = '';
  setFitGroups(groups: Array<{ id: string; label: string; n: number }>, def: string) {
    this.fitGroups = groups; this.fitDefault = def;
    if (!this.fitPick) { try { this.fitPick = localStorage.getItem(FIT_KEY) || ''; } catch { /* private */ } }
    const host = this.root.querySelector('.fitSel') as HTMLElement, hd = this.root.querySelector('.fitToH') as HTMLElement;
    host.hidden = hd.hidden = groups.length < 2;
    const cur = this.fitChoice();
    const sig = JSON.stringify([groups.map((g) => g.id), cur]);
    if (sig !== this.fitSig) {
      this.fitSig = sig;
      host.innerHTML = groups.map((g) => `<button class="btn chip" type="button" role="radio" data-fit="${esc(g.id)}" aria-checked="${g.id === cur}" aria-pressed="${g.id === cur}">${esc(g.label)}<span class="n"></span></button>`).join('');
      for (const b of Array.from(host.querySelectorAll('[data-fit]')) as HTMLButtonElement[]) b.onclick = () => {
        this.fitPick = b.dataset.fit || '';
        try { localStorage.setItem(FIT_KEY, this.fitPick); } catch { /* private */ }
        this.fitSig = ''; this.setFitGroups(this.fitGroups, this.fitDefault);
        this.fitOnce();
      };
    }
    for (const b of Array.from(host.querySelectorAll('[data-fit]')) as HTMLElement[]) {
      const n = b.querySelector('.n') as HTMLElement, g = groups.find((x) => x.id === b.dataset.fit);
      const t = g ? String(g.n) : ''; if (n.textContent !== t) n.textContent = t;
    }
    this.fitTitles();
  }
  /** The layer FIT and AUTO frame now: the viewer's pick while that layer is on the map, else the default, else the first. */
  fitChoice(): string {
    const ids = this.fitGroups.map((g) => g.id);
    return ids.includes(this.fitPick) ? this.fitPick : ids.includes(this.fitDefault) ? this.fitDefault : ids[0] || '';
  }
  private fitTitles() {
    const g = this.fitGroups.find((x) => x.id === this.fitChoice());
    const b = this.root.querySelector('.fitOnce') as HTMLButtonElement;
    const what = g ? g.label : this.o.listTitle.toLowerCase();
    b.title = g && g.n === 0 ? `Nothing on the map from ${what} yet — FIT will frame it once there is` : `Fit the map to ${what} — far outliers (the furthest 5 %) are left out, so one far-off aircraft does not zoom the whole view out`;
    const note = this.root.querySelector('.fitNote') as HTMLElement;
    const t = this.fitGroups.length ? `AUTO and FIT frame ${what}, leaving out far outliers.` : '';
    if (note.textContent !== t) note.textContent = t;
  }
  /** The chosen layer's points, the furthest few per cent dropped (from the receiver, else from the middle of them). */
  private fitPoints(all: Array<{ lat: number; lon: number; g: string }>): [number, number][] {
    const ch = this.fitChoice();
    const pts = this.fitGroups.length ? all.filter((p) => p.g === ch) : all;
    return trimOutliers(pts, this.rx).map((p) => [p.lat, p.lon] as [number, number]);
  }
  private fitAll: Array<{ lat: number; lon: number; g: string }> = [];
  /** FIT pressed: the auto fit frames the chosen layer now (and AUTO, if it was paused, resumes on it). */
  fitOnce() {
    this.refit = false;
    try {
      if (this.af) { this.af.fitNow(); return; }   // ★ frames it now; AUTO (on, or paused by a touch) carries on from there
      this.layer?.fit?.();
    } catch (e) { console.error('[tracks] fit', e); }
  }
  get open() { return !this.root.hidden; }
  /** ★★ beta96 a host's own chrome in this view (ADS-B mode's GAIN, CHAT, capture warning; its list chips; its feed
   *  warning and gain pop-over): 'top' = in the top bar before the status line, 'head' = under the list's header, 'over' =
   *  over the map. Made on first use; the host moves its elements in and hides them when they do not apply. */
  slot(where: 'top' | 'head' | 'over' | 'share' | 'tune' | 'ctl'): HTMLElement {
    if (where === 'tune' || where === 'ctl') return this.root.querySelector(`.slot-${where}`) as HTMLElement;   // ★ beta96 in the TUNE / MAP cards
    const cls = `slot-${where}`;
    let el = this.root.querySelector(`.${cls}`) as HTMLElement | null;
    if (el) return el;
    el = document.createElement(where === 'top' || where === 'share' ? 'span' : 'div');
    el.className = `trkSlot ${cls}`;
    // ★★★ beta96 'share' = the SHARED TUNER line + CHAT, right after the title: first in the sideways row, so a phone
    //   shows it without scrolling (main.ts mapSharedSync)
    if (where === 'share') (this.root.querySelector('.ttl') as HTMLElement).after(el);
    else if (where === 'top') (this.root.querySelector('.trkTop .rate') as HTMLElement).before(el);   // ★ CHAT before the rate
    else if (where === 'head') this.rowsEl.before(el);
    else this.mapHost.appendChild(el);
    return el;
  }
  private tglSig = '';
  /** ★★ V12 2026-11-02 the layer toggles, redrawn (only when they changed — a poll must not steal a focused button). */
  setToggles(toggles: Array<{ id: string; label: string; words: string; group?: string; live?: boolean; n?: number; colour?: string }>, toggled: Record<string, boolean>) {
    const host = this.root.querySelector('.tgl') as HTMLElement;
    // ★★★ beta96 SHOW (Stuart 11-26): the counts change every second — written in place (below), never a redraw, so a poll
    //   cannot steal a focused chip
    const counts = () => {
      const by = new Map(toggles.map((x) => [x.id, x.n]));
      for (const b of Array.from(host.querySelectorAll('[data-tgl]')) as HTMLElement[]) {
        const n = b.querySelector('.n') as HTMLElement | null; if (!n) continue;
        const v = by.get(b.dataset.tgl || ''); const t = v != null ? String(v) : '';
        if (n.textContent !== t) n.textContent = t;
      }
    };
    const sig = JSON.stringify([toggles.map((x) => [x.id, x.group || '', !!x.live, x.n != null, x.colour || '']), toggles.map((x) => toggled[x.id] !== false)]);
    if (sig === this.tglSig) { counts(); if (!this.lsumHost) this.drawLayerSummary(toggles.filter((x) => x.n != null && toggled[x.id] !== false).map((x) => ({ glyph: glyphOf(x.id), n: x.n!, colour: x.colour || '#9fb0c2', words: `${x.label} ${x.n}` }))); return; }
    this.tglSig = sig;
    host.hidden = !toggles.length;
    this.viewSync();
    // ★★ beta96 THE LIVE MAPS (liveMaps.ts liveChips): a group's name before its first chip, a red LIVE dot on the live
    //   layer's (what the receiver is doing now). ★★★ SHOW (Stuart 11-26): THIS viewer's own filter, labelled, the community
    //   map's chips — a colour dot, the name, its count; nothing here is ever sent to the server.
    host.innerHTML = (toggles.length ? '<span class="lbl" title="What this map shows you — your own view, nothing changes on the radio">SHOW</span>' : '')
      + toggles.map((x) => (x.group ? `<span class="grp" aria-hidden="true">${esc(x.group)}</span>` : '')
      + `<button class="btn chip${x.live ? ' live' : ''}" type="button" data-tgl="${esc(x.id)}" aria-pressed="${toggled[x.id] !== false}" aria-label="${esc(x.words)}" title="${esc(x.words)}">`
      + (x.colour ? `<span class="dot" style="background:${esc(x.colour)};color:${esc(x.colour)}"></span>` : '')
      + `${esc(x.label)}<span class="n"></span></button>`).join('');
    counts();
    if (!this.lsumHost) this.drawLayerSummary(toggles.filter((x) => x.n != null && toggled[x.id] !== false).map((x) => ({ glyph: glyphOf(x.id), n: x.n!, colour: x.colour || '#9fb0c2', words: `${x.label} ${x.n}` })));
    for (const b of Array.from(host.querySelectorAll('[data-tgl]')) as HTMLButtonElement[]) {
      b.onclick = () => {
        const on = b.getAttribute('aria-pressed') !== 'true';
        b.setAttribute('aria-pressed', String(on));
        this.o.onToggle?.(b.dataset.tgl || '', on);
      };
    }
  }
  private scopeSig = '';
  /** ★★★ beta96 OTHER RADIOS ON THIS SERVER (mapScope.ts): a SHOW chip of its own after the layer chips — this viewer's view,
   *  never sent, never dial-locked. null = none (one source, the viewer). */
  setScope(chip: { label: string; words: string; on: boolean } | null, onToggle: (on: boolean) => void) {
    const host = this.root.querySelector('.trkCard2 .scope') as HTMLElement;
    host.hidden = !chip;
    (this.root.querySelector('.scopeSect') as HTMLElement).hidden = !chip;
    this.viewSync();
    const sig = chip ? JSON.stringify(chip) : '';
    if (sig !== this.scopeSig) {
      this.scopeSig = sig;
      host.innerHTML = chip ? `<button class="btn chip" type="button" data-scope="others" aria-pressed="${chip.on}" aria-label="${esc(chip.words)}" title="${esc(chip.words)}">${esc(chip.label)}</button>` : '';
    }
    const b = host.querySelector('[data-scope]') as HTMLButtonElement | null;
    if (b) b.onclick = () => onToggle(b.getAttribute('aria-pressed') !== 'true');
  }
  /** ★★★ beta96 what the scope hides, under the status line ('' = nothing hidden). */
  setScopeNote(words: string) {
    const el = this.root.querySelector('.scopeNote2') as HTMLElement;
    if (el.textContent !== words) el.textContent = words;
    // ★ the VIEW button says a filter is on (its words are in the card)
    (this.root.querySelector('[data-cbtn="layers"]') as HTMLElement).classList.toggle('filtered', !!words);
  }
  private tuneSig = '';
  private tuneRows: Array<{ id: string; name: string; freq: string; icon: string; group: string; live: boolean; why: string }> = [];
  /** ★★★ beta96 TUNE (Stuart 11-26): ONE collapsible chip in the top bar — "TUNE · VDL2 136.700–136.975 MHz ▾", the live
   *  mode — that opens a LIST of the whole-radio modes this radio can run (airSeaTune.ts: the server's own tables, grouped,
   *  an icon, the name, the frequencies; greyed with the server's reason where it cannot run). The same chip on every width.
   *  A row picked → onTune (the host tunes, under the ask / lock rules), the list shuts. The chip carries data-dial, so the
   *  dial lock (setDialLock) greys it and every row with its reason. [] = no chip (no radio behind the map). Touch, mouse,
   *  keyboard and the pad: the chip and every row are buttons; ↑ ↓ move between rows, Escape shuts the list. */
  setTune(rows: Array<{ id: string; name: string; freq: string; icon: string; group: string; live: boolean; why: string }>, chip: string, onTune: (id: string) => void) {
    /* ★★★ beta96 (map cards, Stuart 11-27) the TUNE list is the TUNE CARD's body now: its button in the bar, the rows in
     *  the card (the same rows, the same lock and refusal rules); `chip` is the live mode's words, the button's title.
     *  Opening the card moves nothing — the ROWS carry data-dial. */
    const pop = this.root.querySelector('.tunePop') as HTMLElement;
    if (!pop.dataset.wired) {
      pop.dataset.wired = '1';
      pop.addEventListener('keydown', (e) => {
        const items = Array.from(pop.querySelectorAll('[data-tune]')) as HTMLElement[];
        const i = items.indexOf(document.activeElement as HTMLElement);
        if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); items[(i + (e.key === 'ArrowDown' ? 1 : items.length - 1)) % items.length]?.focus(); }
      });
    }
    this.onTune = onTune;
    this.tuneRows = rows;
    const btn = this.root.querySelector('[data-cbtn="tune"]') as HTMLButtonElement;
    btn.title = chip ? `${chip} — tune the radio (for everybody listening on a shared dial)` : 'Tune the radio';
    const sig = JSON.stringify(rows);
    if (sig !== this.tuneSig) {
      this.tuneSig = sig;
      let last = '';
      pop.innerHTML = rows.map((r) => {
        const head = r.group !== last ? `<div class="tpG" role="presentation">${esc(r.group)}</div>` : '';
        last = r.group;
        return head + `<button type="button" class="tpRow${r.live ? ' live' : ''}" role="menuitemradio" aria-checked="${r.live}" data-tune="${esc(r.id)}" data-dial="1"`
          + `${r.why ? ` aria-disabled="true" data-why="${esc(r.why)}" title="${esc(r.why)}"` : ''}>`
          + `<svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor" aria-hidden="true"><path d="${esc(r.icon)}"/></svg>`
          + `<span class="nm">${esc(r.name)}</span><span class="fq">${esc(r.freq)}</span>${r.live ? '<span class="lv">LIVE</span>' : ''}</button>`;
      }).join('');
      for (const b of Array.from(pop.querySelectorAll('[data-tune]')) as HTMLButtonElement[]) {
        b.onclick = () => {
          if (b.dataset.why) { this.o.onTuneRefused?.(b.dataset.why); return; }
          if (b.getAttribute('aria-checked') !== 'true') this.onTune?.(b.dataset.tune || '');
        };
      }
      if (this.dialWhy) this.setDialLock(this.dialWhy, this.dialExit);   // ★ new rows: the lock goes back on them
    }
    pop.hidden = !rows.length;
    this.tuneSync();
  }
  /** TUNE is offered when it has rows or the host put a control in it (ADS-B's GAIN) — never an empty card. */
  tuneSync() {
    const slot = this.root.querySelector('.slot-tune') as HTMLElement;
    const any = this.tuneRows.length > 0 || Array.from(slot.children).some((c) => !(c as HTMLElement).hidden);
    if (any !== this.cardAvail.has('tune')) { this.cardsAvail('tune', any); this.cardsLayout(); }
  }
  /** VIEW is offered while it has a SHOW chip or the scope chip. */
  private viewSync() {
    const t = this.root.querySelector('.trkCard2 .tgl') as HTMLElement | null, sc = this.root.querySelector('.trkCard2 .scope') as HTMLElement | null;
    if (!t || !sc) return;
    const any = !t.hidden || !sc.hidden;
    if (any !== this.cardAvail.has('layers')) { this.cardsAvail('layers', any); this.cardsLayout(); }
  }
  /** Open / shut the TUNE card (the old chip's list) — kept for the host and the drivers. */
  tuneOpen(on: boolean) {
    if (on && !this.tuneRows.length) on = false;
    this.cardOpen('tune', on, false);
    if (on) ((this.root.querySelector('.tunePop .tpRow.live') || this.root.querySelector('.tunePop .tpRow')) as HTMLElement | null)?.focus();
  }
  get tuneList(): Array<{ id: string; name: string; freq: string; group: string; live: boolean; why: string; locked: boolean }> {
    return (Array.from(this.root.querySelectorAll('.tunePop [data-tune]')) as HTMLElement[]).map((b) => {
      const r = this.tuneRows.find((x) => x.id === b.dataset.tune)!;
      return { id: r.id, name: r.name, freq: r.freq, group: r.group, live: r.live, why: r.why, locked: b.classList.contains('dialLocked') };
    });
  }
  private onTune: ((id: string) => void) | null = null;
  /** ★★ V12 2026-11-16 the AVIATION MAP's AIRPORTS layer: the basemap's profile, switched live ('aero' ↔ 'aero-plain'),
   *  on whichever map this view made (GPU or Leaflet). Remembered for a map made later. */
  setProfile(name: string) {
    if (this.o.profile === name) return;
    this.o.profile = name;
    (window as any).__vibeTrkProfile = name;   // ★ for drivers (scripts/test-web-avmap.mjs), like __vibeTrkMap
    try {
      // ★ a server whose map pack predates 'aero-plain' says no: the airport layers are switched by name instead
      if (this.vm && this.vm.setProfile?.(name) === false) {
        // ★ beta96 'airsea' / 'airsea-plain' on an older pack: its aero profile, then the ports switched on by name
        const sea = name === 'airsea' || name === 'airsea-plain';
        const base = name === 'airsea' ? 'aero' : name === 'airsea-plain' ? 'aero-plain' : name;
        if (sea) this.vm.setProfile?.(base);
        const m = this.vm.map;
        for (const id of ['airports-4', 'airports-6', 'airports-7', 'airports-8', 'runways', 'runway-ends'])
          if (m?.getLayer?.(id)) m.setLayoutProperty(id, 'visibility', base === 'aero-plain' ? 'none' : 'visible');
        if (sea && m?.getLayer?.('ports')) m.setLayoutProperty('ports', 'visibility', 'visible');
      }
    } catch (e) { console.error('[tracks] profile', e); }
    try { if (this.vmL?.setProfile?.(name) === false) this.vmL?.setProfile?.(name === 'airsea' ? 'aero' : name === 'airsea-plain' ? 'aero-plain' : name); } catch (e) { console.error('[tracks] profile', e); }
  }
  get profile(): string { return this.o.profile || ''; }
  /** ★ V12 2026-11-02: the time filter's pressed button, from outside (the two shipping views share one filter). */
  setRange(id: string) {
    for (const b of Array.from(this.root.querySelectorAll('[data-rng]')) as HTMLElement[]) b.setAttribute('aria-pressed', String(b.dataset.rng === id));
  }
  /** ★★★ beta96 THE LIVE MAPS (liveMaps.ts liveChrome): the title and the exit key are COMPUTED from the current mode on
   *  every render and set here — a view is never left wearing the mode it was made for (Mac Ha 11-26). */
  setChrome(title: string, exitLabel: string) {
    const t = this.root.querySelector('.ttl') as HTMLElement, x = this.root.querySelector('.exit') as HTMLElement;
    if (t.textContent !== title) { t.textContent = title; this.root.setAttribute('aria-label', title + ' map'); }
    if (x.textContent !== exitLabel) x.textContent = exitLabel;
  }
  /** ★★★ beta96 THE DIAL LOCK ON A MAP (Stuart 11-26): while the server says this listener may not move the shared radio
   *  (ask before tuning, the owner's scan), the controls that WOULD move it are greyed with `why` — EXIT when `exitToo`
   *  (it leaves a whole-radio mode, which retunes the radio) and every element the host marked `data-dial` (band keys,
   *  GAIN). A press on one is refused here and handed to `onDialLocked` (the host's nudge: the line flashes, CHAT pulses).
   *  ✗ NEVER the map itself: pan, zoom, FIT, layer chips, tabs, sort, cards, KEY, labels and the time filter are each
   *  listener's own view and stay live. '' = unlocked. */
  private dialWhy = '';
  private dialExit = false;
  onDialLocked: ((why: string) => void) | null = null;
  setDialLock(why: string, exitToo: boolean) {
    this.dialWhy = why; this.dialExit = !!why && exitToo;
    const x = this.root.querySelector('.exit') as HTMLElement;
    const mark = (el: HTMLElement, on: boolean) => {
      el.classList.toggle('dialLocked', on);
      if (on) { el.setAttribute('aria-disabled', 'true'); if (el.dataset.dialTitle === undefined) el.dataset.dialTitle = el.title || ''; el.title = why; }
      else if (el.classList.contains('dialLockedWas') || el.dataset.dialTitle !== undefined) {
        // ★ a TUNE row the SERVER greys (data-why) stays greyed with its own reason when the lock lifts
        if (el.dataset.why) { el.setAttribute('aria-disabled', 'true'); el.title = el.dataset.why; }
        else { el.removeAttribute('aria-disabled'); el.title = el.dataset.dialTitle || ''; }
        delete el.dataset.dialTitle;
      }
    };
    mark(x, this.dialExit);
    // ★★★ tweak 3: the TUNE chip FADES while this listener must ask first (the ask shield's look); a press nudges
    this.root.querySelector('.mDock[data-dock="tune"]')?.classList.toggle('askFaded', !!why);
    for (const el of Array.from(this.root.querySelectorAll('[data-dial], [data-dial] button')) as HTMLElement[]) if (el.tagName === 'BUTTON' || el.tagName === 'SELECT' || el.tagName === 'INPUT') mark(el, !!why);
  }
  get dialLocked(): { why: string; exit: boolean } { return { why: this.dialWhy, exit: this.dialExit }; }
  /** A press the lock refuses: true = refused (the host was told). */
  private dialRefuses(t: EventTarget | null): boolean {
    if (!this.dialWhy) return false;
    const el = t as HTMLElement | null;
    if (!el?.closest) return false;
    const hit = (this.dialExit && el.closest('.trkTop .exit')) || el.closest('[data-dial]');
    if (!hit) return false;
    this.onDialLocked?.(this.dialWhy);
    return true;
  }
  get title(): string { return (this.root.querySelector('.ttl') as HTMLElement).textContent || ''; }
  get exitLabel(): string { return (this.root.querySelector('.exit') as HTMLElement).textContent || ''; }
  private rngSig: string | null = null;
  /** ★ beta96 the segmented control, redrawn when what it offers changes (the time filter for the maritime layers, the
   *  DATALINK's bands) — [] = none. `onRange` is the view's own. */
  setRanges(ranges: Array<{ id: string; label: string; words: string }>, current: string, label = '', hint = '') {
    const sig = JSON.stringify([ranges.map((r) => r.id), label, hint]);
    let host = this.root.querySelector('.rng:not(.tgl):not(.scope)') as HTMLElement | null;
    if (sig !== this.rngSig) {
      this.rngSig = sig;
      if (!host) { host = document.createElement('span'); host.className = 'rng'; host.setAttribute('role', 'group'); (this.root.querySelector('.rngSect') as HTMLElement).appendChild(host); }
      host.hidden = !ranges.length;
      (this.root.querySelector('.rngSect') as HTMLElement).hidden = !ranges.length;
      host.setAttribute('aria-label', label || 'Show what was heard in the last…'); host.title = hint;
      host.innerHTML = ranges.map((x) => `<button class="btn" type="button" data-rng="${esc(x.id)}" aria-label="${esc(x.words)}">${esc(x.label)}</button>`).join('');
      for (const b of Array.from(host.querySelectorAll('[data-rng]')) as HTMLButtonElement[]) {
        b.onclick = () => {
          for (const o2 of Array.from(host!.querySelectorAll('[data-rng]'))) o2.setAttribute('aria-pressed', String(o2 === b));
          this.o.onRange?.(b.dataset.rng || '');
          this.refit = true;
        };
      }
    }
    if (host) for (const b of Array.from(host.querySelectorAll('[data-rng]')) as HTMLElement[]) b.setAttribute('aria-pressed', String(b.dataset.rng === current));
  }
  private tabSig = '';
  /** ★★ beta96 the list's TABS (AIRCRAFT · MESSAGES · SHIPS …, liveMaps.ts liveTabs) in its header, with their counts.
   *  [] = the plain list title. A tab pressed calls `onTab`; the host draws that tab's rows. */
  setTabs(tabs: Array<{ id: string; label: string; n: number }>, current: string, onTab: (id: string) => void) {
    const host = this.root.querySelector('.hd .tabs') as HTMLElement, lt = this.root.querySelector('.hd .lt') as HTMLElement;
    host.hidden = tabs.length < 2; lt.hidden = tabs.length >= 2; this.countEl.hidden = tabs.length >= 2;
    const sig = JSON.stringify([tabs, current]);
    if (sig === this.tabSig) return;
    this.tabSig = sig;
    if (tabs.length === 1) lt.textContent = tabs[0].label;
    host.innerHTML = tabs.map((t) => `<button class="tab" type="button" role="tab" data-tab="${esc(t.id)}" aria-selected="${t.id === current}">${esc(t.label)}<span class="tn">${t.n}</span></button>`).join('');
    for (const b of Array.from(host.querySelectorAll('[data-tab]')) as HTMLButtonElement[]) b.onclick = (e) => { e.stopPropagation(); onTab(b.dataset.tab || ''); };
  }
  get tab(): string { return (this.root.querySelector('.hd .tab[aria-selected="true"]') as HTMLElement | null)?.dataset.tab || ''; }
  private srtSig = '';
  /** ★★ beta96 the list's sort chip, NEAREST | LATEST — null = none (another tab, or no receiver position). `what` names
   *  the rows for a screen reader ("aircraft"; the mesh maps' "nodes" — Stuart 11-26). */
  setSort(sort: string | null, onSort: (s: string) => void, what = 'aircraft') {
    const host = this.root.querySelector('.hd .srt') as HTMLElement;
    host.hidden = !sort;
    const sig = sort ? `${sort}|${what}` : '';
    if (sig === this.srtSig) return;
    this.srtSig = sig;
    host.setAttribute('aria-label', `Sort the ${what}`);
    host.innerHTML = sort ? [['nearest', 'NEAREST', 'nearest first'], ['latest', 'LATEST', 'most recently heard first']].map(([id, l, w]) => `<button type="button" data-srt="${id}" aria-pressed="${id === sort}" title="Sort the ${esc(what)} ${w}">${l}</button>`).join('') : '';
    for (const b of Array.from(host.querySelectorAll('[data-srt]')) as HTMLButtonElement[]) b.onclick = (e) => { e.stopPropagation(); onSort(b.dataset.srt || ''); };
  }
  /** ★★ beta96 the MESSAGES filter line ("Messages from RYR12K · ALL") — '' = none. */
  setFilter(words: string, onClear: () => void) {
    const f = this.root.querySelector('.filt') as HTMLElement;
    f.hidden = !words;
    if (!words) { f.innerHTML = ''; return; }
    if (f.dataset.w === words) return;
    f.dataset.w = words;
    f.innerHTML = `<span>${esc(words)}</span><button type="button" aria-label="Show every message">× ALL</button>`;
    (f.querySelector('button') as HTMLButtonElement).onclick = () => { f.dataset.w = ''; onClear(); };
  }
  /** ★ beta96 the camera's owner on a map with aircraft AND other things: 'aircraft' = the ADS-B layer's AUTO / FIT (the
   *  aviation layers live), 'track' = the shared auto fit over the other items (ships, sondes, NAVTEX live). */
  private camOwner: 'aircraft' | 'track' = 'aircraft';
  private acAuto: boolean | null = null;
  setCamera(owner: 'aircraft' | 'track') {
    if (owner === this.camOwner) return;
    this.camOwner = owner;
    this.applyCamera();
  }
  private applyCamera() {
    if (!this.acLayer) { this.fitBtn.hidden = false; return; }
    // ★★★ beta96 FIT TO ONE LAYER (Stuart 11-27): on the AIR & SEA map the shared auto fit always owns the camera, over the
    //     layer chosen in the MAP card (ADS-B by default) — the ADS-B layer's own AUTO / FIT keys are hidden there
    const track = this.o.mixed ? true : this.camOwner === 'track';
    this.fitBtn.hidden = !track;
    try {
      if (track) { if (this.acAuto == null) this.acAuto = !!this.acLayer.prefs?.().autoZoom; this.acLayer.setPrefs?.({ autoZoom: false }); this.af?.resume?.(); this.af?.fitNow?.(); }
      else { if (this.acAuto != null) { this.acLayer.setPrefs?.({ autoZoom: this.acAuto }); this.acAuto = null; } this.af?.pause?.(); }
    } catch (e) { console.error('[tracks] camera', e); }
  }
  /** ★★ beta96 stage 8 (NDB): another layer on THIS view's map (web/mapkit/ndblayer.js). `cb` gets the map as it is made
   *  ({ gl: the VibeMapGL, or null; lm: the Leaflet map, or null }) and null just before it is destroyed (the view hidden).
   *  Returns the unsubscribe. A host layer never makes a map of its own (one WebGL context per view). */
  private mapCbs = new Set<(m: { gl: any; lm: any } | null) => void>();
  whenMap(cb: (m: { gl: any; lm: any } | null) => void): () => void {
    this.mapCbs.add(cb);
    if (this.layer && (this.vm || this.lmap)) { try { cb({ gl: this.vm, lm: this.vm ? null : this.lmap }); } catch (e) { console.error('[tracks] whenMap', e); } }
    return () => { this.mapCbs.delete(cb); };
  }
  private mapSay(m: { gl: any; lm: any } | null) { for (const cb of Array.from(this.mapCbs)) try { cb(m); } catch (e) { console.error('[tracks] whenMap', e); } }
  /** ★ beta96 stage 8 a host layer's points for the shared auto fit (the NDB beacons heard) — null = none. */
  morePoints: (() => [number, number][]) | null = null;
  /** ★ beta96 the click order for a host layer: is an aircraft under this press? (it is selected if so) */
  pickAircraft(x: number, y: number, touch: boolean): boolean {
    try { return !!this.acLayer?.pickAt?.(x, y, touch); } catch { return false; }
  }
  /** ★ an item to select when its row is pressed (a MESSAGES row names its aircraft's key) — `onRow` hears every press. */
  onRow: ((key: string) => void) | null = null;
  private keySig = '';
  /** ★★ V12 2026-11-02 the KEY, redrawn — it lists the glyphs of the layers this map has (ships, DSC, hazards, arrows). */
  setLegend(html: string) {
    if (html === this.keySig) return;
    this.keySig = html;
    const d = this.root.querySelector('.trkKey') as HTMLElement | null;
    if (!d) return;
    (d.querySelector('.kb') as HTMLElement).innerHTML = html;
    (this.root.querySelector('.keySect') as HTMLElement).hidden = !html;   // ★ tweak 3 the KEY is a fold in MAP CONTROLS
  }
  private setMin(min: boolean, save = true) {
    this.list.classList.toggle('min', min);
    (this.list.querySelector('.mn') as HTMLElement).textContent = min ? '▴' : '▾';
    if (save) try { localStorage.setItem(MIN_KEY, min ? '1' : '0'); } catch { /* private window */ }
  }
  /** ★ the row under the last press — a list redrawn every second (ADS-B's live report) can replace it between the press
   *  and its click: found again by its key (the ADS-B view's rule, adsbDown, carried over in beta96). */
  private down = { key: '', at: 0, x: 0, y: 0 };
  private rowOf(e: Event): HTMLElement | null {
    let row = (e.target as HTMLElement).closest('.trkRow') as HTMLElement | null;
    const me = e as MouseEvent;
    if (!row && e.type === 'click' && this.down.key && performance.now() - this.down.at < 1000
        && Math.abs(me.clientX - this.down.x) < 12 && Math.abs(me.clientY - this.down.y) < 12) {
      const cssEsc = (globalThis as any).CSS?.escape as ((v: string) => string) | undefined;
      row = this.rowsEl.querySelector(`.trkRow[data-key="${cssEsc ? cssEsc(this.down.key) : this.down.key.replace(/["\\]/g, '')}"]`);
    }
    return row;
  }
  private rowPick(e: Event) {
    const row = this.rowOf(e);
    if (!row) return;
    const k = row.dataset.focus || row.dataset.key || '';
    try { this.onRow?.(row.dataset.key || ''); } catch (e) { console.error('[tracks] onRow', e); }
    if (row.dataset.nofocus) { this.markSel(row.dataset.key || ''); return; }
    const hex = this.acKeys.get(k);
    // ★ beta96 a row chosen is where the person wants the map (mapAutoFit's own rule): the auto fit pauses, or its next
    //   tick glides straight back out to everything (the AIR & SEA map pokes it on every feed) — FIT brings AUTO back
    if (!hex || !this.acLayer) this.selPause();
    if (!(hex && this.acLayer ? this.acLayer.focus(hex) : this.layer?.focus(k))) this.note('Not on the map — no position heard yet.', 2500);
    this.markSel(k);
  }
  /** ★ beta96 the selection changed (an icon tapped, a row pressed) — the AIR & SEA map filters its MESSAGES by it. */
  onSel: ((key: string) => void) | null = null;
  /* ★★ beta96 A MESH NODE's ROUTE (src/utils/meshRoute.ts — Stuart 11-26: "clicking a node lights up the repeaters its
   *  advert passed through"): MESH_ROUTE_JS draws it hop by hop on this view's map; null clears it. The ISM maps call it
   *  from onSel — another tap (another node, or the empty map) clears it. */
  private rtMap: any = null; private rtL: any = null; private rtH: { clear(): void } | null = null;
  showRoute(rt: unknown | null) {
    try { this.rtH?.clear(); } catch { /* gone */ }
    this.rtH = null;
    if (!rt || !this.rtMap || !this.rtL) return;
    const w = window as any;
    if (!w.meshRouteDraw) runSource(MESH_ROUTE_JS);
    try { this.rtH = w.meshRouteDraw(this.rtMap, this.rtL, rt); } catch (e) { console.error('[tracks] route', e); }
  }
  /** ★★ beta96 is a route drawn now (the test's handle) */
  get routeShown(): boolean { return !!this.rtH; }
  /* ★★ beta96 THE CARD's ROUTE BUTTON (meshRoute.ts meshRouteButtonHtml): `routeOf` gives a node's route; ROUTE draws it
   *  (and fits the path), ROUTE again or the selection changing / clearing (the card closing) takes it away. The host
   *  passes routeCard() as the view's onCard. */
  private rtKey = '';
  private routeOf: ((key: string) => unknown | null) | null = null;
  setRouteSource(f: (key: string) => unknown | null) { this.routeOf = f; }
  routeCard(_key: string, el: HTMLElement) {
    const b = el.querySelector('.vmrBtn[data-rtk]') as HTMLButtonElement | null;
    if (!b) return;
    b.onclick = (e) => { e.stopPropagation(); this.routeToggle(b.dataset.rtk || ''); };
    this.routeBtn();
  }
  routeToggle(k: string) {
    if (!k || this.rtKey === k) { this.rtKey = ''; this.showRoute(null); }
    else { const rt = this.routeOf?.(k) ?? null; this.rtKey = rt ? k : ''; this.showRoute(rt); }
    this.routeBtn();
  }
  private routeBtn() {
    for (const b of Array.from(this.root.querySelectorAll('.vmrBtn[data-rtk]')) as HTMLElement[]) {
      const on = !!this.rtKey && b.dataset.rtk === this.rtKey;
      b.setAttribute('aria-pressed', String(on)); b.textContent = on ? 'HIDE ROUTE' : 'ROUTE';
    }
  }
  /* ★★ beta96 A SELECTION HOLDS THE VIEW (Stuart's 11-26 recording, the ISM mesh map: a node picked from NODES was flown
   *  to, then the next refresh glided back out to all of England). mapAutoFit's rule, for every way a selection is made —
   *  a row, a link, a tap on the map (and so a mesh route shown): the auto fit PAUSES until FIT is pressed or the
   *  selection is cleared (then it resumes; AUTO switched OFF stays off).
   *  Not while the ADS-B layer owns the camera (its AUTO / FIT keys). */
  private selPaused = false;
  private selPause() {
    if (this.acLayer && this.camOwner !== 'track') return;
    try { const st = this.af?.state?.(); if (st?.on) { if (!st.paused) this.af.pause(); this.selPaused = true; } } catch { /* none */ }
  }
  private selResume() {
    if (!this.selPaused) return;
    this.selPaused = false;
    if (this.acLayer && this.camOwner !== 'track') return;
    try { const st = this.af?.state?.(); if (st?.on && st.paused) this.af.resume(); } catch { /* none */ }
  }
  private markSel(k: string) {
    if (k && k !== this.sel) this.selPause(); else if (!k && this.sel) this.selResume();
    if (k !== this.sel) try { this.onSel?.(k); } catch (e) { console.error('[tracks] onSel', e); }
    if (this.rtKey && k !== this.rtKey) { this.rtKey = ''; this.showRoute(null); this.routeBtn(); }   // ★★ beta96 the card closed / another chosen
    // ★★ beta96 a tap zooms in too (the layer's own focus: the fly, the rings — select() on the same key does nothing more)
    if (this.o.zoomOnSelect && k && k !== this.sel) setTimeout(() => { if (this.sel === k) try { this.layer?.focus(k); } catch { /* gone */ } }, 0);
    this.sel = k;
    for (const el of Array.from(this.rowsEl.querySelectorAll('.trkRow.sel'))) el.classList.remove('sel');
    const cssEsc = (globalThis as any).CSS?.escape as ((v: string) => string) | undefined;
    if (k) this.rowsEl.querySelector(`.trkRow[data-key="${cssEsc ? cssEsc(k) : k.replace(/["\\]/g, '')}"]`)?.classList.add('sel');
  }
  private select(k: string) { this.layer?.select(k || null); this.markSel(k); }
  private wantFocus = '';
  private refit = false;
  /** ★★★ beta92 THE AUTO FIT (src/utils/mapAutoFit.ts): one per map shown, made in attach, gone in hide. Not on the aviation
   *  map's aircraft view — the ADS-B layer frames its aircraft with its own AUTO / FIT keys. */
  private af: any = null;
  private fitBtn: HTMLButtonElement;
  /** The items changed: the auto fit considers a move (its hysteresis decides; a 1 s tick does the rest). */
  /** ★ beta96 stage 8 a host layer's points changed (morePoints): the auto fit considers a move. */
  pokeFit() { this.autoFitCheck(); }
  /** ★ a host layer's item chosen: the auto fit pauses (FIT brings AUTO back), as a row chosen here does. */
  pauseFit() { try { this.af?.pause(); } catch { /* none */ } }
  private autoFitCheck(_items?: any[]) { try { this.af?.poke(); } catch (e) { console.error('[tracks] auto fit', e); } }
  /** ★ The card again from its item (a fetched text arrived for it). */
  refreshCard() { try { this.layer?.refreshCard?.(); } catch (e) { console.error('[tracks] card', e); } }
  /** ★ beta95 the padded view [s, w, n, e] (the sea areas drawn worldwide are those in it), null before the map is made. */
  viewBox(): [number, number, number, number] | null { try { return this.layer?.viewBox?.() ?? null; } catch { return null; } }
  /** Select an item and fly to it (a row tapped outside the map, e.g. the APRS box). Before the map is up: when it is. */
  focus(key: string): boolean {
    this.refit = false;
    this.selPause();   // ★ a focus (a node's link) is where the map should be — FIT brings AUTO back
    if (!this.layer) { this.wantFocus = key; return false; }
    const hex = this.acKeys.get(key);
    const ok = hex && this.acLayer ? !!this.acLayer.focus(hex) : !!this.layer.focus(key);
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
      if (this.o.liveIds?.includes(el.id)) continue;
      el.inert = true; this.inerted.push(el);
    }
    for (const id of this.o.liveIds ?? []) { const el = document.getElementById(id); if (el) el.style.zIndex = '700'; }
    (this.root.querySelector('.exit') as HTMLElement).focus();
    this.boot(++this.gen).catch((e) => { console.error('[tracks] the map could not load', e); this.note('The map did not finish loading — the list still updates.'); });
  }
  hide() {
    if (!this.open) return;
    this.tuneOpen(false);
    this.root.hidden = true;
    this.attribObs?.disconnect(); this.attribObs = null;
    for (const el of this.inerted) el.inert = false;
    this.inerted = [];
    for (const id of this.o.liveIds ?? []) { const el = document.getElementById(id); if (el) el.style.zIndex = ''; }
    this.gen++;
    this.mapSay(null);   // ★ beta96 stage 8 a host layer (NDB) lets go first
    try { this.af?.destroy(); } catch (e) { console.error('[tracks] auto fit destroy', e); }
    this.af = null;
    try { this.acLayer?.destroy(); } catch (e) { console.error('[tracks] aircraft layer destroy', e); }
    this.acLayer = null; this.acMap = null; this.acAuto = null; this.acKeys.clear(); this.acHex.clear();
    (this.root.querySelector('.ctlSect') as HTMLElement).hidden = true;
    (this.root.querySelector('.slot-ctl') as HTMLElement).innerHTML = '';
    this.showRoute(null); this.rtMap = null; this.rtL = null; this.rtKey = '';   // ★★ beta96 a route drawn goes with its map
    try { this.layer?.destroy(); } catch (e) { console.error('[tracks] layer.destroy', e); }
    this.layer = null;
    try { this.vm?.destroy(); } catch (e) { console.error('[tracks] map destroy', e); }
    this.vm = null;
    try { this.lmap?.remove(); } catch (e) { console.error('[tracks] Leaflet remove', e); }
    this.lmap = null; this.vmL = null;
    // ★ V12 2026-11-02: the KEY stays (it is the view's, not the map's) — it was removed here, so a map closed and opened
    //   again had no KEY (and setLegend found nothing to fill)
    // ★ beta96 the cards (open ones and the park) are the view's, not the map's — they stay
    for (const el of Array.from(this.mapHost.children)) if (!el.classList.contains('trkCards') && !el.classList.contains('trkPark') && !el.classList.contains('trkSlot') && !el.classList.contains('cardBtns')) el.remove();
    this.mapHost.className = 'trkMapHost';
    this.lastRows = ''; this.sel = ''; this.pending = null; this.selPaused = false;
  }
  destroy() { this.hide(); this.root.remove(); }

  private async boot(my: number) {
    const w = window as any;
    const { TRACK_LAYER_JS } = await import('./generated/trackLayerSource');
    if (my !== this.gen) return;
    if (!w.VibeTrackLayer) runSource(TRACK_LAYER_JS);
    if (!w.VibeAutoFit) runSource(MAP_AUTOFIT_JS);
    if (this.o.aircraft && !w.VibeAdsbLayer) {
      const { ADSB_LAYER_JS } = await import('./generated/adsbLayerSource');
      if (my !== this.gen) return;
      if (!w.VibeAdsbLayer) runSource(ADSB_LAYER_JS);
    }
    const holder = document.createElement('div');
    holder.style.cssText = 'position:absolute;inset:0';
    this.mapHost.insertBefore(holder, this.mapHost.firstChild);
    const rx = this.rx;
    const kit = await probeMapGL();
    if (my !== this.gen) return;
    if (kit && await loadMapGLScripts(kit.base)) {
      if (my !== this.gen) return;
      try {
        const vm = w.VibeMapGL.createNow(holder, { style: kit.style, load: mapglLoad, base: kit.base, rangeBase: kit.base, detail: kit.detail,
                                                   ...(this.o.profile ? { profile: this.o.profile } : {}), scale: this.scaleKind(),
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
    if (w.VibeMap) this.vmL = w.VibeMap.attach(this.lmap, { dataBase: dataUrl('/mapdata/v1/'), ...(this.o.profile ? { profile: this.o.profile } : {}), scale: this.scaleKind() });
    this.attach(this.lmap, L);
  }
  /** The scale bar's units: NM beside km on the sea and air maps. */
  private scaleKind(): 'nautical' | 'metric' {
    const n = this.o.nautical ?? (this.o.profile === 'marine' || this.o.profile === 'aero' || this.o.profile === 'airsea');
    return n ? 'nautical' : 'metric';
  }
  private attach(map: any, L: any) {
    const w = window as any;
    // ★ A handle for drivers (scripts/test-web-navtex-map.mjs — the close-up coast check), like window.__vibeWf: the map of
    //   the track view last opened. Read-only use; nothing in the page reads it.
    w.__vibeTrkMap = map;
    this.rtMap = map; this.rtL = L;   // ★★ beta96 showRoute draws on this map, with this map's Leaflet (or the GPU adapter)
    this.layer = w.VibeTrackLayer.create(map, L, {
      receiver: this.rx, icon: this.o.icon, label: this.o.label, card: this.o.card, trail: this.o.trail,
      labelZoom: this.o.labelZoom, focusZoom: this.o.focusZoom, heard: this.o.heard, labelRank: this.o.labelRank, arc: this.o.arc,
      shape: this.o.shape, cluster: this.o.cluster, clusterZoom: this.o.clusterZoom, clusterAlways: this.o.clusterAlways, yieldGroups: this.o.yieldGroups, aria: this.o.aria, onCard: this.o.onCard,
      onSelect: (k: string | null) => this.markSel(k || ''),
      // ★★ beta96 THE CLICK ORDER (receiver → points → lines → areas): a press on an AREA's marker (a sea area's name, a
      //   NAVTEX area warning, a forecast arrow) goes to an aircraft under it first — Stuart's community-map bug
      yieldClick: (it: any, ev: any) => {
        if (!this.acLayer || !isAreaItem(it)) return false;
        const t = ev?.changedTouches?.[0] ?? ev;
        const took = !!this.acLayer.pickAt?.(t?.clientX, t?.clientY, !!ev?.changedTouches || ev?.pointerType === 'touch');
        if (took) this.layer?.select?.(null);
        return took;
      },
      // ★ beta92 the first framing is the auto fit's (a glide's rules), not an immediate fitBounds — unless it is absent
      firstFit: (this.o.aircraft && !this.o.mixed) || !w.VibeAutoFit ? undefined : false,   // ★ beta96 a mixed map is the auto fit's
    });
    this.acMap = map;
    // ★ beta96 a MIXED map (AIR & SEA) makes the ADS-B layer only when it first has aircraft — a shipping-only view has no
    //   dead AUTO / RINGS / RANGE keys and no rings over the sea (feed → makeAcLayer)
    if (this.o.aircraft && !this.o.mixed) this.makeAcLayer();
    /* ★★★ beta92 THE AUTO FIT — every item heard and the receiver, clear of the list's corner; the FIT key is its AUTO key.
     *  On the aviation map the ADS-B layer frames the aircraft (its own AUTO / FIT), so the key is hidden there. */
    this.fitBtn.hidden = !!this.acLayer;
    // ★ beta96 a map with aircraft AND other layers (the AIR & SEA map) has the shared auto fit too; setCamera says whose
    //   camera it is (the ADS-B layer's while aviation is live, else this one's)
    if ((!this.acLayer || this.o.mixed) && w.VibeAutoFit) {
      const layer = this.layer;
      try {
        this.af = w.VibeAutoFit.attach(map, {
          points: () => {
            if (this.fitGroups.length) return this.fitPoints(this.fitAll);   // ★★★ beta96 the chosen layer, outliers out
            const p = layer.points(); const m = this.morePoints?.(); return m?.length ? [...p, ...m] : p;
          },
          receiver: () => layer.receiver(), button: this.fitBtn,
          what: this.o.listTitle.toLowerCase().replace(/s$/, ''),
          obstacle: () => this.cardsObstacle(),
        });
      } catch (e) { console.error('[tracks] auto fit', e); this.af = null; }
    }
    if (this.o.mixed) this.applyCamera();
    if (this.pending) {
      const p = this.pending; this.pending = null; this.feed(p);
      if (!this.wantFocus) this.autoFitCheck(p);
    }
    this.watchAttrib();
    this.mapSay({ gl: this.vm, lm: this.vm ? null : this.lmap });   // ★ beta96 stage 8 a host layer (NDB) joins this map
    if (this.wantFocus) { const k = this.wantFocus; this.wantFocus = ''; setTimeout(() => this.focus(k), 50); }
  }

  private acMap: any = null;
  /** ★ beta90 the ADS-B layer (aircraft:true) — at attach, or (mixed) when the first aircraft arrives. */
  private makeAcLayer() {
    const w = window as any, map = this.acMap;
    if (this.acLayer || !map || !w.VibeAdsbLayer) return;
    try {
      let prefs: any = null;
      // ★ beta96 ADS-B mode's own map is this one now: its toggles (RANGE, RINGS, AUTO …) carry over until set here
      try { prefs = JSON.parse(localStorage.getItem('vibesdr.avMapPrefs') || localStorage.getItem('vibesdr.adsbMapPrefs') || 'null'); } catch { /* private */ }
      this.acLayer = w.VibeAdsbLayer.create(map, {
        receiver: this.rx, prefs, theme: 'dark', sparseLegend: true, drawReceiver: false,   // ★ the track layer draws the house
        imageBase: this.o.imageBase ?? null,
        ctlHost: this.slot('ctl'),   // ★★★ beta96 its keys in the MAP card (map cards), not floating over the map
        onPrefs: (p: unknown) => { try { localStorage.setItem('vibesdr.avMapPrefs', JSON.stringify(p)); } catch { /* private */ } },
        // ★ an aircraft tapped on the map: its list row is marked (the layer opens its own card)
        onSelect: (h: string | null) => { this.layer?.select?.(null); this.markSel(h ? this.acHex.get(h) || '' : ''); },
      });
      this.placeAcObstacle();
      this.cardsLayout();
      w.__vibeAcLayer = this.acLayer;   // ★ a handle for drivers (scripts/test-web-avmap.mjs), as __vibeTrkMap
    } catch (e) { console.error('[tracks] the aircraft layer could not start', e); this.acLayer = null; }
    if (this.o.mixed) this.applyCamera();
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
      // ★ beta96 the cards always stop above the attribution line (a tall KEY card ran under it on a wide window, Stuart 11-27)
      void wide;
      this.root.style.setProperty('--trk-attrib-h', `${Math.round(h)}px`);
    };
    this.attribObs = new ResizeObserver(sync);
    this.attribObs.observe(at); this.attribObs.observe(this.mapHost);
    sync();
  }

  /** Every report: the whole list (items with key/lat/lon), the rows' HTML, the status line, the count. */
  /** ★ beta90 the items to their layers: aircraft (`_adsb`) to the ADS-B layer, the rest to the track layer. */
  private feed(items: any[]) {
    // ★★★ beta96 FIT TO: every placed item's group (the host's _fg; aircraft default to 'adsb' / 'datalink' by their data)
    this.fitAll = [];
    for (const it of items) {
      const lat = Number(it?.lat), lon = Number(it?.lon);
      if (!Number.isFinite(lat) || !Number.isFinite(lon) || it?._nofit) continue;
      const g = typeof it._fg === 'string' ? it._fg : it._adsb ? (it._adsb.sparse ? 'datalink' : 'adsb') : 'other';
      this.fitAll.push({ lat, lon, g });
    }
    if (!this.acLayer && this.o.mixed && this.acMap && items.some((it) => it && it._adsb)) this.makeAcLayer();
    if (!this.acLayer) { this.layer.update(items); return; }
    const rest: any[] = [], ac: any[] = [];
    this.acKeys.clear(); this.acHex.clear();
    for (const it of items) {
      const a = it && it._adsb;
      if (a && typeof a.hex === 'string') { ac.push(a); this.acKeys.set(it.key, a.hex); this.acHex.set(a.hex, it.key); }
      else rest.push(it);
    }
    this.layer.update(rest);
    try { this.acLayer.update(ac); } catch (e) { console.error('[tracks] aircraft update', e); }
  }
  /** ★ beta96 the open cards' corner of the map (top-left; a phone's card spans the width) — the fits keep clear of it. */
  private cardsObstacle(): { corner: string; w: number; h: number } | null {
    // ★ tweak 3: chips and cards down BOTH edges — the fits keep clear of the bigger group's corner
    const m = this.mapHost.getBoundingClientRect();
    if (m.width <= 0) return null;
    const group = (sels: string) => {
      let l = Infinity, r = -Infinity, t = Infinity, b = -Infinity;
      for (const el of Array.from(this.root.querySelectorAll(sels)) as HTMLElement[]) {
        const x = el.getBoundingClientRect(); if (x.width <= 0 || x.height <= 0 || el.closest('[hidden]')) continue;
        l = Math.min(l, x.left); r = Math.max(r, x.right); t = Math.min(t, x.top); b = Math.max(b, x.bottom);
      }
      return r > l ? { l, r, t, b } : null;
    };
    const L = group('.mChipsL .mDock:not(.tucked) .hCard, .trkCards .cL .trkCard2');
    const R = group('.mChipsR .mDock:not(.tucked) .hCard, .trkCards .cR .trkCard2');
    const aL = L ? (L.r - m.left) * (L.b - m.top) : 0, aR = R ? (m.right - R.l) * (R.b - m.top) : 0;
    if (!aL && !aR) return null;
    return aL >= aR ? { corner: 'top-left', w: L!.r - m.left + 8, h: Math.min(L!.b - m.top + 8, m.height) }
                    : { corner: 'top-right', w: m.right - R!.l + 8, h: Math.min(R!.b - m.top + 8, m.height) };
  }
  /** The list's corner of the map — the ADS-B layer keeps aircraft (and its fit) clear of it. */
  private placeAcObstacle() {
    if (!this.acLayer?.setObstacle) return;
    try { this.acLayer.setObstacle(this.cardsObstacle()); }
    catch (e) { console.error('[tracks] obstacle', e); }
  }
  update(items: any[], rowsHtml: string, status: string, count: number, rx: LatLon | null, safety = '') {
    if (rx && (!this.rx || rx.lat !== this.rx.lat || rx.lon !== this.rx.lon)) {
      this.rx = rx;
      try { this.layer?.setReceiver(rx.lat, rx.lon); this.acLayer?.setReceiver(rx.lat, rx.lon, rx.approx || ''); } catch (e) { console.error('[tracks] rx', e); }
    }
    if (this.layer) {
      try { this.feed(items); } catch (e) { console.error('[tracks] layer.update', e); }
      // ★ a new time range: fit what it shows (the ADS-B layer fits its own aircraft — AUTO / FIT)
      if (this.refit && items.length && !this.acLayer) {
        this.refit = false;
        try { if (this.af) this.af.fitNow(); else this.layer.fit(); } catch (e) { console.error('[tracks] fit', e); }
      }
      this.autoFitCheck(items);
      if (this.acLayer) this.placeAcObstacle();
    }
    else this.pending = items;
    if (rowsHtml !== this.lastRows) {
      const top = this.rowsEl.scrollTop;
      /* ★ beta96 (ADS-B's live list, redrawn every second) the controller's highlight and the keyboard's focus are on an
       *  element about to be replaced: found again by its key */
      const hiKey = (this.rowsEl.querySelector('.trkRow.gpFocus') as HTMLElement | null)?.dataset.key || '';
      const act = document.activeElement as HTMLElement | null;
      const focusKey = act && this.rowsEl.contains(act) ? (act.closest('.trkRow') as HTMLElement | null)?.dataset.key || '' : '';
      this.rowsEl.innerHTML = rowsHtml; this.lastRows = rowsHtml;
      this.rowsEl.scrollTop = top;
      if (this.sel) this.markSel(this.sel);
      const cssEsc = (globalThis as any).CSS?.escape as ((v: string) => string) | undefined;
      const byKey = (k: string) => this.rowsEl.querySelector(`.trkRow[data-key="${cssEsc ? cssEsc(k) : k.replace(/["\\]/g, '')}"]`) as HTMLElement | null;
      if (hiKey) { const r2 = byKey(hiKey); if (r2) padRehighlight(r2); }
      if (focusKey) byKey(focusKey)?.focus({ preventScroll: true });
    }
    if (this.statEl.textContent !== status) this.statEl.textContent = status;
    // ★ beta96 a map whose host does not place its facts (the ISM maps): the status line heads the LIST card
    if (!this.hostFacts) { const f = this.root.querySelector('.trkCard2[data-card="list"] .facts') as HTMLElement; if (f.textContent !== status) f.textContent = status; }
    this.countEl.textContent = String(count);
    for (const c of Array.from(this.root.querySelectorAll('.mDock[data-dock="list"] .cn, .mDock[data-dock="list"] .tCn')) as HTMLElement[]) if (c.textContent !== String(count)) c.textContent = String(count);
    this.safetyEl.hidden = !safety;
    if (safety && this.safetyEl.innerHTML !== safety) this.safetyEl.innerHTML = safety;
  }
}

/** ★ The AIS view's safety strip: the newest safety-related broadcasts (types 12/14) — a SART's "SART ACTIVE" first. */
export function aisSafetyHtml(r: AisReport): string {
  const safety = r.safety.slice(-3).reverse().map((s) => `<div>⚠ <b>${esc(s.name || 'MMSI ' + s.mmsi)}</b>: ${esc(s.text)} <span style="opacity:.7">· ${esc(ago(s.age))} ago</span></div>`);
  // ★ V12 2026-11-16 area notices and text descriptions (binary messages): after the safety texts, newest first, two at most
  const notes = (r.notices || []).slice(-2).reverse().map((n) => `<div>ℹ <b>${esc(n.name || 'MMSI ' + n.mmsi)}</b>: ${esc(n.text)} <span style="opacity:.7">· ${esc(ago(n.age))} ago</span></div>`);
  return [...safety, ...notes].join('');
}
