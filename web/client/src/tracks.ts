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
import { flagEmoji } from '../../../src/services/adsbInfo';
import { snrChipHtml } from '../../../src/utils/snrColour';   // ★ beta96 the colour-coded SNR per message
import { aprsColour, aprsIcon as aprsSymIcon, aprsAltPlausible } from '../../../src/utils/aprsSymbols';
import { aisDxSanitise, aisDxState, aisDxWords, AIS_DX_COLOUR, AIS_DX_SHORT } from '../../../src/services/aisDx';
import { esc, ago, type LatLon } from './mapChrome';
// ★ the map card system moved to mapChrome.ts (11-27); this file keeps what a ship or a station LOOKS like, and re-exports
//   the chrome so its importers are unchanged
export * from './mapChrome';

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

/** ★ The AIS view's safety strip: the newest safety-related broadcasts (types 12/14) — a SART's "SART ACTIVE" first. */
export function aisSafetyHtml(r: AisReport): string {
  const safety = r.safety.slice(-3).reverse().map((s) => `<div>⚠ <b>${esc(s.name || 'MMSI ' + s.mmsi)}</b>: ${esc(s.text)} <span style="opacity:.7">· ${esc(ago(s.age))} ago</span></div>`);
  // ★ V12 2026-11-16 area notices and text descriptions (binary messages): after the safety texts, newest first, two at most
  const notes = (r.notices || []).slice(-2).reverse().map((n) => `<div>ℹ <b>${esc(n.name || 'MMSI ' + n.mmsi)}</b>: ${esc(n.text)} <span style="opacity:.7">· ${esc(ago(n.age))} ago</span></div>`);
  return [...safety, ...notes].join('');
}

