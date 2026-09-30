/**
 * VfdParts — the pieces a VFD / LED display is made of (faceplates brief §7, §8.1; Deck.mockup `seg7`,
 * the `fx.ghost` grids).
 *
 *   GhostGrid     the dot-matrix ghost: every dot of the glass, unlit, at text colour α .10.
 *   SegDigits     DRAWN 7-segment digits — polygons, skewX −7°, never a font — with every unlit
 *                 segment ghosted at text colour α .07 and a decimal point per cell.
 *
 * ★★ Both are split STATIC / LIT like the tubes: the ghost layer (every dot, every segment of every
 *   cell) is one memoised canvas redrawn only when its size or cell count changes; a tune step only
 *   blits the lit digits, which are glow SPRITES rasterised once per colour × size (glowSprite.ts).
 */

import React, { useCallback, useMemo, useState } from 'react';
import { View, type LayoutChangeEvent, type StyleProp, type ViewStyle } from 'react-native';
import { Canvas, Group, Image as SkImageNode, Path, Skia, type SkImage, type SkPath } from '@shopify/react-native-skia';
import { rgba } from '../constants/faceplate';
import { glowPaint, makeSprite } from './glowSprite';

export function useBoxSize() {
  const [sz, setSz] = useState({ w: 0, h: 0 });
  const onLayout = useCallback((e: LayoutChangeEvent) => {
    const w = Math.round(e.nativeEvent.layout.width), h = Math.round(e.nativeEvent.layout.height);
    setSz(p => (p.w === w && p.h === h ? p : { w, h }));
  }, []);
  return [sz, onLayout] as const;
}

// ── Ghost dot grid ────────────────────────────────────────────────────────────

const GhostGridCanvas = React.memo(function GhostGridCanvas({ w, h, color, pitch, dot }: {
  w: number; h: number; color: string; pitch: number; dot: number;
}) {
  const path = useMemo(() => {
    const p = Skia.Path.Make();
    for (let y = pitch / 2; y < h; y += pitch) for (let x = pitch / 2; x < w; x += pitch) p.addCircle(x, y, dot / 2);
    return p;
  }, [w, h, pitch, dot]);
  return (
    <Canvas style={{ position: 'absolute', left: 0, top: 0, width: w, height: h }} pointerEvents="none">
      <Path path={path} color={color} />
    </Canvas>
  );
});

/**
 * The ghost grid, filling its parent (absolute). `rgb` is the text colour's triplet; the dots are at
 * α .10 (§7 "text colour α .10, 0.7–0.8 pt dots on a 3–3.4 pt pitch").
 */
export function GhostGrid({ rgb, pitch = 3.4, dot = 0.8, style }: {
  rgb: string; pitch?: number; dot?: number; style?: StyleProp<ViewStyle>;
}) {
  const [{ w, h }, onLayout] = useBoxSize();
  return (
    <View style={[{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }, style]} onLayout={onLayout} pointerEvents="none">
      {w > 0 && h > 0 && <GhostGridCanvas w={w} h={h} color={rgba(rgb, 0.10)} pitch={pitch} dot={dot} />}
    </View>
  );
}

// ── 7-segment ─────────────────────────────────────────────────────────────────

/** Deck.mockup's polygons, in its 24 × 38 cell, under `translate(4 1) skewX(-7)`. */
const SEG_POLYS: Record<string, number[]> = {
  a: [5.5, 1, 15.5, 1, 17.5, 3, 15.5, 5, 5.5, 5, 3.5, 3],
  b: [18, 4, 20, 6, 20, 15.5, 18, 17.5, 16, 15.5, 16, 6],
  c: [18, 18.5, 20, 20.5, 20, 30, 18, 32, 16, 30, 16, 20.5],
  d: [5.5, 31, 15.5, 31, 17.5, 33, 15.5, 35, 5.5, 35, 3.5, 33],
  e: [3, 18.5, 5, 20.5, 5, 30, 3, 32, 1, 30, 1, 20.5],
  f: [3, 4, 5, 6, 5, 15.5, 3, 17.5, 1, 15.5, 1, 6],
  g: [5.5, 16, 15.5, 16, 17.5, 18, 15.5, 20, 5.5, 20, 3.5, 18],
};
const SEG_MAP: Record<string, string> = {
  '0': 'abcdef', '1': 'bc', '2': 'abged', '3': 'abgcd', '4': 'fgbc',
  '5': 'afgcd', '6': 'afgedc', '7': 'abc', '8': 'abcdefg', '9': 'abcdfg', '-': 'g',
};
const CELL_W = 24, CELL_H = 38;
const SKEW = Math.tan((-7 * Math.PI) / 180);

/** A segment (or the DP) as a path in a cell of height `sh`, its top-left at (ox, oy). */
function segPath(key: string, ox: number, oy: number, sh: number): SkPath {
  const k = sh / CELL_H;
  const T = (x: number, y: number): [number, number] => [ox + (4 + x + SKEW * y) * k, oy + (1 + y) * k];
  const p = Skia.Path.Make();
  if (key === 'dp') {
    const [cx, cy] = T(21, 34);
    p.addCircle(cx, cy, 1.7 * k);
    return p;
  }
  const pts = SEG_POLYS[key];
  const [x0, y0] = T(pts[0], pts[1]);
  p.moveTo(x0, y0);
  for (let i = 2; i < pts.length; i += 2) { const [x, y] = T(pts[i], pts[i + 1]); p.lineTo(x, y); }
  p.close();
  return p;
}

/** Split a frequency string into 7-segment cells: digits (and '-'), a '.' riding on the cell before. */
export function segDigitCells(text: string): Array<{ ch: string; dp: boolean }> {
  const cells: Array<{ ch: string; dp: boolean }> = [];
  for (const ch of text) {
    if (ch === '.') { if (cells.length && !cells[cells.length - 1].dp) cells[cells.length - 1].dp = true; else cells.push({ ch: ' ', dp: true }); continue; }
    if (SEG_MAP[ch] || ch === ' ') cells.push({ ch, dp: false });
    // anything else (thousands commas, letters) is not a 7-segment glyph and takes no cell
  }
  return cells;
}

interface Sprite { img: SkImage; w: number; h: number; m: number }

/** The lit glyphs, glow included (`drop-shadow(0 0 3px glow)`), rasterised once per colour × size. */
function useSegSprites(sh: number, core: string, glow: string): Record<string, Sprite> | null {
  return useMemo(() => {
    if (sh <= 0) return null;
    const k = sh / CELL_H;
    const m = 6;
    const w = CELL_W * k + 2 * m, h = CELL_H * k + 2 * m;
    const out: Record<string, Sprite> = {};
    const keys = [...Object.keys(SEG_MAP), 'dp'];
    for (const key of keys) {
      const segs = key === 'dp' ? ['dp'] : SEG_MAP[key].split('');
      const img = makeSprite(w, h, (c) => {
        for (const s of segs) c.drawPath(segPath(s, m, m, sh), glowPaint(glow, 3));
        for (const s of segs) c.drawPath(segPath(s, m, m, sh), glowPaint(core));
      });
      if (img) out[key] = { img, w, h, m };
    }
    return out;
  }, [sh, core, glow]);
}

const SegGhost = React.memo(function SegGhost({ w, h, n, sh, cw, gap, x0, y0, color }: {
  w: number; h: number; n: number; sh: number; cw: number; gap: number; x0: number; y0: number; color: string;
}) {
  const path = useMemo(() => {
    const p = Skia.Path.Make();
    for (let i = 0; i < n; i++) {
      const ox = x0 + i * (cw + gap);
      for (const s of [...Object.keys(SEG_POLYS), 'dp']) p.addPath(segPath(s, ox, y0, sh));
    }
    return p;
  }, [n, sh, cw, gap, x0, y0]);
  return (
    <Canvas style={{ position: 'absolute', left: 0, top: 0, width: w, height: h }} pointerEvents="none">
      <Path path={path} color={color} />
    </Canvas>
  );
});

/**
 * DRAWN 7-segment digits (§7: "polygons, skewX −7°, unlit segments at text colour α .07 — not a
 * font"). Sized to its box: the design cell height `designH` (mockup sh: bar 30, shared 27) capped by
 * the box, then narrowed to fit its width. Right-aligned, like a real counter, so the unit label
 * beside it never moves a digit.
 */
export function SegDigits({ text, rgb, core, glow, designH, style }: {
  text: string; rgb: string; core: string; glow: string; designH: number; style?: StyleProp<ViewStyle>;
}) {
  const [{ w, h }, onLayout] = useBoxSize();
  const cells = useMemo(() => segDigitCells(text), [text]);
  const n = cells.length;
  const gap = 1;
  let sh = Math.max(0, Math.min(designH, h - 4));
  let cw = (CELL_W * sh) / CELL_H;
  const need = n * cw + (n - 1) * gap;
  if (need > w && n > 0) { const f = (w - (n - 1) * gap) / (n * cw); sh *= f; cw *= f; }
  sh = Math.round(sh * 2) / 2;
  cw = (CELL_W * sh) / CELL_H;
  const x0 = Math.max(0, w - (n * cw + (n - 1) * gap));
  const y0 = Math.max(0, (h - sh) / 2);
  const sprites = useSegSprites(sh, core, glow);
  return (
    <View style={style} onLayout={onLayout} pointerEvents="none">
      {w > 0 && sh > 0 && (<>
        <SegGhost w={w} h={h} n={n} sh={sh} cw={cw} gap={gap} x0={x0} y0={y0} color={rgba(rgb, 0.07)} />
        {sprites && (
          <Canvas style={{ position: 'absolute', left: 0, top: 0, width: w, height: h }} pointerEvents="none">
            {cells.map((c, i) => {
              const ox = x0 + i * (cw + gap);
              const g = SEG_MAP[c.ch] ? sprites[c.ch] : null;
              const dp = c.dp ? sprites.dp : null;
              return (
                <Group key={i}>
                  {g && <SkImageNode image={g.img} x={ox - g.m} y={y0 - g.m} width={g.w} height={g.h} />}
                  {dp && <SkImageNode image={dp.img} x={ox - dp.m} y={y0 - dp.m} width={dp.w} height={dp.h} />}
                </Group>
              );
            })}
          </Canvas>
        )}
      </>)}
    </View>
  );
}
