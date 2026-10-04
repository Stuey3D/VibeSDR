/**
 * wefaxAlign — SHIFT and SLANT for a WEFAX chart, applied as each line is DRAWN (app + web; no server cost).
 *
 * ★★★ WHY (Stuart, 2026-10-04): Northwood (GYA) charts come out crooked on every SDR, UberSDR included, while
 *     DWD's are straight — and Northwood's black margin sits "slightly to the right of the left edge causing wrap
 *     around". Measured on Northwood 4610 through Stuart's HF+: the margin starts ~40 px in and walks LEFT about
 *     0.06 px per line — a ~33 ppm line-rate offset at the transmitter plus a phasing offset. Both are a steady
 *     sideways shift that grows with the line number, so one formula undoes both:
 *         line y is moved LEFT by  shift + slant × y   (circularly — what falls off the left reappears on the right)
 * ★ An automatic margin tracker was prototyped first and dropped: on charts with no margin it locked onto map
 *   features as strong as a real margin. A remembered per-frequency setting is predictable.
 */
export interface WefaxAlign { shift: number; slant: number }
export const WEFAX_ALIGN_ZERO: WefaxAlign = { shift: 0, slant: 0 };

/** Northwood (GYA) carrier frequencies, kHz. */
const NORTHWOOD_KHZ = [2618.5, 4610, 8040, 11086.5];
/** ★ Measured 2026-10-04 (see the header). SLANT only: the margin's position differs chart to chart (~40 px in on
 *  some, dead centre on others), so the SHIFT is found per chart by findMargin, never preset. */
export const NORTHWOOD_ALIGN: WefaxAlign = { shift: 0, slant: -0.06 };

/** The station preset for a dial frequency, or zero. The dial in USB sits ~1.9 kHz below the carrier, so match
 *  within 5 kHz of a carrier. */
export function wefaxPreset(dialHz: number): WefaxAlign {
  const khz = dialHz / 1000;
  return NORTHWOOD_KHZ.some((c) => Math.abs(khz - c) <= 5) ? { ...NORTHWOOD_ALIGN } : { ...WEFAX_ALIGN_ZERO };
}
/** Where a listener's own setting is remembered: per frequency, to the nearest kHz. */
export function wefaxAlignKey(dialHz: number): string { return `wefaxAlign:${Math.round(dialHz / 1000)}`; }
export function parseAlign(v: unknown): WefaxAlign | null {
  if (!v || typeof v !== 'object') return null;
  const o = v as Record<string, unknown>;
  const s = Number(o.shift), k = Number(o.slant);
  if (!Number.isFinite(s) || !Number.isFinite(k)) return null;
  return { shift: Math.max(-2000, Math.min(2000, s)), slant: Math.max(-2, Math.min(2, k)) };
}
/** The whole-pixel left shift for line `y` of a `width`-pixel chart, 0 ≤ result < width. */
export function wefaxOffset(a: WefaxAlign, y: number, width: number): number {
  if (!width) return 0;
  const o = Math.round(a.shift + a.slant * y);
  return ((o % width) + width) % width;
}
/** Copy `src` (one line) into `dst` moved left by `off` pixels, wrapping. */
export function rotateLine(src: ArrayLike<number>, dst: Uint8Array | number[], width: number, off: number): void {
  for (let x = 0; x < width; x++) dst[x] = src[(x + off) % width] ?? 0;
}
/** Steps for the adjust keys. */
export const SHIFT_STEP = 5;
export const SLANT_STEP = 0.005;

/** ★ Lines a chart must have before its margin is looked for, and how many are summed. */
export const MARGIN_AFTER_LINES = 150;

/**
 * ★★★ FIND THE CHART'S MARGIN — once per chart, every station (Stuart, 2026-10-04: a noob sits on Northwood, presses
 *     WEFAX and gets a clean chart; "same with DDK"). The black margin line is the one feature that runs EXACTLY down
 *     the page once the station's slant is taken out, so summing each column's darkness along the slant over
 *     MARGIN_AFTER_LINES lines makes it one sharp peak wherever it sits — Northwood puts it ~40 px in on some charts
 *     and dead centre (~370) on others; map lines, curved or diagonal, smear out. Measured on two real Northwood
 *     charts: margin strength 53–97 against ~20 for the 30th-best column, at the same column in every window.
 * Returns the raw column of the margin AT LINE 0 (so `shift = column − 2` puts it at the left edge), or null when no
 * line is clearly a margin — then nothing is moved (a chart without one is never shifted).
 * `rows[y]` = line y as received (greyscale), any missing rows skipped.
 */
export function findMargin(rows: ReadonlyArray<ArrayLike<number> | undefined>, width: number, slant: number,
                           fromRow = 0, count = MARGIN_AFTER_LINES): number | null {
  const W = width;
  if (!W) return null;
  const acc = new Float64Array(W);
  let n = 0;
  for (let y = fromRow; y < fromRow + count && y < rows.length; y++) {
    const r = rows[y];
    if (!r) continue;
    const off = ((Math.round(slant * y) % W) + W) % W;
    for (let x = 0; x < W; x++) acc[x] += 255 - (r[(x + off) % W] ?? 255);
    n++;
  }
  if (n < count / 2) return null;
  const sm = new Float64Array(W);
  for (let x = 0; x < W; x++) {
    let a = 0;
    for (let k = -2; k <= 2; k++) a += acc[((x + k) % W + W) % W];
    sm[x] = a / 5 / n;
  }
  // Narrow: darker than BOTH sides 25–40 px away (the edge of a dark band is lighter on one side only).
  const narrow = new Float64Array(W);
  for (let x = 0; x < W; x++) {
    let l = 0, r = 0;
    for (let k = 25; k <= 40; k++) { r += sm[(x + k) % W]; l += sm[((x - k) % W + W) % W]; }
    narrow[x] = sm[x] - Math.max(l, r) / 16;
  }
  let best = 0;
  for (let x = 1; x < W; x++) if (narrow[x] > narrow[best]) best = x;
  const sorted = Array.from(narrow).sort((a, b) => b - a);
  const ref = sorted[Math.min(29, sorted.length - 1)];
  return narrow[best] >= 40 && narrow[best] >= 2 * Math.max(ref, 1) ? best : null;
}

/** Per-station slant (px per line); the shift comes from findMargin per chart. Northwood −0.06, everything else 0. */
export function stationSlant(dialHz: number): number { return wefaxPreset(dialHz).slant; }
