/**
 * spriteSizing — the sizes that reach Skia's offscreen surfaces, as pure functions (no React Native),
 * so scripts/test_sprite_sizing.ts can prove no input ever produces a size Metal would abort on.
 */

/** The 7-segment design cell (VfdParts' polygons are drawn in this box). */
export const SEG_CELL_W = 24, SEG_CELL_H = 38;

/** The largest sprite side, in pixels. Every sprite here is a glyph, an LED or a meter part — a few
 *  hundred px at most; Metal's own limit is 16384 and anything near it is a sizing bug, not a sprite. */
export const SPRITE_MAX_PX = 4096;

/**
 * The pixel size of a w × h (pt) sprite at pixel ratio `pr`, or null when there is nothing sane to draw.
 * ★★★ THIS IS THE LAST LINE BEFORE METAL, AND METAL DOES NOT RETURN AN ERROR — IT ABORTS THE APP.
 *   `Math.max(1, NaN)` is NaN, so the old `max(1, ceil(...))` let a NaN size straight through; on iOS
 *   Skia.Surface.MakeOffscreen(NaN, NaN) killed the process (11 B7/B8 VCR crash, TestFlight report
 *   2026-10-01 02:32). Android's GL backend returns no surface for the same call, which is why only
 *   the iPhone crashed. Non-finite, zero, negative or absurd sizes are refused HERE, for every caller.
 */
export function spritePixels(w: number, h: number, pr: number): { pw: number; ph: number } | null {
  if (!Number.isFinite(w) || !Number.isFinite(h) || !Number.isFinite(pr) || w <= 0 || h <= 0 || pr <= 0) return null;
  const pw = Math.max(1, Math.ceil(w * pr)), ph = Math.max(1, Math.ceil(h * pr));
  if (pw > SPRITE_MAX_PX || ph > SPRITE_MAX_PX) return null;
  return { pw, ph };
}

/**
 * The cell height and width that fit `n` cells into a w × h box: the design height capped by the box,
 * then narrowed to fit the width, rounded to half a point.
 * ★★★ ALWAYS FINITE, 0 WHEN NOTHING FITS. The first render is unmeasured (w = h = 0): sh = 0, cw = 0,
 *   and the narrowing used to divide by n·cw = 0 — -Infinity × 0 = NaN. NaN passes `sh <= 0`, so the
 *   glyph sprites were built at NaN × NaN, and on iOS Metal ABORTS the app for a texture that size
 *   (11 B7/B8: choosing VCR crashed the iPhone instantly; Android's GL returned no surface instead).
 */
export function segFit(w: number, h: number, n: number, gap: number, designH: number): { sh: number; cw: number } {
  let sh = Math.max(0, Math.min(designH, h - 4));
  let cw = (SEG_CELL_W * sh) / SEG_CELL_H;
  const need = n * cw + (n - 1) * gap;
  if (n > 0 && cw > 0 && need > w) { const f = (w - (n - 1) * gap) / (n * cw); sh *= f; }
  sh = Math.round(sh * 2) / 2;
  if (!Number.isFinite(sh) || sh <= 0) return { sh: 0, cw: 0 };
  cw = (SEG_CELL_W * sh) / SEG_CELL_H;
  return { sh, cw };
}
