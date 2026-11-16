/**
 * sondes.ts — ★ V12 RADIOSONDES (docs/v12/RS41.md): the words and shapes both clients use for a VibeServer's SONDES
 * whole-radio mode ({"type":"sondes"}: RS41 weather balloons on 400–406 MHz). One copy (the app's SondesPanel and the
 * web's sondes.ts), so every screen says the same thing.
 * ★ NEVER A GUESSED PTU: the server sends `temp` only once the sonde's whole calibration (51 subframes) is in; until then
 *   the line says how much has arrived. Humidity and pressure are not decoded at all.
 */

/** One sonde from the report (vibe_rs41_service.h Tracker::json). */
export interface Sonde {
  serial: string; kind: string; hz: number; txHz?: number;
  age: number; n: number; frame: number; cal: number;
  lat?: number; lon?: number; alt?: number; climb?: number; speed?: number; heading?: number; sats?: number;
  posAge?: number; maxAlt?: number; temp?: number; batt?: number; enc?: boolean; gpsTime?: number;
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
    sondes.push({
      serial, kind: str(s.kind, 10) ?? 'RS41', hz: num(s.hz) ?? 0, txHz: num(s.txHz),
      age: num(s.age) ?? 0, n: num(s.n) ?? 0, frame: num(s.frame) ?? -1, cal: Math.max(0, Math.min(51, num(s.cal) ?? 0)),
      lat: la != null && lo != null ? la : undefined, lon: la != null && lo != null ? lo : undefined,
      alt: num(s.alt), climb: num(s.climb), speed: num(s.speed), heading: num(s.heading), sats: num(s.sats),
      posAge: num(s.posAge), maxAlt: num(s.maxAlt),
      temp: num(s.cal) === 51 ? num(s.temp) : undefined,   // ★ a temperature with an incomplete calibration is not shown
      batt: num(s.batt), enc: s.enc === true, gpsTime: num(s.gpsTime), track,
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
/** The list's first line: serial · altitude · climb. */
export function sondeLine(s: Sonde): string {
  const parts = [s.serial];
  if (s.alt != null) parts.push(`${Math.round(s.alt).toLocaleString('en-GB')} m`);
  if (s.climb != null) parts.push(sondeClimb(s.climb));
  return parts.join(' · ');
}
/** The temperature, or how far the calibration has got — never a guess. */
export function sondeTemp(s: Sonde): string {
  if (s.temp != null) return `${s.temp.toFixed(1)} °C`;
  return s.cal >= 51 ? 'temperature not decoded' : `not yet calibrated (${s.cal} of 51)`;
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
  return `Searching ${span} for RS41 radiosondes. Weather services launch them twice a day (around 11:15 and 23:15 UTC in `
    + 'much of Europe) and each is heard for one to three hours; an aerial for 400 MHz with a clear view of the sky hears them '
    + 'furthest. Step the band if your nearest launch site uses another frequency.';
}
