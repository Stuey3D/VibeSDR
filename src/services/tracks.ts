/**
 * tracks.ts — the app's AIS and APRS (V12, docs/v12/AIS-APRS.md): the pure half of the ship map (AIS mode) and the APRS
 * decoder's box and station map.
 *
 * ★★ ONE RULE, TWO READERS: the web client's web/client/src/tracks.ts is the reference, and every rule here is that one —
 *    the report sanitisers, the icons (our own drawings), the cards, the status lines, the list's order and wording, the
 *    colours. scripts/test_ais_aprs_app.ts runs the same reports through both and holds them equal.
 * ★★ The MAP is the shared layer (web/mapkit/tracklayer.js → src/generated/trackLayerSource.ts, injected into MapOverlay's
 *    page): it knows positions, selection, tracks and motion. What an item LOOKS like is the host's — here, worked out in
 *    React Native and handed to the page with each item (`_ic` icon, `_lb` label, `_cd` card: trackItems), so the page
 *    carries no copy of these rules.
 * ★ Everything in a report came off the air: every string is escaped where it meets HTML (the icons and cards).
 *
 * Wire (vibe_ais_service.h, on the SPECTRUM socket):
 *   → {"type":"ais","on":1|0} · {"type":"ais_trail","mmsi":N}
 *   ← {"type":"ais", rate, now, msgs, msgRate, chA, chB, repaired, other, dropped, capture, noise, gain?, gainAuto?, vessels:[…], safety:[…]}
 *   ← {"type":"ais_off"} · {"type":"ais_error","why"} · {"type":"ais_trail","mmsi","pts":[[lat,lon,ageSec]…]}
 * Wire (vibe_aprs_run.h, on the DECODER socket after `audio_extension_attach` 'aprs' {baud_rate}):
 *   ← {"type":"aprs_list","data":{baud,frames,repaired,dupes,stations:[…],messages:[…]}} (2 s, when changed; at once to a joiner)
 *   ← {"type":"aprs_packet","data":{key,from,kind,line,raw,lat?,lon?,to?,text?}}
 */
import { flagEmoji } from './adsbInfo';
import { cleanText } from '../utils/safeText';

export type LatLon = { lat: number; lon: number };

// ── the report shapes (vibe_ais_service.h / vibe_aprs_run.h) — the web's, field for field ──────────────────────────
export interface AisVessel {
  mmsi: number; kind: string; name?: string; call?: string; dest?: string; imo?: number; flag?: string; country?: string;
  stype?: number; stypeName?: string; family?: string; nav?: number; navName?: string; aton?: number; atonName?: string;
  virtual?: number; offpos?: number; lat?: number; lon?: number; sog?: number; cog?: number; hdg?: number; rot?: number; turn?: number;
  draught?: number; len?: number; beam?: number; eta?: string; alt?: number; trend?: number; line?: string;
  ch?: string; level?: number; snr?: number; seen: number; age: number; posAge: number; count: number; trail?: number;
}
export interface AisSafety { age: number; mmsi: number; name: string; text: string; to?: number }
export interface AisReport {
  type: 'ais'; rate: number; now: number; msgs: number; msgRate: number; chA: number; chB: number; repaired: number;
  dropped: number; capture: number; noise: number; vessels: AisVessel[]; safety: AisSafety[];
  gain?: number; gainAuto?: number;
}
export interface AprsStation {
  key: string; from: string; kind: string; name?: string; sym: string; symName?: string; lat?: number; lon?: number;
  course?: number; speed?: number; alt?: number; comment?: string; status?: string; micE?: string; path?: string; via?: string;
  raw?: string; line?: string; wx?: Record<string, number>; killed?: number; msg?: number; seen: number; age: number; count: number;
  posAge: number; trail?: [number, number][];
}
export interface AprsMessage { age: number; from: string; to: string; text: string; kind: string; id?: string }
export interface AprsList {
  baud: number; frames: number; repaired: number; dupes: number; stations: AprsStation[]; messages: AprsMessage[];
}
export interface AprsPacket { key: string; from: string; kind: string; line: string; raw: string; lat?: number; lon?: number; to?: string; text?: string }

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
/** An aprs_packet's data — the web DecoderClient's checks (key required; strings cut short; a position only as numbers). */
export function aprsPacketSanitise(d: any): AprsPacket | null {
  if (!d || typeof d !== 'object') return null;
  const s = (v: unknown, n: number) => (typeof v === 'string' ? v.slice(0, n) : '');
  const n = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
  if (!s(d.key, 40)) return null;
  return { key: s(d.key, 40), from: s(d.from, 16), kind: s(d.kind, 14), line: s(d.line, 300), raw: s(d.raw, 400),
           lat: n(d.lat), lon: n(d.lon), to: s(d.to, 12) || undefined, text: s(d.text, 200) || undefined };
}
/** An ais_trail's points ([lat, lon] — the age is dropped), at most 400; null for anything else. */
export function aisTrailSanitise(m: any): { mmsi: number; pts: [number, number][] } | null {
  const mmsi = num(m?.mmsi);
  if (mmsi == null || mmsi <= 0 || !Array.isArray(m?.pts)) return null;
  const pts = m.pts.slice(-400).filter((p: any) => Array.isArray(p) && num(p[0]) != null && num(p[1]) != null
    && Math.abs(Number(p[0])) <= 90 && Math.abs(Number(p[1])) <= 180).map((p: any) => [Number(p[0]), Number(p[1])] as [number, number]);
  return { mmsi, pts };
}

// ── the capability (the RADIO's /vibeserver.json) ─────────────────────────────────────────────────────────────────
/** ★ AIS: `ais` true = live, false + `aisWhy` = greyed with the reason, absent = this radio cannot reach 162 MHz (an HF+,
 *  an Icom, an HF-only radio) — no control at all (AGENTS.md). The web's aisReadCap. */
export interface AisCap { ok: boolean; why: string }
export function parseAisCapability(j: unknown): AisCap | undefined {
  if (!j || typeof j !== 'object') return undefined;
  const o = j as Record<string, unknown>;
  if (o.ais === true) return { ok: true, why: '' };
  if (o.ais === false) {
    const why = typeof o.aisWhy === 'string' ? cleanText(o.aisWhy, 200) : '';
    return { ok: false, why: why || 'AIS is not available on this receiver' };
  }
  return undefined;
}
/** ★ APRS: a listener's decoder, offered only where the server says `"aprs": true` (absent on an older server; false where
 *  the owner switched it off). `aprsHz` = the region's VHF channel from the server's position (144.800 when unsaid). */
export function parseAprsCapability(j: unknown): { on: boolean; hz: number } {
  const o = (j && typeof j === 'object' ? j : {}) as Record<string, unknown>;
  const hz = typeof o.aprsHz === 'number' && Number.isFinite(o.aprsHz) && o.aprsHz > 30e6 && o.aprsHz < 1e9 ? o.aprsHz : 144.8e6;
  return { on: o.aprs === true, hz };
}

// ── the APRS decoder's settings: VHF 1200 Bd (NFM on the region's channel) or HF 300 Bd (USB) ─────────────────────
export type AprsBaud = 1200 | 300;
export const APRS_HF_HZ = 10147600;   // ★ 30 m HF packet (USB dial); 14.105 on 20 m is in the hint
export function aprsBaudOf(v: unknown): AprsBaud { return Number(v) === 300 ? 300 : 1200; }
/** The attach parameters — the web's decParams('aprs'). */
export function aprsParams(baud: AprsBaud): { baud_rate: number } { return { baud_rate: baud }; }
/** The TUNE key: the region's channel in NFM, or 10.1476 MHz USB. */
export function aprsTuneFor(baud: AprsBaud, aprsHz: number): { hz: number; mode: 'nfm' | 'usb'; label: string } {
  return baud === 300 ? { hz: APRS_HF_HZ, mode: 'usb', label: 'TUNE 10.1476 USB' }
                      : { hz: Math.round(aprsHz), mode: 'nfm', label: `TUNE ${(aprsHz / 1e6).toFixed(3)} NFM` };
}
/** The hint under the settings — the web's aprsHint, as plain text. */
export function aprsHintText(baud: AprsBaud, aprsHz: number): string {
  return baud === 300
    ? 'HF packet: 300 Bd on 10.1476 MHz USB (30 m; 14.105 on 20 m). The decoder tries a few tunings either side, so close is close enough.'
    : `VHF APRS: ${(aprsHz / 1e6).toFixed(3)} MHz NFM here. Every station heard is listed, with its position, weather, messages and the digipeater that carried it. MAP in the box shows them on the map.`;
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
/** ★ The app always draws the flag as its emoji (the web shows Windows the ISO code — Windows has no flag glyphs). */
function flagHtml(iso: string | undefined): string {
  if (!iso || !/^[A-Z]{2}$/.test(iso)) return '';
  return `<span class="flag">${flagEmoji(iso)}</span> `;
}
function fmtPos(lat?: number, lon?: number): string {
  if (lat == null || lon == null) return '';
  const f = (v: number, p: string, n: string, w: number) => {
    const a = Math.abs(v), d = Math.floor(a), m = (a - d) * 60;
    return `${String(d).padStart(w, '0')}°${m.toFixed(3).padStart(6, '0')}′${v >= 0 ? p : n}`;
  };
  return `${f(lat, 'N', 'S', 2)} ${f(lon, 'E', 'W', 3)}`;
}
/** "4.2 NM" / "12 NM" — a ship's range (the list and the card). */
const nmText = (nm: number) => `${nm < 10 ? nm.toFixed(1) : Math.round(nm)} NM`;
const kmText = (km: number) => `${km < 10 ? km.toFixed(1) : Math.round(km)} km`;

// ── AIS: icons, rows, cards ───────────────────────────────────────────────────────────────────────────────────────
/** ★ The colour of a ship by what it is (shipFamily on the server) — the conventions chart plotters share. */
export const AIS_COLOURS: Record<string, string> = {
  cargo: '#6dd36d', tanker: '#ff6b5e', passenger: '#4fa3ff', fishing: '#ffad42', tug: '#39d0d8', special: '#ffe14d',
  leisure: '#c58cff', hsc: '#f7f06a', other: '#b8c2cc',
};
export function aisColour(v: AisVessel): string {
  return v.kind === 'sart' ? '#ff3b3b' : AIS_COLOURS[v.family || 'other'] || AIS_COLOURS.other;
}
function aisMoving(v: AisVessel) { return v.sog != null && v.sog >= 0.5 && v.nav !== 1 && v.nav !== 5 && v.nav !== 6; }
export function aisIcon(v: AisVessel): { html: string; w: number; h: number } {
  const col = aisColour(v);
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
export function aisKindWord(v: AisVessel): string {
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
    if (rx) { const rb = rangeBearing(rx, v.lat, v.lon); add('From here', `${nmText(rb.nm)} ${compass8(rb.brg)}`); }
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
export const AIS_EMPTY = 'No ships heard yet. Each one appears as its transponder is decoded — a class A ship reports every few seconds under way, every three minutes at anchor.';

/** One row of the live list, for React Native to draw (the web's aisRows, field by field). */
export interface TrackRow {
  key: string; colour: string; flag: string; name: string; second: string; sp: string; trend: -1 | 0 | 1;
  /** "4.2 NM" / "12 km", or '' (a position, no receiver), or 'no position' (dim). */
  range: string; brg: string; noPos: boolean; age: string; em: boolean;
}
/** ★ NEAREST FIRST (no position last, then the newest) — the web's aisRows order. */
export function aisListRows(r: AisReport | null, rx: LatLon | null): TrackRow[] {
  if (!r) return [];
  const rows = r.vessels.map((v) => ({ v, rb: rx && v.lat != null && v.lon != null ? rangeBearing(rx, v.lat, v.lon) : null }));
  rows.sort((a, b) => (a.rb && b.rb ? a.rb.km - b.rb.km : a.rb ? -1 : b.rb ? 1 : a.v.seen - b.v.seen));
  return rows.map(({ v, rb }) => ({
    key: String(v.mmsi), colour: aisColour(v), flag: v.flag && /^[A-Z]{2}$/.test(v.flag) ? flagEmoji(v.flag) : '', name: aisName(v),
    second: [aisKindWord(v), v.navName && v.nav !== 0 && v.nav !== 15 ? v.navName : '', v.dest ? '→ ' + v.dest : ''].filter(Boolean).join(' · ')
      || `MMSI ${v.mmsi}`,
    sp: v.sog != null && v.sog >= 0.1 ? `${v.sog.toFixed(v.sog < 10 ? 1 : 0)} kn${v.cog != null && v.sog >= 0.5 ? ' ' + compass8(v.cog) : ''}` : v.lat != null ? 'still' : '',
    trend: v.trend === 1 ? 1 : v.trend === -1 ? -1 : 0,
    range: rb ? nmText(rb.nm) : v.lat != null ? '' : 'no position', brg: rb ? compass8(rb.brg) : '',
    noPos: v.lat == null, age: '', em: v.kind === 'sart',
  }));
}
/** ★ The safety strip: the newest safety-related broadcasts (types 12/14), at most three, newest first. */
export function aisSafetyLines(r: AisReport | null): string[] {
  if (!r || !r.safety.length) return [];
  return r.safety.slice(-3).reverse().map((s) => `⚠ ${s.name || 'MMSI ' + s.mmsi}: ${s.text} · ${ago(s.age)} ago`);
}

// ── APRS: icons, rows, cards ──────────────────────────────────────────────────────────────────────────────────────
/** ★ OUR OWN SYMBOL SET (drawn here — no third-party artwork): the APRS 1.0.1 symbol CODE picks a glyph, the table and
 *  any overlay character are shown as the spec says. A code we have no glyph for is the symbol character itself on the
 *  badge, so nothing is ever blank. (The web's APRS_GLYPH, the same drawings.) */
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
export function aprsColour(s: { kind: string; sym: string; speed?: number }): string {
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
    if (rx) { const rb = rangeBearing(rx, s.lat, s.lon); add('From here', `${kmText(rb.km)} ${compass8(rb.brg)}`); }
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
export const APRS_EMPTY = 'No stations heard yet. Each appears with its first packet — most beacon every 10 to 30 minutes.';
/** The map's live list — the web's aprsRows: the server's order (newest first). */
export function aprsListRows(l: AprsList | null, rx: LatLon | null): TrackRow[] {
  if (!l) return [];
  return l.stations.map((s) => {
    const rb = rx && s.lat != null && s.lon != null ? rangeBearing(rx, s.lat, s.lon) : null;
    return {
      key: s.key, colour: aprsColour(s), flag: '', name: aprsName(s),
      second: [s.symName, s.comment || s.status].filter(Boolean).join(' · ') || s.kind,
      sp: s.speed != null && s.speed >= 2 ? `${Math.round(s.speed)} km/h` : s.wx?.tempC != null ? `${s.wx.tempC.toFixed(1)} °C` : '',
      trend: 0, range: rb ? kmText(rb.km) : s.lat != null ? '' : 'no position', brg: rb ? compass8(rb.brg) : '',
      noPos: s.lat == null, age: ago(s.seen), em: false,
    };
  });
}

// ── the APRS decoder box (the web's renderAprs) ───────────────────────────────────────────────────────────────────
export interface AprsBoxRow { key: string; colour: string; call: string; line: string; right: string; noPos: boolean; raw: string }
/** "12s" · "4m" · "2h" — the box's short age (the web's aprsAgo). */
export function aprsAgo(s: number): string { return s < 60 ? `${Math.round(s)}s` : s < 3600 ? `${Math.round(s / 60)}m` : `${Math.round(s / 3600)}h`; }
/** ★ The box's station rows, newest first (the server's order), at most 80: the call, the readable line without its
 *  leading call, the distance (flat-earth km, as the web's box) and the age. A tap opens the map on the station. */
export function aprsBoxRows(l: AprsList | null, rx: LatLon | null): AprsBoxRow[] {
  if (!l) return [];
  return l.stations.slice(0, 80).map((st) => {
    let rb = '';
    if (rx && st.lat != null && st.lon != null) {
      const d = Math.hypot((st.lat - rx.lat) * 111.2, (st.lon - rx.lon) * 111.2 * Math.cos(rx.lat * Math.PI / 180));
      rb = d < 10 ? d.toFixed(1) : String(Math.round(d));
    }
    const line = (st.line || '').replace(/^[^·]*·\s*/, '');
    return { key: st.key, colour: aprsColour(st), call: st.name || st.from, line: line || st.kind,
             right: `${rb ? rb + ' km · ' : ''}${aprsAgo(st.seen)}`, noPos: st.lat == null, raw: st.raw || '' };
  });
}
/** The messages heard (newest first, at most 20) — "G4ABC → M0XYZ: text" (an ack carries its id). */
export function aprsMessageLines(l: AprsList | null): string[] {
  return (l?.messages ?? []).slice(-20).reverse().map((m) => `${m.from} → ${m.to}: ${m.kind === 'ack' ? m.text + ' ' + (m.id || '') : m.text}`);
}
/** ★ The packets as they arrive: newest first, at most 60 kept — a new array. */
export function aprsLogPush(log: readonly { t: number; line: string }[], p: AprsPacket, now: number): { t: number; line: string }[] {
  const out = [{ t: now, line: p.line || p.raw }, ...log];
  if (out.length > 60) out.length = 60;
  return out;
}
/** "14:32:07" — a packet's local time in the box (the web's). */
export function aprsLogTime(t: number): string {
  const d = new Date(t);
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}:${String(d.getSeconds()).padStart(2, '0')}`;
}
/** ★ A packet's position between lists: the station moved on the map at once and its track grown (the list carries the
 *  whole track only to a joiner). Mutates `trails` (key → points, ≤ 200) and the station in `l`; returns whether a
 *  station in the list moved. The web's onAprsPacket. */
export function aprsApplyPacket(l: AprsList | null, trails: Map<string, [number, number][]>, p: AprsPacket): boolean {
  if (p.lat == null || p.lon == null) return false;
  const tr = trails.get(p.key) ?? [];
  const last = tr[tr.length - 1];
  if (!last || last[0] !== p.lat || last[1] !== p.lon) { tr.push([p.lat, p.lon]); if (tr.length > 200) tr.shift(); trails.set(p.key, tr); }
  const st = l?.stations.find((x) => x.key === p.key);
  if (st) { st.lat = p.lat; st.lon = p.lon; st.seen = 0; return true; }
  return false;
}

// ── what the map page is handed ───────────────────────────────────────────────────────────────────────────────────
/** ★ An item for the shared layer: its key, position and — worked out HERE, so the page carries no copy of these rules —
 *  its icon (`_ic`), label (`_lb`) and card (`_cd`). Items without a position are not on the map; an APRS object its
 *  owner removed is not either (the web's). */
export interface TrackItem { key: string; lat: number; lon: number; _ic: { html: string; w: number; h: number }; _lb: string; _cd: string; /** ★ a count that grows when heard again — the layer pulses it */ _h?: number }
export function aisItems(r: AisReport | null, rx: LatLon | null): TrackItem[] {
  if (!r) return [];
  return r.vessels.filter((v) => v.lat != null && v.lon != null)
    .map((v) => ({ key: String(v.mmsi), lat: v.lat as number, lon: v.lon as number, _ic: aisIcon(v), _lb: aisName(v), _cd: aisCard(v, rx) }));
}
export function aprsItems(l: AprsList | null, rx: LatLon | null): TrackItem[] {
  if (!l) return [];
  return l.stations.filter((s) => s.lat != null && s.lon != null && !s.killed)
    .map((s) => ({ key: s.key, lat: s.lat as number, lon: s.lon as number, _ic: aprsIcon(s), _lb: aprsName(s), _cd: aprsCard(s, rx) }));
}

/** ★ The card's look inside the map page — the web's .trkCard rules (tracks.ts CSS), so a card reads the same on both. */
export const TRACK_CARD_CSS = '.trkCard .t{font-size:13px;margin:0 18px 6px 0;color:#fff}'
  + '.trkCard .r{display:grid;grid-template-columns:7.2em minmax(0,1fr);gap:8px;padding:1px 0}'
  + '.trkCard .k{color:#8596a7}.trkCard .v{color:#e3eaf1;word-break:break-word}'
  + '.trkCard .raw{margin-top:6px;padding:5px 6px;background:#0d151c;border-radius:4px;color:#9fb0c2;font-size:11px;word-break:break-all}'
  + '.trkCard i.up{color:#6dd36d;font-style:normal}.trkCard i.dn{color:#ff8a6b;font-style:normal}.trkCard .flag{font-size:14px}'
  + '@media (max-width:520px){.vtl-card{top:6px !important;right:6px !important;left:52px;max-width:none !important;max-height:30% !important;font-size:11px !important}}';
