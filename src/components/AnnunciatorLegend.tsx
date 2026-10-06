/**
 * AnnunciatorLegend — a car stereo's fixed TP / TA / AF legend, drawn the way RdsMark draws the RDS mark
 * (Stuart, 2026-10-02: "the SAME WAY as the fixed RDS icon … fixed-shape phosphor legends baked into the
 * glass, with the wire grid over them"). Not text: these never change, so they are fixed shapes — never
 * a 7-segment, 14-segment or Doto rendering.
 *
 *   plain  (hyper, nixie)  lit in the resolved text colour with its glow; unlit = the same legend at the
 *                          ghost colour, so the cluster keeps its place and reads as a set of lamps.
 *   picto  (dot, seg)      a VFD annunciator: always there as a ghost electrode, lit (glow 2) when true,
 *                          seen through the same 60° mesh as RdsMark's picto mark.
 *
 * ★ The letterforms are STROKES ON A 10 × 14 CELL turned into filled paths here, not a typeface: a system
 *   font differs between iOS and Android and between weights, and a legend printed in the glass must be
 *   the same shape on every device. Bold, square-ended, like a front-panel legend.
 */
import React, { useMemo } from 'react';
import { View } from 'react-native';
import { Canvas, Image as SkImageNode, Path, Skia, StrokeCap, StrokeJoin, type SkPath } from '@shopify/react-native-skia';
import { glowPaint, imageBuild, makeSprite, useSharedSprite } from './glowSprite';
import { MESH_SHADES, vfdMesh } from './vfdMesh';

export type AnnunciatorName = 'TP' | 'TA' | 'AF';

const CELL_W = 10, CELL_H = 14, GAP = 2.2, STROKE = 2.5;

/** One letter's centre-lines in cell units, as [x1,y1,x2,y2] segments, plus P's bowl. */
function letter(ch: string, ox: number): SkPath {
  const p = Skia.Path.Make();
  const seg = (x1: number, y1: number, x2: number, y2: number) => { p.moveTo(ox + x1, y1); p.lineTo(ox + x2, y2); };
  const h = STROKE / 2;
  switch (ch) {
    case 'T': seg(0, h, CELL_W, h); seg(CELL_W / 2, h, CELL_W / 2, CELL_H); break;
    case 'P':
      seg(1 + h, 0, 1 + h, CELL_H);
      p.moveTo(ox + 1 + h, h); p.lineTo(ox + 6, h);
      p.quadTo(ox + CELL_W - h, h, ox + CELL_W - h, 4.4);
      p.quadTo(ox + CELL_W - h, 8.2 - h, ox + 6, 8.2 - h);
      p.lineTo(ox + 1 + h, 8.2 - h);
      break;
    case 'A': seg(h, CELL_H, CELL_W / 2, h); seg(CELL_W / 2, h, CELL_W - h, CELL_H); seg(2.6, 9.6, CELL_W - 2.6, 9.6); break;
    case 'F': seg(1 + h, 0, 1 + h, CELL_H); seg(1 + h, h, CELL_W, h); seg(1 + h, 7, CELL_W - 2, 7); break;
    // ★ 2026-10-06: the VCR mode box's dB / F / S legends (ControlsBar SegReading) — the readout's units are
    //   fixed legends lit per meter mode, like TP / TA / AF, so they come from the same strokes.
    case 'd': {
      // A real lower-case d (the panel legend reads "dB"): the stem on the RIGHT, full height; a bowl from x-height.
      const st = CELL_W - 1 - h;
      seg(st, 0, st, CELL_H);
      p.moveTo(ox + st, 5 + h); p.lineTo(ox + 4.6, 5 + h);
      p.quadTo(ox + h, 5 + h, ox + h, 9.5);
      p.quadTo(ox + h, CELL_H - h, ox + 4.6, CELL_H - h);
      p.lineTo(ox + st, CELL_H - h);
      break;
    }
    case 'B':
      seg(1 + h, 0, 1 + h, CELL_H);
      p.moveTo(ox + 1 + h, h); p.lineTo(ox + 5.4, h);
      p.quadTo(ox + CELL_W - 1 - h, h, ox + CELL_W - 1 - h, 3.8);
      p.quadTo(ox + CELL_W - 1 - h, 6.8, ox + 5.4, 6.8);
      p.lineTo(ox + 1 + h, 6.8);
      p.moveTo(ox + 5.4, 6.8); p.lineTo(ox + 5.8, 6.8);
      p.quadTo(ox + CELL_W - h, 6.8, ox + CELL_W - h, 10.3);
      p.quadTo(ox + CELL_W - h, CELL_H - h, ox + 5.8, CELL_H - h);
      p.lineTo(ox + 1 + h, CELL_H - h);
      break;
    case 'S':
      p.moveTo(ox + CELL_W - h, 3);
      p.quadTo(ox + CELL_W - h, h, ox + 5, h);
      p.quadTo(ox + h, h, ox + h, 3.6);
      p.quadTo(ox + h, 6.6, ox + 5, 6.9);
      p.quadTo(ox + CELL_W - h, 7.2, ox + CELL_W - h, 10.3);
      p.quadTo(ox + CELL_W - h, CELL_H - h, ox + 5, CELL_H - h);
      p.quadTo(ox + h, CELL_H - h, ox + h, 11);
      break;
  }
  return p.stroke({ width: STROKE, cap: StrokeCap.Butt, join: StrokeJoin.Miter }) ?? Skia.Path.Make();
}

const LEGEND_W = 2 * CELL_W + GAP;

/** Any run of the letters above as one filled path, in cell units (0..legendUnits(n) × 0..CELL_H) — for a
 *  caller that draws the legend into a canvas of its own (SegField: one canvas per readout, not one per legend). */
export function legendPath(text: string): SkPath {
  const p = Skia.Path.Make();
  [...text].forEach((ch, i) => p.addPath(letter(ch, i * (CELL_W + GAP))));
  return p;
}
/** A legend's width in cell units, and the cell's height in the same units. */
export const legendUnits = (n: number) => n * CELL_W + Math.max(0, n - 1) * GAP;
export const LEGEND_CELL_H = CELL_H;

/** The two letters as one filled path, in cell units (0..LEGEND_W × 0..CELL_H). */
const LEGENDS: Record<AnnunciatorName, SkPath> = (() => {
  const out = {} as Record<AnnunciatorName, SkPath>;
  for (const name of ['TP', 'TA', 'AF'] as AnnunciatorName[]) {
    const p = Skia.Path.Make();
    p.addPath(letter(name[0], 0));
    p.addPath(letter(name[1], CELL_W + GAP));
    out[name] = p;
  }
  return out;
})();

/** The VFD mesh at RdsMark's proportions (a 42-unit pitch on a 260-unit mark = 16 % of the height). */
const meshFor = (legend: SkPath): [SkPath, SkPath] =>
  vfdMesh(legend, (42 / 260) * CELL_H, (8 / 260) * CELL_H, { count: 60, extent: 200 });
const MESHES: Record<AnnunciatorName, [SkPath, SkPath]> = {
  TP: meshFor(LEGENDS.TP), TA: meshFor(LEGENDS.TA), AF: meshFor(LEGENDS.AF),
};

/** Legend width for a height. */
export const annunciatorWidth = (h: number) => (h * LEGEND_W) / CELL_H;

const MARGIN = 6;

function scaled(p: SkPath, k: number): SkPath {
  const c = p.copy();
  const m = Skia.Matrix();
  m.translate(MARGIN, MARGIN);
  m.scale(k, k);
  c.transform(m);
  return c;
}

export default function AnnunciatorLegend({ name, height = 10, kind, color, glow, ghost, lit }: {
  name: AnnunciatorName;
  height?: number;
  kind: 'plain' | 'picto';
  /** The lit colour (the resolved text colour; neon under Nixie). */
  color: string;
  glow: string | null;
  /** The unlit colour — a ghost of the lit one. */
  ghost: string;
  lit: boolean;
}) {
  const k = height / CELL_H;
  const w = annunciatorWidth(height);
  const W = w + 2 * MARGIN, H = height + 2 * MARGIN;
  // ★ Shared across instances and freed when unused — see useSharedSprite.
  const sprite = useSharedSprite(`ann|${name}|${k}|${W}|${H}|${color}|${glow}|${kind}`, () => imageBuild(makeSprite(W, H, (c) => {
    const p = scaled(LEGENDS[name], k);
    if (glow) c.drawPath(p, glowPaint(glow, kind === 'picto' ? 2 : 4));
    c.drawPath(p, glowPaint(color));
  })));
  const statics = useMemo(() => ({
    ghost: scaled(LEGENDS[name], k),
    meshA: kind === 'picto' ? scaled(MESHES[name][0], k) : null,
    meshB: kind === 'picto' ? scaled(MESHES[name][1], k) : null,
  }), [name, k, kind]);
  return (
    <View style={{ width: w, height }} pointerEvents="none" accessibilityRole="image"
          accessibilityLabel={`${name} ${lit ? 'on' : 'off'}`}>
      <Canvas style={{ position: 'absolute', left: -MARGIN, top: -MARGIN, width: W, height: H }}>
        {!lit && <Path path={statics.ghost} color={ghost} />}
        {lit && sprite && <SkImageNode image={sprite} x={0} y={0} width={W} height={H} />}
        {statics.meshA && <Path path={statics.meshA} color={MESH_SHADES[0]} />}
        {statics.meshB && <Path path={statics.meshB} color={MESH_SHADES[1]} />}
      </Canvas>
    </View>
  );
}
