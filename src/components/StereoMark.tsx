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
 * VCR only: the rings are drawn INSIDE the mode field's canvas (SegField extras) from stereoRingsPath. Hyper and Nixie
 * keep ControlsBar's plain StereoIcon.
 * ★ 2026-10-06: the DOT Display's rings are DOTS now (constants/dotField DOT_RINGS, on the mode field's own grid), so
 *   the standalone StereoMark component this file was named for — DOT's, beside its Doto label — is gone.
 */

import { PathOp, Skia, type SkPath } from '@shopify/react-native-skia';
import { vfdCut } from './vfdMesh';

/** StereoIcon's geometry: rings `size` across, the second 0.62 × size to the right, 1.62 × size wide in all. */
export const STEREO_W = 1.62;
export const stereoMarkWidth = (size: number) => size * STEREO_W;

/**
 * The rings at `size`, top-left at (ox, oy), in points — filled outlines, cut into their electrodes.
 * The VCR mode field draws it (SegField extras), ghost, lit and meshed.
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
