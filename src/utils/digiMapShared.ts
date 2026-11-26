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
  /** ★ beta95 its packets heard (the TOTALS' APRS line) */
  count?: number;
  kind: string; symName: string; comment: string; status: string; km: number | null;
  /** a weather station's report, as words ("12.3 °C · 81 % RH · wind 14 km/h SW · 1012.4 hPa"); '' when none */
  wx: string;
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
const C8 = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
/** ★ An APRS weather report as words — tracks.ts aprsCard's line (the same order and units). */
export function digiAprsWx(w: any): string {
  if (!w || typeof w !== 'object') return '';
  const n = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : null);
  const parts: string[] = [];
  const t = n(w.tempC), h = n(w.hum), ws = n(w.windKmh), wd = n(w.windDir), g = n(w.gustKmh), r = n(w.rain1hMm), p = n(w.hPa);
  if (t != null) parts.push(`${t.toFixed(1)} °C`);
  if (h != null) parts.push(`${Math.round(h)} % RH`);
  if (ws != null) parts.push(`wind ${Math.round(ws)} km/h${wd != null && ws > 0 ? ' ' + C8[Math.round((((wd % 360) + 360) % 360) / 45) % 8] : ''}`);
  if (g != null) parts.push(`gust ${Math.round(g)}`);
  if (r != null && r > 0) parts.push(`rain ${r.toFixed(1)} mm/h`);
  if (p != null) parts.push(`${p.toFixed(1)} hPa`);
  return parts.join(' · ');
}
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
      // ★ beta95 age = since LAST heard (the station's "seen"; its "age" is since first heard — the comment above was right,
      //   the field was not), and its packets for the TOTALS' APRS line
      key: okTxt(st.key, 40), call: okTxt(st.name || st.from || st.key, 40), lat, lon, age: okNum(st.seen ?? st.age, 0, 1e9) ?? 0,
      count: okNum(st.count, 0, 1e9) ?? 0,
      kind: okTxt(st.kind, 16), symName: okTxt(st.symName, 40), comment: okTxt(st.comment, 120), status: okTxt(st.status, 120),
      km: kmOf(lat, lon), wx: okTxt(digiAprsWx(st.wx), 120), speed: okNum(st.speed, 0, 5000), course: okNum(st.course, 0, 360), alt: okNum(st.alt, -500, 100000),
      svg: digiSvgOk(svgOf(st)), trail,
    });
  }
  out.sort((a, b) => a.age - b.age);
  return out.slice(0, 1000);
}

/* ★★ beta92 THE FIT KEY (Stuart, 2026-11-23: "spots map could do with the auto fit button too"; then "include the
 *  receiver"): the digi modes map's FIT is the shared AUTO FIT (src/utils/mapAutoFit.ts — the ADS-B map's glide). What
 *  it frames is said HERE: every station the filters SHOW (the mode, band, time and radio filters, the APRS key), not
 *  every station kept, and not a satellite pass's overlay — plus the receiver (the auto fit adds it). Never past
 *  DIGI_FIT_MAX_ZOOM — one station (or a few in one town) is a regional view, not a street.
 *  ★ AS A STRING because both map pages are their own documents (the app's WebView — MapOverlay.tsx — and the web's
 *  window — main.ts spotsMapHtml) and embed it as is; scripts/test_digi_map.ts runs this very text. Plain ES5, no
 *  backticks and no backslashes (it sits inside the pages' template literals). */
export const DIGI_FIT_MAX_ZOOM = 9;
export const DIGI_FIT_JS = `
function digiShownPoints(spots,isShown,aprs,aprsOn){
  var p=[];
  (spots||[]).forEach(function(s){if(s&&isShown(s))p.push([Number(s.plat),Number(s.plon)]);});
  if(aprsOn)(aprs||[]).forEach(function(a){if(a)p.push([Number(a.lat),Number(a.lon)]);});
  return p;
}
`;

/* ★★ beta92 THE GALLERY'S DIGI MODES MAP ROW, one copy for the web gallery (main.ts) and the app's (GalleryOverlay):
 *  "SPOTS · DIGI MODES MAP · 5,000 stations heard · FT8 4175 · WSPR 286 · …" with MAP, on page 1 of ALL and the spot
 *  kinds, from stations.json?window=all (its total and byMode). null = no row (an older server, the store off, nothing
 *  heard) — never a dead MAP key. */
export const DIGI_GAL_KINDS: readonly string[] = ['ft8', 'cw', 'psk', 'rttyskim'];
export const DIGI_GAL_SRC = 'Every station the digital modes, CW, PSK and RTTY decoders heard, by the day — background decoding and every listener';
export function digiGalCard(j: unknown): { title: string; src: string } | null {
  const x = j as any;
  if (!x || typeof x !== 'object' || x.off) return null;
  const total = Math.max(0, Math.round(Number(x.total) || 0));
  if (!total) return null;
  const by = x.byMode && typeof x.byMode === 'object' ? x.byMode as Record<string, unknown> : {};
  const modes = Object.entries(by).filter(([m, n]) => /^[A-Z0-9+-]{1,12}$/.test(m) && Number.isFinite(Number(n)))
    .map(([m, n]) => [m, Math.round(Number(n))] as [string, number])
    .sort((a, b) => b[1] - a[1] || (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)).slice(0, 6).map(([m, n]) => `${m} ${n}`).join(' · ');
  return { title: `DIGI MODES MAP · ${total.toLocaleString('en-GB')} station${total === 1 ? '' : 's'} heard${modes ? ' · ' + modes : ''}`, src: DIGI_GAL_SRC };
}
/** The stations.json question the row asks: every window, nothing new since tomorrow (the totals only, no stations). */
export function digiGalQuery(nowSec: number): string { return `/vibeserver/stations.json?window=all&since=${Math.floor(nowSec + 86400)}`; }
/** A spots summary card's MAP opens the window that holds its day (the card's hour is inside 24 H; older inside 7 D / 30 D). */
export function digiGalItemWindow(t: number, nowSec: number): string {
  const age = nowSec - (Number(t) || 0);
  return age < 86400 ? '24h' : age < 7 * 86400 ? '7d' : age < 30 * 86400 ? '30d' : 'all';
}
