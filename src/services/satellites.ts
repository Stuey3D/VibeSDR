/**
 * satellites.ts — ★ V12 SATELLITE ENGINE (docs/v12/SATELLITE-ENGINE.md): the words and shapes both clients use for a
 * VibeServer's satellite picker and Doppler lock. One copy (the app's SatellitePanel and the web's satellites.ts), so
 * every screen says the same thing.
 *
 * Wire (server → client):
 *   {"type":"sat_list", time, located, now, why?, sats:[{id,name,norad,note,age,ageState,tx:[{label,hz,hiHz?,mode,loop,
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

export type SatLockState = 'OFF' | 'ARMED' | 'TRACK' | 'COAST' | 'ENDED';

export interface SatTx {
  label: string; hz: number; hiHz?: number; mode: string; loop: string; consumer?: string;
  upHz?: number; ctcss?: number; note?: string; reach: boolean;
}
export interface SatPass { aos: number; tca: number; los: number; maxEl: number; aosAz: number; losAz: number }
export interface SatInfo {
  id: string; name: string; norad: number; note: string; age: number; ageState: string;
  tx: SatTx[]; az?: number; el?: number; rangeKm?: number; lat?: number; lon?: number; passes: SatPass[];
}
export interface SatList { time: string; located: boolean; now: number; why?: string; sats: SatInfo[] }
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
                  consumer: str(t.consumer, 20), upHz: num(t.upHz), ctcss: num(t.ctcss), note: str(t.note, 120), reach: t.reach === true });
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
                rangeKm: num(s.rangeKm), lat: num(s.lat), lon: num(s.lon), passes });
  }
  return { time: str(o.time, 8) ?? 'none', located: o.located === true, now: num(o.now) ?? Date.now() / 1000, why: str(o.why, 200), sats };
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
