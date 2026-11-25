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
