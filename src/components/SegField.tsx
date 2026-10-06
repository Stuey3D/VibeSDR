/**
 * SegField — a FIXED run of 14-segment cells, drawn the VCR Display's way (2026-10-06): DSEG14, the VTS
 * strip's own face, every electrode always there as a ghost (text colour α .10), the lit glyphs glowing on
 * top, and the 60° grid mesh over the lot — the RDS mark's treatment (vfdMesh.ts), so the mode box reads
 * as part of the same glass. Used by ControlsBar's VCR mode box: the mode field and the S-readout.
 *
 * ★ The cells are DRAWN, not React Native Text: the mesh is a path intersected with the electrodes, and a
 *   Text has no path. Skia's own DSEG14 outlines (Path.MakeFromText) give the electrodes; the TTF is loaded
 *   the way NixieTubes loads Nixie One.
 * ★★ ONE CANVAS PER FIELD (the Mac GPU audit: every <Canvas> is a Metal layer). The extras — the stereo
 *   rings, the dB / F / S legends — are paths drawn into this canvas, not components with canvases of their own.
 * ★★ STATIC / LIT split, like SegDigits: the ghost and the mesh are paths built once per layout; a new
 *   reading only blits glyphs from ONE atlas image (every glyph, glow included, rasterised once per size ×
 *   colour) — a reading changes several times a second and must never re-rasterise a blur.
 */

import React, { useMemo } from 'react';
import { View } from 'react-native';
import { Canvas, Group, Image as SkImageNode, Path, Skia, useTypeface, type SkFont, type SkPath } from '@shopify/react-native-skia';
import { glowPaint, imageBuild, makeSprite, spriteBuild, useSharedSprite } from './glowSprite';
import { MESH_FINE_BAR, MESH_FINE_PITCH, MESH_SHADES_FINE, vfdMesh } from './vfdMesh';
import { SEG14_ADV, SEG14_PITCH, segCellsWidth } from '../constants/modeBox';

const DSEG14_TTF = require('../../assets/fonts/DSEG14Classic-BoldItalic.ttf');

/** Every glyph a field can light — DSEG14's full cells (displayText SEG_OK), its point and its colon. */
const ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-+?@&$%'\"()*,/<=>^_\\|`.:~";
/** The atlas is a grid this many glyphs wide (one long row overran SPRITE_MAX_PX on an iPad at 3×). */
const ATLAS_COLS = 16;
const ATLAS_ROWS = Math.ceil(ALPHABET.length / ATLAS_COLS);
/** Room round each atlas glyph for its glow. */
const GLOW_M = 4;
/** DSEG14's ':' is 0.2 em wide with its dots centred in it; '.' is zero-width, drawn just LEFT of the pen. */
const COLON_ADV = 0.2;
const DP_LEFT = 0.12;

/** Where a glyph's PEN goes within its atlas slot (the point hangs left of its pen). */
const penIn = (ch: string, fs: number) => GLOW_M + (ch === '.' ? DP_LEFT * fs : 0);
const slotW = (fs: number) => SEG14_ADV * fs + 2 * GLOW_M + DP_LEFT * fs;

/** Cell i's pen x. */
export const segCellX = (i: number, fs: number) => i * SEG14_PITCH * fs;
/** The pen x for the colon electrode in the GAP after cell i — its dots centred between the two glyphs
 *  (a glyph's ink runs 0.062 … 0.754 em of its cell). */
export const segColonX = (i: number, fs: number) => segCellX(i, fs) + ((0.754 + SEG14_PITCH + 0.062) / 2 - COLON_ADV / 2) * fs;

/** A lit or ghost glyph at a place: the char and its pen. */
type Placed = { ch: string; x: number };

/** A cell's glyphs as pens: each overlay char at the cell's pen, a '.' after the glyph, a ':' centred. */
function placeCell(cell: string, i: number, fs: number): Placed[] {
  const x0 = segCellX(i, fs);
  const out: Placed[] = [];
  for (const ch of cell) {
    if (ch === '.') out.push({ ch, x: x0 + SEG14_ADV * fs });
    else if (ch === ':') out.push({ ch, x: x0 + ((SEG14_ADV - COLON_ADV) / 2) * fs });
    else out.push({ ch, x: x0 });
  }
  return out;
}

function glyphsPath(font: SkFont, placed: Placed[], baseline: number): SkPath {
  const p = Skia.Path.Make();
  for (const g of placed) {
    const gp = Skia.Path.MakeFromText(g.ch, g.x, baseline, font);
    if (gp) p.addPath(gp);
  }
  return p;
}

/** A shape drawn in the field's canvas beside the cells: the stereo rings, a unit legend. */
export interface SegExtra { path: SkPath; lit: boolean }

export interface SegFieldProps {
  /** Cell height (= DSEG14's font size). */
  fs: number;
  /** The field's box (the extras may sit beyond the cells). */
  width: number;
  /** Each cell's ghost electrodes ('' = no cell there, e.g. the rings' slot). */
  ghost: readonly string[];
  /** Each cell's lit glyphs ('' = dark). May be longer or shorter than `ghost`. */
  lit: readonly string[];
  /** The colon electrode in the gap after this cell (always ghosted), or null for none. */
  colonAfter?: number | null;
  colonLit?: boolean;
  extras?: readonly SegExtra[];
  /** Names everything `extras` draws (their shapes and places) — the static layer's memo key. */
  extrasKey?: string;
  color: string;
  glow: string | null;
  ghostColor: string;
  accessibilityLabel?: string;
}

const M = GLOW_M;

export const SegField = React.memo(function SegField({ fs, width, ghost, lit, colonAfter = null, colonLit = false,
  extras = [], extrasKey = '', color, glow, ghostColor, accessibilityLabel }: SegFieldProps) {
  const typeface = useTypeface(DSEG14_TTF);
  const font = useMemo(() => (typeface ? Skia.Font(typeface, fs) : null), [typeface, fs]);
  const H = fs;
  const baseline = fs;   // DSEG14's glyphs fill 0 … 1 em above the baseline

  // ── The atlas: every glyph, lit with its glow, once per size × colour. ──
  // ★ NaN-proof key (SegDigits' trap: `fs > 0` is false for NaN, so no sprite is built for an unmeasured size).
  const atlas = useSharedSprite(font && fs > 0 ? `segfield|${fs}|${color}|${glow}` : null, () => {
    if (!font) return null;
    const sw = slotW(fs), sh = fs + 2 * M;
    const img = makeSprite(sw * ATLAS_COLS, sh * ATLAS_ROWS, (c) => {
      [...ALPHABET].forEach((ch, i) => {
        const gp = Skia.Path.MakeFromText(ch, (i % ATLAS_COLS) * sw + penIn(ch, fs),
                                          Math.floor(i / ATLAS_COLS) * sh + M + baseline, font);
        if (!gp) return;
        if (glow) c.drawPath(gp, glowPaint(glow, 3));
        c.drawPath(gp, glowPaint(color));
      });
    });
    return spriteBuild(img ? { img, sw, sh } : null, [img]);
  });

  // ── The lit extras (rings, legends): one image per lit set — they change with the mode, not the reading. ──
  const litMask = extras.map(e => (e.lit ? 1 : 0)).join('');
  const W = width + 2 * M, HH = H + 2 * M;
  const extrasLit = useSharedSprite(font && extras.some(e => e.lit) ? `segx|${extrasKey}|${litMask}|${fs}|${W}|${color}|${glow}` : null,
    () => imageBuild(makeSprite(W, HH, (c) => {
      c.translate(M, M);
      for (const e of extras) {
        if (!e.lit) continue;
        if (glow) c.drawPath(e.path, glowPaint(glow, 2));
        c.drawPath(e.path, glowPaint(color));
      }
    })));

  // ── The static layer: every electrode as a ghost, and the mesh over all of them. ──
  const ghostKey = `${ghost.join('|')}|${colonAfter}|${extrasKey}`;
  const statics = useMemo(() => {
    if (!font) return null;
    const placed: Placed[] = [];
    ghost.forEach((g, i) => placed.push(...placeCell(g, i, fs)));
    if (colonAfter != null) placed.push({ ch: ':', x: segColonX(colonAfter, fs) });
    const g = glyphsPath(font, placed, baseline);
    for (const e of extras) g.addPath(e.path);
    // The lit glyphs are inside their electrodes, so the mesh over the GHOST covers them too.
    const mesh = vfdMesh(g, MESH_FINE_PITCH, MESH_FINE_BAR, { count: 200, extent: 320 });
    const t = Skia.Matrix(); t.translate(M, M);
    g.transform(t); mesh[0].transform(t); mesh[1].transform(t);
    return { ghost: g, mesh };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [font, fs, ghostKey]);

  const litPlaced = useMemo(() => {
    const out: Placed[] = [];
    lit.forEach((l, i) => out.push(...placeCell(l, i, fs)));
    if (colonLit && colonAfter != null) out.push({ ch: ':', x: segColonX(colonAfter, fs) });
    return out;
  }, [lit, fs, colonLit, colonAfter]);

  return (
    <View style={{ width, height: H }} pointerEvents="none" accessibilityRole="text" accessibilityLabel={accessibilityLabel}>
      {statics && (
        <Canvas style={{ position: 'absolute', left: -M, top: -M, width: W, height: HH }}>
          <Path path={statics.ghost} color={ghostColor} />
          {extrasLit && <SkImageNode image={extrasLit} x={0} y={0} width={W} height={HH} />}
          {atlas && litPlaced.map((g, k) => {
            const i = ALPHABET.indexOf(g.ch);
            if (i < 0) return null;
            const x = M + g.x - penIn(g.ch, fs);
            const col = i % ATLAS_COLS, row = Math.floor(i / ATLAS_COLS);
            return (
              <Group key={k} clip={Skia.XYWHRect(x, 0, atlas.sw, atlas.sh)}>
                <SkImageNode image={atlas.img} x={x - col * atlas.sw} y={-row * atlas.sh}
                  width={atlas.sw * ATLAS_COLS} height={atlas.sh * ATLAS_ROWS} />
              </Group>
            );
          })}
          <Path path={statics.mesh[0]} color={MESH_SHADES_FINE[0]} />
          <Path path={statics.mesh[1]} color={MESH_SHADES_FINE[1]} />
        </Canvas>
      )}
    </View>
  );
});

/** The width of a run of `n` cells (re-exported for the callers' layout). */
export const segFieldCellsWidth = segCellsWidth;
