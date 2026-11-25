/**
 * ★★ WHERE A DECODER BOX GOES — DecoderShell's placement decision, pure (no React Native) so
 * scripts/test_decoder_shell_layout.ts can prove that every window WITHOUT a hinge places the box exactly as before the
 * iPhone Duo half-open work, and that a half-open BIG box is two panels clear of the fold.
 */
import type { HalfOpenSplit } from './duoGeometry';

/** What SDRScreen provides through DecoderAvoidContext (see DecoderShell). */
export interface ShellAvoid {
  rects: { x: number; y: number; w: number; h: number }[];
  windowW: number;
  frame?: { left: number; right: number; bottom: number; maxHeight: number; split?: HalfOpenSplit & { windowH: number } };
}

export interface WrapPos { bottom: number; left?: number; right?: number }
export type ShellPlacement =
  | { kind: 'single'; wrap: WrapPos; maxWidth?: number; maxHeight?: number }
  | { kind: 'split'; upper: { wrap: WrapPos; maxHeight: number }; lower: { wrap: WrapPos; maxHeight: number } };

/**
 * `clearRight` is the right edge DecoderShell measured clear of the system's corner rects (8 = nothing in the way).
 * Single box: `bottom`, the box's own caps, nudged left of a corner rect — or, half-open, the frame beside the deck.
 * Split: only in BIG and only where SDRScreen offers it (frame.split).
 */
export function shellPlacement(avoid: ShellAvoid, p: { tall: boolean; bottom: number; maxWidth: number; maxHeight?: number;
                                                       clearRight: number }): ShellPlacement {
  const sp = p.tall ? avoid.frame?.split : undefined;
  if (sp) {
    const at = (span: { top: number; bottom: number }) => ({
      wrap: { left: sp.x0, right: avoid.windowW - sp.x1, bottom: sp.windowH - span.bottom },
      maxHeight: span.bottom - span.top });
    return { kind: 'split', upper: at(sp.upper), lower: at(sp.lower) };
  }
  if (avoid.frame) {
    return { kind: 'single', wrap: { left: avoid.frame.left, right: avoid.frame.right, bottom: avoid.frame.bottom },
             maxHeight: Math.min(p.maxHeight ?? Infinity, avoid.frame.maxHeight) };
  }
  const wrap: WrapPos = { bottom: p.bottom };
  if (p.clearRight > 8) wrap.right = p.clearRight;
  return { kind: 'single', wrap, maxWidth: p.maxWidth, maxHeight: p.maxHeight };
}

/**
 * ★★ HOW FAR A BOX'S RIGHT EDGE KEEPS IN FROM THE WINDOW'S RIGHT EDGE to clear a system corner rect (the camera, the
 * status column / pill) — 8 = nothing in the way. The box is normally CENTRED in a wrap 8 pt in from each side, at its
 * `maxWidth`. It is moved only when that centred box, in the rows it occupies (`rows`, window y), would actually come
 * within 8 pt of a rect on the right half — then its right edge ends 8 pt left of the rect, off-centre by design
 * (Stuart, folded front screen: "it means it will be off centre but should work").
 * ★ Not merely because a rect exists in those rows (Stuart, open inner screen in landscape, DAB BIG: "inner screen
 *   doesn't need the shift to the left in big mode" — the pill sat at ~96 % of the width, nowhere near the box, and BIG
 *   was pulled left anyway). Clear of the rect, the box stays centred exactly as on an iPad.
 */
export function cornerClearRight(rects: { x: number; y: number; w: number; h: number }[], windowW: number,
                                 rows: { y: number; h: number }, maxWidth: number): number {
  let clear = 8;
  if (!rects.length || windowW <= 0) return clear;
  const boxW = Math.min(maxWidth, windowW - 16);
  const boxL = (windowW - boxW) / 2, boxR = boxL + boxW;
  for (const r of rects) {
    if (r.y >= rows.y + rows.h || r.y + r.h <= rows.y) continue;
    if (r.x + r.w / 2 < windowW / 2) continue;
    if (r.x - 8 >= boxR || r.x + r.w + 8 <= boxL) continue;   // the centred box clears it: no move
    clear = Math.max(clear, Math.ceil(windowW - r.x) + 8);
  }
  return clear;
}
