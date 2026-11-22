/**
 * satellites.ts — ★ V12 SATELLITE ENGINE (docs/v12/SATELLITE-ENGINE.md): the words and shapes both clients use for a
 * VibeServer's satellite picker and Doppler lock. One copy (the app's SatellitePanel and the web's satellites.ts), so
 * every screen says the same thing.
 *
 * Wire (server → client):
 *   {"type":"sat_list", time, located, now, why?, source?:{orbits, transmitters?, via, fetched?, newestEpoch?}, sats:[{id,name,norad,note,age,ageState,tx:[{label,hz,hiHz?,mode,loop,
 *     consumer?,upHz?,ctcss?,note?,reach}],az?,el?,rangeKm?,lat?,lon?,passes:[{aos,tca,los,maxEl,aosAz,losAz}]}]}
 *   {"type":"sat", state:"OFF"|"ARMED"|"TRACK"|"COAST"|"ENDED", why?, shared?, id, name, norad, tx, loop, consumer?,
 *     nominal, offset, rate, trim, az, el, aos, los, maxEl, age, ageState, time}
 *   {"type":"sat_track", norad, aos, los, pts:[[az,el]…]}
 * Client → server: {"type":"sat_list"} · {"type":"sat_list","op":"track",norad,aos,los} · {"type":"sat","op":"lock",norad?}
 *   · {"type":"sat","op":"unlock"}.
 * hwinfo: `doppler` 1 (a version) where the radio reaches a listed downlink; `dopplerWhy` = greyed, with the reason.
 *
 * ★★ THE DIAL SHOWS THE NOMINAL (brief §10): the main digits stay on the published frequency; `offset` is where the
 *    channel actually is relative to it — the waterfall marker goes there (marker TRAP), bookmarks store the nominal.
 * ★ No flashing anywhere: COAST only dims the readout.
 */
import { tlmLivePush, type TlmLiveFrame } from '../utils/tlmCharts';
import { GLOBE_COAST } from './globeCoast';

export type SatLockState = 'OFF' | 'ARMED' | 'TRACK' | 'COAST' | 'ENDED';

export interface SatTx {
  label: string; hz: number; hiHz?: number; mode: string; loop: string; consumer?: string;
  upHz?: number; ctcss?: number; note?: string; reach: boolean;
  /** SatNOGS DB's word, via vibesdr.net's sats.json: "inactive" = listed but reported off. */
  status?: 'active' | 'inactive';
  /** ★ SATELLITE MODE contract: what the downlink is FOR — the box's size and the decoder the server attaches on a pick. */
  use?: SatUse;
  /** ★ SATELLITE MODE: the ISS 145.800 voice downlink also carries SSTV — the server runs SSTV on it; the box grows on a picture. */
  sstv?: boolean;
  /** Why this radio cannot hear it (reach false) — shown on the greyed row. */
  why?: string;
  /** ★ SATELLITE-BEST 1.4: a transponder's AMSAT band letters, uplink UPPER / downlink lower ("U/v", "V/a") — the server's. */
  band?: string;
  /** ★ SATELLITE-BEST 1.4: the catalogue's own name for the mode ("Mode A", "Mode B"), where it has one. */
  modeName?: string;
  /** ★ beta90: the decoder a pick attaches (the server's word — the same as picked.decoder); '' = heard only. */
  decoder?: SatPicked['decoder'];
}
export type SatUse = 'voice' | 'image' | 'packet';
export interface SatPass { aos: number; tca: number; los: number; maxEl: number; aosAz: number; losAz: number }
export interface SatInfo {
  id: string; name: string; norad: number; note: string; age: number; ageState: string;
  tx: SatTx[]; az?: number; el?: number; rangeKm?: number; lat?: number; lon?: number; passes: SatPass[];
  /** ★ SATELLITE MODE: seconds to LOS while the bird is up (absent when it is not). */
  inRangeFor?: number;
}
/** Where the data came from — the credit the picker shows (CelesTrak's orbits; SatNOGS's transmitter data where used). */
export interface SatSource {
  orbits: string; transmitters?: string; via: string; fetched?: number; newestEpoch?: number;
  /** ★ SATELLITE MODE: unix s of the server's last good /api/sats fetch (0 = never) and whether one is running now. */
  synced?: number; fetching?: boolean;
  /** ★★ SATELLITE-BEST 1.6: true = a pick here disturbs nobody (a single-user radio, or a locked range where each listener has
   *  their own channel) — only then is REMIND offered. false = a shared dial. Absent (an older server) = never offered: the
   *  client does not guess. */
  pickPrivate?: boolean;
}
export interface SatList { time: string; located: boolean; now: number; why?: string; source?: SatSource; sats: SatInfo[] }
export interface SatReadout {
  state: SatLockState; why?: string; shared?: boolean;
  id?: string; name?: string; norad?: number; tx?: string; loop?: string; consumer?: string;
  nominal?: number; offset?: number; rate?: number; trim?: number; az?: number; el?: number;
  aos?: number; los?: number; maxEl?: number; age?: number; ageState?: string; time: string;
}
export interface SatTrack {
  norad: number; aos: number; los: number; pts: [number, number][];
  /** ★ SATELLITE-BEST 1.7: the sub-satellite points [lat, lon, altKm] over the pass (AOS − 10 min … LOS + 10 min, evenly
   *  spaced in time), ≤ 120 — the dotted orbit on the half-globe; null where the server could not compute one (the pen
   *  lifts). Absent from an older server: no line is drawn. */
  geo?: GeoPt[];
  /** the geo index of the pass's highest point (from the server's tca, geoT0, geoStep), when it says */
  tcaIdx?: number;
}
export type GeoPt = [number, number, number] | null;

const str = (v: unknown, max = 80): string | undefined =>
  typeof v === 'string' && v.length ? v.replace(/[\u0000-\u001f\u007f]/g, '').slice(0, max) : undefined;
const num = (v: unknown): number | undefined => (typeof v === 'number' && isFinite(v) ? v : undefined);
const STATES: SatLockState[] = ['OFF', 'ARMED', 'TRACK', 'COAST', 'ENDED'];

export function parseSatList(m: unknown): SatList | null {
  const o = m as Record<string, unknown> | null;
  if (!o || o.type !== 'sat_list' || !Array.isArray(o.sats)) return null;
  const sats: SatInfo[] = [];
  for (const s0 of (o.sats as unknown[]).slice(0, 64)) {
    const s = s0 as Record<string, unknown>;
    const id = str(s?.id, 24), name = str(s?.name, 40), norad = num(s?.norad);
    if (!id || !name || norad == null) continue;
    const tx: SatTx[] = [];
    if (Array.isArray(s.tx))
      for (const t0 of (s.tx as unknown[]).slice(0, 8)) {
        const t = t0 as Record<string, unknown>;
        const label = str(t?.label, 40), hz = num(t?.hz);
        if (!label || hz == null || hz <= 0) continue;
        const hi = num(t.hiHz);
        tx.push({ label, hz, hiHz: hi != null && hi > hz ? hi : undefined, mode: str(t.mode, 12) ?? 'nfm', loop: str(t.loop, 8) ?? 'none',
                  consumer: str(t.consumer, 20), upHz: num(t.upHz), ctcss: num(t.ctcss), note: str(t.note, 120), reach: t.reach === true,
                  status: t.status === 'inactive' || t.status === 'active' ? t.status : undefined,
                  use: t.use === 'voice' || t.use === 'image' || t.use === 'packet' ? t.use : undefined,
                  sstv: t.sstv === true ? true : undefined, why: str(t.why, 160),
                  band: typeof t.band === 'string' && /^[A-Za-z]{1,2}\/[A-Za-z]{1,2}$/.test(t.band) ? t.band : undefined,
                  modeName: str(t.modeName, 24),
                  decoder: t.decoder === 'lrpt' || t.decoder === 'sstv' || t.decoder === 'aprs' || t.decoder === 'cw' || t.decoder === 'fctlm' || t.decoder === '' ? t.decoder : undefined });
      }
    const passes: SatPass[] = [];
    if (Array.isArray(s.passes))
      for (const p0 of (s.passes as unknown[]).slice(0, 8)) {
        const p = p0 as Record<string, unknown>;
        const aos = num(p?.aos), los = num(p?.los);
        if (aos == null || los == null || los <= aos) continue;
        passes.push({ aos, tca: num(p.tca) ?? (aos + los) / 2, los, maxEl: num(p.maxEl) ?? 0, aosAz: num(p.aosAz) ?? 0, losAz: num(p.losAz) ?? 0 });
      }
    const el = num(s.el), az = num(s.az);
    sats.push({ id, name, norad, note: str(s.note, 160) ?? '', age: num(s.age) ?? -1, ageState: str(s.ageState, 10) ?? 'none', tx,
                az: az != null && az >= 0 && az <= 360 ? az : undefined, el: el != null && Math.abs(el) <= 90 ? el : undefined,
                rangeKm: num(s.rangeKm), lat: num(s.lat), lon: num(s.lon), passes,
                inRangeFor: (() => { const v = num(s.inRangeFor); return v != null && v > 0 ? v : undefined; })() });
  }
  const so = o.source as Record<string, unknown> | undefined;
  const source: SatSource | undefined = so && typeof so === 'object' && (str(so.orbits, 120) || num(so.synced) != null || so.fetching === true)
    ? { orbits: str(so.orbits, 120) ?? '', transmitters: str(so.transmitters, 160), via: str(so.via, 20) ?? '', fetched: num(so.fetched), newestEpoch: num(so.newestEpoch),
        synced: num(so.synced), fetching: so.fetching === true,
        pickPrivate: so.pickPrivate === true ? true : so.pickPrivate === false ? false : undefined }
    : undefined;
  return { time: str(o.time, 8) ?? 'none', located: o.located === true, now: num(o.now) ?? Date.now() / 1000, why: str(o.why, 200), source, sats };
}

export function parseSatReadout(m: unknown): SatReadout | null {
  const o = m as Record<string, unknown> | null;
  if (!o || o.type !== 'sat') return null;
  const state = STATES.includes(o.state as SatLockState) ? (o.state as SatLockState) : 'OFF';
  return {
    state, why: str(o.why, 200), shared: o.shared === true, id: str(o.id, 24), name: str(o.name, 40), norad: num(o.norad),
    tx: str(o.tx, 40), loop: str(o.loop, 8), consumer: str(o.consumer, 20), nominal: num(o.nominal), offset: num(o.offset),
    rate: num(o.rate), trim: num(o.trim), az: num(o.az), el: num(o.el), aos: num(o.aos), los: num(o.los), maxEl: num(o.maxEl),
    age: num(o.age), ageState: str(o.ageState, 10), time: str(o.time, 8) ?? 'ok',
  };
}

export function parseSatTrack(m: unknown): SatTrack | null {
  const o = m as Record<string, unknown> | null;
  if (!o || o.type !== 'sat_track' || !Array.isArray(o.pts)) return null;
  const pts: [number, number][] = [];
  for (const p of (o.pts as unknown[]).slice(0, 200))
    if (Array.isArray(p) && isFinite(p[0]) && isFinite(p[1])) pts.push([Number(p[0]), Math.max(0, Math.min(90, Number(p[1])))]);
  let geo: GeoPt[] | undefined, tcaIdx: number | undefined;
  if (Array.isArray(o.geo)) {
    geo = [];
    // ★ a bad / null entry stays a gap (null) so geo[i] keeps its time (geoT0 + i·geoStep) and the pen lifts there
    for (const g of (o.geo as unknown[]).slice(0, 200))
      geo.push(Array.isArray(g) && isFinite(g[0]) && isFinite(g[1]) && Math.abs(Number(g[0])) <= 90 && Math.abs(Number(g[1])) <= 360
        ? [Number(g[0]), Number(g[1]), isFinite(g[2]) ? Math.max(0, Math.min(50000, Number(g[2]))) : 600] : null);
    if (geo.filter(Boolean).length < 2) geo = undefined;
    const t0 = num(o.geoT0), st = num(o.geoStep), tca = num(o.tca);
    if (geo && t0 != null && st != null && st > 0 && tca != null) {
      const i = Math.round((tca - t0) / st);
      if (i >= 0 && i < geo.length && geo[i]) tcaIdx = i;
    }
  }
  return { norad: num(o.norad) ?? 0, aos: num(o.aos) ?? 0, los: num(o.los) ?? 0, pts, ...(geo ? { geo } : {}), ...(tcaIdx != null ? { tcaIdx } : {}) };
}

/** hwinfo → the button: absent = no button at all (the radio reaches no listed downlink); else live or greyed. */
export function satCapFrom(j: Record<string, unknown> | null | undefined): { ok: boolean; why: string } | null {
  if (!j || typeof j.doppler !== 'number' || j.doppler < 1) return null;
  const why = str(j.dopplerWhy, 200) ?? '';
  return { ok: !why, why };
}

// ── words ──────────────────────────────────────────────────────────────────────────────────────────────────────
/** The white text beside the digits: "ISS · ▼ 3.21 kHz" (brief §10: name · direction · offset, 2 dp kHz). */
export function satChanText(r: SatReadout | null): string {
  if (!r || r.state === 'OFF' || !r.name) return '';
  if (r.state === 'ARMED') return `${r.name} · AOS ${clockIn(r.aos, Date.now() / 1000)}`;
  const off = r.offset ?? 0;
  const arrow = off >= 0 ? '▲' : '▼';   // ▲ approaching (received high) · ▼ receding
  return `${r.name} · ${arrow} ${Math.abs(off / 1000).toFixed(2)} kHz`;
}
/** The small line under it (brief §10): TRACK / COAST / ARMED / NO TIME / STALE / the age. */
export function satStateWord(r: SatReadout | null): string {
  if (!r || r.state === 'OFF') return '';
  if (r.time === 'none') return 'NO TIME';
  if (r.ageState === 'stale' && r.state !== 'ARMED') return 'STALE';
  if (r.ageState === 'old' && r.age != null) return `ELEMENTS ${Math.round(r.age)} d`;
  if (r.time === 'system' && r.state !== 'ARMED') return `${r.state} · TIME: SYSTEM`;
  return r.state === 'ENDED' ? 'DOPPLER ENDED' : r.state;
}
/** The bottom line between overs: "ISS · EL 34° · LOS 6:12". */
export function satStatusLine(r: SatReadout | null, nowSec: number): string {
  if (!r || r.state === 'OFF' || !r.name) return '';
  if (r.state === 'ARMED') return `${r.name} · AOS ${clockIn(r.aos, nowSec)}`;
  const el = r.el != null ? `EL ${Math.round(r.el)}°` : '';
  const los = r.los ? `LOS ${clockIn(r.los, nowSec)}` : '';
  return [r.shared ? 'SAT TRACK' : '', r.name, el, los].filter(Boolean).join(' · ');
}
/** "6:12" until `t` (m:ss; h:mm:ss over an hour); "now" once passed. */
export function clockIn(t: number | undefined, nowSec: number): string {
  if (!t) return '';
  const d = Math.round(t - nowSec);
  if (d <= 0) return 'now';
  const h = Math.floor(d / 3600), m = Math.floor((d % 3600) / 60), s = d % 60;
  return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}` : `${m}:${String(s).padStart(2, '0')}`;
}
/** A downlink in words: "437.800 MHz FM" / "435.610–435.670 MHz USB". */
export function txText(t: SatTx): string {
  const mhz = (hz: number) => (hz / 1e6).toFixed(3);
  const mode = t.mode === 'nfm' ? 'FM' : t.mode.toUpperCase();
  const down = t.hiHz ? `${mhz(t.hz)}–${mhz(t.hiHz)} MHz ${mode}` : `${mhz(t.hz)} MHz ${mode}`;
  // ★ beta91 (Stuart 11-22: "can we show which is the uplink and which is the downlink"): a downlink with an uplink says
  //   which is which; a beacon / telemetry / weather downlink has nothing to tell apart and stays the bare frequency
  const up = satUplinkText(t);
  return up ? `Downlink ${down} · ${up}` : down;
}
/** ★ beta91 the uplink in words — "Uplink 145.990 MHz · 67 Hz tone", a transponder's "Uplink 145.900–145.960 MHz" — or ''.
 *  (We only receive: it is said so people who transmit know where to call; never a control.) */
export function satUplinkText(t: SatTx): string {
  if (!(t.upHz && t.upHz > 0)) return '';
  const mhz = (hz: number) => (hz / 1e6).toFixed(3);
  const span = t.hiHz && t.hiHz > t.hz ? t.hiHz - t.hz : 0;
  const up = span ? `${mhz(t.upHz)}–${mhz(t.upHz + span)} MHz` : `${mhz(t.upHz)} MHz`;
  return `Uplink ${up}${t.ctcss && t.ctcss > 0 ? ` · ${Number(t.ctcss.toFixed(1))} Hz tone` : ''}`;
}
/** Where the picker's TRACK button tunes: a single channel's frequency, a passband's centre. */
export function txTuneHz(t: SatTx): number { return t.hiHz ? Math.round((t.hz + t.hiHz) / 2) : t.hz; }
/** The demodulator a downlink is listened to with (a decoder's own mode → its carrier's). */
export function txListenMode(t: SatTx): string {
  if (t.mode === 'nfm' || t.mode === 'usb' || t.mode === 'lsb' || t.mode === 'cw') return t.mode;
  if (t.mode === 'lrpt') return 'nfm';   // ★ the LRPT decoder (v12-lrpt) takes the channel IQ; NFM is its listening mode
  return 'nfm';
}
/** The element-set age in words, or '' when fresh. */
export function ageText(s: { age: number; ageState: string }): string {
  if (s.ageState === 'none') return 'no orbit data';
  if (s.ageState === 'fresh') return '';
  if (s.ageState === 'expired') return `orbit data ${Math.round(s.age)} days old — too old to track`;
  return `orbit data ${Math.round(s.age)} days old`;
}
/** The picker's rows, sorted: up now first (highest), then by next AOS; birds this radio cannot hear left out. */
export function pickerRows(l: SatList | null, nowSec: number): Array<{ sat: SatInfo; up: boolean; next?: SatPass; reach: SatTx[] }> {
  if (!l) return [];
  const rows = l.sats.map(sat => {
    const reach = sat.tx.filter(t => t.reach);
    const next = sat.passes.find(p => p.los > nowSec);
    // ★ beta91: the pass times decide "up" (the elevation is the last sat_list's snapshot) — as satModeRows
    const up = sat.passes.length ? sat.passes.some(p => p.aos <= nowSec && p.los > nowSec) : (sat.el ?? -90) >= 0;
    return { sat, up, next, reach };
  }).filter(r => r.reach.length > 0);
  rows.sort((a, b) => (a.up !== b.up ? (a.up ? -1 : 1) : a.up ? (b.sat.el ?? 0) - (a.sat.el ?? 0) : (a.next?.aos ?? Infinity) - (b.next?.aos ?? Infinity)));
  return rows;
}

// ── the sky plot (polar: zenith at the centre, the horizon the rim, north up) ──────────────────────────────────
/** az/el → x/y in a unit circle (radius 1 = horizon). */
export function skyXY(azDeg: number, elDeg: number): [number, number] {
  const r = Math.max(0, Math.min(1, (90 - elDeg) / 90));
  const a = (azDeg * Math.PI) / 180;
  return [r * Math.sin(a), -r * Math.cos(a)];
}
/** An SVG path (viewBox -1 -1 2 2) for a pass's track. */
export function skyPath(pts: [number, number][]): string {
  let d = '';
  pts.forEach(([az, el], i) => { const [x, y] = skyXY(az, el); d += `${i ? 'L' : 'M'}${x.toFixed(3)} ${y.toFixed(3)}`; });
  return d;
}
/** The whole plot as an SVG string — rings at 0/30/60°, N/E/S/W, the track, the bird now. Colours from CSS currentColor. */
export function skySvg(track: SatTrack | null, now?: { az?: number; el?: number }): string {
  const ring = (el: number) => `<circle cx="0" cy="0" r="${((90 - el) / 90).toFixed(3)}" fill="none" stroke="currentColor" stroke-opacity="0.3" stroke-width="0.01"/>`;
  const lbl = (t: string, x: number, y: number) => `<text x="${x}" y="${y}" font-size="0.12" text-anchor="middle" dominant-baseline="middle" fill="currentColor" fill-opacity="0.6">${t}</text>`;
  let s = `<svg viewBox="-1.15 -1.15 2.3 2.3" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="Sky track">`;
  s += ring(0) + ring(30) + ring(60) + lbl('N', 0, -1.08) + lbl('E', 1.08, 0) + lbl('S', 0, 1.08) + lbl('W', -1.08, 0);
  if (track && track.pts.length > 1) s += `<path d="${skyPath(track.pts)}" fill="none" stroke="currentColor" stroke-width="0.025" stroke-linecap="round"/>`;
  if (now && now.az != null && now.el != null && now.el >= 0) {
    const [x, y] = skyXY(now.az, now.el);
    s += `<circle cx="${x.toFixed(3)}" cy="${y.toFixed(3)}" r="0.05" fill="currentColor"/>`;
  }
  return s + '</svg>';
}

/** ★ The picker's credit line (CelesTrak's data is theirs — say so, with its age): "Orbits: CelesTrak · 5 h old".
 *  The age is from the mirror's CelesTrak fetch, else the newest element set's epoch. '' = nothing to credit yet. */
/** A synced orbit copy older than this is LATE (the mirror's 6-hourly round + a server's 6-h re-ask). */
export const SAT_LATE_H = 12;
export function satCreditText(l: SatList | null, nowSec: number): string {
  if (!l?.source?.orbits) return '';
  const short = /CelesTrak/i.test(l.source.orbits) ? 'Orbits: CelesTrak' : l.source.orbits;
  const at = l.source.fetched ?? l.source.newestEpoch;
  if (at == null || !(at > 0)) return short;
  const h = Math.max(0, (nowSec - at) / 3600);
  const age = h < 1 ? 'under an hour old' : h < 48 ? `${Math.round(h)} h old` : `${Math.round(h / 24)} days old`;
  /* ★ beta91 LATE, said (Stuart 11-22: "it says the orbits are 26h old"): the mirror refreshes every 6 h and a server
   *  asks again within 6 h once its copy passes 12 h, so a synced copy over 12 h old is late — say so, not only the age.
   *  Only for a synced copy (`fetched`): the built-in list is old by nature and says its age alone. */
  const late = l.source.fetched != null && h >= SAT_LATE_H ? ' \u2014 late (normally under 12 h)' : '';
  return `${short} · ${age}${late}`;
}
/** The SatNOGS credit, only when the transmitter data used it (CC BY-SA 4.0 asks for attribution). */
export function satTxCreditText(l: SatList | null): string {
  return l?.source?.transmitters && /SatNOGS/i.test(l.source.transmitters) ? 'Transmitters: SatNOGS DB (CC BY-SA 4.0)' : '';
}

// ══ ★★★ SATELLITE MODE (docs/v12/SATELLITE-MODE.md, Stuart 2026-11-20) ════════════════════════════════════════════════
// ONE whole-radio mode for every satellite: a timetable of every listed downlink (passing now, the SHORTEST remaining pass
// first, then the next passes by AOS; what this radio cannot hear GREYED with why — the list is a timetable, so greying is
// the rule here), a pick = {"type":"sat","op":"pick"} (the server tunes, locks the Doppler and attaches the decoder the
// downlink's `use` needs, all in one step), AUTO DOPPLER (default ON), and a box sized by the use: voice / packet COMPACT
// (the corrected frequency, the countdown, the synced line, a half-globe), image BIG (the picture as it arrives).
// Shared by the web (web/client/src/satellites.ts) and the app (SatellitePanel.tsx) so both say the same thing.

/** The use of a downlink — the server's word, else (an older server) worked out from its mode / consumer. */
export function txUse(t: SatTx): SatUse {
  if (t.use) return t.use;
  if (t.mode === 'lrpt' || t.consumer === 'lrpt') return 'image';
  if (/^ax25|aprs|packet|9600|1200/i.test(t.mode) || /aprs|ax25|packet/i.test(t.consumer ?? '')) return 'packet';
  return 'voice';
}

/** {"type":"sat","state":"picked",norad,tx,decoder} — the server's answer to a pick (a refusal comes as OFF + why). */
export interface SatPicked { norad: number; tx: number; decoder: '' | 'lrpt' | 'sstv' | 'aprs' | 'cw' | 'fctlm' }
export function parseSatPicked(m: unknown): SatPicked | null {
  const o = m as Record<string, unknown> | null;
  if (!o || o.type !== 'sat' || o.state !== 'picked') return null;
  const norad = num(o.norad), tx = num(o.tx);
  if (norad == null || tx == null || tx < 0) return null;
  const d = o.decoder;
  // ★ SATELLITE-BEST 1.1 / 1.2: a CW beacon's `cw` and AO-73's FUNcube telemetry `fctlm` (the server's, beta90)
  return { norad, tx: Math.floor(tx), decoder: d === 'lrpt' || d === 'sstv' || d === 'aprs' || d === 'cw' || d === 'fctlm' ? d : '' };
}
/** The three requests the mode sends (spectrum socket). */
export const satPickMsg = (norad: number, tx: number, doppler: boolean) => ({ type: 'sat', op: 'pick', norad, tx, doppler });
export const satDopplerMsg = (on: boolean) => ({ type: 'sat', op: 'doppler', on });
export const SAT_DROP_MSG = { type: 'sat', op: 'drop' } as const;

export interface SatRow {
  sat: SatInfo; tx: SatTx; i: number;
  /** passing now */
  up: boolean;
  /** seconds to LOS while up (inRangeFor, else the current pass's LOS) */
  left?: number;
  /** the next pass (not up) */
  next?: SatPass;
  /** ★ the pass this row is about — the one in progress while up, else the next (its maxEl rates it, satPassQuality) */
  pass?: SatPass;
  /** greyed: this radio cannot hear it, or its orbit cannot be tracked — `why` says which */
  grey: boolean; why: string;
}
/** ★ The timetable: one row per downlink; birds up now first (SHORTEST remaining pass on top — it is the one about to go),
 *  then the rest by next AOS, soonest first; no pass in the window last. A bird's downlinks stay together, in its order.
 *  Nothing is left out: what this radio cannot hear is GREYED with why (spec item 6). */
export function satModeRows(l: SatList | null, nowSec: number): SatRow[] {
  if (!l) return [];
  const birds = l.sats.map((sat) => {
    const cur = sat.passes.find((p) => p.aos <= nowSec && p.los > nowSec);
    /* ★★ beta91 (Stuart 2026-11-22: "when it transitions from next pass in 0:00 to the pass actually running it has a 30
     *  second or so window where it shows the next pass instead"): the PASS TIMES decide, against this second's clock —
     *  the server's inRangeFor / el are a SNAPSHOT from its last sat_list (tens of seconds old), so at AOS it still said
     *  "not up", the pass now running was skipped (aos > now is false) and the row showed the FOLLOWING pass; at LOS the
     *  same snapshot kept it "up" past its end. The snapshot is used only when the server sent no passes. */
    const up = sat.passes.length ? !!cur : sat.inRangeFor != null ? sat.inRangeFor > 0 : (sat.el ?? -90) >= 0;
    const left = !up ? undefined : cur ? cur.los - nowSec : sat.inRangeFor;
    const next = up ? undefined : sat.passes.find((p) => p.aos > nowSec);
    const key = up ? (left ?? 1e9) : next ? 1e10 + next.aos : 1e12;
    const pass = up ? cur : next;
    return { sat, up, left, next, pass, key };
  });
  birds.sort((a, b) => a.key - b.key || a.sat.name.localeCompare(b.sat.name));
  const out: SatRow[] = [];
  for (const b of birds) {
    const orbitWhy = b.sat.ageState === 'expired' || b.sat.ageState === 'none' ? ageText(b.sat) : '';
    b.sat.tx.forEach((tx, i) => {
      const why = !tx.reach ? (tx.why || 'out of this receiver’s range') : orbitWhy;
      out.push({ sat: b.sat, tx, i, up: b.up, left: b.left, next: b.next, pass: b.pass, grey: !!why, why });
    });
  }
  return out;
}

const two = (n: number) => String(n).padStart(2, '0');
/** "16:42" — local wall-clock time of a unix second. */
export function hhmm(t: number): string { const d = new Date(t * 1000); return `${two(d.getHours())}:${two(d.getMinutes())}`; }
/** "0:38" / "12:05" — hours:minutes until `t` (a pass hours away reads as a clock, not seconds). */
export function hmIn(t: number, nowSec: number): string {
  const m = Math.max(0, Math.floor((t - nowSec) / 60));
  return `${Math.floor(m / 60)}:${two(m % 60)}`;
}
/** m:ss (h:mm:ss over an hour) of a duration in seconds. */
export function mss(sec: number): string { return sec >= 1 ? clockIn(sec, 0) : '0:00'; }

/** The row's right-hand words: "UP · 6:12 left" / "AOS 16:42 (in 0:38)" / "No pass in the next day". */
export function satWhenText(r: Pick<SatRow, 'up' | 'left' | 'next'>, nowSec: number): string {
  if (r.up) return r.left != null ? `UP · ${mss(r.left)} left` : 'UP';
  if (r.next) return `AOS ${hhmm(r.next.aos)} (in ${hmIn(r.next.aos, nowSec)})`;
  return 'No pass in the next day';
}
/** The compact box's countdown: "In range for 9:55" while up, else "Next pass in 0:38 · AOS 16:42 · max 56°". */
/** `geo` (beta91, the pick's track path, time order) adds "· heading north / south" while the bird is up, where certain. */
export function satCountdownText(sat: SatInfo | undefined, nowSec: number, geo?: GeoPt[] | null): string {
  if (!sat) return '';
  const row = satModeRows({ time: 'ok', located: true, now: nowSec, sats: [{ ...sat, tx: [{ label: '-', hz: 1, mode: 'nfm', loop: 'none', reach: true }] }] }, nowSec)[0];
  if (row.up) {
    const hd = satHeadingText(geo, sat.lat, sat.lon);
    return (row.left != null ? `In range for ${mss(row.left)}` : 'In range now') + (hd ? ` · ${hd}` : '');
  }
  if (row.next) return `Next pass in ${hmIn(row.next.aos, nowSec)} · AOS ${hhmm(row.next.aos)} · max ${Math.round(row.next.maxEl)}°`;
  return 'No pass in the next day';
}
/* ★★ HOW GOOD THE PASS SHOULD BE (Stuart 2026-11-21: "on the satellite list are we able to determine if the pass is likely
 *  to be strong or weak" — n2yo's table shades its passes). n2yo rates what the EYE sees (brightness, sunlit); for RADIO
 *  the measure is the pass's HIGHEST ELEVATION: a high pass brings the bird closer (a LEO at 10° is ~3× further away than
 *  overhead — ~10 dB more path loss) and lifts it clear of the trees, roofs and hills on the horizon, and it stays up
 *  longer. Four levels, shaded like n2yo's legend. Nothing about the transmitter is known, so this is the geometry only. */
export type SatPassQ = 'EXCELLENT' | 'GOOD' | 'MARGINAL' | 'POOR';
export function satPassQuality(maxEl: number): SatPassQ {
  return maxEl >= 50 ? 'EXCELLENT' : maxEl >= 30 ? 'GOOD' : maxEl >= 15 ? 'MARGINAL' : 'POOR';
}
/** The row's rating: "max 56° · EXCELLENT" ('' = no pass to rate). */
export function satPassText(p: SatPass | undefined): string {
  return p ? `max ${Math.round(p.maxEl)}° · ${satPassQuality(p.maxEl)}` : '';
}
/** ★★ THE FREQUENCY READOUT'S BADGE (Stuart 2026-11-21: "a simple auto doppler icon would be enough with an up or down
 *  arrow" — the name, AOS and state are the SATELLITE box's; the readout got squished carrying them). null = no lock: no
 *  badge. dir 1 = ▲ approaching (received high), -1 = ▼ receding, 0 = armed, waiting for the pass. dim = waiting, coasting
 *  or on old orbits. `label` is for screen readers and the tooltip. */
export interface SatDopBadge { dir: 1 | -1 | 0; dim: boolean; label: string }
export function satDopplerBadge(r: SatReadout | null): SatDopBadge | null {
  if (!r || r.state === 'OFF' || r.state === 'ENDED') return null;
  const who = r.name ? ` · ${r.name}` : '';
  if (r.state === 'ARMED') return { dir: 0, dim: true, label: `Auto Doppler armed${who}, waiting for the pass` };
  const off = r.offset ?? 0;
  const dir: 1 | -1 = off >= 0 ? 1 : -1;
  const dim = r.state === 'COAST' || r.ageState === 'stale' || r.time === 'none';
  return { dir, dim, label: `Auto Doppler ${satStateWord(r)}${who} · ${dir > 0 ? 'approaching' : 'receding'} ${Math.abs(off / 1000).toFixed(2)} kHz` };
}
/** ★ Spec item 10: "Orbits synced 16:08 · 2 h ago" / "Fetching orbit data…" / "No orbit data — why". '' = an older server
 *  that does not say (its credit line still shows). */
export function satSyncedText(l: SatList | null, nowSec: number): string {
  const so = l?.source;
  if (!l || !so || (so.synced == null && !so.fetching)) return '';
  if (so.fetching) return 'Fetching orbit data…';
  if (!so.synced || so.synced <= 0) return `No orbit data — ${l.why || 'this server could not fetch any yet'}`;
  const s = Math.max(0, nowSec - so.synced);
  const ago = s < 60 ? 'just now' : s < 3600 ? `${Math.floor(s / 60)} min ago` : s < 48 * 3600 ? `${Math.floor(s / 3600)} h ago` : `${Math.floor(s / 86400)} days ago`;
  return `Orbits synced ${hhmm(so.synced)} · ${ago}`;
}
/** Re-ask the list every 3 s while the server is fetching (the contract), else every 30 s. */
export function satListEveryMs(l: SatList | null): number { return l?.source?.fetching ? 3000 : 30000; }
/** The frequency the box shows: the nominal with the Doppler correction applied (where the channel really is now). */
export function satCorrectedHz(t: SatTx, r: SatReadout | null): number {
  const live = r && r.state !== 'OFF';
  const base = live && r!.nominal ? r!.nominal : txTuneHz(t);
  return Math.round(base + (live ? r!.offset ?? 0 : 0));
}
/** "145.803 21 MHz" — the corrected frequency to 10 Hz. */
export function satHzText(hz: number): string {
  const m = (hz / 1e6).toFixed(5);
  return `${m.slice(0, -2)} ${m.slice(-2)} MHz`;
}
/** ★ The box's size by use: image BIG; voice / packet COMPACT; the ISS 145.800 (sstv) compact until a picture starts. */
export function satBoxSize(t: SatTx | undefined, pictureRunning: boolean): 'compact' | 'big' {
  if (!t) return 'compact';
  if (txUse(t) === 'image') return 'big';
  return t.sstv && pictureRunning ? 'big' : 'compact';
}
/** The decoder a pick needs (the server attaches it; the client follows on its decoder socket to receive its frames). */
export function satDecoderFor(t: SatTx): SatPicked['decoder'] {
  if (t.decoder != null) return t.decoder;   // ★ beta90: the server says
  const u = txUse(t);
  return u === 'image' ? (t.mode === 'lrpt' || t.consumer === 'lrpt' ? 'lrpt' : 'sstv') : u === 'packet' ? 'aprs' : t.sstv ? 'sstv' : '';
}
/** APRS baud for a packet downlink ('9600' / '1200'). */
export function satPacketBaud(t: SatTx): '9600' | '1200' { return /9600/.test(`${t.mode} ${t.label} ${t.consumer ?? ''}`) ? '9600' : '1200'; }

// ── the half-globe (spec item 4): an orthographic hemisphere centred on the receiver, the bird at its sub-point ────────
/** Height above the ground (km) from slant range and elevation (the server does not send it). */
export function satAltKm(rangeKm: number | undefined, elDeg: number | undefined): number | undefined {
  if (rangeKm == null || elDeg == null || !(rangeKm > 0)) return undefined;
  const R = 6371, e = (elDeg * Math.PI) / 180;
  return Math.sqrt(R * R + rangeKm * rangeKm + 2 * R * rangeKm * Math.sin(e)) - R;
}
const DEG = Math.PI / 180;
/** Orthographic projection of (lat, lon) at radius `r` (1 = the ground) seen from above (lat0, lon0): x right, y up,
 *  z towards the viewer (z < 0 = the far side). */
export function orthoXYZ(lat: number, lon: number, lat0: number, lon0: number, r = 1): [number, number, number] {
  const p = lat * DEG, l = (lon - lon0) * DEG, p0 = lat0 * DEG;
  const x = r * Math.cos(p) * Math.sin(l);
  const y = r * (Math.cos(p0) * Math.sin(p) - Math.sin(p0) * Math.cos(p) * Math.cos(l));
  const z = r * (Math.sin(p0) * Math.sin(p) + Math.cos(p0) * Math.cos(p) * Math.cos(l));
  return [x, y, z];
}
/** The view is tipped this far south of the receiver, so the receiver stands near the top of the dome. */
export const GLOBE_TILT = 50;
/** ★★ SATELLITE-BEST 1.8: the bird, drawn as paths in scene pixels (one geometry for the web canvas and react-native-svg) —
 *  a TUBE body standing on the Earth side, a DISH under it opening DOWN towards the globe's centre (the sub-point), and
 *  TWO panels on a boom lying ALONG the heading (the screen direction of the orbit, 1.7's path; level with no path). */
export interface GlobeBird {
  body: string; dish: string; panels: string; cells: string; boom: string;
  /** unit screen vectors (y down): the boom's axis (the heading) and the dish's (towards the globe centre) */
  u: [number, number]; d: [number, number];
  /** where the beam leaves the dish (the rim's middle) */
  focus: { x: number; y: number };
  /** half the bird's size across its widest (panel tip to panel tip) */
  reach: number;
  /** the drawing's bounding box on screen, for labels to keep clear */
  box: { l: number; t: number; r: number; b: number };
}
export interface GlobeScene {
  w: number; h: number; cx: number; cy: number; r: number;
  /** the graticule (30°), SVG path in pixels — Path2D on the web, <Path d> in the app */
  grid: string;
  /** rough land outlines (globeCoast.ts, Natural Earth simplified 0.6°), the same clipping as the grid */
  coast: string;
  rx: { x: number; y: number } | null;
  sat: { x: number; y: number } | null;
  /** the beam: a cone from the bird's dish down to its footprint (SVG path), and the footprint itself */
  beam: string; foot: string;
  /** ★ 1.7: the pass's orbit at the bird's (exaggerated) height — the renderer DASHES it ('' = no path from the server) */
  orbit: string;
  /** ★ 1.7: the highest point of the pass (closest to the receiver) and its small tick; null = not on the visible side */
  tca: { x: number; y: number } | null;
  tcaTick: string;
  /** ★ beta91: WHICH WAY the bird travels (Stuart 11-22: "i only caught it halfway through i dont know if its going over the
   *  north pole or going down over europe/africa") — chevrons on the visible orbit pointing along increasing time (geo[0]
   *  is the earliest: the server samples AOS − 10 min → LOS + 10 min). [] = no path (an older server) or none visible. The
   *  renderers draw them with globeArrowPath at a size on SCREEN. */
  arrows: GlobeArrow[];
  /** ★ 1.8: the bird's drawing (null = no bird to draw) */
  bird: GlobeBird | null;
}
/** ★ beta91: one direction chevron — at (x, y) in scene px, pointing along the unit vector (ux, uy); `i` = where on the
 *  path it sits as a fractional geo index (so a later arrow has a larger i); `ahead` = the one just in front of the bird. */
export interface GlobeArrow { x: number; y: number; ux: number; uy: number; i: number; ahead?: boolean }
/** ★ beta91: the chevrons as one open M/L path (stroked, never filled — a filled shape would read as the beam); `size` =
 *  the chevron's length in scene units (the renderers pass a few CSS px × their scale). */
/** the chevron's length in CSS px / points (× the renderer's scene-per-pixel scale) */
export const GLOBE_ARROW_PX = 7;
export function globeArrowPath(arrows: GlobeArrow[], size: number): string {
  let d = '';
  for (const a of arrows) {
    const px = -a.uy, py = a.ux, h = size / 2;
    const tx = a.x + a.ux * h, ty = a.y + a.uy * h, bx = a.x - a.ux * h, by = a.y - a.uy * h;
    d += `M${f1(bx + px * size * 0.6)} ${f1(by + py * size * 0.6)}L${f1(tx)} ${f1(ty)}L${f1(bx - px * size * 0.6)} ${f1(by - py * size * 0.6)}`;
  }
  return d;
}
/** ★ beta91: "heading north" / "heading south" — from the sign of the sub-satellite latitude's change at the geo point
 *  nearest the bird now (geo in time order). '' where it is not certain: no path, the bird not near it (> 5°), or near the
 *  turn at the top / bottom of the orbit (< 0.3° change across the neighbouring points). */
export function satHeadingText(geo: GeoPt[] | null | undefined, lat: number | undefined, lon: number | undefined): string {
  if (!geo || lat == null || lon == null) return '';
  let near = -1, nA = Infinity;
  geo.forEach((p, i) => { if (!p) return; const a = gcAngle(p[0], p[1], lat, lon); if (a < nA) { nA = a; near = i; } });
  if (near < 0 || nA > 5 * DEG) return '';
  const a = geo[near - 1] ?? geo[near], b = geo[near + 1] ?? geo[near];
  if (!a || !b || a === b) return '';
  const dl = b[0] - a[0];
  return Math.abs(dl) < 0.3 ? '' : dl > 0 ? 'heading north' : 'heading south';
}
/** How far above the globe a height is drawn (× the radius): exaggerated so 400 km is not a hair above the ground. */
export function globeLift(altKm: number | undefined): number {
  const alt = altKm != null && altKm > 0 ? altKm : 600;
  return Math.max(0.1, Math.min(0.35, (alt / 6371) * 2.5));
}
/** The central angle (radians) between two points on the globe. */
function gcAngle(la1: number, lo1: number, la2: number, lo2: number): number {
  const p1 = la1 * DEG, p2 = la2 * DEG, dl = (lo2 - lo1) * DEG;
  const c = Math.sin(p1) * Math.sin(p2) + Math.cos(p1) * Math.cos(p2) * Math.cos(dl);
  return Math.acos(Math.max(-1, Math.min(1, c)));
}
const f1 = (n: number) => n.toFixed(1);
const poly = (pts: Array<[number, number]>, close = true) => pts.map(([x, y], k) => `${k ? 'L' : 'M'}${f1(x)} ${f1(y)}`).join('') + (close ? 'Z' : '');
/** ★ 1.8: the bird's paths at screen point `p`, heading `u`, dish towards `d` (unit vectors, y down); `k` = scene size unit. */
export function globeBird(p: { x: number; y: number }, u: [number, number], d: [number, number], k: number): GlobeBird {
  const at = (a: number, ua: [number, number], b: number, ub: [number, number]): [number, number] => [p.x + ua[0] * a + ub[0] * b, p.y + ua[1] * a + ub[1] * b];
  const pu: [number, number] = [-u[1], u[0]];   // across the boom
  const pd: [number, number] = [-d[1], d[0]];   // across the tube
  // the tube: along d, rounded ends (a capsule of straight sides + 5-point caps)
  const HL = 5 * k, HW = 3.6 * k, body: Array<[number, number]> = [];
  for (let i = 0; i <= 6; i++) { const a = Math.PI * (i / 6); body.push(at(HL - HW + Math.sin(a) * HW, d, Math.cos(a) * HW, pd)); }
  for (let i = 0; i <= 6; i++) { const a = Math.PI * (i / 6); body.push(at(-(HL - HW) - Math.sin(a) * HW, d, -Math.cos(a) * HW, pd)); }
  // the dish: a bowl under the tube, its rim towards the globe (the beam leaves the rim's middle)
  const base = HL + 0.6 * k, DW = 5.6 * k, DD = 3.2 * k, dish: Array<[number, number]> = [];
  for (let i = 0; i <= 10; i++) { const t = -1 + i / 5; dish.push(at(base + DD * t * t, d, DW * t, pd)); }
  const focus = at(base + DD, d, 0, pd);
  // the boom and the two panels along the heading, each ruled into three cells
  const B0 = 3.8 * k, P0 = 8 * k, P1 = 19 * k, PW = 3.8 * k;
  let panels = '', cells = '';
  for (const sg of [-1, 1]) {
    panels += poly([at(sg * P0, u, -PW, pu), at(sg * P1, u, -PW, pu), at(sg * P1, u, PW, pu), at(sg * P0, u, PW, pu)]);
    for (let c = 1; c < 3; c++) { const a = sg * (P0 + ((P1 - P0) * c) / 3); cells += poly([at(a, u, -PW, pu), at(a, u, PW, pu)], false); }
    cells += poly([at(sg * P0, u, 0, pu), at(sg * P1, u, 0, pu)], false);
  }
  const boom = poly([at(-P0, u, 0, pu), at(-B0, u, 0, pu)], false) + poly([at(B0, u, 0, pu), at(P0, u, 0, pu)], false)
    + poly([at(base - 0.4 * k, d, 0, pd), at(base + DD * 0.8, d, 0, pd)], false);   // ★ the feed, in the bowl
  const out = { body: poly(body), dish: poly(dish), panels, cells, boom };
  const box = { l: Infinity, t: Infinity, r: -Infinity, b: -Infinity };
  for (const m of Object.values(out).join(' ').matchAll(/(-?[\d.]+) (-?[\d.]+)/g)) {
    const x = Number(m[1]), y = Number(m[2]);
    box.l = Math.min(box.l, x); box.r = Math.max(box.r, x); box.t = Math.min(box.t, y); box.b = Math.max(box.b, y);
  }
  return { ...out, u, d, focus: { x: focus[0], y: focus[1] }, reach: P1 + PW, box };
}
/** ★ The whole half-globe as numbers + SVG paths (no tiles, nothing to download). The dome is the top half of the disc; the
 *  bird at radius 1 + its (exaggerated) height above its sub-point, hidden behind the globe or below the cut. `sat` null =
 *  no position (the scene still draws the dome and the receiver). `path` (1.7, the track reply's geo) = the pass's orbit,
 *  drawn at the same height, front side only, and the heading the bird's panels lie along. */
export function globeScene(w: number, h: number, rx: { lat: number; lon: number }, sat: { lat: number; lon: number; altKm?: number } | null,
                           path?: GeoPt[] | null, tcaIdx?: number): GlobeScene {
  // ★ headroom: the highest bird (1 + 0.35), its panels and the label over it stay inside
  const r = Math.max(10, Math.min(w * 0.46, (h - 30) / 1.38));
  const cx = w / 2, cy = h - 2;
  const lat0 = Math.max(-90, rx.lat - GLOBE_TILT), lon0 = rx.lon;
  const P = (x: number, y: number) => `${(cx + x * r).toFixed(1)} ${(cy - y * r).toFixed(1)}`;
  let grid = '';
  const line = (pts: Array<[number, number]>) => {
    let pen = false;
    for (const [la, lo] of pts) {
      const [x, y, z] = orthoXYZ(la, lo, lat0, lon0);
      if (z < 0 || y < 0) { pen = false; continue; }
      grid += `${pen ? 'L' : 'M'}${P(x, y)}`; pen = true;
    }
  };
  for (let la = -60; la <= 60; la += 30) { const pts: Array<[number, number]> = []; for (let lo = -180; lo <= 180; lo += 6) pts.push([la, lo]); line(pts); }
  for (let lo = -180; lo < 180; lo += 30) { const pts: Array<[number, number]> = []; for (let la = -90; la <= 90; la += 6) pts.push([la, lo]); line(pts); }
  // ★ rough outlines (Stuart 11-20: "rough outlines would be fine, its just to show satellite pass status visually") —
  //   the same pen as the grid, written to `coast`; the far side and below the cut lift the pen
  const gridOnly = grid; grid = '';
  for (const ring of GLOBE_COAST) { const pts: Array<[number, number]> = []; for (let k = 0; k + 1 < ring.length; k += 2) pts.push([ring[k + 1] / 10, ring[k] / 10]); line(pts); }
  const coast = grid; grid = gridOnly;
  const [rxX, rxY, rxZ] = orthoXYZ(rx.lat, rx.lon, lat0, lon0);
  const rxPt = rxZ >= 0 && rxY >= 0 ? { x: cx + rxX * r, y: cy - rxY * r } : null;
  /** a point above the ground at `altKm` (exaggerated) → the screen, and whether it is on the side we see, above the cut */
  const lifted = (la: number, lo: number, altKm: number | undefined) => {
    const [x, y, z] = orthoXYZ(la, lo, lat0, lon0, 1 + globeLift(altKm));
    return { x: cx + x * r, y: cy - y * r, vis: y > 0 && !(z < 0 && Math.hypot(x, y) < 1) };
  };
  // ★ 1.7 the orbit: the pen lifts behind the globe and below the cut, as the coastlines'
  let orbit = '', tca: { x: number; y: number } | null = null, tcaTick = '';
  const geo = path && path.filter(Boolean).length >= 2 ? path : null;
  const gp = geo ? geo.map((p) => (p ? lifted(p[0], p[1], p[2]) : null)) : [];
  if (geo) {
    let pen = false;
    for (const q of gp) { if (!q || !q.vis) { pen = false; continue; } orbit += `${pen ? 'L' : 'M'}${f1(q.x)} ${f1(q.y)}`; pen = true; }
    // the highest point of the pass: the server's tca where it says, else the point closest to the receiver
    let best = -1, bestA = Infinity;
    if (tcaIdx != null && geo[tcaIdx]) best = tcaIdx;
    else geo.forEach((p, i) => { if (!p) return; const a = gcAngle(p[0], p[1], rx.lat, rx.lon); if (a < bestA) { bestA = a; best = i; } });
    const q = best >= 0 ? gp[best] : null;
    if (q && q.vis) {
      tca = { x: q.x, y: q.y };
      const a = gp[Math.max(0, best - 1)] ?? q, b = gp[Math.min(gp.length - 1, best + 1)] ?? q;
      const tx = b.x - a.x, ty = b.y - a.y, n = Math.hypot(tx, ty) || 1, nx = -ty / n, ny = tx / n, L = r * 0.045;
      tcaTick = `M${f1(q.x - nx * L)} ${f1(q.y - ny * L)}L${f1(q.x + nx * L)} ${f1(q.y + ny * L)}`;
    }
  }
  let satPt: { x: number; y: number } | null = null, beam = '', foot = '', bird: GlobeBird | null = null;
  if (sat) {
    const alt = sat.altKm != null && sat.altKm > 0 ? sat.altKm : 600;
    const sp0 = lifted(sat.lat, sat.lon, alt);
    if (sp0.vis) {
      const sp = { x: sp0.x, y: sp0.y };
      satPt = sp;
      // ★ the heading: the screen direction from the path's point nearest the bird to the next one (level until a path)
      let u: [number, number] = [1, 0];
      if (geo) {
        let near = -1, nA = Infinity;
        geo.forEach((p, i) => { if (!p) return; const a = gcAngle(p[0], p[1], sat.lat, sat.lon); if (a < nA) { nA = a; near = i; } });
        const a = near < 0 ? null : gp[near === gp.length - 1 ? near - 1 : near], b = near < 0 ? null : gp[near === gp.length - 1 ? near : near + 1];
        if (a && b) { const hx = b.x - a.x, hy = b.y - a.y, n = Math.hypot(hx, hy); if (n > 1e-6) u = [hx / n, hy / n]; }
      }
      const dx = cx - sp.x, dy = cy - sp.y, dn = Math.hypot(dx, dy) || 1;
      bird = globeBird(sp, u, [dx / dn, dy / dn], r / 100);
      const o = bird.focus;
      // the footprint: the ground the bird can see (half-angle acos(R/(R+h))), a ring of points round the sub-point
      const lam = Math.acos(6371 / (6371 + alt));
      const p1 = sat.lat * DEG, l1 = sat.lon * DEG;
      const ring: Array<{ x: number; y: number }> = [];
      for (let k = 0; k < 36; k++) {
        const b = k * 10 * DEG;
        const p2 = Math.asin(Math.sin(p1) * Math.cos(lam) + Math.cos(p1) * Math.sin(lam) * Math.cos(b));
        const l2 = l1 + Math.atan2(Math.sin(b) * Math.sin(lam) * Math.cos(p1), Math.cos(lam) - Math.sin(p1) * Math.sin(p2));
        const [x, y, z] = orthoXYZ(p2 / DEG, l2 / DEG, lat0, lon0);
        if (z >= 0 && y >= 0) ring.push({ x: cx + x * r, y: cy - y * r });
      }
      if (ring.length >= 3) {
        foot = ring.map((q, k) => `${k ? 'L' : 'M'}${q.x.toFixed(1)} ${q.y.toFixed(1)}`).join('') + 'Z';
        // the cone's two edges: the footprint points furthest either side of the dish → sub-point axis
        const [gx, gy] = orthoXYZ(sat.lat, sat.lon, lat0, lon0);
        const ax = cx + gx * r - o.x, ay = cy - gy * r - o.y;
        const n = Math.hypot(ax, ay) || 1, nx = -ay / n, ny = ax / n;
        let lo = ring[0], hi = ring[0], dlo = Infinity, dhi = -Infinity;
        for (const q of ring) { const d = (q.x - o.x) * nx + (q.y - o.y) * ny; if (d < dlo) { dlo = d; lo = q; } if (d > dhi) { dhi = d; hi = q; } }
        beam = `M${o.x.toFixed(1)} ${o.y.toFixed(1)}L${lo.x.toFixed(1)} ${lo.y.toFixed(1)}L${hi.x.toFixed(1)} ${hi.y.toFixed(1)}Z`;
      }
    }
  }
  const arrows = geo ? globeArrows(gp, r, satPt, bird ? bird.reach : 0, tca) : [];
  return { w, h, cx, cy, r, grid, coast, rx: rxPt, sat: satPt, beam, foot, orbit, tca, tcaTick, arrows, bird };
}

/** ★ beta91: where the direction chevrons go on the visible orbit `gp` (screen points in time order; null / !vis = pen up):
 *  one just ahead of the bird (clear of its panels), and 1–3 spread along the visible line — kept off the bird, the
 *  ahead-chevron and the highest point's tick, so nothing is drawn on top of another mark. */
function globeArrows(gp: Array<{ x: number; y: number; vis: boolean } | null>, r: number, sat: { x: number; y: number } | null,
                     reach: number, tca: { x: number; y: number } | null): GlobeArrow[] {
  const segs: Array<{ i: number; a: { x: number; y: number }; b: { x: number; y: number }; len: number; at: number }> = [];
  let T = 0;
  for (let i = 0; i + 1 < gp.length; i++) {
    const a = gp[i], b = gp[i + 1];
    if (!a || !b || !a.vis || !b.vis) continue;
    const len = Math.hypot(b.x - a.x, b.y - a.y);
    if (len < 1e-6) continue;
    segs.push({ i, a, b, len, at: T }); T += len;
  }
  if (!segs.length || T < r * 0.15) return [];
  const mk = (sg: typeof segs[number], t: number, ahead?: boolean): GlobeArrow => {
    const ux = (sg.b.x - sg.a.x) / sg.len, uy = (sg.b.y - sg.a.y) / sg.len;
    return { x: sg.a.x + (sg.b.x - sg.a.x) * t, y: sg.a.y + (sg.b.y - sg.a.y) * t, ux, uy, i: sg.i + t, ...(ahead ? { ahead: true } : {}) };
  };
  const out: GlobeArrow[] = [];
  const clearOfBird = reach + r * 0.05;
  if (sat) {
    // the segment the bird is on (its nearest), then forward to the first point a bird's reach away
    let s0 = -1, best = Infinity, t0 = 0;
    segs.forEach((sg, k) => {
      const vx = sg.b.x - sg.a.x, vy = sg.b.y - sg.a.y, t = Math.max(0, Math.min(1, ((sat.x - sg.a.x) * vx + (sat.y - sg.a.y) * vy) / (sg.len * sg.len)));
      const d = Math.hypot(sg.a.x + vx * t - sat.x, sg.a.y + vy * t - sat.y);
      if (d < best) { best = d; s0 = k; t0 = t; }
    });
    if (s0 >= 0 && best < r * 0.1) {
      const far = (sg: typeof segs[number], t: number) => Math.hypot(sg.a.x + (sg.b.x - sg.a.x) * t - sat.x, sg.a.y + (sg.b.y - sg.a.y) * t - sat.y);
      for (let k = s0; k < segs.length; k++) {
        const sg = segs[k];
        if (k > s0 && segs[k - 1].i + 1 !== sg.i) break;   // ★ the pen lifted (behind the globe / below the cut): none past it
        const tS = k === s0 ? t0 : 0;   // ★ on the bird's own segment, start AT the bird (never behind it)
        if (far(sg, 1) < clearOfBird) continue;
        let lo = tS, hi = 1;
        if (far(sg, tS) >= clearOfBird) hi = tS;
        else for (let it = 0; it < 16; it++) { const m = (lo + hi) / 2; if (far(sg, m) < clearOfBird) lo = m; else hi = m; }
        out.push(mk(sg, hi, true));
        break;
      }
    }
  }
  const at = (L: number) => { const sg = segs.find((q) => L <= q.at + q.len) ?? segs[segs.length - 1]; return mk(sg, Math.max(0, Math.min(1, (L - sg.at) / sg.len))); };
  const fits = (a: GlobeArrow) => !(sat && Math.hypot(a.x - sat.x, a.y - sat.y) < clearOfBird)
    && !(tca && Math.hypot(a.x - tca.x, a.y - tca.y) < r * 0.06) && !out.some((o) => Math.hypot(a.x - o.x, a.y - o.y) < r * 0.2);
  const n = Math.max(1, Math.min(3, Math.round(T / (r * 0.25))));
  for (let k = 0; k < n; k++) { const a = at((T * (k + 0.5)) / n); if (fits(a)) out.push(a); }
  // ★ every mark was under the bird (it sits mid-line): one more where the line is clear of it, so the way is always shown
  if (!out.length) for (let k = 1; k < 24; k++) { const a = at((T * k) / 24); if (fits(a)) { out.push(a); break; } }
  out.sort((a, b) => a.i - b.i);
  return out;
}

/* ★★ SATELLITE-BEST 1.8 — LABELS (Stuart 11-21: "it took me a second or so to realise what the dots and lines were when i
 *  first saw them"): the receiver's dot (the server's name, else RECEIVER) and the bird (its name). They keep off each
 *  other, off the bird and inside the picture; a bird near the dot puts its own label above it. `font` is in scene units
 *  (the renderer passes ~10 CSS px worth, so the words read at SMALL and BIG alike). */
export interface GlobeLabel { x: number; y: number; anchor: 'start' | 'middle' | 'end'; text: string }
/** ★ The receiver dot's look on every faceplate (Stuart 11-21: "make the receiver marker green or a different contrasting
 *  colour") — the health pill's / the TRACK dot's green, never the dome's ink; r and ring in CSS px (renderers scale). */
export const GLOBE_RX_DOT = { fill: '#5BE36B', ring: 'rgba(0,0,0,0.85)', r: 5, ringW: 1.5 } as const;

/** ★★ beta91 THE LAND IS NEUTRAL, NOT THE FACEPLATE's INK (Stuart 11-22, a screenshot on the blue faceplate: "the country
 *  outlines should be white or a different colour to the signal beam so that it is easier to see"). The beam, the
 *  footprint, the orbit and the bird are the faceplate's accent; on a blue or amber faceplate the ink is the same hue,
 *  so the coastlines vanished into the footprint. Land is drawn white on a dark window, near-black on a light one —
 *  judged from the window's ground colour (any CSS rgb()/rgba()/#hex; unknown → the dark answer, the default faceplates). */
export function globeLandInk(ground: string): string {
  let r = 0, g = 0, b = 0;
  const m = /rgba?\(\s*([\d.]+)[ ,]+([\d.]+)[ ,]+([\d.]+)/i.exec(ground || '');
  const h = /^#([0-9a-f]{3}|[0-9a-f]{6})\b/i.exec((ground || '').trim());
  if (m) { r = +m[1]; g = +m[2]; b = +m[3]; }
  else if (h) {
    const x = h[1].length === 3 ? h[1].split('').map((c) => c + c).join('') : h[1];
    r = parseInt(x.slice(0, 2), 16); g = parseInt(x.slice(2, 4), 16); b = parseInt(x.slice(4, 6), 16);
  }
  const lum = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
  return lum > 0.5 ? '#1a1a1a' : '#ffffff';
}

export const GLOBE_KEY_TEXT = 'dotted: this pass’s orbit · arrows: its direction · shaded: where it can be heard';
export function globeLabelText(s: string | undefined, max = 18): string {
  const t = (s ?? '').replace(/[\u0000-\u001f\u007f]/g, '').trim();
  return t.length > max ? t.slice(0, max - 1) + '…' : t;
}
export function globeLabels(g: GlobeScene, rxName: string | undefined, satName: string | undefined, font: number): { rx: GlobeLabel | null; sat: GlobeLabel | null } {
  const cw = font * 0.62, pad = font * 0.25;
  type Box = { l: number; t: number; r: number; b: number };
  const boxOf = (L: GlobeLabel): Box => {
    const wd = L.text.length * cw;
    const l = L.anchor === 'start' ? L.x : L.anchor === 'end' ? L.x - wd : L.x - wd / 2;
    return { l, t: L.y - font * 0.85, r: l + wd, b: L.y + font * 0.2 };
  };
  const hit = (a: Box, b: Box) => a.l < b.r + pad && b.l < a.r + pad && a.t < b.b + pad && b.t < a.b + pad;
  const inside = (b: Box) => b.l >= 1 && b.r <= g.w - 1 && b.t >= 1 && b.b <= g.cy;
  const clampMid = (L: GlobeLabel): GlobeLabel => {
    if (L.anchor !== 'middle') return L;
    const half = (L.text.length * cw) / 2;
    return { ...L, x: Math.max(half + 2, Math.min(g.w - half - 2, L.x)) };
  };
  let satL: GlobeLabel | null = null, birdBox: Box | null = null;
  const st = globeLabelText(satName, 16);
  if (g.sat && g.bird && st) {
    const bb = g.bird.box;
    birdBox = bb;
    const gap = pad * 1.5 + 1;
    const tries: GlobeLabel[] = [
      clampMid({ x: g.sat.x, y: bb.t - gap - font * 0.2, anchor: 'middle', text: st }),       // above (the rule)
      { x: bb.r + gap, y: g.sat.y + font * 0.35, anchor: 'start', text: st },                 // right
      { x: bb.l - gap, y: g.sat.y + font * 0.35, anchor: 'end', text: st },                   // left
      clampMid({ x: g.sat.x, y: bb.b + gap + font * 0.85, anchor: 'middle', text: st }),      // below
    ];
    satL = tries.find((L) => inside(boxOf(L)) && !hit(boxOf(L), birdBox!)) ?? null;
  }
  let rxL: GlobeLabel | null = null;
  const rt = globeLabelText(rxName) || 'RECEIVER';
  if (g.rx) {
    const dr = font * 0.6;
    const tries: GlobeLabel[] = [
      clampMid({ x: g.rx.x, y: g.rx.y + dr + font, anchor: 'middle', text: rt }),             // below the dot
      clampMid({ x: g.rx.x, y: g.rx.y - dr - pad, anchor: 'middle', text: rt }),              // above
      { x: g.rx.x + dr + pad, y: g.rx.y + font * 0.35, anchor: 'start', text: rt },
      { x: g.rx.x - dr - pad, y: g.rx.y + font * 0.35, anchor: 'end', text: rt },
    ];
    const clear = (L: GlobeLabel) => inside(boxOf(L)) && !(birdBox && hit(boxOf(L), birdBox)) && !(satL && hit(boxOf(L), boxOf(satL)));
    rxL = tries.find(clear) ?? null;
  }
  return { rx: rxL, sat: satL };
}

// ══ ★★ SATELLITE-BEST 1.4 — A TRANSPONDER's MODE, as operators know it ═══════════════════════════════════════════════════
/** AMSAT's band letter of a frequency (A 10 m, H 15 m, V 2 m, U 70 cm, L 23 cm, S 13 cm, X 3 cm); '' = none of those. */
export function satBandLetter(hz: number | undefined): string {
  if (!hz || !(hz > 0)) return '';
  const m = hz / 1e6;
  return m >= 28 && m < 30 ? 'A' : m >= 21 && m < 21.5 ? 'H' : m >= 144 && m < 148 ? 'V' : m >= 420 && m < 450 ? 'U'
    : m >= 1240 && m < 1300 ? 'L' : m >= 2300 && m < 2450 ? 'S' : m >= 10000 && m < 10500 ? 'X' : '';
}
/** ★ The row's mode label: "Mode B · U/v" (catalogue name · uplink UPPER / downlink lower). The server's `band` first; an
 *  older server's transponder (a passband with an uplink) is worked out from its two frequencies. The name is left out
 *  where the row's own label already says it ("AO-7 · Mode B"). '' = nothing to add. */
export function satTxModeLabel(t: SatTx): string {
  let band = t.band ?? '';
  if (!band && t.hiHz && t.upHz) { const up = satBandLetter(t.upHz), dn = satBandLetter(t.hz); if (up && dn) band = `${up}/${dn.toLowerCase()}`; }
  const name = t.modeName && !t.label.toLowerCase().includes(t.modeName.toLowerCase()) ? t.modeName : '';
  return [name, band].filter(Boolean).join(' · ');
}
/** The row's detail line: "145.925–145.975 MHz USB · Mode B · U/v". */
export function satTxDetail(t: SatTx): string { const m = satTxModeLabel(t); return m ? `${txText(t)} · ${m}` : txText(t); }

// ══ ★★★ SATELLITE-BEST 1.6 — REMIND ON A PASS (Stuart 2026-11-21) — CLIENT-HELD ONLY ═══════════════════════════════════════
// A REMIND key on each pass row, offered only where a pick disturbs nobody (source.pickPrivate — the server's word, never
// guessed). The reminder lives in this page / app session ONLY, in memory: no server state, nothing written to storage — if
// the listener leaves, the connection is gone or their session ends, nothing fires and the next user of that radio is never
// disturbed. Shortly before AOS a pop-up says "ISS · Voice / SSTV rises in 2:00 — LISTEN?"; LISTEN enters SATELLITE and
// picks through the ORDINARY pick (ask-before-tuning; the server gates it as a hand pick). Re-checked when it fires.
export const SAT_REMIND_LEADS = [1, 2, 5, 10] as const;
export type SatRemindLead = typeof SAT_REMIND_LEADS[number];
export const SAT_REMIND_LEAD_DEFAULT: SatRemindLead = 2;
export const SAT_REMIND_MAX = 20;
export const SAT_REMIND_SHARED_WHY = 'This radio is now shared — a pass cannot be picked for everyone';
export function satRemindLead(v: unknown): SatRemindLead {
  const n = Number(v);
  return (SAT_REMIND_LEADS as readonly number[]).includes(n) ? (n as SatRemindLead) : SAT_REMIND_LEAD_DEFAULT;
}
export interface SatReminder { key: string; server: string; norad: number; tx: number; aos: number; los: number; name: string; label: string }
/** ★ The REMIND key is drawn only where it can act: the server says a pick is private, the row is hearable (not greyed) and
 *  its pass is still to come. Hidden — not greyed — everywhere else (a control that can never act is not drawn). */
export function satRemindOffered(l: SatList | null, r: Pick<SatRow, 'grey' | 'up' | 'next'>): boolean {
  return l?.source?.pickPrivate === true && !r.grey && !r.up && !!r.next;
}
/** The key's words: "REMIND" / "REMINDING · 2 min". */
export function satRemindKeyText(on: boolean, lead: number): string { return on ? `REMINDING · ${lead} min` : 'REMIND'; }
/** ★ The re-check as it fires: why it can no longer be picked ('' = it can). The server's own words where it gave them. */
export function satRemindWhy(l: SatList | null, r: Pick<SatReminder, 'norad' | 'tx' | 'name'>): string {
  if (!l) return '';
  if (l.source?.pickPrivate !== true) return SAT_REMIND_SHARED_WHY;
  const sat = l.sats.find((s) => s.norad === r.norad);
  const t = sat?.tx[r.tx];
  if (!sat || !t) return `${r.name} is no longer listed on this server`;
  if (!t.reach) return t.why || 'out of this receiver’s range';
  if (sat.ageState === 'expired' || sat.ageState === 'none') return ageText(sat);
  return '';
}
/** The pop-up's line: "ISS · Voice / SSTV rises in 2:00 — LISTEN?" (or "is up now" once AOS has gone by). */
export function satRemindText(r: Pick<SatReminder, 'aos' | 'name' | 'label'>, nowSec: number): string {
  const who = r.label ? `${r.name} · ${r.label}` : r.name;
  return r.aos > nowSec ? `${who} rises in ${mss(r.aos - nowSec)} — LISTEN?` : `${who} is up now — LISTEN?`;
}
/** ★ Two passes at once: one receiver, one pick — the pop-up says what picking this one ends ('' = nothing to end). */
export function satRemindEnds(picked: { norad: number; tx: number } | null, pickedName: string | undefined, r: Pick<SatReminder, 'norad' | 'tx'>): string {
  if (!picked || (picked.norad === r.norad && picked.tx === r.tx) || !pickedName) return '';
  return `picking this ends ${pickedName}`;
}
/** ★ The in-memory book of reminders for this page / app session — bounded (SAT_REMIND_MAX), never stored. The clock is the
 *  caller's (unix s), so a fake clock tests it. */
export class SatReminders {
  private m = new Map<string, SatReminder>();
  static key(server: string, norad: number, tx: number, aos: number) { return `${server}|${norad}|${tx}|${Math.round(aos)}`; }
  get size() { return this.m.size; }
  all(): SatReminder[] { return [...this.m.values()].sort((a, b) => a.aos - b.aos); }
  /** the reminder on this downlink (any pass), if one is held */
  find(server: string, norad: number, tx: number): SatReminder | undefined {
    for (const r of this.m.values()) if (r.server === server && r.norad === norad && r.tx === tx) return r;
    return undefined;
  }
  /** Hold one for the row's next pass. false = refused (no pass to come, AOS gone, or the book is full). */
  add(server: string, row: Pick<SatRow, 'sat' | 'tx' | 'i' | 'next'>, nowSec: number): boolean {
    const p = row.next;
    if (!p || p.aos <= nowSec) return false;
    const ex = this.find(server, row.sat.norad, row.i);
    if (ex) this.m.delete(ex.key);
    if (this.m.size >= SAT_REMIND_MAX) return false;
    const key = SatReminders.key(server, row.sat.norad, row.i, p.aos);
    this.m.set(key, { key, server, norad: row.sat.norad, tx: row.i, aos: p.aos, los: p.los, name: row.sat.name, label: row.sat.tx.length > 1 ? row.tx.label : '' });
    return true;
  }
  cancel(key: string): boolean { return this.m.delete(key); }
  /** Leaving the server (or the session ending) cancels every reminder for it. */
  clearServer(server: string) { for (const [k, r] of this.m) if (r.server === server) this.m.delete(k); }
  clear() { this.m.clear(); }
  /** ★ Every sat_list: a new element set moves the pass a little — the reminder follows its pass (the nearest AOS within
   *  15 min on the same norad/tx), rather than keeping a stale time. A bird the list no longer has is kept (the fire-time
   *  check says why). */
  refresh(server: string, l: SatList | null) {
    if (!l) return;
    for (const r of [...this.m.values()]) {
      if (r.server !== server) continue;
      const sat = l.sats.find((s) => s.norad === r.norad);
      if (!sat) continue;
      let best: SatPass | undefined, bd = 15 * 60;
      for (const p of sat.passes) { const dd = Math.abs(p.aos - r.aos); if (dd <= bd) { bd = dd; best = p; } }
      if (!best || (best.aos === r.aos && best.los === r.los)) continue;
      this.m.delete(r.key);
      const key = SatReminders.key(server, r.norad, r.tx, best.aos);
      this.m.set(key, { ...r, key, aos: best.aos, los: best.los });
    }
  }
  /** ★ The reminders due now (AOS − lead reached, the pass not yet over) — taken out of the book as they fire; passes that
   *  are over are dropped unfired. `connected` false = a reminder whose connection is gone never fires: it is dropped
   *  silently when its time comes. */
  due(nowSec: number, leadMin: number, connected: boolean): SatReminder[] {
    const out: SatReminder[] = [];
    for (const [k, r] of this.m) {
      if (nowSec >= r.los) { this.m.delete(k); continue; }
      if (nowSec >= r.aos - leadMin * 60) { this.m.delete(k); if (connected) out.push(r); }
    }
    return out.sort((a, b) => a.aos - b.aos);
  }
  /** seconds until the next one is due (Infinity = none) — the caller asks for a fresh list shortly before */
  nextIn(nowSec: number, leadMin: number): number {
    let n = Infinity;
    for (const r of this.m.values()) n = Math.min(n, r.aos - leadMin * 60 - nowSec);
    return n;
  }
}

// ══ ★★ SATELLITE-BEST 1.3 — TELEMETRY TURNED INTO VALUES (the client half) ═══════════════════════════════════════════════
/* The server's decoder (AO-73 FUNcube `fctlm`; SONATE-2 / APRS later) sends {"type":"fctlm_status","data":{…}} on the decoder
 * socket — frames, good / failed, the bird's mode, and named values with units; a channel with no published calibration
 * comes RAW, labelled "raw" (never a guessed conversion). Whitelisted ASCII / numbers only — cleaned again here. */
export interface SatTlmValue { name: string; value: string; unit: string; raw: boolean }
/** The box's telemetry: the decoder's counts (fctlm_status, once a second) and the last frame's values (sat_tlm — from
 *  fctlm for AO-73, from aprs for APRS T# and SONATE-2), merged; a field a message does not carry keeps its last value. */
export interface SatTelemetry {
  frames?: number; good?: number; failed?: number; sync?: boolean; snrDb?: number;
  sat: string; format: string; mode: string; at: number; values: SatTlmValue[];
  /** the decoded payload, hex — behind the RAW switch */
  hex: string;
  /** ★★ beta91 TELEMETRY CHARTS: every frame of this pick, oldest first (bounded — src/utils/tlmCharts.ts) */
  hist?: TlmLiveFrame[];
}
/** {"type":"fctlm_status","data":{sync,frames,good,failed,afcHz,snrDb,…}} or {"type":"sat_tlm","data":{sat,format,t,mode,
 *  values:[{k,name,v,unit,raw}],hex}} (docs/v12/SATELLITE-BEST.md "Contract additions") → one shape. */
export function parseSatTelemetry(d: unknown): Partial<SatTelemetry> | null {
  const o = d as Record<string, unknown> | null;
  if (!o || typeof o !== 'object') return null;
  const out: Partial<SatTelemetry> = {};
  const cnt = (x: unknown) => { const k = num(x); return k != null && k >= 0 ? Math.floor(k) : undefined; };
  if (cnt(o.frames) != null) out.frames = cnt(o.frames);
  if (cnt(o.good) != null) out.good = cnt(o.good);
  if (cnt(o.failed) != null) out.failed = cnt(o.failed);
  if (typeof o.sync === 'boolean') out.sync = o.sync;
  if (num(o.snrDb) != null) out.snrDb = num(o.snrDb);
  if (str(o.sat, 24)) out.sat = str(o.sat, 24);
  if (str(o.format, 24)) out.format = str(o.format, 24);
  if (str(o.mode, 24)) out.mode = str(o.mode, 24);
  const at = num(o.t) ?? num(o.at); if (at != null) out.at = at;
  if (typeof o.hex === 'string') out.hex = o.hex.replace(/[^0-9a-fA-F]/g, '').slice(0, 512);
  if (Array.isArray(o.values)) {
    const values: SatTlmValue[] = [];
    for (const v0 of (o.values as unknown[]).slice(0, 64)) {
      const v = v0 as Record<string, unknown>;
      const name = str(v?.name ?? v?.n ?? v?.k, 32);
      const val = v?.v ?? v?.value;
      if (!name || (typeof val !== 'number' && typeof val !== 'string')) continue;
      const value = typeof val === 'number' ? (isFinite(val) ? String(Math.round(val * 1000) / 1000) : '') : str(val, 24) ?? '';
      if (!value) continue;
      const raw = v.raw === true;
      values.push({ name, value, unit: raw ? '' : str(v.unit ?? v.u, 8) ?? '', raw });   // ★ raw: the count, no unit
    }
    out.values = values;
  }
  return out;
}
/** A message merged into what the box shows (a status keeps the last frame's values; a frame keeps the counts). */
export function satTelemetryMerge(prev: SatTelemetry | null, m: Partial<SatTelemetry> | null): SatTelemetry | null {
  if (!m) return prev;
  const base: SatTelemetry = prev ?? { sat: '', format: '', mode: '', at: 0, values: [], hex: '' };
  const next = { ...base } as SatTelemetry;
  for (const [k, v] of Object.entries(m)) if (v !== undefined) (next as unknown as Record<string, unknown>)[k] = v;
  // ★★ beta91 a frame (values + its time) joins the pick's history — the THIS PASS charts
  if (m.values?.length && m.at) next.hist = tlmLivePush(base.hist, { t: m.at, values: m.values });
  return next;
}
/** The block's first line: "TELEMETRY · 14 frames · 13 good · 1 failed · Safe" ("waiting for a frame" until one). */
export function satTelemetryHead(t: SatTelemetry | null): string {
  if (!t || (t.frames == null && !t.values.length)) return t?.sync ? 'TELEMETRY · in sync — waiting for a good frame' : 'TELEMETRY · waiting for a frame';
  const parts = ['TELEMETRY'];
  if (t.frames != null) parts.push(`${t.frames} frame${t.frames === 1 ? '' : 's'}`, `${t.good ?? 0} good`, `${t.failed ?? 0} failed`);
  if (t.mode) parts.push(t.mode);
  return parts.join(' · ');
}
/** A value in words: "Battery voltage 8.13 V" / "Reboots 14" / "Battery temperature 143 raw". */
export function satTelemetryLine(v: SatTlmValue): string { return `${v.name} ${v.value}${v.unit ? ' ' + v.unit : ''}${v.raw ? ' raw' : ''}`; }
/** The RAW switch's text: the payload hex in groups of 4 bytes. */
export function satTelemetryHex(t: SatTelemetry | null): string { return (t?.hex ?? '').replace(/(.{8})/g, '$1 ').trim(); }

// ══ ★★ SATELLITE-BEST 1.9 — THE MAP KEY of a packet satellite ═══════════════════════════════════════════════════════════
/** "via ISS" — the label stations heard through the bird carry on the DIGI MODES map's APRS layer. */
export function satViaText(name: string | undefined): string { return name ? `via ${name}` : ''; }
/** ★ What the DIGI MODES map draws over its APRS layer for the pass (the b90-maps DigiMapOverlay shape): the orbit's ground
 *  track (split where it crosses the antimeridian or has a gap), the footprint around the bird now, and the bird itself. */
export interface SatMapOverlay {
  label: string;
  lines?: [number, number][][];
  circles?: { lat: number; lon: number; km: number }[];
  points?: { lat: number; lon: number; label: string }[];
}
export function satMapOverlay(name: string, geo: GeoPt[] | null | undefined, now: { lat?: number; lon?: number; altKm?: number } | null): SatMapOverlay {
  const lines: [number, number][][] = [];
  let cur: [number, number][] = [];
  for (const p of geo ?? []) {
    if (!p) { if (cur.length > 1) lines.push(cur); cur = []; continue; }
    const last = cur[cur.length - 1];
    if (last && Math.abs(p[1] - last[1]) > 180) { if (cur.length > 1) lines.push(cur); cur = []; }
    cur.push([p[0], p[1]]);
  }
  if (cur.length > 1) lines.push(cur);
  const o: SatMapOverlay = { label: `${name} PASS`.slice(0, 24) };
  if (lines.length) o.lines = lines;
  if (now && now.lat != null && now.lon != null) {
    const alt = now.altKm && now.altKm > 0 ? now.altKm : 600;
    o.circles = [{ lat: now.lat, lon: now.lon, km: Math.round(6371 * Math.acos(6371 / (6371 + alt))) }];
    o.points = [{ lat: now.lat, lon: now.lon, label: name }];
  }
  return o;
}
/** ★ The MAP key: a packet downlink, and only once a station with a position has been heard (no dead key). */
export function satMapOffered(t: SatTx | undefined, stationsWithPos: number): boolean {
  return !!t && txUse(t) === 'packet' && stationsWithPos > 0;
}
