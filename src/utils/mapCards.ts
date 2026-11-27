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
/* ★★★ TWEAK 3 (Stuart 11-27, live): the HEALTH CARDS' shape — four chips FLANKING the map, each popping out a card and
 *  shrinking back: TUNE and LAYERS down the LEFT edge, MAP CONTROLS on the RIGHT, LIST at the BOTTOM (its card pops up).
 *  KEY and SOURCES live inside MAP CONTROLS now. */
export const CARD_IDS = ['tune', 'layers', 'servers', 'map', 'list', 'help'] as const;
export type CardId = typeof CARD_IDS[number];
export const CARD_LABEL: Record<CardId, string> = { tune: 'TUNE', layers: 'LAYERS', servers: 'SERVERS', map: 'MAP', list: 'LIST', help: 'HELP' };
/** Where each chip sits and its card opens: L = the left edge, R = the right edge, B = the bottom (the card pops up). */
export const CARD_SIDE: Record<CardId, 'L' | 'R' | 'B' | 'H'> = { tune: 'L', layers: 'L', servers: 'L', map: 'R', list: 'B', help: 'H' };
/** A window narrower than this is a phone's: ONE card at a time; the LAYERS chip stands as a slim column. */
export const CARD_NARROW_W = 600;
/** A card's width on a wide window (px / pt); narrower under CARD_ROOMY_W so a left and a right card leave the map. */
export const CARD_W = 340;
export const CARD_W_SNUG = 290;
export const CARD_ROOMY_W = 1100;
/** Tall enough for the two left cards (TUNE, LAYERS) stacked. */
export const CARD_TALL_H = 700;

/** ★★★ Stuart 11-27: "on a large screen all could go down one side, shorter screens flank either side" — a window this
 *  tall (and wider than a phone's) puts MAP CONTROLS on the left too, under LAYERS; shorter ones keep it top-right. */
/*  Calibrated on his phones (11-27): an iPhone SE portrait (≈375×550 in Safari, a map ≈440 tall) FLANKS; a 17 Pro Max
 *  portrait (≈440×830, a map ≈720 tall) puts everything on the LEFT; a phone in landscape (≈956×400) FLANKS. So: by the
 *  MAP's own height against the whole stack of chips (their real heights) plus room for a card to open — never a device. */
export const ONE_SIDE_MIN_H = 600;
export const ONE_SIDE_ROOM = 160;
/** Every chip down the left fits: the map is at least ONE_SIDE_MIN_H tall and holds the stack (SERVERS, TUNE, LAYERS, MAP
 *  CONTROLS, LIST and their gaps — `stackH`, measured) with ONE_SIDE_ROOM to spare. */
export function oneSide(mapH: number, stackH: number): boolean { return mapH >= Math.max(ONE_SIDE_MIN_H, stackH + ONE_SIDE_ROOM); }
/** The side a chip and its card stand on: MAP CONTROLS joins the left when `one`. */
export function cardSide(id: CardId, one = false): 'L' | 'R' | 'B' | 'H' { return id === 'map' && one ? 'L' : CARD_SIDE[id]; }
/** How many cards a window of w × h takes at once: one on a phone; else one per side, two on the left when tall. */
export function cardLimit(w: number, h: number): number {
  if (w < CARD_NARROW_W) return 1;
  return h >= CARD_TALL_H ? 6 : 4;
}
/** The open cards by side, the window's limit applied (the newest kept); a short window keeps one on the left. */
export function cardPlace(open: CardId[], w: number, h: number, one = false): Record<'L' | 'R' | 'B' | 'H', CardId[]> {
  // ★★★ tweak 5 (Stuart 11-27): LIST stays open UNDER the others — the window's limit counts the transient cards only
  const shown = [...open.filter((x) => STICKY_CARDS.has(x)), ...open.filter((x) => !STICKY_CARDS.has(x)).slice(-cardLimit(w, h))];
  const out: Record<'L' | 'R' | 'B' | 'H', CardId[]> = { L: [], R: [], B: [], H: [] };
  for (const id of shown) out[cardSide(id, one)].push(id);
  if (h < CARD_TALL_H && out.L.length > 1) out.L = out.L.slice(-1);
  if (h < CARD_TALL_H && out.R.length > 1) out.R = out.R.slice(-1);
  return out;
}
/** A card opened (or shut): the new open list, the oldest giving way past the window's limit. */
export function cardToggle(open: CardId[], id: CardId, on: boolean, w: number, h: number, one = false): CardId[] {
  const out = open.filter((x) => x !== id);
  if (on) out.push(id);
  const lim = cardLimit(w, h);
  let r = out.slice(Math.max(0, out.length - lim));
  // ★ a short window holds one card on the left: the other left one gives way
  if (h < CARD_TALL_H && w >= CARD_NARROW_W && on && cardSide(id, one) !== 'B' && cardSide(id, one) !== 'H') r = r.filter((x) => x === id || cardSide(x, one) !== cardSide(id, one));
  return r;
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

/** ★★★ THE MAP HELP (Stuart 11-27: "a little floating ? Help button to explain the cards") — one place for the words, the
 *  web page's ? card (tracks.ts) and the app's port alike. UK English, plain, one line each. */
export const MAP_HELP: Array<{ id: Exclude<CardId, 'help'>; name: string; words: string }> = [
  { id: 'servers', name: 'SERVERS', words: 'Leave this server, or switch to its other radios.' },
  { id: 'tune', name: 'TUNE', words: 'Change mode and gain. Faded when you must ask in chat first.' },
  { id: 'layers', name: 'LAYERS', words: 'What is on the map. Tap a row to show or hide it.' },
  { id: 'map', name: 'MAP', words: 'Time filter, fit, rings, range, labels, sources and the key.' },
  { id: 'list', name: 'LIST', words: 'Every aircraft, ship and message, nearest or latest.' },
];
export const MAP_HELP_NOTES: string[] = [
  'Tap a chip to open its card; tap the card\u2019s heading to close it. Escape or BACK closes it too.',
  'On a phone-sized window one card opens at a time.',
];

/** ★★★ Stuart 11-27 (tweak 5): "list is the only one that stays open the others collapse back". LIST is STICKY (open until
 *  closed, remembered per viewer; on a phone another card hides it only for as long as that card is open). The rest are
 *  TRANSIENT: one at a time, back to their tab after their action, a tap or drag on the map, another transient card,
 *  CARD_IDLE_MS without a touch (not while the pointer rests on it), Escape / BACK — and never remembered. */
export const STICKY_CARDS: ReadonlySet<CardId> = new Set<CardId>(['list']);
export const CARD_IDLE_MS = 9000;
/** A card opened (or shut) under the sticky / transient rule: the new open list (the window's limit is the layout's). */
export function cardOpenRule(open: CardId[], id: CardId, on: boolean): CardId[] {
  let out = open.filter((x) => x !== id);
  if (on) { if (!STICKY_CARDS.has(id)) out = out.filter((x) => STICKY_CARDS.has(x)); out.push(id); }
  return out;
}
