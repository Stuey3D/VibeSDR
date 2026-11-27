/* ★★★ beta97 PASS PREVIEW (Stuart 11-27) — one set of rules for the web page (web/client/src/satellites.ts) and the app
 *  (src/components/SatellitePanel.tsx). A future pass's ▶ plays it out on the SATELLITE box's half-globe and on a small
 *  polar sky plot: the bird along its track from AOS to LOS in ~10 s, its footprint with it, the clock, elevation,
 *  azimuth, range and the Doppler at the downlink, with AOS / MAX / LOS marked.
 *
 *  ★★ THE POINT (Stuart): judging whether a MARGINAL pass is worth trying. His station hears passes to the EAST well, so
 *     the preview lays the station's OWN reception history (the sky plot's heat map — vibe_skyplot.h cells, the same data
 *     the ORBCOMM sky plot draws) under the pass, colours the trail by it (green: this station usually decodes there,
 *     amber: marginal, grey: rarely) and says it in one line: "Mostly over your good sector (E) · max 23°". With no history
 *     it SAYS so ("No reception history yet; this shows geometry only") — never a guess.
 *
 *  ★★ NO SECOND PROPAGATOR. The positions are the server's own SGP4 — the {"type":"sat_list","op":"track"} reply's `geo`
 *     (sub-satellite point + height every geoStep s, AOS − 10 min … LOS + 10 min). This file only does GEOMETRY on them:
 *     WGS-84 to Earth-fixed coordinates, a cubic (Catmull-Rom) curve through them for the in-between times and the
 *     velocity, and the look angles from the receiver exactly as vibe_sat_orbit.h look() does (south-east-zenith). The
 *     Doppler is the range rate × f / c, as Look::dopplerHz. Tested against the server's own pass and Doppler for fixed
 *     element sets (test/fixtures/sat-preview/passes.json, written by vibeserver/gen-sat-preview-fixture.cpp).
 *
 *  ★ Pure: no DOM, no React — scripts/test_sat_preview.ts. */
import type { GeoPt, SatList, SatPass, SatReadout, SatTrack } from './satellites';
import { skyCellBounds, skyCellIndex, skyPlotXY, type SkyCells } from '../utils/skyPlot';

const DEG = Math.PI / 180;
const C_KM_S = 299792.458;
type V3 = [number, number, number];

/** WGS-84 geodetic (deg, deg, km) → Earth-fixed km (the server's Observer::set and look()'s inverse). */
export function geodeticToEcef(latDeg: number, lonDeg: number, altKm: number): V3 {
  const a = 6378.137, f = 1 / 298.257223563, e2 = f * (2 - f);
  const sl = Math.sin(latDeg * DEG), cl = Math.cos(latDeg * DEG), so = Math.sin(lonDeg * DEG), co = Math.cos(lonDeg * DEG);
  const n = a / Math.sqrt(1 - e2 * sl * sl);
  return [(n + altKm) * cl * co, (n + altKm) * cl * so, (n * (1 - e2) + altKm) * sl];
}
/** Earth-fixed → geodetic (Bowring, 4 iterations — look()'s own). */
function ecefToGeodetic(p: V3): { lat: number; lon: number; altKm: number } {
  const a = 6378.137, f = 1 / 298.257223563, e2 = f * (2 - f);
  const q = Math.hypot(p[0], p[1]);
  let lat = Math.atan2(p[2], q * (1 - e2));
  for (let i = 0; i < 4; i++) { const sl = Math.sin(lat), n = a / Math.sqrt(1 - e2 * sl * sl); lat = Math.atan2(p[2] + n * e2 * sl, q); }
  const sl = Math.sin(lat), n = a / Math.sqrt(1 - e2 * sl * sl);
  return { lat: lat / DEG, lon: Math.atan2(p[1], p[0]) / DEG, altKm: q / Math.cos(lat) - n };
}

export interface PassLook { az: number; el: number; rangeKm: number; rrKmS: number }
/** What the receiver sees of a satellite at Earth-fixed `p` moving at `v` (km/s, Earth-fixed) — look()'s formulae. */
export function lookAt(rx: { lat: number; lon: number }, p: V3, v: V3 = [0, 0, 0]): PassLook {
  const o = geodeticToEcef(rx.lat, rx.lon, 0);
  const sLa = Math.sin(rx.lat * DEG), cLa = Math.cos(rx.lat * DEG), sLo = Math.sin(rx.lon * DEG), cLo = Math.cos(rx.lon * DEG);
  const x = p[0] - o[0], y = p[1] - o[1], z = p[2] - o[2];
  const rng = Math.hypot(x, y, z) || 1e-9;
  const s_ = sLa * cLo * x + sLa * sLo * y - cLa * z;
  const e_ = -sLo * x + cLo * y;
  const z_ = cLa * cLo * x + cLa * sLo * y + sLa * z;
  let az = Math.atan2(e_, -s_) / DEG; if (az < 0) az += 360;
  return { az, el: Math.asin(Math.max(-1, Math.min(1, z_ / rng))) / DEG, rangeKm: rng, rrKmS: (x * v[0] + y * v[1] + z * v[2]) / rng };
}
/** Received − transmitted at a downlink of `hz` for a range rate (km/s, + receding) — Look::dopplerHz. */
export const dopplerHzFor = (hz: number, rrKmS: number) => -hz * rrKmS / C_KM_S;

/** One moment of the pass: where the bird is (sub-point + height), what the receiver sees, the range rate. */
export interface PassSample extends PassLook { t: number; lat: number; lon: number; altKm: number }
export interface PassTrack {
  norad: number;
  /** the pass as THIS geometry finds it (horizon crossings, the highest point) — the server's within a second or two */
  aos: number; tca: number; los: number; maxEl: number; aosAz: number; tcaAz: number; losAz: number;
  /** AOS → LOS every `step` s (the last at LOS) */
  samples: PassSample[]; step: number;
  /** the server's geo (the dotted orbit) and its highest point, for the globe */
  geo: GeoPt[]; tcaIdx?: number;
}
/** The default sample spacing (s of pass time): a 10-min pass is 300 samples — the animation interpolates between them. */
export const PASS_SAMPLE_SEC = 2;

/** A cubic through the server's points (Catmull-Rom in Earth-fixed km) — position and velocity at any time inside them. */
function curve(track: SatTrack): ((t: number) => { p: V3; v: V3 } | null) | null {
  const geo = track.geo, t0 = track.geoT0, st = track.geoStep;
  if (!geo || t0 == null || !st || st <= 0) return null;
  const P = geo.map((g) => (g ? geodeticToEcef(g[0], g[1], g[2]) : null));
  return (t: number) => {
    const u = (t - t0) / st;
    const i = Math.floor(u);
    if (i < 0 || i + 1 >= P.length) return null;
    const p1 = P[i], p2 = P[i + 1];
    if (!p1 || !p2) return null;
    const p0 = P[i - 1] ?? null, p3 = P[i + 2] ?? null;
    const s = u - i;
    const out: V3 = [0, 0, 0], vel: V3 = [0, 0, 0];
    for (let k = 0; k < 3; k++) {
      // tangents (per index): central differences, one-sided at a gap / the ends
      const m1 = p0 ? (p2[k] - p0[k]) / 2 : p2[k] - p1[k];
      const m2 = p3 ? (p3[k] - p1[k]) / 2 : p2[k] - p1[k];
      const s2 = s * s, s3 = s2 * s;
      out[k] = (2 * s3 - 3 * s2 + 1) * p1[k] + (s3 - 2 * s2 + s) * m1 + (-2 * s3 + 3 * s2) * p2[k] + (s3 - s2) * m2;
      vel[k] = ((6 * s2 - 6 * s) * p1[k] + (3 * s2 - 4 * s + 1) * m1 + (-6 * s2 + 6 * s) * p2[k] + (3 * s2 - 2 * s) * m2) / st;
    }
    return { p: out, v: vel };
  };
}

/** ★★★ THE SAMPLER: the server's track reply → the pass, sampled every `step` s from AOS to LOS. null = nothing to play
 *  (an older server's reply without geo / its clock, or no part of it above the horizon). `pass` (the list's own, when
 *  known) only chooses WHICH pass of the window — the times come from the geometry. */
export function passTrackFrom(track: SatTrack | null | undefined, rx: { lat: number; lon: number } | null | undefined,
                              pass?: Pick<SatPass, 'aos' | 'los'> | null, step = PASS_SAMPLE_SEC): PassTrack | null {
  if (!track || !rx || !track.geo || track.geoT0 == null || !track.geoStep) return null;
  const cv = curve(track);
  if (!cv) return null;
  const t0 = track.geoT0, t1 = track.geoT0 + (track.geo.length - 1) * track.geoStep;
  const elAt = (t: number) => { const c = cv(t); return c ? lookAt(rx, c.p, c.v).el : -90; };
  // coarse scan for the up-intervals (5 s), then the one that overlaps the list's pass (else the longest)
  const ups: Array<[number, number]> = [];
  let prevT = t0, prevEl = elAt(t0), start: number | null = prevEl >= 0 ? t0 : null;
  const refine = (a: number, b: number, rising: boolean) => {   // bisection to 0.05 s
    for (let k = 0; k < 30 && b - a > 0.05; k++) { const m = (a + b) / 2; if ((elAt(m) >= 0) === rising) b = m; else a = m; }
    return (a + b) / 2;
  };
  for (let t = t0 + 5; t <= t1 + 1e-6; t += 5) {
    const tt = Math.min(t, t1), el = elAt(tt);
    if (prevEl < 0 && el >= 0) start = refine(prevT, tt, true);
    else if (prevEl >= 0 && el < 0 && start != null) { ups.push([start, refine(prevT, tt, false)]); start = null; }
    prevT = tt; prevEl = el;
  }
  if (start != null) ups.push([start, t1]);
  if (!ups.length) return null;
  const want = pass ?? { aos: track.aos, los: track.los };
  let best = ups[0], bestScore = -Infinity;
  for (const u of ups) {
    const ov = Math.min(u[1], want.los) - Math.max(u[0], want.aos);
    const score = want.los > want.aos ? ov : u[1] - u[0];
    if (score > bestScore) { bestScore = score; best = u; }
  }
  const [aos, los] = best;
  // the highest point: the best sample, then golden section
  let tca = aos, maxEl = -90;
  for (let t = aos; t <= los; t += 2) { const e = elAt(t); if (e > maxEl) { maxEl = e; tca = t; } }
  { let a = Math.max(aos, tca - 2), b = Math.min(los, tca + 2); const g = (Math.sqrt(5) - 1) / 2;
    for (let k = 0; k < 40 && b - a > 0.01; k++) { const c = b - g * (b - a), d = a + g * (b - a); if (elAt(c) > elAt(d)) b = d; else a = c; }
    tca = (a + b) / 2; maxEl = elAt(tca); }
  const sampleAt = (t: number): PassSample | null => {
    const c = cv(t);
    if (!c) return null;
    const L = lookAt(rx, c.p, c.v), g = ecefToGeodetic(c.p);
    return { t, lat: g.lat, lon: g.lon, altKm: g.altKm, ...L };
  };
  const samples: PassSample[] = [];
  const n = Math.max(1, Math.ceil((los - aos) / step));
  for (let i = 0; i <= n; i++) { const s = sampleAt(Math.min(los, aos + i * step)); if (s) samples.push(s); }
  if (samples.length < 2) return null;
  return { norad: track.norad, aos, tca, los, maxEl, aosAz: samples[0].az, tcaAz: sampleAt(tca)?.az ?? 0, losAz: samples[samples.length - 1].az,
           samples, step, geo: track.geo, ...(track.tcaIdx != null ? { tcaIdx: track.tcaIdx } : {}) };
}

const lerp = (a: number, b: number, f: number) => a + (b - a) * f;
const lerpAng = (a: number, b: number, f: number) => { const d = ((b - a + 540) % 360) - 180; return ((a + d * f) % 360 + 360) % 360; };
/** The bird at pass time `t` (clamped to AOS…LOS), interpolated between the samples — the animation's every frame. */
export function passSampleAt(pt: PassTrack, t: number): PassSample {
  const s = pt.samples;
  if (t <= s[0].t) return s[0];
  if (t >= s[s.length - 1].t) return s[s.length - 1];
  const i = Math.min(s.length - 2, Math.max(0, Math.floor((t - s[0].t) / pt.step)));
  const a = s[i], b = s[i + 1], f = b.t > a.t ? (t - a.t) / (b.t - a.t) : 0;
  const lon = lerpAng(a.lon + 180, b.lon + 180, f) - 180;
  return { t, lat: lerp(a.lat, b.lat, f), lon, altKm: lerp(a.altKm, b.altKm, f), az: lerpAng(a.az, b.az, f), el: lerp(a.el, b.el, f),
           rangeKm: lerp(a.rangeKm, b.rangeKm, f), rrKmS: lerp(a.rrKmS, b.rrKmS, f) };
}

/* ── the player: pass time against real time ─────────────────────────────────────────────────────────────────────── */
/** A whole pass plays in this long at 1× (Stuart: ~8–12 s). */
export const PASS_PLAY_SEC = 10;
export const PASS_SPEEDS = [0.5, 1, 2] as const;
export type PassSpeed = typeof PASS_SPEEDS[number];
/** The preview's clock. advance() moves pass time by real milliseconds × the speed; at LOS it stops (ended) and ▶ replays.
 *  Paused or ended = no frames (the caller stops its requestAnimationFrame when advance returns false). */
export class PassPlayer {
  t: number; playing = true; ended = false; speed: PassSpeed = 1;
  constructor(public readonly aos: number, public readonly los: number) { this.t = aos; }
  /** pass seconds per real second */
  rate(): number { return Math.max(1, (this.los - this.aos) / PASS_PLAY_SEC) * this.speed; }
  /** → true while it still wants frames */
  advance(dtMs: number): boolean {
    if (!this.playing || this.ended) return false;
    this.t = Math.min(this.los, this.t + (Math.max(0, Math.min(dtMs, 250)) / 1000) * this.rate());   // ★ a stalled tab never jumps the pass
    if (this.t >= this.los) { this.ended = true; this.playing = false; return false; }
    return true;
  }
  frac(): number { return this.los > this.aos ? (this.t - this.aos) / (this.los - this.aos) : 0; }
  seek(frac: number) { this.t = this.aos + Math.max(0, Math.min(1, frac)) * (this.los - this.aos); this.ended = this.t >= this.los; if (this.ended) this.playing = false; }
  /** ▶ / ❚❚ — at LOS ▶ starts again from AOS */
  toggle() { if (this.ended) { this.t = this.aos; this.ended = false; this.playing = true; } else this.playing = !this.playing; }
  setSpeed(s: number) { this.speed = (PASS_SPEEDS as readonly number[]).includes(s) ? (s as PassSpeed) : 1; }
}

/* ── the words ───────────────────────────────────────────────────────────────────────────────────────────────────── */
const COMPASS8 = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
export const compass8 = (az: number) => COMPASS8[Math.floor((((az % 360) + 360) % 360 + 22.5) / 45) % 8];
const p2 = (n: number) => String(n).padStart(2, '0');
/** "14:32:05" in UTC, or the device's own zone with `local` */
export function passClock(t: number, local = false): string {
  const d = new Date(t * 1000);
  return local ? `${p2(d.getHours())}:${p2(d.getMinutes())}:${p2(d.getSeconds())}` : `${p2(d.getUTCHours())}:${p2(d.getUTCMinutes())}:${p2(d.getUTCSeconds())}`;
}
const hm = (t: number, local: boolean) => passClock(t, local).slice(0, 5);
const dur = (s: number) => { const m = Math.floor(s / 60), r = Math.round(s - m * 60); return `${m}:${p2(r === 60 ? 59 : r)}`; };
/** The readout's lines at pass time `t`: the clock (UTC · local), where it is, how far, the Doppler (when a downlink). */
export function passReadout(pt: PassTrack, t: number, downlinkHz?: number): { clock: string; look: string; range: string; doppler: string } {
  const s = passSampleAt(pt, t);
  const dop = downlinkHz && downlinkHz > 0 ? dopplerHzFor(downlinkHz, s.rrKmS) : null;
  return {
    clock: `${passClock(t)} UTC · ${passClock(t, true)} local`,
    look: `EL ${Math.max(0, s.el).toFixed(0)}° · AZ ${s.az.toFixed(0)}° ${compass8(s.az)}`,
    range: `${Math.round(s.rangeKm).toLocaleString('en-GB')} km`,
    doppler: dop == null ? '' : `Doppler ${dop >= 0 ? '+' : '−'}${(Math.abs(dop) / 1000).toFixed(1)} kHz at ${(downlinkHz! / 1e6).toFixed(3)} MHz`,
  };
}
/** The marked moments: "AOS 14:30 · MAX 23° 14:35 · LOS 14:41 · 10:53" (UTC; `local` = the device's zone). */
export function passMomentsText(pt: PassTrack, local = false): string {
  return `AOS ${hm(pt.aos, local)} ${compass8(pt.aosAz)} · MAX ${pt.maxEl.toFixed(0)}° ${hm(pt.tca, local)} · LOS ${hm(pt.los, local)} ${compass8(pt.losAz)} · ${dur(pt.los - pt.aos)}`;
}
/** The strip pinned at the globe: "▶ PREVIEW ISS · 14:30 UTC" */
export function passStripText(name: string, aos: number): string { return `▶ PREVIEW ${name} · ${hm(aos, false)} UTC`; }
/** What ▶ does (the button's title / accessibility label) */
export const PASS_PREVIEW_WHAT = 'Preview this pass: play it out on the globe and the sky plot, coloured by where this station usually hears';
export const PASS_PREVIEW_STOP = 'Stop the preview and go back to the live view';
/** ★ The satellite box's help line, over the passes (both clients) */
export const PASS_PREVIEW_HINT = '▶ on a pass plays it out first: the globe, where to point on the sky plot, and how well this station usually hears that part of the sky';
/** A pass in progress on the pick takes priority: the preview stops (or will not start) and says so. */
export const PASS_LIVE_NOW = 'LIVE PASS NOW: the preview has stopped so the pass that is really happening shows';
/** ★ The live pass that blocks a preview: the PICKED bird up now (its lock tracking, or its pass between AOS and LOS). */
export function passLiveNow(picked: { norad: number } | null | undefined, readout: SatReadout | null | undefined, list: SatList | null | undefined, now: number): boolean {
  if (!picked) return false;
  if (readout && readout.norad === picked.norad && (readout.state === 'TRACK' || readout.state === 'COAST')) return true;
  const sat = list?.sats.find((s) => s.norad === picked.norad);
  return !!sat && (sat.el != null && sat.el > 0 || sat.passes.some((p) => p.aos <= now && p.los > now));
}

/* ── the station's reception history (the sky plot's cells) ─────────────────────────────────────────────────────── */
export type PassGrade = 'good' | 'fair' | 'poor' | 'none';
/** The trail's colours: green = this station usually decodes there, amber = marginal, grey = rarely, none = no history
 *  (drawn in the panel's own ink, faint). */
export const PASS_GRADE_CSS: Record<Exclude<PassGrade, 'none'>, string> = { good: 'rgb(60,200,80)', fair: 'rgb(240,170,40)', poor: 'rgb(130,130,130)' };
export const PASS_GRADE_WORD: Record<PassGrade, string> = { good: 'usually decodes', fair: 'marginal', poor: 'rarely decodes', none: 'no history' };
export const PASS_NO_HISTORY = 'No reception history yet; this shows geometry only';
/** The history: the cells, and per 45° sector × elevation band sums for where a cell alone is too thin. */
export interface SkyHistory { cells: SkyCells; sectors: Map<string, { n: number; grey: number; snr: number }>; dots: number }
const EL_BANDS = [0, 20, 45, 90];
const bandOf = (el: number) => (el < EL_BANDS[1] ? 0 : el < EL_BANDS[2] ? 1 : 2);
const sectorOf = (az: number) => Math.floor((((az % 360) + 360) % 360 + 22.5) / 45) % 8;
/** The cells → the history (null / no dots in any cell = none). */
export function skyHistory(cells: SkyCells | null | undefined): SkyHistory | null {
  if (!cells) return null;
  const sectors = new Map<string, { n: number; grey: number; snr: number }>();
  let dots = 0;
  for (const [i, x] of cells.cells) {
    if (!(x.n + x.grey)) continue;
    dots += x.n + x.grey;
    const b = skyCellBounds(cells, i);
    const k = `${sectorOf((b.az0 + b.az1) / 2)}:${bandOf((b.el0 + b.el1) / 2)}`;
    const s = sectors.get(k) ?? { n: 0, grey: 0, snr: 0 };
    s.n += x.n; s.grey += x.grey; s.snr += (x.mean ?? 0) * x.n;
    sectors.set(k, s);
  }
  return dots ? { cells, sectors, dots } : null;
}
/** A patch of sky's grade: heard in ≥ 60 % of its tracked 5-s buckets (and not just scraping, ≥ 3 dB mean) = good, ≥ 20 % =
 *  marginal, less = rarely; under `min` dots = not enough to say. */
function gradeOf(n: number, grey: number, meanSnr: number | null, min: number): PassGrade | null {
  const tot = n + grey;
  if (tot < min) return null;
  const rate = n / tot;
  if (rate >= 0.6 && (meanSnr == null || meanSnr >= 3)) return 'good';
  return rate >= 0.2 ? 'fair' : 'poor';
}
/** The grade at an azimuth / elevation: its own cell where that holds ≥ 2 dots, else its 45° sector and elevation band
 *  (≥ 4 dots — the ring of sectors when the heat map is sparse), else 'none'. */
export function skyGradeAt(h: SkyHistory | null, az: number, el: number): PassGrade {
  if (!h) return 'none';
  const x = h.cells.cells.get(skyCellIndex(h.cells, az, Math.max(0, el)));
  const own = x ? gradeOf(x.n, x.grey, x.mean, 2) : null;
  if (own) return own;
  const s = h.sectors.get(`${sectorOf(az)}:${bandOf(Math.max(0, el))}`);
  return (s && gradeOf(s.n, s.grey, s.n ? s.snr / s.n : null, 4)) || 'none';
}
/** The circular mean azimuth of some samples */
function meanAz(az: number[]): number {
  let x = 0, y = 0; for (const a of az) { x += Math.cos(a * DEG); y += Math.sin(a * DEG); }
  return ((Math.atan2(y, x) / DEG) + 360) % 360;
}
export interface PassVerdict { grade: PassGrade; text: string; frac: Record<PassGrade, number>; history: boolean }
/** ★★ THE ONE-LINE VERDICT (the row and the preview): where the pass spends its time against this station's history. */
export function passVerdict(pt: PassTrack, h: SkyHistory | null): PassVerdict {
  const m = `max ${pt.maxEl.toFixed(0)}°`;
  const frac: Record<PassGrade, number> = { good: 0, fair: 0, poor: 0, none: 0 };
  if (!h) { frac.none = 1; return { grade: 'none', text: `${PASS_NO_HISTORY} · ${m}`, frac, history: false }; }
  const az: Record<PassGrade, number[]> = { good: [], fair: [], poor: [], none: [] };
  for (const s of pt.samples) { const g = skyGradeAt(h, s.az, s.el); frac[g]++; az[g].push(s.az); }
  const n = pt.samples.length || 1;
  for (const k of Object.keys(frac) as PassGrade[]) frac[k] /= n;
  const dir = (g: PassGrade) => compass8(meanAz(az[g]));
  const low = pt.maxEl < 15;
  if (frac.good >= 0.5) return { grade: 'good', text: `Mostly over your good sector (${dir('good')}) · ${m}`, frac, history: true };
  if (frac.poor >= 0.5) return { grade: 'poor', text: `${low ? 'Low and through' : 'Through'} your weak sector (${dir('poor')}) · ${m}`, frac, history: true };
  if (frac.none >= 0.5) return { grade: 'none', text: `Through sky this station has no history for (${dir('none')}): geometry only · ${m}`, frac, history: true };
  if (frac.fair >= 0.5) return { grade: 'fair', text: `${low ? 'Low and mostly' : 'Mostly'} through your marginal sector (${dir('fair')}) · ${m}`, frac, history: true };
  const parts = ([['good', 'good'], ['fair', 'marginal'], ['poor', 'weak'], ['none', 'no history']] as Array<[PassGrade, string]>)
    .filter(([g]) => frac[g] >= 0.2).map(([g, w]) => `${w} to the ${dir(g)}`);
  return { grade: frac.good >= 0.3 ? 'fair' : frac.poor >= 0.3 ? 'poor' : 'fair', text: `Mixed: ${parts.join(', ')} · ${m}`, frac, history: true };
}
/** Which sky plot kind to read for a bird's history: its own decoder's when it has one stored, else ORBCOMM's (the
 *  aerial's general picture — ORBCOMM is up nearly all the time, so it fills the dome first). */
export function passHistoryKinds(decoder: string | undefined): string[] {
  const own = decoder === 'lrpt' ? 'meteor' : decoder === 'aprs' ? 'aprs' : decoder === 'fctlm' ? 'funcube' : '';
  return own ? [own, 'orbcomm'] : ['orbcomm'];
}
/** "from ORBCOMM reception history" — which history the verdict used (the line under it) */
export function passHistorySource(kind: string, h: SkyHistory | null): string {
  if (!h) return '';
  const name = kind === 'meteor' ? 'Meteor' : kind === 'aprs' ? 'APRS' : kind === 'funcube' ? 'FUNcube' : 'ORBCOMM';
  return `from this station's ${name} reception history (${h.dots.toLocaleString('en-GB')} dots, 30 days)`;
}
/** The key under the preview: what the trail's colours mean */
export const PASS_KEY_TEXT = 'trail: green = this station usually decodes there · amber = marginal · grey = rarely · faint = no history';

/* ── the trail and the polar plot ─────────────────────────────────────────────────────────────────────────────────── */
/** The samples up to `t` grouped by grade: consecutive runs (each run shares its end point with the next, so no gaps). */
export function passTrailRuns(pt: PassTrack, h: SkyHistory | null, t = pt.los): Array<{ grade: PassGrade; pts: PassSample[] }> {
  const out: Array<{ grade: PassGrade; pts: PassSample[] }> = [];
  for (const s of pt.samples) {
    if (s.t > t) break;
    const g = skyGradeAt(h, s.az, s.el);
    const last = out[out.length - 1];
    if (last && last.grade === g) last.pts.push(s);
    else { if (last) last.pts.push(s); out.push({ grade: g, pts: [s] }); }
  }
  if (t > pt.samples[0].t && t < pt.los && out.length) out[out.length - 1].pts.push(passSampleAt(pt, t));
  return out;
}
/** The pass on the polar sky plot (N up, E right, the rim the horizon — skyPlotXY): one path per grade run, the AOS / MAX
 *  / LOS marks. The moving dot is placed per frame with skyPlotXY (a transform). */
export function passSkyPaths(pt: PassTrack, h: SkyHistory | null, cx: number, cy: number, R: number):
  { runs: Array<{ grade: PassGrade; d: string }>; aos: [number, number]; los: [number, number]; max: [number, number] } {
  const f = (n: number) => n.toFixed(1);
  const runs = passTrailRuns(pt, h).map((r) => ({ grade: r.grade, d: r.pts.map((s, k) => { const [x, y] = skyPlotXY(s.az, s.el, cx, cy, R); return `${k ? 'L' : 'M'}${f(x)} ${f(y)}`; }).join('') }));
  const tc = passSampleAt(pt, pt.tca);
  return { runs, aos: skyPlotXY(pt.aosAz, 0, cx, cy, R), los: skyPlotXY(pt.losAz, 0, cx, cy, R), max: skyPlotXY(tc.az, tc.el, cx, cy, R) };
}
/** The key of a pass (the row ▶'s and the track cache's) */
export const passKey = (norad: number, aos: number) => `${norad}:${Math.round(aos)}`;
/** How many future passes get their track asked for (for the rows' verdicts) — the first few hearable ones, once each. */
export const PASS_VERDICT_ROWS = 6;

/* ── the panel's rules (both clients) ─────────────────────────────────────────────────────────────────────────────── */
/** What a row's ▶ previews: the pass, the downlink (its Doppler), its decoder (which history). */
export interface PassPreviewRow { norad: number; tx: number; name: string; hz: number; decoder?: string; aos: number; los: number }
/** One preview per pass AND downlink: "25544:1795609401:0" */
export const previewId = (r: { norad: number; aos: number; tx: number }) => `${passKey(r.norad, r.aos)}:${r.tx}`;
/** A timetable row → its ▶, or null: only a FUTURE pass (not up now) on a downlink this radio hears, with the receiver's
 *  position known (no position = no geometry to show). */
export function passPreviewRow(r: { sat: { norad: number; name: string; tx: unknown[] }; tx: { label: string; hz: number; decoder?: string }; i: number;
                                    grey: boolean; up: boolean; next?: { aos: number; los: number } }, rxKnown: boolean): PassPreviewRow | null {
  if (r.grey || r.up || !r.next || !rxKnown) return null;
  return { norad: r.sat.norad, tx: r.i, name: r.sat.tx.length > 1 ? `${r.sat.name} · ${r.tx.label}` : r.sat.name, hz: r.tx.hz,
           decoder: r.tx.decoder || undefined, aos: r.next.aos, los: r.next.los };
}
/** ▶ pressed: the row already previewing → back to live; a live pass on the pick → no preview, the note; else this one
 *  (replacing any other — one at a time). */
export function passPreviewPress(curId: string | null, id: string, live: boolean): { id: string | null; note: string } {
  if (curId === id) return { id: null, note: '' };
  if (live) return { id: null, note: PASS_LIVE_NOW };
  return { id, note: '' };
}
/** The beat between bringing the preview into view and the bird starting to move (ms) — so the start is not missed. */
export const PASS_START_DELAY_MS = 350;
