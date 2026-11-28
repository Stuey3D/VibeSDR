/**
 * aviation.ts — ★ V12 AVIATION (docs/v12/AVIATION.md): the words and shapes both clients use for a VibeServer's own
 * aircraft decoders — HFDL on a listener's USB dial (ext 'hfdl', vibe_hfdl_run.h) and the Aircraft datalink whole-radio
 * mode (ACARS / VDL2, {"type":"avdata"}). One copy (the app and the web's main.ts / avdata.ts), so every screen says the
 * same thing.
 * ★ V12 2026-11-16 the ACARS APPLICATIONS (vibe_acars_apps.h): a row may carry a position the server is SURE of (ADS-C,
 *   an FMS report with its CRC, a fixed airline layout) with its altitude and the time of the fix as reported, the
 *   airports and the last OUT / OFF / ON / IN. These are flight data, like the registration; message text stays the
 *   owner's opt-in (`text`). CPDLC is only ever NAMED (`app`).
 */
import { rangeBearing, type LatLon, type TrackItem, type TrackRow } from './tracks';

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
  /** ★ the applications: feet; "ADS-C" | "FMS" | "ACARS" | "HFDL"; the fix time AS REPORTED ("21:57:54", ":06:36" = past the hour) */
  alt?: number; posSrc?: string; posTime?: string;
  /** ★★ 11-28 a heading the server DERIVED from the aircraft's last two fixes of one source ("trkSrc":"derived" — it turns
   *  the icon only, never a track to fly on); undefined from an older server */
  trk?: number;
  from?: string; to?: string; oooi?: string; oooiTime?: string; app?: string;
  label?: string; labelName?: string; text?: string;
  level: number; n: number; age: number; via: string;
  /** ★ beta96 the LAST message's SNR (dB) and its scale key ("acars" "vdl2" "hfdl300"… — src/utils/snrColour.ts); only
   *  for a message the aircraft sent; undefined from an older server */
  snr?: number; sm?: string;
}
/** A band the server's datalink mode steps through (hwinfo / vibeserver.json avdataBands). */
export interface AvBand { id: string; name: string; region: string; hz: number; channels: number; chLo?: number; chHi?: number;
  /** ★★ V12 every channel the band decodes (Hz), from the server's own table; absent = an older server */
  chans?: number[] }   // ★ beta96 chLo/chHi: first / last channel (Hz)
/** {"type":"avdata", band, bandName, hz, msgs, aircraft, dropped, gain?, gainAuto?, gainNote?, rateNote?}
 *  ★ V12 2026-11-24 `gainNote`: on a radio the mode sets no gain on (the Airspy HF+) what its front end IS doing, in place
 *    of a dB figure ("HF+ AGC · attenuator 12 dB"); `rateNote`: the capture was raised for ACARS + VDL2 (the owner's 8 MS/s
 *    switch) and will be given back. */
export interface AvReport { band: string; bandName: string; hz: number; msgs: number; dropped: number;
  /** ★★ beta96 messages in the last minute (the server's rolling 60 s, AIS's rule); undefined = an older server */
  msgRate?: number; aircraft: AvAircraft[]; gain?: number; gainAuto?: number;
  gainNote?: string; rateNote?: string;
  /** ★ what the applications read: positions, OOOI events, ADS-C and CPDLC messages, and those with text no layout fitted */
  apps?: { pos: number; oooi: number; adsc: number; cpdlc: number; unparsed: number } }

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
    const lat = num(a.lat), lon = num(a.lon), alt = num(a.alt);
    const icao4 = (v: unknown) => (typeof v === 'string' && /^[A-Z]{4}$/.test(v) ? v : undefined);   // ★ whole, never cut down to fit
    const oooi = str(a.oooi, 4);
    aircraft.push({
      icao: str(a.icao, 6), reg: str(a.reg, 10), flight: str(a.flight, 10),
      lat: lat != null && Math.abs(lat) <= 90 ? lat : undefined, lon: lon != null && Math.abs(lon) <= 180 ? lon : undefined,
      posAge: num(a.posAge),
      alt: alt != null && alt > -2000 && alt < 70000 ? alt : undefined, posSrc: str(a.posSrc, 8), posTime: str(a.posTime, 9),
      trk: a.trkSrc === 'derived' && typeof a.trk === 'number' && a.trk >= 0 && a.trk < 360 ? a.trk : undefined,
      from: icao4(a.from) && icao4(a.to) ? icao4(a.from) : undefined, to: icao4(a.from) && icao4(a.to) ? icao4(a.to) : undefined, oooi: oooi && /^(out|off|on|in)$/.test(oooi) ? oooi : undefined, oooiTime: str(a.oooiTime, 5),
      app: str(a.app, 10), label: str(a.label, 4), labelName: str(a.labelName, 60), text: str(a.text, 300),
      level: num(a.level) ?? -120, n: num(a.n) ?? 0, age: num(a.age) ?? 0, via: str(a.via, 60) ?? '',
      ...(num(a.snr) != null && str(a.sm, 10) ? { snr: num(a.snr), sm: str(a.sm, 10) } : {}),
    });
  }
  const ap = (o.apps && typeof o.apps === 'object' ? o.apps : null) as Record<string, unknown> | null;
  const apps = ap ? { pos: num(ap.pos) ?? 0, oooi: num(ap.oooi) ?? 0, adsc: num(ap.adsc) ?? 0, cpdlc: num(ap.cpdlc) ?? 0, unparsed: num(ap.unparsed) ?? 0 } : undefined;
  return { band: str(o.band, 10) ?? '', bandName: str(o.bandName, 20) ?? '', hz: num(o.hz) ?? 0, msgs: num(o.msgs) ?? 0, msgRate: num(o.msgRate),
           dropped: num(o.dropped) ?? 0, aircraft, gain: num(o.gain), gainAuto: num(o.gainAuto),
           gainNote: str(o.gainNote, 60), rateNote: str(o.rateNote, 120), apps };
}
/** ★★ beta96 (Stuart 11-26: "a messages per minute like ais … it will allow me to check antenna performance quickly"): the
 *  status line's rate — " · 12 msg/min", AIS's words and rounding (tracks.ts aisStatus); '' from an older server. */
export function avRateText(r: AvReport | null): string {
  const m = r?.msgRate;
  return m == null || !isFinite(m) ? '' : ` · ${m.toFixed(m < 10 ? 1 : 0)} msg/min`;
}
/** ★ The status line's gain part — the same words on the web and in the app: " · gain 43.4 dB auto", or the radio's own
 *  state where the mode sets no gain (" · HF+ AGC"); then " · capture raised to 8 MS/s …" while it is. */
export function avGainText(r: AvReport): string {
  return (r.gain != null ? ` · gain ${r.gain.toFixed(1)} dB${r.gainAuto ? ' auto' : ''}` : r.gainNote ? ` · ${r.gainNote}` : '')
    + (r.rateNote ? ` · ${r.rateNote}` : '');
}
export function parseAvBands(v: unknown): AvBand[] {
  if (!Array.isArray(v)) return [];
  const out: AvBand[] = [];
  for (const b0 of v.slice(0, 8)) {
    const b = b0 as Record<string, unknown>;
    const id = str(b?.id, 10), name = str(b?.name, 20);
    if (id && name) {
      const lo = num(b.chLo), hi = num(b.chHi);
      const chans = Array.isArray(b.chans) ? (b.chans as unknown[]).slice(0, 64).map((c) => num(c)).filter((c): c is number => !!c && c > 0) : [];
      out.push({ id, name, region: str(b.region, 80) ?? '', hz: num(b.hz) ?? 0, channels: num(b.channels) ?? 0, ...(lo && hi ? { chLo: lo, chHi: hi } : {}), ...(chans.length ? { chans } : {}) });
    }
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

/** ★ The position's line: "37,000 ft · fix 21:57:54 UTC · FMS" (":06:36" = minutes and seconds past the hour, as ADS-C
 *  sends it). Empty without a position. */
export function avFix(a: AvAircraft): string {
  if (a.lat == null || a.lon == null) return '';
  const t = a.posTime ? (a.posTime.startsWith(':') ? `fix at ${a.posTime} past the hour` : `fix ${a.posTime} UTC`) : '';
  return [a.alt != null ? `${Math.round(a.alt).toLocaleString('en-GB')} ft` : '', t, a.posSrc ?? ''].filter(Boolean).join(' · ');
}
/** ★ The route and the last OOOI event: "EGLL → EDDF · OUT 20:30 UTC". Empty when neither was heard. */
export function avRoute(a: AvAircraft): string {
  const r = a.from && a.to ? `${a.from} \u2192 ${a.to}` : '';
  const o = a.oooi ? `${a.oooi.toUpperCase()}${a.oooiTime ? ` ${a.oooiTime} UTC` : ''}` : '';
  return [r, o].filter(Boolean).join(' \u00b7 ');
}
/** The detail line under the list's line: route / OOOI, the fix, then how it was heard. */
export function avDetail(a: AvAircraft): string {
  return [avRoute(a), avFix(a), a.app === 'CPDLC' ? 'CPDLC' : '', a.via, a.n > 1 ? `${a.n} messages` : ''].filter(Boolean).join(' \u00b7 ');
}
/** The status line's tail: how many messages the applications read ("3 positions · 1 OOOI"), or ''. */
export function avAppsText(r: AvReport | null): string {
  const p = r?.apps;
  if (!p) return '';
  const w = (n: number, one: string, many: string) => (n ? `${n} ${n === 1 ? one : many}` : '');
  return [w(p.pos + p.adsc, 'position message', 'position messages'), w(p.oooi, 'OOOI', 'OOOI'), w(p.cpdlc, 'CPDLC', 'CPDLC')].filter(Boolean).join(' \u00b7 ');
}

function esc(v: unknown): string {
  return String(v ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string));
}
export const avKey = (a: AvAircraft, i: number): string => a.icao || a.reg || a.flight || `row${i}`;
/** The map's aircraft (both clients: the web's TrackView, the app's MapOverlay WebView). */
export function avIcon(): { html: string; w: number; h: number } {
  return { html: '<svg width="20" height="20" viewBox="0 0 24 24" aria-hidden="true"><path fill="#ffd34d" stroke="#000" stroke-width="0.8" d="M12 2l1.6 7.2 7.4 3.3v1.9l-7.4-1.6-.4 5.2 2.3 1.7V21L12 20l-3.5 1v-1.3l2.3-1.7-.4-5.2L3 14.4v-1.9l7.4-3.3z"/></svg>', w: 20, h: 20 };
}
/** The map's card (HTML, escaped): who and what, the route, the fix, how it was heard; the body only where the server sent it. */
export function avCard(a: AvAircraft): string {
  const route = avRoute(a), fix = avFix(a);
  return `<b>${esc(avLine(a))}</b>${route ? `<br>${esc(route)}` : ''}${fix ? `<br>${esc(fix)}` : ''}`
    + `<br>${esc(a.via)} \u00b7 ${esc(avAge(a.age))}${a.text ? `<br><small>${esc(a.text)}</small>` : ''}`;
}
/** The app's map (MapOverlay 'avdata', the shared track layer): the aircraft that reported a position. */
export function avTrackItems(r: AvReport | null): TrackItem[] {
  if (!r) return [];
  return r.aircraft.map((a, i) => ({ a, key: avKey(a, i) })).filter(({ a }) => a.lat != null && a.lon != null)
    .map(({ a, key }) => ({ key, lat: a.lat as number, lon: a.lon as number, _ic: avIcon(), _lb: a.flight || a.reg || a.icao || '', _cd: avCard(a) }));
}
const COMPASS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
/** The app's map list: the newest first (the server's order) — a datalink row is about what was just heard. */
export function avTrackRows(r: AvReport | null, rx: LatLon | null): TrackRow[] {
  if (!r) return [];
  return r.aircraft.map((a, i) => {
    const rb = rx && a.lat != null && a.lon != null ? rangeBearing(rx, a.lat, a.lon) : null;
    return {
      key: avKey(a, i), colour: '#ffd34d', flag: '', name: avLine(a), second: avDetail(a),
      sp: a.alt != null ? `FL${String(Math.max(0, Math.round(a.alt / 100))).padStart(3, '0')}` : '', trend: 0 as const,
      // ★ beta95 NM on the aviation map (Stuart 11-25: the datalink list said km, its card and the rings NM)
      range: rb ? `${rb.nm < 10 ? rb.nm.toFixed(1) : Math.round(rb.nm)} NM` : a.lat == null ? 'no position' : '',
      brg: rb ? COMPASS[Math.round(rb.brg / 45) % 8] : '', noPos: a.lat == null, age: avAge(a.age), em: false,
    };
  });
}
/** What the empty map list says. */
export const AV_MAP_EMPTY = 'No aircraft yet. Most ACARS and VDL2 messages carry no position; the ones that do (ADS-C, FMS '
  + 'position reports, some airlines\' own reports) put the aircraft here.';
