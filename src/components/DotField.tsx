/**
 * DotField — a FIXED dot-matrix field, drawn the DOT Display's way (2026-10-06): 5 × 7 cells of Doto's own square dots
 * (constants/dotField DOTO_GLYPHS, read out of the font), EVERY dot of every cell always there as a ghost (text colour
 * α .10, as on a real dot-matrix VFD / LCD), the lit dots in the text colour with its glow on top. The DOT twin of
 * SegField: the mode field, the readout and the frequency unit.
 *
 * ★ Positions are in DOTS, not points: a cell at column 21 is 21 × pitch along, so every field sits on one dot grid
 *   at its pitch — the rings and the colon included (`marks`).
 * ★★ ONE CANVAS PER FIELD (the Mac GPU audit: every <Canvas> is a Metal layer). The marks (rings, colon) are drawn
 *   into this canvas, not components with canvases of their own.
 * ★★ STATIC / LIT split, like SegField: the ghost is a path built once per layout; a new reading only blits glyphs
 *   from ONE atlas image (every glyph, glow included, rasterised once per pitch × colour) — a reading changes several
 *   times a second and must never re-rasterise a blur.
 * ★ No wire mesh (SegField's 60° grid): on a dot matrix the DOTS are the structure, and the Doto text beside these
 *   fields (the frequency, the station strip) carries none.
 */

import React, { useMemo } from 'react';
import { View } from 'react-native';
import { Canvas, Group, Image as SkImageNode, Path, Skia, type SkPath } from '@shopify/react-native-skia';
import { glowPaint, imageBuild, makeSprite, spriteBuild, useSharedSprite } from './glowSprite';
import { bitmapDots, dotGlyph, DOT_ALPHABET, DOT_COLS, DOT_ROWS, DOT_SIZE } from '../constants/dotField';

/** The atlas is a grid this many glyphs wide (SegField's rule: one long row can overrun SPRITE_MAX_PX at 3×). */
const ATLAS_COLS = 16;
const ALPHA = [...DOT_ALPHABET];
const ATLAS_ROWS = Math.ceil(ALPHA.length / ATLAS_COLS);
/** Room round each glyph for its glow. */
const M = 4;
/** Every dot of a cell — the ghost. */
const ALL = Array.from({ length: DOT_ROWS }, () => '#'.repeat(DOT_COLS)).join(' ');

/** Adds `rows`' lit dots to `p`, the bitmap's top-left dot at dot (col, row), at `pitch`, offset (ox, oy) pt. */
function addDots(p: SkPath, rows: string, col: number, pitch: number, ox = 0, oy = 0, row = 0) {
  const d = DOT_SIZE * pitch, inset = ((1 - DOT_SIZE) / 2) * pitch;
  for (const [x, y] of bitmapDots(rows)) {
    p.addRect(Skia.XYWHRect(ox + (col + x) * pitch + inset, oy + (row + y) * pitch + inset, d, d));
  }
}

/** A shape on the field's dot grid beside the cells — the stereo rings, the colon: always ghosted, lit or not. */
export interface DotMark { col: number; rows: string; lit: boolean }

export interface DotFieldProps {
  /** The dot pitch, pt (a Doto font size × 0.1 — constants/dotField dotPitch). */
  pitch: number;
  /** The field's width in dots. */
  cols: number;
  /** Each cell's first dot column (its ghost is all 35 dots). */
  cellCols: readonly number[];
  /** Each cell's lit glyph ('' = dark), index-matched to `cellCols`. */
  lit: readonly string[];
  marks?: readonly DotMark[];
  color: string;
  glow: string | null;
  ghostColor: string;
  accessibilityLabel?: string;
}

export const DotField = React.memo(function DotField({ pitch, cols, cellCols, lit, marks = [], color, glow, ghostColor,
  accessibilityLabel }: DotFieldProps) {
  const width = cols * pitch, H = DOT_ROWS * pitch;
  const W = width + 2 * M, HH = H + 2 * M;

  // ── The atlas: every glyph, lit with its glow, once per pitch × colour. NaN-proof key (`pitch > 0`). ──
  const atlas = useSharedSprite(pitch > 0 ? `dotfield|${pitch}|${color}|${glow}` : null, () => {
    const sw = DOT_COLS * pitch + 2 * M, sh = H + 2 * M;
    const img = makeSprite(sw * ATLAS_COLS, sh * ATLAS_ROWS, (c) => {
      ALPHA.forEach((ch, i) => {
        const rows = dotGlyph(ch);
        if (!rows) return;
        const p = Skia.Path.Make();
        addDots(p, rows, 0, pitch, (i % ATLAS_COLS) * sw + M, Math.floor(i / ATLAS_COLS) * sh + M);
        if (glow) c.drawPath(p, glowPaint(glow, 2));
        c.drawPath(p, glowPaint(color));
      });
    });
    return spriteBuild(img ? { img, sw, sh } : null, [img]);
  });

  // ── The lit marks: one image per lit set — they change with the mode / the pilot, not the reading. ──
  const marksKey = marks.map(m => `${m.col}:${m.rows}`).join('|');
  const litMask = marks.map(m => (m.lit ? 1 : 0)).join('');
  const marksLit = useSharedSprite(pitch > 0 && marks.some(m => m.lit) ? `dotx|${marksKey}|${litMask}|${pitch}|${W}|${color}|${glow}` : null,
    () => imageBuild(makeSprite(W, HH, (c) => {
      const p = Skia.Path.Make();
      for (const m of marks) if (m.lit) addDots(p, m.rows, m.col, pitch, M, M);
      if (glow) c.drawPath(p, glowPaint(glow, 2));
      c.drawPath(p, glowPaint(color));
    })));

  // ── The static layer: every dot of every cell and mark, unlit. ──
  const ghostKey = `${cellCols.join(',')}|${marksKey}`;
  const ghost = useMemo(() => {
    if (!(pitch > 0)) return null;
    const p = Skia.Path.Make();
    for (const c of cellCols) addDots(p, ALL, c, pitch, M, M);
    for (const m of marks) addDots(p, m.rows, m.col, pitch, M, M);
    return p;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [pitch, ghostKey]);

  return (
    <View style={{ width, height: H }} pointerEvents="none" accessibilityRole="text" accessibilityLabel={accessibilityLabel}>
      {ghost && (
        <Canvas style={{ position: 'absolute', left: -M, top: -M, width: W, height: HH }}>
          <Path path={ghost} color={ghostColor} />
          {marksLit && <SkImageNode image={marksLit} x={0} y={0} width={W} height={HH} />}
          {atlas && lit.map((ch, k) => {
            const i = ch ? ALPHA.indexOf(ch) : -1;
            if (i < 0 || cellCols[k] == null) return null;
            const x = cellCols[k] * pitch;   // the slot's own margin M lands the glyph at M + x
            const col = i % ATLAS_COLS, row = Math.floor(i / ATLAS_COLS);
            return (
              <Group key={k} clip={Skia.XYWHRect(x, 0, atlas.sw, atlas.sh)}>
                <SkImageNode image={atlas.img} x={x - col * atlas.sw} y={-row * atlas.sh}
                  width={atlas.sw * ATLAS_COLS} height={atlas.sh * ATLAS_ROWS} />
              </Group>
            );
          })}
        </Canvas>
      )}
    </View>
  );
});
