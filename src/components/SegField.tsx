/**
 * SegField — a FIXED run of 14-segment cells, drawn the VCR Display's way (2026-10-06): DSEG14, the VTS
 * strip's own face, every electrode always there as a ghost (text colour α .10), the lit glyphs glowing on
 * top, and the 60° grid mesh over the lot — the RDS mark's treatment (vfdMesh.ts), so the mode box reads
 * as part of the same glass. Used by ControlsBar's VCR mode box: the mode field and the S-readout — and, from
 * 2026-10-06, every run of the status row (StatusField: gap colons for the clocks, the raised point for `·`).
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
/** ★ The RAISED POINT (2026-10-06, the status row's `·` separator: a 14-segment cell has no middle dot). The cell's
 *  own point electrode lifted to the centre line and centred on the slanted centre bar — DSEG14's '.' ink is
 *  −0.106 … 0.018 em of its pen and 0 … 0.124 em up; the centre bar ('-') runs 0.175 … 0.640 em at 0.438 … 0.562 em.
 *  A cell token '·' places it; a '·' cell has no segments, only this. */
const MID_DOT_PEN = (0.175 + 0.640) / 2 + 0.044;
const MID_DOT_RAISE = 0.5 - 0.062;

/** Where a glyph's PEN goes within its atlas slot (the point hangs left of its pen). */
const penIn = (ch: string, fs: number) => GLOW_M + (ch === '.' ? DP_LEFT * fs : 0);
const slotW = (fs: number) => SEG14_ADV * fs + 2 * GLOW_M + DP_LEFT * fs;

/** Cell i's pen x. */
export const segCellX = (i: number, fs: number) => i * SEG14_PITCH * fs;
/** The pen x for the colon electrode in the GAP after cell i — its dots centred between the two glyphs
 *  (a glyph's ink runs 0.062 … 0.754 em of its cell). */
export const segColonX = (i: number, fs: number) => segCellX(i, fs) + ((0.754 + SEG14_PITCH + 0.062) / 2 - COLON_ADV / 2) * fs;

/** A lit or ghost glyph at a place: the char, its pen, and how far it is raised (pt; the '·' point only). */
type Placed = { ch: string; x: number; up?: number };

/** A cell's glyphs as pens: each overlay char at the cell's pen, a '.' after the glyph, a ':' centred, a '·' the
 *  point raised to the centre line. ★ Overlays are how a cell shows what DSEG14 has no glyph for: the readout's "-1"
 *  half-digit, and (2026-10-06) the unit dB's lower-case d, 'J-' (statusField SEG_LOWER_D). */
function placeCell(cell: string, i: number, fs: number): Placed[] {
  const x0 = segCellX(i, fs);
  const out: Placed[] = [];
  for (const ch of cell) {
    if (ch === '·') out.push({ ch: '.', x: x0 + MID_DOT_PEN * fs, up: MID_DOT_RAISE * fs });
    else if (ch === '.') out.push({ ch, x: x0 + SEG14_ADV * fs });
    else if (ch === ':') out.push({ ch, x: x0 + ((SEG14_ADV - COLON_ADV) / 2) * fs });
    else out.push({ ch, x: x0 });
  }
  return out;
}

function glyphsPath(font: SkFont, placed: Placed[], baseline: number): SkPath {
  const p = Skia.Path.Make();
  for (const g of placed) {
    const gp = Skia.Path.MakeFromText(g.ch, g.x, baseline - (g.up ?? 0), font);
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
  /** ★ LIT colon electrodes in the gaps after these cells (2026-10-06, the status row's clocks: `17:03`, `0:00:10`),
   *  ghosted with them — besides `colonAfter`. */
  gapColons?: readonly number[];
  extras?: readonly SegExtra[];
  /** Names everything `extras` draws (their shapes and places) — the static layer's memo key. */
  extrasKey?: string;
  color: string;
  glow: string | null;
  ghostColor: string;
  accessibilityLabel?: string;
}

const M = GLOW_M;

const NO_COLONS: readonly number[] = [];

export const SegField = React.memo(function SegField({ fs, width, ghost, lit, colonAfter = null, colonLit = false,
  gapColons = NO_COLONS, extras = [], extrasKey = '', color, glow, ghostColor, accessibilityLabel }: SegFieldProps) {
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
  const ghostKey = `${ghost.join('|')}|${colonAfter}|${gapColons.join(',')}|${extrasKey}`;
  const statics = useMemo(() => {
    if (!font) return null;
    const placed: Placed[] = [];
    ghost.forEach((g, i) => placed.push(...placeCell(g, i, fs)));
    if (colonAfter != null) placed.push({ ch: ':', x: segColonX(colonAfter, fs) });
    for (const c of gapColons) placed.push({ ch: ':', x: segColonX(c, fs) });
    const g = glyphsPath(font, placed, baseline);
    for (const e of extras) g.addPath(e.path);
    // The lit glyphs are inside their electrodes, so the mesh over the GHOST covers them too.
    // ★ The grid must reach the far end of the field: ±320 pt covered every mode-box field, but a status run on a
    //   wide Mac window is longer (2026-10-06). The same 200 bars / 320 pt for anything that fits in it.
    const reach = Math.max(320, width + fs + 2 * M);
    const mesh = vfdMesh(g, MESH_FINE_PITCH, MESH_FINE_BAR, { count: Math.ceil(reach / MESH_FINE_PITCH), extent: reach });
    const t = Skia.Matrix(); t.translate(M, M);
    g.transform(t); mesh[0].transform(t); mesh[1].transform(t);
    return { ghost: g, mesh };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [font, fs, ghostKey, width]);

  const litPlaced = useMemo(() => {
    const out: Placed[] = [];
    lit.forEach((l, i) => out.push(...placeCell(l, i, fs)));
    if (colonLit && colonAfter != null) out.push({ ch: ':', x: segColonX(colonAfter, fs) });
    for (const c of gapColons) out.push({ ch: ':', x: segColonX(c, fs) });
    return out;
  }, [lit, fs, colonLit, colonAfter, gapColons]);

  return (
    <View style={{ width, height: H }} pointerEvents="none" accessibilityRole="text" accessibilityLabel={accessibilityLabel}>
      {statics && (
        <Canvas style={{ position: 'absolute', left: -M, top: -M, width: W, height: HH }}>
          <Path path={statics.ghost} color={ghostColor} />
          {extrasLit && <SkImageNode image={extrasLit} x={0} y={0} width={W} height={HH} />}
          {atlas && litPlaced.map((g, k) => {
            const i = ALPHABET.indexOf(g.ch);
            if (i < 0) return null;
            const x = M + g.x - penIn(g.ch, fs), y = -(g.up ?? 0);
            const col = i % ATLAS_COLS, row = Math.floor(i / ATLAS_COLS);
            return (
              <Group key={k} clip={Skia.XYWHRect(x, y, atlas.sw, atlas.sh)}>
                <SkImageNode image={atlas.img} x={x - col * atlas.sw} y={y - row * atlas.sh}
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
