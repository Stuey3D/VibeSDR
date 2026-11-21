/**
 * digiMapShared.ts — V12 beta90 THE DIGI MODES MAP's pure parts, shared by the web client (web/client/src/digiMap.ts) and
 * the app (SDRScreen → MapOverlay kind 'digi'): the time windows, a caller's overlay cleaned, and the APRS stations as the
 * map draws them. docs/v12/DIGI-MODES-MAP.md.
 */

/** ★ An extra layer a caller draws on the map — the satellite client's orbit track and footprint, the stations it heard
 *  through the bird. Numbers only (and short labels), cleaned again by the map page. */
export interface DigiMapOverlay {
  /** The toggle's name ("ISS PASS"), ≤ 24 characters. */
  label: string;
  /** Polylines of [lat, lon] — an orbit's ground track (split at the antimeridian by the caller). */
  lines?: [number, number][][];
  /** Circles — a footprint (km radius) around a sub-satellite point. */
  circles?: { lat: number; lon: number; km: number }[];
  /** Points with a label — the satellite now, a station. */
  points?: { lat: number; lon: number; label: string }[];
  /** A colour for all of it (#rgb / #rrggbb), default the map's cyan. */
  colour?: string;
}

export const DIGI_WINDOWS: { id: string; label: string; words: string }[] = [
  { id: '1h', label: '1H', words: 'the last hour' },
  { id: '24h', label: '24H', words: 'the last 24 hours' },
  { id: '7d', label: '7D', words: 'today and the 7 days before' },
  { id: '30d', label: '30D', words: 'today and the 30 days before' },
  { id: 'all', label: 'ALL', words: 'everything this server has kept' },
];
export function digiWindowOf(v: unknown): string { const s = String(v ?? ''); return DIGI_WINDOWS.some((w) => w.id === s) ? s : '24h'; }

const okNum = (v: unknown, lo: number, hi: number): number | null => { const n = Number(v); return v == null || !Number.isFinite(n) ? null : Math.max(lo, Math.min(hi, n)); };
const okTxt = (v: unknown, n: number) => String(v ?? '').replace(/[<>&"'`\\\u0000-\u001f]/g, '').slice(0, n);

/** ★ An overlay cut to its fields and bounds (a caller's bug must not put an object where a number goes). */
export function digiOverlayClean(o: unknown): DigiMapOverlay | null {
  const x = o as any;
  if (!x || typeof x !== 'object') return null;
  const ll = (p: any): [number, number] | null => {
    const la = okNum(p?.[0], -90, 90), lo = okNum(p?.[1], -540, 540);
    return la == null || lo == null ? null : [la, lo];
  };
  const lines = Array.isArray(x.lines) ? x.lines.slice(0, 16).map((l: any) => (Array.isArray(l) ? l.slice(0, 2000).map(ll).filter(Boolean) : []))
    .filter((l: any[]) => l.length >= 2) : [];
  const circles = Array.isArray(x.circles) ? x.circles.slice(0, 16).map((c: any) => {
    const la = okNum(c?.lat, -90, 90), lo = okNum(c?.lon, -180, 180), km = okNum(c?.km, 0, 20000);
    return la == null || lo == null || km == null ? null : { lat: la, lon: lo, km };
  }).filter(Boolean) : [];
  const points = Array.isArray(x.points) ? x.points.slice(0, 500).map((p: any) => {
    const la = okNum(p?.lat, -90, 90), lo = okNum(p?.lon, -180, 180);
    return la == null || lo == null ? null : { lat: la, lon: lo, label: okTxt(p?.label, 32) };
  }).filter(Boolean) : [];
  const colour = /^#[0-9a-fA-F]{3}([0-9a-fA-F]{3})?$/.test(String(x.colour ?? '')) ? String(x.colour) : undefined;
  if (!lines.length && !circles.length && !points.length) return null;
  return { label: okTxt(x.label, 24) || 'OVERLAY', lines, circles, points, ...(colour ? { colour } : {}) };
}

/** One APRS station as the map draws it. */
export interface DigiAprsPoint {
  key: string; call: string; lat: number; lon: number;
  /** seconds since it was last heard */
  age: number;
  kind: string; symName: string; comment: string; status: string; km: number | null;
  speed: number | null; course: number | null; alt: number | null;
  /** its symbol (tracks.ts aprsIcon — our own SVG) */
  svg: string;
  trail: [number, number][];
}
/** ★ Our own SVG symbol only: an <svg> with no script, handler, link or foreign content (the map page checks again). */
export function digiSvgOk(s: unknown): string {
  const v = String(s ?? '');
  return v.length < 4000 && /^<svg [^]*<\/svg>$/.test(v) && !/<script|<foreignObject|\son\w+\s*=|javascript:|href\s*=/i.test(v) ? v : '';
}
/** The APRS list's stations with a position → the map's points (newest first, at most 1000). */
export function digiAprsPoints(stations: any[] | null | undefined, svgOf: (st: any) => string,
                               trailOf: (key: string) => [number, number][] | undefined, kmOf: (lat: number, lon: number) => number | null): DigiAprsPoint[] {
  if (!Array.isArray(stations)) return [];
  const out: DigiAprsPoint[] = [];
  for (const st of stations) {
    if (!st || st.killed) continue;
    const lat = okNum(st.lat, -90, 90), lon = okNum(st.lon, -180, 180);
    if (lat == null || lon == null) continue;
    const trail = (trailOf(String(st.key)) || []).slice(-200).filter((p) => Array.isArray(p) && Number.isFinite(p[0]) && Number.isFinite(p[1]));
    out.push({
      key: okTxt(st.key, 40), call: okTxt(st.name || st.from || st.key, 40), lat, lon, age: okNum(st.age, 0, 1e9) ?? 0,
      kind: okTxt(st.kind, 16), symName: okTxt(st.symName, 40), comment: okTxt(st.comment, 120), status: okTxt(st.status, 120),
      km: kmOf(lat, lon), speed: okNum(st.speed, 0, 5000), course: okNum(st.course, 0, 360), alt: okNum(st.alt, -500, 100000),
      svg: digiSvgOk(svgOf(st)), trail,
    });
  }
  out.sort((a, b) => a.age - b.age);
  return out.slice(0, 1000);
}
