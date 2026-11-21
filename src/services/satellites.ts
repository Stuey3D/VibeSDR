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
}
export interface SatList { time: string; located: boolean; now: number; why?: string; source?: SatSource; sats: SatInfo[] }
export interface SatReadout {
  state: SatLockState; why?: string; shared?: boolean;
  id?: string; name?: string; norad?: number; tx?: string; loop?: string; consumer?: string;
  nominal?: number; offset?: number; rate?: number; trim?: number; az?: number; el?: number;
  aos?: number; los?: number; maxEl?: number; age?: number; ageState?: string; time: string;
}
export interface SatTrack { norad: number; aos: number; los: number; pts: [number, number][] }

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
                  sstv: t.sstv === true ? true : undefined, why: str(t.why, 160) });
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
        synced: num(so.synced), fetching: so.fetching === true }
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
  return { norad: num(o.norad) ?? 0, aos: num(o.aos) ?? 0, los: num(o.los) ?? 0, pts };
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
  return t.hiHz ? `${mhz(t.hz)}–${mhz(t.hiHz)} MHz ${mode}` : `${mhz(t.hz)} MHz ${mode}`;
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
    return { sat, up: (sat.el ?? -90) >= 0, next, reach };
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
export function satCreditText(l: SatList | null, nowSec: number): string {
  if (!l?.source?.orbits) return '';
  const short = /CelesTrak/i.test(l.source.orbits) ? 'Orbits: CelesTrak' : l.source.orbits;
  const at = l.source.fetched ?? l.source.newestEpoch;
  if (at == null || !(at > 0)) return short;
  const h = Math.max(0, (nowSec - at) / 3600);
  const age = h < 1 ? 'under an hour old' : h < 48 ? `${Math.round(h)} h old` : `${Math.round(h / 24)} days old`;
  return `${short} · ${age}`;
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
export interface SatPicked { norad: number; tx: number; decoder: '' | 'lrpt' | 'sstv' | 'aprs' }
export function parseSatPicked(m: unknown): SatPicked | null {
  const o = m as Record<string, unknown> | null;
  if (!o || o.type !== 'sat' || o.state !== 'picked') return null;
  const norad = num(o.norad), tx = num(o.tx);
  if (norad == null || tx == null || tx < 0) return null;
  const d = o.decoder;
  return { norad, tx: Math.floor(tx), decoder: d === 'lrpt' || d === 'sstv' || d === 'aprs' ? d : '' };
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
    const up = sat.inRangeFor != null ? sat.inRangeFor > 0 : (sat.el ?? -90) >= 0;
    const left = !up ? undefined : sat.inRangeFor ?? (cur ? cur.los - nowSec : undefined);
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
export function satCountdownText(sat: SatInfo | undefined, nowSec: number): string {
  if (!sat) return '';
  const row = satModeRows({ time: 'ok', located: true, now: nowSec, sats: [{ ...sat, tx: [{ label: '-', hz: 1, mode: 'nfm', loop: 'none', reach: true }] }] }, nowSec)[0];
  if (row.up) return row.left != null ? `In range for ${mss(row.left)}` : 'In range now';
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
export function satDecoderFor(t: SatTx): '' | 'lrpt' | 'sstv' | 'aprs' {
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
export interface GlobeScene {
  w: number; h: number; cx: number; cy: number; r: number;
  /** the graticule (30°), SVG path in pixels — Path2D on the web, <Path d> in the app */
  grid: string;
  /** rough land outlines (globeCoast.ts, Natural Earth simplified 0.6°), the same clipping as the grid */
  coast: string;
  rx: { x: number; y: number } | null;
  sat: { x: number; y: number } | null;
  /** the beam: a cone from the bird down to its footprint (SVG path), and the footprint itself */
  beam: string; foot: string;
}
/** ★ The whole half-globe as numbers + SVG paths (no tiles, no coastlines — nothing to download, ~2 kB of code). The dome
 *  is the top half of the disc; the bird at radius 1 + its (exaggerated) height above its sub-point, hidden behind the
 *  globe or below the cut. `sat` null = no position (the scene still draws the dome and the receiver). */
export function globeScene(w: number, h: number, rx: { lat: number; lon: number }, sat: { lat: number; lon: number; altKm?: number } | null): GlobeScene {
  const r = Math.max(10, Math.min(w * 0.46, (h - 14) / 1.38));   // ★ headroom: the highest bird (1 + 0.35) and its panels stay inside
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
  let satPt: { x: number; y: number } | null = null, beam = '', foot = '';
  if (sat) {
    const alt = sat.altKm != null && sat.altKm > 0 ? sat.altKm : 600;
    const vis = Math.max(0.1, Math.min(0.35, (alt / 6371) * 2.5));   // ★ exaggerated so 400 km is not a hair above the ground
    const [sx, sy, sz] = orthoXYZ(sat.lat, sat.lon, lat0, lon0, 1 + vis);
    const behind = sz < 0 && Math.hypot(sx, sy) < 1;
    if (!behind && sy > 0) {
      const sp = { x: cx + sx * r, y: cy - sy * r };
      satPt = sp;
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
        // the cone's two edges: the footprint points furthest either side of the bird → sub-point axis
        const [gx, gy] = orthoXYZ(sat.lat, sat.lon, lat0, lon0);
        const ax = cx + gx * r - sp.x, ay = cy - gy * r - sp.y;
        const n = Math.hypot(ax, ay) || 1, nx = -ay / n, ny = ax / n;
        let lo = ring[0], hi = ring[0], dlo = Infinity, dhi = -Infinity;
        for (const q of ring) { const d = (q.x - sp.x) * nx + (q.y - sp.y) * ny; if (d < dlo) { dlo = d; lo = q; } if (d > dhi) { dhi = d; hi = q; } }
        beam = `M${sp.x.toFixed(1)} ${sp.y.toFixed(1)}L${lo.x.toFixed(1)} ${lo.y.toFixed(1)}L${hi.x.toFixed(1)} ${hi.y.toFixed(1)}Z`;
      }
    }
  }
  return { w, h, cx, cy, r, grid, coast, rx: rxPt, sat: satPt, beam, foot };
}
