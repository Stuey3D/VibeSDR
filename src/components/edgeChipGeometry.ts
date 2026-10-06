/**
 * ★★★ WHERE AN EDGE CHIP SITS — ONE GEOMETRY FOR THE CARD AND ITS TAB (Stuart, 2026-10-06, iPhone landscape, RC15:
 *  "still got issues with the server chips not anchoring to the edge of the screen when it is rotated").
 *
 *  The cards and tabs were placed at `right: insets.right` — the SAFE AREA's edge, which in landscape is ~47-62 pt
 *  inside the glass (iOS reports the inset on BOTH sides, whichever way round the island is). So they floated ~43 pt
 *  in from an edge they claim to be joined to, and the card and its tab each worked their slide distance out from
 *  that inset separately, so a rotation could leave one placed for the old inset and the other for the new.
 *
 *  ★ Now: BOTH are anchored at the PHYSICAL edge (right 0) — the frame runs flush to the glass — and the
 *    CONTENT is padded in by the inset, so nothing sits under the island / notch or a rounded corner.
 *  ★ Everything is derived from the WINDOW and the insets in this one pure function (layout follows the window,
 *    never the device — an iPad split or a resized Mac window takes the same path as a rotation).
 *  ★ No React Native import: scripts/test_edge_chip.ts runs it under plain node.
 */

/** The collapsed tab's own width (the arrow and the icon), before the inset is added. */
export const EDGE_TAB_W = 26;
export const EDGE_TAB_H = 52;

export interface EdgeInsets { top: number; right: number; bottom: number; left: number }

export interface EdgeChipGeometry {
  /** The card and the tab's `right` — always the physical edge. */
  right: 0;
  /** Padding inside the frame on the edge side, so the content clears the island / notch / corner. */
  padRight: number;
  /** The tab's full width: its glyphs plus the inset it reaches across to the glass. */
  tabW: number;
  /** translateX that puts the tucked card wholly past the edge (its measured width + a margin). */
  cardOff: number;
  /** translateX that puts the tab wholly past the edge while the card is out. */
  tabOff: number;
  /** The card may never be wider than this, so an expanded card cannot run off the far side either. */
  maxCardW: number;
}

export function edgeChipGeometry({ windowW, insets, cardW }: {
  windowW: number; insets: EdgeInsets;
  /** The card's measured width (it includes padRight, so it changes with a rotation). */
  cardW: number;
}): EdgeChipGeometry {
  const padRight = Math.max(0, Math.round(insets.right));
  const tabW = EDGE_TAB_W + padRight;
  // ★ Width-safe: a card can be no wider than the window less the far-side inset and a small gutter.
  const maxCardW = Math.max(EDGE_TAB_W * 2, windowW - Math.max(0, insets.left) - 8);
  const w = Math.min(Math.max(0, cardW), maxCardW);
  return { right: 0, padRight, tabW, cardOff: w + 12, tabOff: tabW + 4, maxCardW };
}
