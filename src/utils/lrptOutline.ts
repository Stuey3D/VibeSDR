/**
 * lrptOutline.ts — METEOR LRPT's MAP OUTLINE in the clients (V12, 2026-11-24; docs/v12/LRPT-MAP-OVERLAY.md). One rule for
 * the app (GalleryOverlay, SatellitePanel) and the web page (main.ts, satellites.ts).
 *
 * ★★★ THE SERVER MAKES IT, THE CLIENTS DRAW IT. At a pass's end the server georeferences the picture from the satellite's
 *     own line times and its orbit (vibe_lrpt_overlay.h) and files the outline — coastlines, lakes, borders, a 5° grid with
 *     labels, in the PICTURE'S OWN PIXELS — as a small JSON item beside the plain PNG; each picture names it ("geo") or
 *     says why there is none ("gw"). Live, {"type":"lrpt_geo"} carries the same for the quarter-size preview.
 * ★★ A TOGGLE, OFF BY DEFAULT (Stuart, 2026-11-24: "map outline should be a toggle"): MAP OUTLINE: OFF / ON, remembered
 *    per viewer once they switch it (OUTLINE_KEY). The saved picture is never altered; DOWNLOAD with it ON asks the server
 *    for a copy with the outline burnt in (?outline=1 — vibe_lrpt_burn.h); OFF gives the plain picture.
 * ★ No dead control (AGENTS.md): no key where the picture has no outline. A pass whose checks failed shows the reason
 *   ("No map outline: the satellite's clock reads 3 h 0 min ahead of ours"); a picture saved before this feature shows
 *   nothing extra.
 * ★ Parsed defensively — numbers only, bounded — it is the server's JSON, but a client never trusts a shape.
 */

/** ★ THE DEFAULT (one constant — Stuart may flip it): the outline is OFF until the viewer turns it on. */
export const OUTLINE_DEFAULT_ON = false;
/** Where the viewer's choice is kept (AsyncStorage in the app, localStorage on the web page). */
export const OUTLINE_KEY = 'vibe.lrptOutline';
export function outlineFromStored(v: string | null | undefined): boolean {
  return v === '1' ? true : v === '0' ? false : OUTLINE_DEFAULT_ON;
}
export const outlineToStored = (on: boolean) => (on ? '1' : '0');
/** The key's words: it reads as a switch, saying what it is NOW. */
export const outlineKeyLabel = (on: boolean) => (on ? 'MAP OUTLINE: ON' : 'MAP OUTLINE: OFF');
export const outlineKeyA11y = (on: boolean) => (on ? 'Map outline is on. Hide the coastlines, borders and grid'
                                                   : 'Map outline is off. Show the coastlines, borders and grid over the picture');
/** The caption line for a picture whose pass has no outline (the server's reason, plain English). */
export const outlineWhyLine = (why: string) => `No map outline: ${why}`;

export interface OutlineLabel { x: number; y: number; text: string }
export interface Outline {
  w: number; h: number;
  sat: string;
  /** SVG path data per layer, in the picture's pixels (viewBox 0 0 w h). */
  coast: string; lake: string; border: string; grid: string;
  labels: OutlineLabel[];
  /** The polylines themselves (Skia / canvas drawing), [x0, y0, x1, y1, …] each. */
  lines: { coast: number[][]; lake: number[][]; border: number[][]; grid: number[][] };
  centre: [number, number] | null;
  points: number;
}
/** The clients' colours (the server's burnt-in copy uses the same, vibe_lrpt_burn.h). */
export const OUTLINE_INK = { coast: '#FFD600', lake: '#FFD600', border: '#FF8C3C', grid: '#96CDFF', shade: 'rgba(0,0,0,0.75)' };

const MAX_POINTS = 400_000;   // ★ bounded: a whole 16-minute pass is ~60 000
function lines(v: unknown, budget: { n: number }): number[][] {
  const out: number[][] = [];
  if (!Array.isArray(v)) return out;
  for (const l of v) {
    if (!Array.isArray(l) || l.length < 4) continue;
    const pts: number[] = [];
    for (let i = 0; i + 1 < l.length; i += 2) {
      const x = l[i], y = l[i + 1];
      if (typeof x !== 'number' || typeof y !== 'number' || !Number.isFinite(x) || !Number.isFinite(y)) continue;
      pts.push(x, y);
    }
    if (pts.length < 4) continue;
    budget.n += pts.length / 2;
    if (budget.n > MAX_POINTS) break;
    out.push(pts);
  }
  return out;
}
function pathOf(ls: number[][]): string {
  const parts: string[] = [];
  for (const l of ls) {
    let d = `M${l[0]} ${l[1]}`;
    for (let i = 2; i + 1 < l.length; i += 2) d += `L${l[i]} ${l[i + 1]}`;
    parts.push(d);
  }
  return parts.join('');
}
/** The server's outline JSON (text or parsed) → what a client draws; null when it is not an outline. */
export function parseOutline(src: unknown): Outline | null {
  let j: any = src;
  if (typeof src === 'string') { try { j = JSON.parse(src); } catch { return null; } }
  if (!j || typeof j !== 'object' || j.v !== 1) return null;
  const w = Number(j.w), h = Number(j.h);
  if (!(w > 0 && h > 0 && w < 100000 && h < 100000)) return null;
  const budget = { n: 0 };
  const L = { coast: lines(j.coast, budget), lake: lines(j.lake, budget), border: lines(j.border, budget), grid: lines(j.grid, budget) };
  const labels: OutlineLabel[] = [];
  if (Array.isArray(j.labels)) for (const a of j.labels.slice(0, 200)) {
    if (Array.isArray(a) && typeof a[0] === 'number' && typeof a[1] === 'number' && typeof a[2] === 'string' && a[2].length <= 12) labels.push({ x: a[0], y: a[1], text: a[2] });
  }
  const c = Array.isArray(j.centre) && typeof j.centre[0] === 'number' && typeof j.centre[1] === 'number' ? [j.centre[0], j.centre[1]] as [number, number] : null;
  return {
    w, h, sat: typeof j.sat === 'string' ? j.sat.slice(0, 40) : '',
    coast: pathOf(L.coast), lake: pathOf(L.lake), border: pathOf(L.border), grid: pathOf(L.grid),
    labels, lines: L, centre: c, points: budget.n,
  };
}
/** A live {"type":"lrpt_geo","data":…} message → the outline, or the reason there is none (`why`). */
export function parseLiveOutline(data: unknown): { outline: Outline | null; why: string } {
  if (data && typeof data === 'object' && typeof (data as any).why === 'string') return { outline: null, why: String((data as any).why).slice(0, 200) };
  return { outline: parseOutline(data), why: '' };
}
/** A gallery item's outline: the name of its outline item ("geo"), or why it has none ("gw"); neither = a picture saved
 *  before this feature (plain, nothing said). */
export function outlineOfItem(it: { geo?: unknown; gw?: unknown; f?: string }): { geo: string; why: string } {
  const geo = typeof it.geo === 'string' && /^[A-Za-z0-9._\-/ ]{1,160}$/.test(it.geo) ? it.geo : '';
  const why = typeof it.gw === 'string' ? it.gw.slice(0, 200) : '';
  return { geo, why };
}
/** ★ The outline items are data for the pictures, not things to list: every list leaves them out. */
export const OUTLINE_KIND = 'lrptgeo';
export const isOutlineItem = (it: { k?: string }) => it.k === OUTLINE_KIND;
/** The DOWNLOAD address: with the outline ON and the picture having one, the server burns it into a copy. */
export function outlineDownloadUrl(fileUrl: string, on: boolean, hasOutline: boolean): string {
  const sep = fileUrl.includes('?') ? '&' : '?';
  return `${fileUrl}${sep}download=1${on && hasOutline ? '&outline=1' : ''}`;
}
/** Label text with the degree sign as the server wrote it ("55°N"). Font size for a picture drawn `scale` px per
 *  picture pixel: ~11 screen px. */
export function outlineLabelSize(scale: number): number { return Math.max(6, 11 / Math.max(0.05, scale)); }
