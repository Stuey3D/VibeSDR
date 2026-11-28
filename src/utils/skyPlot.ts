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
  /** ★★★ beta94 the sky by POSITION (30 days, whatever the dot log still holds) — null = an older server */
  cells: SkyCells | null;
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
    cells: parseSkyCells(o.cells),
  };
}

/* ══ ★★★ beta94 THE HEATMAP (Stuart 11-25: "could keep it as tracks when less data then a heatmap when it gets more" ·
 *  "when in user mode you still see a track for the satellite being tracked, once the pass ends it goes to the heatmap").
 *  The server keeps every dot counted into a CELL of the sky for 30 days (vibe_skyplot.h namespace cells — an
 *  equal-area-ish grid, its per-band counts sent here, never copied). A cell is coloured by the MEAN of its dots' SNRs
 *  (each dot already the best of its 5 s, so the mean of those maxima is a high-percentile figure one freak burst cannot
 *  repaint); a cell tracked with nothing decoded is grey. AUTO (the default) = the server's rule: tracks until ≥ 30 % of
 *  the sky's cells are heard, averaging ≥ 3 dots each, then the heatmap — and while a pass is LIVE (its newest dot under
 *  SKY_LIVE_SEC old) that pass is drawn as a track on top; at LOS it has been merged into the cells and the heatmap is
 *  all that shows. TRACKS / HEATMAP override. ══ */
export interface SkyCell { n: number; grey: number; mean: number | null; max: number | null; lastT: number }
export interface SkyCells {
  elStep: number; az: number[]; start: number[]; total: number; days: number; bucketDays: number;
  /** the server's verdict, and its rule (re-judged here after live dots join) */
  auto: 'heatmap' | 'tracks'; heatCoverage: number; heatDotsPerCell: number;
  cells: Map<number, SkyCell>;
}
/** A newest dot younger than this = the pass is live (a status every second; a dot per 5 s; LOS leaves it to age out). */
export const SKY_LIVE_SEC = 60;
export type SkyMode = 'auto' | 'tracks' | 'heatmap';
export const SKY_MODES: { id: SkyMode; label: string }[] = [{ id: 'auto', label: 'AUTO' }, { id: 'tracks', label: 'TRACKS' }, { id: 'heatmap', label: 'HEATMAP' }];

export function parseSkyCells(v: unknown): SkyCells | null {
  const o = v as Record<string, unknown> | null;
  if (!o || typeof o !== 'object' || !Array.isArray(o.az) || !Array.isArray(o.c)) return null;
  const az = (o.az as unknown[]).map((x) => (fin(x) && x >= 1 && x <= 720 ? Math.floor(x) : 0));
  if (!az.length || az.some((x) => !x) || az.length > 90) return null;
  const elStep = fin(o.elStep) && o.elStep > 0 ? o.elStep : 90 / az.length;
  const start: number[] = []; let k = 0; for (const n of az) { start.push(k); k += n; } start.push(k);
  const cells = new Map<number, SkyCell>();
  for (const r of (o.c as unknown[]).slice(0, k)) {
    if (!Array.isArray(r) || !fin(r[0]) || r[0] < 0 || r[0] >= k) continue;
    const n = fin(r[1]) && r[1] > 0 ? Math.floor(r[1]) : 0, grey = fin(r[2]) && r[2] > 0 ? Math.floor(r[2]) : 0;
    if (!n && !grey) continue;
    cells.set(Math.floor(r[0]), { n, grey, mean: n && fin(r[3]) ? r[3] / 10 : null, max: n && fin(r[4]) ? r[4] : null, lastT: fin(r[5]) ? r[5] : 0 });
  }
  return {
    elStep, az, start, total: k, days: fin(o.days) && o.days > 0 ? Math.floor(o.days) : 30, bucketDays: fin(o.bucketDays) && o.bucketDays > 0 ? Math.floor(o.bucketDays) : 3,
    auto: o.auto === 'heatmap' ? 'heatmap' : 'tracks',
    heatCoverage: fin(o.heatCoverage) && o.heatCoverage > 0 ? o.heatCoverage : 0.3,
    heatDotsPerCell: fin(o.heatDotsPerCell) && o.heatDotsPerCell > 0 ? o.heatDotsPerCell : 3,
    cells,
  };
}
/** The cell of an az/el on the server's grid. */
export function skyCellIndex(c: SkyCells, azDeg: number, elDeg: number): number {
  const b = Math.max(0, Math.min(c.az.length - 1, Math.floor(Math.max(0, elDeg) / c.elStep)));
  const n = c.az[b];
  const a = ((azDeg % 360) + 360) % 360;
  return c.start[b] + Math.max(0, Math.min(n - 1, Math.floor(a / 360 * n)));
}
/** A cell's corners (degrees). */
export function skyCellBounds(c: SkyCells, idx: number): { az0: number; az1: number; el0: number; el1: number } {
  let b = 0; while (b + 1 < c.az.length && c.start[b + 1] <= idx) b++;
  const n = c.az[b], k = idx - c.start[b];
  return { az0: 360 * k / n, az1: 360 * (k + 1) / n, el0: b * c.elStep, el1: (b + 1) * c.elStep };
}
/** Live dots counted into a copy of the cells (the box's own pass joins the heatmap as it is heard — at LOS it is in). */
export function skyCellsAddDots(c: SkyCells | null, dots: SkyDot[]): SkyCells | null {
  if (!c || !dots.length) return c;
  const cells = new Map(c.cells);
  for (const d of dots) {
    const i = skyCellIndex(c, d.az, d.el);
    const x = { ...(cells.get(i) ?? { n: 0, grey: 0, mean: null, max: null, lastT: 0 }) };
    if (d.snr == null) x.grey++;
    else { x.mean = ((x.mean ?? 0) * x.n + d.snr) / (x.n + 1); x.n++; x.max = Math.max(x.max ?? -128, d.snr); }
    x.lastT = Math.max(x.lastT, d.t);
    cells.set(i, x);
  }
  return { ...c, cells, auto: skyCellsHeatDue({ ...c, cells }) ? 'heatmap' : 'tracks' };
}
/** The server's AUTO rule (vibe_skyplot.h cells::heatmapDue), with the figures it sent. */
export function skyCellsHeatDue(c: SkyCells): boolean {
  let heard = 0, dots = 0;
  for (const x of c.cells.values()) if (x.n + x.grey > 0) { heard++; dots += x.n + x.grey; }
  return heard > 0 && heard >= c.heatCoverage * c.total && dots / heard >= c.heatDotsPerCell;
}
/** The pass being heard NOW: the dots of `norad` (0 = the newest dot's bird), newest back through gaps under 2 min — only
 *  when its newest dot is under SKY_LIVE_SEC old. [] = nothing live (after LOS). */
export function skyLivePass(dots: SkyDot[], nowSec: number, norad = 0): SkyDot[] {
  const mine = dots.filter((d) => d.t > 0 && (norad ? d.norad === norad : true));
  if (!mine.length) return [];
  let newest = mine[0]; for (const d of mine) if (d.t > newest.t) newest = d;
  if (nowSec - newest.t > SKY_LIVE_SEC) return [];
  const bird = norad || newest.norad;
  const v = mine.filter((d) => d.norad === bird).sort((a, b) => b.t - a.t);
  const out: SkyDot[] = [];
  for (const d of v) { if (out.length && out[out.length - 1].t - d.t > 120) break; out.push(d); }
  return out;
}
/** What to draw: the base (tracks of every dot, or the heatmap) and the track on top (the live pass). */
export function skyViewOf(choice: SkyMode, cells: SkyCells | null, dots: SkyDot[], nowSec: number, norad = 0): { base: 'tracks' | 'heatmap'; overlay: SkyDot[]; live: boolean } {
  const live = skyLivePass(dots, nowSec, norad);
  const heat = !!cells && (choice === 'heatmap' || (choice === 'auto' && cells.auto === 'heatmap'));
  return heat ? { base: 'heatmap', overlay: live, live: live.length > 0 } : { base: 'tracks', overlay: [], live: live.length > 0 };
}
const COMPASS8 = ['north', 'north-east', 'east', 'south-east', 'south', 'south-west', 'west', 'north-west'];
/** The 40° azimuth window (10° steps) whose heard cells have the best n-weighted mean SNR (≥ 20 dots) — the server's
 *  cells::strongest. null = not enough yet. */
export function skyStrongest(c: SkyCells): { azLo: number; azHi: number; name: string } | null {
  let best = -1e9, at = -1;
  for (let ctr = 0; ctr < 360; ctr += 10) {
    let sum = 0, n = 0;
    for (const [i, x] of c.cells) {
      if (!x.n || x.mean == null) continue;
      const b = skyCellBounds(c, i), mid = (b.az0 + b.az1) / 2;
      const d = Math.abs(((mid - ctr + 540) % 360) - 180);
      if (d <= 20) { sum += x.mean * x.n; n += x.n; }
    }
    if (n < 20) continue;
    if (sum / n > best + 1e-9) { best = sum / n; at = ctr; }
  }
  if (at < 0) return null;
  const azLo = (at - 20 + 360) % 360, azHi = (at + 20) % 360;
  return { azLo, azHi, name: COMPASS8[Math.floor((at + 22) / 45) % 8] };
}
/** "heatmap · 30 days · 1,240 cells heard · strongest east (az 70–110°) · 12,345 dots" (tracks: the dot summary). */
export function skyHeatSummary(c: SkyCells, dotCount: number): string {
  let heard = 0; for (const x of c.cells.values()) if (x.n + x.grey > 0) heard++;
  const st = skyStrongest(c);
  return [`heatmap · ${c.days} days · ${heard.toLocaleString('en-GB')} cell${heard === 1 ? '' : 's'} heard`,
          st ? `strongest ${st.name} (az ${st.azLo}–${st.azHi}°)` : '',
          `${dotCount.toLocaleString('en-GB')} dot${dotCount === 1 ? '' : 's'}`].filter(Boolean).join(' · ');
}
/** The heatmap as SVG paths, ONE PER COLOUR (a dB step each, and grey) — a phone draws ~20 paths, not 2,000 cells. */
export function skyHeatPaths(c: SkyCells, cx: number, cy: number, R: number): { d: string; fill: string }[] {
  const by = new Map<string, string[]>();
  const f = (n: number) => n.toFixed(1);
  for (const [i, x] of c.cells) {
    if (!(x.n + x.grey)) continue;
    const fill = skySnrCss(x.n && x.mean != null ? Math.round(x.mean) : null);
    const b = skyCellBounds(c, i);
    const ro = R * (90 - b.el0) / 90, ri = R * (90 - Math.min(90, b.el1)) / 90;
    const p = (az: number, r: number) => { const a = az * Math.PI / 180; return `${f(cx + r * Math.sin(a))} ${f(cy - r * Math.cos(a))}`; };
    const seg = `M${p(b.az0, ro)}A${f(ro)} ${f(ro)} 0 0 1 ${p(b.az1, ro)}L${p(b.az1, ri)}` + (ri > 0.05 ? `A${f(ri)} ${f(ri)} 0 0 0 ${p(b.az0, ri)}` : '') + 'Z';
    const list = by.get(fill) ?? []; list.push(seg); by.set(fill, list);
  }
  // grey first (underneath, as the grey dots), then the ramp low → high
  return [...by.entries()].sort((a, b) => (a[0] === skySnrCss(null) ? -1 : b[0] === skySnrCss(null) ? 1 : 0)).map(([fill, segs]) => ({ fill, d: segs.join('') }));
}

/** Live dots joined to what is shown: newest first, bounded. */
export function skyPlotMerge(prev: SkyDot[], add: SkyDot[], max = SKY_MAX_DOTS): SkyDot[] {
  if (!add.length) return prev;
  const out = [...add].sort((a, b) => b.t - a.t).concat(prev);
  return out.length > max ? out.slice(0, max) : out;
}

/** "/vibeserver/skyplot.json?kind=orbcomm&radio=a1b2c3d4" (radio '' = every radio; days 0 = all kept). */
export function skyPlotQuery(kind: string, radio = '', days = 0, norad = 0, png = false, mode: SkyMode = 'auto', cellsOnly = false): string {
  let q = `/vibeserver/skyplot.${png ? 'png' : 'json'}?kind=${encodeURIComponent(kind || 'orbcomm')}`;
  if (cellsOnly && !png) q += '&dots=0';   // ★★★ beta97 the pass preview's verdict: the heat map's cells, no dots (an older server sends them anyway)
  if (png && mode !== 'auto') q += `&mode=${mode}`;   // ★★★ beta94 the server's picture in the view chosen
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
  /** ★★★ beta94 the heatmap's cells (one path per colour), under the rings; [] in TRACKS */
  cells: { d: string; fill: string }[];
  mode: 'tracks' | 'heatmap';
  rings: { r: number; el: number }[];
  /** the N–S and E–W lines through the zenith */
  cross: string;
  labels: { text: string; x: number; y: number; anchor: 'start' | 'middle' | 'end' }[];
  /** in draw order: grey first (underneath), then the decodes oldest first (the newest on top) */
  dots: { x: number; y: number; r: number; fill: string; grey: boolean }[];
}
/** The scene in a size×size box (the dome inset by `pad` for the N/E/S/W labels). Dots past `maxDraw` (the oldest) are left
 *  out — a phone draws a few thousand circles comfortably, not twenty thousand. */
export function skyPlotScene(dots: SkyDot[], size = 300, maxDraw = 4000,
                             heat?: { cells: SkyCells; overlay: SkyDot[] } | null): SkyScene {
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
  // ★★★ beta94 HEATMAP: the cells, and on top only the live pass's track (Stuart: "you still see a track for the satellite
  //     being tracked, once the pass ends it goes to the heatmap")
  const src = heat ? heat.overlay : dots;
  const shown = src.length > maxDraw ? src.slice(0, maxDraw) : src;   // newest first → the newest kept
  const rd = Math.max(1.6, size / 150), rg = Math.max(1, rd * 0.6);
  const out: SkyScene['dots'] = [];
  for (const pass of [0, 1])
    for (let i = shown.length - 1; i >= 0; i--) {   // oldest first, so the newest sit on top
      const d = shown[i];
      if ((d.snr == null) !== (pass === 0)) continue;
      const [x, y] = skyPlotXY(d.az, d.el, cx, cy, R);
      out.push({ x, y, r: pass === 0 ? rg : rd, fill: skySnrCss(d.snr), grey: pass === 0 });
    }
  return { size, cx, cy, R, rings, cross, labels, dots: out, cells: heat ? skyHeatPaths(heat.cells, cx, cy, R) : [], mode: heat ? 'heatmap' : 'tracks' };
}
/** The scene for a view choice — skyViewOf + skyPlotScene in one (every client calls this). */
export function skySceneFor(choice: SkyMode, cells: SkyCells | null, dots: SkyDot[], nowSec: number, size = 300, maxDraw = 4000, norad = 0): SkyScene {
  const v = skyViewOf(choice, cells, dots, nowSec, norad);
  return skyPlotScene(dots, size, maxDraw, v.base === 'heatmap' && cells ? { cells, overlay: v.overlay } : null);
}

/** The scene as an SVG string (the web page) — the dome and its lines in `ink` (currentColor by default, so it follows
 *  the panel's text colour on every faceplate), the dots in the ramp's own colours. */
export function skyPlotSvg(s: SkyScene, label = 'Sky plot', ink = 'currentColor'): string {
  const f = (n: number) => n.toFixed(1);
  let h = `<svg viewBox="0 0 ${s.size} ${s.size}" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="${label.replace(/[<>&"]/g, '')}">`;
  h += `<circle cx="${f(s.cx)}" cy="${f(s.cy)}" r="${f(s.R)}" fill="${ink}" fill-opacity="0.07"/>`;
  // ★★★ beta94 the heatmap, under the lines — each path stroked in its own colour so no hairline seams show between cells
  for (const c of s.cells) h += `<path d="${c.d}" fill="${c.fill}" stroke="${c.fill}" stroke-width="0.6" stroke-linejoin="round" data-heat="1"/>`;
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

/** ★★★ beta94 the line under the plot for a view choice: the heatmap's ("heatmap · 30 days · 1,240 cells heard · strongest
 *  east (az 70–110°) · 12,345 dots", "· the live pass on top" while one is), or the tracks' dot summary. */
export function skySummaryFor(choice: SkyMode, cells: SkyCells | null, dots: SkyDot[], nowSec: number, truncated = false, norad = 0): string {
  const v = skyViewOf(choice, cells, dots, nowSec, norad);
  if (v.base === 'heatmap' && cells) return skyHeatSummary(cells, dots.length) + (v.live ? ' · the live pass on top' : '');
  return skyPlotSummary(dots, truncated);
}
/** ★★★ beta94 the web page's AUTO / TRACKS / HEATMAP buttons (data-act="skymode", data-v = the mode). */
export function skyModesHtml(mode: SkyMode): string {
  return '<div class="sky-modes" role="group" aria-label="Sky plot view">'
    + SKY_MODES.map((m) => `<button type="button" data-act="skymode" data-v="${m.id}" class="btn${m.id === mode ? ' on' : ''}" aria-pressed="${m.id === mode}"`
      + ` title="${m.id === 'auto' ? 'Tracks until there is enough data, then the heatmap — with a live pass drawn on top' : m.id === 'tracks' ? 'Every dot along each pass' : 'Each patch of sky coloured by its average SNR over 30 days'}">${m.label}</button>`).join('')
    + '</div>';
}
/** The SKY_MODES value of a button's data-v ('auto' when not one). */
export const skyModeOf = (v: string | undefined): SkyMode => (v === 'tracks' || v === 'heatmap' ? v : 'auto');
