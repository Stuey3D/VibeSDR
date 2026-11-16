/**
 * aviation.ts — ★ V12 AVIATION (docs/v12/AVIATION.md): the words and shapes both clients use for a VibeServer's own
 * aircraft decoders — HFDL on a listener's USB dial (ext 'hfdl', vibe_hfdl_run.h) and the Aircraft datalink whole-radio
 * mode (ACARS / VDL2, {"type":"avdata"}). One copy (the app and the web's main.ts / avdata.ts), so every screen says the
 * same thing.
 */

/** ★ HFDL: where it is heard. A VibeServer decodes the listener's OWN dial (an UberSDR's add-on decodes every channel). */
export const HFDL_HINT = 'HFDL: aircraft on HF. Tune an HFDL frequency in USB — the published one, e.g. 8977 or 6535 kHz '
  + '(the carrier sits 1440 Hz above it) — and the aircraft this receiver hears go on the HFDL MAP: flight, position and '
  + 'the ground station it was talking to.';

/** The status line from {"type":"hfdl_status","data":{msgs, bursts, detections}}. */
export function hfdlStatusText(d: unknown): string {
  const o = (d && typeof d === 'object' ? d : {}) as Record<string, unknown>;
  const n = (k: string) => (typeof o[k] === 'number' && isFinite(o[k] as number) ? Math.max(0, Math.round(o[k] as number)) : 0);
  const msgs = n('msgs'), bursts = n('bursts');
  if (!msgs && !bursts) return 'listening for HFDL';
  return `${msgs} message${msgs === 1 ? '' : 's'} · ${bursts} burst${bursts === 1 ? '' : 's'} heard`;
}

/** One aircraft from the datalink mode's report (vibe_avi_service.h Tracker::json). */
export interface AvAircraft {
  icao?: string; reg?: string; flight?: string;
  lat?: number; lon?: number; posAge?: number;
  label?: string; labelName?: string; text?: string;
  level: number; n: number; age: number; via: string;
}
/** A band the server's datalink mode steps through (hwinfo / vibeserver.json avdataBands). */
export interface AvBand { id: string; name: string; region: string; hz: number; channels: number }
/** {"type":"avdata", band, bandName, hz, msgs, aircraft, dropped, gain?, gainAuto?} */
export interface AvReport { band: string; bandName: string; hz: number; msgs: number; dropped: number; aircraft: AvAircraft[]; gain?: number; gainAuto?: number }

const str = (v: unknown, max = 40): string | undefined =>
  typeof v === 'string' && v.length ? v.replace(/[\u0000-\u001f\u007f]/g, '').slice(0, max) : undefined;
const num = (v: unknown): number | undefined => (typeof v === 'number' && isFinite(v) ? v : undefined);

/** Parse a report defensively (the server is not trusted to be well formed). Null = not one. */
export function parseAvReport(m: unknown): AvReport | null {
  const o = m as Record<string, unknown> | null;
  if (!o || o.type !== 'avdata' || !Array.isArray(o.aircraft)) return null;
  const aircraft: AvAircraft[] = [];
  for (const a0 of (o.aircraft as unknown[]).slice(0, 2000)) {
    const a = a0 as Record<string, unknown>;
    if (!a || typeof a !== 'object') continue;
    const lat = num(a.lat), lon = num(a.lon);
    aircraft.push({
      icao: str(a.icao, 6), reg: str(a.reg, 10), flight: str(a.flight, 10),
      lat: lat != null && Math.abs(lat) <= 90 ? lat : undefined, lon: lon != null && Math.abs(lon) <= 180 ? lon : undefined,
      posAge: num(a.posAge), label: str(a.label, 4), labelName: str(a.labelName, 60), text: str(a.text, 300),
      level: num(a.level) ?? -120, n: num(a.n) ?? 0, age: num(a.age) ?? 0, via: str(a.via, 60) ?? '',
    });
  }
  return { band: str(o.band, 10) ?? '', bandName: str(o.bandName, 20) ?? '', hz: num(o.hz) ?? 0, msgs: num(o.msgs) ?? 0,
           dropped: num(o.dropped) ?? 0, aircraft, gain: num(o.gain), gainAuto: num(o.gainAuto) };
}
export function parseAvBands(v: unknown): AvBand[] {
  if (!Array.isArray(v)) return [];
  const out: AvBand[] = [];
  for (const b0 of v.slice(0, 8)) {
    const b = b0 as Record<string, unknown>;
    const id = str(b?.id, 10), name = str(b?.name, 20);
    if (id && name) out.push({ id, name, region: str(b.region, 80) ?? '', hz: num(b.hz) ?? 0, channels: num(b.channels) ?? 0 });
  }
  return out;
}
/** The list's line for one aircraft: who, then what was last heard (never the body — that is its own field). */
export function avLine(a: AvAircraft): string {
  const who = [a.flight, a.reg, a.icao].filter(Boolean).join(' · ') || 'unknown aircraft';
  const what = a.labelName || (a.label ? `label ${a.label}` : '');
  return what ? `${who} — ${what}` : who;
}
/** "12 s ago" / "4 min ago". */
export function avAge(s: number): string {
  const v = Math.max(0, Math.round(s));
  return v < 60 ? `${v} s ago` : v < 3600 ? `${Math.round(v / 60)} min ago` : `${Math.floor(v / 3600)} h ago`;
}
