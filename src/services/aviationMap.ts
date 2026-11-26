// aviationMap.ts — V12 2026-11-16 THE AVIATION MAP: ADS-B, ACARS, VDL2, HFDL and ADS-C on ONE map, each aircraft once with
// the sources that heard it, the HFDL ground stations, and the airports. ONE RULE, TWO READERS: the web client
// (web/client/src/main.ts) and the app (SDRScreen → MapOverlay). docs/v12/AVIATION.md §The aviation map.
//
// ★★★ Stuart (2026-11-16): share the aircraft table across radios — "a server with 2 SDRs, one on ADS-B, one on ACARS/VDL2
//     must not show duplicate aircraft" — and ONE combined aviation map, modelled exactly on the shipping map
//     (src/services/shippingMap.ts): every source feeds it, each aircraft shows which sources heard it, airports by default.
//  · The data: GET /vibeserver/aircraft.json (vibe_aircraft_share.h) — every radio on the server writes it — and, while this
//    listener's radio is in the DATALINK mode, its live report (newest; the table adds the other radios' sources).
//  · The layers are the SOURCES (ADS-B · ACARS · VDL2 · HFDL · ADS-C): an aircraft is drawn while ANY source that heard it is
//    switched on. Then HFDL STATIONS (the ground stations heard) and AIRPORTS (the basemap's airports and runways, ON by
//    default — the 'aero' profile; off = 'aero-plain').
//  · ★ NO DEAD CONTROLS (AGENTS.md): a source with no aircraft here is ABSENT — no toggle. The source the map was opened for
//    (the DATALINK mode → ACARS + VDL2, an HFDL MAP → HFDL, ADS-B's AVIATION MAP → ADS-B) is always there, and first.
//    AIRPORTS is always there (the basemap always has them). One layer alone draws no toggle row.
//  · ⚖ Flight data only (address, registration, flight, position, altitude, airports, the label's name) — never a message
//    body: the table never holds one (the owner's opt-in body stays on the DATALINK list, on the owner's own socket).
import { esc, rangeBearing, compass8, ago, heardAgo, type LatLon, type TrackItem, type TrackRow } from './tracks';
import { avFix, avRoute, type AvAircraft, type AvReport } from './aviation';
import { cleanAdsbInfo, flagEmoji, routeShort, ADSB_DATA_CREDIT, adsbCreditHtml, type AdsbInfo, type AdsbRoute } from './adsbInfo';

export const AVIATION_TITLE = 'AVIATION MAP';

// ── the layers ─────────────────────────────────────────────────────────────────────────────────────────────────────
export type AvSrcId = 'adsb' | 'acars' | 'vdl2' | 'hfdl' | 'adsc';
export type AvLayerId = AvSrcId | 'stations' | 'airports';
export type AvLayers = Record<AvLayerId, boolean>;
export type AvFocus = 'datalink' | 'hfdl' | 'adsb';
/** The server's source names (vibe_aircraft_share.h srcName) → the layer. */
export const AV_SRC: Array<{ id: AvSrcId; name: string; colour: string }> = [
  { id: 'adsb', name: 'ADS-B', colour: '#ffd34d' },
  { id: 'vdl2', name: 'VDL2', colour: '#7ee787' },
  { id: 'acars', name: 'ACARS', colour: '#79c0ff' },
  { id: 'hfdl', name: 'HFDL', colour: '#ffa657' },
  { id: 'adsc', name: 'ADS-C', colour: '#d2a8ff' },
];
const SRC_BY_NAME = new Map(AV_SRC.map((s) => [s.name, s]));
export const AV_LAYERS: Array<{ id: AvLayerId; label: string; words: string }> = [
  { id: 'adsb', label: 'ADS-B', words: 'Show the aircraft heard on ADS-B (1090 MHz)' },
  { id: 'vdl2', label: 'VDL2', words: 'Show the aircraft heard on VDL Mode 2 (136 MHz)' },
  { id: 'acars', label: 'ACARS', words: 'Show the aircraft heard on VHF ACARS (131 MHz)' },
  { id: 'hfdl', label: 'HFDL', words: 'Show the aircraft heard on HFDL (HF)' },
  { id: 'adsc', label: 'ADS-C', words: 'Show the aircraft whose position came in an ADS-C report' },
  { id: 'stations', label: 'HFDL STATIONS', words: 'Show the HFDL ground stations heard' },
  { id: 'airports', label: 'AIRPORTS', words: 'Show the airports and runways' },
];
/** Saved toggles — every layer on by default (airports too). */
export function avLayersOf(saved: string | null | undefined): AvLayers {
  const d: AvLayers = { adsb: true, acars: true, vdl2: true, hfdl: true, adsc: true, stations: true, airports: true };
  try { const j = saved ? JSON.parse(saved) : null; if (j && typeof j === 'object') for (const k of Object.keys(d) as AvLayerId[]) if (typeof j[k] === 'boolean') d[k] = j[k]; } catch { /* default */ }
  return d;
}
/** What each layer has (counts of aircraft heard by each source; ground stations heard). */
export interface AvHave { adsb: number; acars: number; vdl2: number; hfdl: number; adsc: number; stations: number }
const FOCUS: Record<AvFocus, AvSrcId[]> = { datalink: ['vdl2', 'acars'], hfdl: ['hfdl'], adsb: ['adsb'] };
/** ★★ LAYERS WITH NO DATA ARE ABSENT: the toggles, in order — the opened source(s) first, AIRPORTS last (always). */
export function avLayersPresent(have: AvHave, focus: AvFocus): AvLayerId[] {
  const out: AvLayerId[] = [];
  const add = (id: AvLayerId) => { if (!out.includes(id)) out.push(id); };
  FOCUS[focus].forEach(add);
  if (focus === 'hfdl') add('stations');   // ★ the HFDL map's own stations beside it
  for (const s of AV_SRC) if (have[s.id] > 0) add(s.id);
  if (have.stations > 0 || focus === 'hfdl') add('stations');
  add('airports');
  return out;
}
export function avToggles(present: AvLayerId[]): Array<{ id: string; label: string; words: string }> {
  if (present.length < 2) return [];
  return present.map((id) => AV_LAYERS.find((l) => l.id === id)!).filter(Boolean);
}
export function avOn(id: AvLayerId, present: AvLayerId[], layers: AvLayers): boolean {
  return present.includes(id) && (present.length < 2 || layers[id]);
}
/** ★ The basemap: airports and runways ('aero') unless AIRPORTS is switched off ('aero-plain' — the same map without them). */
export function avProfile(present: AvLayerId[], layers: AvLayers): 'aero' | 'aero-plain' {
  return avOn('airports', present, layers) ? 'aero' : 'aero-plain';
}

// ── the aircraft ───────────────────────────────────────────────────────────────────────────────────────────────────
export interface AvPlane {
  key: string; icao?: string; reg?: string; flight?: string; type?: string;
  lat?: number; lon?: number; alt?: number; trk?: number; gs?: number; ground?: boolean;
  posSrc?: string; posAge?: number; posTime?: string; from?: string; to?: string;
  label?: string; labelName?: string; app?: string; via?: string;
  /** the last OUT / OFF / ON / IN (the live datalink report's) */
  oooi?: string; oooiTime?: string;
  /** ★ what the live report said the fix came from ("FMS", "ACARS" layout, "ADS-C", "HFDL") — the row's words; posSrc is the source */
  posApp?: string;
  /** the sources that heard it (server names: "ADS-B", "VDL2", …) and how long ago each did (s) */
  src: string[]; srcAge: Record<string, number>;
  age: number; n: number;
  /** ★ this listener's live DATALINK report has it now (the rest is the shared table's) */
  live?: boolean;
  /** ★ beta88 (avNowRule): ADS-B heard it, but not in the last AV_ADSB_NOW_SEC — how long ago (s). 'ADS-B' is then out of
   *  `src` (the counts, layers and "heard on more than one" are NOW); the card keeps it as history. */
  adsbWas?: number;
  /** ★ beta88: the position is an ADS-B fix older than AV_ADSB_NOW_SEC — drawn faded, shown as "last seen HH:MM UTC",
   *  never as a live fix (no track, no speed). */
  lastSeen?: string;
  /** ★★ V12 beta91 WHAT THE AIRCRAFT IS — the ADS-B aircraft data (vibe_aircraft_info.h), for EVERY source: a VDL2 /
   *  ACARS / HFDL-only aircraft too (Stuart, 2026-11-22: "they are the same aircraft after all"). The server's names
   *  (op, opName, logo, sil, country, countryName, mil, desc, route …), cleaned by adsbInfo.ts cleanAdsbInfo; the row's
   *  own reg / type stay in `reg` / `type`. */
  info?: AdsbInfo;
}
export interface AvStation { id: number; name: string; lat: number; lon: number; khz: number; age: number }
/** ★ beta88 the server's ADS-B state (vibe_aircraft_share.h toJson). `live` null = an older server that cannot say. */
export interface AvAdsbState { live: boolean | null; last: number; off: boolean }
export interface SharedAircraft { now: number; count: number; off: boolean; aircraft: AvPlane[]; stations: AvStation[]; adsb: AvAdsbState }

const str = (v: unknown, max: number): string | undefined =>
  typeof v === 'string' && v.length ? v.replace(/[\u0000-\u001f\u007f]/g, '').slice(0, max) : undefined;
const num = (v: unknown): number | undefined => (typeof v === 'number' && isFinite(v) ? v : undefined);
const regKey = (r: string) => r.replace(/[.\- ]/g, '').toUpperCase();
const fltKey = (f: string) => f.replace(/[. ]/g, '').toUpperCase();

/** ★ beta91 the aircraft data the server attached to a row (the ADS-B names; r / t are the row's reg / type) — undefined
 *  when it sent none (an older server, an address the data does not know). */
function avInfoOf(a: unknown): AdsbInfo | undefined {
  const i = cleanAdsbInfo(a);
  delete i.r; delete i.t;
  return Object.keys(i).length ? i : undefined;
}
/** ★ GET /vibeserver/aircraft.json — parsed defensively (the server is not trusted to be well formed). */
export function sharedSanitise(j: any): SharedAircraft | null {
  if (!j || typeof j !== 'object' || !Array.isArray(j.aircraft)) return null;
  const aircraft: AvPlane[] = [];
  for (const a of j.aircraft.slice(0, 2048)) {
    if (!a || typeof a !== 'object') continue;
    const key = str(a.key, 24);
    if (!key) continue;
    const lat = num(a.lat), lon = num(a.lon), alt = num(a.alt);
    const src = Array.isArray(a.src) ? (a.src as unknown[]).filter((s): s is string => typeof s === 'string' && SRC_BY_NAME.has(s)).slice(0, 5) : [];
    const srcAge: Record<string, number> = {};
    if (a.srcAge && typeof a.srcAge === 'object') for (const s of src) { const v = num(a.srcAge[s]); if (v != null) srcAge[s] = Math.max(0, v); }
    const icao4 = (v: unknown) => (typeof v === 'string' && /^[A-Z0-9]{3,4}$/.test(v) ? v : undefined);
    const ok = lat != null && lon != null && Math.abs(lat) <= 90 && Math.abs(lon) <= 180;
    aircraft.push({
      key, icao: str(a.icao, 6), reg: str(a.reg, 11), flight: str(a.flight, 11), type: str(a.type, 7),
      lat: ok ? lat : undefined, lon: ok ? lon : undefined, alt: alt != null && alt > -2000 && alt < 70000 ? alt : undefined,
      trk: num(a.trk), gs: num(a.gs), ground: a.ground === 1, posSrc: str(a.posSrc, 6), posAge: num(a.posAge), posTime: str(a.posTime, 11),
      from: icao4(a.from) && icao4(a.to) ? icao4(a.from) : undefined, to: icao4(a.from) && icao4(a.to) ? icao4(a.to) : undefined,
      label: str(a.label, 3), labelName: str(a.labelName, 60), app: str(a.app, 7), via: str(a.via, 40),
      src, srcAge, age: Math.max(0, num(a.age) ?? 0), n: Math.max(0, num(a.n) ?? 0),
      info: avInfoOf(a),
    });
  }
  const stations: AvStation[] = [];
  if (Array.isArray(j.stations)) for (const s of j.stations.slice(0, 128)) {
    const id = num(s?.id), lat = num(s?.lat), lon = num(s?.lon);
    if (id == null || lat == null || lon == null || Math.abs(lat) > 90 || Math.abs(lon) > 180) continue;
    stations.push({ id, name: str(s.name, 40) ?? `station ${id}`, lat, lon, khz: num(s.khz) ?? 0, age: Math.max(0, num(s.age) ?? 0) });
  }
  const adsb: AvAdsbState = { live: j.adsbLive === 1 ? true : j.adsbLive === 0 ? false : null, last: num(j.adsbLast) ?? -1, off: j.adsbOff === 1 };
  return { now: num(j.now) ?? 0, count: num(j.count) ?? aircraft.length, off: j.off === 1, aircraft, stations, adsb };
}

/** ★ The datalink mode's live row → the map's shape (its sources from how it was heard). */
export function planeFromLive(a: AvAircraft, i: number): AvPlane {
  const src: string[] = [];
  const v = a.via || '';
  if (v.startsWith('VDL2')) src.push('VDL2'); else if (v.startsWith('ACARS')) src.push('ACARS'); else if (v.startsWith('HFDL')) src.push('HFDL');
  if (a.posSrc === 'ADS-C' || a.app === 'ADS-C') src.push('ADS-C');
  const srcAge: Record<string, number> = {};
  for (const s of src) srcAge[s] = a.age;
  return {
    key: a.icao || (a.reg ? `r:${regKey(a.reg)}` : a.flight ? `f:${fltKey(a.flight)}` : `row${i}`),
    icao: a.icao, reg: a.reg, flight: a.flight, lat: a.lat, lon: a.lon, alt: a.alt, posAge: a.posAge, posTime: a.posTime,
    posSrc: a.posSrc === 'ADS-C' ? 'ADS-C' : a.lat != null ? (src[0] ?? undefined) : undefined,
    posApp: a.posSrc, from: a.from, to: a.to, oooi: a.oooi, oooiTime: a.oooiTime, label: a.label, labelName: a.labelName, app: a.app, via: a.via, src, srcAge, age: a.age, n: a.n, live: true,
  };
}
/** ★★ ONE AIRCRAFT ONCE: the live report's rows (this radio, now) and the shared table's (every radio), joined by ICAO
 *  address, else registration, else flight — the live row's fields win, the table adds the sources the other radios heard
 *  and a newer position. Newest first. */
export function avMerge(live: AvReport | null, shared: SharedAircraft | null): AvPlane[] {
  const out: AvPlane[] = live ? live.aircraft.map(planeFromLive) : [];
  const byIcao = new Map<string, AvPlane>(), byReg = new Map<string, AvPlane>(), byFlt = new Map<string, AvPlane>();
  const index = (p: AvPlane) => { if (p.icao) byIcao.set(p.icao, p); if (p.reg) byReg.set(regKey(p.reg), p); if (p.flight) byFlt.set(fltKey(p.flight), p); };
  out.forEach(index);
  for (const s of shared?.aircraft ?? []) {
    let hit = (s.icao && byIcao.get(s.icao)) || (s.reg && byReg.get(regKey(s.reg))) || undefined;
    if (hit && s.icao && hit.icao && hit.icao !== s.icao) hit = undefined;
    if (!hit && s.flight) { const f = byFlt.get(fltKey(s.flight)); if (f && (!s.icao || !f.icao || f.icao === s.icao)) hit = f; }
    if (!hit) { const c = { ...s, src: s.src.slice(), srcAge: { ...s.srcAge } }; out.push(c); index(c); continue; }
    for (const n of s.src) {
      if (!hit.src.includes(n)) hit.src.push(n);
      const a = s.srcAge[n];
      if (a != null && (hit.srcAge[n] == null || a < hit.srcAge[n])) hit.srcAge[n] = a;
    }
    hit.icao = hit.icao || s.icao; hit.reg = hit.reg || s.reg; hit.flight = hit.flight || s.flight; hit.type = hit.type || s.type;
    if (s.info) hit.info = { ...s.info, ...(hit.info ?? {}) };   // ★ beta91 the aircraft data (the live row has none)
    if (!hit.from && s.from) { hit.from = s.from; hit.to = s.to; }
    if (s.lat != null && (hit.lat == null || (s.posAge ?? 1e9) < (hit.posAge ?? 1e9))) {
      hit.lat = s.lat; hit.lon = s.lon; hit.alt = s.alt; hit.trk = s.trk; hit.gs = s.gs; hit.ground = s.ground; hit.posSrc = s.posSrc; hit.posAge = s.posAge; hit.posTime = s.posTime;
    }
    if (s.icao && !hit.key.match(/^[0-9A-F]{6}$/)) hit.key = s.icao;
    hit.age = Math.min(hit.age, s.age);
  }
  const order = (p: AvPlane) => Math.min(p.age, ...Object.values(p.srcAge));
  return out.sort((a, b) => order(a) - order(b));
}

/** ★★★ beta88 ADS-B ONLY WHILE HEARD (Stuart, 2026-11-20, the Pi 500 in ACARS mode on the V4L that normally does ADS-B:
 *  "how is there 326 adsb aircraft on the map when i am in ACARS mode? plus my adsb only picks up about 30-40 on this
 *  box … there should be 0 then"). The server's table keeps every aircraft an HOUR — right for ACARS / VDL2 / HFDL, which
 *  talk in bursts minutes apart, wrong for ADS-B: an aircraft in range squitters its position twice a second, so one ADS-B
 *  has not heard for a minute has flown out of range or the radio stopped listening. Same number as the server's
 *  kAdsbNowSec (vibe_aircraft_share.h). */
export const AV_ADSB_NOW_SEC = 60;
/** ★★ V12 11-24: an ADS-B position is drawn faded ("last seen") from AV_ADSB_NOW_SEC until this, then removed (Stuart). */
export const AV_ADSB_EXPIRE_SEC = 300;
const hhmmUtc = (epochSec: number) => { const d = new Date(epochSec * 1000); return `${String(d.getUTCHours()).padStart(2, '0')}:${String(d.getUTCMinutes()).padStart(2, '0')}`; };
/** ★★ The now-rule, on the merged aircraft (copies; the input is not touched):
 *   · ADS-B heard it in the last AV_ADSB_NOW_SEC AND the server says a radio is decoding ADS-B (adsb.live; an older
 *     server that cannot say → the age alone) → ADS-B counts.
 *   · else an aircraft heard ONLY on ADS-B is gone — neither drawn nor counted;
 *   · else (a datalink source too) it stays with its datalink history: 'ADS-B' leaves `src` (adsbWas keeps when), and an
 *     ADS-B position is "last seen HH:MM UTC" — faded, never a live fix — and past AV_ADSB_EXPIRE_SEC it is removed.
 *  `adsbGone`: how many had ADS-B and lost it (the status line's ADS-B words). */
export function avNowRule(planes: AvPlane[], shared: SharedAircraft | null): { planes: AvPlane[]; adsbGone: number } {
  const adsbOk = shared?.adsb?.live !== false && !shared?.adsb?.off;
  const nowSec = shared?.now || Date.now() / 1000;
  const out: AvPlane[] = [];
  let adsbGone = 0;
  for (const p0 of planes) {
    const hadAdsb = p0.src.includes('ADS-B');
    const heard = hadAdsb && adsbOk && (p0.srcAge['ADS-B'] ?? Infinity) <= AV_ADSB_NOW_SEC;
    const p: AvPlane = { ...p0, src: p0.src.slice(), srcAge: { ...p0.srcAge } };
    if (hadAdsb && !heard) {
      adsbGone++;
      p.src = p.src.filter((x) => x !== 'ADS-B');
      if (!p.src.length) continue;                      // ADS-B only: not heard now = not on the map, not counted
      p.adsbWas = p0.srcAge['ADS-B'] ?? p0.age;
      delete p.srcAge['ADS-B'];
    }
    if (p.posSrc === 'ADS-B' && p.lat != null && (!heard || (p.posAge ?? Infinity) > AV_ADSB_NOW_SEC)) {
      if ((p.posAge ?? Infinity) > AV_ADSB_EXPIRE_SEC) {
        // ★★ V12 11-24 (Stuart: "keep them for 1 minute as now but then fade off at 5 mins") — past five minutes an ADS-B
        //   fix EXPIRES: the aircraft stays for its datalink ("no position") but is not drawn where it was. The hour-long
        //   faded "last seen" drew 16 aircraft at fixes minutes old after a radio moved from ADS-B to VDL2.
        p.lat = undefined; p.lon = undefined; p.trk = undefined; p.gs = undefined;
        p.posSrc = undefined; p.posAge = undefined; p.posTime = undefined;
      } else {
        p.lastSeen = hhmmUtc(nowSec - (p.posAge ?? 0));
        p.trk = undefined; p.gs = undefined;            // ★ never drawn as if it were flying there now
      }
    }
    out.push(p);
  }
  return { planes: out, adsbGone };
}
const SRC_LAYER: Record<string, AvSrcId> = Object.fromEntries(AV_SRC.map((s) => [s.name, s.id]));
export function avHave(planes: AvPlane[], stations: AvStation[]): AvHave {
  const h: AvHave = { adsb: 0, acars: 0, vdl2: 0, hfdl: 0, adsc: 0, stations: stations.length };
  for (const p of planes) for (const s of p.src) { const id = SRC_LAYER[s]; if (id) h[id]++; }
  return h;
}
/** ★ Drawn while ANY source that heard it is shown. */
export function avVisible(p: AvPlane, present: AvLayerId[], layers: AvLayers): boolean {
  return p.src.some((s) => { const id = SRC_LAYER[s]; return !!id && avOn(id, present, layers); });
}
/** A position is drawn for an hour (the table's keep); past ten minutes it is faded — the aircraft has flown on. */
export const AV_FADE_SEC = 600;
export function avFaded(p: AvPlane): boolean { return !!p.lastSeen || (p.posAge ?? 0) > AV_FADE_SEC; }
export function avColour(p: AvPlane): string { return SRC_BY_NAME.get(p.posSrc || p.src[0] || '')?.colour ?? '#ffd34d'; }
export function avName(p: AvPlane): string { return p.flight || p.reg || p.icao || 'aircraft'; }
/** ★ The aircraft glyph: pointing along its track where one is known (ADS-B), coloured by the source of its position. */
export function avPlaneIcon(p: AvPlane): { html: string; w: number; h: number } {
  const col = avColour(p), rot = p.trk != null ? Math.round(p.trk) : 0, op = avFaded(p) ? ' opacity=".45"' : '';
  const multi = p.src.length > 1 ? `<circle cx="20" cy="4" r="3.4" fill="#0b1420" stroke="${col}" stroke-width="1.2"/><text x="20" y="5.6" text-anchor="middle" font-family="ui-monospace,Menlo,monospace" font-size="5" font-weight="800" fill="#fff">${p.src.length}</text>` : '';
  return { w: 24, h: 24, html: `<svg width="24" height="24" viewBox="0 0 24 24" aria-hidden="true"${op}><g transform="rotate(${rot} 12 12)"><path fill="${col}" stroke="#000" stroke-width="0.8" d="M12 2l1.6 7.2 7.4 3.3v1.9l-7.4-1.6-.4 5.2 2.3 1.7V21L12 20l-3.5 1v-1.3l2.3-1.7-.4-5.2L3 14.4v-1.9l7.4-3.3z"/></g>${multi}</svg>` };
}
function heardLine(p: AvPlane): string {
  const now = AV_SRC.filter((s) => p.src.includes(s.name)).map((s) => `${s.name} ${ago(p.srcAge[s.name] ?? p.age)}`);
  if (p.adsbWas != null) now.push(`ADS-B ${heardAgo(p.adsbWas)} (not since)`);   // ★ beta88 the history, never a live source
  return now.join(' · ');
}
export function avPlaneCard(p: AvPlane, rx: LatLon | null, wrap: 'web' | 'app' = 'web'): string {
  const rows: string[] = [];
  const add = (k: string, v: string) => { if (v) rows.push(`<div class="r"><span class="k">${esc(k)}</span><span class="v">${esc(v)}</span></div>`); };
  add('Flight', p.flight ?? ''); add('Registration', p.reg ?? ''); add('ICAO', p.icao ?? ''); add('Type', p.type ?? '');
  if (p.from && p.to) add('Route', `${p.from} → ${p.to}`);
  if (p.oooi) add(p.oooi.toUpperCase(), p.oooiTime ? `${p.oooiTime} UTC` : 'heard');
  if (p.lat != null && p.lon != null) {
    add(p.lastSeen ? 'Last seen' : 'Position', `${Math.abs(p.lat).toFixed(3)}°${p.lat >= 0 ? 'N' : 'S'} ${Math.abs(p.lon).toFixed(3)}°${p.lon >= 0 ? 'E' : 'W'}`
      + `${p.posSrc ? ` · from ${p.posSrc}` : ''}${p.lastSeen ? ` · at ${p.lastSeen} UTC — not heard there since` : p.posAge != null ? ` · ${heardAgo(p.posAge)}` : ''}`);
    if (p.alt != null) add('Altitude', p.ground ? 'on the ground' : `${Math.round(p.alt).toLocaleString('en-GB')} ft`);
    if (p.gs != null) add('Speed', `${Math.round(p.gs)} kt${p.trk != null ? ` · track ${Math.round(p.trk)}°` : ''}`);
    if (p.posTime) add('Fix time', p.posTime.startsWith(':') ? `${p.posTime} past the hour (as reported)` : `${p.posTime} UTC (as reported)`);
    if (rx) { const rb = rangeBearing(rx, p.lat, p.lon); add('From here', `${rb.nm < 10 ? rb.nm.toFixed(1) : Math.round(rb.nm)} NM ${compass8(rb.brg)}`); }
  }
  add('Heard on', heardLine(p));
  if (p.labelName || p.label) add('Last message', p.labelName ? `${p.labelName}${p.label ? ` (label ${p.label})` : ''}` : `label ${p.label}`);
  if (p.app) add('Application', p.app);
  if (p.via) add('Via', p.via);
  const title = `<div class="t"><b>${esc(avName(p))}</b></div>`;
  const body = title + rows.join('');
  return wrap === 'app' ? `<div class="trkCard"><div style="white-space:normal;max-width:320px">${body}</div></div>` : `<div class="trkCard">${body}</div>`;
}
/* ★★★ V12 beta90 ONE AIRCRAFT MAP BUILT ON THE ADS-B MAP (Stuart, 2026-11-21): every aircraft — ADS-B, ACARS, VDL2, HFDL,
 *  ADS-C — is drawn by the ADS-B layer (web/mapkit/adsblayer.js): its silhouettes and tones, labels, range rings, AUTO / FIT,
 *  the glide. This is an AvPlane in the shape that layer reads (docs/v12/ADSB.md aircraft objects):
 *   · hex: the ICAO address where known, else a stable '~'-address made from the plane's key (the layer's own form for an
 *     aircraft without one — no address links are offered for it);
 *   · sparse: a datalink-only position (minutes between fixes): the layer GLIDES it to each new fix, faces it along the
 *     bearing of its last two fixes when no track is sent, and keeps its tones on a datalink clock (2 min / 10 min);
 *   · rowsExtra: the AVIATION MAP's own card lines (heard on, route, the last message) — text, escaped by the layer. */
export interface AvAdsbShape {
  hex: string; flight?: string; r?: string; t?: string; lat: number; lon: number;
  /** ★★ beta91 the ADS-B aircraft data's names, for every source — the layer's type silhouette (t), operator badge (op),
   *  the owner's logo / silhouette keys (logo, sil), the flag (country), MIL (mil), the card's lines (desc, opName, route) */
  desc?: string; wtc?: string; eng?: string; species?: string; country?: string; countryName?: string; mil?: 1;
  op?: string; opName?: string; logo?: string; sil?: string; route?: AdsbRoute;
  alt_baro?: number | 'ground'; gs?: number; track?: number; seen_pos?: number; seen?: number;
  sparse?: boolean; rowsExtra?: [string, string][];
}
function stableHex(key: string): string {
  let h = 0x811c9dc5;
  for (let i = 0; i < key.length; i++) { h ^= key.charCodeAt(i); h = Math.imul(h, 0x01000193) >>> 0; }
  return '~' + (h & 0xffffff).toString(16).padStart(6, '0');
}
export function avPlaneAdsb(p: AvPlane): AvAdsbShape | null {
  if (p.lat == null || p.lon == null) return null;
  const icao = p.icao && /^[0-9A-Fa-f]{6}$/.test(p.icao) ? p.icao.toLowerCase() : '';
  const rows: [string, string][] = [['Heard on', heardLine(p)]];
  if (p.posSrc) rows.push(['Position from', p.posSrc + (p.lastSeen ? ` · last seen ${p.lastSeen} UTC` : '')]);
  if (p.from && p.to) rows.push(['Route', `${p.from} → ${p.to}`]);
  if (p.oooi) rows.push([p.oooi.toUpperCase(), p.oooiTime ? `${p.oooiTime} UTC` : 'heard']);
  if (p.posTime) rows.push(['Fix time', p.posTime.startsWith(':') ? `${p.posTime} past the hour (as reported)` : `${p.posTime} UTC (as reported)`]);
  if (p.labelName || p.label) rows.push(['Last message', p.labelName ? `${p.labelName}${p.label ? ` (label ${p.label})` : ''}` : `label ${p.label}`]);
  if (p.app) rows.push(['Application', p.app]);
  if (p.via) rows.push(['Via', p.via]);
  const sparse = p.posSrc !== 'ADS-B' || !!p.lastSeen;
  // ★ beta91: the route the datalink heard (rows above) wins; the database's only where the aircraft said none
  const { r: _r, t: _t, route, ...info } = p.info ?? {};
  return {
    ...info, ...(route && !(p.from && p.to) ? { route } : {}),
    hex: icao || stableHex(p.key), flight: p.flight, r: p.reg, t: p.type, lat: p.lat, lon: p.lon,
    alt_baro: p.ground ? 'ground' : p.alt, gs: p.gs, track: p.trk,
    // ★ a "last seen" ADS-B fix is old by construction: drawn stale, never as live
    seen_pos: p.lastSeen ? Math.max(AV_FADE_SEC + 1, p.posAge ?? 0) : p.posAge, seen: p.age, sparse, rowsExtra: rows,
  };
}
export function avPlaneItems(planes: AvPlane[], rx: LatLon | null, wrap: 'web' | 'app' = 'web'): TrackItem[] {
  return planes.filter((p) => p.lat != null && p.lon != null).slice(0, 2048).map((p) => ({
    key: `a:${p.key}`, lat: p.lat as number, lon: p.lon as number, _ic: avPlaneIcon(p), _lb: avName(p), _cd: avPlaneCard(p, rx, wrap),
    _adsb: avPlaneAdsb(p),   // ★ beta90 the ADS-B layer draws it (TrackView aircraft:true / the app's aviation page)
    // ★ the declutter: a fresh fix's name above a faded one's; stations and airports' names yield to aircraft
    _rk: 5e11 - Math.min(p.posAge ?? 1e6, 1e6),
    _aria: `${avName(p)}: heard on ${p.src.join(', ')}${p.lastSeen ? `; its ADS-B position was last seen at ${p.lastSeen} UTC` : ''}. Press Enter for the card.`,
  } as TrackItem));
}
export function avPlaneRows(planes: AvPlane[], rx: LatLon | null): TrackRow[] {
  return planes.slice(0, 2048).map((p) => {
    const rb = rx && p.lat != null && p.lon != null ? rangeBearing(rx, p.lat, p.lon) : null;
    // ★ beta91 the operator and the flag for every source (the ADS-B list's own: opName, else the badge code)
    const who = [p.reg && p.reg !== avName(p) ? p.reg : '', p.icao && p.icao !== avName(p) ? p.icao : '', p.type ?? '',
                 p.info?.opName || p.info?.op || '', p.info?.mil ? 'MIL' : ''].filter(Boolean).join(' · ');
    const dbRoute = !(p.from && p.to) ? routeShort(p.info?.route) : '';
    return {
      key: `a:${p.key}`, colour: avColour(p), flag: flagEmoji(p.info?.country), name: avName(p),
      second: [who, p.src.join(' + '), avRoute(p as unknown as AvAircraft) || dbRoute,
               p.lastSeen ? `ADS-B last seen ${p.lastSeen} UTC` : avFix({ ...p, posSrc: p.posApp ?? p.posSrc } as unknown as AvAircraft), p.labelName ?? ''].filter(Boolean).join(' · '),
      sp: p.alt != null ? (p.ground ? 'GND' : `FL${String(Math.max(0, Math.round(p.alt / 100))).padStart(3, '0')}`) : '', trend: 0 as const,
      // ★ beta95 NM on the aviation map — the list, the card and the rings agree
      range: rb ? `${rb.nm < 10 ? rb.nm.toFixed(1) : Math.round(rb.nm)} NM` : p.lat == null ? 'no position' : '',
      brg: rb ? compass8(rb.brg) : '', noPos: p.lat == null, age: ago(Math.min(p.age, ...Object.values(p.srcAge))), em: false,
    };
  });
}
/** ★ The HFDL ground station glyph: a mast, the colour of HFDL. */
export function avStationIcon(): { html: string; w: number; h: number } {
  return { w: 20, h: 20, html: '<svg width="20" height="20" viewBox="0 0 20 20" aria-hidden="true"><path d="M10 3 L14 18 L12.6 18 L10 9 L7.4 18 L6 18 Z" fill="#ffa657" stroke="#000" stroke-width=".8"/><circle cx="10" cy="3.5" r="2" fill="#ffa657" stroke="#000" stroke-width=".8"/></svg>' };
}
export function avStationItems(st: AvStation[], wrap: 'web' | 'app' = 'web'): TrackItem[] {
  return st.map((s) => {
    const body = `<div class="t"><b>HFDL ${esc(s.name)}</b></div><div class="r"><span class="k">Heard</span><span class="v">${esc(`${s.khz ? `${Math.round(s.khz)} kHz · ` : ''}${ago(s.age)} ago`)}</span></div>`;
    return { key: `g:${s.id}`, lat: s.lat, lon: s.lon, _ic: avStationIcon(), _lb: `HFDL ${s.name}`,
      _cd: wrap === 'app' ? `<div class="trkCard"><div style="white-space:normal;max-width:320px">${body}</div></div>` : `<div class="trkCard">${body}</div>`,
      _rk: 1e11, _aria: `HFDL ground station ${s.name}. Press Enter for the card.` } as TrackItem;
  });
}
/** ★★ beta88 the ADS-B words, in plain language, when the map has no ADS-B aircraft to show: why. '' when it has some, or
 *  when ADS-B has nothing to explain (a server that never decoded it and has no ADS-B history, not opened for ADS-B). */
export function avAdsbNote(planes: AvPlane[], adsb: AvAdsbState | null | undefined, adsbGone: number, focus: AvFocus): string {
  if (planes.some((p) => p.src.includes('ADS-B'))) return '';
  const relevant = adsbGone > 0 || focus === 'adsb' || (adsb?.last ?? -1) >= 0;
  if (!relevant) return '';
  if (adsb?.off) return 'ADS-B: switched off on this server';
  if (adsb?.live === false) return adsb.last >= 0 ? 'ADS-B: this server\'s ADS-B radio is in use for something else' : 'ADS-B: no radio on this server is decoding it';
  return 'ADS-B: none heard in the last minute';
}
/** ★ The status line counts NOW (beta88): ADS-B's within the last minute (avNowRule), the datalink's within the hour the
 *  server keeps them (their bursts are minutes apart). */
export function avMapStatus(planes: AvPlane[], note = ''): string {
  if (!planes.length) return note;
  const pos = planes.filter((p) => p.lat != null).length;
  const by = AV_SRC.map((s) => [s.name, planes.filter((p) => p.src.includes(s.name)).length] as const).filter(([, n]) => n > 0).map(([n, c]) => `${n} ${c}`);
  const both = planes.filter((p) => p.src.filter((s) => s !== 'ADS-C').length > 1).length;
  return `${planes.length} aircraft (${pos} on the map)${by.length ? ` · ${by.join(' · ')}` : ''}${both ? ` · ${both} heard on more than one` : ''}${note ? ` · ${note}` : ''}`;
}
export const AV_EMPTY = 'No aircraft yet. ADS-B (1090 MHz), the aircraft datalink (ACARS / VDL2) and HFDL each add theirs — on any radio of this server.';

// ── the KEY ────────────────────────────────────────────────────────────────────────────────────────────────────────
export function avLegendRows(present: AvLayerId[], adsbWay = false): Array<[string, string]> {
  /* ★★ V12 beta90: on the map built on the ADS-B layer, the aircraft are its silhouettes in its tones (adsblayer.js TONES,
   *  dark) — how fresh the position is, on ADS-B's clock or the datalink's; which sources heard it is on its card. */
  if (adsbWay) {
    const pl = (rgb: string) => `<span style="display:inline-block;width:20px;text-align:center;color:rgb(${rgb})">✈</span>`;
    const r: Array<[string, string]> = [
      [pl('80,255,120'), 'A fresh position — under 10 s (ADS-B) or 2 min (ACARS, VDL2, HFDL, ADS-C)'],
      [pl('180,255,80'), 'Ageing — 10–30 s (ADS-B) or 2–10 min (datalink)'],
      [pl('255,190,40'), 'Old — over 30 s (ADS-B) or 10 min (datalink): it has flown on'],
      [pl('150,160,170'), 'On the ground'], [pl('255,80,60'), 'Emergency'], [pl('90,170,255'), 'Military'],
      ['<span style="display:inline-block;width:20px;text-align:center">➚</span>', 'Pointing the way it is going: its track, or the line between its last two fixes; each new fix glides in'],
      ['<span style="display:inline-block;width:20px;text-align:center">◎</span>', 'RINGS: range rings in NM · AUTO / FIT: keep every aircraft in view · its card says which sources heard it'],
    ];
    if (present.includes('stations')) r.push([`<span style="display:inline-block;width:20px;height:20px;overflow:hidden;vertical-align:middle">${avStationIcon().html}</span>`, 'HFDL ground station heard']);
    if (present.includes('airports')) r.push(['<span style="display:inline-block;width:20px;text-align:center">✈</span>', 'Airports and runways (the AIRPORTS layer)']);
    return r;
  }
  const small = (h: string) => `<span style="display:inline-block;width:20px;height:20px;overflow:hidden;vertical-align:middle;transform:scale(.8);transform-origin:center">${h}</span>`;
  const plane = (posSrc: string, src: string[], extra: Partial<AvPlane> = {}) => avPlaneIcon({ key: 'k', src, srcAge: {}, age: 0, n: 1, posSrc, trk: 45, ...extra }).html;
  const rows: Array<[string, string]> = [];
  for (const s of AV_SRC) if (present.includes(s.id)) rows.push([small(plane(s.name, [s.name])), `Aircraft whose position came from ${s.name}`]);
  rows.push([small(plane('ADS-B', ['ADS-B', 'VDL2'])), 'The number: heard on that many sources (one aircraft, never drawn twice)']);
  rows.push([small(plane('VDL2', ['VDL2'], { posAge: AV_FADE_SEC + 1 })), 'Faded: an old position — a datalink fix over ten minutes old, or an ADS-B one not heard for a minute ("last seen"; removed after five)']);
  if (present.includes('stations')) rows.push([small(avStationIcon().html), 'HFDL ground station heard']);
  if (present.includes('airports')) rows.push(['<span style="display:inline-block;width:20px;text-align:center">✈</span>', 'Airports and runways (the AIRPORTS layer)']);
  return rows;
}
export function avLegendHtml(present: AvLayerId[], adsbWay = false): string {
  return avLegendRows(present, adsbWay).map(([g, w]) => `<div class="kr">${g}<span>${esc(w)}</span></div>`).join('');
}

/** ★★ V12 beta91 THE ODC-By NOTICE ON THE LIST (Stuart, 2026-11-22). ODC-By 1.0 §4.3: the notice goes with the produced
 *  work wherever the Mictronics fields show — the ADS-B layer's card carries it (adsblayer.js `credit`); the AIRCRAFT list
 *  shows registrations, types and operators from the same data, so it carries it too, as one small line at its foot.
 *  The card's own words and links (kept in step with web/mapkit/adsblayer.js). Shown while any row shows the data: a type
 *  (the datalink never sends one — it is the data's) or the aircraft data the server attached. */
export const AV_DATA_CREDIT = ADSB_DATA_CREDIT;
export const avCreditHtml = adsbCreditHtml;
export function avShowsData(planes: AvPlane[]): boolean {
  return planes.some((p) => !!p.type || !!(p.info && Object.keys(p.info).length));
}
/** ★★ ONE COMPOSITION for both clients: the items, the list's rows (sections when several layers), the status line. */
export function avCompose(live: AvReport | null, shared: SharedAircraft | null, focus: AvFocus, layers: AvLayers, rx: LatLon | null, wrap: 'web' | 'app' = 'web') {
  const { planes, adsbGone } = avNowRule(avMerge(live, shared), shared);   // ★ beta88 ADS-B only while heard
  const stations = shared?.stations ?? [];
  const present = avLayersPresent(avHave(planes, stations), focus);
  const shown = planes.filter((p) => avVisible(p, present, layers));
  const items: TrackItem[] = [...avPlaneItems(shown, rx, wrap)];
  if (avOn('stations', present, layers)) items.push(...avStationItems(stations, wrap));
  const note = avAdsbNote(planes, shared?.adsb, adsbGone, focus);
  return { planes: shown, items, rows: avPlaneRows(shown, rx), status: avMapStatus(shown, note), present, toggles: avToggles(present), profile: avProfile(present, layers),
           credit: avShowsData(shown) };
}
