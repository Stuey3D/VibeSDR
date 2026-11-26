/**
 * sondes.ts — ★ V12 RADIOSONDES (docs/v12/RS41.md): the words and shapes both clients use for a VibeServer's SONDES
 * whole-radio mode ({"type":"sondes"}: weather balloons on 400–406 MHz — Vaisala RS41, Graw DFM, Meteomodem M10 / M20,
 * the type found per sonde by the server, docs/v12/SONDES-TYPES.md). One copy (the app's SondesPanel and the web's
 * sondes.ts), so every screen says the same thing.
 * ★ tracks.ts's range and bearing; the map's icon and card are HTML strings (the web's TrackView and the app's MapOverlay
 *   WebView draw the same ones), every field escaped.
 * ★ NEVER A GUESSED PTU: the server sends `temp` only once the sonde's whole calibration is in (`cal` of `calOf`: 51
 *   subframes for an RS41, 6 channels for a DFM; calOf 0 = this type's temperature is not decoded); until then the line
 *   says how much has arrived. Humidity and pressure are not decoded at all.
 */

import { rangeBearing, type LatLon, type TrackItem, type TrackRow } from './tracks';

/** One sonde from the report (vibe_rs41_service.h Tracker::json). */
export interface Sonde {
  serial: string; kind: string; model?: string; hz: number; txHz?: number;
  age: number; n: number; frame: number; cal: number; calOf: number;
  lat?: number; lon?: number; alt?: number; climb?: number; speed?: number; heading?: number; sats?: number;
  posAge?: number; maxAlt?: number; temp?: number; batt?: number; enc?: boolean; gpsTime?: number;
  /** ★ beta96 the last frame's SNR: S/N in the scan's box (dB) — only from a server that says so (report snrv ≥ 2; an
   *  older one sent the demodulator's eye figure, which saturates near 5 dB) */
  snr?: number;
  track: [number, number, number][];
}
/** A band the server steps through (hwinfo / vibeserver.json sondesBands). */
export interface SondeBand { id: string; name: string; hz: number; lo: number; hi: number }
/** {"type":"sondes", band, bandName, hz, lo, hi, dropped, chans, frames, sondes, gain?, gainAuto?} */
export interface SondeReport {
  band: string; bandName: string; hz: number; lo: number; hi: number; dropped: number; frames: number;
  chans: Array<{ hz: number; db: number; n: number }>;
  sondes: Sonde[]; gain?: number; gainAuto?: number;
}

const str = (v: unknown, max = 40): string | undefined =>
  typeof v === 'string' && v.length ? v.replace(/[\u0000-\u001f\u007f]/g, '').slice(0, max) : undefined;
const num = (v: unknown): number | undefined => (typeof v === 'number' && isFinite(v) ? v : undefined);
const lat = (v: unknown) => { const x = num(v); return x != null && Math.abs(x) <= 90 ? x : undefined; };
const lon = (v: unknown) => { const x = num(v); return x != null && Math.abs(x) <= 180 ? x : undefined; };

/** Parse a report defensively (bounded; control characters out; bad numbers dropped). Null = not one. */
export function parseSondeReport(m: unknown): SondeReport | null {
  const o = m as Record<string, unknown> | null;
  if (!o || o.type !== 'sondes' || !Array.isArray(o.sondes)) return null;
  const sondes: Sonde[] = [];
  const snrOk = (num(o.snrv) ?? 0) >= 2;
  for (const s0 of (o.sondes as unknown[]).slice(0, 64)) {
    const s = s0 as Record<string, unknown>;
    if (!s || typeof s !== 'object') continue;
    const serial = str(s.serial, 12);
    if (!serial) continue;
    const track: [number, number, number][] = [];
    if (Array.isArray(s.track))
      for (const p of (s.track as unknown[]).slice(0, 400)) {
        if (!Array.isArray(p)) continue;
        const a = lat(p[0]), b = lon(p[1]), h = num(p[2]);
        if (a != null && b != null) track.push([a, b, h ?? 0]);
      }
    const la = lat(s.lat), lo = lon(s.lon);
    // ★ calOf: what this type's calibration needs (an older server sends none: an RS41's 51)
    const calOf = Math.max(0, Math.min(64, num(s.calOf) ?? 51)), cal = Math.max(0, Math.min(calOf || 64, num(s.cal) ?? 0));
    sondes.push({
      serial, kind: str(s.kind, 10) ?? 'RS41', model: str(s.model, 16), hz: num(s.hz) ?? 0, txHz: num(s.txHz),
      age: num(s.age) ?? 0, n: num(s.n) ?? 0, frame: num(s.frame) ?? -1, cal, calOf,
      lat: la != null && lo != null ? la : undefined, lon: la != null && lo != null ? lo : undefined,
      alt: num(s.alt), climb: num(s.climb), speed: num(s.speed), heading: num(s.heading), sats: num(s.sats),
      posAge: num(s.posAge), maxAlt: num(s.maxAlt),
      temp: calOf > 0 && cal >= calOf ? num(s.temp) : undefined,   // ★ a temperature with an incomplete calibration is not shown
      batt: num(s.batt), enc: s.enc === true, gpsTime: num(s.gpsTime), track,
      ...(snrOk && num(s.snr) != null ? { snr: num(s.snr) } : {}),
    });
  }
  const chans: SondeReport['chans'] = [];
  if (Array.isArray(o.chans))
    for (const c0 of (o.chans as unknown[]).slice(0, 16)) {
      const c = c0 as Record<string, unknown>;
      const hz = num(c?.hz);
      if (hz != null) chans.push({ hz, db: num(c.db) ?? 0, n: num(c.n) ?? 0 });
    }
  return { band: str(o.band, 10) ?? '', bandName: str(o.bandName, 24) ?? '', hz: num(o.hz) ?? 0, lo: num(o.lo) ?? 0, hi: num(o.hi) ?? 0,
           dropped: num(o.dropped) ?? 0, frames: num(o.frames) ?? 0, chans, sondes, gain: num(o.gain), gainAuto: num(o.gainAuto) };
}
export function parseSondeBands(v: unknown): SondeBand[] {
  if (!Array.isArray(v)) return [];
  const out: SondeBand[] = [];
  for (const b0 of v.slice(0, 8)) {
    const b = b0 as Record<string, unknown>;
    const id = str(b?.id, 10), name = str(b?.name, 24);
    if (id && name) out.push({ id, name, hz: num(b.hz) ?? 0, lo: num(b.lo) ?? 0, hi: num(b.hi) ?? 0 });
  }
  return out;
}

/** ★★ V12 The map's band keys — the web's sondeRanges (web/client/src/sondes.ts), the same words. */
export function sondeRanges(bands: SondeBand[]): Array<{ id: string; label: string; words: string }> {
  return bands.map((b) => ({ id: b.id, label: b.name.replace(' MHz', ''), words: `Search ${b.name}` }));
}

/** "403.300 MHz" — the frequency the sonde was heard on (to the kHz). */
export function sondeMHz(hz: number): string { return `${(Math.round(hz / 1e3) / 1e3).toFixed(3)} MHz`; }
/** "12 s ago" / "4 min ago" / "2 h ago". */
export function sondeAge(s: number): string {
  const v = Math.max(0, Math.round(s));
  return v < 60 ? `${v} s ago` : v < 3600 ? `${Math.round(v / 60)} min ago` : `${Math.floor(v / 3600)} h ago`;
}
/** "+5.2 m/s" climbing, "−14.0 m/s" falling. */
export function sondeClimb(v?: number): string {
  if (v == null) return '';
  return `${v >= 0 ? '+' : '−'}${Math.abs(v).toFixed(1)} m/s`;
}
/** ★ beta96 the SNR scale for a sonde's type (src/utils/snrColour.ts: each type's own threshold, read off test-rs41 /
 *  test-sondes-types). */
export function sondeSnrMode(s: Sonde): 'rs41' | 'dfm' | 'm10' | 'm20' | null {
  const k = (s.kind || 'RS41').toUpperCase();
  return k === 'RS41' ? 'rs41' : k === 'DFM' ? 'dfm' : k === 'M10' ? 'm10' : k === 'M20' ? 'm20' : null;
}
/** The sonde's type as the list shows it: the finer model where the server knows it ("DFM-09", "M10+"), else the kind. */
export function sondeType(s: Sonde): string { return s.model || s.kind || 'RS41'; }
/** The list's first line: type · serial · altitude · climb. */
export function sondeLine(s: Sonde): string {
  const parts = [`${sondeType(s)} ${s.serial}`];
  if (s.alt != null) parts.push(`${Math.round(s.alt).toLocaleString('en-GB')} m`);
  if (s.climb != null) parts.push(sondeClimb(s.climb));
  return parts.join(' · ');
}
/** The temperature, or how far the calibration has got — never a guess. */
export function sondeTemp(s: Sonde): string {
  if (s.temp != null) return `${s.temp < 0 ? '\u2212' : ''}${Math.abs(s.temp).toFixed(1)} \u00b0C`;
  if (!s.calOf) return 'temperature not decoded for this type';
  return s.cal >= s.calOf ? 'temperature not decoded' : `not yet calibrated (${s.cal} of ${s.calOf})`;
}
/** The second line: where it was heard, its state, the temperature (or the calibration's progress). */
export function sondeDetail(s: Sonde): string {
  const parts = [sondeMHz(s.hz)];
  if (s.enc) parts.push('encrypted (military RS41-SGM) — no position');
  else if (s.lat == null) parts.push('no GPS fix yet');
  else if (s.climb != null && s.climb < -2 && s.maxAlt != null && s.alt != null && s.maxAlt - s.alt > 300) parts.push(`descending — burst near ${Math.round(s.maxAlt).toLocaleString('en-GB')} m`);
  if (!s.enc) parts.push(sondeTemp(s));
  return parts.join(' · ');
}
/** The status line: the band, how many, the gain. */
export function sondeStatus(r: SondeReport | null): string {
  if (!r) return 'taking the receiver to the radiosonde band…';
  const n = r.sondes.length;
  return `${r.bandName} · ${n} sonde${n === 1 ? '' : 's'} · listening on ${r.chans.length} signal${r.chans.length === 1 ? '' : 's'}`
    + (r.gain != null ? ` · gain ${r.gain.toFixed(1)} dB${r.gainAuto ? ' auto' : ''}` : '') + (r.dropped ? ' · fell behind' : '');
}
/** What the empty list says. */
export function sondeEmpty(r: SondeReport | null): string {
  if (!r) return 'Taking the receiver to the radiosonde band…';
  const span = r.lo && r.hi ? `${(r.lo / 1e6).toFixed(1)}–${(r.hi / 1e6).toFixed(1)} MHz` : r.bandName;
  return `Searching ${span} for radiosondes (RS41, DFM, M10 and M20 — the type is found for each). Weather services launch them twice a day (around 11:15 and 23:15 UTC in `
    + 'much of Europe) and each is heard for one to three hours; an aerial for 400 MHz with a clear view of the sky hears them '
    + 'furthest. Step the band if your nearest launch site uses another frequency.';
}

function esc(v: unknown): string {
  return String(v ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string));
}
/** ★ Falling = the climb rate below −2 m/s: after the balloon burst, on its way down. */
export function sondeFalling(s?: Sonde): boolean { return !!s && s.climb != null && s.climb < -2; }
/** A balloon: falling ones orange, pointing down (the climb rate's sign is the one thing a glance must get). */
export function sondeIcon(s?: Sonde): { html: string; w: number; h: number } {
  const down = sondeFalling(s);
  const fill = down ? '#ff8a4d' : '#7fd4ff';
  return { html: `<svg width="20" height="22" viewBox="0 0 20 22" aria-hidden="true"><ellipse cx="10" cy="8" rx="6.5" ry="7.2" fill="${fill}" stroke="#000" stroke-width="0.9"/>`
    + `<path d="M10 15.2v4.3" stroke="#000" stroke-width="1"/><rect x="7.6" y="19" width="4.8" height="2.6" fill="#fff" stroke="#000" stroke-width="0.7"/>`
    + (down ? '<path d="M10 4.5v6M7.4 8.3 10 11l2.6-2.7" stroke="#000" stroke-width="1.2" fill="none"/>' : '') + '</svg>', w: 20, h: 22 };
}
/** The map's card (HTML, escaped). */
export function sondeCard(s: Sonde): string {
  const pos = s.lat != null && s.lon != null
    ? `<br>${s.lat.toFixed(5)}, ${s.lon.toFixed(5)}${s.speed != null ? ` \u00b7 ${s.speed.toFixed(1)} m/s towards ${Math.round(s.heading ?? 0)}\u00b0` : ''}` : '';
  return `<b>${esc(sondeLine(s))}</b><br>${esc(sondeDetail(s))}${pos}<br><small>${s.n} frame${s.n === 1 ? '' : 's'} \u00b7 last ${esc(sondeAge(s.age))}`
    + `${s.sats != null ? ` \u00b7 ${s.sats} satellites` : ''}</small>`;
}
/** A sonde's track, [lat, lon] oldest first (the map's trail — from the report, never a request). */
export function sondeTrail(r: SondeReport | null, key: string): [number, number][] {
  const s = r?.sondes.find((x) => x.serial === key);
  return s ? s.track.map((p) => [p[0], p[1]] as [number, number]) : [];
}
/** The app's map (MapOverlay 'sondes', the shared track layer): the sondes with a position. */
export function sondeTrackItems(r: SondeReport | null): TrackItem[] {
  if (!r) return [];
  return r.sondes.filter((s) => s.lat != null && s.lon != null)
    .map((s) => ({ key: s.serial, lat: s.lat as number, lon: s.lon as number, _ic: sondeIcon(s), _lb: s.serial, _cd: sondeCard(s) }));
}
const COMPASS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
/** The app's map list (TrackLiveList): nearest first where the receiver's place is known, else the newest. */
export function sondeTrackRows(r: SondeReport | null, rx: LatLon | null): TrackRow[] {
  if (!r) return [];
  const rows = r.sondes.map((s) => ({ s, rb: rx && s.lat != null && s.lon != null ? rangeBearing(rx, s.lat, s.lon) : null }));
  if (rx) rows.sort((a, b) => (a.rb && b.rb ? a.rb.km - b.rb.km : a.rb ? -1 : b.rb ? 1 : a.s.age - b.s.age));
  return rows.map(({ s, rb }) => ({
    key: s.serial, colour: sondeFalling(s) ? '#ff8a4d' : '#7fd4ff', flag: '', name: sondeLine(s), second: sondeDetail(s),
    sp: s.speed != null ? `${s.speed.toFixed(0)} m/s` : '', trend: (s.climb == null ? 0 : s.climb > 1 ? 1 : s.climb < -1 ? -1 : 0) as -1 | 0 | 1,
    range: rb ? `${rb.km < 10 ? rb.km.toFixed(1) : Math.round(rb.km)} km` : s.lat == null ? 'no position' : '',
    brg: rb ? COMPASS[Math.round(rb.brg / 45) % 8] : '', noPos: s.lat == null, age: sondeAge(s.age), em: false,
    ...(s.snr != null && sondeSnrMode(s) ? { snr: s.snr, snrMode: sondeSnrMode(s)! } : {}),   // ★ beta96 the last frame's SNR
  }));
}
