/**
 * ★★★ THE DECODER KEYS ARE ONE SIZE (Stuart, 2026-10-29: "can we also make the buttons for the decoders one uniform size
 * too please, this panel looks like a mess, left justify them under their category title please"). The pure half of the
 * Mode panel's DECODERS and VIEWS grids — shared by the app (ModeSelector) and the web client (decoderBandsDom.ts), tested
 * by scripts/test_decoder_key_grid.ts.
 *
 *   • ONE SIZE for every key in the panel (TUNES THE RADIO ITSELF, every band group, SCANNER, VIEWS): the widest legend
 *     (its glyph included) decides how many columns the MEASURED panel width holds, and every key is one column wide.
 *     A short row simply ends early — nothing stretches to fill it, nothing is centred.
 *   • THE WINDOW, NEVER THE DEVICE (memory: layout_follows_window_not_device): the only inputs are the panel's own width
 *     and the legends' own widths, both measured where they are drawn.
 *   • THE GLYPH GOES ABOVE THE LEGEND only on the narrowest panels (an iPhone SE in Display Zoom): when beside would
 *     leave fewer than two columns, or when going above wins a column on a panel that would otherwise hold only two.
 *   • A KEY'S OPTIONS POP OUT OF IT (Stuart: "the options look like they have popped out of the decoder button itself
 *     onto the row below"): they open on the row directly under the key's own row, so every other key keeps its place.
 *     `optionsAfter` names that row; `tabCorners` says which of the panel's top corners sit under the key (square, so
 *     the key's tab and the panel read as one shape).
 */

/** One key to be laid out: its legend's measured width (px / pt) and whether a glyph is drawn beside it. */
export interface KeyLegend { w: number; icon: boolean }

/** What a key adds round its legend — the same numbers the drawing uses. */
export interface KeyMetrics {
  /** The gap between keys, both ways. */
  gap: number;
  /** Horizontal padding plus border, each side. */
  padX: number;
  /** A glyph's width, and its gap to the legend when beside it. */
  iconW: number;
  iconGap: number;
  /** Room for the focus ring and rounding (added once). */
  slack?: number;
  /** Never more columns than this (a very wide window would otherwise draw a row of tiny keys). */
  maxCols?: number;
}

export interface KeyGrid {
  /** Keys per row. */
  cols: number;
  /** Every key's width. */
  keyW: number;
  /** The glyph sits ABOVE the legend (the narrowest panels). */
  stack: boolean;
}

/** The width one key needs: the widest legend, its glyph beside it (or above, `stack`), the padding. */
export function keyNeed(keys: readonly KeyLegend[], m: KeyMetrics, stack: boolean): number {
  let need = 0;
  for (const k of keys) {
    const w = !k.icon ? k.w : stack ? Math.max(k.w, m.iconW) : k.w + m.iconW + m.iconGap;
    if (w > need) need = w;
  }
  return Math.ceil(need + 2 * m.padX + (m.slack ?? 0));
}

/** How many keys of width `need` fit across `panelW` with `gap` between them (at least one). */
export function colsFor(panelW: number, need: number, gap: number, maxCols = 6): number {
  if (!(panelW > 0) || !(need > 0)) return 1;
  return Math.max(1, Math.min(maxCols, Math.floor((panelW + gap) / (need + gap))));
}

/**
 * The grid for the keys drawn in a panel `panelW` wide. Every key is one column wide, so the rows line up under each
 * other and with the panel's edges; a key's legend never needs more than that.
 */
export function decoderKeyGrid(keys: readonly KeyLegend[], panelW: number, m: KeyMetrics): KeyGrid {
  const maxCols = m.maxCols ?? 6;
  const beside = colsFor(panelW, keyNeed(keys, m, false), m.gap, maxCols);
  const anyIcon = keys.some((k) => k.icon);
  const above = anyIcon ? colsFor(panelW, keyNeed(keys, m, true), m.gap, maxCols) : beside;
  // ★ Above only where the panel is narrow: beside cannot hold two keys a row, or holds only two and above holds more.
  const stack = anyIcon && above > beside && beside <= 2;
  const cols = stack ? above : beside;
  // ★ Not rounded to whole points: the last column's right edge must meet the panel's (an open key's tab and its options
  //   panel share that edge). Floored to 1/100 so the row never overflows by a rounding error.
  const keyW = panelW > 0 ? Math.floor(((panelW - (cols - 1) * m.gap) / cols) * 100) / 100 : 0;
  return { cols, keyW, stack };
}

/** The items in rows of `cols`, in order (the last row may be short). */
export function gridRows<T>(items: readonly T[], cols: number): T[][] {
  const c = Math.max(1, Math.floor(cols) || 1);
  const out: T[][] = [];
  for (let i = 0; i < items.length; i += c) out.push(items.slice(i, i + c));
  return out;
}

/** The row (0-based) whose keys a key at `index` sits in — its options open directly beneath it. -1 = not drawn. */
export function optionsAfter(index: number, cols: number): number {
  if (!(index >= 0)) return -1;
  return Math.floor(index / Math.max(1, Math.floor(cols) || 1));
}

/** Where, in a flat list of the keys and their options panel, the panel goes: after the last key of `index`'s row. */
export function optionsInsertAt(index: number, cols: number, count: number): number {
  const r = optionsAfter(index, cols);
  if (r < 0) return -1;
  return Math.min(count, (r + 1) * Math.max(1, Math.floor(cols) || 1));
}

/**
 * The panel's TOP corners that sit directly under the open key — drawn square, so the key's tab runs straight down into
 * the panel. A key in the first column owns the left corner; one in the last column of a full-width grid the right.
 */
export function tabCorners(index: number, cols: number): { left: boolean; right: boolean; col: number } {
  const c = Math.max(1, Math.floor(cols) || 1);
  const col = index >= 0 ? index % c : -1;
  return { left: col === 0, right: col === c - 1, col };
}
