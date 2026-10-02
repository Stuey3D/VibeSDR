/**
 * VfdParts — the pieces a VFD / LED display is made of (faceplates brief §7, §8.1; Deck.mockup `seg7`,
 * the `fx.ghost` grids).
 *
 *   GhostGrid     the dot-matrix ghost: every dot of the glass, unlit, at text colour α .10.
 *   SegDigits     DRAWN 7-segment digits — polygons, skewX −7°, never a font — with every unlit
 *                 segment ghosted at text colour α .07 and a decimal point per cell.
 *   VfdFilaments  the glass's filament WIRES, frontmost (BRIEF-lighting-and-vfd-glass §1).
 *
 * ★★ Both are split STATIC / LIT like the tubes: the ghost layer (every dot, every segment of every
 *   cell) is one memoised canvas redrawn only when its size or cell count changes; a tune step only
 *   blits the lit digits, which are glow SPRITES rasterised once per colour × size (glowSprite.ts).
 */

import React, { useCallback, useMemo, useState } from 'react';
import { PixelRatio, View, type LayoutChangeEvent, type StyleProp, type ViewStyle } from 'react-native';
import { Canvas, Group, Image as SkImageNode, Path, Rect, Skia, type SkImage, type SkPath } from '@shopify/react-native-skia';
import { devicePixel, filamentYs, FILAMENT_DARK, FILAMENT_LIGHT } from '../constants/vfdGlass';
import { rgba } from '../constants/faceplate';
import { glowPaint, makeSprite } from './glowSprite';
import { segFit, SEG_CELL_W as CELL_W, SEG_CELL_H as CELL_H } from '../constants/spriteSizing';

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
    if (!(sh > 0)) return null;   // ★ NaN-proof: `sh <= 0` is false for NaN
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

/** One cell's box: its left edge, its top, and its height (a small cell is shorter and sits lower). */
type SegBox = { ox: number; oy: number; sh: number };

const SegGhost = React.memo(function SegGhost({ w, h, boxes, boxKey, color }: {
  w: number; h: number; boxes: SegBox[]; boxKey: string; color: string;
}) {
  const path = useMemo(() => {
    const p = Skia.Path.Make();
    for (const b of boxes) {
      for (const s of [...Object.keys(SEG_POLYS), 'dp']) p.addPath(segPath(s, b.ox, b.oy, b.sh));
    }
    return p;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [boxKey]);
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
type SegDigitsProps = { text: string; rgb: string; core: string; glow: string; designH: number; style?: StyleProp<ViewStyle>;
  /** ★ 'left' anchors the digits so a label of changing width beside them cannot push them about;
   *  'center' centres them in the window. Default 'right' (a counter's alignment). */
  align?: 'left' | 'right' | 'center';
  /** ★★ SMALL CELLS at each end, drawn at SMALL_SEG of the height on the same baseline — the frequency
   *  readout's Hz digits and the matching dark cells that keep it centred (vfdFreqLayout). Real radios
   *  draw the sub-kHz digits small; it also keeps a 13-cell readout from crowding the airband label
   *  and shifting over to make room (Stuart, 2026-10-02). */
  smallLead?: number; smallTail?: number };

/** A small cell's height as a share of a full one. */
const SMALL_SEG = 0.75;

/** ★ Shallow, by value: the caller builds its `style` object afresh on every render. */
function sameStyle(a?: StyleProp<ViewStyle>, b?: StyleProp<ViewStyle>): boolean {
  if (a === b) return true;
  if (!a || !b || Array.isArray(a) || Array.isArray(b) || typeof a !== 'object' || typeof b !== 'object') return false;
  const ka = Object.keys(a), kb = Object.keys(b);
  return ka.length === kb.length && ka.every(k => (a as Record<string, unknown>)[k] === (b as Record<string, unknown>)[k]);
}

/**
 * ★★ MEMOISED (B8 power audit, emulator: the VCR window redrew ~58 times a second with nothing
 *   changing). Every render of a Skia <Canvas> re-records and redraws it, and this one sits inside a
 *   deck that re-renders for unrelated reasons (meters, status); the digits only change on a retune.
 */
export const SegDigits = React.memo(function SegDigits({ text, rgb, core, glow, designH, style, align,
                                                         smallLead = 0, smallTail = 0 }: SegDigitsProps) {
  const [{ w, h }, onLayout] = useBoxSize();
  const cells = useMemo(() => segDigitCells(text), [text]);
  const n = cells.length;
  const gap = 1;
  const lead = Math.min(smallLead, n), tail = Math.min(smallTail, Math.max(0, n - lead));
  const isSmall = (i: number) => i < lead || i >= n - tail;
  // ★ Fit by WIDTH IN FULL CELLS: a small cell counts as SMALL_SEG of one.
  const { sh, cw } = segFit(w, h, n - (lead + tail) * (1 - SMALL_SEG), gap, designH);
  const shS = Math.round(sh * SMALL_SEG * 2) / 2, cwS = cw * (shS / (sh || 1));
  const span = cells.reduce((a, _c, i) => a + (isSmall(i) ? cwS : cw), 0) + Math.max(0, n - 1) * gap;
  const x0 = align === 'left' ? 0 : align === 'center' ? Math.max(0, (w - span) / 2) : Math.max(0, w - span);
  const y0 = Math.max(0, (h - sh) / 2);
  // Each cell's box; a small cell shares the full cells' BASELINE (its bottom edge), like a real readout.
  const boxes = useMemo(() => {
    const out: SegBox[] = [];
    let x = x0;
    for (let i = 0; i < n; i++) {
      const small = isSmall(i);
      out.push({ ox: x, oy: small ? y0 + (sh - shS) : y0, sh: small ? shS : sh });
      x += (small ? cwS : cw) + gap;
    }
    return out;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [n, lead, tail, x0, y0, sh, shS, cw, cwS]);
  const boxKey = `${n}|${lead}|${tail}|${x0}|${y0}|${sh}|${shS}`;
  const sprites = useSegSprites(sh, core, glow);
  const spritesS = useSegSprites(lead + tail > 0 ? shS : 0, core, glow);
  return (
    <View style={style} onLayout={onLayout} pointerEvents="none">
      {w > 0 && sh > 0 && (<>
        <SegGhost w={w} h={h} boxes={boxes} boxKey={boxKey} color={rgba(rgb, 0.07)} />
        {sprites && (
          <Canvas style={{ position: 'absolute', left: 0, top: 0, width: w, height: h }} pointerEvents="none">
            {cells.map((c, i) => {
              const b = boxes[i];
              const set = isSmall(i) && spritesS ? spritesS : sprites;
              if (!b) return null;
              const g = SEG_MAP[c.ch] ? set[c.ch] : null;
              const dp = c.dp ? set.dp : null;
              return (
                <Group key={i}>
                  {g && <SkImageNode image={g.img} x={b.ox - g.m} y={b.oy - g.m} width={g.w} height={g.h} />}
                  {dp && <SkImageNode image={dp.img} x={b.ox - dp.m} y={b.oy - dp.m} width={dp.w} height={dp.h} />}
                </Group>
              );
            })}
          </Canvas>
        )}
      </>)}
    </View>
  );
}, (a, b) => a.text === b.text && a.rgb === b.rgb && a.core === b.core && a.glow === b.glow
             && a.designH === b.designH && a.align === b.align && sameStyle(a.style, b.style)
             && (a.smallLead ?? 0) === (b.smallLead ?? 0) && (a.smallTail ?? 0) === (b.smallTail ?? 0));

// ── Filament wires (BRIEF-lighting-and-vfd-glass §1) ──────────────────────────

const FilamentCanvas = React.memo(function FilamentCanvas({ w, h, radius }: { w: number; h: number; radius: number }) {
  const pr = PixelRatio.get();
  const px = devicePixel(pr);
  const ys = useMemo(() => filamentYs(h, pr), [h, pr]);
  const clip = useMemo(() => Skia.RRectXY(Skia.XYWHRect(0, 0, w, h), radius, radius), [w, h, radius]);
  return (
    <Canvas style={{ position: 'absolute', left: 0, top: 0, width: w, height: h }} pointerEvents="none">
      <Group clip={clip}>
        {ys.map((y, i) => (
          <Group key={i}>
            {/* the hairline highlight directly ABOVE the wire — what reads over unlit glass */}
            <Rect x={0} y={y - px} width={w} height={px} color={FILAMENT_LIGHT} />
            {/* the wire itself, one device pixel — it shadows a lit segment */}
            <Rect x={0} y={y} width={w} height={px} color={FILAMENT_DARK} />
          </Group>
        ))}
      </Group>
    </Canvas>
  );
});

/**
 * ★★ THE FILAMENT WIRES over a VFD window, filling its parent (absolute) — render it as the window's LAST
 * child so it is the frontmost thing in the glass: above the ghost layer, the lit segments and sprites, and
 * the RDS pictogram. ★★ TRAP (layer order): never inside the ghost canvas (GhostGrid / SegDigits' static
 * layer) — those sit UNDER the lit sprites, so the glow would paint over the wires, the reverse of a tube.
 * ★ Zero per-frame cost: one memoised canvas, redrawn only when the window's size changes. Keyed by size,
 *   like ChassisPlate's canvases (a Mac resize once left a canvas drawing into its first surface). Static —
 *   MOTION EFFECTS does not touch it. `radius` = the window's corner radius, which clips the wires.
 */
export function VfdFilaments({ radius = 0 }: { radius?: number }) {
  const [{ w, h }, onLayout] = useBoxSize();
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }} onLayout={onLayout} pointerEvents="none">
      {w > 0 && h > 0 && <FilamentCanvas key={`${w}x${h}`} w={w} h={h} radius={radius} />}
    </View>
  );
}
