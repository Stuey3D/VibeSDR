// adsbTypes.ts — VibeServer's ADS-B 1090 mode, as the app reads it (V12, docs/v12/ADSB.md).
//
// ★★★ ONE TABLE, TWO SHAPES. The server speaks dump1090's `aircraft.json` field names (hex,
//     alt_baro, gs, baro_rate, track, messages …) because that is what every ADS-B tool already
//     knows. The app has spoken OWRX's shape (icao, altitude, speed, vspeed, course, msgs) since
//     OWRX ADS-B landed, and the decoder box (AircraftPanel) and the WATCH (WatchLink.Aircraft,
//     a Codable keyed on exactly those names) both read it. So the report is mapped HERE, once,
//     into the existing `Aircraft` — and the raw objects are kept beside it for the map, whose
//     shared layer (web/mapkit/adsblayer.js) is written against the server's names.
// ★★ PURE: no React Native, no imports at run time — scripts/test_adsb_report.ts runs it in node.
// ★ Every field is checked for its TYPE, never trusted: this is somebody else's server's JSON, and
//   a string where a number belongs must become "unknown", not NaN in a table cell.
import type { Aircraft } from './SDRBackend';
import { adsbInfoLine, type AdsbInfo } from './adsbInfo.ts';

/** What `/vibeserver.json` says about ADS-B on THIS radio. `undefined` = an older server (no field):
 *  draw no control at all. `{ ok: false, why }` = draw it DISABLED, with the reason (never hidden). */
export interface AdsbCap { ok: boolean; why?: string }

/** Read the capability from a radio's /vibeserver.json body. `modes` containing "adsb" is the same
 *  as `adsb: true` (the directory's badge list) — ADSB.md, "Server contract". */
export function parseAdsbCapability(j: unknown): AdsbCap | undefined {
  if (!j || typeof j !== 'object') return undefined;
  const o = j as Record<string, unknown>;
  if (o.adsb === true) return { ok: true };
  if (Array.isArray(o.modes) && o.modes.some((m) => m === 'adsb')) return { ok: true };
  if (o.adsb === false) {
    const why = cleanText(o.adsbWhy, 200);
    return { ok: false, why: why || 'ADS-B is not available on this receiver' };
  }
  return undefined;
}

/** One aircraft as the server sends it (dump1090 names; absent = unknown). Kept for the map.
 *  ★ V12: plus what it IS (AdsbInfo — r, t, op, country, route …), merged in from adsb_info (adsbInfo.ts). */
export interface AdsbAircraftRaw extends AdsbInfo {
  hex: string;
  flight?: string;
  category?: string;
  lat?: number; lon?: number;
  seen_pos?: number;
  /** ft, or 'ground' */
  alt_baro?: number | 'ground';
  alt_geom?: number;
  gs?: number; track?: number; mag_heading?: number; tas?: number; ias?: number;
  baro_rate?: number;
  squawk?: string;
  emergency?: string;
  seen?: number;
  messages?: number;
  rssi?: number;
  /** ★ how far the server has followed it, NM (vibe_adsb.h setPos) */
  tracked_nm?: number;
}

/** A whole `adsb` report. */
export interface AdsbReport {
  now: number;
  messages: number;
  aircraft: AdsbAircraftRaw[];
  rate?: number; noise?: number; msgRate?: number; dropped?: number;
  /** ★ % of the promised samples the server's decoder RECEIVED over ~10 s; -1 until known (vibe_adsb_capture.h). */
  capture?: number;
  /** ★ V12 (19f2d603): the gain ADS-B is at (dB), whether the server is choosing it (gainAuto 1) and still measuring
   *  (gainBusy 1 — the ramp or the ten-minute look trying steps). Absent on an older server: no GAIN key. */
  gain?: number; gainAuto?: number; gainBusy?: number;
}

/** ★★★ THE CAPTURE WARNING for the map's top bar (Stuart, 2026-10-24): under 97 % the receiver is dropping samples
 *  before the decoder sees them (the dongle / USB bus, or the server's IQ queue — vibe_adsb_capture.h CaptureMeter).
 *  `short` stays on a phone; `rest` says what it means. Null when all is well.
 *  ★★ ONE RULE, TWO READERS: web/client/src/adsb.ts adsbCaptureWarning — scripts/test_capture_and_rate_guard.ts. */
export const ADSB_CAPTURE_WARN_PCT = 97;
export function adsbCaptureWarning(r: Pick<AdsbReport, 'capture' | 'rate'> | null | undefined): { short: string; rest: string } | null {
  const c = r ? num(r.capture) : undefined;
  if (c === undefined || c < 0 || c >= ADSB_CAPTURE_WARN_PCT) return null;
  return { short: `capture ${Math.round(c)} %`, rest: ' \u2014 the receiver is dropping samples (its USB bus or this server cannot keep up)' };
}

/* ★ Bounded, printable text only — a callsign is drawn in a list and injected into a WebView. */
function cleanText(v: unknown, max: number): string {
  if (typeof v !== 'string') return '';
  // eslint-disable-next-line no-control-regex
  return v.replace(/[\u0000-\u001f\u007f<>]/g, '').trim().slice(0, max);
}
function num(v: unknown): number | undefined {
  return typeof v === 'number' && Number.isFinite(v) ? v : undefined;
}

/** ★ The report is capped: the server drops an aircraft after 60 s, but a hostile or broken one
 *  could send anything, and this array is rendered as a list and pushed into the map each second. */
export const ADSB_MAX_AIRCRAFT = 1000;

export function parseAdsbMessage(msg: Record<string, unknown>): AdsbReport {
  const list = Array.isArray(msg.aircraft) ? msg.aircraft : [];
  const aircraft: AdsbAircraftRaw[] = [];
  for (const x of list) {
    if (aircraft.length >= ADSB_MAX_AIRCRAFT) break;
    if (!x || typeof x !== 'object') continue;
    const a = x as Record<string, unknown>;
    const hex = cleanText(a.hex, 8).toLowerCase();
    if (!/^~?[0-9a-f]{6}$/.test(hex)) continue;          // ★ the one field that must be there
    const r: AdsbAircraftRaw = { hex };
    const flight = cleanText(a.flight, 12);
    if (flight) r.flight = flight;
    const cat = cleanText(a.category, 2).toUpperCase();
    if (/^[A-D][0-7]$/.test(cat)) r.category = cat;
    const lat = num(a.lat), lon = num(a.lon);
    if (lat !== undefined && lon !== undefined && Math.abs(lat) <= 90 && Math.abs(lon) <= 180) {
      r.lat = lat; r.lon = lon;
    }
    if (a.alt_baro === 'ground') r.alt_baro = 'ground';
    else if (num(a.alt_baro) !== undefined) r.alt_baro = num(a.alt_baro);
    for (const k of ['seen_pos', 'alt_geom', 'gs', 'track', 'mag_heading', 'tas', 'ias',
                     'baro_rate', 'seen', 'messages', 'rssi', 'tracked_nm'] as const) {
      const v = num(a[k]);
      if (v !== undefined) (r as unknown as Record<string, number>)[k] = v;
    }
    const sq = cleanText(typeof a.squawk === 'number' ? String(a.squawk) : a.squawk, 4);
    if (/^[0-7]{4}$/.test(sq)) r.squawk = sq;
    const em = typeof a.emergency === 'number' ? String(a.emergency) : cleanText(a.emergency, 16);
    if (em && em !== '0' && em !== 'none') r.emergency = em;
    aircraft.push(r);
  }
  return {
    now: num(msg.now) ?? 0,
    messages: num(msg.messages) ?? 0,
    aircraft,
    rate: num(msg.rate), noise: num(msg.noise), msgRate: num(msg.msgRate), dropped: num(msg.dropped),
    capture: num(msg.capture),
    gain: num(msg.gain), gainAuto: num(msg.gainAuto), gainBusy: num(msg.gainBusy),
  };
}

/** Great-circle distance, km — the same formula (and the same rounding) OwrxAdapter uses, so a
 *  row reads the same whichever server the aircraft came from. */
export function haversineKm(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const R = 6371;
  const dLat = (lat2 - lat1) * Math.PI / 180;
  const dLon = (lon2 - lon1) * Math.PI / 180;
  const a = Math.sin(dLat / 2) ** 2 +
            Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) * Math.sin(dLon / 2) ** 2;
  return Math.round(R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a)));
}
/** Initial bearing from the receiver to the aircraft, degrees (0–359). */
export function bearingDeg(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const p1 = lat1 * Math.PI / 180, p2 = lat2 * Math.PI / 180;
  const dl = (lon2 - lon1) * Math.PI / 180;
  const y = Math.sin(dl) * Math.cos(p2);
  const x = Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl);
  return Math.round((Math.atan2(y, x) * 180 / Math.PI + 360) % 360);
}

/** ★★ THE SERVER'S AIRCRAFT IN THE APP'S SHAPE, nearest first.
 *  hex→icao, alt_baro→altitude ("ground" → 0 and `onGround`), gs→speed, baro_rate→vspeed,
 *  track→course (the heading when there is no track), messages→msgs; distance and bearing are
 *  OURS to compute from the receiver, exactly as OwrxAdapter does (the server sends position,
 *  not range). `rx` null/0,0 = unknown receiver: no distances, sorted by signal instead. */
export function adsbToAircraft(list: AdsbAircraftRaw[], rx: { lat: number; lon: number } | null): Aircraft[] {
  const rxOk = !!rx && Number.isFinite(rx.lat) && Number.isFinite(rx.lon) && (rx.lat !== 0 || rx.lon !== 0);
  const out = list.map((a): Aircraft => {
    const hasPos = a.lat !== undefined && a.lon !== undefined;
    const ground = a.alt_baro === 'ground';
    const row: Aircraft = {
      icao: a.hex,
      flight: a.flight,
      altitude: ground ? 0 : typeof a.alt_baro === 'number' ? a.alt_baro : undefined,
      speed: a.gs,
      vspeed: a.baro_rate,
      course: a.track ?? a.mag_heading,
      squawk: a.squawk,
      rssi: a.rssi,
      msgs: a.messages,
      lat: a.lat, lon: a.lon,
      distKm: hasPos && rxOk ? haversineKm(rx!.lat, rx!.lon, a.lat!, a.lon!) : undefined,
      bearing: hasPos && rxOk ? bearingDeg(rx!.lat, rx!.lon, a.lat!, a.lon!) : undefined,
    };
    // ★ The raw extras ride along (the map and the box can use them; the watch's Codable ignores them).
    if (a.category) row.category = a.category;
    if (a.seen !== undefined) row.seen = a.seen;
    // ★ tracked for xx — the server's NM in the panel's unit (km, as distKm)
    if (a.tracked_nm !== undefined && a.tracked_nm >= 1) row.trackedKm = Math.round(a.tracked_nm * 1.852);
    if (ground) row.onGround = true;
    if (a.emergency) row.emergency = a.emergency;
    // ★ V12 aircraft data (merged from adsb_info): what the aircraft IS, for the list's second line
    if (a.r) row.reg = a.r;
    if (a.country) row.ccode = a.country;
    if (a.countryName) row.country = a.countryName;
    if (a.op) row.op = a.op;
    if (a.mil) row.mil = true;
    if (a.logo) row.logo = a.logo;
    const info = adsbInfoLine(a);
    if (info) row.info = info;
    // ★ Drop undefined keys: this array is JSON'd to the watch every few seconds.
    for (const k of Object.keys(row) as (keyof Aircraft)[]) if (row[k] === undefined) delete row[k];
    return row;
  });
  // ★ AircraftPanel sorts again (it serves OWRX too); sorting here as well keeps the watch's order the same.
  return out.sort((x, y) => {
    if (x.distKm != null && y.distKm != null) return x.distKm - y.distKm;
    if (x.distKm != null) return -1;
    if (y.distKm != null) return 1;
    return (y.rssi ?? -99) - (x.rssi ?? -99);
  });
}

/** The decoder box's status line: what is being heard, in words. */
export function adsbStatusText(r: AdsbReport | null): string {
  if (!r) return 'starting…';
  const n = r.aircraft.length;
  const pos = r.aircraft.filter((a) => a.lat !== undefined).length;
  const rate = r.msgRate !== undefined ? ` · ${Math.round(r.msgRate)} msg/s` : '';
  if (!n) return `listening…${rate}`;
  return `${n} aircraft${pos < n ? ` (${pos} placed)` : ''}${rate}`;
}

/** The map's in-map toggles (the shared layer draws them; the app remembers them per device). */
export interface AdsbMapPrefs { autoZoom: boolean; fit: boolean; rings: boolean; labels: boolean }
export const ADSB_MAP_PREFS_DEFAULT: AdsbMapPrefs = { autoZoom: true, fit: false, rings: true, labels: true };
export function parseAdsbMapPrefs(s: string | null | undefined): AdsbMapPrefs {
  try {
    const j = s ? JSON.parse(s) : null;
    if (!j || typeof j !== 'object') return { ...ADSB_MAP_PREFS_DEFAULT };
    const b = (v: unknown, d: boolean) => (typeof v === 'boolean' ? v : d);
    return { autoZoom: b(j.autoZoom, ADSB_MAP_PREFS_DEFAULT.autoZoom),
             fit: b(j.fit, ADSB_MAP_PREFS_DEFAULT.fit),
             rings: b(j.rings, ADSB_MAP_PREFS_DEFAULT.rings),
             labels: b(j.labels, ADSB_MAP_PREFS_DEFAULT.labels) };
  } catch { return { ...ADSB_MAP_PREFS_DEFAULT }; }
}

/* ── ★★ THE ADS-B GAIN KEY AND ITS CARD (V12, 19f2d603 — the web's adsbGainRender) ────────────────────────────────────
 * The radio's own steps, shown ONLY where a gain can be set: not on a radio with no gain list (an Airspy HF+ has none —
 * AGENTS.md: a control that only works on one radio must not be there), not where the owner fixed the gain (hwGainLocked
 * hides every gain control), not on a server that does not report one. AUTO is the SERVER's ramp (messages decoded), not
 * VibeAGC; a step chosen here is the listener's until AUTO is pressed again. The owner's per-band ceiling holds here too. */

export interface AdsbGainView {
  /** "GAIN 33.8 · AUTO" / "GAIN 33.8 dB" — the key's text (TRAINING is drawn beside it when `training`). */
  key: string; auto: boolean; training: boolean;
  /** The gain now, dB, one place. */
  now: string;
  /** Index in the radio's list nearest the report's gain, and the highest index the owner allows. */
  idx: number; maxIdx: number;
  note: string;
}
/** The highest step index the owner's ceiling allows (tenths of a dB, -1 = none) — the web's applyGainCap. */
export function gainCapIdx(gains: readonly number[], capTenthDb: number): number {
  if (capTenthDb < 0) return gains.length - 1;
  let m = -1;
  for (let i = 0; i < gains.length; i++) if (gains[i] <= capTenthDb) m = i;
  return m < 0 ? 0 : m;
}
/** The step nearest a gain in tenths of a dB (the web's nearestGainIdx). */
export function nearestGainIdx(gains: readonly number[], tenthDb: number): number {
  let best = -1, bd = Infinity;
  for (let i = 0; i < gains.length; i++) { const d = Math.abs(gains[i] - tenthDb); if (d < bd) { bd = d; best = i; } }
  return best;
}
export function adsbGainView(r: Pick<AdsbReport, 'gain' | 'gainAuto' | 'gainBusy'> | null | undefined,
                             gains: readonly number[], locked: boolean, capTenthDb = -1): AdsbGainView | null {
  if (!r || typeof r.gain !== 'number' || !Number.isFinite(r.gain) || gains.length <= 1 || locked) return null;
  const auto = r.gainAuto === 1, busy = r.gainBusy === 1;
  const maxIdx = gainCapIdx(gains, capTenthDb);
  return {
    key: `GAIN ${r.gain.toFixed(1)}${auto ? ' · AUTO' : ' dB'}`,
    auto, training: auto && busy, now: `${r.gain.toFixed(1)} dB`,
    idx: Math.min(maxIdx, Math.max(0, nearestGainIdx(gains, Math.round(r.gain * 10)))), maxIdx,
    note: auto
      ? (busy ? 'Finding the gain that decodes the most — trying each step for a few seconds.'
              : 'Automatic: chosen by messages decoded, and checked again every ten minutes.')
      : 'Set by hand. AUTO hands it back.',
  };
}
/** ★ A − / + press: one step from where the LISTENER last put it (for 2.5 s after a press — the report lags about a second,
 *  so stepping from the report would repeat a step), else from the report's gain; clamped to 0…maxIdx. */
export const ADSB_GAIN_TOUCH_MS = 2500;
export function adsbGainStep(view: AdsbGainView, last: { idx: number; at: number } | null, now: number, dir: -1 | 1): number {
  const from = last && now - last.at < ADSB_GAIN_TOUCH_MS ? last.idx : view.idx;
  return Math.max(0, Math.min(view.maxIdx, from + dir));
}
