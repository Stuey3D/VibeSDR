// skyPlot.ts — V12 2026-11-24 THE SKY PLOT, the clients' half (docs/v12/SKY-PLOT.md; the server is
// android/app/src/main/cpp/vibe_skyplot.h). One copy for the app (SatellitePanel, GalleryOverlay — react-native-svg) and
// the web page (web/client/src/satellites.ts and the gallery — an SVG string), so both draw the same dome.
//
// ★★★ WHAT IT IS: a polar dome — the outer ring the horizon, the centre the zenith, north up, east right (a map's view),
//     rings at 30° and 60° — with one dot per 5 s of decodes at the satellite's azimuth / elevation, coloured by SNR. It
//     ACCUMULATES across passes, so after a few days it shows where this station's aerial really hears: a loop's nulls, a
//     vertical's overhead hole, the house in the way. A bird tracked while NOTHING decoded leaves GREY dots — the blocked
//     directions are the point, so grey is drawn first, underneath.
// ★★ THE SNR RAMP is the server's (vibe_skyplot.h snrColour) — the same four stops, so the app, the web page and the
//    gallery's PNG agree: 0 dB deep blue · 6 teal · 12 green · 18+ yellow; linear between, clamped 0…18.
// ★ Pure (no React, no DOM) — scripts/test_orbcomm_app.ts.
import { logRadioChoices, type LogRadio } from './logRadio';

/** One dot: where the bird was, how well it was heard (null = tracked, nothing decoded), when, which bird. */
export interface SkyDot {
  az: number; el: number; snr: number | null; t: number; norad: number;
  /** decodes in the 5 s bucket (the stored view only) */
  count?: number;
  /** the radio id that heard it ('' = unknown — written before radios were recorded) */
  radio?: string;
}

export type Rgb = [number, number, number];
/** The server's four stops (dB → colour). */
export const SKY_SNR_STOPS: ReadonlyArray<readonly [number, Rgb]> = [
  [0, [50, 60, 200]], [6, [30, 170, 190]], [12, [60, 200, 80]], [18, [250, 225, 40]],
];
/** Tracked, nothing decoded. */
export const SKY_GREY: Rgb = [110, 110, 110];

/** The colour of an SNR (dB), exactly as the server's PNG draws it (its uint8 conversion truncates). */
export function skySnrRgb(snr: number | null | undefined): Rgb {
  if (snr == null || !Number.isFinite(snr)) return [...SKY_GREY] as Rgb;
  const s = Math.max(0, Math.min(18, snr));
  let k = 0;
  while (k < SKY_SNR_STOPS.length - 2 && s > SKY_SNR_STOPS[k + 1][0]) k++;
  const [s0, c0] = SKY_SNR_STOPS[k], [s1, c1] = SKY_SNR_STOPS[k + 1];
  const f = (s - s0) / (s1 - s0);
  return [0, 1, 2].map((i) => Math.floor(c0[i] + f * (c1[i] - c0[i]))) as Rgb;
}
export const rgbCss = (c: Rgb) => `rgb(${c[0]},${c[1]},${c[2]})`;
export const skySnrCss = (snr: number | null | undefined) => rgbCss(skySnrRgb(snr));

/** az/el (degrees) → x/y on a dome of radius R centred at (cx, cy): el 90 = the centre, el 0 = the rim, az 0 = up (north),
 *  az 90 = right (east). Below the horizon is held on the rim. */
export function skyPlotXY(azDeg: number, elDeg: number, cx: number, cy: number, R: number): [number, number] {
  const r = R * (90 - Math.max(0, Math.min(90, elDeg))) / 90;
  const a = (azDeg * Math.PI) / 180;
  return [cx + r * Math.sin(a), cy - r * Math.cos(a)];
}

const fin = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);
/** A dot from the wire's tenths of a degree; null when it is not one. */
function dotOf(az10: unknown, el10: unknown, snr: unknown, t: unknown, norad: unknown): SkyDot | null {
  if (!fin(az10) || !fin(el10)) return null;
  const az = ((az10 / 10) % 360 + 360) % 360, el = el10 / 10;
  if (el < -1 || el > 90) return null;
  return { az, el: Math.max(0, el), snr: fin(snr) ? Math.max(-127, Math.min(127, snr)) : null,
           t: fin(t) && t > 0 ? t : 0, norad: fin(norad) && norad > 0 ? Math.floor(norad) : 0 };
}

/** The LIVE dots an orbcomm_status carries — [[az10, el10, snr|null, t, norad], …], the ones new since the last status. */
export function parseSkyDotsLive(v: unknown, max = 64): SkyDot[] {
  if (!Array.isArray(v)) return [];
  const out: SkyDot[] = [];
  for (const d of v.slice(0, max)) {
    if (!Array.isArray(d)) continue;
    const x = dotOf(d[0], d[1], d[2], d[3], d[4]);
    if (x) out.push(x);
  }
  return out;
}

/** /vibeserver/skyplot.json's answer, checked. */
export interface SkyPlotData {
  kind: string; dots: SkyDot[];
  /** the radios worth a filter (logRadio.ts — two or more holding dots), else null: no chips */
  radios: LogRadio[] | null;
  /** dots the server holds that carry no radio */
  radioUnknown: number;
  count: number; truncated: boolean;
}
/** The stored view's cap (vibe_skyplot.h kMaxOut). */
export const SKY_MAX_DOTS = 20000;
/** {"v":1,"kind","dots":[[az10,el10,snr|null,t,kind,norad,count,radioIndex]…],"radioIds":[…],"radios":[…],"count","truncated"}
 *  → the dots (newest first, as sent), each with its radio id ('' where radioIndex is −1 or out of range). null = not one. */
export function parseSkyPlotJson(j: unknown): SkyPlotData | null {
  const o = j as Record<string, unknown> | null;
  if (!o || typeof o !== 'object' || !Array.isArray(o.dots)) return null;
  const ids = Array.isArray(o.radioIds) ? (o.radioIds as unknown[]).map((x) => (typeof x === 'string' && /^[0-9a-z]{1,16}$/.test(x) ? x : '')) : [];
  const dots: SkyDot[] = [];
  for (const d of (o.dots as unknown[]).slice(0, SKY_MAX_DOTS)) {
    if (!Array.isArray(d)) continue;
    const x = dotOf(d[0], d[1], d[2], d[3], d[5]);
    if (!x) continue;
    if (fin(d[6]) && d[6] >= 0) x.count = Math.floor(d[6]);
    const ri = fin(d[7]) ? Math.floor(d[7]) : -1;
    x.radio = ri >= 0 && ri < ids.length ? ids[ri] : '';
    dots.push(x);
  }
  return {
    kind: typeof o.kind === 'string' ? o.kind.replace(/[^a-z]/g, '').slice(0, 16) : '',
    dots, radios: logRadioChoices(o.radios),
    radioUnknown: fin(o.radioUnknown) && o.radioUnknown > 0 ? Math.floor(o.radioUnknown) : 0,
    count: fin(o.count) && o.count >= 0 ? Math.floor(o.count) : dots.length, truncated: o.truncated === true,
  };
}

/** Live dots joined to what is shown: newest first, bounded. */
export function skyPlotMerge(prev: SkyDot[], add: SkyDot[], max = SKY_MAX_DOTS): SkyDot[] {
  if (!add.length) return prev;
  const out = [...add].sort((a, b) => b.t - a.t).concat(prev);
  return out.length > max ? out.slice(0, max) : out;
}

/** "/vibeserver/skyplot.json?kind=orbcomm&radio=a1b2c3d4" (radio '' = every radio; days 0 = all kept). */
export function skyPlotQuery(kind: string, radio = '', days = 0, norad = 0, png = false): string {
  let q = `/vibeserver/skyplot.${png ? 'png' : 'json'}?kind=${encodeURIComponent(kind || 'orbcomm')}`;
  if (radio && /^[0-9a-z]{1,16}$/.test(radio)) q += `&radio=${radio}`;
  if (days > 0) q += `&days=${Math.floor(days)}`;
  if (norad > 0) q += `&norad=${Math.floor(norad)}`;
  return q;
}

/** The radio a client is on, from its base URL / path: a radio behind a front door lives at …/r/<id>. '' = the root
 *  (a one-radio server: its dots are its own, the filter is not needed). */
export function skyRadioFromBase(base: string | null | undefined): string {
  const m = String(base ?? '').match(/\/r\/([0-9a-z]{1,16})\/?(?:[?#].*)?$/);
  return m ? m[1] : '';
}

// ── the picture (one geometry, drawn by react-native-svg in the app and as an SVG string on the web page) ──────────────
export interface SkyScene {
  size: number; cx: number; cy: number; R: number;
  rings: { r: number; el: number }[];
  /** the N–S and E–W lines through the zenith */
  cross: string;
  labels: { text: string; x: number; y: number; anchor: 'start' | 'middle' | 'end' }[];
  /** in draw order: grey first (underneath), then the decodes oldest first (the newest on top) */
  dots: { x: number; y: number; r: number; fill: string; grey: boolean }[];
}
/** The scene in a size×size box (the dome inset by `pad` for the N/E/S/W labels). Dots past `maxDraw` (the oldest) are left
 *  out — a phone draws a few thousand circles comfortably, not twenty thousand. */
export function skyPlotScene(dots: SkyDot[], size = 300, maxDraw = 4000): SkyScene {
  const pad = Math.round(size * 0.07);
  const cx = size / 2, cy = size / 2, R = size / 2 - pad;
  const rings = [0, 30, 60].map((el) => ({ el, r: R * (90 - el) / 90 }));
  const cross = `M${(cx - R).toFixed(1)} ${cy.toFixed(1)}H${(cx + R).toFixed(1)}M${cx.toFixed(1)} ${(cy - R).toFixed(1)}V${(cy + R).toFixed(1)}`;
  const off = pad * 0.55;
  const labels: SkyScene['labels'] = [
    { text: 'N', x: cx, y: cy - R - off, anchor: 'middle' }, { text: 'S', x: cx, y: cy + R + off, anchor: 'middle' },
    { text: 'E', x: cx + R + off, y: cy, anchor: 'middle' }, { text: 'W', x: cx - R - off, y: cy, anchor: 'middle' },
    { text: '30°', x: cx + 3, y: cy - R * 60 / 90 - 3, anchor: 'start' }, { text: '60°', x: cx + 3, y: cy - R * 30 / 90 - 3, anchor: 'start' },
  ];
  const shown = dots.length > maxDraw ? dots.slice(0, maxDraw) : dots;   // newest first → the newest kept
  const rd = Math.max(1.6, size / 150), rg = Math.max(1, rd * 0.6);
  const out: SkyScene['dots'] = [];
  for (const pass of [0, 1])
    for (let i = shown.length - 1; i >= 0; i--) {   // oldest first, so the newest sit on top
      const d = shown[i];
      if ((d.snr == null) !== (pass === 0)) continue;
      const [x, y] = skyPlotXY(d.az, d.el, cx, cy, R);
      out.push({ x, y, r: pass === 0 ? rg : rd, fill: skySnrCss(d.snr), grey: pass === 0 });
    }
  return { size, cx, cy, R, rings, cross, labels, dots: out };
}

/** The scene as an SVG string (the web page) — the dome and its lines in `ink` (currentColor by default, so it follows
 *  the panel's text colour on every faceplate), the dots in the ramp's own colours. */
export function skyPlotSvg(s: SkyScene, label = 'Sky plot', ink = 'currentColor'): string {
  const f = (n: number) => n.toFixed(1);
  let h = `<svg viewBox="0 0 ${s.size} ${s.size}" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="${label.replace(/[<>&"]/g, '')}">`;
  h += `<circle cx="${f(s.cx)}" cy="${f(s.cy)}" r="${f(s.R)}" fill="${ink}" fill-opacity="0.07"/>`;
  h += `<path d="${s.cross}" stroke="${ink}" stroke-opacity="0.18" stroke-width="1" fill="none"/>`;
  for (const r of s.rings) h += `<circle cx="${f(s.cx)}" cy="${f(s.cy)}" r="${f(r.r)}" fill="none" stroke="${ink}" stroke-opacity="${r.el === 0 ? 0.6 : 0.28}" stroke-width="1"/>`;
  for (const d of s.dots) h += `<circle cx="${f(d.x)}" cy="${f(d.y)}" r="${f(d.r)}" fill="${d.fill}"/>`;
  const fs = Math.max(9, s.size / 26);
  for (const l of s.labels) {
    const small = /°$/.test(l.text);
    h += `<text x="${f(l.x)}" y="${f(l.y)}" font-size="${f(small ? fs * 0.8 : fs)}" text-anchor="${l.anchor}" dominant-baseline="middle" fill="${ink}" fill-opacity="${small ? 0.55 : 0.85}" font-family="system-ui,sans-serif">${l.text}</text>`;
  }
  return h + '</svg>';
}

/** The legend: the ramp's stops and the grey (the same words in both clients). */
export const SKY_LEGEND: { label: string; css: string }[] = [
  { label: '0 dB', css: skySnrCss(0) }, { label: '6', css: skySnrCss(6) }, { label: '12', css: skySnrCss(12) },
  { label: '18+ dB SNR', css: skySnrCss(18) }, { label: 'tracked, nothing decoded', css: skySnrCss(null) },
];
/** A CSS gradient of the ramp (the web legend's strip). */
export const SKY_RAMP_CSS = `linear-gradient(to right,${SKY_SNR_STOPS.map(([db, c]) => `${rgbCss(c)} ${Math.round(db / 18 * 100)}%`).join(',')})`;
/** What the plot is, in one line (under it, both clients). */
export const SKY_PLOT_WHAT = 'Where this aerial hears the satellites: one dot per 5 s of decodes at the bird’s place in the sky, coloured by SNR — N up, E right, the rim is the horizon. Grey = tracked, nothing decoded.';

/** "1,240 dots · 3 satellites · since 21 Nov" — '' when empty. */
export function skyPlotSummary(dots: SkyDot[], truncated = false): string {
  if (!dots.length) return '';
  const birds = new Set(dots.map((d) => d.norad).filter((n) => n > 0)).size;
  const grey = dots.filter((d) => d.snr == null).length;
  let first = Infinity;
  for (const d of dots) if (d.t > 0 && d.t < first) first = d.t;
  const since = Number.isFinite(first) ? new Date(first * 1000).toLocaleDateString('en-GB', { day: 'numeric', month: 'short' }) : '';
  return [`${dots.length.toLocaleString('en-GB')} dot${dots.length === 1 ? '' : 's'}${truncated ? ' (the newest)' : ''}`,
          birds ? `${birds} satellite${birds === 1 ? '' : 's'}` : '', grey ? `${grey.toLocaleString('en-GB')} grey` : '',
          since ? `since ${since}` : ''].filter(Boolean).join(' · ');
}
