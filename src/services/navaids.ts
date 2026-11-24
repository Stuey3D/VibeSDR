/**
 * navaids.ts — V12 VOR / ILS and 406 MHz beacons, the PURE half shared by the app (ModeSelector, DecoderClient) and the web
 * client (web/client/src/main.ts): the hints, the status lines, the servers' {"type":"vor"} / {"type":"sarsat"} statuses cut to
 * their fields, the compass words and the radial's line for a map. The decoding — and the PRIVACY RULE — are the server's
 * (android/app/src/main/cpp/vibe_nav_run.h, vibe_sarsat_run.h; docs/v12/VOR-ILS.md, docs/v12/SARSAT.md): a client is never
 * sent what it must not show (a beacon's identity and position reach the owner's own lines only), so nothing here filters.
 *
 * Wire (decoder socket), "vor":
 *   0x01 text — a line when a bearing / deviation is first held, the ident confirmed, and once a minute
 *   0x03 | u8 state — 0 searching · 1 VOR bearing held · 2 ILS deviation held
 *   {"type":"vor","data":{chan, expect:"vor"|"loc"|"gs"|"none", kind:"vor"|"loc"|"gs"|"none", rate, secs,
 *     radial?, sigma?, trueBrg?, ddm?, sdm?, pair?, ident, reading, wpm?, station?:{ident,name,cc,lat,lon,var,dme,km10?},
 *     listed:[station…], credit, q:{…}}} once a second
 * "sarsat":
 *   0x01 text — one line per burst (the owner's carries the Hex ID, identity, position) and the note on attach
 *   {"type":"sarsat","data":{bursts, tests, rate, lines, refused, last?:{rf, country, countryName, type, protocol, test,
 *     selfTest, long}}}
 */

export interface VorStation { ident: string; name: string; cc: string; lat: number; lon: number; variation: number; dme: string; km10: number | null }
export interface VorStatus {
  chan: string; expect: 'vor' | 'loc' | 'gs' | 'none'; kind: 'vor' | 'loc' | 'gs' | 'none';
  radial: number | null; sigma: number | null; trueBrg: number | null; ddm: number | null; sdm: number | null; pair: string;
  ident: string; reading: string; wpm: number | null; station: VorStation | null; listed: VorStation[]; rate: number; secs: number;
  /** ★ V12 11-24: set by vorHold — this is the last good reading, held this many seconds (drawn dimmed, with its age). */
  heldSecs?: number;
}
const num = (v: unknown, lo: number, hi: number): number | null => (typeof v === 'number' && Number.isFinite(v) && v >= lo && v <= hi ? v : null);
const str = (v: unknown, n: number): string => (typeof v === 'string' ? v.slice(0, n) : '');
const kindOf = (v: unknown): VorStatus['kind'] => (v === 'vor' || v === 'loc' || v === 'gs' ? v : 'none');
function stationOf(d: any): VorStation | null {
  if (!d || typeof d !== 'object') return null;
  const lat = num(d.lat, -90, 90), lon = num(d.lon, -180, 180);
  if (lat == null || lon == null) return null;
  return { ident: str(d.ident, 6), name: str(d.name, 60), cc: str(d.cc, 2), lat, lon, variation: num(d.var, -180, 180) ?? 0,
           dme: str(d.dme, 5), km10: num(d.km10, 0, 40000) };
}
/** ★★ V12 11-24 (Stuart: "the compass doesnt stay on the screen long enough") — the server sends a status every second, and
 *  any second without a fresh reading (the Morse ident keying, a fade, the estimator settling) used to blank the compass /
 *  needle. Now the last good reading stays up, dimmed with its age, for VOR_HOLD_SEC on the same channel; a fresh reading
 *  replaces it, a retune or the hold running out clears it. */
export const VOR_HOLD_SEC = 60;
export function vorDrawable(s: VorStatus | null): boolean {
  return !!s && ((s.kind === 'vor' && s.radial != null) || ((s.kind === 'loc' || s.kind === 'gs') && s.ddm != null));
}
export interface VorHoldState { good: VorStatus | null; at: number }
export function vorHold(h: VorHoldState, next: VorStatus | null, nowMs: number): VorStatus | null {
  if (vorDrawable(next)) { h.good = next; h.at = nowMs; return next; }
  const g = h.good;
  if (g && next && g.chan === next.chan && nowMs - h.at <= VOR_HOLD_SEC * 1000) {
    // ★ the held reading, with the newest ident / station words (an ident read during the gap is news)
    return { ...g, ident: next.ident || g.ident, reading: next.reading || g.reading, station: next.station ?? g.station,
             listed: next.listed.length ? next.listed : g.listed, heldSecs: Math.round((nowMs - h.at) / 1000) };
  }
  if (!next || (g && g.chan !== next.chan) || nowMs - h.at > VOR_HOLD_SEC * 1000) h.good = null;
  return next;
}
/** ★ The server's status, cut to its fields. */
export function vorSanitise(d: any): VorStatus | null {
  if (!d || typeof d !== 'object') return null;
  return {
    chan: str(d.chan, 10), expect: kindOf(d.expect), kind: kindOf(d.kind),
    radial: num(d.radial, 0, 360), sigma: num(d.sigma, 0, 180), trueBrg: num(d.trueBrg, 0, 360),
    ddm: num(d.ddm, -1, 1), sdm: num(d.sdm, 0, 2), pair: str(d.pair, 10),
    ident: str(d.ident, 6), reading: str(d.reading, 8), wpm: num(d.wpm, 1, 40),
    station: stationOf(d.station), listed: Array.isArray(d.listed) ? d.listed.slice(0, 4).map(stationOf).filter(Boolean) as VorStation[] : [],
    rate: num(d.rate, 0, 1e7) ?? 0, secs: num(d.secs, 0, 1e7) ?? 0,
  };
}

export const VOR_HINT = 'Tune a VOR (108–117.95 MHz), an ILS localiser (108.10–111.95, odd tenths) or its glideslope (329–335 MHz) in AM. '
  + 'A VOR gives the radial you are on FROM the station — the magnetic bearing it transmits, and the true one with the '
  + 'station’s declination — and its Morse ident names it from the published list (OurAirports, public domain). '
  + 'An ILS gives how far off the course or glide path you are (DDM). Two VORs’ radials cross where you are.';

/** ★ ILS words: which side, and how much of full scale (0.155 DDM localiser, 0.175 glideslope). */
export function ilsWords(kind: 'loc' | 'gs', ddm: number): { side: string; pct: number } {
  const full = kind === 'gs' ? 0.175 : 0.155;
  const pct = Math.min(999, Math.round(Math.abs(ddm) / full * 100));
  if (Math.abs(ddm) < 0.004) return { side: kind === 'gs' ? 'on the glide path' : 'on the course line', pct };
  if (kind === 'gs') return { side: ddm > 0 ? 'above the glide path — fly down' : 'below the glide path — fly up', pct };
  return { side: ddm > 0 ? 'left of the course — fly right' : 'right of the course — fly left', pct };
}
/** "247° FROM LON" — the big readout. */
export function vorReadout(s: VorStatus | null): string {
  if (!s) return '';
  if (s.kind === 'vor' && s.radial != null) {
    const r = String(Math.round(s.radial) % 360).padStart(3, '0');
    return `${r}° FROM ${s.station?.ident || s.ident || s.chan}`;
  }
  if ((s.kind === 'loc' || s.kind === 'gs') && s.ddm != null) {
    const w = ilsWords(s.kind, s.ddm);
    return `${s.kind === 'gs' ? 'GS' : 'LOC'} ${s.ddm >= 0 ? '+' : ''}${s.ddm.toFixed(3)} DDM · ${w.side} (${w.pct} %)`;
  }
  return '';
}
/** The status line. */
export function vorStateText(st: number, s: VorStatus | null): string {
  if (!s) return 'listening for a VOR or ILS…';
  const where = s.expect === 'vor' ? 'a VOR channel' : s.expect === 'loc' ? `an ILS localiser channel${s.pair ? ` (glideslope ${s.pair})` : ''}`
              : s.expect === 'gs' ? `a glideslope channel${s.pair ? ` (localiser ${s.pair})` : ''}` : 'not a VOR or ILS channel';
  const name = s.station ? `${s.station.ident} ${s.station.name}` : s.ident ? `ident ${s.ident}` : s.reading ? `reading “${s.reading}”…` : '';
  if (s.kind === 'vor' && s.radial != null) {
    const t = s.trueBrg != null ? ` · true ${s.trueBrg.toFixed(1)}°` : '';
    return `${s.chan} MHz · radial ${s.radial.toFixed(1)}° ±${(s.sigma ?? 0).toFixed(1)}°${t}${name ? ` · ${name}` : ''}`;
  }
  if (s.kind === 'loc' || s.kind === 'gs') return `${s.chan} MHz · ${vorReadout(s)}${name ? ` · ${name}` : ''}`;
  return `${s.chan} MHz, ${where} · ${st ? 'held' : 'searching'}…${name ? ` · ${name}` : ''}`;
}
/** ★ The radial's line for a map: from the station, along the TRUE bearing, `km` long (great circle). [] without both. */
export function radialLine(s: VorStatus | null, km = 300, steps = 24): Array<[number, number]> {
  if (!s || !s.station || s.trueBrg == null) return [];
  const R = 6371, rad = Math.PI / 180;
  const p1 = s.station.lat * rad, l1 = s.station.lon * rad, t = s.trueBrg * rad;
  const out: Array<[number, number]> = [];
  for (let i = 0; i <= steps; i++) {
    const d = (km * i / steps) / R;
    const p2 = Math.asin(Math.sin(p1) * Math.cos(d) + Math.cos(p1) * Math.sin(d) * Math.cos(t));
    const l2 = l1 + Math.atan2(Math.sin(t) * Math.sin(d) * Math.cos(p1), Math.cos(d) - Math.sin(p1) * Math.sin(p2));
    out.push([p2 / rad, ((l2 / rad + 540) % 360) - 180]);
  }
  return out;
}
/** ★ Where two radials cross (the fix) — great circles from two stations along their true bearings; null if parallel or
 *  the crossing is behind either station / more than `maxKm` away. */
export function radialFix(a: { lat: number; lon: number; brg: number }, b: { lat: number; lon: number; brg: number }, maxKm = 600): [number, number] | null {
  const rad = Math.PI / 180;
  const v = (lat: number, lon: number) => [Math.cos(lat * rad) * Math.cos(lon * rad), Math.cos(lat * rad) * Math.sin(lon * rad), Math.sin(lat * rad)];
  const cross = (x: number[], y: number[]) => [x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]];
  const norm = (x: number[]) => { const m = Math.hypot(x[0], x[1], x[2]); return m > 0 ? x.map((c) => c / m) : x; };
  const east = (lon: number) => [-Math.sin(lon * rad), Math.cos(lon * rad), 0];
  const north = (lat: number, lon: number) => [-Math.sin(lat * rad) * Math.cos(lon * rad), -Math.sin(lat * rad) * Math.sin(lon * rad), Math.cos(lat * rad)];
  const dir = (p: typeof a) => { const e = east(p.lon), n = north(p.lat, p.lon), s = Math.sin(p.brg * rad), c = Math.cos(p.brg * rad); return [n[0] * c + e[0] * s, n[1] * c + e[1] * s, n[2] * c + e[2] * s]; };
  const pa = v(a.lat, a.lon), pb = v(b.lat, b.lon);
  const na = norm(cross(pa, dir(a))), nb = norm(cross(pb, dir(b)));
  let x = norm(cross(na, nb));
  if (!Number.isFinite(x[0]) || Math.hypot(...x) < 0.5) return null;
  // ★ ahead of a station = in the half of the sphere its direction of travel points into
  const ahead = (d: number[], q: number[]) => q[0] * d[0] + q[1] * d[1] + q[2] * d[2] > 0;
  if (!ahead(dir(a), x)) x = x.map((c) => -c);
  if (!ahead(dir(a), x) || !ahead(dir(b), x)) return null;
  const lat = Math.asin(x[2]) / rad, lon = Math.atan2(x[1], x[0]) / rad;
  const dist = (p: number[]) => Math.acos(Math.max(-1, Math.min(1, p[0] * x[0] + p[1] * x[1] + p[2] * x[2]))) * 6371;
  if (dist(pa) > maxKm || dist(pb) > maxKm) return null;
  return [lat, lon];
}

// ── 406 MHz ─────────────────────────────────────────────────────────────────────────────────────────────────────────
export interface SarsatStatus {
  bursts: number; tests: number; rate: number;
  last: { rf: number; country: number; countryName: string; type: string; protocol: string; test: boolean; selfTest: boolean; long: boolean } | null;
}
export function sarsatSanitise(d: any): SarsatStatus | null {
  if (!d || typeof d !== 'object') return null;
  const l = d.last && typeof d.last === 'object' ? d.last : null;
  return {
    bursts: num(d.bursts, 0, 1e7) ?? 0, tests: num(d.tests, 0, 1e7) ?? 0, rate: num(d.rate, 0, 1e7) ?? 0,
    last: l ? { rf: num(l.rf, 0, 1e10) ?? 0, country: num(l.country, 0, 1023) ?? 0, countryName: str(l.countryName, 60), type: str(l.type, 30),
                protocol: str(l.protocol, 40), test: l.test === true, selfTest: l.selfTest === true, long: l.long === true } : null,
  };
}
/** ★ Said on the key, in the box and on the setup page — the brief's "real alerts go to the authorities, not to us". */
export const SARSAT_NOTE = 'Real distress alerts are received by the COSPAS-SARSAT satellites and handled by the rescue authorities — '
  + 'VibeSDR reports nothing to anyone. Most beacons heard on the ground are tests. If you believe one is real and not already '
  + 'being handled, tell the coastguard or the police (in the UK 999 or 112).';
export const SARSAT_HINT = 'COSPAS-SARSAT 406 MHz distress beacons (EPIRB, ELT, PLB): tune 406.0–406.1 MHz (406.025, 406.028, 406.031, 406.037, '
  + '406.040 MHz…) in NFM. Every listener sees that a beacon was heard, its kind and country, and whether it is a test; '
  + 'its identity and encoded position are shown to the server’s owner only. ' + SARSAT_NOTE;
export function sarsatStateText(st: number, s: SarsatStatus | null): string {
  if (!s || !s.bursts) return st ? 'a burst…' : 'listening for 406 MHz beacons (a burst every ~50 s while one is active)';
  const l = s.last;
  const what = l ? `${l.type || 'beacon'} · ${l.countryName}${l.selfTest ? ' · SELF-TEST' : l.test ? ' · TEST' : ''}` : '';
  return `${s.bursts} burst${s.bursts === 1 ? '' : 's'} heard${s.tests ? ` (${s.tests} test${s.tests === 1 ? '' : 's'})` : ''}${what ? ` · last: ${what}` : ''}`;
}

/** ★ The web box's picture (an SVG string, theme colours from CSS variables): a VOR's compass card with the radial FROM the
 *  station drawn on it (TRUE when the station's declination is known, else magnetic — said under it); an ILS's needle on a
 *  five-dot scale (horizontal for a localiser, vertical for a glideslope). '' when nothing is held. */
export function navCardSvg(s: VorStatus | null): string {
  if (!s) return '';
  const e = (t: string) => t.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!));
  if (s.kind === 'vor' && s.radial != null) {
    const brg = s.trueBrg ?? s.radial;
    const a = (brg - 90) * Math.PI / 180, r = 52, cx = 64, cy = 64;
    let ticks = '';
    for (let d = 0; d < 360; d += 10) {
      const t = (d - 90) * Math.PI / 180, r0 = d % 30 === 0 ? 50 : 54;
      ticks += `<line x1="${(cx + r0 * Math.cos(t)).toFixed(1)}" y1="${(cy + r0 * Math.sin(t)).toFixed(1)}" x2="${(cx + 58 * Math.cos(t)).toFixed(1)}" y2="${(cy + 58 * Math.sin(t)).toFixed(1)}" stroke="var(--text-dim)" stroke-width="1"/>`;
    }
    const lab = (d: number, t: string) => { const q = (d - 90) * Math.PI / 180; return `<text x="${(cx + 42 * Math.cos(q)).toFixed(1)}" y="${(cy + 42 * Math.sin(q) + 4).toFixed(1)}" text-anchor="middle" font-size="10" fill="var(--text-dim)">${t}</text>`; };
    const x2 = cx + r * Math.cos(a), y2 = cy + r * Math.sin(a);
    const big = vorReadout(s);
    return `<svg viewBox="0 0 300 128" width="100%" height="128" role="img" aria-label="${e(big)}"${s.heldSecs != null ? ' opacity=".55"' : ''}>`
      + `<circle cx="${cx}" cy="${cy}" r="58" fill="none" stroke="var(--text-dim)"/>${ticks}${lab(0, 'N')}${lab(90, 'E')}${lab(180, 'S')}${lab(270, 'W')}`
      + `<line x1="${cx}" y1="${cy}" x2="${x2.toFixed(1)}" y2="${y2.toFixed(1)}" stroke="var(--amber)" stroke-width="3" stroke-linecap="round"/>`
      + `<circle cx="${cx}" cy="${cy}" r="4" fill="var(--amber)"/>`
      + `<text x="136" y="40" font-size="22" font-weight="600" fill="var(--text)">${e(big)}</text>`
      + `<text x="136" y="62" font-size="12" fill="var(--text-dim)">${e(s.station ? `${s.station.name} (${s.station.cc})${s.station.dme ? ` · DME ${s.station.dme}` : ''}` : s.ident ? `ident ${s.ident} — not in the list` : 'ident not read yet')}</text>`
      + `<text x="136" y="80" font-size="12" fill="var(--text-dim)">${e(`magnetic ${s.radial.toFixed(1)}° ±${(s.sigma ?? 0).toFixed(1)}°${s.trueBrg != null ? ` · true ${s.trueBrg.toFixed(1)}°` : ''}`)}</text>`
      + `<text x="136" y="98" font-size="11" fill="var(--text-dim)">${e(s.heldSecs != null ? `last reading ${s.heldSecs} s ago — waiting for the next` : s.trueBrg != null ? 'the card is drawn true north up' : 'drawn magnetic: the station is not listed')}</text>`
      + `</svg>`;
  }
  if ((s.kind === 'loc' || s.kind === 'gs') && s.ddm != null) {
    const full = s.kind === 'gs' ? 0.175 : 0.155;
    // ★ the needle shows where the COURSE is (fly toward it): 90 Hz dominant (DDM > 0) on a localiser = the course is to the right
    const k = Math.max(-1, Math.min(1, s.ddm / full));
    const w = ilsWords(s.kind, s.ddm);
    const horiz = s.kind === 'loc';
    let dots = '';
    for (let i = -2; i <= 2; i++) dots += horiz ? `<circle cx="${64 + i * 22}" cy="64" r="3" fill="var(--text-dim)"/>` : `<circle cx="64" cy="${64 + i * 22}" r="3" fill="var(--text-dim)"/>`;
    const p = 64 + (horiz ? k : -k) * 44;   // GS: 90 Hz dominant (above) → the path is below → needle down
    const needle = horiz ? `<line x1="${p.toFixed(1)}" y1="14" x2="${p.toFixed(1)}" y2="114" stroke="var(--amber)" stroke-width="3"/>`
                         : `<line x1="14" y1="${(128 - p).toFixed(1)}" x2="114" y2="${(128 - p).toFixed(1)}" stroke="var(--amber)" stroke-width="3"/>`;
    return `<svg viewBox="0 0 300 128" width="100%" height="128" role="img" aria-label="${e(w.side)}"${s.heldSecs != null ? ' opacity=".55"' : ''}>`
      + `<rect x="8" y="8" width="112" height="112" rx="56" fill="none" stroke="var(--text-dim)"/>${dots}${needle}`
      + `<text x="136" y="40" font-size="20" font-weight="600" fill="var(--text)">${e(`${s.kind === 'gs' ? 'GLIDESLOPE' : 'LOCALISER'} ${s.ddm >= 0 ? '+' : ''}${s.ddm.toFixed(3)}`)}</text>`
      + `<text x="136" y="62" font-size="12" fill="var(--text-dim)">${e(`${w.side} · ${w.pct} % of full scale`)}</text>`
      + `<text x="136" y="80" font-size="12" fill="var(--text-dim)">${e(s.station ? s.station.name : s.ident ? `ident ${s.ident}` : 'runway not in the open data')}</text>`
      + `<text x="136" y="98" font-size="11" fill="var(--text-dim)">${e(s.heldSecs != null ? `last reading ${s.heldSecs} s ago — waiting for the next` : s.pair ? `paired ${s.kind === 'gs' ? 'localiser' : 'glideslope'} ${s.pair} MHz` : '')}</text>`
      + `</svg>`;
  }
  return '';
}
