/**
 * vfdMesh — the two things that make a shape read as a VFD ELECTRODE rather than a printed picture
 * (faceplates brief §7.1; Deck.mockup `rdsCut`, `vfdMesh`):
 *
 *   vfdCut    non-emissive BREAKS: lines cut out of the shape, where one electrode ends and the next begins.
 *   vfdMesh   the 60° GRID MESH the phosphor is seen through: two families of thin bars, intersected with
 *             the shape, drawn dark over it (one family darker than the other, as the mockup does).
 *
 * ★★ ONE COPY (2026-10-06). RdsMark wrote these first; AnnunciatorLegend and DabMark each grew their own copy
 *   of the mesh loop, and the VCR mode box (SegField) and the stereo rings (StereoMark) needed it as well.
 *   Every one of them now calls this. The defaults reproduce each caller's old paths exactly (same rects, same
 *   matrix order), so nothing that was already on screen moved.
 * ★ Path operations, done ONCE per shape × size by the caller (useMemo / module scope) — never per frame.
 */

import { PathOp, Skia, type SkPath } from '@shopify/react-native-skia';

/** RdsMark's / AnnunciatorLegend's mesh shades (§7.1: α .55 one way, .35 the other). */
export const MESH_SHADES: readonly [string, string] = ['rgba(0,0,0,0.55)', 'rgba(0,0,0,0.35)'];
/** ★ DabMark's lighter shades: on a small mark with ~1 pt strokes the grid should be SEEN on the phosphor,
 *  not cut the strokes. Also the 14-segment cells' (SegField) and the stereo rings' — the same size class. */
export const MESH_SHADES_FINE: readonly [string, string] = ['rgba(0,0,0,0.38)', 'rgba(0,0,0,0.22)'];
/** ★ DabMark's FIXED pitch in points (1.6 pt, 0.25 pt bars): a pitch scaled to a ~10 pt mark puts bars nearly
 *  as thick as its strokes, and the shape turns to mush (Stuart, 2026-10-02, on the DAB+ mark). */
export const MESH_FINE_PITCH = 1.6;
export const MESH_FINE_BAR = 0.25;

/**
 * The mesh over `shape`, in `shape`'s own units: bars `bar` wide every `pitch`, rotated 60°, as two paths
 * (one per bar family) already intersected with the shape.
 * @param count  bars each side of the origin; `extent` = half the bars' length. Both must cover the shape
 *               AFTER the rotation (the defaults cover ±200 units).
 * @param dx,dy  a translation applied before the rotation (RdsMark's viewBox origin) — it sets the grid's phase.
 */
export function vfdMesh(shape: SkPath, pitch: number, bar: number,
                        opts: { count?: number; extent?: number; dx?: number; dy?: number } = {}): [SkPath, SkPath] {
  const count = opts.count ?? Math.ceil(400 / pitch);
  const extent = opts.extent ?? 200;
  const a = Skia.Path.Make(), b = Skia.Path.Make();
  for (let k = -count; k <= count; k++) {
    a.addRect(Skia.XYWHRect(-extent, pitch * k, 2 * extent, bar));
    b.addRect(Skia.XYWHRect(pitch * k, -extent, bar, 2 * extent));
  }
  const m = Skia.Matrix();
  if (opts.dx || opts.dy) m.translate(opts.dx ?? 0, opts.dy ?? 0);
  m.rotate((60 * Math.PI) / 180);
  a.transform(m); b.transform(m);
  return [Skia.Path.MakeFromOp(a, shape, PathOp.Intersect) ?? a, Skia.Path.MakeFromOp(b, shape, PathOp.Intersect) ?? b];
}

/** A cut: a straight line from (x1, y1) to (x2, y2), `w` wide, in the shape's units. */
export type VfdCutLine = readonly [x1: number, y1: number, x2: number, y2: number, w: number];

/** `shape` with every cut line removed — the gaps between its electrodes. Falls back to the uncut shape. */
export function vfdCut(shape: SkPath, cuts: readonly VfdCutLine[], dx = 0, dy = 0): SkPath {
  const all = Skia.Path.Make();
  for (const [x1, y1, x2, y2, w] of cuts) {
    const l = Skia.Path.Make();
    l.moveTo(x1 + dx, y1 + dy); l.lineTo(x2 + dx, y2 + dy);
    const s = l.stroke({ width: w });
    if (s) all.addPath(s);
  }
  return Skia.Path.MakeFromOp(shape, all, PathOp.Difference) ?? shape;
}

/** A copy of `p` scaled by `k` and then moved to (ox, oy) — a mark in its own units placed in a canvas. */
export function placePath(p: SkPath, k: number, ox = 0, oy = 0): SkPath {
  const c = p.copy();
  const m = Skia.Matrix();
  m.translate(ox, oy);
  m.scale(k, k);
  c.transform(m);
  return c;
}
