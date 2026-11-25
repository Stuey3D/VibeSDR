/**
 * vorMap.ts — ★★ V12 THE VOR / ILS MAP (docs/v12/VOR-MAP.md): the words, the list rows, the map's items and the receiver FIX
 * both clients draw for a VibeServer's VOR map mode ({"type":"vormap"} — the radio sweeping 108–117.95 MHz window by window,
 * every 50 kHz channel decoded at once, android/app/src/main/cpp/vibe_vormap_service.h). One copy: the app's MapOverlay
 * ('vormap', the shared track layer) and the web's TrackView (web/client/src/vormap.ts) read the same functions.
 *
 * ★★ A NAME IS EARNED ON AIR: the server names a beacon only by the ident it read from it, matched to a station published on
 *    that channel; one with no ident read is "unidentified" and has no place on the map (the list only) — never a guess.
 * ★★★ beta94 THE BEARING IS THE SECOND WITNESS: a listed ident read ONCE plus the measured radial matching the radial THAT
 *    station should send to the receiver (its position and the station's, the station's variation) names it too — the server
 *    says why (idBy / reads / expRadial / idRadial) and the card says it in words (vorIdentWhy), so a person sees why it was
 *    trusted: "ident read once · radial matches (054° vs 060° expected)".
 * ★★ THE FIX: two or more VOR radials from DIFFERENT stations, heard this sweep, cross where the receiver is (radialFix).
 *    Several crossings make a cluster: their weighted centre and how far they spread. The words say how sure it is — each
 *    radial is good to about a degree and a half (the station's own alignment and our reading), so the error grows with the
 *    distance from the station and as the radials cross more shallowly.
 * ★ Everything off the air or the network is escaped where it meets HTML.
 */

import { radialFix } from './navaids';
import { flagEmoji } from './adsbInfo';
import { type LatLon, type TrackItem, type TrackRow } from './tracks';

export interface VorMapStation { ident: string; name: string; cc: string; lat: number; lon: number; variation: number; dme: string; km10: number | null; brg: number | null }
export interface VorMapBeacon {
  hz: number; kind: 'vor' | 'loc'; win: number; ident: string; reading: string; votes: number; held: 0 | 1 | 2;
  radial: number | null; sigma: number | null; trueBrg: number | null; ddm: number | null; sdm: number | null;
  level: number; first: number; last: number; fresh: boolean; wpm: number | null; station: VorMapStation | null;
  /** ★ beta94 why the ident was trusted: 'votes' (three readings alike), 'reads' (a listed ident read twice), 'radial' (read
   *  once + the radial matched the station's expected one); '' = not identified / an older server. */
  idBy: '' | 'votes' | 'reads' | 'radial'; reads: number; expRadial: number | null; idRadial: number | null;
}
export interface VorMapReport {
  now: number; rate: number; window: number; windows: number; wlo: number; whi: number; centre: number; sweep: number;
  inWin: number; dwell: number; progress: number; pos: boolean; wins: Array<[number, number]>; heard: number; identified: number;
  dropped: number; starting: boolean; beacons: VorMapBeacon[];
}

const num = (v: unknown, lo: number, hi: number): number | null => (typeof v === 'number' && Number.isFinite(v) && v >= lo && v <= hi ? v : null);
const str = (v: unknown, n: number): string => (typeof v === 'string' ? v.replace(/[\u0000-\u001f\u007f]/g, '').slice(0, n) : '');
const ident = (v: unknown): string => str(v, 8).replace(/[^A-Z0-9]/g, '').slice(0, 6);
export function esc(s: unknown): string {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string));
}

function stationOf(d: any): VorMapStation | null {
  if (!d || typeof d !== 'object') return null;
  const lat = num(d.lat, -90, 90), lon = num(d.lon, -180, 180);
  if (lat == null || lon == null) return null;
  return { ident: ident(d.ident), name: str(d.name, 80), cc: str(d.cc, 2).replace(/[^A-Z]/g, ''), lat, lon, variation: num(d.var, -180, 180) ?? 0,
           dme: str(d.dme, 5), km10: num(d.km10, 0, 40000), brg: num(d.brg, 0, 360) };
}
/** ★ The server's report, cut to its fields (null = not one). */
export function vorMapSanitise(m: any): VorMapReport | null {
  if (!m || typeof m !== 'object' || m.type !== 'vormap') return null;
  const beacons: VorMapBeacon[] = (Array.isArray(m.beacons) ? m.beacons : []).slice(0, 200).flatMap((b: any) => {
    if (!b || typeof b !== 'object') return [];
    const hz = num(b.hz, 107.9e6, 118.1e6);
    if (hz == null) return [];
    const held = b.held === 1 ? 1 : b.held === 2 ? 2 : 0;
    return [{
      hz, kind: b.kind === 'loc' ? 'loc' : 'vor', win: num(b.win, 0, 64) ?? 0, ident: ident(b.ident), reading: ident(b.reading),
      votes: num(b.votes, 0, 1e4) ?? 0, held, radial: num(b.radial, 0, 360), sigma: num(b.sigma, 0, 180), trueBrg: num(b.trueBrg, 0, 360),
      ddm: num(b.ddm, -1, 1), sdm: num(b.sdm, 0, 2), level: num(b.level, -250, 50) ?? -200, first: num(b.first, 0, 1e11) ?? 0,
      last: num(b.last, 0, 1e11) ?? 0, fresh: b.fresh === true, wpm: num(b.wpm, 1, 40), station: stationOf(b.station),
      idBy: b.idBy === 'votes' || b.idBy === 'reads' || b.idBy === 'radial' ? b.idBy : '', reads: num(b.reads, 0, 1e4) ?? 0,
      expRadial: num(b.expRadial, 0, 360), idRadial: num(b.idRadial, 0, 360),
    } as VorMapBeacon];
  });
  const wins: Array<[number, number]> = (Array.isArray(m.wins) ? m.wins : []).slice(0, 64)
    .map((w: any) => [num(w?.[0], 0, 2e8) ?? 0, num(w?.[1], 0, 2e8) ?? 0] as [number, number]);
  return {
    now: num(m.now, 0, 1e11) ?? 0, rate: num(m.rate, 0, 1e9) ?? 0, window: num(m.window, 0, 64) ?? 0, windows: num(m.windows, 0, 64) ?? 0,
    wlo: num(m.wlo, 0, 2e8) ?? 0, whi: num(m.whi, 0, 2e8) ?? 0, centre: num(m.centre, 0, 2e8) ?? 0, sweep: num(m.sweep, 0, 1e9) ?? 0,
    inWin: num(m.inWin, 0, 1e4) ?? 0, dwell: num(m.dwell, 0, 1e4) ?? 30, progress: num(m.progress, 0, 1) ?? 0, pos: m.pos === true,
    wins, heard: num(m.heard, 0, 1e4) ?? beacons.length, identified: num(m.identified, 0, 1e4) ?? 0, dropped: num(m.dropped, 0, 1e15) ?? 0,
    starting: m.starting === 1 || m.starting === true, beacons,
  };
}

export const mhz = (hz: number): string => (hz / 1e6).toFixed(2);
const pad3 = (d: number) => String(Math.round(((d % 360) + 360) % 360) % 360).padStart(3, '0');
const C8 = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
const compass8 = (d: number) => C8[Math.round((((d % 360) + 360) % 360) / 45) % 8];
/** "12 s" / "4 min" / "2.1 h". */
export function vorAgo(nowS: number, tS: number): string {
  const d = Math.max(0, Math.round(nowS - tS));
  return d < 60 ? `${d} s` : d < 3600 ? `${Math.round(d / 60)} min` : `${(d / 3600).toFixed(1)} h`;
}
/** ★ The name the list and the map give it: the ident read off the air, else "unidentified". */
export function vorName(b: VorMapBeacon): string { return b.ident || 'unidentified'; }
/** ★ What it is reading: "054° FROM" for a VOR (magnetic, as transmitted), the localiser's side for an ILS; '' = nothing held. */
export function vorReading(b: VorMapBeacon): string {
  if (b.kind === 'vor' && b.radial != null) return `${pad3(b.radial)}° FROM`;
  if (b.kind === 'loc' && b.ddm != null) {
    const pct = Math.min(999, Math.round(Math.abs(b.ddm) / 0.155 * 100));
    const side = Math.abs(b.ddm) < 0.004 ? 'on the course line' : b.ddm > 0 ? 'left of the course' : 'right of the course';
    return `DDM ${b.ddm >= 0 ? '+' : ''}${b.ddm.toFixed(3)} · ${side} (${pct} %)`;
  }
  return '';
}
/** The station's words: "Daventry (GB)", a localiser's runway where the open data names it, or why there is none. */
export function vorStationWords(b: VorMapBeacon): string {
  if (b.station) return `${b.station.name}${b.station.cc ? ` (${b.station.cc})` : ''}`;
  if (b.ident) return b.kind === 'loc' ? 'not in the open data (most runways are not)' : 'not in the published list';
  return b.reading ? `reading “${b.reading}”…` : 'ident not read yet';
}
const times = (n: number) => (n === 1 ? 'once' : n === 2 ? 'twice' : `${n} times`);
/** ★★ beta94 WHY IT WAS TRUSTED, in words (the card's Ident row, the web row's tooltip): "ident read once · radial matches
 *  (054° vs 060° expected)", "ident read twice · listed on this channel", "read 3 times alike". '' = not identified, or a
 *  server from before beta94 (which does not say). */
export function vorIdentWhy(b: VorMapBeacon): string {
  if (!b.ident) return '';
  const geo = b.expRadial != null && b.idRadial != null ? `radial matches (${pad3(b.idRadial)}° vs ${pad3(b.expRadial)}° expected)` : '';
  if (b.idBy === 'radial') return `ident read ${times(Math.max(1, b.reads))} · ${geo || 'radial matches'}`;
  if (b.idBy === 'reads') return [`ident read ${times(Math.max(2, b.reads))} · listed on this channel`, geo].filter(Boolean).join(' · ');
  if (b.idBy === 'votes') return [`read ${times(Math.max(3, b.votes))} alike`, geo].filter(Boolean).join(' · ');
  return '';
}
/** Distance in nautical miles from the server's place, as the server gives it (to 10 km). */
export function vorRange(b: VorMapBeacon): string {
  const k = b.station?.km10;
  if (k == null) return '';
  const nm = k / 1.852;
  return `~${nm < 10 ? nm.toFixed(0) : Math.round(nm / 5) * 5} NM`;
}

/** ★ The list: LOWEST FREQUENCY FIRST (a row keeps its place as it updates), every beacon heard. */
export function vorMapRowsOrdered(r: VorMapReport | null): VorMapBeacon[] {
  return r ? [...r.beacons].sort((a, b) => a.hz - b.hz) : [];
}
/** The status line: where the sweep is, how far round, how many heard. */
export function vorMapStatus(r: VorMapReport | null): string {
  if (!r) return 'starting the sweep of 108–118 MHz…';
  const n = r.windows || r.wins.length || 1;
  const w = r.wins[r.window];
  const where = w ? `${mhz(w[0] + 25000)}–${mhz(w[1] - 25000)} MHz` : '';
  const held = Math.min(Math.round(r.inWin), Math.round(r.dwell));
  return `Window ${r.window + 1} of ${n}${where ? ` · ${where}` : ''} · ${held} of ${Math.round(r.dwell)} s · sweep ${Math.round(r.progress * 100)} %`
    + ` · ${r.heard} heard${r.heard ? `, ${r.identified} identified` : ''}${r.dropped > 0 ? ' · the decoder is behind' : ''}`;
}
/** How long a whole sweep takes (the key's words): "a sweep takes 2½ min". */
export function vorSweepWords(windows: number, dwell: number): string {
  const s = Math.max(1, windows) * Math.max(1, dwell);
  return s < 90 ? `${Math.round(s)} s` : `${(s / 60).toFixed(s % 60 ? 1 : 0)} min`;
}
export const VORMAP_EMPTY = 'Sweeping the navaid band, 108–118 MHz, a window at a time — each VOR or localiser appears as it is heard, '
  + 'named once its Morse ident is read (twice for a listed station, about 20 s — or once, when its radial matches where you are; '
  + 'three times otherwise). A whole sweep takes a few minutes.';
/** ★ Said on the key (Mode / DECODERS) and in the mode list. */
export const VORMAP_HINT = 'VOR / ILS map — the whole navaid band (108–118 MHz) swept window by window, every VOR and localiser decoded at once: '
  + 'each station on the map with its radial towards you, and your position where radials from two stations cross.';

// ── the FIX ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
export interface VorFix {
  lat: number; lon: number; km: number;        ///< the best estimate and its uncertainty (km, about one sigma)
  stations: string[];                          ///< the idents whose radials made it
  crossings: number;                           ///< pairwise crossings used
  angle: number;                               ///< the widest crossing angle (deg) — 90 is best, under 20 poor
  words: string;                               ///< plain words for the card / the status
}
/** ★ Each radial's own spread: the reading's sigma and the station's alignment (ICAO: about ±1.4° at 95 %) together. */
const RADIAL_SIGMA_DEG = 1.5;
const R_KM = 6371;
const toRad = Math.PI / 180;
function distKm(a: LatLon, b: LatLon): number {
  const p1 = a.lat * toRad, p2 = b.lat * toRad, dl = (b.lon - a.lon) * toRad;
  const h = Math.sin((p2 - p1) / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2;
  return 2 * R_KM * Math.asin(Math.min(1, Math.sqrt(h)));
}
function bearing(a: LatLon, b: LatLon): number {
  const p1 = a.lat * toRad, p2 = b.lat * toRad, dl = (b.lon - a.lon) * toRad;
  return (Math.atan2(Math.sin(dl) * Math.cos(p2), Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl)) / toRad + 360) % 360;
}
/** The radials a fix may use: VORs heard THIS sweep, named (a station's position), with a true bearing — one per station. */
export function fixRadials(r: VorMapReport | null): Array<{ ident: string; lat: number; lon: number; brg: number; sigma: number }> {
  if (!r) return [];
  const by = new Map<string, { ident: string; lat: number; lon: number; brg: number; sigma: number; last: number }>();
  for (const b of r.beacons) {
    if (b.kind !== 'vor' || !b.fresh || !b.station || b.trueBrg == null) continue;
    const k = `${b.station.ident}@${b.station.lat.toFixed(3)},${b.station.lon.toFixed(3)}`;
    const cur = by.get(k);
    if (!cur || b.last > cur.last) by.set(k, { ident: b.station.ident, lat: b.station.lat, lon: b.station.lon, brg: b.trueBrg, sigma: b.sigma ?? 0, last: b.last });
  }
  return [...by.values()].map(({ last: _l, ...x }) => x);
}
/** ★★ Where the radials cross, and how sure that is (null = fewer than two stations, or no crossing ahead of them). */
export function vorFix(r: VorMapReport | null, maxKm = 600): VorFix | null {
  const rs = fixRadials(r);
  if (rs.length < 2) return null;
  const pts: Array<{ lat: number; lon: number; err: number; angle: number }> = [];
  const used = new Set<string>();
  for (let i = 0; i < rs.length; i++) {
    for (let j = i + 1; j < rs.length; j++) {
      const a = rs[i], b = rs[j];
      const x = radialFix(a, b, maxKm);
      if (!x) continue;
      const p = { lat: x[0], lon: x[1] };
      const da = distKm(a, p), db = distKm(b, p);
      // the crossing angle: between the two radials' directions where they meet
      const ta = (bearing(a, p) + 0) % 360, tb = bearing(b, p);
      let ang = Math.abs(((ta - tb + 540) % 360) - 180);
      if (ang > 90) ang = 180 - ang;
      const sa = Math.hypot(a.sigma, RADIAL_SIGMA_DEG) * toRad, sb = Math.hypot(b.sigma, RADIAL_SIGMA_DEG) * toRad;
      const err = Math.hypot(da * Math.sin(sa), db * Math.sin(sb)) / Math.max(Math.sin(Math.max(ang, 1) * toRad), 0.017);
      pts.push({ lat: p.lat, lon: p.lon, err, angle: ang });
      used.add(a.ident); used.add(b.ident);
    }
  }
  if (!pts.length) return null;
  // ★ the weighted centre (1/err²) and the larger of the formal error and the crossings' own spread
  let sw = 0, la = 0, lo = 0;
  const lon0 = pts[0].lon;
  for (const q of pts) {
    const w = 1 / Math.max(q.err, 0.05) ** 2;
    sw += w; la += w * q.lat; lo += w * (((q.lon - lon0 + 540) % 360) - 180);
  }
  const fix = { lat: la / sw, lon: ((lon0 + lo / sw + 540) % 360) - 180 };
  const formal = Math.sqrt(1 / sw);
  const spread = pts.length > 1 ? Math.sqrt(pts.reduce((s, q) => s + distKm(q, fix) ** 2, 0) / pts.length) : 0;
  const km = Math.max(formal, spread, 0.5);
  const angle = Math.max(...pts.map((q) => q.angle));
  const stations = [...used].sort();
  const near = km < 10 ? km.toFixed(1) : String(Math.round(km));
  let words = pts.length === 1
    ? `Your position from the ${stations.join(' and ')} radials: within about ${near} km (they cross at ${Math.round(angle)}°).`
    : `Your position from ${stations.length} VORs (${stations.join(', ')}): within about ${near} km — ${pts.length} crossings, their centre.`;
  if (angle < 20) words += ' The radials cross at a shallow angle, so this fix is poor along their line.';
  else if (km > 25) words += ' The stations are far off, so a degree of radial is several km here.';
  return { lat: fix.lat, lon: fix.lon, km, stations, crossings: pts.length, angle, words };
}
/** The fix in one line for the status / list head; '' with one radial or none. */
export function vorFixLine(r: VorMapReport | null): string {
  const f = vorFix(r);
  if (f) return `FIX ±${f.km < 10 ? f.km.toFixed(1) : Math.round(f.km)} km from ${f.stations.join(' + ')}`;
  const n = fixRadials(r).length;
  return n === 1 ? 'One radial so far — a second station is needed for a fix' : '';
}

// ── the map ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
const BRIGHT = '#ffb000', FADED = '#a08050', FIXC = '#7cff8a';
/** ★ A VOR: the compass-rose hexagon; a localiser: a runway arrow. Faded = not heard this sweep. */
export function vorIcon(b: VorMapBeacon): { html: string; w: number; h: number } {
  const c = b.fresh ? BRIGHT : FADED, o = b.fresh ? 1 : 0.5;
  if (b.kind === 'loc') {
    return { html: `<svg width="18" height="20" viewBox="0 0 18 20" aria-hidden="true" opacity="${o}"><path d="M9 1 16 18 9 14 2 18Z" fill="${c}" stroke="#000" stroke-width="1"/></svg>`, w: 18, h: 20 };
  }
  return { html: `<svg width="22" height="20" viewBox="0 0 22 20" aria-hidden="true" opacity="${o}"><path d="M6 1h10l5 9-5 9H6L1 10Z" fill="rgba(0,0,0,0.55)" stroke="${c}" stroke-width="2"/>`
    + `<circle cx="11" cy="10" r="2.6" fill="${c}"/></svg>`, w: 22, h: 20 };
}
export function vorFixIcon(): { html: string; w: number; h: number } {
  return { html: `<svg width="24" height="24" viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="7" fill="none" stroke="${FIXC}" stroke-width="2"/>`
    + `<path d="M12 1v7M12 16v7M1 12h7M16 12h7" stroke="${FIXC}" stroke-width="2"/></svg>`, w: 24, h: 24 };
}
/** ★ A radial on the map: from the station along its TRUE bearing, a little past the receiver (great circle). */
export function vorRadialPath(b: VorMapBeacon, steps = 24): Array<[number, number]> {
  if (b.kind !== 'vor' || !b.station || b.trueBrg == null) return [];
  const km = Math.max(60, Math.min(400, (b.station.km10 ?? 150) + 40));
  const p1 = b.station.lat * toRad, l1 = b.station.lon * toRad, t = b.trueBrg * toRad;
  const out: Array<[number, number]> = [];
  for (let i = 0; i <= steps; i++) {
    const d = (km * i / steps) / R_KM;
    const p2 = Math.asin(Math.sin(p1) * Math.cos(d) + Math.cos(p1) * Math.sin(d) * Math.cos(t));
    const l2 = l1 + Math.atan2(Math.sin(t) * Math.sin(d) * Math.cos(p1), Math.cos(d) - Math.sin(p1) * Math.sin(p2));
    out.push([p2 / toRad, ((l2 / toRad + 540) % 360) - 180]);
  }
  return out;
}
/** A circle (the fix's uncertainty) as a polygon. */
function circle(c: LatLon, km: number, n = 36): Array<[number, number]> {
  const out: Array<[number, number]> = [];
  const p1 = c.lat * toRad, l1 = c.lon * toRad, d = km / R_KM;
  for (let i = 0; i <= n; i++) {
    const t = 2 * Math.PI * i / n;
    const p2 = Math.asin(Math.sin(p1) * Math.cos(d) + Math.cos(p1) * Math.sin(d) * Math.cos(t));
    const l2 = l1 + Math.atan2(Math.sin(t) * Math.sin(d) * Math.cos(p1), Math.cos(d) - Math.sin(p1) * Math.sin(p2));
    out.push([p2 / toRad, ((l2 / toRad + 540) % 360) - 180]);
  }
  return out;
}
const row = (k: string, v: string) => `<div class="r"><span class="k">${esc(k)}</span><span class="v">${esc(v)}</span></div>`;
/** The card (HTML, escaped) — the map's and the list's. */
export function vorCard(b: VorMapBeacon, nowS: number): string {
  const fl = b.station?.cc && /^[A-Z]{2}$/.test(b.station.cc) ? flagEmoji(b.station.cc) + ' ' : '';
  let h = `<div class="t">${esc(fl)}<b>${esc(vorName(b))}</b> ${esc(b.station ? b.station.name : '')}</div>`;
  h += row('Frequency', `${mhz(b.hz)} MHz · ${b.kind === 'loc' ? 'ILS localiser' : 'VOR'}`);
  if (b.kind === 'vor' && b.radial != null) {
    h += row('Radial', `${b.radial.toFixed(1)}° FROM (magnetic) ±${(b.sigma ?? 0).toFixed(1)}°`);
    if (b.trueBrg != null) h += row('True', `${b.trueBrg.toFixed(1)}° — the line on the map`);
  }
  if (b.kind === 'loc' && b.ddm != null) h += row('Deviation', vorReading(b));
  h += row('Station', vorStationWords(b));
  if (b.station?.dme) h += row('DME', b.station.dme);
  const rg = vorRange(b);
  if (rg) h += row('Distance', `${rg}${b.station?.brg != null ? ` ${compass8(b.station.brg)}` : ''} (to 10 km)`);
  const why = vorIdentWhy(b);
  h += row('Ident', b.ident ? `${b.ident} — ${why || `read off the air${b.votes ? ` (${b.votes}× alike)` : ''}`}` : b.reading ? `reading “${b.reading}” — not confirmed yet` : 'not read yet');
  h += row('Level', `${b.level.toFixed(0)} dBFS`);
  h += row('Heard', `${b.fresh ? 'this sweep' : 'an earlier sweep'} · ${vorAgo(nowS, b.last)} ago`);
  return h;
}
export function vorFixCard(f: VorFix): string {
  return `<div class="t"><b>FIX</b> — where the radials cross</div>${row('Position', `${f.lat.toFixed(3)}, ${f.lon.toFixed(3)}`)}`
    + row('Within', `about ${f.km < 10 ? f.km.toFixed(1) : Math.round(f.km)} km`) + row('From', f.stations.join(', '))
    + `<div class="raw">${esc(f.words)}</div>`;
}
/** ★ The map's items (the shared track layer): every NAMED beacon at its published position — a VOR with its radial drawn
 *  towards the receiver — and the FIX with its circle of uncertainty. Unidentified ones are in the list only. */
export function vorMapItems(r: VorMapReport | null): TrackItem[] {
  if (!r) return [];
  const nowS = r.now || Date.now() / 1000;
  const items: TrackItem[] = vorMapRowsOrdered(r).filter((b) => !!b.station).map((b) => {
    const path = vorRadialPath(b);
    const it: TrackItem = { key: String(b.hz), lat: b.station!.lat, lon: b.station!.lon, _ic: vorIcon(b), _lb: vorName(b), _cd: vorCard(b, nowS),
      _aria: `${vorName(b)} ${b.station!.name}, ${mhz(b.hz)} MHz${vorReading(b) ? ', ' + vorReading(b) : ''}`, _rk: b.fresh ? 2 : 1 };
    if (path.length) it._sh = { kind: 'route', pts: path, colour: b.fresh ? BRIGHT : FADED, faded: !b.fresh, weight: 2 };
    return it;
  });
  const f = vorFix(r);
  if (f) items.push({ key: 'fix', lat: f.lat, lon: f.lon, _ic: vorFixIcon(), _lb: 'FIX', _cd: vorFixCard(f), _aria: f.words, _rk: 3,
                      _sh: { kind: 'area', pts: circle(f, f.km), colour: FIXC, fillOpacity: 0.12, weight: 1.4 } });
  return items;
}
/** The app's list rows (TrackLiveList), the web's rows' fields. */
export function vorMapTrackRows(r: VorMapReport | null): TrackRow[] {
  if (!r) return [];
  const nowS = r.now || Date.now() / 1000;
  return vorMapRowsOrdered(r).map((b) => ({
    key: String(b.hz), colour: b.fresh ? BRIGHT : FADED, flag: b.station?.cc && /^[A-Z]{2}$/.test(b.station.cc) ? flagEmoji(b.station.cc) : '',
    name: vorName(b), second: [`${mhz(b.hz)} ${b.kind === 'loc' ? 'LOC' : 'VOR'}`, b.station ? b.station.name : vorStationWords(b), vorReading(b)].filter(Boolean).join(' · '),
    sp: `${b.level.toFixed(0)} dB`, trend: 0, range: vorRange(b), brg: b.station?.brg != null ? compass8(b.station.brg) : '', noPos: !b.station,
    age: vorAgo(nowS, b.last), em: false,   // ★ (em is an emergency — a SART; faded is the colour)
  }));
}
