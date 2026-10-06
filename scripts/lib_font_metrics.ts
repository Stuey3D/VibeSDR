/**
 * lib_font_metrics.ts — advance widths read straight from the bundled TTFs (assets/fonts), for the faceplate fit
 * tests (★ 2026-10-06). The status row and the notices now take the DISPLAY's font (Nixie One, Atkinson, Doto), and a
 * proportional face cannot be modelled as "n cells × a pitch" the way VCR / DOT are — so the tests measure the real
 * glyphs: cmap (format 4, the BMP) → glyph id → hmtx advance, over the head table's unitsPerEm.
 *
 * ★ Advances only — no kerning. Kerning in these faces is almost all NEGATIVE, so a width measured here errs wide,
 *   which is the safe side for a fit test.
 * ★ A character the font has no glyph for is reported by `missing` and measured at the notdef advance — on a device
 *   the OS falls back to the system font for it.
 */
import { readFileSync } from 'node:fs';

export interface FontMetrics {
  name: string;
  unitsPerEm: number;
  /** Advance of `ch` in em, or null when the font has no glyph for it. */
  advance(ch: string): number | null;
  /** Width of `text` in pt at `size`, + letterSpacing after every character (as React Native applies it). */
  width(text: string, size: number, letterSpacing?: number): number;
  /** The characters of `text` the font cannot draw (spaces aside). */
  missing(text: string): string[];
}

export function loadFont(file: string): FontMetrics {
  const b = readFileSync(new URL(`../assets/fonts/${file}`, import.meta.url));
  const u16 = (o: number) => b.readUInt16BE(o);
  const s16 = (o: number) => b.readInt16BE(o);
  const u32 = (o: number) => b.readUInt32BE(o);
  const tables: Record<string, number> = {};
  for (let i = 0; i < u16(4); i++) {
    const r = 12 + 16 * i;
    tables[b.toString('latin1', r, r + 4)] = u32(r + 8);
  }
  const unitsPerEm = u16(tables.head + 18);
  const numHMetrics = u16(tables.hhea + 34);
  const adv = (gid: number) => u16(tables.hmtx + 4 * Math.min(gid, numHMetrics - 1));

  // cmap: the (3,1) — else (0,x) — format-4 subtable.
  const cmap = tables.cmap;
  let sub = -1;
  for (let i = 0; i < u16(cmap + 2); i++) {
    const r = cmap + 4 + 8 * i;
    const pid = u16(r), eid = u16(r + 2), off = u32(r + 4);
    if (u16(cmap + off) !== 4) continue;
    if (pid === 3 && eid === 1) { sub = cmap + off; break; }
    if (pid === 0 && sub < 0) sub = cmap + off;
  }
  if (sub < 0) throw new Error(`${file}: no format-4 cmap`);
  const segX2 = u16(sub + 6);
  const ends = sub + 14, starts = ends + segX2 + 2, deltas = starts + segX2, ranges = deltas + segX2;
  const gidOf = (cp: number): number => {
    if (cp > 0xffff) return 0;
    for (let i = 0; i < segX2 / 2; i++) {
      if (cp > u16(ends + 2 * i)) continue;
      const start = u16(starts + 2 * i);
      if (cp < start) return 0;
      const delta = s16(deltas + 2 * i), ro = u16(ranges + 2 * i);
      if (ro === 0) return (cp + delta) & 0xffff;
      const g = u16(ranges + 2 * i + ro + 2 * (cp - start));
      return g === 0 ? 0 : (g + delta) & 0xffff;
    }
    return 0;
  };
  const advance = (ch: string) => {
    const g = gidOf(ch.codePointAt(0) ?? 0);
    return g === 0 ? null : adv(g) / unitsPerEm;
  };
  return {
    name: file, unitsPerEm, advance,
    width(text, size, ls = 0) {
      let w = 0;
      for (const ch of text) w += (advance(ch) ?? adv(0) / unitsPerEm) * size + ls;
      return w;
    },
    missing(text) { return [...new Set([...text].filter(c => c !== ' ' && advance(c) === null))]; },
  };
}

const cache = new Map<string, FontMetrics>();
const once = (file: string) => () => {
  let f = cache.get(file);
  if (!f) { f = loadFont(file); cache.set(file, f); }
  return f;
};
export const NIXIE = once('NixieOne-Regular.ttf');
export const HYPER = once('AtkinsonHyperlegible-Regular.ttf');
export const DOTO  = once('Doto-Black.ttf');
