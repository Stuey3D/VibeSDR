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

/** The collapsed tab's own width (the arrow and the icon), before any padding is added. */
export const EDGE_TAB_W = 26;
export const EDGE_TAB_H = 52;

/* ★★★ ONLY THE ISLAND'S SIDE NEEDS THE INSET (Stuart, 2026-10-06, RC16 landscape: "now the health and timer pills
 *  are huge when collapsed, bigger than when they are open"). iOS reports the landscape inset on BOTH sides, so
 *  padding by `insets.right` gave the tab ~59 pt of empty frame — icon jammed against its left side — whenever the
 *  island was on the LEFT. The side comes from the interface orientation (src/hooks/useIslandSide.ts):
 *    'right' — the island / notch is on this edge: pad by the full inset, as before.
 *    'left' / 'none' — it is not: pad only by what the glass's ROUNDED CORNER takes out of the chip's rows.
 *    null — not known (Android, whose cutout insets are already one-sided; an older native build; the first value
 *           not in yet): the full inset, today's safe behaviour.
 *  ★ The corner is MEASURED from the chip's own rows, not guessed: a circle of the display's corner radius takes
 *    R − √(R² − (R − y)²) off a row y points from the top (or bottom) and nothing once y ≥ R. The chips sit ~70 pt
 *    down in landscape, past even the largest radius, so the side without the island gets 0.
 *  ✗ Not done: on the island side the tab still takes the whole inset even where its rows miss the island (it is
 *    centred on that edge, ~126 pt long on a 402 pt-tall Pro). The island's and notches' lengths differ by model and
 *    no API gives them, so a guess there would put the chevron under the notch on some phone. */
export type IslandSide = 'left' | 'right' | 'none';

/** The interface orientation's name (VibeInterfaceOrientation.mm) → the side the island / notch is on.
 *  Apple: landscapeRight = "the Home button on the right" ⇒ the top of the phone (the island) on the LEFT. */
export function islandSideFor(orientation: string | null | undefined): IslandSide | null {
  switch (orientation) {
    case 'landscapeRight': return 'left';
    case 'landscapeLeft': return 'right';
    case 'portrait': case 'portraitUpsideDown': return 'none';
    default: return null;
  }
}

/** The largest iPhone display corner radius (16 / 17 Pro and Pro Max ≈ 62 pt; 14 / 15 Pro 55; earlier ≤ 47).
 *  Taking the largest over-pads an older phone by a point or two, never under-pads a newer one. */
export const DISPLAY_CORNER_R = 62;

/** How far the glass's rounded corners cut into the chip's edge anywhere over its rows [top, top + h]. */
export function cornerIntrusion(top: number, h: number, windowH: number, r = DISPLAY_CORNER_R): number {
  const cut = (y: number) => (y >= r ? 0 : r - Math.sqrt(r * r - (r - Math.max(0, y)) ** 2));
  return Math.ceil(Math.max(cut(top), cut(windowH - (top + h))));
}

export interface EdgeInsets { top: number; right: number; bottom: number; left: number }

export interface EdgeChipGeometry {
  /** The card and the tab's `right` — always the physical edge. */
  right: 0;
  /** Padding inside the frame on the edge side, so the content clears the island / notch / corner. */
  padRight: number;
  /** The tab's full width: its glyphs plus the padding it actually needs — never wider than the open card. */
  tabW: number;
  /** translateX that puts the tucked card wholly past the edge (its measured width + a margin). */
  cardOff: number;
  /** translateX that puts the tab wholly past the edge while the card is out. */
  tabOff: number;
  /** The card may never be wider than this, so an expanded card cannot run off the far side either. */
  maxCardW: number;
}

export function edgeChipGeometry({ windowW, windowH, insets, cardW, cardH = 0, top = 0, islandSide = null }: {
  windowW: number; insets: EdgeInsets;
  /** The card's measured width (it includes padRight, so it changes with a rotation). */
  cardW: number;
  /** The window's height, the chip's top and the card's measured height: which rows the corners can reach. */
  windowH?: number; top?: number; cardH?: number;
  /** Where the island / notch is (useIslandSide); null = not known ⇒ the full inset. */
  islandSide?: IslandSide | null;
}): EdgeChipGeometry {
  const inset = Math.max(0, Math.round(insets.right));
  const span = Math.max(EDGE_TAB_H, cardH);
  const padRight = islandSide === 'left' || islandSide === 'none'
    ? Math.min(inset, windowH == null ? inset : cornerIntrusion(top, span, windowH))
    : inset;
  // ★ Width-safe: a card can be no wider than the window less the far-side inset and a small gutter.
  const maxCardW = Math.max(EDGE_TAB_W * 2, windowW - Math.max(0, insets.left) - 8);
  const w = Math.min(Math.max(0, cardW), maxCardW);
  // ★ The collapsed tab is a compact pill: never wider than the open card it stands for (once that is measured).
  const tabW = Math.max(EDGE_TAB_W, w > 0 ? Math.min(EDGE_TAB_W + padRight, w) : EDGE_TAB_W + padRight);
  return { right: 0, padRight, tabW, cardOff: w + 12, tabOff: tabW + 4, maxCardW };
}
