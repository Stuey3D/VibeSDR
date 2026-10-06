/**
 * StereoMark — the WFM stereo rings as a VFD ANNUNCIATOR (2026-10-06, Stuart: "Can the stereo icon be given the
 * same treatment as the RDS logo with the breaks and the electrode grid"). RdsMark's picto treatment, through
 * the same code (vfdMesh.ts):
 *
 *   breaks   the rings are cut into electrodes — a horizontal cut through both rings' equator and a near-
 *            vertical one through the lens where they cross, the RDS mark's two cuts in the rings' terms;
 *   ghost    always there, at text colour α .10, so the slot never empties on a weak station whose pilot
 *            comes and goes (the box's width already never changed — ControlsBar stereoSlot);
 *   lit      the text colour with a 2 pt glow, while the pilot is locked;
 *   mesh     the 60° grid, at DabMark's fixed fine pitch: these strokes are ~1.5 pt, the DAB mark's size class.
 *
 * Only on the VFD Displays (dot / seg). Hyper and Nixie keep ControlsBar's plain StereoIcon. On VCR the rings
 * are drawn INSIDE the mode field's canvas (SegField extras) from stereoRingsPath — this component is the dot
 * Display's, beside its Doto label.
 */

import React, { useMemo } from 'react';
import { View } from 'react-native';
import { Canvas, Image as SkImageNode, Path, PathOp, Skia, type SkPath } from '@shopify/react-native-skia';
import { glowPaint, imageBuild, makeSprite, useSharedSprite } from './glowSprite';
import { MESH_FINE_BAR, MESH_FINE_PITCH, MESH_SHADES_FINE, vfdCut, vfdMesh } from './vfdMesh';

/** StereoIcon's geometry: rings `size` across, the second 0.62 × size to the right, 1.62 × size wide in all. */
export const STEREO_W = 1.62;
export const stereoMarkWidth = (size: number) => size * STEREO_W;

/**
 * The rings at `size`, top-left at (ox, oy), in points — filled outlines, cut into their electrodes.
 * ★ Shared by StereoMark and the VCR mode field (SegField extras), so the two Displays draw ONE shape.
 */
export function stereoRingsPath(size: number, ox = 0, oy = 0): SkPath {
  const bw = Math.max(1.2, size * 0.13);
  const r = (size - bw) / 2;
  const ring = (cx: number) => {
    const c = Skia.Path.Make();
    c.addCircle(ox + cx, oy + size / 2, r);
    return c.stroke({ width: bw }) ?? Skia.Path.Make();
  };
  const both = Skia.Path.MakeFromOp(ring(size / 2), ring(size * 0.62 + size / 2), PathOp.Union) ?? ring(size / 2);
  // The cuts, in ring units (size = 1): the equator through both rings, and a near-vertical through the lens
  // (the rings cross at x = 0.81), leaning like the RDS mark's.
  const w = Math.max(0.5, size * 0.07);
  const u = (x: number) => ox + x * size, v = (y: number) => oy + y * size;
  return vfdCut(both, [
    [u(-0.1), v(0.5), u(1.72), v(0.5), w],
    [u(0.78), v(-0.1), u(0.84), v(1.1), w],
  ]);
}

const MARGIN = 6;

export default function StereoMark({ size, color, glow, ghost, lit }: {
  size: number;
  color: string;
  glow: string | null;
  /** The unlit electrode colour (text colour α .10). */
  ghost: string;
  /** The pilot is locked. */
  lit: boolean;
}) {
  const w = stereoMarkWidth(size);
  const W = w + 2 * MARGIN, H = size + 2 * MARGIN;
  const statics = useMemo(() => {
    const p = stereoRingsPath(size, MARGIN, MARGIN);
    return { p, mesh: vfdMesh(p, MESH_FINE_PITCH, MESH_FINE_BAR, { count: 60, extent: 80 }) };
  }, [size]);
  // ★ Shared across instances and freed when unused — see useSharedSprite.
  const sprite = useSharedSprite(size > 0 ? `stereo|${size}|${color}|${glow}` : null, () => imageBuild(makeSprite(W, H, (c) => {
    if (glow) c.drawPath(statics.p, glowPaint(glow, 2));
    c.drawPath(statics.p, glowPaint(color));
  })));
  return (
    <View style={{ width: w, height: size, marginLeft: 5 }} pointerEvents="none" accessibilityRole="image"
          accessibilityLabel={lit ? 'Stereo' : 'Mono'}>
      <Canvas style={{ position: 'absolute', left: -MARGIN, top: -MARGIN, width: W, height: H }}>
        <Path path={statics.p} color={ghost} />
        {lit && sprite && <SkImageNode image={sprite} x={0} y={0} width={W} height={H} />}
        <Path path={statics.mesh[0]} color={MESH_SHADES_FINE[0]} />
        <Path path={statics.mesh[1]} color={MESH_SHADES_FINE[1]} />
      </Canvas>
    </View>
  );
}
