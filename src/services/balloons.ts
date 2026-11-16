/**
 * balloons.ts — V12 high-altitude balloons, the PURE half shared by the app and the web client (web/client/src/main.ts
 * imports it): the server's report cut to its fields, the picture frames, the status line, the words, and the attach
 * message for AUTO and the manual pins. The decoding is the server's (android/app/src/main/cpp/decoders/balloon_rx.h,
 * vibe_balloon_run.h; docs/v12/BALLOONS.md).
 *
 * Wire (decoder socket):
 *   {"type":"balloon","data":{now,state,rtty:{centre,shift,settings,candidates,rejected},horus:{tones|null,baud,packets},
 *                             payloads:[{call,src,count,time,lat,lon,alt,speed,sats,temp,batt,heard,n,fec,sentence,extra}…],
 *                             images:[{key,call,image,have,expected,complete,mcus,total}…]}}
 *   0x0B | u8 keyLen | key | u16 have | u16 expected | u8 complete | u16 w | u16 h | JPEG   — an SSDV picture so far
 *   + the text lines (one per report) and the state (0 listening · 1 a signal · 3 decoding).
 * ★ AUTO finds everything itself (any RTTY speed 50–600 Bd, 7 or 8 bits, either polarity, the shift; Horus's four tones).
 *   The MANUAL pins under it (Stuart's rule for automatic decoders) narrow the search when AUTO cannot settle.
 */

export interface BalloonPayload {
  call: string; src: 'ukhas' | 'horus'; count: number; time: string;
  lat: number | null; lon: number | null; alt: number | null; speed: number | null; sats: number | null; temp: number | null; batt: number | null;
  heard: number; n: number; fec: number; sentence: string; extra: string[];
}
export interface BalloonImageInfo { key: string; call: string; image: number; have: number; expected: number; complete: boolean; mcus: number; total: number }
export interface BalloonReport {
  now: number; state: number;
  rtty: { centre: number; shift: number; settings: string; candidates: number; rejected: number };
  horus: { tones: number[] | null; baud: number; packets: number };
  payloads: BalloonPayload[]; images: BalloonImageInfo[];
}
/** One SSDV picture frame (0x0B). */
export interface BalloonPicture { key: string; have: number; expected: number; complete: boolean; w: number; h: number; jpeg: Uint8Array }

const num = (v: unknown, d = 0) => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const numOrNull = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : null);
const str = (v: unknown, n: number) => (typeof v === 'string' ? v.slice(0, n) : '');

/** ★ What a report may carry, cut to its fields (a broken server must not put an object where a number goes). */
export function balloonSanitise(d: any): BalloonReport | null {
  if (!d || typeof d !== 'object') return null;
  const r = d.rtty && typeof d.rtty === 'object' ? d.rtty : {};
  const h = d.horus && typeof d.horus === 'object' ? d.horus : {};
  const tones = Array.isArray(h.tones) && h.tones.length === 4 && h.tones.every((t: unknown) => typeof t === 'number' && Number.isFinite(t)) ? h.tones.slice(0, 4) : null;
  const payloads: BalloonPayload[] = Array.isArray(d.payloads) ? d.payloads.slice(0, 12).filter((p: any) => p && typeof p === 'object' && typeof p.call === 'string' && p.call).map((p: any) => {
    const lat = numOrNull(p.lat), lon = numOrNull(p.lon);
    const okPos = lat !== null && lon !== null && Math.abs(lat) <= 90 && Math.abs(lon) <= 180;
    return {
      call: str(p.call, 20), src: p.src === 'horus' ? 'horus' : 'ukhas', count: num(p.count, -1), time: str(p.time, 8),
      lat: okPos ? lat : null, lon: okPos ? lon : null, alt: numOrNull(p.alt), speed: numOrNull(p.speed), sats: numOrNull(p.sats),
      temp: numOrNull(p.temp), batt: numOrNull(p.batt), heard: num(p.heard), n: Math.max(0, num(p.n)), fec: Math.max(0, num(p.fec)),
      sentence: str(p.sentence, 200), extra: Array.isArray(p.extra) ? p.extra.slice(0, 16).map((x: unknown) => str(x, 24)) : [],
    } as BalloonPayload;
  }) : [];
  const images: BalloonImageInfo[] = Array.isArray(d.images) ? d.images.slice(0, 4).filter((i: any) => i && typeof i.key === 'string').map((i: any) => ({
    key: str(i.key, 40), call: str(i.call, 10), image: num(i.image), have: Math.max(0, num(i.have)), expected: Math.max(0, num(i.expected)),
    complete: i.complete === true, mcus: Math.max(0, num(i.mcus)), total: Math.max(0, num(i.total)),
  })) : [];
  const st = num(d.state);
  return {
    now: num(d.now), state: st === 1 || st === 3 ? st : 0,
    rtty: { centre: num(r.centre), shift: num(r.shift), settings: str(r.settings, 60), candidates: num(r.candidates), rejected: num(r.rejected) },
    horus: { tones, baud: num(h.baud, 100), packets: num(h.packets) },
    payloads, images,
  };
}

/** ★ A 0x0B frame → the picture, or null when it is not one (any length checked before it is read). */
export function balloonPicture(buf: ArrayBuffer | Uint8Array): BalloonPicture | null {
  const b = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  if (b.length < 12 || b[0] !== 0x0b) return null;
  const kl = b[1];
  if (b.length < 2 + kl + 9 + 4) return null;
  let key = '';
  for (let i = 0; i < kl; i++) key += String.fromCharCode(b[2 + i]);
  const p = 2 + kl;
  const jpeg = b.subarray(p + 9);
  if (jpeg[0] !== 0xff || jpeg[1] !== 0xd8) return null;   // ★ only a JPEG is ever drawn
  return { key, have: b[p] << 8 | b[p + 1], expected: b[p + 2] << 8 | b[p + 3], complete: b[p + 4] !== 0, w: b[p + 5] << 8 | b[p + 6], h: b[p + 7] << 8 | b[p + 8], jpeg };
}

/** ★ What the box keeps: every payload seen since it opened, by callsign, the newest report of each, newest heard first
 *  (≤ 12). A re-attach (a pin changed) starts a fresh run on the server, whose list begins empty — the rows must not vanish. */
export function balloonMerge(prev: readonly BalloonPayload[], r: BalloonReport | null): BalloonPayload[] {
  const m = new Map<string, BalloonPayload>();
  for (const p of prev) m.set(p.call, p);
  if (r) for (const p of r.payloads) { const o = m.get(p.call); if (!o || p.heard >= o.heard) m.set(p.call, p); }
  return [...m.values()].sort((a, b) => b.heard - a.heard).slice(0, 12);
}
/** The status line. */
export function balloonStatus(r: BalloonReport | null): string {
  if (!r) return 'listening for a balloon…';
  const parts: string[] = [];
  if (r.rtty.settings) parts.push(`RTTY ${r.rtty.settings}`);
  else if (r.rtty.shift > 0 && r.state >= 1) parts.push(`RTTY tones ${Math.round(r.rtty.shift)} Hz apart — working out its settings…`);
  if (r.horus.tones) parts.push(`Horus 4-FSK (${Math.round((r.horus.tones[3] - r.horus.tones[0]) / 3)} Hz spacing)`);
  if (!parts.length) return r.state ? 'a signal — working out what it is…' : 'listening — tune a payload in USB, its tones in the passband';
  return (r.state >= 3 ? 'decoding · ' : '') + parts.join(' · ');
}
/** "52.20601, 0.11950 · 31 050 m · 9 sats · −12 °C" — one payload's line of numbers. */
export function balloonLine(p: BalloonPayload): string {
  const bits: string[] = [];
  bits.push(p.lat !== null && p.lon !== null ? `${p.lat.toFixed(5)}, ${p.lon.toFixed(5)}` : 'no GPS fix');
  if (p.alt !== null) bits.push(`${Math.round(p.alt).toLocaleString('en-GB')} m`);
  if (p.speed !== null && p.src === 'horus') bits.push(`${Math.round(p.speed)} km/h`);
  if (p.sats !== null) bits.push(`${p.sats} sats`);
  if (p.temp !== null) bits.push(`${Math.round(p.temp)} °C`);
  if (p.batt !== null) bits.push(`${p.batt.toFixed(2)} V`);
  return bits.join(' · ');
}
/** "#123 · 10:00:05 · heard 12 s ago" */
export function balloonWhen(p: BalloonPayload, now: number): string {
  const ago = Math.max(0, Math.round(now - p.heard));
  const a = ago < 60 ? `${ago} s` : ago < 3600 ? `${Math.round(ago / 60)} min` : `${Math.floor(ago / 3600)} h`;
  return `#${p.count}${p.time ? ' · ' + p.time : ''} · heard ${a} ago${p.src === 'horus' ? ' · Horus' : ' · UKHAS'}`;
}
/** A picture's progress: "VIBE1 picture 7 · 12 of 30 packets" / "… complete". */
export function balloonPictureText(i: BalloonImageInfo): string {
  return `${i.call} picture ${i.image} · ${i.complete ? 'complete' : `${i.have}${i.expected ? ' of ' + i.expected : ''} packets`}`;
}

export const BALLOON_HINT = 'Tune the payload in USB with its tones inside the passband (most UK flights are on 434.000–434.650 MHz). '
  + 'Everything is found by itself: UKHAS RTTY telemetry at any speed from 50 to 600 baud, Horus Binary 4-FSK, and SSDV pictures, '
  + 'which build up here as their packets arrive. A report is only shown once its checksum is right. '
  + 'If AUTO cannot settle on a weak or unusual signal, pin what you know below.';

// ── the manual pins under AUTO ──
export interface BalloonSettings { baud: number; bits: number; reverse: number; shift: number; horusSpacing: number }
export const BALLOON_AUTO: BalloonSettings = { baud: 0, bits: 0, reverse: -1, shift: 0, horusSpacing: 0 };
export const BALLOON_BAUDS = [0, 50, 75, 100, 150, 300, 600] as const;
export const BALLOON_BITS = [0, 7, 8] as const;
export const BALLOON_POLARITY = [-1, 0, 1] as const;
export const BALLOON_SHIFTS = [0, 350, 425, 600, 850] as const;
export const BALLOON_SPACINGS = [0, 244, 270] as const;
export const balloonPinLabel = (k: keyof BalloonSettings, v: number): string =>
  k === 'reverse' ? (v === 1 ? 'REV' : v === 0 ? 'NORM' : 'AUTO')   // ★ polarity: −1 AUTO · 0 normal · 1 reversed
  : v === 0 ? 'AUTO' : k === 'baud' ? `${v}` : k === 'bits' ? `${v}-bit` : `${v} Hz`;
/** ★ The attach message's parameters: only what is pinned (absent = AUTO, which is what an older client sends too). */
export function balloonParams(s: BalloonSettings): Record<string, number> {
  const o: Record<string, number> = {};
  if (s.baud > 0) o.baud_rate = s.baud;
  if (s.bits === 7 || s.bits === 8) o.bits = s.bits;
  if (s.reverse === 0 || s.reverse === 1) o.reverse = s.reverse;
  if (s.shift > 0) o.shift = s.shift;
  if (s.horusSpacing > 0) o.horus_spacing = s.horusSpacing;
  return o;
}
export const balloonIsAuto = (s: BalloonSettings) => Object.keys(balloonParams(s)).length === 0;
