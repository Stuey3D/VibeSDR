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
import { aprsColour, aprsIcon as aprsSymIcon, aprsAltPlausible } from '../utils/aprsSymbols';
import { aisDxSanitise, aisDxState, aisDxWords, AIS_DX_COLOUR, AIS_DX_SHORT } from './aisDx';

export type LatLon = { lat: number; lon: number };

// ── the report shapes (vibe_ais_service.h / vibe_aprs_run.h) — the web's, field for field ──────────────────────────
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
  /** ★★★ beta96 (mapScope.ts) the ship store's: the radios on this server that heard it (absent = unknown / the live report) */
  radios?: string[];
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
export interface AisSafety { age: number; mmsi: number; name: string; text: string; to?: number }
export interface AisReport {
  type: 'ais'; rate: number; now: number; msgs: number; msgRate: number; chA: number; chB: number; repaired: number;
  dropped: number; capture: number; noise: number; vessels: AisVessel[]; safety: AisSafety[];
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
export interface AisCap { ok: boolean; why: string; /** ★★★ beta96 MARITIME (AIS + DSC ch 70) offered — the capture holds both */ dsc?: boolean }
export function parseAisCapability(j: unknown): AisCap | undefined {
  if (!j || typeof j !== 'object') return undefined;
  const o = j as Record<string, unknown>;
  if (o.ais === true) return { ok: true, why: '', ...(o.aisDsc === true ? { dsc: true } : {}) };
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
    ? 'HF packet: 300 Bd on 10.1476 MHz USB (30 m; 14.105 on 20 m). The decoder tries a few tunings either side, so close is close enough. The speaker is muted while APRS listens.'
    : `VHF APRS: ${(aprsHz / 1e6).toFixed(3)} MHz NFM here. Every station heard is listed, with its position, weather, messages and the digipeater that carried it. MAP in the box shows them on the map. The speaker is muted while APRS listens.`;
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
  const ic = aisIconBase(v);
  // ★ beta95 a DX ship stands out (Stuart: "they are the exciting ones"): a gold halo round whatever it is
  return v.dx ? { ...ic, html: `<span style="display:inline-block;filter:drop-shadow(0 0 2px ${AIS_DX_COLOUR}) drop-shadow(0 0 3px ${AIS_DX_COLOUR})">${ic.html}</span>` } : ic;
}
function aisIconBase(v: AisVessel): { html: string; w: number; h: number } {
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
/** ★ beta95 the map's label: the name, and DX when it is one. */
export function aisLabel(v: AisVessel): string { return v.dx ? `${aisName(v)} · DX` : aisName(v); }
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
  } else if (v.unconf && v.uLat != null && v.uLon != null) {
    const ub = rx ? rangeBearing(rx, v.uLat, v.uLon) : null;
    add('Claims', esc(fmtPos(v.uLat, v.uLon)) + (ub ? ` · ${nmText(ub.nm)} ${compass8(ub.brg)}` : ''));
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
export const AIS_EMPTY = 'No ships heard yet. Each one appears as its transponder is decoded — a class A ship reports every few seconds under way, every three minutes at anchor.';

/** One row of the live list, for React Native to draw (the web's aisRows, field by field). */
export interface TrackRow {
  key: string; colour: string; flag: string; name: string; second: string; sp: string; trend: -1 | 0 | 1;
  /** "4.2 NM" / "12 km", or '' (a position, no receiver), or 'no position' (dim). */
  range: string; brg: string; noPos: boolean; age: string; em: boolean;
  /** ★ beta95 a DX ship (aisDx.ts): the list tags it in gold */
  dx?: boolean;
  /** ★ beta96 the message's SNR (dB, its decoder's own measure) and which scale colours it (src/utils/snrColour.ts) —
   *  the row's colour-coded chip; absent = no chip (an older server, a source that does not measure). */
  snr?: number; snrMode?: string;
}
/** ★ NEAREST FIRST (no position last, then the newest) — the web's aisRows order. */
export function aisListRows(r: AisReport | null, rx: LatLon | null): TrackRow[] {
  if (!r) return [];
  const rows = r.vessels.map((v) => ({ v, rb: rx && v.lat != null && v.lon != null ? rangeBearing(rx, v.lat, v.lon) : null }));
  rows.sort((a, b) => (a.rb && b.rb ? a.rb.km - b.rb.km : a.rb ? -1 : b.rb ? 1 : a.v.seen - b.v.seen));
  return rows.map(({ v, rb }) => { const dxs = aisDxState(v); const ub = dxs === 'unconf' && rx && v.uLat != null && v.uLon != null ? rangeBearing(rx, v.uLat, v.uLon) : null; return {
    key: String(v.mmsi), colour: aisColour(v), flag: v.flag && /^[A-Z]{2}$/.test(v.flag) ? flagEmoji(v.flag) : '', name: aisName(v),
    second: [aisKindWord(v), dxs === 'unconf' ? 'unconfirmed position' : dxs === 'implausible' ? 'position implausible' : '',
      v.navName && v.nav !== 0 && v.nav !== 15 ? v.navName : '', v.dest ? '→ ' + v.dest : '', aisMetShort(v.met?.[0])].filter(Boolean).join(' · ')
      || `MMSI ${v.mmsi}`,
    // ★★ beta95 DX (aisDx.ts): a DX ship's row is tagged; an unconfirmed one says "DX?" and where it claims; a fake "implausible"
    ...(dxs === 'dx' ? { dx: true } : {}),
    sp: v.sog != null && v.sog >= 0.1 ? `${v.sog.toFixed(v.sog < 10 ? 1 : 0)} kn${v.cog != null && v.sog >= 0.5 ? ' ' + compass8(v.cog) : ''}` : v.lat != null ? 'still' : '',
    trend: v.trend === 1 ? 1 : v.trend === -1 ? -1 : 0,
    range: rb ? nmText(rb.nm) : v.lat != null ? '' : dxs === 'unconf' ? `${AIS_DX_SHORT.unconf}${ub ? ' ' + nmText(ub.nm) : ''}`
      : dxs === 'implausible' ? AIS_DX_SHORT.implausible : 'no position',
    brg: rb ? compass8(rb.brg) : ub ? compass8(ub.brg) : '',
    noPos: v.lat == null, age: '', em: v.kind === 'sart',
    ...(v.snr != null && (r.snrv ?? 0) >= 2 ? { snr: v.snr, snrMode: 'ais' } : {}),   // ★ beta96 the ship's last frame's SNR
  }; });
}
/** ★ The safety strip: the newest safety-related broadcasts (types 12/14), at most three, newest first. */
export function aisSafetyLines(r: AisReport | null): string[] {
  if (!r) return [];
  const safety = r.safety.slice(-3).reverse().map((s) => `⚠ ${s.name || 'MMSI ' + s.mmsi}: ${s.text} · ${ago(s.age)} ago`);
  // ★ V12 2026-11-16 area notices and text descriptions (binary messages): after the safety texts, two at most
  const notes = (r.notices || []).slice(-2).reverse().map((n) => `ℹ ${n.name || 'MMSI ' + n.mmsi}: ${n.text} · ${ago(n.age)} ago`);
  return [...safety, ...notes];
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
      ...(s.snr != null ? { snr: s.snr, snrMode: 'aprs' } : {}),   // ★ beta96 its last frame's SNR (IQ only)
    };
  });
}

// ── the APRS decoder box (the web's renderAprs) ───────────────────────────────────────────────────────────────────
export interface AprsBoxRow { key: string; colour: string; call: string; line: string; right: string; noPos: boolean; raw: string;
  /** ★ beta96 the station's last frame's SNR (the box's chip) */
  snr?: number }
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
             right: `${rb ? rb + ' km · ' : ''}${aprsAgo(st.seen)}`, noPos: st.lat == null, raw: st.raw || '', ...(st.snr != null ? { snr: st.snr } : {}) };
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
export interface TrackItem { key: string; lat: number; lon: number; _ic: { html: string; w: number; h: number }; _lb: string; _cd: string;
  /** ★ V12 the NAVTEX map: a shape under it, its cluster group, its screen-reader words, the gallery file its card shows */
  _sh?: unknown; _cl?: string; _aria?: string; _f?: string; /** ★ a count that grows when heard again — the layer pulses it */ _h?: number; /** ★ label rank (declutter) */ _rk?: number; /** ★ a great-circle line from the receiver */ _arc?: boolean;
  /** ★ V12 beta90 an aircraft the ADS-B layer draws (aviationMap.ts avPlaneAdsb) — the aviation map's planes */ _adsb?: unknown }
export function aisItems(r: AisReport | null, rx: LatLon | null): TrackItem[] {
  if (!r) return [];
  return r.vessels.filter((v) => v.lat != null && v.lon != null)
    .map((v) => ({ key: String(v.mmsi), lat: v.lat as number, lon: v.lon as number, _ic: aisIcon(v), _lb: aisLabel(v), _cd: aisCard(v, rx) }));
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
  // ★ V12 the NAVTEX cards' TRANSLATE (the gallery's key) — tracks.ts's web rule
  + '.trkCard .nvTr{background:#1d2a36;color:#e3eaf1;border:1px solid #3c5266;border-radius:5px;font:600 11px ui-monospace,Menlo,monospace;padding:4px 9px}'
  + '.trkCard .nvTr.on{background:#ffd34d;color:#111;border-color:#ffd34d}.trkCard .nvTr[hidden]{display:none}'
  + '@media (max-width:520px){.vtl-card{top:6px !important;right:6px !important;left:52px;max-width:none !important;max-height:30% !important;font-size:11px !important}}';
