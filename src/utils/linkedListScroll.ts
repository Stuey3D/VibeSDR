/**
 * ★★★ ONE LIST ACROSS THE CREASE (iPhone Duo half-open BIG, Stuart 2026-11-26): when a decoder's ONE list spans both
 * panels it is one continuous list — scrolling in either panel scrolls both, as if the crease were not there; the lower
 * panel shows the rows that follow the upper panel's last row. (Two panels holding SEPARATE data — a live view above, a
 * station list below — scroll on their own; see decoderShellLayout LIST_DECODER_SPLIT.)
 *
 * The model (pure, so scripts/test_decoder_shell_layout.ts proves it):
 *   · The list is one column of rows (`RowBox`: top and height, in the column's own coordinates, in drawing order).
 *   · Both panels show that column at the SAME scroll offset `y`. The upper panel's window is [y, y + seam) and the
 *     lower's continues straight on, [y + seam, y + seam + lowerH) — the lower's content is pulled up by `seam`.
 *   · `seam` is the upper window's height: the bottom of the last row that fits WHOLLY in the upper panel from the
 *     list's top (linkedSeam) — never more than the panel, so the panel keeps a blank sliver under it rather than a
 *     row cut in half at the fold. It is constant while the rows are, so the two panels move in lockstep, pixel for
 *     pixel, while a finger drags — nothing jumps.
 *   · At rest the offset is SNAPPED (linkedSnapOffsets: the ScrollViews' snapToOffsets) to offsets where the fold falls
 *     on a row boundary — so a row straddling the fold is never left cut in half; it ends up wholly above or below.
 *   · A pad / D-pad / keyboard row is brought into view by linkedReveal: unmoved while it is wholly in either panel
 *     (so focus walks from the upper panel's last row to the lower panel's first without anything scrolling), else
 *     the nearest snap offset that shows it whole.
 */

export interface RowBox { top: number; h: number }

/** Every boundary between rows (each row's top, and the last row's bottom), ascending, unique. */
function boundaries(rows: readonly RowBox[]): number[] {
  const b = new Set<number>();
  for (const r of rows) { b.add(r.top); b.add(r.top + r.h); }
  return [...b].sort((p, q) => p - q);
}

/** The column's height (the last row's bottom), 0 for none. */
export function linkedContentH(rows: readonly RowBox[]): number {
  let h = 0;
  for (const r of rows) h = Math.max(h, r.top + r.h);
  return h;
}

/**
 * The upper window's height: the largest row boundary that fits in `upperH`. If the first row alone is taller than the
 * panel there is no clean place for the fold — then the whole panel (that row is cut, as any list cuts a row too tall
 * for it). No rows yet: the whole panel.
 */
export function linkedSeam(rows: readonly RowBox[], upperH: number): number {
  let seam = 0;
  for (const b of boundaries(rows)) if (b <= upperH + 0.5 && b > seam) seam = b;
  return seam > 0 ? seam : upperH;
}

/** How far the list scrolls: until its last row reaches the bottom of the LOWER panel (0 = it all fits). */
export function linkedMaxY(contentH: number, seam: number, lowerH: number): number {
  return Math.max(0, Math.ceil(contentH - seam - lowerH));
}

/**
 * Where the list may come to rest: every offset that puts a row boundary exactly on the fold (boundary − seam), within
 * [0, maxY], plus 0 and maxY themselves (the top and the end are always reachable — at 0 the fold is on a boundary by
 * the seam's own definition). Ascending, unique.
 */
export function linkedSnapOffsets(rows: readonly RowBox[], seam: number, maxY: number): number[] {
  const out = new Set<number>([0, maxY]);
  for (const b of boundaries(rows)) {
    const y = b - seam;
    if (y > 0 && y < maxY) out.add(y);
  }
  return [...out].sort((p, q) => p - q);
}

/** The offset the list settles on from `y` (the nearest snap offset). */
export function linkedSnap(y: number, offsets: readonly number[]): number {
  let best = offsets[0] ?? 0;
  for (const o of offsets) if (Math.abs(o - y) < Math.abs(best - y)) best = o;
  return best;
}

export type LinkedWhere = 'upper' | 'lower' | 'fold' | 'above' | 'below';

/** Where row `r` is drawn at offset `y`: wholly in one panel, across the fold (cut — only ever mid-drag), or out of view. */
export function linkedRowWhere(r: RowBox, y: number, seam: number, lowerH: number): LinkedWhere {
  const top = r.top - y, bot = r.top + r.h - y;   // in window terms: upper [0, seam), lower [seam, seam + lowerH)
  if (top >= -0.5 && bot <= seam + 0.5) return 'upper';
  if (top >= seam - 0.5 && bot <= seam + lowerH + 0.5) return 'lower';
  if (bot <= 0.5) return 'above';
  if (top >= seam + lowerH - 0.5) return 'below';
  if (top < seam && bot > seam) return 'fold';
  return top < 0 ? 'above' : 'below';   // partly off the top of the upper / the bottom of the lower
}

/**
 * The offset that shows row `i` whole (pad / D-pad / keyboard focus). Unmoved when it already is, in either panel.
 * Otherwise the smallest move to a snap offset: upward, the largest offset that has its top in view; downward, the
 * smallest that has its bottom in view. A row taller than a panel is shown from its top.
 */
export function linkedReveal(rows: readonly RowBox[], i: number, y: number, seam: number, lowerH: number,
                             offsets: readonly number[]): number {
  const r = rows[i];
  if (!r || !offsets.length) return y;
  const w = linkedRowWhere(r, y, seam, lowerH);
  if (w === 'upper' || w === 'lower') return y;
  const whole = (o: number) => { const x = linkedRowWhere(r, o, seam, lowerH); return x === 'upper' || x === 'lower'; };
  if (r.top < y || (w === 'fold' && r.top - y < seam / 2)) {
    // Going up (or a cut row nearer the top): the nearest offset at or below y that shows it whole, else its top at the top.
    for (let k = offsets.length - 1; k >= 0; k--) if (offsets[k] <= y && whole(offsets[k])) return offsets[k];
    for (let k = offsets.length - 1; k >= 0; k--) if (offsets[k] <= r.top) return offsets[k];
    return offsets[0];
  }
  for (const o of offsets) if (o >= y && whole(o)) return o;
  for (const o of offsets) if (o >= r.top) return o;
  return offsets[offsets.length - 1];
}
