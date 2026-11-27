/**
 * mapCards.ts — ★★★ beta96 THE MAP CONTROL CARDS' RULES (Stuart 11-27, docs/v12/MAP-CONTROL-CARDS.md), pure, for the web
 * page (web/client/src/tracks.ts TrackView) and the app's map (the port: MapOverlay / SDRScreen) alike:
 *   • which cards there are and their words;
 *   • how many may be open at once — from the WINDOW, never the device (memory: layout_follows_window_not_device):
 *     a phone-width window ONE, wider ones columns of CARD_W, two stacked where the window is tall, never more than
 *     ~72 % of the width;
 *   • FIT's outlier rule (Stuart 11-27: "with this much data the fit button doesnt work") — the furthest few per cent
 *     of the chosen layer are left out, so one aircraft at Tallinn cannot zoom the whole UK view out;
 *   • the layer FIT frames by default.
 */
export const CARD_IDS = ['list', 'tune', 'view', 'map', 'key', 'sources'] as const;
export type CardId = typeof CARD_IDS[number];
export const CARD_LABEL: Record<CardId, string> = { list: 'LIST', tune: 'TUNE', view: 'VIEW', map: 'MAP', key: 'KEY', sources: 'SOURCES' };
/** A window narrower than this is a phone's: ONE card at a time, the buttons on a row of their own. */
export const CARD_NARROW_W = 600;
/** A card's width on a wide window (px / pt). */
export const CARD_W = 340;
/** Tall enough for two cards stacked in one column. */
export const CARD_TALL_H = 700;

/** How many cards a window of w × h takes at once. */
export function cardLimit(w: number, h: number): number {
  if (w < CARD_NARROW_W) return 1;
  const cols = Math.max(1, Math.min(3, Math.floor((w * 0.72) / (CARD_W + 8))));
  return Math.min(CARD_IDS.length, cols * (h >= CARD_TALL_H ? 2 : 1));
}
/** The cards to show, in their columns: LIST alone in the first (it is the tall one), the rest in the order opened. */
export function cardColumns(open: CardId[], w: number, h: number): CardId[][] {
  const narrow = w < CARD_NARROW_W, perCol = narrow ? 1 : h >= CARD_TALL_H ? 2 : 1;
  const shown = open.slice(-cardLimit(w, h));
  const ordered = [...shown.filter((x) => x === 'list'), ...shown.filter((x) => x !== 'list')];
  const cols: CardId[][] = [];
  for (const id of ordered) {
    const last = cols[cols.length - 1];
    if (!last || last.length >= perCol || (last[0] === 'list' && !narrow)) cols.push([id]); else last.push(id);
  }
  return cols;
}
/** A card opened (or shut): the new open list, the oldest giving way past the window's limit. */
export function cardToggle(open: CardId[], id: CardId, on: boolean, w: number, h: number): CardId[] {
  const out = open.filter((x) => x !== id);
  if (on) out.push(id);
  const lim = cardLimit(w, h);
  return out.slice(Math.max(0, out.length - lim));
}
/** What opens by default: nothing on a phone (the map is the point), LIST on a wide window. */
export function cardDefaults(w: number): CardId[] { return w < CARD_NARROW_W ? [] : ['list']; }

const D2R = Math.PI / 180;
function km(a: { lat: number; lon: number }, b: { lat: number; lon: number }): number {
  const p1 = a.lat * D2R, p2 = b.lat * D2R, dp = (b.lat - a.lat) * D2R, dl = (b.lon - a.lon) * D2R;
  const h = Math.sin(dp / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2;
  return 2 * 6371.0088 * Math.asin(Math.min(1, Math.sqrt(h)));
}
/** FIT's outlier rule: with 8 or more points the furthest 5 % (at least one) are left out — measured from the receiver
 *  where it is known, else from the median point. Fewer than 8: all of them (nothing is an outlier among a few). */
export const FIT_TRIM_MIN = 8;
export const FIT_TRIM_FRAC = 0.05;
export function trimOutliers<T extends { lat: number; lon: number }>(pts: T[], rx: { lat: number; lon: number } | null): T[] {
  if (pts.length < FIT_TRIM_MIN) return pts;
  const med = (a: number[]) => { const s = a.slice().sort((x, y) => x - y); return s[s.length >> 1]; };
  const c = rx ?? { lat: med(pts.map((p) => p.lat)), lon: med(pts.map((p) => p.lon)) };
  const d = pts.map((p) => ({ p, k: km(c, p) })).sort((a, b) => a.k - b.k);
  const drop = Math.max(1, Math.ceil(pts.length * FIT_TRIM_FRAC));
  return d.slice(0, d.length - drop).map((x) => x.p);
}
/** The layer FIT frames by default: ADS-B where there is ADS-B, else the live mode's own layer, else the fullest. */
export function fitDefault(counts: Record<string, number>, main: string): string {
  if ((counts.adsb ?? 0) > 0) return 'adsb';
  if (main && (counts[main] ?? 0) > 0) return main;
  let best = '', n = 0;
  for (const [k, v] of Object.entries(counts)) if (k !== 'other' && v > n) { best = k; n = v; }
  return best;
}
/** The live message rate's words, top right: ADS-B per second, the rest per minute ('' = nothing live). */
export function rateWords(n: number): string { return n >= 100 ? Math.round(n).toLocaleString('en-GB') : n.toFixed(n < 10 ? 1 : 0); }
