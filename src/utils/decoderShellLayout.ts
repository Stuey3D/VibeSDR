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

/**
 * ★★★ beta96 THE LIST DECODERS IN HALF-OPEN BIG (iPhone Duo; Stuart 2026-11-26: "the duo needs the layout we just built with
 * decoder list split across the crease"). In beta95 these stayed one box below the fold with BIG not offered; now each is
 * two panels like DAB / spots / Advanced RDS. How each splits, and how it scrolls (Stuart: one list = ONE continuous list,
 * both panels scroll together — utils/linkedListScroll; separate data = each panel on its own):
 */
export type ListDecoder = 'cw' | 'psk' | 'rtty' | 'jtty' | 'dsc' | 'ncdxf' | 'balloon' | 'aprs' | 'scanner';
export interface ListDecoderSplit { scroll: 'linked' | 'independent'; upper: string; lower: string }
export const LIST_DECODER_SPLIT: Record<ListDecoder, ListDecoderSplit> = {
  // The signals being decoded now (and ONE SIGNAL's own text) are the live view; SPOTS is the log of calls heard.
  cw:      { scroll: 'independent', upper: 'the signals being decoded (ONE SIGNAL: its text first)', lower: 'SPOTS' },
  psk:     { scroll: 'independent', upper: 'the signals being decoded', lower: 'SPOTS' },
  rtty:    { scroll: 'independent', upper: 'the RTTY skimmer\'s signals (or the one signal\'s text)', lower: 'SPOTS' },
  jtty:    { scroll: 'independent', upper: 'the stations being decoded', lower: 'SPOTS' },
  // ONE list (the tune offer and channel keys at its head, then the calls, newest first): it continues across the fold.
  dsc:     { scroll: 'linked', upper: 'the keys and the calls, continuing', lower: 'the calls that follow' },
  ncdxf:   { scroll: 'independent', upper: 'the beacon grid', lower: 'the heard log' },
  balloon: { scroll: 'independent', upper: 'the payloads heard (position, height)', lower: 'the SSDV pictures and the report log' },
  aprs:    { scroll: 'independent', upper: 'the stations heard', lower: 'MESSAGES, PACKETS and FRAMES' },
  scanner: { scroll: 'independent', upper: 'status and the live keys (START · HOLD · SKIP · IGNORE · TAG)',
             lower: 'the source (band / bookmarks / manual), FOUND, settings and saved scans' },
};

/** An independent split: list-zone rows below `upperRows` are drawn in the upper panel, the rest in the lower. */
export function independentRowPanel(i: number, upperRows: number): 'upper' | 'lower' {
  return i < upperRows ? 'upper' : 'lower';
}

/**
 * Whether the BIG key is offered: wherever the half-open split is (every decoder now has a two-panel layout), else only
 * where BIG gains 40 pt over SMALL (DecoderPanel's rule since 2026-10-03 — unchanged on every window without a hinge).
 */
export function bigOffered(splitOffered: boolean, bigH: number, smallFit: number): boolean {
  return splitOffered || bigH >= smallFit + 40;
}
